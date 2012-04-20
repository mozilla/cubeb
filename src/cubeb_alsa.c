/*
 * Copyright © 2011 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#undef NDEBUG
#define _BSD_SOURCE
#define _POSIX_SOURCE
#include <pthread.h>
#include <sys/time.h>
#include <assert.h>
#include <limits.h>
#include <poll.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include "cubeb/cubeb.h"

#define CUBEB_STREAM_MAX 16
#define UNUSED __attribute__ ((__unused__))

/* ALSA is not thread-safe.  snd_pcm_t instances are individually protected
   by the owning cubeb_stream's mutex.  snd_pcm_t creation and destruction
   is not thread-safe until ALSA 1.0.24 (see alsa-lib.git commit 91c9c8f1),
   so those calls must be wrapped in the following mutex. */
static pthread_mutex_t cubeb_alsa_mutex = PTHREAD_MUTEX_INITIALIZER;
static int cubeb_alsa_set_error_handler = 0;

typedef void (*poll_waitable_callback)(void * user_ptr, struct pollfd * fds, nfds_t nfds);
typedef void (*poll_timer_callback)(void * user_ptr);

struct mutex {
  pthread_mutex_t mutex;
  pthread_t owner;
  int locked;
};

struct poll_timer {
  struct poll_timer * next;
  struct poll_timer * prev;

  struct cubeb * context;

  struct timeval wakeup;

  poll_timer_callback callback;
  void * user_ptr;
};

struct poll_waitable {
  struct poll_waitable * next;
  struct poll_waitable * prev;

  struct cubeb * context;

  struct pollfd * saved_fds; /* A copy of the pollfds passed in at init time. */
  struct pollfd * fds; /* Pointer to this waitable's pollfds within struct cubeb's fds. */
  nfds_t nfds;

  poll_waitable_callback callback;
  void * user_ptr;

  unsigned int idle_count;
  unsigned int refs;
};

struct cubeb {
  pthread_t thread;

  struct poll_timer * timer;
  struct poll_waitable * waitable;

  /* fds and nfds are only updated by cubeb_run when rebuild is set. */
  struct pollfd * fds;
  nfds_t nfds;
  int rebuild;

  int shutdown;

  /* Control pipe for forcing poll to wake and rebuild fds or recalculate timeout. */
  int control_fd_read;
  int control_fd_write;

  /* Mutex for timer and waitable lists, must not be held while blocked in
     poll(2) or when writing to control_fd */
  struct mutex mutex;
  pthread_cond_t cond;

  int phase;

  unsigned int active_streams;

  struct poll_timer * watchdog_timer;
};

struct cubeb_stream {
  cubeb * context;
  struct mutex mutex;
  snd_pcm_t * pcm;
  cubeb_data_callback data_callback;
  cubeb_state_callback state_callback;
  void * user_ptr;
  snd_pcm_uframes_t write_position;
  snd_pcm_uframes_t last_position;
  snd_pcm_uframes_t buffer_size;
  snd_pcm_uframes_t period_size;
  cubeb_stream_params params;

  struct poll_waitable * waitable;
  struct poll_timer * timer;
};

static void
mutex_lock(struct mutex * mtx)
{
  int r;

  r = pthread_mutex_lock(&mtx->mutex);
  assert(r == 0);
  mtx->owner = pthread_self();
  mtx->locked = 1;
}

static void
mutex_unlock(struct mutex * mtx)
{
  int r;

  memset(&mtx->owner, 0, sizeof(pthread_t));
  mtx->locked = 0;
  r = pthread_mutex_unlock(&mtx->mutex);
  assert(r == 0);
}

static void
mutex_assert_held(struct mutex * mtx)
{
  assert(mtx->locked && pthread_equal(mtx->owner, pthread_self()));
}

static void
mutex_assert_not_held(struct mutex * mtx)
{
  assert(!mtx->locked || !pthread_equal(mtx->owner, pthread_self()));
}

static long
stream_data_callback(cubeb_stream * stm, void * user_ptr, void * buffer, long nframes)
{
  mutex_assert_not_held(&stm->mutex);
  return stm->data_callback(stm, user_ptr, buffer, nframes);
}

static int
stream_state_callback(cubeb_stream * stm, void * user_ptr, cubeb_state state)
{
  mutex_assert_not_held(&stm->mutex);
  return stm->state_callback(stm, user_ptr, state);
}

static int
any_revents(struct pollfd * fds, nfds_t nfds)
{
  nfds_t i;

  for (i = 0; i < nfds; ++i) {
    if (fds[i].revents) {
      return 1;
    }
  }

  return 0;
}

static int
cmp_timeval(struct timeval * a, struct timeval * b)
{
  if (a->tv_sec == b->tv_sec) {
    if (a->tv_usec == b->tv_usec) {
      return 0;
    }
    return a->tv_usec > b->tv_usec ? 1 : -1;
  }
  return a->tv_sec > b->tv_sec ? 1 : -1;
}

static int
timeval_to_relative_ms(struct timeval * tv)
{
  struct timeval now;
  struct timeval dt;
  long long t;

  gettimeofday(&now, NULL);
  if (cmp_timeval(tv, &now) <= 0) {
    return 0;
  }

  timersub(tv, &now, &dt);
  t = dt.tv_sec;
  t *= 1000;
  t += (dt.tv_usec + 500) / 1000;
  return t <= INT_MAX ? t : INT_MAX;
}

static void
pipe_init(int * read_fd, int * write_fd)
{
  int r;
  int fd[2];

  r = pipe(fd);
  assert(r == 0);

  *read_fd = fd[0];
  *write_fd = fd[1];
}

static void
set_close_on_exec(int fd)
{
  long flags;
  int r;

  assert(fd >= 0);

  flags = fcntl(fd, F_GETFD);
  assert(flags >= 0);

  r = fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  assert(r == 0);
}

static void
set_non_block(int fd)
{
  long flags;
  int r;

  assert(fd >= 0);

  flags = fcntl(fd, F_GETFL);
  assert(flags >= 0);

  r = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  assert(r == 0);
}

static void
rebuild(struct cubeb * ctx)
{
  nfds_t nfds;
  int i;
  struct poll_waitable * item;

  mutex_assert_held(&ctx->mutex);
  assert(ctx->rebuild);

  nfds = 0;
  for (item = ctx->waitable; item; item = item->next) {
    nfds += item->nfds;
  }

  /* Special case: add control pipe fd. */
  nfds += 1;

  free(ctx->fds);
  ctx->fds = calloc(nfds, sizeof(struct pollfd));
  assert(ctx->fds);
  ctx->nfds = nfds;

  for (i = 0, item = ctx->waitable; item; item = item->next) {
    memcpy(&ctx->fds[i], item->saved_fds, item->nfds * sizeof(struct pollfd));
    item->fds = &ctx->fds[i];
    i += item->nfds;
  }

  /* Special case: add control pipe fd. */
  ctx->fds[i].fd = ctx->control_fd_read;
  ctx->fds[i].events = POLLIN | POLLERR;

  ctx->rebuild = 0;
}

static void
poll_woke(struct cubeb * ctx)
{
  ssize_t r;
  char dummy;

  r = read(ctx->control_fd_read, &dummy, 1);
  assert(dummy == 'x' && r == 1);
}

static void
poll_wake(struct cubeb * ctx)
{
  ssize_t r;
  char dummy;

  dummy = 'x';
  r = write(ctx->control_fd_write, &dummy, 1);
  assert(r == 1);
}

static struct poll_waitable *
poll_waitable_init(struct cubeb * ctx, struct pollfd * fds, nfds_t nfds,
                   poll_waitable_callback callback, void * user_ptr)
{
  struct poll_waitable * waitable;

  waitable = calloc(1, sizeof(struct poll_waitable));
  assert(waitable);
  waitable->context = ctx;

  waitable->saved_fds = calloc(nfds, sizeof(struct pollfd));
  assert(waitable->saved_fds);
  waitable->nfds = nfds;
  memcpy(waitable->saved_fds, fds, nfds * sizeof(struct pollfd));

  waitable->callback = callback;
  waitable->user_ptr = user_ptr;

  waitable->idle_count = 0;
  waitable->refs = 1;

  mutex_lock(&ctx->mutex);

  waitable->next = ctx->waitable;
  if (ctx->waitable) {
    ctx->waitable->prev = waitable;
  }
  ctx->waitable = waitable;
  ctx->rebuild = 1;

  poll_wake(ctx);
  mutex_unlock(&ctx->mutex);

  return waitable;
}

static void
poll_waitable_ref(struct poll_waitable * w)
{
  struct cubeb * ctx = w->context;

  mutex_assert_held(&ctx->mutex);
  w->refs += 1;
}

static void
poll_waitable_unref(struct poll_waitable * w)
{
  struct cubeb * ctx = w->context;

  mutex_lock(&ctx->mutex);

  w->refs -= 1;

  if (w->refs == 0) {
    if (w->next) {
      w->next->prev = w->prev;
    }
    if (w->prev) {
      w->prev->next = w->next;
    }

    if (ctx->waitable == w) {
      ctx->waitable = w->next;
    }

    free(w->saved_fds);
    free(w);

    ctx->rebuild = 1;
    poll_wake(ctx);
  }

  mutex_unlock(&ctx->mutex);
}

static struct poll_timer *
poll_timer_absolute_init(struct cubeb * ctx, struct timeval * wakeup,
                         poll_timer_callback callback, void * user_ptr)
{
  struct poll_timer * timer;
  struct poll_timer * item;

  timer = calloc(1, sizeof(*timer));
  assert(timer);
  timer->context = ctx;
  timer->wakeup = *wakeup;
  timer->callback = callback;
  timer->user_ptr = user_ptr;

  mutex_lock(&ctx->mutex);

  for (item = ctx->timer; item; item = item->next) {
    if (cmp_timeval(&timer->wakeup, &item->wakeup) < 0) {
      timer->next = item;
      timer->prev = item->prev;

      if (timer->prev) {
        timer->prev->next = timer;
      }
      item->prev = timer;

      break;
    }

    if (!item->next) {
      item->next = timer;
      timer->prev = item;
      break;
    }
  }

  if (!timer->prev) {
    ctx->timer = timer;
  }

  poll_wake(ctx);
  mutex_unlock(&ctx->mutex);

  return timer;
}

static struct poll_timer *
poll_timer_relative_init(struct cubeb * ctx, unsigned int ms,
                         poll_timer_callback callback, void * user_ptr)
{
  struct timeval wakeup;

  gettimeofday(&wakeup, NULL);
  wakeup.tv_sec += ms / 1000;
  wakeup.tv_usec += (ms % 1000) * 1000;

  return poll_timer_absolute_init(ctx, &wakeup, callback, user_ptr);
}

static void
poll_timer_destroy(struct poll_timer * t)
{
  struct cubeb * ctx = t->context;

  mutex_lock(&ctx->mutex);

  if (t->next) {
    t->next->prev = t->prev;
  }
  if (t->prev) {
    t->prev->next = t->next;
  }

  if (ctx->timer == t) {
    ctx->timer = t->next;
  }

  free(t);

  poll_wake(ctx);
  mutex_unlock(&ctx->mutex);
}

static void
poll_phase_wait(cubeb * ctx)
{
  int phase;

  mutex_lock(&ctx->mutex);
  phase = ctx->phase;
  while (ctx->phase == phase) {
    pthread_cond_wait(&ctx->cond, &ctx->mutex.mutex);
  }
  mutex_unlock(&ctx->mutex);
}

static int
cubeb_run(struct cubeb * ctx)
{
  int r;
  int timeout;
  struct poll_waitable * waitable;
  struct poll_timer * timer;
  struct poll_waitable ** ready;
  int i;

  mutex_lock(&ctx->mutex);

  if (ctx->rebuild) {
    rebuild(ctx);
  }

  timeout = -1;
  timer = ctx->timer;
  if (timer) {
    timeout = timeval_to_relative_ms(&timer->wakeup);
  }

  /* No timers or waitables, we're done. */
  if (timeout == -1 && ctx->nfds == 0) {
    return -1;
  }

  mutex_unlock(&ctx->mutex);
  r = poll(ctx->fds, ctx->nfds, timeout);
  mutex_lock(&ctx->mutex);

  if (r > 0) {
    if (ctx->fds[ctx->nfds - 1].revents & POLLIN) {
      poll_woke(ctx);
      if (ctx->shutdown) {
        mutex_unlock(&ctx->mutex);
        return -1;
      }
    }

    i = 0;
    ready = calloc(r, sizeof(struct poll_waitable *));

    /* TODO: Break once r pfds have been processed, ideally with a waitable
       list sorted by latency. */
    for (waitable = ctx->waitable; waitable; waitable = waitable->next) {
      if (waitable->fds && any_revents(waitable->fds, waitable->nfds)) {
        poll_waitable_ref(waitable);
        ready[i++] = waitable;
        waitable->idle_count = 0;
      }
    }

    mutex_unlock(&ctx->mutex);
    for (i = 0; i < r; ++i) {
      if (!ready[i]) {
        break;
      }
      ready[i]->callback(ready[i]->user_ptr, ready[i]->fds, ready[i]->nfds);
      poll_waitable_unref(ready[i]);
    }
    mutex_lock(&ctx->mutex);

    free(ready);
  } else if (r == 0) {
    assert(timer);
    mutex_unlock(&ctx->mutex);
    timer->callback(timer->user_ptr);
    mutex_lock(&ctx->mutex);
  }

  ctx->phase += 1;
  pthread_cond_broadcast(&ctx->cond);

  mutex_unlock(&ctx->mutex);

  return 0;
}

static void
cubeb_watchdog(void * context)
{
  cubeb * ctx = context;
  struct poll_waitable * waitable;
  struct poll_waitable * tmp[16];
  int broken_streams;
  int i;

  if (ctx->watchdog_timer) {
    poll_timer_destroy(ctx->watchdog_timer);
  }

  ctx->watchdog_timer = poll_timer_relative_init(ctx, 1000, cubeb_watchdog, ctx);

  mutex_lock(&ctx->mutex);
  /* XXX: Horrible hack -- if an active (registered) stream has been idle
     for 10 ticks of the watchdog, kill it and mark the stream in error.
     This works around a bug seen with older versions of ALSA and PulseAudio
     where streams would stop requesting new data despite still being
     logically active and playing. */
  broken_streams = 0;
  for (waitable = ctx->waitable; waitable; waitable = waitable->next) {
    waitable->idle_count += 1;
    if (waitable->idle_count >= 10) {
      poll_waitable_ref(waitable);
      tmp[broken_streams++] = waitable;
    }
  }

  mutex_unlock(&ctx->mutex);

  for (i = 0; i < broken_streams; ++i) {
    cubeb_stream * stm = tmp[i]->user_ptr;
    assert(tmp[i]->idle_count >= 10);
    mutex_lock(&stm->mutex);
    if (stm->waitable) {
      poll_waitable_unref(stm->waitable);
      stm->waitable = NULL;
    }
    mutex_unlock(&stm->mutex);
    poll_waitable_unref(tmp[i]);
    stream_state_callback(stm, stm->user_ptr, CUBEB_STATE_ERROR);
  }
}

static void
cubeb_drain_stream(void * stream)
{
  cubeb_stream * stm = stream;
  int drained = 0;

  mutex_lock(&stm->mutex);
  /* It's possible that the stream was stopped after the timer fired but
     before we locked the stream. */
  if (stm->timer) {
    poll_timer_destroy(stm->timer);
    stm->timer = NULL;
    drained = 1;
  }
  mutex_unlock(&stm->mutex);
  if (drained) {
    stream_state_callback(stm, stm->user_ptr, CUBEB_STATE_DRAINED);
  }
}

static void
cubeb_refill_stream(void * stream, struct pollfd * fds, nfds_t nfds)
{
  cubeb_stream * stm = stream;
  int r;
  unsigned short revents;
  snd_pcm_sframes_t avail;
  long got;
  void * p;

  mutex_lock(&stm->mutex);

  r = snd_pcm_poll_descriptors_revents(stm->pcm, fds, nfds, &revents);
  if (r < 0 || revents != POLLOUT) {
    /* This should be a stream error; it makes no sense for poll(2) to wake
       for this stream and then have the stream report that it's not ready.
       Unfortunately, this does happen, so just bail out and try again. */
    mutex_unlock(&stm->mutex);
    return;
  }

  avail = snd_pcm_avail_update(stm->pcm);
  if (avail == -EPIPE) {
    snd_pcm_recover(stm->pcm, avail, 1);
    avail = snd_pcm_avail_update(stm->pcm);
  }

  /* Failed to recover from an xrun, this stream must be broken. */
  if (avail < 0) {
    poll_waitable_unref(stm->waitable);
    stm->waitable = NULL;
    mutex_unlock(&stm->mutex);
    stream_state_callback(stm, stm->user_ptr, CUBEB_STATE_ERROR);
    return;
  }

  /* This should never happen. */
  if ((unsigned int) avail > stm->buffer_size) {
    avail = stm->buffer_size;
  }

  /* poll(2) claims this stream is active, so there should be some space
     available to write.  If avail is still zero here, the stream must be in
     a funky state, so recover and try again. */
  if (avail == 0) {
    snd_pcm_recover(stm->pcm, -EPIPE, 1);
    avail = snd_pcm_avail_update(stm->pcm);
    if (avail <= 0) {
      poll_waitable_unref(stm->waitable);
      stm->waitable = NULL;
      mutex_unlock(&stm->mutex);
      stream_state_callback(stm, stm->user_ptr, CUBEB_STATE_ERROR);
      return;
    }
  }

  p = calloc(1, snd_pcm_frames_to_bytes(stm->pcm, avail));
  assert(p);

  mutex_unlock(&stm->mutex);
  got = stream_data_callback(stm, stm->user_ptr, p, avail);
  mutex_lock(&stm->mutex);
  if (got < 0) {
    poll_waitable_unref(stm->waitable);
    stm->waitable = NULL;
    mutex_unlock(&stm->mutex);
    stream_state_callback(stm, stm->user_ptr, CUBEB_STATE_ERROR);
    return;
  }
  if (got > 0) {
    snd_pcm_sframes_t wrote = snd_pcm_writei(stm->pcm, p, got);
    if (wrote == -EPIPE) {
      snd_pcm_recover(stm->pcm, wrote, 1);
      wrote = snd_pcm_writei(stm->pcm, p, got);
    }
    assert(wrote >= 0 && wrote == got);
    stm->write_position += wrote;
  }
  if (got != avail) {
    long buffer_fill = stm->buffer_size - (avail - got);
    double buffer_time = (double) buffer_fill / stm->params.rate;

    /* Fill the remaining buffer with silence to guarantee at least a period has been written. */
    snd_pcm_writei(stm->pcm, (char *) p + got, avail - got);

    poll_waitable_unref(stm->waitable);
    stm->waitable = NULL;

    stm->timer = poll_timer_relative_init(stm->context, buffer_time * 1000,
                                          cubeb_drain_stream, stm);
  }

  free(p);
  mutex_unlock(&stm->mutex);
}

static void *
cubeb_run_thread(void * context)
{
  cubeb * ctx = context;
  int r;

  do {
    r = cubeb_run(ctx);
  } while (r >= 0);

  return NULL;
}

static int
cubeb_locked_pcm_open(snd_pcm_t ** pcm, snd_pcm_stream_t stream)
{
  int r;

  pthread_mutex_lock(&cubeb_alsa_mutex);
  r = snd_pcm_open(pcm, "default", stream, SND_PCM_NONBLOCK);
  pthread_mutex_unlock(&cubeb_alsa_mutex);

  return r;
}

static int
cubeb_locked_pcm_close(snd_pcm_t * pcm)
{
  int r;

  pthread_mutex_lock(&cubeb_alsa_mutex);
  r = snd_pcm_close(pcm);
  pthread_mutex_unlock(&cubeb_alsa_mutex);

  return r;
}

static cubeb_stream *
cubeb_new_stream(cubeb * ctx)
{
  cubeb_stream * stm = NULL;

  stm = calloc(1, sizeof(*stm));
  assert(stm);

  mutex_lock(&ctx->mutex);
  if (ctx->active_streams < CUBEB_STREAM_MAX) {
    ctx->active_streams += 1;
  } else {
    free(stm);
    stm = NULL;
  }
  mutex_unlock(&ctx->mutex);

  return stm;
}

static void
cubeb_free_stream(cubeb * ctx, cubeb_stream * stm)
{
  mutex_lock(&ctx->mutex);
  assert(ctx->active_streams >= 1);
  ctx->active_streams -= 1;
  mutex_unlock(&ctx->mutex);
  free(stm);
}

static void
silent_error_handler(char const * file UNUSED, int line UNUSED, char const * function UNUSED,
                     int err UNUSED, char const * fmt UNUSED, ...)
{
}

int
cubeb_init(cubeb ** context, char const * context_name UNUSED)
{
  cubeb * ctx;
  int r;
  pthread_attr_t attr;

  assert(context);
  *context = NULL;

  pthread_mutex_lock(&cubeb_alsa_mutex);
  if (!cubeb_alsa_set_error_handler) {
    snd_lib_error_set_handler(silent_error_handler);
    cubeb_alsa_set_error_handler = 1;
  }
  pthread_mutex_unlock(&cubeb_alsa_mutex);

  ctx = calloc(1, sizeof(*ctx));
  assert(ctx);

  pipe_init(&ctx->control_fd_read, &ctx->control_fd_write);

  set_close_on_exec(ctx->control_fd_read);
  set_non_block(ctx->control_fd_read);

  set_close_on_exec(ctx->control_fd_write);
  set_non_block(ctx->control_fd_write);

  r = pthread_mutex_init(&ctx->mutex.mutex, NULL);
  assert(r == 0);

  r = pthread_cond_init(&ctx->cond, NULL);
  assert(r == 0);

  ctx->phase = 0;

  /* Force an early rebuild when cubeb_run is first called to ensure fds and
   * nfds have been initialized. */
  ctx->rebuild = 1;

  cubeb_watchdog(ctx);

  r = pthread_attr_init(&attr);
  assert(r == 0);

  r = pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN);
  assert(r == 0);

  r = pthread_create(&ctx->thread, &attr, cubeb_run_thread, ctx);
  assert(r == 0);

  r = pthread_attr_destroy(&attr);
  assert(r == 0);

  ctx->active_streams = 0;

  *context = ctx;

  return CUBEB_OK;
}

void
cubeb_destroy(cubeb * ctx)
{
  int r;

  assert(ctx);
  assert(ctx->active_streams == 0);

  mutex_lock(&ctx->mutex);
  ctx->shutdown = 1;
  poll_wake(ctx);
  mutex_unlock(&ctx->mutex);

  r = pthread_join(ctx->thread, NULL);
  assert(r == 0);

  poll_timer_destroy(ctx->watchdog_timer);
  ctx->watchdog_timer = NULL;

  assert(!ctx->waitable && !ctx->timer);
  close(ctx->control_fd_read);
  close(ctx->control_fd_write);
  pthread_cond_destroy(&ctx->cond);
  pthread_mutex_destroy(&ctx->mutex.mutex);
  free(ctx->fds);

  free(ctx);
}

int
cubeb_stream_init(cubeb * context, cubeb_stream ** stream, char const * stream_name UNUSED,
                  cubeb_stream_params stream_params, unsigned int latency,
                  cubeb_data_callback data_callback, cubeb_state_callback state_callback,
                  void * user_ptr)
{
  cubeb_stream * stm;
  int r;
  snd_pcm_format_t format;

  assert(context && stream);

  *stream = NULL;

  if (stream_params.rate < 1 || stream_params.rate > 192000 ||
      stream_params.channels < 1 || stream_params.channels > 32 ||
      latency < 1 || latency > 2000) {
    return CUBEB_ERROR_INVALID_FORMAT;
  }

  switch (stream_params.format) {
  case CUBEB_SAMPLE_S16LE:
    format = SND_PCM_FORMAT_S16_LE;
    break;
  case CUBEB_SAMPLE_S16BE:
    format = SND_PCM_FORMAT_S16_BE;
    break;
  case CUBEB_SAMPLE_FLOAT32LE:
    format = SND_PCM_FORMAT_FLOAT_LE;
    break;
  case CUBEB_SAMPLE_FLOAT32BE:
    format = SND_PCM_FORMAT_FLOAT_BE;
    break;
  default:
    return CUBEB_ERROR_INVALID_FORMAT;
  }

  stm = cubeb_new_stream(context);
  if (!stm) {
    return CUBEB_ERROR;
  }

  stm->context = context;
  stm->data_callback = data_callback;
  stm->state_callback = state_callback;
  stm->user_ptr = user_ptr;
  stm->params = stream_params;

  r = pthread_mutex_init(&stm->mutex.mutex, NULL);
  assert(r == 0);

  r = cubeb_locked_pcm_open(&stm->pcm, SND_PCM_STREAM_PLAYBACK);
  if (r < 0) {
    cubeb_stream_destroy(stm);
    return CUBEB_ERROR;
  }

  r = snd_pcm_nonblock(stm->pcm, 1);
  assert(r == 0);

  r = snd_pcm_set_params(stm->pcm, format, SND_PCM_ACCESS_RW_INTERLEAVED,
                         stm->params.channels, stm->params.rate, 1,
                         latency * 1000);
  if (r < 0) {
    cubeb_stream_destroy(stm);
    return CUBEB_ERROR_INVALID_FORMAT;
  }

  r = snd_pcm_get_params(stm->pcm, &stm->buffer_size, &stm->period_size);
  assert(r == 0);

  *stream = stm;

  return CUBEB_OK;
}

void
cubeb_stream_destroy(cubeb_stream * stm)
{
  assert(stm && !stm->waitable && !stm->timer);

  mutex_lock(&stm->mutex);
  if (stm->pcm) {
    cubeb_locked_pcm_close(stm->pcm);
    stm->pcm = NULL;
  }
  mutex_unlock(&stm->mutex);
  pthread_mutex_destroy(&stm->mutex.mutex);

  cubeb_free_stream(stm->context, stm);
}

int
cubeb_stream_start(cubeb_stream * stm)
{
  int nfds;
  struct pollfd * fds;
  int r;

  assert(stm);

  mutex_lock(&stm->mutex);

  if (stm->waitable) {
    mutex_unlock(&stm->mutex);
    return CUBEB_OK;
  }

  snd_pcm_pause(stm->pcm, 0);

  nfds = snd_pcm_poll_descriptors_count(stm->pcm);
  assert(nfds > 0);

  fds = calloc(nfds, sizeof(struct pollfd));
  assert(fds);
  r = snd_pcm_poll_descriptors(stm->pcm, fds, nfds);
  assert(r == nfds);

  stm->waitable = poll_waitable_init(stm->context, fds, nfds, cubeb_refill_stream, stm);

  free(fds);
  mutex_unlock(&stm->mutex);

  return CUBEB_OK;
}

int
cubeb_stream_stop(cubeb_stream * stm)
{
  assert(stm);

  mutex_lock(&stm->mutex);

  if (stm->waitable) {
    poll_waitable_unref(stm->waitable);
    stm->waitable = NULL;
  }

  if (stm->timer) {
    poll_timer_destroy(stm->timer);
    stm->timer = NULL;
  }

  mutex_unlock(&stm->mutex);

  poll_phase_wait(stm->context);

  mutex_lock(&stm->mutex);

  snd_pcm_pause(stm->pcm, 1);

  mutex_unlock(&stm->mutex);

  return CUBEB_OK;
}

int
cubeb_stream_get_position(cubeb_stream * stm, uint64_t * position)
{
  snd_pcm_sframes_t delay;

  assert(stm && position);

  mutex_lock(&stm->mutex);

  delay = -1;
  if (snd_pcm_state(stm->pcm) != SND_PCM_STATE_RUNNING ||
      snd_pcm_delay(stm->pcm, &delay) != 0) {
    *position = stm->last_position;
    mutex_unlock(&stm->mutex);
    return CUBEB_OK;
  }

  assert(delay >= 0);

  *position = 0;
  if (stm->write_position >= (snd_pcm_uframes_t) delay) {
    *position = stm->write_position - delay;
  }

  stm->last_position = *position;

  mutex_unlock(&stm->mutex);
  return CUBEB_OK;
}
