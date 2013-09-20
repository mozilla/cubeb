/*
 * Copyright © 2012 David Richards
 * Copyright © 2013 Sebastien Alaiwan
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

#define _BSD_SOURCE
#define _POSIX_SOURCE
#include <algorithm>
#include <limits>
#include <stdio.h>
#include <pthread.h>
#include <sys/time.h>
#include <assert.h>
#include <string.h>
#include <limits.h>
#include <poll.h>
#include <unistd.h>
#include <stdlib.h>
#include "cubeb/cubeb.h"
#include "cubeb-internal.h"

#include "cubeb-speex-resampler.h"

#include <jack/jack.h>
#include <jack/ringbuffer.h>

static const int MAX_STREAMS = 16;
static const int MAX_CHANNELS  = 8;
static const int FIFO_SIZE = 4096 * sizeof(float);
static const bool AUTO_CONNECT_JACK_PORTS = true;

static void
s16ne_to_float(float *dst, const int16_t *src, size_t n)
{
  for(size_t i=0;i < n;i++)
    *(dst++) = *(src++) / 32767.0f;
}

typedef enum {
  STATE_INACTIVE,
  STATE_STARTING,
  STATE_STARTED,
  STATE_RUNNING,
  STATE_DRAINING,
  STATE_STOPPING,
  STATE_STOPPED,
  STATE_DRAINED,
} play_state;

static bool
is_running(play_state state)
{
  return state == STATE_STARTING
    || state == STATE_STARTED
    || state == STATE_DRAINING
    || state == STATE_RUNNING;
}

struct AutoLock
{
  AutoLock(pthread_mutex_t& m) : mutex(m)
  {
    pthread_mutex_lock(&mutex);
  }

  ~AutoLock()
  {
    pthread_mutex_unlock(&mutex);
  }

private:
  pthread_mutex_t& mutex;
};

extern "C"
{
/*static*/ int jack_init (cubeb ** context, char const * context_name);
}
static char const * cbjack_get_backend_id(cubeb * context);
static int cbjack_get_max_channel_count(cubeb * ctx, uint32_t * max_channels);
static void cbjack_destroy(cubeb * context);
static int cbjack_stream_init(cubeb * context, cubeb_stream ** stream, char const * stream_name,
                  cubeb_stream_params stream_params, unsigned int latency,
                  cubeb_data_callback data_callback,
                  cubeb_state_callback state_callback,
                  void * user_ptr);
static void cbjack_stream_destroy(cubeb_stream * stream);
static int cbjack_stream_start(cubeb_stream * stream);
static int cbjack_stream_stop(cubeb_stream * stream);
static int cbjack_stream_get_position(cubeb_stream * stream, uint64_t * position);
static int cbjack_stream_get_latency(cubeb_stream * stream, uint32_t * latency);

static struct cubeb_ops const cbjack_ops = {
  .init = jack_init,
  .get_backend_id = cbjack_get_backend_id,
  .get_max_channel_count = cbjack_get_max_channel_count,
  .destroy = cbjack_destroy,
  .stream_init = cbjack_stream_init,
  .stream_destroy = cbjack_stream_destroy,
  .stream_start = cbjack_stream_start,
  .stream_stop = cbjack_stream_stop,
  .stream_get_position = cbjack_stream_get_position,
  .stream_get_latency = cbjack_stream_get_latency
};

struct cubeb_stream {
  cubeb *context;

  bool in_use; /**< Set to false iff the stream is free */
  bool ports_ready; /**< Set to true iff the JACK ports are ready */

  cubeb_data_callback data_callback;
  cubeb_state_callback state_callback;
  void *user_ptr;
  cubeb_stream_params params;

  SpeexResamplerState *resampler;

  play_state state;
  uint64_t position;
  char stream_name[256];
  jack_port_t *output_ports[MAX_CHANNELS];
  jack_ringbuffer_t *ringbuffer[MAX_CHANNELS];
};

struct cubeb {
  struct cubeb_ops const * ops;

  /**< Mutex for stream array */
  pthread_mutex_t mutex;

  /**< Raw input buffer, as filled by data_callback */
  char input_buffer[FIFO_SIZE * MAX_CHANNELS];

  /**< Audio buffer, converted to float */
  float float_interleaved_buffer[FIFO_SIZE * MAX_CHANNELS];

  /**< Audio buffer, at the sampling rate of the output */
  float resampled_interleaved_buffer[FIFO_SIZE * MAX_CHANNELS * 3];

  /**< Non-interleaved audio buffer, used to push to the fifo */
  float buffer[MAX_CHANNELS][FIFO_SIZE];

  cubeb_stream streams[MAX_STREAMS];
  pthread_t stream_refill_thread;
  unsigned int active_streams;

  bool active;
  unsigned int jack_sample_rate;
  jack_client_t *jack_client;
};

static void
cbjack_connect_ports (cubeb_stream * stream)
{
  const char **physical_ports = jack_get_ports (stream->context->jack_client,
      NULL, NULL, JackPortIsInput | JackPortIsPhysical);
  if (physical_ports == NULL) {
    return;
  }

  // Connect to all physical ports
  for (unsigned int c = 0; c < stream->params.channels && physical_ports[c]; c++) {
    const char *src_port = jack_port_name (stream->output_ports[c]);

    jack_connect (stream->context->jack_client, src_port, physical_ports[c]);
  }
  jack_free(physical_ports);
}

static jack_nframes_t
cbjack_stream_data_ready(cubeb_stream * stream)
{
  jack_nframes_t max_num_frames = std::numeric_limits<jack_nframes_t>::max();
  for(unsigned int c = 0; c < stream->params.channels; c++) {
    size_t read_space = jack_ringbuffer_read_space(stream->ringbuffer[c]);
    jack_nframes_t nframes = read_space / sizeof(float);
    max_num_frames = std::min(nframes, max_num_frames);
  }
  return max_num_frames;
}

static unsigned int
cbjack_stream_data_space(cubeb_stream * stream)
{
  unsigned int avail = UINT_MAX;

  for(unsigned int c = 0; c < stream->params.channels; c++) {
    size_t write_space = jack_ringbuffer_write_space(stream->ringbuffer[c]);
    avail = std::min<unsigned int>(avail, write_space);
  }

  return avail;
}

static void
cbjack_stream_data_out(cubeb_stream * stream, jack_nframes_t nframes)
{
  for(unsigned int c = 0; c < stream->params.channels; c++) {
    float* samples = (float*)jack_port_get_buffer(stream->output_ports[c], nframes);
    size_t needed_bytes = nframes * sizeof(float);
    size_t nread = jack_ringbuffer_read(stream->ringbuffer[c], (char *)samples, needed_bytes);
    assert(nread == needed_bytes);
  }

  stream->position += nframes;
}

static void
cbjack_stream_silence_out(cubeb_stream * stream, jack_nframes_t nframes)
{
  for(unsigned int c = 0; c < stream->params.channels; c++) {
    float *samples = (float*)jack_port_get_buffer(stream->output_ports[c], nframes);
    for(jack_nframes_t s = 0; s < nframes; s++) {
      samples[s] = 0.0f;
    }
  }
}

static int
cbjack_process(jack_nframes_t nframes, void *arg)
{
  cubeb *ctx = (cubeb *)arg;

  for(int s = 0; s < MAX_STREAMS; s++) {
    cubeb_stream *stm = &ctx->streams[s];
    if (!stm->in_use)
      continue;
    if (!stm->ports_ready)
      continue;

    if (is_running(stm->state)) {
      jack_nframes_t const max_read_frames = cbjack_stream_data_ready(stm);
      jack_nframes_t const frames = std::min(nframes, max_read_frames);
      cbjack_stream_data_out(stm, frames);

      // occurs when draining
      if(frames < nframes)
        cbjack_stream_silence_out(stm, nframes-frames);
    } else {
      cbjack_stream_silence_out(stm, nframes);
    }

    if (stm->state == STATE_STARTING) {
      stm->state = STATE_STARTED;
    }

    if (stm->state == STATE_STOPPING) {
      stm->state = STATE_STOPPED;
    }

    if (stm->state == STATE_DRAINING && cbjack_stream_data_ready(stm) == 0) {
      stm->state = STATE_DRAINED;
    }
  }

  return 0;
}

static void
cbjack_stream_refill(cubeb_stream * stream)
{
  unsigned int max_bytes = cbjack_stream_data_space(stream);
  long const max_num_output_frames = max_bytes / sizeof(float); // we're outputing floats
  long const max_num_frames = (max_num_output_frames * stream->params.rate) / stream->context->jack_sample_rate;

  long num_frames = stream->data_callback(stream,
      stream->user_ptr,
      stream->context->input_buffer,
      max_num_frames);

  // check for drain
  if(num_frames < max_num_frames)
    stream->state = STATE_DRAINING;

  float *interleaved_buffer;

  // convert 16-bit to float if needed
  if(stream->params.format == CUBEB_SAMPLE_S16NE) {
    s16ne_to_float(stream->context->float_interleaved_buffer, (short*)stream->context->input_buffer, num_frames * stream->params.channels);
    interleaved_buffer = stream->context->float_interleaved_buffer;
  }
  else if(stream->params.format == CUBEB_SAMPLE_FLOAT32NE) {
    interleaved_buffer = (float *)stream->context->input_buffer;
  }
  else {
    assert(0); // should not occur, checked by cbjack_stream_init
  }

  if (stream->resampler != NULL) {
    uint32_t resampler_consumed_frames = num_frames;
    uint32_t resampler_output_frames = (FIFO_SIZE / sizeof(float)) * MAX_CHANNELS * 3;

    int resampler_error = speex_resampler_process_interleaved_float(stream->resampler,
        interleaved_buffer,
        &resampler_consumed_frames,
        stream->context->resampled_interleaved_buffer,
        &resampler_output_frames);
    assert(resampler_error == 0);
    assert(resampler_consumed_frames == num_frames);
    num_frames = resampler_output_frames;
    interleaved_buffer = stream->context->resampled_interleaved_buffer;
  }

  // convert interleaved buffers to contigous buffers
  for(unsigned int c = 0; c < stream->params.channels; c++) {
    float* buffer = stream->context->buffer[c];
    for (long f = 0; f < num_frames; f++) {
      buffer[f] = interleaved_buffer[(f * stream->params.channels) + c];
    }
  }

  // send contigous buffers to ring buffers
  for(unsigned int c = 0; c < stream->params.channels; c++) {
    size_t bytes_to_write = num_frames * sizeof(float);
    char* buffer = (char*)stream->context->buffer[c];
    while(bytes_to_write > 0) {
      size_t nwritten = jack_ringbuffer_write(stream->ringbuffer[c], buffer, bytes_to_write);
      bytes_to_write -= nwritten;
      buffer += nwritten;
      if(nwritten == 0) {
        assert(bytes_to_write == 0);
        break;
      }
    }
  }
}

static void *
stream_refill_thread (void *arg)
{
  cubeb *ctx = (cubeb *)arg;

  while (ctx->active) {
    for(int s = 0; s < MAX_STREAMS; s++) {
      AutoLock lock(ctx->mutex);
      cubeb_stream* stream = &ctx->streams[s];
      if (!stream->in_use)
        continue;

      if (is_running(stream->state)) {
        if (stream->state != STATE_DRAINING) {
          cbjack_stream_refill(stream);
        }
      }

      if (stream->state == STATE_STARTED) {
        stream->state = STATE_RUNNING;
        stream->state_callback(stream, stream->user_ptr, CUBEB_STATE_STARTED);
      }

      if (stream->state == STATE_STOPPED) {
        stream->state = STATE_INACTIVE;
        stream->state_callback(stream, stream->user_ptr, CUBEB_STATE_STOPPED);
      }

      if (stream->state == STATE_DRAINED) {
        stream->state = STATE_INACTIVE;
        stream->state_callback(stream, stream->user_ptr, CUBEB_STATE_DRAINED);
      }
    }
    usleep (10000);
  }

  return NULL;
}

/*static*/ int
jack_init (cubeb ** context, char const * context_name)
{
  int r;

  if(context == NULL) {
    return CUBEB_ERROR;
  }

  *context = NULL;

  cubeb *ctx = (cubeb*)calloc(1, sizeof(*ctx));
  if(ctx == NULL) {
    return CUBEB_ERROR;
  }

  r = pthread_mutex_init(&ctx->mutex, NULL);
  if(r != 0) {
    cbjack_destroy(ctx);
    return CUBEB_ERROR;
  }

  ctx->ops = &cbjack_ops;

  const char* jack_client_name = "cubeb";
  if(context_name)
    jack_client_name = context_name;

  ctx->jack_client = jack_client_open (jack_client_name,
                                       JackNoStartServer,
                                       NULL);

  if (ctx->jack_client == NULL) {
    cbjack_destroy(ctx);
    return CUBEB_ERROR;
  }

  ctx->jack_sample_rate = jack_get_sample_rate(ctx->jack_client);

  jack_set_process_callback (ctx->jack_client, cbjack_process, ctx);

  if (jack_activate (ctx->jack_client)) {
    cbjack_destroy(ctx);
    return CUBEB_ERROR;
  }

  for(int s = 0; s < MAX_STREAMS; s++) {
    for (int c = 0; c < MAX_CHANNELS; c++) {
      ctx->streams[s].ringbuffer[c] = jack_ringbuffer_create(FIFO_SIZE);
      if(!ctx->streams[s].ringbuffer[c]) {
        cbjack_destroy(ctx);
        return CUBEB_ERROR;
      }
    }
  }

  ctx->active = true;
  int ret = pthread_create (&ctx->stream_refill_thread, NULL, stream_refill_thread, (void *)ctx);
  if(ret != 0) {
    ctx->stream_refill_thread = 0;
    cbjack_destroy(ctx);
    return CUBEB_ERROR;
  }

  *context = ctx;

  return CUBEB_OK;
}

static char const *
cbjack_get_backend_id(cubeb * context)
{
  return "jack";
}

static int
cbjack_get_max_channel_count(cubeb * ctx, uint32_t * max_channels)
{
  *max_channels = MAX_CHANNELS;
  return CUBEB_OK;
}

static void
cbjack_destroy(cubeb * context)
{
  // stop thread if any
  if(context->stream_refill_thread) {
    context->active = false;
    pthread_join (context->stream_refill_thread, NULL);
  }

  if(context->jack_client)
    jack_client_close (context->jack_client);

  for(int s = 0; s < MAX_STREAMS; s++) {
    for (int c = 0; c < MAX_CHANNELS; c++) {
      if(context->streams[s].ringbuffer[c])
        jack_ringbuffer_free(context->streams[s].ringbuffer[c]);
    }
  }

  pthread_mutex_destroy(&context->mutex);
  free(context);
}

static int
cbjack_stream_init(cubeb * context, cubeb_stream ** stream, char const * stream_name,
                  cubeb_stream_params stream_params, unsigned int latency,
                  cubeb_data_callback data_callback,
                  cubeb_state_callback state_callback,
                  void * user_ptr)
{
  if (stream_params.format != CUBEB_SAMPLE_FLOAT32NE
      && stream_params.format != CUBEB_SAMPLE_S16NE) {
    return CUBEB_ERROR_INVALID_FORMAT;
  }

  if (stream == NULL) {
    return CUBEB_ERROR;
  }

  *stream = NULL;

  // Lock streams
  AutoLock lock(context->mutex);

  // Find a free stream.
  cubeb_stream * stm = NULL;
  int stream_number;
  for(stream_number = 0; stream_number < MAX_STREAMS; stream_number++) {
    if(!context->streams[stream_number].in_use) {
      stm = &context->streams[stream_number];
      break;
    }
  }

  // No free stream?
  if(stm == NULL) {
    return CUBEB_ERROR;
  }

  snprintf(stm->stream_name, 255, "%s_%u", stream_name, stream_number);

  stm->user_ptr = user_ptr;
  stm->context = context;
  stm->params = stream_params;
  stm->data_callback = data_callback;
  stm->state_callback = state_callback;
  stm->position = 0;

  if (stm->params.rate != stm->context->jack_sample_rate) {
    int resampler_error;
    stm->resampler = speex_resampler_init(stm->params.channels,
                                           stm->params.rate,
                                           stm->context->jack_sample_rate,
                                           10,
                                           &resampler_error);
    if (resampler_error != 0) {
      return CUBEB_ERROR;
    }
  }

  for(unsigned int c = 0; c < stm->params.channels; c++) {
    char portname[256];
    snprintf(portname, 255, "%s_%d", stm->stream_name, c);
    stm->output_ports[c] = jack_port_register(stm->context->jack_client,
                                              portname,
                                              JACK_DEFAULT_AUDIO_TYPE,
                                              JackPortIsOutput,
                                              0);
  }

  if (AUTO_CONNECT_JACK_PORTS) {
    cbjack_connect_ports(stm);
  }

  stm->in_use = true;
  stm->ports_ready = true;

  *stream = stm;

  return CUBEB_OK;
}

static void
cbjack_stream_destroy(cubeb_stream * stream)
{
  AutoLock lock(stream->context->mutex);
  stream->state = STATE_INACTIVE;
  stream->ports_ready = false;

  for(unsigned int c = 0; c < stream->params.channels; c++) {
    jack_port_unregister (stream->context->jack_client, stream->output_ports[c]);
    stream->output_ports[c] = NULL;
    jack_ringbuffer_reset(stream->ringbuffer[c]);
  }

  if (stream->resampler != NULL) {
    speex_resampler_destroy(stream->resampler);
    stream->resampler = NULL;
  }
  stream->in_use = false;
}

static int
cbjack_stream_start(cubeb_stream * stream)
{
  stream->state = STATE_STARTING;
  return CUBEB_OK;
}

static int
cbjack_stream_stop(cubeb_stream * stream)
{
  stream->state = STATE_STOPPING;
  return CUBEB_OK;
}

static int
cbjack_stream_get_position(cubeb_stream * stream, uint64_t * position)
{
  float const ratio = (float)stream->params.rate / (float)stream->context->jack_sample_rate;
  *position = stream->position * ratio;
  return CUBEB_OK;
}

static int
cbjack_stream_get_latency(cubeb_stream * stream, uint32_t * latency)
{
  *latency = 0;
  return CUBEB_ERROR;
}

