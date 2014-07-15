/*
 * Copyright © 2011 Mozilla Foundation
 * Copyright (c) 2011 Alexandre Ratchov <alex@caoua.org>
 * Copyright © 2014 Andriy Voskoboinyk <andriivos@gmail.com>
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/ioctl.h>
#include <sys/soundcard.h>

#include "cubeb/cubeb.h"
#include "cubeb-internal.h"

#define DEFAULT_OUTPUT_DEVICE "/dev/dsp"

#define CUBEB_STREAM_MAX 16
#define MIN_LATENCY 20
#define NBUFS 5

static struct cubeb_ops const oss_ops;

struct cubeb {
  struct cubeb_ops const * ops;

  pthread_t thread;

  pthread_mutex_t mutex;

  pthread_cond_t cond;

  /* Sparse array of streams managed by this context. */
  cubeb_stream * streams[CUBEB_STREAM_MAX];

  /* fds are only updated by run_thread when rebuild is set. */
  struct pollfd fds[CUBEB_STREAM_MAX + 1];
  nfds_t nfds;
  unsigned int rebuild;

  /* Control pipe for forcing poll to wake and rebuild fds */
  int control_fd_read;
  int control_fd_write;

  /* Track number of active streams.  This is limited to CUBEB_STREAM_MAX
     due to resource contraints. */
  /* Set it to -1 when shutdown is needed */
  int active_streams;
};

typedef enum {
  INACTIVE,
  STARTED,
  RUNNING,
  PROCESSING,
  STOPPING,
  DRAINED,
  ERROR
} stream_state;

struct cubeb_stream {
  cubeb * context;
  stream_state state;
  cubeb_data_callback data_callback;
  cubeb_state_callback state_callback;
  int fd;
  int conv;
  long nfr;
  pthread_mutex_t mutex;
  uint64_t pos;
  unsigned int size;
  unsigned int bpf;
  unsigned int channels;
  void * arg;
};

typedef struct {
  unsigned int rate;
  unsigned int format;
  unsigned int disable_set_latency;
  unsigned int buf_latency;
  unsigned int total_latency;
  unsigned int trigger_level;
} init_data;

static void
poll_wake(cubeb * ctx)
{
  (void)write(ctx->control_fd_write, "x", 1);
}

static void
set_stream_state(cubeb_stream * s, stream_state state)
{
  int internal = 0;
  cubeb_state cb_state;
  cubeb * ctx;

  switch (state) {
  case RUNNING:
  case PROCESSING:
  case STOPPING:
    internal = 1;
    break;
  case STARTED:
    state = RUNNING;
    cb_state = CUBEB_STATE_STARTED;
    break;
  case DRAINED:
    cb_state = CUBEB_STATE_DRAINED;
    break;
  case ERROR:
    cb_state = CUBEB_STATE_ERROR;
    break;
  case INACTIVE:
    cb_state = CUBEB_STATE_STOPPED;
    break;
  default:
    assert(0);
  }

  pthread_mutex_lock(&s->mutex);
  s->state = state;
  pthread_mutex_unlock(&s->mutex);

  if (!internal) {
    ctx = s->context;

    if (ctx->rebuild != 1) {
      poll_wake(ctx);
      ctx->rebuild = 1;
    }

    s->state_callback(s, s->arg, cb_state);
  }
}

static void
float_to_s16(void * ptr, long nsamp)
{
  float * src = ptr;
  int16_t * dst = ptr;

  while (nsamp-- > 0) {
    *(dst++) = *(src++) * 32767;
  }
}

static stream_state
oss_refill_stream(cubeb_stream * s)
{
  char buf[s->size];
  long got, start, size, written;

  got = s->data_callback(s, s->arg, buf, s->nfr);

  if (got < 0) {
    return ERROR;
  }

  start = 0;
  size = got * s->bpf;

  if (s->conv == 1) {
    float_to_s16(buf, got*s->channels);
  }

  if (got > 0) {
    pthread_mutex_lock(&s->mutex);
    do {
      written = write(s->fd, buf + start, size - start);

      if (written > 0) {
        start += written;
      } else {
        if (errno == EINTR) {
          continue;
        } else {
          s->pos += start / s->bpf;
          pthread_mutex_unlock(&s->mutex);
          return ERROR;
        }
      }
    } while (size > start);

    s->pos += got;
    pthread_mutex_unlock(&s->mutex);
  }

  if (got != s->nfr) {
    return DRAINED;
  }

  return RUNNING;
}

static int
rebuild(cubeb * ctx)
{
  int i, r, d;
  cubeb_stream * s[CUBEB_STREAM_MAX];

  assert(ctx->rebuild);

  memcpy(s, ctx->streams, CUBEB_STREAM_MAX * sizeof(cubeb_stream *));

  for (i = 0, r = 0, d = CUBEB_STREAM_MAX - 1; i != CUBEB_STREAM_MAX; ++i) {
    if (s[i] && s[i]->state == RUNNING) {
      ctx->fds[r].fd = s[i]->fd;
      ctx->streams[r] = s[i];
      r += 1;
    } else {
      ctx->fds[d].fd = -1;
      ctx->streams[d] = s[i];
      d -= 1;
    }
  }

  assert(r == d + 1);

  ctx->rebuild = 0;

  return r;
}

static void *
run_thread(void * context)
{
  int i, running;
  stream_state state;
  char dummy;
  cubeb * ctx;
  cubeb_stream * s;

  ctx = (cubeb *)context;

  for (running = 0;;) {
    i = poll(ctx->fds, ctx->nfds, -1);

    if (i > 0) {
      if (ctx->fds[ctx->nfds - 1].revents & POLLIN) {
        (void)read(ctx->control_fd_read, &dummy, 1);

        pthread_mutex_lock(&ctx->mutex);
        if (ctx->active_streams == -1) {
          pthread_mutex_unlock(&ctx->mutex);
          break;
        }

        if (ctx->rebuild == 1) {
          running = rebuild(ctx);
        }

        pthread_mutex_unlock(&ctx->mutex);
      }

      pthread_mutex_lock(&ctx->mutex);
      for (i = 0; i < running; ++i) {
        s = ctx->streams[i];
        if (ctx->fds[i].revents & POLLOUT && s && s->state == RUNNING) {
          set_stream_state(s, PROCESSING);
          pthread_mutex_unlock(&ctx->mutex);

          state = oss_refill_stream(s);

          pthread_mutex_lock(&ctx->mutex);
          if (s->state == STOPPING) {
            state = (state == RUNNING) ? INACTIVE : state;
            set_stream_state(s, state);
            pthread_cond_signal(&ctx->cond);
          } else {
            set_stream_state(s, state);
          }
        }
      }
      pthread_mutex_unlock(&ctx->mutex);
    }
  }

  return NULL;
}

static char const *
oss_get_backend_id(cubeb * context)
{
  (void)context;

  return "oss";
}

int
oss_init(cubeb ** context, char const * context_name)
{
  cubeb * ctx;
  int i, ret;
  int fd[2];

  if ((ctx = calloc(1, sizeof(*ctx))) == NULL) {
    return CUBEB_ERROR;
  }

  ctx->ops = &oss_ops;

  ret = pthread_mutex_init(&ctx->mutex, NULL);
  assert(ret == 0);

  ret = pthread_cond_init(&ctx->cond, NULL);
  assert(ret == 0);

  ret = pipe(fd);
  assert(ret == 0);

  for (i = 0; i < 2; ++i) {
    fcntl(fd[i], F_SETFD, fcntl(fd[i], F_GETFD) | FD_CLOEXEC);
    fcntl(fd[i], F_SETFL, fcntl(fd[i], F_GETFL) | O_NONBLOCK);
  }

  ctx->control_fd_read = fd[0];
  ctx->control_fd_write = fd[1];

  ctx->nfds = CUBEB_STREAM_MAX + 1;

  /* Include context's control pipe fd. */
  ctx->fds[ctx->nfds - 1].fd = ctx->control_fd_read;
  ctx->fds[ctx->nfds - 1].events = POLLIN;

  for (i = 0; i < CUBEB_STREAM_MAX; ++i) {
    ctx->fds[i].fd = -1;
    ctx->fds[i].events = POLLOUT;
  }

  ret = pthread_create(&ctx->thread, NULL, run_thread, ctx);
  assert(ret == 0);

  *context = ctx;

  (void)context_name;

  return CUBEB_OK;
}

static void
oss_destroy(cubeb * ctx)
{
  int ret;

  pthread_mutex_lock(&ctx->mutex);
  assert(ctx->active_streams == 0);
  ctx->active_streams = -1;
  pthread_mutex_unlock(&ctx->mutex);

  poll_wake(ctx);

  ret = pthread_join(ctx->thread, NULL);
  assert(ret == 0);

  close(ctx->control_fd_read);
  close(ctx->control_fd_write);

  ret = pthread_mutex_destroy(&ctx->mutex);
  assert(ret == 0);

  ret = pthread_cond_destroy(&ctx->cond);
  assert(ret == 0);

  free(ctx);
}

static int
oss_start(int * fd)
{
  int i, flags = O_WRONLY;

  *fd = open(DEFAULT_OUTPUT_DEVICE, flags);
  for (i = 0; *fd < 0 && errno == EBUSY && i != 3; ++i) {
    sleep(1);
    *fd = open(DEFAULT_OUTPUT_DEVICE, flags);
  }

  if (*fd < 0) {
    perror("[oss] ERROR: Cannot open default sound device");
    return CUBEB_ERROR;
  }

  return CUBEB_OK;
}

static void
oss_set_latency(const cubeb_stream * s, init_data * d)
{
  const int frag = 10;
  int i, tmp;

  /* size for single fragment */
  tmp = d->rate * s->bpf * frag / 1000;

  /* minimum 2^i which can hold fragment */
  for (i = 0; (tmp >> i) != 0; ++i);

  /* number of fragments */
  tmp = d->total_latency * tmp / ((1 << i) * frag) + 1;

  tmp = (tmp << 16) + i;

  if (ioctl(s->fd, SNDCTL_DSP_SETFRAGMENT, &tmp) < 0) {
    d->disable_set_latency = 1;
  }
}

static int
oss_conf_test(cubeb_stream * s, init_data * d)
{
  int ret;
  unsigned int oss_params[3] = { s->channels, d->format, d->rate };

  if ((ret = oss_start(&(s->fd))) != CUBEB_OK) {
    return ret;
  }

  if (!d->disable_set_latency) {
    oss_set_latency(s, d);
  }

  if (ioctl(s->fd, SNDCTL_DSP_CHANNELS, &oss_params[0]) < 0 ||
      ioctl(s->fd, SNDCTL_DSP_SETFMT,   &oss_params[1]) < 0 ||
      ioctl(s->fd, SNDCTL_DSP_SPEED,    &oss_params[2]) < 0) {
    ret = CUBEB_ERROR;
    goto conf_failed;
  } else if (oss_params[0] != s->channels || oss_params[1] != d->format ||
             oss_params[2] != d->rate) {
    ret = CUBEB_ERROR_INVALID_FORMAT;
    goto conf_failed;
  }

  return CUBEB_OK;

conf_failed:
  close(s->fd);
  return ret;
}

static int
oss_set_trigger_level(const cubeb_stream * s, init_data * d)
{
#ifdef SNDCTL_DSP_LOW_WATER
  d->trigger_level *= d->rate * s->bpf / 1000;
  if (ioctl(s->fd, SNDCTL_DSP_LOW_WATER, &d->trigger_level) < 0) {
    return CUBEB_ERROR;
  }
#endif

  return CUBEB_OK;
}

static int
oss_conf(cubeb_stream * s, init_data * d)
{
  int ret;

  if ((ret = oss_conf_test(s, d)) != CUBEB_OK) {
    return ret;
  }

  if ((ret = oss_set_trigger_level(s, d)) != CUBEB_OK) {
    return ret;
  }

  return CUBEB_OK;
}

static int
oss_calc_trigger_level(cubeb_stream * s, init_data * d)
{
  audio_buf_info bi;

  if (ioctl(s->fd, SNDCTL_DSP_GETOSPACE, &bi) >= 0) {
    d->trigger_level = bi.fragsize * bi.fragstotal * 1000 / (d->rate * s->bpf);
    if (d->trigger_level >=  d->total_latency) {
      d->buf_latency = d->total_latency / NBUFS;
      d->trigger_level -= d->total_latency - d->buf_latency;
    } else if (d->trigger_level >= NBUFS * MIN_LATENCY / (NBUFS - 1)) {
      d->trigger_level /= NBUFS;
      d->buf_latency = d->trigger_level;
    } else {
      d->disable_set_latency = 1;
      return CUBEB_ERROR;
    }
  } else {
    return CUBEB_ERROR;
  }

  return CUBEB_OK;
}

static int
register_stream(cubeb * ctx, cubeb_stream * s)
{
  int i;

  pthread_mutex_lock(&ctx->mutex);
  for (i = 0; i < CUBEB_STREAM_MAX; ++i) {
    if (!ctx->streams[i]) {
      ctx->streams[i] = s;
      ctx->active_streams += 1;
      break;
    }
  }
  pthread_mutex_unlock(&ctx->mutex);

  return i == CUBEB_STREAM_MAX;
}

static int
oss_stream_init(cubeb * ctx, cubeb_stream ** stm, char const * stream_name,
                cubeb_stream_params stream_params, unsigned int latency,
                cubeb_data_callback data_cb, cubeb_state_callback state_cb,
                void * user_ptr)
{
  cubeb_stream * s;
  init_data d;
  int ret;

  if (latency < MIN_LATENCY) {
    return CUBEB_ERROR_INVALID_PARAMETER;
  }

  if ((s = calloc(1, sizeof(cubeb_stream))) == NULL) {
    return CUBEB_ERROR;
  }

  s->context = ctx;
  s->channels = stream_params.channels;
  s->bpf = s->channels * 2;

  switch (stream_params.format) {
  case CUBEB_SAMPLE_S16LE:
    d.format = AFMT_S16_LE;
    break;
  case CUBEB_SAMPLE_S16BE:
    d.format = AFMT_S16_BE;
    break;
  case CUBEB_SAMPLE_FLOAT32NE:
    d.format = AFMT_S16_NE;
    s->conv = 1;
    break;
  default:
    ret = CUBEB_ERROR_INVALID_FORMAT;
    goto stream_init_failed;
  }

  d.disable_set_latency = 0;
  d.rate = stream_params.rate;
  d.total_latency = NBUFS * latency / (NBUFS - 1);

  if ((ret = oss_conf_test(s, &d)) != CUBEB_OK) {
    goto stream_init_failed;
  }

  if ((ret = oss_calc_trigger_level(s, &d)) != CUBEB_OK) {
    if (d.disable_set_latency) {
      /* Retry without SNDCTL_DSP_SETFRAGMENT */
      close(s->fd);

      if ((ret = oss_conf_test(s, &d)) != CUBEB_OK) {
        goto stream_init_failed;
      }

      if ((ret = oss_calc_trigger_level(s, &d)) != CUBEB_OK) {
        goto stream_init_failed2;
      }
    } else {
      goto stream_init_failed2;
    }
  }

  close(s->fd);

  if ((ret = oss_conf(s, &d)) != CUBEB_OK) {
    goto stream_init_failed;
  }

  s->nfr = d.rate * d.buf_latency / 1000;
  s->data_callback = data_cb;
  s->state_callback = state_cb;
  s->arg = user_ptr;
  s->state = INACTIVE;

  if (s->conv == 1) {
    s->size = s->nfr * s->channels * sizeof(float);
  } else {
    s->size = s->nfr * s->bpf;
  }

  if ((ret = register_stream(ctx, s)) != 0) {
    goto stream_init_failed2;
  }

  ret = pthread_mutex_init(&s->mutex, NULL);
  assert(ret == 0);

  *stm = s;
  (void)stream_name;

  return CUBEB_OK;

stream_init_failed2:
  close(s->fd);
stream_init_failed:
  free(s);
  return ret;
}

static void
unregister_stream(cubeb_stream * s)
{
  int i;
  cubeb * ctx;

  ctx = s->context;

  pthread_mutex_lock(&ctx->mutex);
  for (i = 0; i < CUBEB_STREAM_MAX; ++i) {
    if (ctx->streams[i] == s) {
      ctx->streams[i] = NULL;
      ctx->active_streams -= 1;
      break;
    }
  }
  assert(i < CUBEB_STREAM_MAX);
  assert(ctx->active_streams >= 0);
  pthread_mutex_unlock(&ctx->mutex);
}

static void
oss_stream_destroy(cubeb_stream * s)
{
  int ret;

  assert(s && (s->state == INACTIVE || s->state == DRAINED || s->state == ERROR));

  unregister_stream(s);

  ret = pthread_mutex_destroy(&s->mutex);
  assert(ret == 0);

  ret = close(s->fd);
  assert(ret == 0 || errno != EBADF);

  free(s);
}

static int
oss_stream_start(cubeb_stream * s)
{
  cubeb * ctx = s->context;

  if (s->state != INACTIVE) {
    return CUBEB_ERROR;
  }

  pthread_mutex_lock(&ctx->mutex);
  set_stream_state(s, STARTED);
  pthread_mutex_unlock(&ctx->mutex);

  return CUBEB_OK;
}

static int
oss_stream_stop(cubeb_stream * s)
{
  int ret;
  cubeb * ctx;

  ctx = s->context;

  pthread_mutex_lock(&ctx->mutex);

  if (s->state != RUNNING && s->state != PROCESSING) {
    pthread_mutex_unlock(&ctx->mutex);
    return CUBEB_ERROR;
  }

  if (s->state != PROCESSING) {
    set_stream_state(s, INACTIVE);
  } else {
    set_stream_state(s, STOPPING);
    while (s->state == STOPPING) {
      ret = pthread_cond_wait(&ctx->cond, &ctx->mutex);
      assert(ret == 0);
    }

    if (s->state != INACTIVE) {
      pthread_mutex_unlock(&ctx->mutex);
      return CUBEB_ERROR;
    }
  }

  pthread_mutex_unlock(&ctx->mutex);

#ifdef SNDCTL_DSP_HALT
  if (ioctl(s->fd, SNDCTL_DSP_HALT, NULL) < 0) {
#else
  if (ioctl(s->fd, SNDCTL_DSP_RESET, NULL) < 0) {
#endif
    return CUBEB_ERROR;
  }

  return CUBEB_OK;
}

static int
check_value(const unsigned long call, const int value)
{
  int fd, tmp;

  tmp = value;

  if (oss_start(&fd) == CUBEB_OK) {
    if (ioctl(fd, call, &tmp) < 0) {
      tmp = value;
    }

    close(fd);
  }

  return tmp;
}

static int
oss_get_max_channel_count(cubeb * ctx, uint32_t * max_channels)
{
  *max_channels = check_value(SNDCTL_DSP_CHANNELS, 8);

  (void)ctx;

  return CUBEB_OK;
}

static int
oss_get_preferred_sample_rate(cubeb * ctx, uint32_t * rate)
{
  *rate = check_value(SNDCTL_DSP_SPEED, 48000);

  (void)ctx;

  return CUBEB_OK;
}

static int
oss_get_min_latency(cubeb * ctx, cubeb_stream_params params, uint32_t * latency_ms)
{
  /* depends on max_intrate */
  *latency_ms = MIN_LATENCY;

  (void)ctx;
  (void)params;

  return CUBEB_OK;
}

static int
oss_latency(cubeb_stream * s, uint32_t * latency)
{
  if (s->state == RUNNING || s->state == PROCESSING) {
    if (ioctl(s->fd, SNDCTL_DSP_GETODELAY, latency) >= 0) {
      *latency /= s->bpf;
      *latency = (*latency > s->pos) ? s->pos : *latency;
    } else {
      return CUBEB_ERROR;
    }
  } else {
    *latency = 0;
  }

  return CUBEB_OK;
}

static int
oss_stream_get_position(cubeb_stream * s, uint64_t * p)
{
  int ret;
  uint32_t latency;

  pthread_mutex_lock(&s->mutex);
  if ((ret = oss_latency(s, &latency)) == CUBEB_OK) {
    *p = s->pos - latency;
  }
  pthread_mutex_unlock(&s->mutex);

  return ret;
}

static int
oss_stream_get_latency(cubeb_stream * s, uint32_t * latency)
{
  int ret;

  pthread_mutex_lock(&s->mutex);
  ret = oss_latency(s, latency);
  pthread_mutex_unlock(&s->mutex);

  return ret;
}

static struct cubeb_ops const oss_ops = {
  .init = oss_init,
  .get_backend_id = oss_get_backend_id,
  .get_max_channel_count = oss_get_max_channel_count,
  .get_min_latency = oss_get_min_latency,
  .get_preferred_sample_rate = oss_get_preferred_sample_rate,
  .destroy = oss_destroy,
  .stream_init = oss_stream_init,
  .stream_destroy = oss_stream_destroy,
  .stream_start = oss_stream_start,
  .stream_stop = oss_stream_stop,
  .stream_get_position = oss_stream_get_position,
  .stream_get_latency = oss_stream_get_latency
};
