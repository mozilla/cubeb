/*
 * Copyright © 2014 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#if defined(HAVE_SYS_SOUNDCARD_H)
#include <sys/soundcard.h>
#else
#include <soundcard.h>
#endif
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <assert.h>

#include "cubeb/cubeb.h"
#include "cubeb-internal.h"

#define OSS_BUFFER_SIZE 1024

struct cubeb {
  struct cubeb_ops const * ops;
};

struct cubeb_stream {
  cubeb * context;

  cubeb_data_callback data_callback;
  cubeb_state_callback state_callback;
  void * user_ptr;
  float volume;
  float panning;

  pthread_mutex_t state_mutex;
  pthread_cond_t state_cond;

  int running;
  int stopped;
  int floating;

  /* These two vars are needed to support old versions of OSS */
  unsigned int position_bytes;
  unsigned int last_position_bytes;

  uint64_t written_frags; /* The number of fragments written to /dev/dsp */
  uint64_t missed_frags; /* fragments output with stopped stream */

  cubeb_stream_params params;
  int fd;
  pthread_t th;
};

static struct cubeb_ops const oss_ops;

int oss_init(cubeb ** context, char const * context_name)
{
  cubeb* ctx = (cubeb*)malloc(sizeof(cubeb));
  ctx->ops = &oss_ops;
  *context = ctx;
  return CUBEB_OK;
}

static void oss_destroy(cubeb *ctx)
{
  free(ctx);
}

static char const * oss_get_backend_id(cubeb * context)
{
  static char oss_name[] = "oss";
  return oss_name;
}

static int oss_get_max_channel_count(cubeb * ctx, uint32_t * max_channels)
{
  *max_channels = 2; /* Let's support only stereo for now */
  return CUBEB_OK;
}

static int oss_get_min_latency(cubeb * context, cubeb_stream_params params,
                               uint32_t * latency_frames)
{
  (void)context;
  /* 40ms is a big enough number to work ok */
  *latency_frames = 40 * params.rate / 1000;
  return CUBEB_OK;
}

static int oss_get_preferred_sample_rate(cubeb *context, uint32_t * rate)
{
  /* 48000 seems a prefered choice for most audio devices
   * and a good choice for OSS */
  *rate = 48000;
  return CUBEB_OK;
}

static void run_state_callback(cubeb_stream *stream, cubeb_state state)
{
  if (stream->state_callback) {
    stream->state_callback(stream, stream->user_ptr, state);
  }
}

static long run_data_callback(cubeb_stream *stream, void *buffer, long nframes)
{
  long got = 0;
  pthread_mutex_lock(&stream->state_mutex);
  if (stream->data_callback && stream->running && !stream->stopped) {
    pthread_mutex_unlock(&stream->state_mutex);
    got = stream->data_callback(stream, stream->user_ptr, NULL, buffer, nframes);
  } else {
    pthread_mutex_unlock(&stream->state_mutex);
  }
  return got;
}

static void apply_volume_int(int16_t* buffer, unsigned int n,
                             float volume, float panning)
{
  float left = volume;
  float right = volume;
  unsigned int i;
  int pan[2];
  if (panning<0) {
    right *= (1+panning);
  } else {
    left *= (1-panning);
  }
  pan[0] = 128.0*left;
  pan[1] = 128.0*right;
  for(i=0; i<n; i++){
    buffer[i] = ((int)buffer[i])*pan[i%2]/128;
  }
}

static void apply_volume_float(float* buffer, unsigned int n,
                               float volume, float panning)
{
  float left = volume;
  float right = volume;
  unsigned int i;
  float pan[2];
  if (panning<0) {
    right *= (1+panning);
  } else {
    left *= (1-panning);
  }
  pan[0] = left;
  pan[1] = right;
  for(i=0; i<n; i++){
    buffer[i] = buffer[i]*pan[i%2];
  }
}


static void *writer(void *stm)
{
  cubeb_stream* stream = (cubeb_stream*)stm;
  int16_t buffer[OSS_BUFFER_SIZE];
  float f_buffer[OSS_BUFFER_SIZE];
  int got;
  unsigned long i;
  while (stream->running) {
    pthread_mutex_lock(&stream->state_mutex);
    if (stream->stopped) {
      pthread_mutex_unlock(&stream->state_mutex);
      run_state_callback(stream, CUBEB_STATE_STOPPED);
      pthread_mutex_lock(&stream->state_mutex);
      while (stream->stopped) {
        pthread_cond_wait(&stream->state_cond, &stream->state_mutex);
      }
      pthread_mutex_unlock(&stream->state_mutex);
      run_state_callback(stream, CUBEB_STATE_STARTED);
      continue;
    }
    pthread_mutex_unlock(&stream->state_mutex);
    if (stream->floating) {
      got = run_data_callback(stream, f_buffer,
                              OSS_BUFFER_SIZE/stream->params.channels);
      apply_volume_float(f_buffer, got*stream->params.channels,
                                   stream->volume, stream->panning);
      for (i=0; i<((unsigned long)got)*stream->params.channels; i++) {
        /* Clipping is prefered to overflow */
	if(f_buffer[i]>=1.0){
	  f_buffer[i]=1.0;
	}
        if(f_buffer[i]<=-1.0){
	  f_buffer[i]=-1.0;
	}
        /* One might think that multipling by 32767.0 is logical but results in clipping */
        buffer[i] = f_buffer[i]*32767.0;
      }
    } else {
      got = run_data_callback(stream, buffer,
                              OSS_BUFFER_SIZE/stream->params.channels);
      apply_volume_int(buffer, got*stream->params.channels,
                               stream->volume, stream->panning);
    }
    if (got<0) {
      run_state_callback(stream, CUBEB_STATE_ERROR);
      break;
    }
    if (!got) {
      run_state_callback(stream, CUBEB_STATE_DRAINED);
    }
    if (got) {
      size_t i = 0;
      size_t s = got*stream->params.channels*sizeof(int16_t);
      while (i < s) {
        ssize_t n = write(stream->fd, ((char*)buffer) + i, s - i);
        if (n<=0) {
          run_state_callback(stream, CUBEB_STATE_ERROR);
          break;
        }
        i+=n;
      }
      stream->written_frags+=got;
    }
  }
  return NULL;
}

static void oss_try_set_latency(cubeb_stream* stream, unsigned int latency)
{
  unsigned int latency_bytes, n_frag;
  int frag;
  /* fragment size of 1024 is a good choice with good chances to be accepted */
  unsigned int frag_log=10; /* 2^frag_log = fragment size */
  latency_bytes =
    latency*stream->params.rate*stream->params.channels*sizeof(uint16_t)/1000;
  n_frag = latency_bytes>>frag_log;
  frag = (n_frag<<16) | frag_log;
  /* Even if this fails we wish to continue, not checking for errors */
  ioctl(stream->fd, SNDCTL_DSP_SETFRAGMENT, &frag);
}

static int oss_stream_init(cubeb * context, cubeb_stream ** stm,
                           char const * stream_name,
                           cubeb_devid input_device,
                           cubeb_stream_params * input_stream_params,
                           cubeb_devid output_device,
                           cubeb_stream_params * output_stream_params,
                           unsigned int latency,
                           cubeb_data_callback data_callback,
                           cubeb_state_callback state_callback, void * user_ptr)
{
  cubeb_stream* stream = (cubeb_stream*)malloc(sizeof(cubeb_stream));
  stream->context = context;
  stream->data_callback = data_callback;
  stream->state_callback = state_callback;
  stream->user_ptr = user_ptr;

  assert(!input_stream_params && "not supported.");
  if (input_device || output_device) {
    /* Device selection not yet implemented. */
    return CUBEB_ERROR_DEVICE_UNAVAILABLE;
  }

  if (((stream->fd = open("/dev/dsp", O_WRONLY)) == -1) &&
      ((stream->fd = open("/dev/sound", O_WRONLY)) == -1)) {
    free(stream);
    return CUBEB_ERROR;
  }
#define SET(what, to) do { unsigned int i = to; \
    int j = ioctl(stream->fd, what, &i); \
    if (j == -1 || i != to) { \
      close(stream->fd); \
      free(stream); \
      return CUBEB_ERROR_INVALID_FORMAT; } } while (0)

  stream->params = *output_stream_params;
  stream->volume = 1.0;
  stream->panning = 0.0;

  oss_try_set_latency(stream, latency); 

  stream->floating = 0;
  SET(SNDCTL_DSP_CHANNELS, stream->params.channels);
  SET(SNDCTL_DSP_SPEED, stream->params.rate);
  switch (stream->params.format) {
    case CUBEB_SAMPLE_S16LE:
      SET(SNDCTL_DSP_SETFMT, AFMT_S16_LE);
    break;
    case CUBEB_SAMPLE_S16BE:
      SET(SNDCTL_DSP_SETFMT, AFMT_S16_BE);
    break;
    case CUBEB_SAMPLE_FLOAT32LE:
      SET(SNDCTL_DSP_SETFMT, AFMT_S16_NE);
      stream->floating = 1;
    break;
    default:
      close(stream->fd);
      free(stream);
      return CUBEB_ERROR;
  }


  pthread_mutex_init(&stream->state_mutex, NULL);
  pthread_cond_init(&stream->state_cond, NULL);

  stream->running = 1;
  stream->stopped = 1;
  stream->position_bytes = 0;
  stream->last_position_bytes = 0;
  stream->written_frags = 0;
  stream->missed_frags = 0;

  pthread_create(&stream->th, NULL, writer, (void*)stream);

  *stm = stream;

  return CUBEB_OK;
}

static void oss_stream_destroy(cubeb_stream * stream)
{
  pthread_mutex_lock(&stream->state_mutex);

  stream->running = 0;
  stream->stopped = 0;
  pthread_cond_signal(&stream->state_cond);

  pthread_mutex_unlock(&stream->state_mutex);

  pthread_join(stream->th, NULL);

  pthread_mutex_destroy(&stream->state_mutex);
  pthread_cond_destroy(&stream->state_cond);
  close(stream->fd);
  free(stream);
}

static int oss_stream_get_latency(cubeb_stream * stream, uint32_t * latency)
{
  if (ioctl(stream->fd, SNDCTL_DSP_GETODELAY, latency)==-1) {
    return CUBEB_ERROR;
  }
  /* Convert latency from bytes to frames */
  *latency /= stream->params.channels*sizeof(int16_t);
  return CUBEB_OK;
}


static int oss_stream_current_optr(cubeb_stream * stream, uint64_t * position)
{
  count_info ci;
  /* Unfortunately, this ioctl is only available in OSS 4.x */
#ifdef SNDCTL_DSP_CURRENT_OPTR
  oss_count_t count;
  if (ioctl(stream->fd, SNDCTL_DSP_CURRENT_OPTR, &count) != -1) {
    *position = count.samples;// + count.fifo_samples;
    return CUBEB_OK;
  }
#endif
  /* Fall back to this ioctl in case the previous one fails */
  if (ioctl(stream->fd, SNDCTL_DSP_GETOPTR, &ci) == -1) {
    return CUBEB_ERROR;
  }
  /* ci.bytes is only 32 bit and will start to wrap after arithmetic overflow */
  stream->position_bytes += ci.bytes - stream->last_position_bytes;
  stream->last_position_bytes = ci.bytes;
  *position = stream->position_bytes/stream->params.channels/sizeof(int16_t);
  return CUBEB_OK;
}

static int oss_stream_get_position(cubeb_stream * stream, uint64_t * position)
{
  if ( oss_stream_current_optr(stream, position) == CUBEB_OK ){
    *position -= stream->missed_frags;
    return CUBEB_OK;
  }
  /* If no correct method to get position works we resort to this */
  *position = stream->written_frags;
  return CUBEB_OK;
}


static int oss_stream_start(cubeb_stream * stream)
{
  pthread_mutex_lock(&stream->state_mutex);
  if (stream->stopped) {
    uint64_t ptr;
    oss_stream_current_optr(stream, &ptr);
    stream->missed_frags = ptr - stream->written_frags;
    stream->stopped = 0;
    pthread_cond_signal(&stream->state_cond);
  }
  pthread_mutex_unlock(&stream->state_mutex);
  return CUBEB_OK;
}

static int oss_stream_stop(cubeb_stream * stream)
{
  pthread_mutex_lock(&stream->state_mutex);
  stream->stopped = 1;
  pthread_mutex_unlock(&stream->state_mutex);
  return CUBEB_OK;
}

int oss_stream_set_panning(cubeb_stream * stream, float panning)
{
  if (stream->params.channels == 2) {
    stream->panning=panning;
  }
  return CUBEB_OK;
}

int oss_stream_set_volume(cubeb_stream * stream, float volume)
{
  stream->volume=volume;
  return CUBEB_OK;
}

static struct cubeb_ops const oss_ops = {
  .init = oss_init,
  .get_backend_id = oss_get_backend_id,
  .get_max_channel_count = oss_get_max_channel_count,
  .get_min_latency = oss_get_min_latency,
  .get_preferred_sample_rate = oss_get_preferred_sample_rate,
  .enumerate_devices = NULL,
  .device_collection_destroy = NULL,
  .destroy = oss_destroy,
  .stream_init = oss_stream_init,
  .stream_destroy = oss_stream_destroy,
  .stream_start = oss_stream_start,
  .stream_stop = oss_stream_stop,
  .stream_reset_default_device = NULL,
  .stream_get_position = oss_stream_get_position,
  .stream_get_latency = oss_stream_get_latency,
  .stream_set_volume = oss_stream_set_volume,
  .stream_set_panning = oss_stream_set_panning,
  .stream_get_current_device = NULL,
  .stream_device_destroy = NULL,
  .stream_register_device_changed_callback = NULL,
  .register_device_collection_changed = NULL
};
