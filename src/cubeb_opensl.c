/*
 * Copyright © 2012 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#undef NDEBUG
#include <assert.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <pthread.h>
#include <SLES/OpenSLES.h>
#include <math.h>
#include <time.h>
#if defined(__ANDROID__)
#include <dlfcn.h>
#include <sys/system_properties.h>
#include "android/sles_definitions.h"
#include <SLES/OpenSLES_Android.h>
#include <android/log.h>
#include <android/api-level.h>

//#define LOGGING_ENABLED
#ifdef LOGGING_ENABLED
#define LOG(args...)  __android_log_print(ANDROID_LOG_INFO, "Cubeb_OpenSL" , ## args)
#else
#define LOG(...)
#endif

#define ANDROID_VERSION_GINGERBREAD_MR1 10
#define ANDROID_VERSION_LOLLIPOP 21
#define ANDROID_VERSION_MARSHMALLOW 23
#endif
#include "cubeb/cubeb.h"
#include "cubeb-internal.h"
#include "cubeb_resampler.h"
#include "cubeb-sles.h"
#include "cubeb_array_queue.h"

#define DEFAULT_SAMPLE_RATE 48000

static struct cubeb_ops const opensl_ops;

struct cubeb {
  struct cubeb_ops const * ops;
  void * lib;
  void * libmedia;
  int32_t (* get_output_latency)(uint32_t * latency, int stream_type);
  SLInterfaceID SL_IID_BUFFERQUEUE;
  SLInterfaceID SL_IID_PLAY;
#if defined(__ANDROID__)
  SLInterfaceID SL_IID_ANDROIDCONFIGURATION;
#endif
  SLInterfaceID SL_IID_VOLUME;
  SLObjectItf engObj;
  SLEngineItf eng;
  SLObjectItf outmixObj;
};

#define NELEMS(A) (sizeof(A) / sizeof A[0])
#define NBUFS 4
#define AUDIO_STREAM_TYPE_MUSIC 3

struct cubeb_stream {
  cubeb * context;
  pthread_mutex_t mutex;
  SLObjectItf playerObj;
  SLPlayItf play;
  SLBufferQueueItf bufq;
  SLVolumeItf volume;
  void ** queuebuf;
  uint32_t queuebuf_capacity;
  int queuebuf_idx;
  long queuebuf_len;
  long bytespersec;
  long framesize;
  long written;
  int draining;
  cubeb_stream_type stream_type;

  array_queue * output_queue;

  /* Flags to determine in/out.*/
  uint32_t input_enabled;
  uint32_t output_enabled;

  /* Recorder abstract object. */
  SLObjectItf recorderObj;
  /* Recorder Itf for input capture. */
  SLRecordItf recorderItf;
  /* Buffer queue for input capture. */
  SLAndroidSimpleBufferQueueItf recorderBufferQueueItf;
  /* Store input buffers. */
  void ** input_buffer_array;
  /* The capacity of the array.
   * On capture only can be small (4).
   * On full duplex is calculated to
   * store 1 sec of data buffers. */
  uint32_t input_array_capacity;
  /* Current filled index of input buffer array. */
  int input_buffer_index;
  /* Length of input buffer.*/
  uint32_t input_buffer_length;
  /* Input frame size */
  uint32_t input_frame_size;
  /* Device sampling rate. If user rate is not
   * accepted an compatible rate is set. If it is
   * accepted this is equal to params.rate. */
  uint32_t input_device_rate;
  /* Exchange input buffers between input
   * and full duplex threads. */
  array_queue * input_queue;
  /* Silent input buffer used on full duplex. */
  void * input_silent_buffer;
  /* Number of input frames from the start of the stream*/
  uint32_t input_total_frames;

  /* On full duplex thread responsible to call
   * user callback through resampler. */
  pthread_t full_duplex_thread;

  /* Flag to stop the execution of user callback and
   * close all working threads.*/
  uint32_t shutdown;

  /* Store user callback. */
  cubeb_data_callback data_callback;
  /* Store state callback. */
  cubeb_state_callback state_callback;
  /* User pointer for data & state callbacks*/
  void * user_ptr;

  cubeb_resampler * resampler;
  unsigned int inputrate;
  unsigned int outputrate;
  unsigned int latency;
  int64_t lastPosition;
  int64_t lastPositionTimeStamp;
  int64_t lastCompensativePosition;
};

/* Forward declaration. */
static int opensl_stop_player(cubeb_stream * stm);
static int opensl_stop_recorder(cubeb_stream * stm);

static void
play_callback(SLPlayItf caller, void * user_ptr, SLuint32 event)
{
  cubeb_stream * stm = user_ptr;
  int draining;
  assert(stm);
  switch (event) {
  case SL_PLAYEVENT_HEADATMARKER:
    pthread_mutex_lock(&stm->mutex);
    draining = stm->draining;
    pthread_mutex_unlock(&stm->mutex);
    if (draining) {
      stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_DRAINED);
      if (stm->play) {
        int r = opensl_stop_player(stm);
        assert(r == CUBEB_OK);
      }
      if (stm->recorderItf) {
        int r = opensl_stop_recorder(stm);
        assert(r == CUBEB_OK);
      }
    }
    break;
  default:
    break;
  }
}

static void
recorder_marker_callback (SLRecordItf caller, void * pContext, SLuint32 event)
{
  cubeb_stream * stm = pContext;
  assert(stm);

  if (event == SL_RECORDEVENT_HEADATMARKER) {
    pthread_mutex_lock(&stm->mutex);
    int draining = stm->draining;
    pthread_mutex_unlock(&stm->mutex);
    if (draining) {
      stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_DRAINED);
      if (stm->recorderItf) {
        int r = opensl_stop_recorder(stm);
        assert(r == CUBEB_OK);
      }
      if (stm->play) {
        int r = opensl_stop_player(stm);
        assert(r == CUBEB_OK);
      }
    }
  }
}

static void
bufferqueue_callback(SLBufferQueueItf caller, void * user_ptr)
{
  cubeb_stream * stm = user_ptr;
  assert(stm);
  SLBufferQueueState state;
  SLresult res;

  res = (*stm->bufq)->GetState(stm->bufq, &state);
  assert(res == SL_RESULT_SUCCESS);

  if (state.count > 1)
    return;

  SLuint32 i;
  for (i = state.count; i < NBUFS; i++) {
    uint8_t *buf = stm->queuebuf[stm->queuebuf_idx];
    long written = 0;
    pthread_mutex_lock(&stm->mutex);
    int draining = stm->draining;
    int shutdown = stm->shutdown;
    pthread_mutex_unlock(&stm->mutex);

    if (!draining && !shutdown) {
      written = cubeb_resampler_fill(stm->resampler,
                                     NULL, NULL,
                                     buf, stm->queuebuf_len / stm->framesize);
      if (written < 0 || written * stm->framesize > stm->queuebuf_len) {
        pthread_mutex_lock(&stm->mutex);
        stm->shutdown = 1;
        pthread_mutex_unlock(&stm->mutex);
        opensl_stop_player(stm);
        stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_ERROR);
        return;
      }
    }

    // Keep sending silent data even in draining mode to prevent the audio
    // back-end from being stopped automatically by OpenSL/ES.
    memset(buf + written * stm->framesize, 0, stm->queuebuf_len - written * stm->framesize);
    res = (*stm->bufq)->Enqueue(stm->bufq, buf, stm->queuebuf_len);
    assert(res == SL_RESULT_SUCCESS);
    stm->queuebuf_idx = (stm->queuebuf_idx + 1) % stm->queuebuf_capacity;
    if (written > 0) {
      pthread_mutex_lock(&stm->mutex);
      stm->written += written;
      pthread_mutex_unlock(&stm->mutex);
    }

    if (!draining && written * stm->framesize < stm->queuebuf_len) {
      pthread_mutex_lock(&stm->mutex);
      int64_t written_duration = INT64_C(1000) * stm->written * stm->framesize / stm->bytespersec;
      stm->draining = 1;
      pthread_mutex_unlock(&stm->mutex);
      // Use SL_PLAYEVENT_HEADATMARKER event from slPlayCallback of SLPlayItf
      // to make sure all the data has been processed.
      (*stm->play)->SetMarkerPosition(stm->play, (SLmillisecond)written_duration);
      return;
    }
  }
}

static int
opensl_enqueue_recorder(cubeb_stream * stm, void ** last_filled_buffer)
{
  assert(stm);

  int current_index = stm->input_buffer_index;
  void * last_buffer = NULL;

  if (current_index < 0) {
    // This is the first enqueue
    current_index = 0;
  } else {
    // The current index hold the last filled buffer get it before advance index.
    last_buffer = stm->input_buffer_array[current_index];
    // Advance to get next available buffer
    current_index = (current_index + 1) % stm->input_array_capacity;
  }
  // enqueue next empty buffer to be filled by the recorder
  SLresult res = (*stm->recorderBufferQueueItf)->Enqueue(stm->recorderBufferQueueItf,
                                                         stm->input_buffer_array[current_index],
                                                         stm->input_buffer_length);
  if (res != SL_RESULT_SUCCESS ) {
    LOG("Enqueue recorder failed. Error code: %lu", res);
    return CUBEB_ERROR;
  }
  // All good, update buffer and index.
  stm->input_buffer_index = current_index;
  if (last_filled_buffer) {
    *last_filled_buffer = last_buffer;
  }
  return CUBEB_OK;
}

// input data callback
void recorder_callback(SLAndroidSimpleBufferQueueItf bq, void * context)
{
  assert(context);
  cubeb_stream * stm = context;
  assert(stm->recorderBufferQueueItf);

  if (stm->shutdown || stm->draining) {
    // Accordint to the doc, on transition to the SL_RECORDSTATE_STOPPED state,
    // the application should continue to enqueue buffers onto the queue
    // to retrieve the residual recorded data in the system.
    int r = opensl_enqueue_recorder(stm, NULL);
    assert(r == CUBEB_OK);
    return;
  }

  // Enqueue next available buffer and get the last filled buffer.
  void * input_buffer = NULL;
  int r = opensl_enqueue_recorder(stm, &input_buffer);
  assert(r == CUBEB_OK);
  assert(input_buffer);
  // Fill resampler with last input
  long input_frame_count = stm->input_buffer_length / stm->input_frame_size;
  long got = cubeb_resampler_fill(stm->resampler,
                                  input_buffer,
                                  &input_frame_count,
                                  NULL,
                                  0);
  // Error case
  if (got < 0 || got > input_frame_count) {
    pthread_mutex_lock(&stm->mutex);
    stm->shutdown = 1;
    pthread_mutex_unlock(&stm->mutex);
    int r = opensl_stop_recorder(stm);
    assert(r == CUBEB_OK);
    stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_ERROR);
  }

  // Advance total stream frames
  stm->input_total_frames += got;

  if (got < input_frame_count) {
    pthread_mutex_lock(&stm->mutex);
    stm->draining = 1;
    pthread_mutex_unlock(&stm->mutex);
    int64_t duration = INT64_C(1000) * stm->input_total_frames / stm->input_device_rate;
    (*stm->recorderItf)->SetMarkerPosition(stm->recorderItf, (SLmillisecond)duration);
    return;
  }
}

void recorder_fullduplex_callback(SLAndroidSimpleBufferQueueItf bq, void * context)
{
  assert(context);
  cubeb_stream * stm = context;
  assert(stm->recorderBufferQueueItf);

  if (stm->shutdown || stm->draining) {
    /* On draining and shutdown the recorder should have been stoped from
    *  the one set the flags. Accordint to the doc, on transition to
    *  the SL_RECORDSTATE_STOPPED state, the application should
    *  continue to enqueue buffers onto the queue to retrieve the residual
    *  recorded data in the system. */
    LOG("Input shutdown %d or drain %d", stm->shutdown, stm->draining);
    int r = opensl_enqueue_recorder(stm, NULL);
    assert(r == CUBEB_OK);
    return;
  }

  // Enqueue next available buffer and get the last filled buffer.
  void * input_buffer = NULL;
  int r = opensl_enqueue_recorder(stm, &input_buffer);
  assert(r == CUBEB_OK);
  assert(input_buffer);

  assert(stm->input_queue);
  r = array_queue_push(stm->input_queue, input_buffer);
  if (r == -1) {
    LOG("Input queue is full, drop input ...");
  }
}

static void
player_fullduplex_callback(SLBufferQueueItf caller, void * user_ptr)
{
  cubeb_stream * stm = user_ptr;
  assert(stm);
  SLresult res;

  void * output_buffer = NULL;
  if (stm->shutdown ||
      stm->draining ||
      (output_buffer = array_queue_pop(stm->output_queue)) == NULL) {
    LOG("Output hole or shutdown send silent");
    output_buffer = stm->queuebuf[stm->queuebuf_idx];
    memset(output_buffer, 0, stm->queuebuf_len);
    // Advance the output buffer queue index
    stm->queuebuf_idx = (stm->queuebuf_idx + 1) % stm->queuebuf_capacity;
  }

  // Enqueue data in player buffer queue
  res = (*stm->bufq)->Enqueue(stm->bufq,
                              output_buffer,
                              stm->queuebuf_len);
  assert(res == SL_RESULT_SUCCESS);
}

void * LoopFullDuplexThread(void * p)
{
  cubeb_stream * stm = p;
  assert(stm);

  while (1) {
    pthread_mutex_lock(&stm->mutex);
    int draining = stm->draining;
    uint32_t shutdown = stm->shutdown;
    pthread_mutex_unlock(&stm->mutex);
    if (draining || shutdown){
      LOG("Exit full duplex thread");
      return NULL;
    }

    // Wait until some input exist.
    array_queue_wait_if_empty(stm->input_queue);
    void * input_buffer = array_queue_pop(stm->input_queue);
    assert(input_buffer);
    long input_frame_count = stm->input_buffer_length / stm->input_frame_size;
    long frames_needed = stm->queuebuf_len / stm->framesize;

    // Check again after waiting
    pthread_mutex_lock(&stm->mutex);
    draining = stm->draining;
    shutdown = stm->shutdown;
    pthread_mutex_unlock(&stm->mutex);
    if (draining || shutdown){
      LOG("Exit full duplex thread after wait");
      return NULL;
    }

    // Get buffer to store the output
    void * output_buffer = stm->queuebuf[stm->queuebuf_idx];
    long written = 0;

    // Trigger user callback through resampler
    written = cubeb_resampler_fill(stm->resampler,
                                   input_buffer,
                                   &input_frame_count,
                                   output_buffer,
                                   frames_needed);

    LOG("Fill: written %d, frames_needed %d, input array %d, output array %d",
        written, frames_needed, array_queue_get_size(stm->input_queue),
        array_queue_get_size(stm->output_queue));

    if (written < 0 || written  > frames_needed) {
      // Error case
      pthread_mutex_lock(&stm->mutex);
      stm->shutdown = 1;
      pthread_mutex_unlock(&stm->mutex);
      opensl_stop_player(stm);
      opensl_stop_recorder(stm);
      stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_ERROR);
      return NULL;
    }

    // Advance total out written  frames counter
    stm->written += written;

    if ( written < frames_needed) {
      pthread_mutex_lock(&stm->mutex);
      int64_t written_duration = INT64_C(1000) * stm->written * stm->framesize / stm->bytespersec;
      stm->draining = 1;
      pthread_mutex_unlock(&stm->mutex);

      // Use SL_PLAYEVENT_HEADATMARKER event from slPlayCallback of SLPlayItf
      // to make sure all the data has been processed.
      (*stm->play)->SetMarkerPosition(stm->play, (SLmillisecond)written_duration);
    }

    // Keep sending silent data even in draining mode to prevent the audio
    // back-end from being stopped automatically by OpenSL/ES.
    memset(output_buffer + written * stm->framesize, 0,
           stm->queuebuf_len - written * stm->framesize);
    // Push output buffer on output queue
    int r = array_queue_push(stm->output_queue, output_buffer);
    if (r == -1) {
      LOG("Output is full, drop it ...");
    }
    // Advance the output buffer queue index
    stm->queuebuf_idx = (stm->queuebuf_idx + 1) % stm->queuebuf_capacity;
  }
}

#if defined(__ANDROID__)
static SLuint32
convert_stream_type_to_sl_stream(cubeb_stream_type stream_type)
{
  switch(stream_type) {
  case CUBEB_STREAM_TYPE_SYSTEM:
    return SL_ANDROID_STREAM_SYSTEM;
  case CUBEB_STREAM_TYPE_MUSIC:
    return SL_ANDROID_STREAM_MEDIA;
  case CUBEB_STREAM_TYPE_NOTIFICATION:
    return SL_ANDROID_STREAM_NOTIFICATION;
  case CUBEB_STREAM_TYPE_ALARM:
    return SL_ANDROID_STREAM_ALARM;
  case CUBEB_STREAM_TYPE_VOICE_CALL:
    return SL_ANDROID_STREAM_VOICE;
  case CUBEB_STREAM_TYPE_RING:
    return SL_ANDROID_STREAM_RING;
  case CUBEB_STREAM_TYPE_SYSTEM_ENFORCED:
    return SL_ANDROID_STREAM_SYSTEM_ENFORCED;
  default:
    return 0xFFFFFFFF;
  }
}
#endif

static void opensl_destroy(cubeb * ctx);

#if defined(__ANDROID__)

// The bionic header file on B2G contains the required
// declarations on all releases.
#ifndef MOZ_WIDGET_GONK

#if (__ANDROID_API__ >= ANDROID_VERSION_LOLLIPOP)
typedef int (system_property_get)(const char*, char*);

static int
__system_property_get(const char* name, char* value)
{
  void* libc = dlopen("libc.so", RTLD_LAZY);
  if (!libc) {
    LOG("Failed to open libc.so");
    return -1;
  }
  system_property_get* func = (system_property_get*)
                              dlsym(libc, "__system_property_get");
  int ret = -1;
  if (func) {
    ret = func(name, value);
  }
  dlclose(libc);
  return ret;
}
#endif
#endif

static int
get_android_version(void)
{
  char version_string[PROP_VALUE_MAX];

  memset(version_string, 0, PROP_VALUE_MAX);

  int len = __system_property_get("ro.build.version.sdk", version_string);
  if (len <= 0) {
    LOG("Failed to get Android version!\n");
    return len;
  }

  int version = (int)strtol(version_string, NULL, 10);
  LOG("Android version %d", version);
  return version;
}
#endif

/*static*/ int
opensl_init(cubeb ** context, char const * context_name)
{
  cubeb * ctx;

#if defined(__ANDROID__)
  int android_version = get_android_version();
  if (android_version > 0 && android_version <= ANDROID_VERSION_GINGERBREAD_MR1) {
    // Don't even attempt to run on Gingerbread and lower
    return CUBEB_ERROR;
  }
#endif

  *context = NULL;

  ctx = calloc(1, sizeof(*ctx));
  assert(ctx);

  ctx->ops = &opensl_ops;

  ctx->lib = dlopen("libOpenSLES.so", RTLD_LAZY);
  ctx->libmedia = dlopen("libmedia.so", RTLD_LAZY);
  if (!ctx->lib || !ctx->libmedia) {
    free(ctx);
    return CUBEB_ERROR;
  }

  /* Get the latency, in ms, from AudioFlinger */
  /* status_t AudioSystem::getOutputLatency(uint32_t* latency,
   *                                        audio_stream_type_t streamType) */
  /* First, try the most recent signature. */
  ctx->get_output_latency =
    dlsym(ctx->libmedia, "_ZN7android11AudioSystem16getOutputLatencyEPj19audio_stream_type_t");
  if (!ctx->get_output_latency) {
    /* in case of failure, try the legacy version. */
    /* status_t AudioSystem::getOutputLatency(uint32_t* latency,
     *                                        int streamType) */
    ctx->get_output_latency =
      dlsym(ctx->libmedia, "_ZN7android11AudioSystem16getOutputLatencyEPji");
    if (!ctx->get_output_latency) {
      opensl_destroy(ctx);
      return CUBEB_ERROR;
    }
  }

  typedef SLresult (*slCreateEngine_t)(SLObjectItf *,
                                       SLuint32,
                                       const SLEngineOption *,
                                       SLuint32,
                                       const SLInterfaceID *,
                                       const SLboolean *);
  slCreateEngine_t f_slCreateEngine =
    (slCreateEngine_t)dlsym(ctx->lib, "slCreateEngine");
  SLInterfaceID SL_IID_ENGINE = *(SLInterfaceID *)dlsym(ctx->lib, "SL_IID_ENGINE");
  SLInterfaceID SL_IID_OUTPUTMIX = *(SLInterfaceID *)dlsym(ctx->lib, "SL_IID_OUTPUTMIX");
  ctx->SL_IID_VOLUME = *(SLInterfaceID *)dlsym(ctx->lib, "SL_IID_VOLUME");
  ctx->SL_IID_BUFFERQUEUE = *(SLInterfaceID *)dlsym(ctx->lib, "SL_IID_BUFFERQUEUE");
#if defined(__ANDROID__)
  ctx->SL_IID_ANDROIDCONFIGURATION = *(SLInterfaceID *)dlsym(ctx->lib, "SL_IID_ANDROIDCONFIGURATION");
#endif
  ctx->SL_IID_PLAY = *(SLInterfaceID *)dlsym(ctx->lib, "SL_IID_PLAY");
  if (!f_slCreateEngine ||
      !SL_IID_ENGINE ||
      !SL_IID_OUTPUTMIX ||
      !ctx->SL_IID_BUFFERQUEUE ||
#if defined(__ANDROID__)
      !ctx->SL_IID_ANDROIDCONFIGURATION ||
#endif
      !ctx->SL_IID_PLAY) {
    opensl_destroy(ctx);
    return CUBEB_ERROR;
  }

  const SLEngineOption opt[] = {{SL_ENGINEOPTION_THREADSAFE, SL_BOOLEAN_TRUE}};

  SLresult res;
  res = cubeb_get_sles_engine(&ctx->engObj, 1, opt, 0, NULL, NULL);

  if (res != SL_RESULT_SUCCESS) {
    opensl_destroy(ctx);
    return CUBEB_ERROR;
  }

  res = cubeb_realize_sles_engine(ctx->engObj);
  if (res != SL_RESULT_SUCCESS) {
    opensl_destroy(ctx);
    return CUBEB_ERROR;
  }

  res = (*ctx->engObj)->GetInterface(ctx->engObj, SL_IID_ENGINE, &ctx->eng);
  if (res != SL_RESULT_SUCCESS) {
    opensl_destroy(ctx);
    return CUBEB_ERROR;
  }

  const SLInterfaceID idsom[] = {SL_IID_OUTPUTMIX};
  const SLboolean reqom[] = {SL_BOOLEAN_TRUE};
  res = (*ctx->eng)->CreateOutputMix(ctx->eng, &ctx->outmixObj, 1, idsom, reqom);
  if (res != SL_RESULT_SUCCESS) {
    opensl_destroy(ctx);
    return CUBEB_ERROR;
  }

  res = (*ctx->outmixObj)->Realize(ctx->outmixObj, SL_BOOLEAN_FALSE);
  if (res != SL_RESULT_SUCCESS) {
    opensl_destroy(ctx);
    return CUBEB_ERROR;
  }

  *context = ctx;

  return CUBEB_OK;
}

static char const *
opensl_get_backend_id(cubeb * ctx)
{
  return "opensl";
}

static int
opensl_get_max_channel_count(cubeb * ctx, uint32_t * max_channels)
{
  assert(ctx && max_channels);
  /* The android mixer handles up to two channels, see
     http://androidxref.com/4.2.2_r1/xref/frameworks/av/services/audioflinger/AudioFlinger.h#67 */
  *max_channels = 2;

  return CUBEB_OK;
}

static int
opensl_get_preferred_sample_rate(cubeb * ctx, uint32_t * rate)
{
  /* https://android.googlesource.com/platform/ndk.git/+/master/docs/opensles/index.html
   * We don't want to deal with JNI here (and we don't have Java on b2g anyways),
   * so we just dlopen the library and get the two symbols we need. */
  int r;
  void * libmedia;
  uint32_t (*get_primary_output_samplingrate)();
  uint32_t (*get_output_samplingrate)(int * samplingRate, int streamType);

  libmedia = dlopen("libmedia.so", RTLD_LAZY);
  if (!libmedia) {
    return CUBEB_ERROR;
  }

  /* uint32_t AudioSystem::getPrimaryOutputSamplingRate(void) */
  get_primary_output_samplingrate =
    dlsym(libmedia, "_ZN7android11AudioSystem28getPrimaryOutputSamplingRateEv");
  if (!get_primary_output_samplingrate) {
    /* fallback to
     * status_t AudioSystem::getOutputSamplingRate(int* samplingRate, int streamType)
     * if we cannot find getPrimaryOutputSamplingRate. */
    get_output_samplingrate =
      dlsym(libmedia, "_ZN7android11AudioSystem21getOutputSamplingRateEPj19audio_stream_type_t");
    if (!get_output_samplingrate) {
      /* Another signature exists, with a int instead of an audio_stream_type_t */
      get_output_samplingrate =
        dlsym(libmedia, "_ZN7android11AudioSystem21getOutputSamplingRateEPii");
      if (!get_output_samplingrate) {
        dlclose(libmedia);
        return CUBEB_ERROR;
      }
    }
  }

  if (get_primary_output_samplingrate) {
    *rate = get_primary_output_samplingrate();
  } else {
    /* We don't really know about the type, here, so we just pass music. */
    r = get_output_samplingrate((int *) rate, AUDIO_STREAM_TYPE_MUSIC);
    if (r) {
      dlclose(libmedia);
      return CUBEB_ERROR;
    }
  }

  dlclose(libmedia);

  /* Depending on which method we called above, we can get a zero back, yet have
   * a non-error return value, especially if the audio system is not
   * ready/shutting down (i.e. when we can't get our hand on the AudioFlinger
   * thread). */
  if (*rate == 0) {
    return CUBEB_ERROR;
  }

  return CUBEB_OK;
}

static int
opensl_get_min_latency(cubeb * ctx, cubeb_stream_params params, uint32_t * latency_ms)
{
  /* https://android.googlesource.com/platform/ndk.git/+/master/docs/opensles/index.html
   * We don't want to deal with JNI here (and we don't have Java on b2g anyways),
   * so we just dlopen the library and get the two symbols we need. */

  int r;
  void * libmedia;
  size_t (*get_primary_output_frame_count)(void);
  int (*get_output_frame_count)(size_t * frameCount, int streamType);
  uint32_t primary_sampling_rate;
  size_t primary_buffer_size;

  r = opensl_get_preferred_sample_rate(ctx, &primary_sampling_rate);

  if (r) {
    return CUBEB_ERROR;
  }

  libmedia = dlopen("libmedia.so", RTLD_LAZY);
  if (!libmedia) {
    return CUBEB_ERROR;
  }

  /* JB variant */
  /* size_t AudioSystem::getPrimaryOutputFrameCount(void) */
  get_primary_output_frame_count =
    dlsym(libmedia, "_ZN7android11AudioSystem26getPrimaryOutputFrameCountEv");
  if (!get_primary_output_frame_count) {
    /* ICS variant */
    /* status_t AudioSystem::getOutputFrameCount(int* frameCount, int streamType) */
    get_output_frame_count =
      dlsym(libmedia, "_ZN7android11AudioSystem19getOutputFrameCountEPii");
    if (!get_output_frame_count) {
      dlclose(libmedia);
      return CUBEB_ERROR;
    }
  }

  if (get_primary_output_frame_count) {
    primary_buffer_size = get_primary_output_frame_count();
  } else {
    if (get_output_frame_count(&primary_buffer_size, params.stream_type) != 0) {
      return CUBEB_ERROR;
    }
  }

  /* To get a fast track in Android's mixer, we need to be at the native
   * samplerate, which is device dependant. Some devices might be able to
   * resample when playing a fast track, but it's pretty rare. */
  *latency_ms = NBUFS * primary_buffer_size / (primary_sampling_rate / 1000);

  dlclose(libmedia);

  return CUBEB_OK;
}

static void
opensl_destroy(cubeb * ctx)
{
  if (ctx->outmixObj)
    (*ctx->outmixObj)->Destroy(ctx->outmixObj);
  if (ctx->engObj)
    cubeb_destroy_sles_engine(&ctx->engObj);
  dlclose(ctx->lib);
  dlclose(ctx->libmedia);
  free(ctx);
}

static void opensl_stream_destroy(cubeb_stream * stm);

static int
opensl_set_format(SLDataFormat_PCM * format, cubeb_stream_params * params)
{
  assert(format);
  assert(params);

  format->formatType = SL_DATAFORMAT_PCM;
  format->numChannels = params->channels;
  // samplesPerSec is in milliHertz
  format->samplesPerSec = params->rate * 1000;
  format->bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_16;
  format->containerSize = SL_PCMSAMPLEFORMAT_FIXED_16;
  format->channelMask = params->channels == 1 ?
                       SL_SPEAKER_FRONT_CENTER :
                       SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT;

  switch (params->format) {
    case CUBEB_SAMPLE_S16LE:
      format->endianness = SL_BYTEORDER_LITTLEENDIAN;
          break;
    case CUBEB_SAMPLE_S16BE:
      format->endianness = SL_BYTEORDER_BIGENDIAN;
          break;
    default:
      return CUBEB_ERROR_INVALID_FORMAT;
  }
  return CUBEB_OK;
}

static int
opensl_configure_capture(cubeb_stream * stm, cubeb_stream_params * params)
{
  assert(stm);
  assert(params);

  SLDataLocator_AndroidSimpleBufferQueue lDataLocatorOut;
  lDataLocatorOut.locatorType = SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE;
  lDataLocatorOut.numBuffers = NBUFS;

  SLDataFormat_PCM lDataFormat;
  int r = opensl_set_format(&lDataFormat, params);
  if (r != CUBEB_OK) {
    return CUBEB_ERROR_INVALID_FORMAT;
  }

  /* For now set device rate to params rate. */
  stm->input_device_rate = params->rate;

  SLDataSink lDataSink;
  lDataSink.pLocator = &lDataLocatorOut;
  lDataSink.pFormat = &lDataFormat;

  SLDataLocator_IODevice lDataLocatorIn;
  lDataLocatorIn.locatorType = SL_DATALOCATOR_IODEVICE;
  lDataLocatorIn.deviceType = SL_IODEVICE_AUDIOINPUT;
  lDataLocatorIn.deviceID = SL_DEFAULTDEVICEID_AUDIOINPUT;
  lDataLocatorIn.device = NULL;

  SLDataSource lDataSource;
  lDataSource.pLocator = &lDataLocatorIn;
  lDataSource.pFormat = NULL;

  const SLuint32 lSoundRecorderIIDCount = 2;
  const SLInterfaceID lSoundRecorderIIDs[] = { SL_IID_RECORD, SL_IID_ANDROIDSIMPLEBUFFERQUEUE };
  const SLboolean lSoundRecorderReqs[] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };
  // create the audio recorder abstract object
  SLresult res = (*stm->context->eng)->CreateAudioRecorder(stm->context->eng,
                                                           &stm->recorderObj,
                                                           &lDataSource,
                                                           &lDataSink,
                                                           lSoundRecorderIIDCount,
                                                           lSoundRecorderIIDs,
                                                           lSoundRecorderReqs);
  // Sample rate not supported. Try again with default sample rate!
  if (res == SL_RESULT_CONTENT_UNSUPPORTED) {
    if (stm->outputrate != 0 && stm->output_enabled) {
      // Set the same with the player. Since there is no
      // api for input device this is a safe choice.
      stm->input_device_rate = stm->outputrate;
    } else {
      // A safe choice for Android.
      stm->input_device_rate = DEFAULT_SAMPLE_RATE;
    }
    lDataFormat.samplesPerSec = stm->input_device_rate * 1000;
    res = (*stm->context->eng)->CreateAudioRecorder(stm->context->eng,
                                                    &stm->recorderObj,
                                                    &lDataSource,
                                                    &lDataSink,
                                                    lSoundRecorderIIDCount,
                                                    lSoundRecorderIIDs,
                                                    lSoundRecorderReqs);

    if (res != SL_RESULT_SUCCESS) {
      LOG("Failed to create recorder. Error code: %lu", res);
      return CUBEB_ERROR;
    }
  }

  // realize the audio recorder
  res = (*stm->recorderObj)->Realize(stm->recorderObj, SL_BOOLEAN_FALSE);
  if (res != SL_RESULT_SUCCESS) {
    LOG("Failed to realize recorder. Error code: %lu", res);
    return CUBEB_ERROR;
  }
  // get the record interface
  res = (*stm->recorderObj)->GetInterface(stm->recorderObj, SL_IID_RECORD, &stm->recorderItf);
  if (res != SL_RESULT_SUCCESS) {
    LOG("Failed to get recorder interface. Error code: %lu", res);
    return CUBEB_ERROR;
  }

  res = (*stm->recorderItf)->RegisterCallback(stm->recorderItf, recorder_marker_callback, stm);
  if (res != SL_RESULT_SUCCESS) {
    opensl_stream_destroy(stm);
    return CUBEB_ERROR;
  }

  (*stm->recorderItf)->SetMarkerPosition(stm->recorderItf, (SLmillisecond)0);

  res = (*stm->recorderItf)->SetCallbackEventsMask(stm->recorderItf, (SLuint32)SL_RECORDEVENT_HEADATMARKER);
  if (res != SL_RESULT_SUCCESS) {
    opensl_stream_destroy(stm);
    return CUBEB_ERROR;
  }
  // get the simple android buffer queue interface
  res = (*stm->recorderObj)->GetInterface(stm->recorderObj,
                                          SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                          &stm->recorderBufferQueueItf);
  if (res != SL_RESULT_SUCCESS) {
    LOG("Failed to get recorder (android) buffer queue interface. Error code: %lu", res);
    return CUBEB_ERROR;
  }

  // register callback on record (input) buffer queue
  slAndroidSimpleBufferQueueCallback rec_callback = recorder_callback;
  if (stm->output_enabled) {
    // Register full duplex callback instead.
    rec_callback = recorder_fullduplex_callback;
  }
  res = (*stm->recorderBufferQueueItf)->RegisterCallback(stm->recorderBufferQueueItf,
                                                         rec_callback,
                                                         stm);
  if (res != SL_RESULT_SUCCESS) {
    LOG("Failed to register recorder buffer queue callback. Error code: %lu", res);
    return CUBEB_ERROR;
  }

  // Calculate length of input buffer according to requested latency
  stm->input_frame_size = params->channels * sizeof(int16_t);
  uint32_t bytes_per_sec = stm->input_device_rate * stm->input_frame_size;
  stm->input_buffer_length = (bytes_per_sec * stm->latency) / (1000 * NBUFS);
  // round up to the next multiple of stm->framesize, if needed.
  if (stm->input_buffer_length % stm->input_frame_size) {
    stm->input_buffer_length += stm->input_frame_size - (stm->input_buffer_length % stm->input_frame_size);
  }

  // Calculate the capacity of input array
  stm->input_array_capacity = NBUFS;
  if (stm->output_enabled) {
    // Full duplex, update capacity to hold 1 sec of data
    stm->input_array_capacity = 1 * stm->input_device_rate / stm->input_buffer_length;
  }
  // Allocate input array
  stm->input_buffer_array = (void**)calloc(1, sizeof(void*)*stm->input_array_capacity);
  // Buffering has not started yet.
  stm->input_buffer_index = -1;
  // Prepare input buffers
  for(int i = 0; i < stm->input_array_capacity; ++i) {
    stm->input_buffer_array[i] = calloc(1, stm->input_buffer_length);
  }

  // On full duplex allocate input queue and silent buffer
  if (stm->output_enabled) {
    stm->input_queue = array_queue_create(stm->input_array_capacity);
    assert(stm->input_queue);
    stm->input_silent_buffer = calloc(1, stm->input_buffer_length);
    assert(stm->input_silent_buffer);
  }

  // Enqueue buffer to start rolling once recorder started
  r = opensl_enqueue_recorder(stm, NULL);
  if (r != CUBEB_OK) {
    return r;
  }

  LOG("Cubeb stream init recorder success");

  return CUBEB_OK;
}

static int
opensl_configure_playback(cubeb_stream * stm, cubeb_stream_params * params) {
  assert(stm);
  assert(params);

  stm->inputrate = params->rate;
  stm->stream_type = params->stream_type;
  stm->framesize = params->channels * sizeof(int16_t);
  stm->lastPosition = -1;
  stm->lastPositionTimeStamp = 0;
  stm->lastCompensativePosition = -1;

  SLDataFormat_PCM format;
  int r = opensl_set_format(&format, params);
  if (r != CUBEB_OK) {
    return CUBEB_ERROR_INVALID_FORMAT;
  }

  SLDataLocator_BufferQueue loc_bufq;
  loc_bufq.locatorType = SL_DATALOCATOR_BUFFERQUEUE;
  loc_bufq.numBuffers = NBUFS;
  SLDataSource source;
  source.pLocator = &loc_bufq;
  source.pFormat = &format;

  SLDataLocator_OutputMix loc_outmix;
  loc_outmix.locatorType = SL_DATALOCATOR_OUTPUTMIX;
  loc_outmix.outputMix = stm->context->outmixObj;
  SLDataSink sink;
  sink.pLocator = &loc_outmix;
  sink.pFormat = NULL;

#if defined(__ANDROID__)
  const SLInterfaceID ids[] = {stm->context->SL_IID_BUFFERQUEUE,
                               stm->context->SL_IID_VOLUME,
                               stm->context->SL_IID_ANDROIDCONFIGURATION};
  const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
#else
  const SLInterfaceID ids[] = {ctx->SL_IID_BUFFERQUEUE, ctx->SL_IID_VOLUME};
  const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
#endif
  assert(NELEMS(ids) == NELEMS(req));

  unsigned int latency = stm->latency;
  uint32_t preferred_sampling_rate = stm->inputrate;
#if defined(__ANDROID__)
  if (get_android_version() >= ANDROID_VERSION_MARSHMALLOW) {
    // Reset preferred samping rate to trigger fallback to native sampling rate.
    preferred_sampling_rate = 0;
    if (opensl_get_min_latency(stm->context, *params, &latency) != CUBEB_OK) {
      // Default to AudioFlinger's advertised fast track latency of 10ms.
      latency = 10;
    }
    stm->latency = latency;
  }
#endif

  SLresult res = SL_RESULT_CONTENT_UNSUPPORTED;
  if (preferred_sampling_rate) {
    res = (*stm->context->eng)->CreateAudioPlayer(stm->context->eng,
                                                  &stm->playerObj,
                                                  &source,
                                                  &sink,
                                                  NELEMS(ids),
                                                  ids,
                                                  req);
  }

  // Sample rate not supported? Try again with primary sample rate!
  if (res == SL_RESULT_CONTENT_UNSUPPORTED) {
    if (opensl_get_preferred_sample_rate(stm->context, &preferred_sampling_rate)) {
      // If fail default is used
      preferred_sampling_rate = DEFAULT_SAMPLE_RATE;
    }

    format.samplesPerSec = preferred_sampling_rate * 1000;
    res = (*stm->context->eng)->CreateAudioPlayer(stm->context->eng,
                                                  &stm->playerObj,
                                                  &source,
                                                  &sink,
                                                  NELEMS(ids),
                                                  ids,
                                                  req);
  }

  if (res != SL_RESULT_SUCCESS) {
    opensl_stream_destroy(stm);
    return CUBEB_ERROR;
  }

  stm->outputrate = preferred_sampling_rate;
  stm->bytespersec = stm->outputrate * stm->framesize;
  stm->queuebuf_len = (stm->bytespersec * latency) / (1000 * NBUFS);
  // round up to the next multiple of stm->framesize, if needed.
  if (stm->queuebuf_len % stm->framesize) {
    stm->queuebuf_len += stm->framesize - (stm->queuebuf_len % stm->framesize);
  }

  // Calculate the capacity of input array
  stm->queuebuf_capacity = NBUFS;
  if (stm->output_enabled) {
    // Full duplex, update capacity to hold 1 sec of data
    stm->queuebuf_capacity = 1 * stm->outputrate / stm->queuebuf_len;
  }
  // Allocate input array
  stm->queuebuf = (void**)calloc(1, sizeof(void*) * stm->queuebuf_capacity);
  for (int i = 0; i < stm->queuebuf_capacity; ++i) {
    stm->queuebuf[i] = calloc(1, stm->queuebuf_len);
    assert(stm->queuebuf[i]);
  }

#if defined(__ANDROID__)
  SLuint32 stream_type = convert_stream_type_to_sl_stream(params->stream_type);
  if (stream_type != 0xFFFFFFFF) {
    SLAndroidConfigurationItf playerConfig;
    res = (*stm->playerObj)->GetInterface(stm->playerObj,
                                          stm->context->SL_IID_ANDROIDCONFIGURATION,
                                          &playerConfig);
    if (res != SL_RESULT_SUCCESS) {
      opensl_stream_destroy(stm);
      return CUBEB_ERROR;
    }

    res = (*playerConfig)->SetConfiguration(playerConfig,
                                            SL_ANDROID_KEY_STREAM_TYPE,
                                            &stream_type,
                                            sizeof(SLint32));
    if (res != SL_RESULT_SUCCESS) {
      opensl_stream_destroy(stm);
      return CUBEB_ERROR;
    }
  }
#endif

  res = (*stm->playerObj)->Realize(stm->playerObj, SL_BOOLEAN_FALSE);
  if (res != SL_RESULT_SUCCESS) {
    opensl_stream_destroy(stm);
    return CUBEB_ERROR;
  }

  res = (*stm->playerObj)->GetInterface(stm->playerObj,
                                        stm->context->SL_IID_PLAY,
                                        &stm->play);
  if (res != SL_RESULT_SUCCESS) {
    opensl_stream_destroy(stm);
    return CUBEB_ERROR;
  }

  res = (*stm->playerObj)->GetInterface(stm->playerObj,
                                        stm->context->SL_IID_BUFFERQUEUE,
                                        &stm->bufq);
  if (res != SL_RESULT_SUCCESS) {
    opensl_stream_destroy(stm);
    return CUBEB_ERROR;
  }

  res = (*stm->playerObj)->GetInterface(stm->playerObj,
                                        stm->context->SL_IID_VOLUME,
                                        &stm->volume);
  if (res != SL_RESULT_SUCCESS) {
    opensl_stream_destroy(stm);
    return CUBEB_ERROR;
  }

  res = (*stm->play)->RegisterCallback(stm->play, play_callback, stm);
  if (res != SL_RESULT_SUCCESS) {
    opensl_stream_destroy(stm);
    return CUBEB_ERROR;
  }

  // Work around wilhelm/AudioTrack badness, bug 1221228
  (*stm->play)->SetMarkerPosition(stm->play, (SLmillisecond)0);

  res = (*stm->play)->SetCallbackEventsMask(stm->play, (SLuint32)SL_PLAYEVENT_HEADATMARKER);
  if (res != SL_RESULT_SUCCESS) {
    opensl_stream_destroy(stm);
    return CUBEB_ERROR;
  }

  slBufferQueueCallback player_callback = bufferqueue_callback;
  if (stm->input_enabled) {
    player_callback = player_fullduplex_callback;
  }
  res = (*stm->bufq)->RegisterCallback(stm->bufq, player_callback, stm);
  if (res != SL_RESULT_SUCCESS) {
    opensl_stream_destroy(stm);
    return CUBEB_ERROR;
  }

  if (stm->input_enabled) {
    // Full duplex
    stm->output_queue = array_queue_create(stm->queuebuf_capacity);
    assert(stm->output_queue);
  }

  {
    // Enqueue a silent frame so once the player becomes playing, the frame
    // will be consumed and kick off the buffer queue callback.
    // Note the duration of a single frame is less than 1ms. We don't bother
    // adjusting the playback position.
    uint8_t *buf = stm->queuebuf[stm->queuebuf_idx++];
    memset(buf, 0, stm->framesize);
    res = (*stm->bufq)->Enqueue(stm->bufq, buf, stm->framesize);
    assert(res == SL_RESULT_SUCCESS);
  }

  LOG("Cubeb stream init playback success");
  return CUBEB_OK;
}

static int
opensl_stream_init(cubeb * ctx, cubeb_stream ** stream, char const * stream_name,
                   cubeb_devid input_device,
                   cubeb_stream_params * input_stream_params,
                   cubeb_devid output_device,
                   cubeb_stream_params * output_stream_params,
                   unsigned int latency,
                   cubeb_data_callback data_callback, cubeb_state_callback state_callback,
                   void * user_ptr)
{
  cubeb_stream * stm;

  assert(ctx);
  if (input_device || output_device) {
    /* Device selection not yet implemented. */
    return CUBEB_ERROR_DEVICE_UNAVAILABLE;
  }

  *stream = NULL;

  if ((output_stream_params &&
       (output_stream_params->channels < 1 || output_stream_params->channels > 32)) ||
      latency < 1 || latency > 2000) {
    return CUBEB_ERROR_INVALID_FORMAT;
  }

  stm = calloc(1, sizeof(*stm));
  assert(stm);

  stm->context = ctx;
  stm->data_callback = data_callback;
  stm->state_callback = state_callback;
  stm->user_ptr = user_ptr;
  stm->latency = latency;
  stm->input_enabled = (input_stream_params) ? 1 : 0;
  stm->output_enabled = (output_stream_params) ? 1 : 0;
  stm->shutdown = 1;

  int r = pthread_mutex_init(&stm->mutex, NULL);
  assert(r == 0);

  if (output_stream_params) {
    r = opensl_configure_playback(stm, output_stream_params);
    if (r != CUBEB_OK) {
      opensl_stream_destroy(stm);
      return r;
    }
  }

  if (input_stream_params) {
    r = opensl_configure_capture(stm, input_stream_params);
    if (r != CUBEB_OK) {
      opensl_stream_destroy(stm);
      return r;
    }
  }

  /* Configure resampler*/
  uint32_t target_sample_rate;
  if (input_stream_params) {
    target_sample_rate = input_stream_params->rate;
  } else {
    assert(output_stream_params);
    target_sample_rate = output_stream_params->rate;
  }

  // Use the actual configured rates for input
  // and output.
  cubeb_stream_params input_params;
  if (input_stream_params) {
    input_params = *input_stream_params;
    input_params.rate = stm->input_device_rate;
  }
  cubeb_stream_params output_params;
  if (output_stream_params) {
    output_params = *output_stream_params;
    output_params.rate = stm->outputrate;
  }

  stm->resampler = cubeb_resampler_create(stm,
                                          input_stream_params ? &input_params : NULL,
                                          output_stream_params ? &output_params : NULL,
                                          target_sample_rate,
                                          data_callback,
                                          user_ptr,
                                          CUBEB_RESAMPLER_QUALITY_DEFAULT);
  if (!stm->resampler) {
    LOG("Failed to create resampler");
    opensl_stream_destroy(stm);
    return CUBEB_ERROR;
  }

  *stream = stm;
  LOG("Cubeb stream (%p) init success", stm);
  return CUBEB_OK;
}

static int
opensl_start_player(cubeb_stream * stm)
{
  assert(stm->playerObj);
  SLuint32 playerState;
  (*stm->playerObj)->GetState(stm->playerObj, &playerState);
  if (playerState == SL_OBJECT_STATE_REALIZED) {
    SLresult res = (*stm->play)->SetPlayState(stm->play, SL_PLAYSTATE_PLAYING);
    if(res != SL_RESULT_SUCCESS) {
      LOG("Failed to start player. Error code: %lu", res);
      return CUBEB_ERROR;
    }
  }
  return CUBEB_OK;
}

static int
opensl_start_recorder(cubeb_stream * stm)
{
  assert(stm->recorderObj);
  SLuint32 recorderState;
  (*stm->recorderObj)->GetState(stm->recorderObj, &recorderState);
  if (recorderState == SL_OBJECT_STATE_REALIZED) {
    SLresult res = (*stm->recorderItf)->SetRecordState(stm->recorderItf, SL_RECORDSTATE_RECORDING);
    if(res != SL_RESULT_SUCCESS) {
      LOG("Failed to start recorder. Error code: %lu", res);
      return CUBEB_ERROR;
    }
  }
  return CUBEB_OK;
}

static int
opensl_start_full_duplex_thread(cubeb_stream * stm)
{
  if(pthread_create(&stm->full_duplex_thread, NULL, LoopFullDuplexThread, stm)) {
    LOG("Error creating full duplex thread");
    return CUBEB_ERROR;
  }
  return CUBEB_OK;
}

static int
opensl_stream_start(cubeb_stream * stm)
{
  assert(stm);

  pthread_mutex_lock(&stm->mutex);
  stm->shutdown = 0;
  stm->draining = 0;
  pthread_mutex_unlock(&stm->mutex);

  if (stm->playerObj) {
    int r = opensl_start_player(stm);
    if (r != CUBEB_OK) {
      return r;
    }
  }

  if (stm->recorderObj) {
    int r = opensl_start_recorder(stm);
    if (r != CUBEB_OK) {
      return r;
    }
  }

  if (stm->input_enabled && stm->output_enabled) {
    int r = opensl_start_full_duplex_thread(stm);
    if (r != CUBEB_OK) {
      return r;
    }
  }

  stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_STARTED);
  LOG("Cubeb stream (%p) started", stm);
  return CUBEB_OK;
}

static int
opensl_stop_player(cubeb_stream * stm)
{
  assert(stm->playerObj);
  assert(stm->shutdown || stm->draining);

  SLresult res = (*stm->play)->SetPlayState(stm->play, SL_PLAYSTATE_STOPPED);
  if (res != SL_RESULT_SUCCESS) {
    LOG("Failed to stop player. Error code: %lu", res);
    return CUBEB_ERROR;
  }

  res = (*stm->bufq)->Clear(stm->bufq);
  if (res != SL_RESULT_SUCCESS) {
    LOG("Failed to clear player buffer queue. Error code: %lu", res);
    return CUBEB_ERROR;
  }
  return CUBEB_OK;
}

static int
opensl_stop_recorder(cubeb_stream * stm)
{
  assert(stm->recorderObj);
  assert(stm->shutdown || stm->draining);

  SLresult res = (*stm->recorderItf)->SetRecordState(stm->recorderItf, SL_RECORDSTATE_STOPPED);
  if (res != SL_RESULT_SUCCESS) {
    LOG("Failed to stop recorder. Error code: %lu", res);
    return CUBEB_ERROR;
  }
  res = (*stm->recorderBufferQueueItf)->Clear(stm->recorderBufferQueueItf);
  if (res != SL_RESULT_SUCCESS) {
    LOG("Failed to clear recorder buffer queue. Error code: %lu", res);
    return CUBEB_ERROR;
  }

  if (stm->input_queue) {
    // In full duplex send an silent buffer to unlock the thread
    // waiting in the input queue (just in case)
    array_queue_push(stm->input_queue, stm->input_silent_buffer);
  }
  return CUBEB_OK;
}

static int
opensl_stream_stop(cubeb_stream * stm)
{
  assert(stm);

  pthread_mutex_lock(&stm->mutex);
  stm->shutdown = 1;
  pthread_mutex_unlock(&stm->mutex);

  if (stm->playerObj) {
    int r = opensl_stop_player(stm);
    if (r != CUBEB_OK) {
      return r;
    }
  }

  if (stm->recorderObj) {
    int r = opensl_stop_recorder(stm);
    if (r != CUBEB_OK) {
      return r;
    }
  }

  // On full duplex wait thread to shutdown
  if (stm->input_enabled && stm->output_enabled) {
    int r = pthread_join(stm->full_duplex_thread, NULL);
    assert(r == 0);
  }

  stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_STOPPED);
  LOG("Cubeb stream (%p) stopped", stm);
  return CUBEB_OK;
}

static void
opensl_stream_destroy(cubeb_stream * stm)
{
  assert(stm->draining || stm->shutdown);

  if (stm->playerObj) {
    (*stm->playerObj)->Destroy(stm->playerObj);
    stm->playerObj = NULL;
    stm->play = NULL;
    stm->bufq = NULL;
    for (int i = 0; i < stm->queuebuf_capacity; ++i) {
      free(stm->queuebuf[i]);
    }
  }

  if (stm->recorderObj) {
    (*stm->recorderObj)->Destroy(stm->recorderObj);
    stm->recorderObj = NULL;
    stm->recorderItf = NULL;
    stm->recorderBufferQueueItf = NULL;
    for(int i = 0; i < stm->input_array_capacity; ++i) {
      free(stm->input_buffer_array[i]);
    }
  }
  cubeb_resampler_destroy(stm->resampler);
  array_queue_destroy(stm->input_queue);
  free(stm->input_silent_buffer);

  array_queue_destroy(stm->output_queue);

  pthread_mutex_destroy(&stm->mutex);

  LOG("Cubeb stream (%p) destroyed", stm);
  free(stm);
}

static int
opensl_stream_get_position(cubeb_stream * stm, uint64_t * position)
{
  SLmillisecond msec;
  uint64_t samplerate;
  SLresult res;
  int r;
  uint32_t mixer_latency;
  uint32_t compensation_msec = 0;

  res = (*stm->play)->GetPosition(stm->play, &msec);
  if (res != SL_RESULT_SUCCESS)
    return CUBEB_ERROR;

  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  if(stm->lastPosition == msec) {
    compensation_msec =
      (t.tv_sec*1000000000LL + t.tv_nsec - stm->lastPositionTimeStamp) / 1000000;
  } else {
    stm->lastPositionTimeStamp = t.tv_sec*1000000000LL + t.tv_nsec;
    stm->lastPosition = msec;
  }

  samplerate = stm->inputrate;

  r = stm->context->get_output_latency(&mixer_latency, stm->stream_type);
  if (r) {
    return CUBEB_ERROR;
  }

  pthread_mutex_lock(&stm->mutex);
  int64_t maximum_position = stm->written * (int64_t)stm->inputrate / stm->outputrate;
  pthread_mutex_unlock(&stm->mutex);
  assert(maximum_position >= 0);

  if (msec > mixer_latency) {
    int64_t unadjusted_position;
    if (stm->lastCompensativePosition > msec + compensation_msec) {
      // Over compensation, use lastCompensativePosition.
      unadjusted_position =
        samplerate * (stm->lastCompensativePosition - mixer_latency) / 1000;
    } else {
      unadjusted_position =
        samplerate * (msec - mixer_latency + compensation_msec) / 1000;
      stm->lastCompensativePosition = msec + compensation_msec;
    }
    *position = unadjusted_position < maximum_position ?
      unadjusted_position : maximum_position;
  } else {
    *position = 0;
  }
  return CUBEB_OK;
}

int
opensl_stream_get_latency(cubeb_stream * stm, uint32_t * latency)
{
  int r;
  uint32_t mixer_latency; // The latency returned by AudioFlinger is in ms.

  /* audio_stream_type_t is an int, so this is okay. */
  r = stm->context->get_output_latency(&mixer_latency, stm->stream_type);
  if (r) {
    return CUBEB_ERROR;
  }

  *latency = stm->latency * stm->inputrate / 1000 + // OpenSL latency
    mixer_latency * stm->inputrate / 1000; // AudioFlinger latency

  return CUBEB_OK;
}

int
opensl_stream_set_volume(cubeb_stream * stm, float volume)
{
  SLresult res;
  SLmillibel max_level, millibels;
  float unclamped_millibels;

  res = (*stm->volume)->GetMaxVolumeLevel(stm->volume, &max_level);

  if (res != SL_RESULT_SUCCESS) {
    return CUBEB_ERROR;
  }

  /* millibels are 100*dB, so the conversion from the volume's linear amplitude
   * is 100 * 20 * log(volume). However we clamp the resulting value before
   * passing it to lroundf() in order to prevent it from silently returning an
   * erroneous value when the unclamped value exceeds the size of a long. */
  unclamped_millibels = 100.0f * 20.0f * log10f(fmaxf(volume, 0.0f));
  unclamped_millibels = fmaxf(unclamped_millibels, SL_MILLIBEL_MIN);
  unclamped_millibels = fminf(unclamped_millibels, max_level);

  millibels = lroundf(unclamped_millibels);

  res = (*stm->volume)->SetVolumeLevel(stm->volume, millibels);

  if (res != SL_RESULT_SUCCESS) {
    return CUBEB_ERROR;
  }
  return CUBEB_OK;
}

static struct cubeb_ops const opensl_ops = {
  .init = opensl_init,
  .get_backend_id = opensl_get_backend_id,
  .get_max_channel_count = opensl_get_max_channel_count,
  .get_min_latency = opensl_get_min_latency,
  .get_preferred_sample_rate = opensl_get_preferred_sample_rate,
  .enumerate_devices = NULL,
  .destroy = opensl_destroy,
  .stream_init = opensl_stream_init,
  .stream_destroy = opensl_stream_destroy,
  .stream_start = opensl_stream_start,
  .stream_stop = opensl_stream_stop,
  .stream_get_position = opensl_stream_get_position,
  .stream_get_latency = opensl_stream_get_latency,
  .stream_set_volume = opensl_stream_set_volume,
  .stream_set_panning = NULL,
  .stream_get_current_device = NULL,
  .stream_device_destroy = NULL,
  .stream_register_device_changed_callback = NULL,
  .register_device_collection_changed = NULL
};
