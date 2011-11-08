/*
 * Copyright © 2011 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#undef NDEBUG
#define _POSIX_SOURCE 1
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include "cubeb/cubeb.h"

/* ALSA is not thread-safe.  snd_pcm_t instances are individually protected
   by the owning cubeb_stream's mutex.  snd_pcm_t creation and destruction
   is not thread-safe until ALSA 1.0.24 (see alsa-lib.git commit 91c9c8f1),
   so those calls must be wrapped in the following global mutex. */
static pthread_mutex_t cubeb_alsa_mutex = PTHREAD_MUTEX_INITIALIZER;

#define CUBEB_MSG_TYPE_SHUTDOWN   0
#define CUBEB_MSG_TYPE_ADD_STREAM 1
#define CUBEB_MSG_TYPE_DEL_STREAM 2

struct cubeb_msg {
  int type;
  void * data;
};

struct cubeb_list_item {
  struct cubeb_list_item * next;
  struct cubeb_list_item ** prev;
  void * data;
};

struct cubeb {
  pthread_t thread;
  struct cubeb_list_item * active_streams;
  struct pollfd * descriptors;
  size_t n_descriptors;
  int control_fd_read;
  int control_fd_write;
};

#define CUBEB_STREAM_STATE_INACTIVE     0
#define CUBEB_STREAM_STATE_DEACTIVATING 1
#define CUBEB_STREAM_STATE_ACTIVE       2
#define CUBEB_STREAM_STATE_ACTIVATING   3

struct cubeb_stream {
  cubeb * context;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  snd_pcm_t * pcm;
  struct pollfd * descriptors;
  int n_descriptors;
  struct cubeb_list_item * key;
  cubeb_data_callback data_callback;
  cubeb_state_callback state_callback;
  void * user_ptr;
  snd_pcm_uframes_t write_position;
  snd_pcm_uframes_t last_position;
  cubeb_stream_params params;
  int state;
};

static void
cubeb_recv_msg(cubeb * ctx, struct cubeb_msg * msg)
{
  ssize_t r;

  r = read(ctx->control_fd_read, msg, sizeof(*msg));
  assert(r == sizeof(*msg));
}

static void
cubeb_send_msg(cubeb * ctx, struct cubeb_msg * msg)
{
  ssize_t r;

  r = write(ctx->control_fd_write, msg, sizeof(*msg));
  assert(r == sizeof(*msg));
}

static void
rebuild_pfds(cubeb * ctx)
{
  struct cubeb_list_item * item;
  struct pollfd * p;

  free(ctx->descriptors);
  ctx->n_descriptors = 0;

  item = ctx->active_streams;
  while (item) {
    cubeb_stream * stm = item->data;
    ctx->n_descriptors += stm->n_descriptors;
    item = item->next;
  }

  /* last descriptor is reserved for the control connection. */
  ctx->n_descriptors += 1;

  ctx->descriptors = calloc(ctx->n_descriptors, sizeof(*ctx->descriptors));
  assert(ctx->descriptors);

  p = ctx->descriptors;
  item = ctx->active_streams;
  while (item) {
    cubeb_stream * stm = item->data;
    memcpy(p, stm->descriptors, stm->n_descriptors * sizeof(*stm->descriptors));
    p += stm->n_descriptors;
    item = item->next;
  }

  /* initialize last descriptor with control connection details. */
  p->fd = ctx->control_fd_read;
  p->events = POLLIN;
}

static void
cubeb_register_active_stream(cubeb * ctx, cubeb_stream * stm)
{
  struct cubeb_list_item * item;

  item = calloc(1, sizeof(*item));
  assert(item);
  item->data = stm;
  stm->key = item;

  item->next = ctx->active_streams;
  if (item->next) {
    ctx->active_streams->prev = &item->next;
  }
  ctx->active_streams = item;
  item->prev = &ctx->active_streams;

  rebuild_pfds(ctx);
}

static void
cubeb_unregister_active_stream(cubeb * ctx, cubeb_stream * stm)
{
  struct cubeb_list_item * item = stm->key;

  if (item->next) {
    item->next->prev = item->prev;
  }
  *item->prev = item->next;

  free(item);
  stm->key = NULL;

  rebuild_pfds(ctx);
}

static void
cubeb_process_stream(cubeb_stream * stm)
{
  long got;
  snd_pcm_sframes_t avail;
  void * p;

  avail = snd_pcm_avail_update(stm->pcm);
  if (avail == -EPIPE) {
    snd_pcm_recover(stm->pcm, avail, 1);
    avail = snd_pcm_avail_update(stm->pcm);
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
    struct cubeb_msg msg;
#if 0
    snd_pcm_state_t state = snd_pcm_state(stm->pcm);
    r = snd_pcm_drain(stm->pcm);
    assert(r == 0 || r == -EAGAIN);
#endif

    /* XXX write out a period of data to ensure real data is flushed to speakers */

    /* XXX only fire this once */
    stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_DRAINED);

#if 1
    /* XXX can't rebuild pfds until we've finished processing the current list */
    stm->state = CUBEB_STREAM_STATE_DEACTIVATING;

    msg.type = CUBEB_MSG_TYPE_DEL_STREAM;
    msg.data = stm;
    cubeb_send_msg(stm->context, &msg);
#else
    /* disable fds for poll */
    /* XXX this will be undone upon next rebuild, so need to flag this somehow */
    for (i = 0; i < stm->n_descriptors; ++i) {
      tmppfds[i].fd = -1;
    }
#endif
  }
  free(p);
}

static void *
cubeb_run_thread(void * context)
{
  cubeb * ctx = context;

  for (;;) {
    int r;
    struct cubeb_list_item * item;
    struct pollfd * tmppfds;
    struct cubeb_msg msg;
    cubeb_stream * stm;

    do {
      r = poll(ctx->descriptors, ctx->n_descriptors, -1);
    } while (r == -1 && errno == EINTR);
    assert(r > 0);

    /* XXX must handle ctrl messages last to maintain stream list to pfd mapping */
    /* XXX plus rebuild loses revents state anyway */

    item = ctx->active_streams;
    tmppfds = ctx->descriptors;
    while (item) {
      unsigned short revents;
      stm = item->data;

      r = snd_pcm_poll_descriptors_revents(stm->pcm, tmppfds, stm->n_descriptors, &revents);
      assert(r >= 0);

      if (revents & POLLERR) {
        /* XXX deal with this properly */
        assert(0);
      }

      if (revents & POLLOUT) {
        cubeb_process_stream(stm);
      }

      tmppfds += stm->n_descriptors;
      item = item->next;
    }

    if (tmppfds->revents & POLLIN) {
      cubeb_recv_msg(ctx, &msg);

      switch (msg.type) {
      case CUBEB_MSG_TYPE_SHUTDOWN:
        return NULL;
      case CUBEB_MSG_TYPE_ADD_STREAM:
        stm = msg.data;
        cubeb_register_active_stream(ctx, stm);
        pthread_mutex_lock(&stm->mutex);
        assert(stm->state == CUBEB_STREAM_STATE_ACTIVATING);
        stm->state = CUBEB_STREAM_STATE_ACTIVE;
        pthread_cond_broadcast(&stm->cond);
        pthread_mutex_unlock(&stm->mutex);
        break;
      case CUBEB_MSG_TYPE_DEL_STREAM:
        stm = msg.data;
        cubeb_unregister_active_stream(ctx, stm);
        pthread_mutex_lock(&stm->mutex);
        assert(stm->state == CUBEB_STREAM_STATE_DEACTIVATING);
        stm->state = CUBEB_STREAM_STATE_INACTIVE;
        pthread_cond_broadcast(&stm->cond);
        pthread_mutex_unlock(&stm->mutex);
        break;
      }
    }
  }

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
  int pipe_fd[2];
  pthread_attr_t attr;

  assert(context);

  assert(sizeof(struct cubeb_msg) <= PIPE_BUF);

  ctx = calloc(1, sizeof(*ctx));
  assert(ctx);

  r = pipe(pipe_fd);
  assert(r == 0);

  ctx->control_fd_read = pipe_fd[0];
  ctx->control_fd_write = pipe_fd[1];

  rebuild_pfds(ctx);

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
  struct cubeb_msg msg;
  int r;

  assert(ctx);

  msg.type = CUBEB_MSG_TYPE_SHUTDOWN;
  msg.data = NULL;
  cubeb_send_msg(ctx, &msg);

  r = pthread_join(ctx->thread, NULL);
  assert(r == 0);

  close(ctx->control_fd_read);
  close(ctx->control_fd_write);

  free(ctx->descriptors);
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

  r = pthread_cond_init(&stm->cond, NULL);
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
    /* XXX: return format error if necessary */
    cubeb_stream_destroy(stm);
    return CUBEB_ERROR;
  }

  /* set up poll infrastructure */

  stm->n_descriptors = snd_pcm_poll_descriptors_count(stm->pcm);
  assert(stm->n_descriptors > 0);

  stm->descriptors = calloc(stm->n_descriptors, sizeof(*stm->descriptors));
  assert(stm->descriptors);

  r = snd_pcm_poll_descriptors(stm->pcm, stm->descriptors, stm->n_descriptors);
  assert(r == stm->n_descriptors);

  stm->state = CUBEB_STREAM_STATE_INACTIVE;

  *stream = stm;

  return CUBEB_OK;
}

void
cubeb_stream_destroy(cubeb_stream * stm)
{
  assert(stm);
  assert(stm->state == CUBEB_STREAM_STATE_INACTIVE);

  if (stm->pcm) {
    cubeb_locked_pcm_close(stm->pcm);
  }

  pthread_cond_destroy(&stm->cond);
  pthread_mutex_destroy(&stm->mutex);

  free(stm->descriptors);

  free(stm);
}

int
cubeb_stream_start(cubeb_stream * stm)
{
  struct cubeb_msg msg;

  assert(stm);

  pthread_mutex_lock(&stm->mutex);

  /* XXX check how this is used in refill loop */
  if (stm->state == CUBEB_STREAM_STATE_ACTIVE || stm->state == CUBEB_STREAM_STATE_ACTIVATING) {
    pthread_mutex_unlock(&stm->mutex);
    return CUBEB_OK; /* XXX perhaps this should signal an error */
  }

  snd_pcm_pause(stm->pcm, 0);

  if (stm->state != CUBEB_STREAM_STATE_ACTIVATING) {
    stm->state = CUBEB_STREAM_STATE_ACTIVATING;

    msg.type = CUBEB_MSG_TYPE_ADD_STREAM;
    msg.data = stm;
    cubeb_send_msg(stm->context, &msg);
  }

  while (stm->state == CUBEB_STREAM_STATE_ACTIVATING) {
    pthread_cond_wait(&stm->cond, &stm->mutex);
  }

  pthread_mutex_unlock(&stm->mutex);

  return CUBEB_OK;
}

int
cubeb_stream_stop(cubeb_stream * stm)
{
  struct cubeb_msg msg;

  assert(stm);

  pthread_mutex_lock(&stm->mutex);

  if (stm->state == CUBEB_STREAM_STATE_INACTIVE) {
    pthread_mutex_unlock(&stm->mutex);
    return CUBEB_OK; /* XXX perhaps this should signal an error */
  }

  snd_pcm_pause(stm->pcm, 1);

  if (stm->state != CUBEB_STREAM_STATE_DEACTIVATING) {
    stm->state = CUBEB_STREAM_STATE_DEACTIVATING;

    msg.type = CUBEB_MSG_TYPE_DEL_STREAM;
    msg.data = stm;
    cubeb_send_msg(stm->context, &msg);
  }

  while (stm->state == CUBEB_STREAM_STATE_DEACTIVATING) {
    pthread_cond_wait(&stm->cond, &stm->mutex);
  }

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
