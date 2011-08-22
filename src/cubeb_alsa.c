/*
 * Copyright © 2011 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#undef NDEBUG
#include <linux/limits.h>
#include <alsa/asoundlib.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include "cubeb/cubeb.h"

static pthread_mutex_t cubeb_alsa_lock = PTHREAD_MUTEX_INITIALIZER;

#define CUBEB_MSG_SHUTDOWN   0
#define CUBEB_MSG_ADD_STREAM 1
#define CUBEB_MSG_DEL_STREAM 2

struct cubeb_msg {
  int type;
  void * data;
};

struct cubeb_list_item {
  struct cubeb_list_item * next;
  void * data;
};

struct cubeb {
  pthread_t thread;
  struct cubeb_list_item * active_streams;
  int control_fd_read;
  int control_fd_write;
};

#define CUBEB_STREAM_STATE_INACTIVE     0
#define CUBEB_STREAM_STATE_DEACTIVATING 1
#define CUBEB_STREAM_STATE_ACTIVE       2
#define CUBEB_STREAM_STATE_ACTIVATING   3

struct cubeb_stream {
  cubeb * context;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  snd_pcm_t * pcm;
  struct pollfd * pfds;
  int npfds;
  cubeb_data_callback data_callback;
  cubeb_state_callback state_callback;
  void * user_ptr;
  snd_pcm_uframes_t write_position;
  snd_pcm_uframes_t last_position;
  cubeb_stream_params params;
  int state;
  int draining;
  size_t buffer_size;
};

static int
cubeb_safe_read(int fd, void * buf, size_t count)
{
  unsigned char * p = buf;

  while (count) {
    ssize_t val = read(fd, p, count);
    if (val <= 0) {
      return -1;
    }
    p += val;
    count -= val;
  }

  return 0;
}

static int
cubeb_safe_write(int fd, void * buf, size_t count)
{
  unsigned char * p = buf;

  while (count) {
    ssize_t val = write(fd, p, count);
    if (val <= 0) {
      return -1;
    }
    p += val;
    count -= val;
  }

  return 0;
}

static void
cubeb_recv_msg(cubeb * ctx, struct cubeb_msg * msg)
{
  int r;

  r = cubeb_safe_read(ctx->control_fd_read, msg, sizeof(*msg));
  assert(r == 0);
}

static void
cubeb_send_msg(cubeb * ctx, struct cubeb_msg * msg)
{
  int r;

  r = cubeb_safe_write(ctx->control_fd_write, msg, sizeof(*msg));
  assert(r == 0);
}

static void
cubeb_list_append(struct cubeb_list_item ** head, cubeb_stream * stm)
{
  struct cubeb_list_item * list;
  struct cubeb_list_item * item;

  item = calloc(1, sizeof(*item));
  assert(item);

  item->data = stm;

  list = *head;
  while (list) {
    if (!list->next) {
      list->next = item;
      break;
    }
    list = list->next;
  }

  if (!*head) {
    *head = item;
  }
}

static void
cubeb_list_remove(struct cubeb_list_item ** head, cubeb_stream * stm)
{
  struct cubeb_list_item * list;
  struct cubeb_list_item * item = NULL;

  list = *head;

  if (list->data == stm) {
    *head = list->next;
    free(list);
    return;
  }

  while (list) {
    if (list->next && list->next->data == stm) {
      item = list->next;
      break;
    }
    list = list->next;
  }

  if (item) {
    list->next = item->next;
    free(item);
    return;
  }

  assert(0);
}

static void
rebuild_pfds(cubeb * ctx, struct pollfd ** pfds, size_t * pfds_count)
{
  struct cubeb_list_item * item;
  struct pollfd * fds;
  size_t nfds = 1;

  item = ctx->active_streams;

  while (item) {
    cubeb_stream * stm = item->data;
    nfds += stm->npfds;
    item = item->next;
  }

  free(*pfds);
  *pfds = NULL;

  fds = calloc(nfds, sizeof(*fds));
  assert(fds);

  *pfds = fds;
  *pfds_count = nfds;

  fds[0].fd = ctx->control_fd_read;
  fds[0].events = POLLIN;

  fds += 1;

  item = ctx->active_streams;
  while (item) {
    cubeb_stream * stm = item->data;
    memcpy(fds, stm->pfds, stm->npfds * sizeof(*stm->pfds));
    fds += stm->npfds;
    item = item->next;
  }
}

static void *
cubeb_run_thread(void * context)
{
  cubeb * ctx = context;
  struct pollfd * pfds = NULL;
  size_t pfds_count;

  rebuild_pfds(ctx, &pfds, &pfds_count);

  for (;;) {
    int r;
    struct cubeb_list_item * item;
    struct pollfd * tmppfds;
    struct cubeb_msg msg;
    cubeb_stream * stm;

    do {
      r = poll(pfds, pfds_count, -1);
    } while (r == -1 && errno == EINTR);
    assert(r > 0);

    /* XXX must handle ctrl messages last to maintain stream list to pfd mapping */
    /* XXX plus rebuild loses revents state anyway */

    item = ctx->active_streams;
    tmppfds = pfds + 1;
    while (item) {
      stm = item->data;
      unsigned short revents;

      r = snd_pcm_poll_descriptors_revents(stm->pcm, tmppfds, stm->npfds, &revents);
      assert(r >= 0);

      if (revents & POLLERR) {
        /* XXX deal with this properly */
        fprintf(stderr, "%p: in error\n", stm);
      }

      if (revents & POLLOUT) {
        long got;
        snd_pcm_sframes_t avail = snd_pcm_avail_update(stm->pcm);
        if (avail == -EPIPE) {
          fprintf(stderr, "%p: recovering %d\n", stm, avail);
          snd_pcm_recover(stm->pcm, avail, 1);
          avail = snd_pcm_avail_update(stm->pcm);
        }
        void * p = calloc(1, snd_pcm_frames_to_bytes(stm->pcm, avail));
        assert(p);
        got = stm->data_callback(stm, stm->user_ptr, p, avail);
        if (got < 0) {
          assert(0); /* XXX handle this case */
        }
        if (got > 0) {
          snd_pcm_sframes_t wrote = snd_pcm_writei(stm->pcm, p, got);
          fprintf(stderr, "%p: refill, wants %d, got %ld, wrote %d\n", stm, avail, got, wrote);
          stm->write_position += wrote;
        }
        if (got != avail) {
          int i;
          struct timeval tv[3];
          struct cubeb_msg msg;
          snd_pcm_state_t state = snd_pcm_state(stm->pcm);
          gettimeofday(&tv[0], NULL);
          fprintf(stderr, "%p: about to drain state=%d\n", stm, state);
          //r = snd_pcm_drain(stm->pcm);
          //assert(r == 0 || r == -EAGAIN);
          gettimeofday(&tv[1], NULL);
          tv[2].tv_sec = tv[1].tv_sec - tv[0].tv_sec;
          tv[2].tv_usec = tv[1].tv_usec - tv[0].tv_usec;
          fprintf(stderr, "%p: %f about to drain (2) state=%d\n", stm, tv[2].tv_sec * 1000.0 + tv[2].tv_usec / 1000.0, snd_pcm_state(stm->pcm));

          /* XXX only fire this once */
          stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_DRAINED);

#if 1
          /* XXX can't rebuild pfds until we've finished processing the current list */
          stm->state = CUBEB_STREAM_STATE_DEACTIVATING;

          msg.type = CUBEB_MSG_DEL_STREAM;
          msg.data = stm;
          cubeb_send_msg(stm->context, &msg);
#else
          /* disable fds for poll */
          /* XXX this will be undone upon next rebuild, so need to flag this somehow */
          for (i = 0; i < stm->npfds; ++i) {
            tmppfds[i].fd = -1;
          }
#endif
        }
        free(p);
      }

      tmppfds += stm->npfds;
      item = item->next;
    }

    if (pfds[0].revents & POLLIN) {
      cubeb_recv_msg(ctx, &msg);

      switch (msg.type) {
      case CUBEB_MSG_SHUTDOWN:
        fprintf(stderr, "shutdown\n");
        goto shutdown;
      case CUBEB_MSG_ADD_STREAM:
        stm = msg.data;
        fprintf(stderr, "add stm %p\n", stm);
        cubeb_list_append(&ctx->active_streams, stm);
        rebuild_pfds(ctx, &pfds, &pfds_count);
        pthread_mutex_lock(&stm->lock);
        assert(stm->state == CUBEB_STREAM_STATE_ACTIVATING);
        stm->state = CUBEB_STREAM_STATE_ACTIVE;
        pthread_cond_broadcast(&stm->cond);
        pthread_mutex_unlock(&stm->lock);
        break;
      case CUBEB_MSG_DEL_STREAM:
        stm = msg.data;
        fprintf(stderr, "del stm %p\n", stm);
        cubeb_list_remove(&ctx->active_streams, stm);
        rebuild_pfds(ctx, &pfds, &pfds_count);
        pthread_mutex_lock(&stm->lock);
        assert(stm->state == CUBEB_STREAM_STATE_DEACTIVATING);
        stm->state = CUBEB_STREAM_STATE_INACTIVE;
        pthread_cond_broadcast(&stm->cond);
        pthread_mutex_unlock(&stm->lock);
        break;
      }
    }
  }

shutdown:
  free(pfds);

  return NULL;
}

static int
cubeb_locked_pcm_open(snd_pcm_t ** pcm, snd_pcm_stream_t stream)
{
  int r;

  pthread_mutex_lock(&cubeb_alsa_lock);
  r = snd_pcm_open(pcm, "default", stream, SND_PCM_NONBLOCK);
  pthread_mutex_unlock(&cubeb_alsa_lock);

  return r;
}

static int
cubeb_locked_pcm_close(snd_pcm_t * pcm)
{
  int r;

  pthread_mutex_lock(&cubeb_alsa_lock);
  r = snd_pcm_close(pcm);
  pthread_mutex_unlock(&cubeb_alsa_lock);

  return r;
}

int
cubeb_init(cubeb ** context, char const * context_name)
{
  cubeb * ctx;
  int r;
  int pipe_fd[2];

  assert(sizeof(struct cubeb_msg) <= PIPE_BUF);

  ctx = calloc(1, sizeof(*ctx));
  assert(ctx);

  r = pipe(pipe_fd);
  assert(r == 0);

  ctx->control_fd_read = pipe_fd[0];
  ctx->control_fd_write = pipe_fd[1];

  /* XXX set stack size to minimum */
  r = pthread_create(&ctx->thread, NULL, cubeb_run_thread, ctx);
  assert(r == 0);

  *context = ctx;

  return CUBEB_OK;
}

void
cubeb_destroy(cubeb * ctx)
{
  struct cubeb_msg msg;
  int r;

  msg.type = CUBEB_MSG_SHUTDOWN;
  msg.data = NULL;
  cubeb_send_msg(ctx, &msg);

  r = pthread_join(ctx->thread, NULL);
  assert(r == 0);

  close(ctx->control_fd_read);
  close(ctx->control_fd_write);

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
  ssize_t bytes_per_frame;

  snd_pcm_hw_params_t * hwparams;
  snd_pcm_uframes_t period_size;
  snd_pcm_uframes_t buffer_size;
  snd_pcm_uframes_t period_time;
  snd_pcm_uframes_t buffer_time;
  int dir;

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

  r = pthread_mutex_init(&stm->lock, NULL);
  assert(r == 0);

  r = pthread_cond_init(&stm->cond, NULL);
  assert(r == 0);

  r = cubeb_locked_pcm_open(&stm->pcm, SND_PCM_STREAM_PLAYBACK);
  assert(r == 0);

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

  bytes_per_frame = snd_pcm_frames_to_bytes(stm->pcm, 1);
  /* XXX fix latency */
  stm->buffer_size = stm->params.rate / 1000.0 * latency * bytes_per_frame;
  if (stm->buffer_size % bytes_per_frame != 0) {
    stm->buffer_size += bytes_per_frame - (stm->buffer_size % bytes_per_frame);
  }
  buffer_time = stm->buffer_size;
  period_time = buffer_time / 4;
  assert(stm->buffer_size % bytes_per_frame == 0);
  assert(period_time % bytes_per_frame == 0);

#if 0
  /* set buffer sizes */

  /* XXX query min/max, limit setup based on that */
  /* XXX check number of periods vs buffer size */

  snd_pcm_hw_params_malloc(&hwparams);

  r = snd_pcm_hw_params_current(stm->pcm, hwparams);
  assert(r == 0);

  r = snd_pcm_hw_params_set_period_size_near(stm->pcm, hwparams, &period_time, &dir);
  assert(r >= 0);

  r = snd_pcm_hw_params_set_buffer_size_near(stm->pcm, hwparams, &buffer_time);
  assert(r >= 0);

  r = snd_pcm_hw_params(stm->pcm, hwparams);
  assert(r >= 0);

  snd_pcm_hw_params_get_period_size(hwparams, &period_size, 0);
  snd_pcm_hw_params_get_buffer_size(hwparams, &buffer_size);

  snd_pcm_hw_params_free(hwparams);
#endif

  /* set up poll infrastructure */

  stm->npfds = snd_pcm_poll_descriptors_count(stm->pcm);
  assert(stm->npfds > 0);

  stm->pfds = calloc(stm->npfds, sizeof(*stm->pfds));
  assert(stm->pfds);

  r = snd_pcm_poll_descriptors(stm->pcm, stm->pfds, stm->npfds);
  assert(r == stm->npfds);

  r = snd_pcm_pause(stm->pcm, 1);
//  assert(r == 0);

  stm->state = CUBEB_STREAM_STATE_INACTIVE;

  fprintf(stderr, "%p: created pcm %p (state: %d)\n", stm, stm->pcm, snd_pcm_state(stm->pcm));

  *stream = stm;

  return CUBEB_OK;
}

void
cubeb_stream_destroy(cubeb_stream * stm)
{
  int r;

  fprintf(stderr, "%p: destroying pcm %p (state: %d)\n", stm, stm->pcm, snd_pcm_state(stm->pcm));

  if (stm->pcm) {
    cubeb_locked_pcm_close(stm->pcm);
  }

  pthread_cond_destroy(&stm->cond);
  pthread_mutex_destroy(&stm->lock);

  free(stm->pfds);

  free(stm);
}

int
cubeb_stream_start(cubeb_stream * stm)
{
  int r;
  struct cubeb_msg msg;

  fprintf(stderr, "%p: start pcm %p (state: %d)\n", stm, stm->pcm, snd_pcm_state(stm->pcm));

  pthread_mutex_lock(&stm->lock);
  if (stm->state == CUBEB_STREAM_STATE_ACTIVE || stm->state == CUBEB_STREAM_STATE_ACTIVATING) {
    pthread_mutex_unlock(&stm->lock);
    return CUBEB_OK; /* XXX perhaps this should signal an error */
  }

  r = snd_pcm_pause(stm->pcm, 0);
//  assert(r == 0);

  if (stm->state != CUBEB_STREAM_STATE_ACTIVATING) {
    stm->state = CUBEB_STREAM_STATE_ACTIVATING;

    msg.type = CUBEB_MSG_ADD_STREAM;
    msg.data = stm;
    cubeb_send_msg(stm->context, &msg);
  }

  while (stm->state == CUBEB_STREAM_STATE_ACTIVATING) {
    pthread_cond_wait(&stm->cond, &stm->lock);
  }

  pthread_mutex_unlock(&stm->lock);

  return CUBEB_OK;
}

int
cubeb_stream_stop(cubeb_stream * stm)
{
  int r;
  struct cubeb_msg msg;

  fprintf(stderr, "%p: pause pcm %p (state: %d)\n", stm, stm->pcm, snd_pcm_state(stm->pcm));

  pthread_mutex_lock(&stm->lock);

  if (stm->state == CUBEB_STREAM_STATE_INACTIVE) {
    pthread_mutex_unlock(&stm->lock);
    return CUBEB_OK; /* XXX perhaps this should signal an error */
  }

  r = snd_pcm_pause(stm->pcm, 1);
//  assert(r == 0);

  if (stm->state != CUBEB_STREAM_STATE_DEACTIVATING) {
    stm->state = CUBEB_STREAM_STATE_DEACTIVATING;

    msg.type = CUBEB_MSG_DEL_STREAM;
    msg.data = stm;
    cubeb_send_msg(stm->context, &msg);
  }

  while (stm->state == CUBEB_STREAM_STATE_DEACTIVATING) {
    pthread_cond_wait(&stm->cond, &stm->lock);
  }

  pthread_mutex_unlock(&stm->lock);

  return CUBEB_OK;
}

int
cubeb_stream_get_position(cubeb_stream * stm, uint64_t * position)
{
  snd_pcm_sframes_t delay;

  *position = stm->last_position;

  if (snd_pcm_state(stm->pcm) != SND_PCM_STATE_RUNNING) {
    return CUBEB_OK;
  }

  if (snd_pcm_delay(stm->pcm, &delay) != 0) {
    return CUBEB_OK;
  }

  assert(delay >= 0);

  *position = 0;
  if (stm->write_position >= (snd_pcm_uframes_t) delay) {
    *position = stm->write_position - delay;
  }

  stm->last_position = *position;

  return CUBEB_OK;
}

