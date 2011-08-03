/*
 * Copyright © 2011 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#undef NDEBUG
#include <alsa/asoundlib.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include "cubeb/cubeb.h"

static pthread_mutex_t cubeb_alsa_lock = PTHREAD_MUTEX_INITIALIZER;

#define CUBEB_STREAM_STATE_INACTIVE 0
#define CUBEB_STREAM_STATE_ACTIVE   1
#define CUBEB_STREAM_STATE_SHUTDOWN 2

struct cubeb_stream {
  pthread_mutex_t lock;
  pthread_cond_t cond;
  pthread_t thread;
  snd_pcm_t * pcm;
  cubeb_data_callback data_callback;
  cubeb_state_callback state_callback;
  void * user_ptr;
  cubeb_stream_params params;
  snd_pcm_uframes_t last_write_position;
  snd_pcm_uframes_t last_position;
  int state;
  size_t buffer_size;
  int shutdown_fd[2];
};

static int
wait_for_poll(snd_pcm_t * pcm, struct pollfd * ufds, int count)
{
  unsigned short revents;

  for (;;) {
    poll(ufds, count, -1);
    snd_pcm_poll_descriptors_revents(pcm, ufds, count-1, &revents);
    if (revents & POLLERR)
      return -1;
    if (revents & POLLOUT)
      return 0;
    if (ufds[count-1].revents & POLLIN)
      return -1;
  }

  return -1;
}

static void *
cubeb_buffer_refill_thread(void * stream)
{
  long got, towrite;
  cubeb_stream * stm = stream;
  snd_pcm_sframes_t nframes, wrote;
  unsigned char * buffer, * p;
  int r, count;
  struct pollfd * ufds = NULL;

  count = snd_pcm_poll_descriptors_count(stm->pcm);
  assert(count > 0);
  ufds = calloc(count + 1, sizeof(*ufds));
  assert(ufds);

  ufds[count].fd = stm->shutdown_fd[0];
  ufds[count].events = POLLIN;

  r = snd_pcm_poll_descriptors(stm->pcm, ufds, count);
  assert(r == count);

  for (;;) {
    pthread_mutex_lock(&stm->lock);

    while (stm->state == CUBEB_STREAM_STATE_INACTIVE) {
      r = pthread_cond_wait(&stm->cond, &stm->lock);
      assert(r == 0);
    }

    if (stm->state == CUBEB_STREAM_STATE_SHUTDOWN) {
          pthread_mutex_unlock(&stm->lock);
          break;
    }

    if (wait_for_poll(stm->pcm, ufds, count+1) < 0) {
      pthread_mutex_unlock(&stm->lock);
      break;
    }

    nframes = stm->buffer_size;

    buffer = calloc(1, snd_pcm_frames_to_bytes(stm->pcm, nframes));
    assert(buffer);

    fprintf(stderr, "requesting %d frames\n", nframes);
    got = stm->data_callback(stm, stm->user_ptr, buffer, nframes);
    if (got < 0) {
      /* XXX handle this case */
      assert(0);
    }

    p = buffer;
    towrite = got;

    while (towrite > 0) {
      pthread_mutex_unlock(&stm->lock);
      wrote = snd_pcm_writei(stm->pcm, p, towrite);
      fprintf(stderr, "towrite=%d wrote=%d state=%d\n", towrite, wrote, snd_pcm_state(stm->pcm));
      pthread_mutex_lock(&stm->lock);
      if (wrote < 0) {
        r = snd_pcm_recover(stm->pcm, wrote, 1);
        assert(r == 0);
      } else {
        towrite -= wrote;
        p += snd_pcm_frames_to_bytes(stm->pcm, wrote);
        stm->last_write_position += wrote;
      }
      if (towrite == 0)
        break;
      if (wait_for_poll(stm->pcm, ufds, count+1) < 0) {
        pthread_mutex_unlock(&stm->lock);
        break;
      }
    }

    free(buffer);

    if ((size_t) got < stm->buffer_size) {
      snd_pcm_drain(stm->pcm);
      stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_DRAINED);
    }

    pthread_mutex_unlock(&stm->lock);
  }

  free(ufds);

  return NULL;
}

int
cubeb_init(cubeb ** context, char const * context_name)
{
  *context = (void *) 0xdeadbeef;
  return CUBEB_OK;
}

void
cubeb_destroy(cubeb * ctx)
{
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

  switch (stream_params.format) {
  case CUBEB_SAMPLE_U8:
    format = SND_PCM_FORMAT_U8;
    break;
  case CUBEB_SAMPLE_S16LE:
    format = SND_PCM_FORMAT_S16_LE;
    break;
  case CUBEB_SAMPLE_FLOAT32LE:
    format = SND_PCM_FORMAT_FLOAT_LE;
    break;
  default:
    return CUBEB_ERROR_INVALID_FORMAT;
  }

  stm = calloc(1, sizeof(*stm));
  assert(stm);

  stm->data_callback = data_callback;
  stm->state_callback = state_callback;
  stm->user_ptr = user_ptr;
  stm->params = stream_params;

  r = pthread_mutex_init(&stm->lock, NULL);
  assert(r == 0);

  r = pthread_cond_init(&stm->cond, NULL);
  assert(r == 0);

  r = pipe2(stm->shutdown_fd, O_NONBLOCK);
  assert(r == 0);

  pthread_mutex_lock(&cubeb_alsa_lock);

  r = snd_pcm_open(&stm->pcm, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
  assert(r == 0);

  pthread_mutex_unlock(&cubeb_alsa_lock);

  /* XXX fix latency */
  stm->buffer_size = latency;

  r = snd_pcm_set_params(stm->pcm, format, SND_PCM_ACCESS_RW_INTERLEAVED,
                         stm->params.channels, stm->params.rate, 1,
                         (double) latency / (double) stm->params.rate * 1e6);
  assert(r == 0);

  r = snd_pcm_pause(stm->pcm, 1);
//  assert(r == 0);

  stm->state = CUBEB_STREAM_STATE_INACTIVE;

  /* XXX set stack size to min */
  r = pthread_create(&stm->thread, NULL, cubeb_buffer_refill_thread, stm);
  assert(r == 0);

  *stream = stm;

  return CUBEB_OK;
}

void
cubeb_stream_destroy(cubeb_stream * stm)
{
  int r;

  write(stm->shutdown_fd[1], "X", 1);

  if (stm->thread) {
    pthread_mutex_lock(&stm->lock);
    stm->state = CUBEB_STREAM_STATE_SHUTDOWN;
    pthread_cond_broadcast(&stm->cond);
    pthread_mutex_unlock(&stm->lock);

    r = pthread_join(stm->thread, NULL);
    assert(r == 0);
  }

  if (stm->pcm) {
    pthread_mutex_lock(&cubeb_alsa_lock);
    snd_pcm_close(stm->pcm);
    pthread_mutex_unlock(&cubeb_alsa_lock);
  }

  close(stm->shutdown_fd[0]);
  close(stm->shutdown_fd[1]);

  pthread_cond_destroy(&stm->cond);

  pthread_mutex_destroy(&stm->lock);

  free(stm);
}

int
cubeb_stream_start(cubeb_stream * stm)
{
  int r;

  pthread_mutex_lock(&stm->lock);
  r = snd_pcm_pause(stm->pcm, 0);
//  assert(r == 0);

  stm->state = CUBEB_STREAM_STATE_ACTIVE;
  pthread_cond_broadcast(&stm->cond);
  pthread_mutex_unlock(&stm->lock);

  return CUBEB_OK;
}

int
cubeb_stream_stop(cubeb_stream * stm)
{
  int r;

  pthread_mutex_lock(&stm->lock);
  r = snd_pcm_pause(stm->pcm, 1);
//  assert(r == 0);

  stm->state = CUBEB_STREAM_STATE_INACTIVE;
  pthread_cond_broadcast(&stm->cond);
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
  if (stm->last_write_position >= (snd_pcm_uframes_t) delay) {
    *position = stm->last_write_position - delay;
  }

  stm->last_position = *position;

  return CUBEB_OK;
}

int
cubeb_stream_set_volume(cubeb_stream * stm, float volume)
{
  return CUBEB_OK;
}
