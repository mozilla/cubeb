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

/* ALSA is not thread-safe.  snd_pcm_t instances are individually protected
   by the owning cubeb_stream's mutex.  snd_pcm_t creation and destruction
   is not thread-safe until ALSA 1.0.24 (see alsa-lib.git commit 91c9c8f1),
   so those calls must be wrapped in the following mutex. */
static pthread_mutex_t cubeb_alsa_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef void (*poll_waitable_callback)(void * user_ptr, struct pollfd * fds, nfds_t nfds);
typedef void (*poll_timer_callback)(void * user_ptr);

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

  unsigned int counter;
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
  pthread_mutex_t mutex;

  struct poll_timer * reaper_timer;
};

struct cubeb_stream {
  cubeb * context;
  pthread_mutex_t mutex;
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

  int error;
};

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

  flags = fcntl(fd, F_GETFD);
  assert(flags >= 0);

  flags |= FD_CLOEXEC;

  r = fcntl(fd, F_SETFD, flags);
  assert(r == 0);
}

static void
rebuild(struct cubeb * ctx)
{
  nfds_t nfds;
  int i;
  struct poll_waitable * item;

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

  waitable->next = ctx->waitable;
  if (ctx->waitable) {
    ctx->waitable->prev = waitable;
  }
  ctx->waitable = waitable;
  ctx->rebuild = 1;

  poll_wake(ctx);

  return waitable;
}

static void
poll_waitable_destroy(struct poll_waitable * w)
{
  struct cubeb * ctx = w->context;

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
}

static int
cubeb_run(struct cubeb * ctx)
{
  int r;
  int timeout;
  struct poll_waitable * waitable;
  struct poll_waitable * tmp;
  struct poll_timer * timer;

  pthread_mutex_lock(&ctx->mutex);

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

  pthread_mutex_unlock(&ctx->mutex);
  r = poll(ctx->fds, ctx->nfds, timeout);
  pthread_mutex_lock(&ctx->mutex);

  if (r > 0) {
    if (ctx->fds[ctx->nfds - 1].revents & POLLIN) {
      poll_woke(ctx);
      if (ctx->shutdown) {
        pthread_mutex_unlock(&ctx->mutex);
        return -1;
      }
    }

    /* TODO: Break once r pfds have been processed, ideally with a waitable
       list sorted by latency. */
    for (waitable = ctx->waitable; waitable && (tmp = waitable->next, 1); waitable = tmp) {
      if (waitable->fds && any_revents(waitable->fds, waitable->nfds)) {
        waitable->callback(waitable->user_ptr, waitable->fds, waitable->nfds);
        waitable->counter = 0;
      }
    }
  } else if (r == 0) {
    assert(timer);
    timer->callback(timer->user_ptr);
  }

  pthread_mutex_unlock(&ctx->mutex);

  return 0;
}

static void
cubeb_reaper(void * context)
{
  cubeb * ctx = context;
  struct poll_waitable * waitable;
  struct poll_waitable * tmp;

  if (ctx->reaper_timer) {
    poll_timer_destroy(ctx->reaper_timer);
  }

  /* XXX: Horrible hack -- if an active (registered) stream has been idle
     for 10 ticks of the reaper, kill it and mark the stream in error.  This
     works around a bug seen with older versions of ALSA and PulseAudio
     where streams would stop requesting new data despite still being
     logically active and playing. */
  for (waitable = ctx->waitable; waitable && (tmp = waitable->next, 1); waitable = tmp) {
    waitable->counter += 1;
    if (waitable->counter == 10) {
      cubeb_stream * stm = waitable->user_ptr;
      pthread_mutex_lock(&stm->mutex);
      poll_waitable_destroy(stm->waitable);
      stm->waitable = NULL;
      stm->error = 1;
      pthread_mutex_unlock(&stm->mutex);
      stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_ERROR);
    }
  }

  ctx->reaper_timer = poll_timer_relative_init(ctx, 1000, cubeb_reaper, ctx);
}

static void
cubeb_drain_stream(void * stream)
{
  cubeb_stream * stm = stream;

  stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_DRAINED);
  pthread_mutex_lock(&stm->mutex);
  poll_timer_destroy(stm->timer);
  stm->timer = NULL;
  pthread_mutex_unlock(&stm->mutex);
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

  pthread_mutex_lock(&stm->mutex);

  r = snd_pcm_poll_descriptors_revents(stm->pcm, fds, nfds, &revents);
  if (r < 0 || revents != POLLOUT) {
#if 0
    /* Running with raw ALSA (no alsa-pulse) results in this situation happening regularly. */
    poll_waitable_destroy(stm->waitable);
    stm->waitable = NULL;
    stm->error = 1;
#endif
    pthread_mutex_unlock(&stm->mutex);
    return;
  }

  avail = snd_pcm_avail_update(stm->pcm);
  if (avail == -EPIPE) {
    snd_pcm_recover(stm->pcm, avail, 1);
    avail = snd_pcm_avail_update(stm->pcm);
  }
  if (avail < 0) {
    poll_waitable_destroy(stm->waitable);
    stm->waitable = NULL;
    stm->error = 1;
    pthread_mutex_unlock(&stm->mutex);
    return;
  }

  if ((unsigned int) avail >= stm->buffer_size * 5 ||
      (unsigned int) avail >= stm->params.rate * 10) {
    avail = stm->period_size;
  }

  if ((unsigned int) avail > stm->buffer_size) {
    avail = stm->buffer_size;
  }

  /* XXX handle avail == 0 */
  if (avail == 0) {
    snd_pcm_recover(stm->pcm, -EPIPE, 1);
    avail = snd_pcm_avail_update(stm->pcm);
  }

  p = calloc(1, snd_pcm_frames_to_bytes(stm->pcm, avail));
  assert(p);

  pthread_mutex_unlock(&stm->mutex);
  got = stm->data_callback(stm, stm->user_ptr, p, avail);
  pthread_mutex_lock(&stm->mutex);
  if (got < 0) {
    assert(0); /* XXX handle this case */
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

    poll_waitable_destroy(stm->waitable);
    stm->waitable = NULL;

    stm->timer = poll_timer_relative_init(stm->context, buffer_time * 1000,
                                          cubeb_drain_stream, stm);
  }

  free(p);
  pthread_mutex_unlock(&stm->mutex);
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

int
cubeb_init(cubeb ** context, char const * context_name)
{
  cubeb * ctx;
  int r;
  pthread_attr_t attr;

  assert(context);
  *context = NULL;

  ctx = calloc(1, sizeof(*ctx));
  assert(ctx);

  pipe_init(&ctx->control_fd_read, &ctx->control_fd_write);

  set_close_on_exec(ctx->control_fd_read);
  set_close_on_exec(ctx->control_fd_write);

  r = pthread_mutex_init(&ctx->mutex, NULL);
  assert(r == 0);

  /* Force an early rebuild when cubeb_run is first called to ensure fds and
   * nfds have been initialized. */
  ctx->rebuild = 1;

  cubeb_reaper(ctx);

  r = pthread_attr_init(&attr);
  assert(r == 0);

  r = pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN);
  assert(r == 0);

  r = pthread_create(&ctx->thread, &attr, cubeb_run_thread, ctx);
  assert(r == 0);

  r = pthread_attr_destroy(&attr);
  assert(r == 0);

  *context = ctx;

  return CUBEB_OK;
}

void
cubeb_destroy(cubeb * ctx)
{
  int r;

  assert(ctx);

  pthread_mutex_lock(&ctx->mutex);
  ctx->shutdown = 1;
  poll_wake(ctx);
  pthread_mutex_unlock(&ctx->mutex);

  r = pthread_join(ctx->thread, NULL);
  assert(r == 0);

  poll_timer_destroy(ctx->reaper_timer);
  ctx->reaper_timer = NULL;

  assert(!ctx->waitable);
  assert(!ctx->timer);
  close(ctx->control_fd_read);
  close(ctx->control_fd_write);
  pthread_mutex_destroy(&ctx->mutex);
  free(ctx->fds);

  free(ctx);
}

int
cubeb_stream_init(cubeb * context, cubeb_stream ** stream, char const * stream_name,
                  cubeb_stream_params stream_params, unsigned int latency,
                  cubeb_data_callback data_callback, cubeb_state_callback state_callback,
                  void * user_ptr)
{
  cubeb_stream * stm;
  int r;
  snd_pcm_format_t format;

  assert(context);
  assert(stream);

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

  stm = calloc(1, sizeof(*stm));
  assert(stm);

  stm->context = context;
  stm->data_callback = data_callback;
  stm->state_callback = state_callback;
  stm->user_ptr = user_ptr;
  stm->params = stream_params;

  r = pthread_mutex_init(&stm->mutex, NULL);
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
    /* XXX return format error if necessary */
    cubeb_stream_destroy(stm);
    return CUBEB_ERROR;
  }

  r = snd_pcm_get_params(stm->pcm, &stm->buffer_size, &stm->period_size);
  assert(r == 0);

  *stream = stm;

  return CUBEB_OK;
}

void
cubeb_stream_destroy(cubeb_stream * stm)
{
  assert(stm);

  pthread_mutex_lock(&stm->mutex);
  if (stm->pcm) {
    cubeb_locked_pcm_close(stm->pcm);
    stm->pcm = NULL;
  }
  pthread_mutex_unlock(&stm->mutex);
  pthread_mutex_destroy(&stm->mutex);

  free(stm);
}

int
cubeb_stream_start(cubeb_stream * stm)
{
  int nfds;
  struct pollfd * fds;
  int r;

  assert(stm);

  pthread_mutex_lock(&stm->context->mutex);
  pthread_mutex_lock(&stm->mutex);
  if (stm->waitable) {
    pthread_mutex_unlock(&stm->mutex);
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
  pthread_mutex_unlock(&stm->mutex);
  pthread_mutex_unlock(&stm->context->mutex);

  return CUBEB_OK;
}

int
cubeb_stream_stop(cubeb_stream * stm)
{
  assert(stm);

  pthread_mutex_lock(&stm->context->mutex);
  pthread_mutex_lock(&stm->mutex);
  if (stm->waitable) {
    poll_waitable_destroy(stm->waitable);
    stm->waitable = NULL;
  }

  if (stm->timer) {
    poll_timer_destroy(stm->timer);
    stm->timer = NULL;
  }

  snd_pcm_pause(stm->pcm, 1);

  pthread_mutex_unlock(&stm->mutex);
  pthread_mutex_unlock(&stm->context->mutex);

  return CUBEB_OK;
}

int
cubeb_stream_get_position(cubeb_stream * stm, uint64_t * position)
{
  snd_pcm_sframes_t delay;

  assert(stm);
  assert(position);

  pthread_mutex_lock(&stm->mutex);

  delay = -1;
  if (snd_pcm_state(stm->pcm) != SND_PCM_STATE_RUNNING ||
      snd_pcm_delay(stm->pcm, &delay) != 0) {
    *position = stm->last_position;
    pthread_mutex_unlock(&stm->mutex);
    return CUBEB_OK;
  }

  assert(delay >= 0);

  if ((unsigned int) delay >= stm->buffer_size * 5 ||
      (unsigned int) delay >= stm->params.rate * 10) {
    delay = 0;
  }

  *position = 0;
  if (stm->write_position >= (snd_pcm_uframes_t) delay) {
    *position = stm->write_position - delay;
  }

  stm->last_position = *position;

  pthread_mutex_unlock(&stm->mutex);
  return CUBEB_OK;
}
