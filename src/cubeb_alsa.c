/*
 * Copyright © 2011 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#undef NDEBUG
#define _BSD_SOURCE 1
#define _POSIX_SOURCE 1
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
   so those calls must be wrapped in the following global mutex. */
static pthread_mutex_t cubeb_alsa_mutex = PTHREAD_MUTEX_INITIALIZER;

#define XPOLL_WAITABLE_CONTINUE 0
#define XPOLL_WAITABLE_REMOVE   1

typedef int (*xpoll_waitable_callback)(void * user_ptr, struct pollfd * fds, nfds_t nfds);
typedef void (*xpoll_timer_callback)(void * user_ptr);

struct xpoll_timer {
  struct xpoll_timer * next;
  struct xpoll_timer * prev;

  struct xpoll * xpoll;

  struct timeval wakeup;

  xpoll_timer_callback callback;
  void * user_ptr;
};

struct xpoll_waitable {
  struct xpoll_waitable * next;
  struct xpoll_waitable * prev;

  struct xpoll * xpoll;

  struct pollfd * saved_fds; /* A copy of the pollfds passed in at init time. */
  struct pollfd * fds; /* Pointer to this waitable's pollfds within struct xpoll's fds. */
  nfds_t nfds;

  xpoll_waitable_callback callback;
  void * user_ptr;
};

struct xpoll {
  pthread_mutex_t timer_mutex;
  struct xpoll_timer * timer;

  pthread_mutex_t waitable_mutex;
  struct xpoll_waitable * waitable;

  /* fds and nfds are only updated by xpoll_run when rebuild is set. */
  struct pollfd * fds;
  nfds_t nfds;
  int rebuild;

  /* Waitable for forcing poll to wake and rebuild fds or recalculate timeout. */
  int control_fd;
  struct xpoll_waitable * control;
};

struct cubeb {
  pthread_t thread;
  struct xpoll * xpoll;
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
  cubeb_stream_params params;

  struct xpoll_waitable * waitable;
  struct xpoll_timer * timer;
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
rebuild(struct xpoll * p)
{
  nfds_t nfds;
  int i;
  struct xpoll_waitable * item;

  assert(p->rebuild);

  nfds = 0;
  for (item = p->waitable; item; item = item->next) {
    nfds += item->nfds;
  }

  free(p->fds);
  p->fds = calloc(nfds, sizeof(struct pollfd));
  assert(p->fds);
  p->nfds = nfds;

  for (i = 0, item = p->waitable; item; item = item->next) {
    memcpy(&p->fds[i], item->saved_fds, item->nfds * sizeof(struct pollfd));
    item->fds = &p->fds[i];
    i += item->nfds;
  }

  p->rebuild = 0;
}

static struct xpoll_waitable *
xpoll_waitable_init(struct xpoll * p, struct pollfd * fds, nfds_t nfds,
                    xpoll_waitable_callback callback, void * user_ptr)
{
  struct xpoll_waitable * w;

  w = calloc(1, sizeof(struct xpoll_waitable));
  assert(w);
  w->xpoll = p;

  w->saved_fds = calloc(nfds, sizeof(struct pollfd));
  assert(w->saved_fds);
  w->nfds = nfds;
  memcpy(w->saved_fds, fds, nfds * sizeof(struct pollfd));

  w->callback = callback;
  w->user_ptr = user_ptr;

  pthread_mutex_lock(&p->waitable_mutex);

  w->next = p->waitable;
  if (p->waitable) {
    p->waitable->prev = w;
  }
  p->waitable = w;

  p->rebuild = 1;

  pthread_mutex_unlock(&p->waitable_mutex);

  write(p->control_fd, "x", 1);

  return w;
}

static struct xpoll_waitable *
waitable_destroy_unlocked(struct xpoll_waitable * w)
{
  struct xpoll_waitable * next;

  next = w->next;
  if (w->next) {
    w->next->prev = w->prev;
  }
  if (w->prev) {
    w->prev->next = w->next;
  }
  if (w->xpoll->waitable == w) {
    w->xpoll->waitable = w->next;
  }

  w->xpoll->rebuild = 1;

  free(w->saved_fds);
  free(w);

  return next;
}

static void
xpoll_waitable_destroy(struct xpoll_waitable * w)
{
  struct xpoll * p = w->xpoll;

  pthread_mutex_lock(&p->waitable_mutex);
  waitable_destroy_unlocked(w);
  pthread_mutex_unlock(&p->waitable_mutex);
  write(p->control_fd, "x", 1);
}

static struct xpoll_timer *
xpoll_timer_absolute_oneshot(struct xpoll * p, struct timeval * wakeup,
                             xpoll_timer_callback callback, void * user_ptr)
{
  struct xpoll_timer * timer;
  struct xpoll_timer * item;

  timer = calloc(1, sizeof(*timer));
  assert(timer);
  timer->xpoll = p;
  timer->wakeup = *wakeup;
  timer->callback = callback;
  timer->user_ptr = user_ptr;

  pthread_mutex_lock(&p->timer_mutex);

  for (item = p->timer; item; item = item->next) {
    if (cmp_timeval(wakeup, &item->wakeup) < 0) {
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
    p->timer = timer;
  }

  pthread_mutex_unlock(&p->timer_mutex);

  write(p->control_fd, "x", 1);

  return timer;
}

static struct xpoll_timer *
xpoll_timer_relative_oneshot(struct xpoll * p, unsigned int ms,
                             xpoll_timer_callback callback, void * user_ptr)
{
  struct timeval wakeup;

  gettimeofday(&wakeup, NULL);
  wakeup.tv_sec += ms / 1000;
  wakeup.tv_usec += (ms % 1000) * 1000;

  return xpoll_timer_absolute_oneshot(p, &wakeup, callback, user_ptr);
}

static void
timer_destroy_unlocked(struct xpoll_timer * t)
{
  if (t->next) {
    t->next->prev = t->prev;
  }
  if (t->prev) {
    t->prev->next = t->next;
  }
  if (t->xpoll->timer == t) {
    t->xpoll->timer = t->next;
  }

  free(t);
}

static void
xpoll_timer_destroy(struct xpoll_timer * t)
{
  struct xpoll * p = t->xpoll;

  pthread_mutex_lock(&p->timer_mutex);
  timer_destroy_unlocked(t);
  pthread_mutex_unlock(&p->timer_mutex);

  write(p->control_fd, "x", 1);
}

static int
wakeup(void * user_ptr, struct pollfd * fds, nfds_t nfds)
{
  char dummy;

  assert(nfds == 1);
  assert(fds[0].revents == POLLIN);

  read(fds[0].fd, &dummy, 1);
  return XPOLL_WAITABLE_CONTINUE;
}

static struct xpoll *
xpoll_init(void)
{
  struct xpoll * p;
  int r;
  int pipe_fd[2];
  struct pollfd fd;

  p = calloc(1, sizeof(struct xpoll));
  assert(p);

  pthread_mutex_init(&p->waitable_mutex, NULL);
  pthread_mutex_init(&p->timer_mutex, NULL);

  r = pipe(pipe_fd);
  assert(r == 0);

  fd.fd = pipe_fd[0];
  fd.events = POLLIN;

  p->control_fd = pipe_fd[1];

  p->control = xpoll_waitable_init(p, &fd, 1, wakeup, p);

  /* Force an early rebuild when xpoll_run is called to ensure fds and nfds
   * have been initialized. */
  p->rebuild = 1;

  return p;
}

static void
xpoll_quit(struct xpoll * p)
{
  xpoll_waitable_destroy(p->control);
}

static void
xpoll_destroy(struct xpoll * p)
{
  assert(!p->waitable);
  assert(!p->timer);
  free(p->fds);
  pthread_mutex_destroy(&p->waitable_mutex);
  pthread_mutex_destroy(&p->timer_mutex);
  free(p);
}

static int
xpoll_run(struct xpoll * p)
{
  int r;
  int timeout;
  struct xpoll_waitable * waitable;
  struct xpoll_timer * timer;

  pthread_mutex_lock(&p->waitable_mutex);
  if (p->rebuild) {
    rebuild(p);
  }
  pthread_mutex_unlock(&p->waitable_mutex);

  pthread_mutex_lock(&p->timer_mutex);
  timeout = -1;
  timer = p->timer;
  if (timer) {
    timeout = timeval_to_relative_ms(&timer->wakeup);
  }
  pthread_mutex_unlock(&p->timer_mutex);

  /* No timers or waitables, we're done. */
  if (timeout == -1 && p->nfds == 0) {
    return -1;
  }

  r = poll(p->fds, p->nfds, timeout);

  if (r > 0) {
    pthread_mutex_lock(&p->waitable_mutex);
    for (waitable = p->waitable; waitable; ) {
      if (waitable->fds && any_revents(waitable->fds, waitable->nfds)) {
        r = waitable->callback(waitable->user_ptr, waitable->fds, waitable->nfds);
        if (r == XPOLL_WAITABLE_REMOVE) {
          waitable = waitable_destroy_unlocked(waitable);
          continue;
        }
      }
      waitable = waitable->next;
    }
    pthread_mutex_unlock(&p->waitable_mutex);
  } else if (r == 0) {
    pthread_mutex_lock(&p->timer_mutex);
    if (timer == p->timer) {
      timer->callback(timer->user_ptr);
      timer_destroy_unlocked(timer);
    }
    pthread_mutex_unlock(&p->timer_mutex);
  }

  return 0;
}

static void
cubeb_drain_stream(void * stream)
{
  cubeb_stream * stm = stream;

  stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_DRAINED);
  stm->timer = NULL;
}

static int
cubeb_refill_stream(void * stream, struct pollfd * fds, nfds_t nfds)
{
  cubeb_stream * stm = stream;
  int r;
  unsigned short revents;
  snd_pcm_sframes_t avail;
  long got;
  void * p;

  r = snd_pcm_poll_descriptors_revents(stm->pcm, fds, nfds, &revents);
  if (r < 0 || revents == POLLERR) {
    stm->waitable = NULL;
    return XPOLL_WAITABLE_REMOVE;
  }

  if (revents != POLLOUT) {
    return XPOLL_WAITABLE_CONTINUE;
  }

  avail = snd_pcm_avail_update(stm->pcm);
  if (avail == -EPIPE) {
    snd_pcm_recover(stm->pcm, avail, 1);
    avail = snd_pcm_avail_update(stm->pcm);
  }
  if (avail < 0) {
    stm->waitable = NULL;
    return XPOLL_WAITABLE_REMOVE;
  }

  p = calloc(1, snd_pcm_frames_to_bytes(stm->pcm, avail));
  assert(p);

  got = stm->data_callback(stm, stm->user_ptr, p, avail);
  if (got < 0) {
    assert(0); /* XXX handle this case */
  }
  if (got > 0) {
    snd_pcm_sframes_t wrote = snd_pcm_writei(stm->pcm, p, got);
    stm->write_position += wrote;
  }
  if (got != avail) {
    long buffer_fill = stm->buffer_size - (avail - got);
    double buffer_time = (double) buffer_fill / stm->params.rate;

    /* Fill the remaining buffer with silence to guarantee at least a period has been written. */
    snd_pcm_writei(stm->pcm, (char *) p + got, avail - got);

    stm->waitable = NULL;

    stm->timer = xpoll_timer_relative_oneshot(stm->context->xpoll, buffer_time * 1000,
                                              cubeb_drain_stream, stm);
  }

  free(p);

  return stm->waitable ? XPOLL_WAITABLE_CONTINUE : XPOLL_WAITABLE_REMOVE;
}

static void *
cubeb_run_thread(void * context)
{
  cubeb * ctx = context;
  int r;

  do {
    r = xpoll_run(ctx->xpoll);
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

  ctx = calloc(1, sizeof(*ctx));
  assert(ctx);

  ctx->xpoll = xpoll_init();
  assert(ctx->xpoll);

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

  xpoll_quit(ctx->xpoll);

  r = pthread_join(ctx->thread, NULL);
  assert(r == 0);

  xpoll_destroy(ctx->xpoll);

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
  snd_pcm_uframes_t period_size;

  assert(context);
  assert(stream);

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

  r = snd_pcm_get_params(stm->pcm, &stm->buffer_size, &period_size);
  assert(r == 0);

  *stream = stm;

  return CUBEB_OK;
}

void
cubeb_stream_destroy(cubeb_stream * stm)
{
  assert(stm);

  if (stm->pcm) {
    cubeb_locked_pcm_close(stm->pcm);
  }

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

  pthread_mutex_lock(&stm->mutex);
  if (stm->waitable) {
    pthread_mutex_unlock(&stm->mutex);
    return CUBEB_OK;
  }

  nfds = snd_pcm_poll_descriptors_count(stm->pcm);
  assert(nfds > 0);

  fds = calloc(nfds, sizeof(struct pollfd));
  assert(fds);
  r = snd_pcm_poll_descriptors(stm->pcm, fds, nfds);
  assert(r == nfds);

  snd_pcm_pause(stm->pcm, 0);
  stm->waitable = xpoll_waitable_init(stm->context->xpoll, fds, nfds, cubeb_refill_stream, stm);

  free(fds);
  pthread_mutex_unlock(&stm->mutex);

  return CUBEB_OK;
}

int
cubeb_stream_stop(cubeb_stream * stm)
{
  assert(stm);

  pthread_mutex_lock(&stm->mutex);
  if (stm->waitable) {
    xpoll_waitable_destroy(stm->waitable);
    stm->waitable = NULL;
  }
  if (stm->timer) {
    xpoll_timer_destroy(stm->timer);
    stm->timer = NULL;
  }
  snd_pcm_pause(stm->pcm, 1);
  pthread_mutex_unlock(&stm->mutex);

  return CUBEB_OK;
}

int
cubeb_stream_get_position(cubeb_stream * stm, uint64_t * position)
{
  snd_pcm_sframes_t delay;

  assert(stm);
  assert(position);

  pthread_mutex_lock(&stm->mutex);

  if (snd_pcm_state(stm->pcm) != SND_PCM_STATE_RUNNING ||
      snd_pcm_delay(stm->pcm, &delay) != 0) {
    *position = stm->last_position;
    pthread_mutex_unlock(&stm->mutex);
    return CUBEB_OK;
  }

  assert(delay >= 0);

  *position = 0;
  if (stm->write_position >= (snd_pcm_uframes_t) delay) {
    *position = stm->write_position - delay;
  }

  stm->last_position = *position;

  pthread_mutex_unlock(&stm->mutex);
  return CUBEB_OK;
}
