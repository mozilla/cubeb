/* ex: set tabstop=2 shiftwidth=2 expandtab:
 * Copyright © 2011 Jan Kelling
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <stdatomic.h>
#include <aaudio/AAudio.h>
#include "cubeb/cubeb.h"
#include "cubeb-internal.h"
#include "cubeb_resampler.h"
#include "cubeb_log.h"


#ifdef DISABLE_LIBAAUDIO_DLOPEN
#define WRAP(x) x
#else
#define WRAP(x) cubeb_##x
#define LIBAAUDIO_API_VISIT(X)                    \
  X(AAudio_convertResultToText)                   \
  X(AAudio_convertStreamStateToText)              \
  X(AAudio_createStreamBuilder)                   \
  X(AAudioStreamBuilder_openStream)               \
  X(AAudioStreamBuilder_setChannelCount)          \
  X(AAudioStreamBuilder_setBufferCapacityInFrames)\
  X(AAudioStreamBuilder_setDirection)             \
  X(AAudioStreamBuilder_setFormat)                \
  X(AAudioStreamBuilder_setSharingMode)           \
  X(AAudioStreamBuilder_setPerformanceMode)       \
  X(AAudioStreamBuilder_setSampleRate)            \
  X(AAudioStreamBuilder_delete)                   \
  X(AAudioStreamBuilder_setDataCallback)          \
  X(AAudioStreamBuilder_setErrorCallback)         \
  X(AAudioStream_close)                           \
  X(AAudioStream_read)                            \
  X(AAudioStream_requestStart)                    \
  X(AAudioStream_requestPause)                    \
  X(AAudioStream_setBufferSizeInFrames)           \
  X(AAudioStream_getTimestamp)                    \
  X(AAudioStream_requestFlush)                    \
  X(AAudioStream_requestStop)                     \
  X(AAudioStream_getPerformanceMode)              \
  X(AAudioStream_getSharingMode)                  \
  X(AAudioStream_getBufferSizeInFrames)           \
  X(AAudioStream_getBufferCapacityInFrames)       \
  X(AAudioStream_getSampleRate)                   \
  X(AAudioStream_waitForStateChange)              \
  X(AAudioStream_getFramesRead)                   \
  X(AAudioStream_getState)                        \


  // not needed or added later on
  // X(AAudioStreamBuilder_setFramesPerDataCallback) \
  // X(AAudioStreamBuilder_setDeviceId)              \
  // X(AAudioStreamBuilder_setSamplesPerFrame)       \
  // X(AAudioStream_getSamplesPerFrame)              \
  // X(AAudioStream_getDeviceId)                     \
  // X(AAudioStream_write)                           \
  // X(AAudioStream_getChannelCount)                 \
  // X(AAudioStream_getFormat)                       \
  // X(AAudioStream_getFramesPerBurst)               \
  // X(AAudioStream_getFramesWritten)                \
  // X(AAudioStream_getXRunCount)                    \
  // X(AAudioStream_isMMapUsed)                      \
  // X(AAudioStreamBuilder_setUsage)                 \
  // X(AAudioStreamBuilder_setContentType)           \
  // X(AAudioStreamBuilder_setInputPreset)           \
  // X(AAudioStreamBuilder_setSessionId)             \
  // X(AAudioStream_getUsage)                        \
  // X(AAudioStream_getContentType)                  \
  // X(AAudioStream_getInputPreset)                  \
  // X(AAudioStream_getSessionId)                    \
  //

#define MAKE_TYPEDEF(x) static typeof(x) * cubeb_##x;
LIBAAUDIO_API_VISIT(MAKE_TYPEDEF)
#undef MAKE_TYPEDEF
#endif

#define MAX_STREAMS 16
static const struct cubeb_ops aaudio_ops;

enum stream_state {
  STREAM_STATE_INIT = 0,
  STREAM_STATE_STOPPED,
  STREAM_STATE_STOPPING,
  STREAM_STATE_STARTED,
  STREAM_STATE_STARTING,
  STREAM_STATE_DRAINING,
  STREAM_STATE_ERROR,
  STREAM_STATE_SHUTDOWN,
};

struct cubeb_stream {
  /* Note: Must match cubeb_stream layout in cubeb.c. */
  cubeb * context;
  void * user_ptr;
  /**/

  _Atomic bool in_use;
  _Atomic enum stream_state state;

  AAudioStream * ostream;
  AAudioStream * istream;
  cubeb_data_callback data_callback;
  cubeb_state_callback state_callback;
  cubeb_resampler * resampler;

  // mutex synchronizes access to the stream from the state thread
  // and user-called functions. Everything that is accessed in the
  // aaudio data (or error) callback is synchronized only via atomics.
  pthread_mutex_t mutex;

  void * in_buf;
  unsigned in_frame_size;

  cubeb_sample_format out_format;
  _Atomic float volume;
  unsigned out_channels;
  unsigned out_frame_size;
};

struct cubeb {
  struct cubeb_ops const * ops;
  void * libaaudio;

  struct {
    // The state thread: it waits for state changes and stops
    // drained streams.
    pthread_t thread;
    pthread_t notifier;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    _Atomic bool join;
    _Atomic bool waiting;
  } state;

  // streams[i].in_use signals whether a stream is used
  struct cubeb_stream streams[MAX_STREAMS];
};

// Only allowed from state thread, while mutex on stm is locked
static void shutdown(cubeb_stream* stm)
{
  if (stm->istream) {
    WRAP(AAudioStream_requestStop)(stm->istream);
  }
  if (stm->ostream) {
    WRAP(AAudioStream_requestStop)(stm->ostream);
  }

  stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_ERROR);
  atomic_store(&stm->state, STREAM_STATE_SHUTDOWN);
}

// Returns whether the given state is one in which we wait for
// an asynchronous change
static bool waiting_state(enum stream_state state)
{
  switch(state) {
    case STREAM_STATE_DRAINING:
    case STREAM_STATE_STARTING:
    case STREAM_STATE_STOPPING:
      return true;
    default:
      return false;
  }
}

// Returns whether this stream is still waiting for a state change
static void update_state(cubeb_stream * stm)
{
  // fast path for streams that don't wait for state change or are invalid
  enum stream_state old_state = atomic_load(&stm->state);
  if (old_state == STREAM_STATE_INIT ||
      old_state == STREAM_STATE_STARTED ||
      old_state == STREAM_STATE_STOPPED ||
      old_state == STREAM_STATE_SHUTDOWN) {
    return;
  }

  // If the main thread currently operates on this thread, we don't
  // have to wait for it
  int err = pthread_mutex_trylock(&stm->mutex);
  if (err != 0) {
    if (err != EBUSY) {
      LOG("pthread_mutex_trylock: %s", strerror(err));
    }
    return;
  }

  // check again: if this is true now, the stream was destroyed or
  // changed between our fast path check and locking the mutex
  old_state = atomic_load(&stm->state);
  if (old_state == STREAM_STATE_INIT ||
      old_state == STREAM_STATE_STARTED ||
      old_state == STREAM_STATE_STOPPED ||
      old_state == STREAM_STATE_SHUTDOWN) {
    pthread_mutex_unlock(&stm->mutex);
    return;
  }

  // We compute the new state the stream has and then compare_exchange it
  // if it has changed. This way we will never just overwrite state
  // changes that were set from the audio thread in the meantime,
  // such as a draining or error state.
  enum stream_state new_state;
  do {
    if (old_state == STREAM_STATE_SHUTDOWN) {
      pthread_mutex_unlock(&stm->mutex);
      return;
    }

    if (old_state == STREAM_STATE_ERROR) {
      shutdown(stm);
      pthread_mutex_unlock(&stm->mutex);
      return;
    }

    new_state = old_state;

    aaudio_stream_state_t istate = 0;
    aaudio_stream_state_t ostate = 0;

    aaudio_result_t res;
    if (stm->istream) {
      res = WRAP(AAudioStream_waitForStateChange)(stm->istream,
        AAUDIO_STREAM_STATE_UNKNOWN, &istate, 0);
      if (res != AAUDIO_OK) {
        pthread_mutex_unlock(&stm->mutex);
        LOG("AAudioStream_waitForStateChanged: %s", WRAP(AAudio_convertResultToText)(res));
        return;
      }
      assert(istate);
    }

    if (stm->ostream) {
      res = WRAP(AAudioStream_waitForStateChange)(stm->ostream,
        AAUDIO_STREAM_STATE_UNKNOWN, &ostate, 0);
      if (res != AAUDIO_OK) {
        pthread_mutex_unlock(&stm->mutex);
        LOG("AAudioStream_waitForStateChanged: %s", WRAP(AAudio_convertResultToText)(res));
        return;
      }
      assert(ostate);
    }

    // handle invalid stream states
    if (istate == AAUDIO_STREAM_STATE_PAUSING ||
       istate == AAUDIO_STREAM_STATE_PAUSED ||
       istate == AAUDIO_STREAM_STATE_FLUSHING ||
       istate == AAUDIO_STREAM_STATE_FLUSHED ||
       istate == AAUDIO_STREAM_STATE_UNKNOWN ||
       istate == AAUDIO_STREAM_STATE_DISCONNECTED) {
      const char* name = WRAP(AAudio_convertStreamStateToText)(istate);
      LOG("Invalid android input stream state %s", name);
      shutdown(stm);
      pthread_mutex_unlock(&stm->mutex);
      return;
    }

    if (ostate == AAUDIO_STREAM_STATE_PAUSING ||
       ostate == AAUDIO_STREAM_STATE_PAUSED ||
       ostate == AAUDIO_STREAM_STATE_FLUSHING ||
       ostate == AAUDIO_STREAM_STATE_FLUSHED ||
       ostate == AAUDIO_STREAM_STATE_UNKNOWN ||
       ostate == AAUDIO_STREAM_STATE_DISCONNECTED) {
      const char* name = WRAP(AAudio_convertStreamStateToText)(istate);
      LOG("Invalid android output stream state %s", name);
      shutdown(stm);
      pthread_mutex_unlock(&stm->mutex);
      return;
    }

    switch (old_state) {
      case STREAM_STATE_STARTING:
        if ((!istate || istate == AAUDIO_STREAM_STATE_STARTED) &&
           (!ostate || ostate == AAUDIO_STREAM_STATE_STARTED)) {
          stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_STARTED);
          new_state = STREAM_STATE_STARTED;
        }
        break;
      case STREAM_STATE_DRAINING:
        // The DRAINING state means that we want to stop the streams but
        // may not have done so yet.
        // The aaudio docs state that returning STOP from the callback isn't
        // enough, the stream has to be stopped from another threads
        // afterwards.
        // No callbacks are triggered anymore when requestStop returns.
        // That is important as we otherwise might read from a closed istream
        // for a duplex stream.
        // Therefor it is important to close ostream first.
        if (ostate &&
            ostate != AAUDIO_STREAM_STATE_STOPPING &&
            ostate != AAUDIO_STREAM_STATE_STOPPED) {
          res = WRAP(AAudioStream_requestStop)(stm->ostream);
          if (res != AAUDIO_OK) {
            LOG("AAudioStream_requestStop: %s", WRAP(AAudio_convertResultToText)(res));
            return;
          }
        }
        if (istate &&
            istate != AAUDIO_STREAM_STATE_STOPPING &&
            istate != AAUDIO_STREAM_STATE_STOPPED) {
          res = WRAP(AAudioStream_requestStop)(stm->istream);
          if (res != AAUDIO_OK) {
            LOG("AAudioStream_requestStop: %s", WRAP(AAudio_convertResultToText)(res));
            return;
          }
        }

        // we always wait until both streams are stopped until we
        // send STATE_DRAINED. Then we can directly transition
        // our logical state to STREAM_STATE_STOPPED, not triggering
        // an additional STATE_STOPPED callback
        if ((!ostate || ostate == AAUDIO_STREAM_STATE_STOPPED) &&
            (!istate || istate == AAUDIO_STREAM_STATE_STOPPED)) {
          new_state = STREAM_STATE_STOPPED;
          stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_DRAINED);
        }

        break;
      case STREAM_STATE_STOPPING:
        assert(!istate ||
            istate == AAUDIO_STREAM_STATE_STOPPING ||
            istate == AAUDIO_STREAM_STATE_STOPPED);
        assert(!ostate ||
            ostate == AAUDIO_STREAM_STATE_STOPPING ||
            ostate == AAUDIO_STREAM_STATE_STOPPED);
        if ((!istate || istate == AAUDIO_STREAM_STATE_STOPPED) &&
           (!ostate || ostate == AAUDIO_STREAM_STATE_STOPPED)) {
          stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_STOPPED);
          new_state = STREAM_STATE_STOPPED;
        }
        break;
      default:
        assert(false && "Unreachable: invalid state");
    }
  } while (old_state != new_state &&
      !atomic_compare_exchange_strong(&stm->state, &old_state, new_state));

  pthread_mutex_unlock(&stm->mutex);
}

static void * notifier_thread(void * user_ptr)
{
  cubeb * ctx = (cubeb*) user_ptr;
  pthread_mutex_lock(&ctx->state.mutex);
  while (!atomic_load(&ctx->state.join)) {
    pthread_cond_wait(&ctx->state.cond, &ctx->state.mutex);
    if (atomic_load(&ctx->state.waiting)) {
      pthread_cond_signal(&ctx->state.cond);
    }
  }

  // make sure other thread joins as well
  pthread_cond_signal(&ctx->state.cond);
  pthread_mutex_unlock(&ctx->state.mutex);
  LOG("Exiting notifier thread");
  return NULL;
}

static void * state_thread(void * user_ptr)
{
  cubeb * ctx = (cubeb*) user_ptr;
  pthread_mutex_lock(&ctx->state.mutex);

  bool waiting = false;
  while (!atomic_load(&ctx->state.join)) {
    waiting |= atomic_load(&ctx->state.waiting);
    if (waiting) {
      atomic_store(&ctx->state.waiting, false);
      waiting = false;
      for (unsigned i = 0u; i < MAX_STREAMS; ++i) {
        cubeb_stream* stm = &ctx->streams[i];
        update_state(stm);
        waiting |= waiting_state(atomic_load(&stm->state));
      }

      // state changed from another thread, update again immediately
      if(atomic_load(&ctx->state.waiting)) {
        waiting = true;
        continue;
      }

      // Not waiting for any change anymore: we can wait on the
      // condition variable without timeout
      if (!waiting) {
        continue;
      }

      // while any stream is waiting for state change we sleep with regular
      // timeouts. But we wake up immediately if signaled.
      // This might seem like a poor man's implementation of state change
      // waiting but (as of december 2019), the implementation of
      // AAudioStream_waitForStateChange is pretty much the same:
      // https://android.googlesource.com/platform/frameworks/av/+/refs/heads/master/media/libaaudio/src/core/AudioStream.cpp#277
      struct timespec timeout;
      clock_gettime(CLOCK_MONOTONIC, &timeout);
      timeout.tv_nsec += 5 * 1000 * 1000; // wait 5ms
      pthread_cond_timedwait(&ctx->state.cond, &ctx->state.mutex, &timeout);
    } else {
      pthread_cond_wait(&ctx->state.cond, &ctx->state.mutex);
    }
  }

  // make sure other thread joins as well
  pthread_cond_signal(&ctx->state.cond);
  pthread_mutex_unlock(&ctx->state.mutex);
  LOG("Exiting state thread");
  return NULL;
}

static char const *
aaudio_get_backend_id(cubeb * ctx)
{
  (void)ctx;
  return "aaudio";
}

static int
aaudio_get_max_channel_count(cubeb * ctx, uint32_t * max_channels)
{
  assert(ctx && max_channels);
  // NOTE: we might get more, AAudio docs don't specify anything.
  *max_channels = 2;
  return CUBEB_OK;
}

static void
aaudio_destroy(cubeb * ctx)
{
#ifndef NDEBUG
  // make sure all streams were destroyed
  for(unsigned i = 0u; i < MAX_STREAMS; ++i) {
    assert(!atomic_load(&ctx->streams[i].in_use));
  }
#endif

  // broadcast joining to both threads
  // they will additionally signal each other before joining
  atomic_store(&ctx->state.join, true);
  pthread_cond_broadcast(&ctx->state.cond);

  int err;
  err = pthread_join(ctx->state.thread, NULL);
  if (err != 0) {
    LOG("pthread_join: %s", strerror(err));
  }

  err = pthread_join(ctx->state.notifier, NULL);
  if (err != 0) {
    LOG("pthread_join: %s", strerror(err));
  }

  pthread_cond_destroy(&ctx->state.cond);
  pthread_mutex_destroy(&ctx->state.mutex);
  for (unsigned i = 0u; i < MAX_STREAMS; ++i) {
    pthread_mutex_destroy(&ctx->streams[i].mutex);
  }

  if (ctx->libaaudio) {
    dlclose(ctx->libaaudio);
  }
  free(ctx);
}

/*static*/ int
aaudio_init(cubeb ** context, char const * context_name) {
  (void) context_name;
  int err;

  // load api
  void * libaaudio = NULL;
#ifndef DISABLE_LIBAAUDIO_DLOPEN
  libaaudio = dlopen("libaaudio.so", RTLD_NOW);
  if (!libaaudio) {
    return CUBEB_ERROR;
  }

#define LOAD(x) {                               \
    WRAP(x) = (typeof(WRAP(x))) (dlsym(libaaudio, #x));             \
    if (!WRAP(x)) {                             \
      LOG("AAudio: Failed to load %s", #x);     \
      dlclose(libaaudio);                       \
      return CUBEB_ERROR;                       \
    }                                           \
  }

  LIBAAUDIO_API_VISIT(LOAD);
#undef LOAD
#endif

  cubeb* ctx = (cubeb*) calloc(1, sizeof(*ctx));
  ctx->ops = &aaudio_ops;
  ctx->libaaudio = libaaudio;
  atomic_init(&ctx->state.join, false);
  atomic_init(&ctx->state.waiting, false);
  pthread_mutex_init(&ctx->state.mutex, NULL);

  pthread_condattr_t cond_attr;
  pthread_condattr_init(&cond_attr);
  pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
  err = pthread_cond_init(&ctx->state.cond, &cond_attr);
  if (err) {
    LOG("pthread_cond_init: %s", strerror(err));
    aaudio_destroy(ctx);
    return CUBEB_ERROR;
  }

  // The stream mutexes are not bound to the lifetimes of the
  // streams since we need them to synchronize the streams with
  // the state thread.
  for (unsigned i = 0u; i < MAX_STREAMS; ++i) {
    atomic_init(&ctx->streams[i].in_use, false);
    atomic_init(&ctx->streams[i].state, STREAM_STATE_INIT);
    pthread_mutex_init(&ctx->streams[i].mutex, NULL);
  }

  err = pthread_create(&ctx->state.thread, NULL, state_thread, ctx);
  if (err != 0) {
    LOG("pthread_create: %s", strerror(err));
    aaudio_destroy(ctx);
    return CUBEB_ERROR;
  }

  // TODO: we could set the priority of the notifier thread lower than
  // the priority of the state thread. This way, it's more likely
  // that the state thread will be woken up by the condition variable signal
  // when both are currently waiting
  err = pthread_create(&ctx->state.notifier, NULL, notifier_thread, ctx);
  if (err != 0) {
    LOG("pthread_create: %s", strerror(err));
    aaudio_destroy(ctx);
    return CUBEB_ERROR;
  }

  *context = ctx;
  return CUBEB_OK;
}

static void
apply_volume(cubeb_stream * stm, void * audio_data, uint32_t num_frames) {
  // optimization: we don't have to change anything in this case
  float volume = atomic_load(&stm->volume);
  if(volume == 1.f) {
    return;
  }

  switch(stm->out_format) {
    case CUBEB_SAMPLE_S16NE:
      for(uint32_t i = 0u; i < num_frames * stm->out_channels; ++i) {
        ((int16_t*)audio_data)[i] *= volume;
      }
      break;
    case CUBEB_SAMPLE_FLOAT32NE:
      for(uint32_t i = 0u; i < num_frames * stm->out_channels; ++i) {
        ((float*)audio_data)[i] *= volume;
      }
      break;
    default:
      assert(false && "Unreachable: invalid stream out_format");
  }
}

// Returning AAUDIO_CALLBACK_RESULT_STOP seems to put the stream in
// an invalid state. Seems like an AAudio bug/bad documentation.
// We therefore only return it on error.

static aaudio_data_callback_result_t
aaudio_duplex_data_cb(AAudioStream * astream, void * user_data,
    void * audio_data, int32_t num_frames)
{
  cubeb_stream * stm = (cubeb_stream*) user_data;
  assert(stm->ostream == astream);
  assert(stm->istream);
  assert(num_frames >= 0);

  enum stream_state state = atomic_load(&stm->state);
  int istate = WRAP(AAudioStream_getState)(stm->istream);
  int ostate = WRAP(AAudioStream_getState)(stm->ostream);
  ALOGV("aaudio duplex data cb on stream %p: state %ld (in: %d, out: %d), num_frames: %ld",
      (void*) stm, state, istate, ostate, num_frames);

  // all other states may happen since the callback might be called
  // from within requestStart
  assert(state != STREAM_STATE_SHUTDOWN);

  // This might happen when we started draining but not yet actually
  // stopped the stream from the state thread.
  if (state == STREAM_STATE_DRAINING) {
    memset(audio_data, 0x0, num_frames * stm->out_frame_size);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }

  long in_num_frames = WRAP(AAudioStream_read)(stm->istream,
    stm->in_buf, num_frames, 0);
  if (in_num_frames < 0) { // error
    atomic_store(&stm->state, STREAM_STATE_ERROR);
    LOG("AAudioStream_read: %s", WRAP(AAudio_convertResultToText)(in_num_frames));
    return AAUDIO_CALLBACK_RESULT_STOP;
  }

  // This can happen shortly after starting the stream. AAudio might immediately
  // begin to buffer output but not have any input ready yet. We could
  // block AAudioStream_read (passing a timeout > 0) but that leads to issues
  // since blocking in this callback is a bad idea in general and it might break
  // the stream when it is stopped by another thread shortly after being started.
  // We therefore simply send silent input to the application.
  if (in_num_frames < num_frames) {
    ALOGV("AAudioStream_read returned not enough frames: %ld instead of %d",
      in_num_frames, num_frames);
    unsigned left = num_frames - in_num_frames;
    char * buf = ((char*) stm->in_buf) + in_num_frames * stm->in_frame_size;
    memset(buf, 0x0, left * stm->in_frame_size);
    in_num_frames = num_frames;
  }

  long done_frames = cubeb_resampler_fill(stm->resampler, stm->in_buf,
    &in_num_frames, audio_data, num_frames);

  if (done_frames < 0 || done_frames > num_frames) {
    LOG("Error in data callback or resampler: %ld", done_frames);
    atomic_store(&stm->state, STREAM_STATE_ERROR);
    return AAUDIO_CALLBACK_RESULT_STOP;
  } else if (done_frames < num_frames) {
    atomic_store(&stm->state, STREAM_STATE_DRAINING);
    atomic_store(&stm->context->state.waiting, true);
    pthread_cond_signal(&stm->context->state.cond);

    char* begin = ((char*)audio_data) + done_frames * stm->out_frame_size;
    memset(begin, 0x0, (num_frames - done_frames) * stm->out_frame_size);
  }

  apply_volume(stm, audio_data, done_frames);
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static aaudio_data_callback_result_t
aaudio_output_data_cb(AAudioStream * astream, void * user_data,
    void * audio_data, int32_t num_frames)
{
  cubeb_stream * stm = (cubeb_stream*) user_data;
  assert(stm->ostream == astream);
  assert(!stm->istream);
  assert(num_frames >= 0);

  enum stream_state state = atomic_load(&stm->state);
  int ostate = WRAP(AAudioStream_getState)(stm->ostream);
  ALOGV("aaudio output data cb on stream %p: state %ld (%d), num_frames: %ld",
      (void*) stm, state, ostate, num_frames);

  // all other states may happen since the callback might be called
  // from within requestStart
  assert(state != STREAM_STATE_SHUTDOWN);

  // This might happen when we started draining but not yet actually
  // stopped the stream from the state thread.
  if (state == STREAM_STATE_DRAINING) {
    memset(audio_data, 0x0, num_frames * stm->out_frame_size);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }

  long done_frames = cubeb_resampler_fill(stm->resampler, NULL, NULL,
    audio_data, num_frames);
  if (done_frames < 0 || done_frames > num_frames) {
    LOG("Error in data callback or resampler: %ld", done_frames);
    atomic_store(&stm->state, STREAM_STATE_ERROR);
    return AAUDIO_CALLBACK_RESULT_STOP;
  } else if (done_frames < num_frames) {
    atomic_store(&stm->state, STREAM_STATE_DRAINING);
    atomic_store(&stm->context->state.waiting, true);
    pthread_cond_signal(&stm->context->state.cond);

    char* begin = ((char*)audio_data) + done_frames * stm->out_frame_size;
    memset(begin, 0x0, (num_frames - done_frames) * stm->out_frame_size);
  }

  apply_volume(stm, audio_data, done_frames);
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static aaudio_data_callback_result_t
aaudio_input_data_cb(AAudioStream * astream, void * user_data,
    void * audio_data, int32_t num_frames)
{
  cubeb_stream * stm = (cubeb_stream*) user_data;
  assert(stm->istream == astream);
  assert(!stm->ostream);
  assert(num_frames >= 0);

  enum stream_state state = atomic_load(&stm->state);
  int istate = WRAP(AAudioStream_getState)(stm->istream);
  ALOGV("aaudio input data cb on stream %p: state %ld (%d), num_frames: %ld",
      (void*) stm, state, istate, num_frames);

  // all other states may happen since the callback might be called
  // from within requestStart
  assert(state != STREAM_STATE_SHUTDOWN);

  // This might happen when we started draining but not yet actually
  // stopped the stream from the state thread.
  if (state == STREAM_STATE_DRAINING) {
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }

  long input_frame_count = num_frames;
  long done_frames = cubeb_resampler_fill(stm->resampler,
    audio_data, &input_frame_count, NULL, 0);
  if (done_frames < 0 || done_frames > num_frames) {
    LOG("Error in data callback or resampler: %ld", done_frames);
    atomic_store(&stm->state, STREAM_STATE_ERROR);
    return AAUDIO_CALLBACK_RESULT_STOP;
  } else if (done_frames < input_frame_count) {
    // we don't really drain an input stream, just have to
    // stop it from the state thread. That is signaled via the
    // DRAINING state.
    atomic_store(&stm->state, STREAM_STATE_DRAINING);
    atomic_store(&stm->context->state.waiting, true);
    pthread_cond_signal(&stm->context->state.cond);
  }

  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void
aaudio_error_cb(AAudioStream * astream, void * user_data, aaudio_result_t error)
{
  cubeb_stream * stm = (cubeb_stream*) user_data;
  assert(stm->ostream == astream || stm->istream == astream);
  LOG("AAudio error callback: %s", WRAP(AAudio_convertResultToText)(error));
  atomic_store(&stm->state, STREAM_STATE_ERROR);
}

static int
realize_stream(AAudioStreamBuilder * sb, const cubeb_stream_params * params,
    AAudioStream ** stream, unsigned * frame_size)
{
  aaudio_result_t res;
  assert(params->rate);
  assert(params->channels);

  WRAP(AAudioStreamBuilder_setSampleRate)(sb, params->rate);
  WRAP(AAudioStreamBuilder_setChannelCount)(sb, params->channels);

  aaudio_format_t fmt;
  switch (params->format) {
    case CUBEB_SAMPLE_S16NE:
      fmt = AAUDIO_FORMAT_PCM_I16;
      *frame_size = 2 * params->channels;
      break;
    case CUBEB_SAMPLE_FLOAT32NE:
      fmt = AAUDIO_FORMAT_PCM_FLOAT;
      *frame_size = 4 * params->channels;
      break;
    default:
      return CUBEB_ERROR_INVALID_FORMAT;
  }

  WRAP(AAudioStreamBuilder_setFormat)(sb, fmt);
  res = WRAP(AAudioStreamBuilder_openStream)(sb, stream);
  if (res == AAUDIO_ERROR_INVALID_FORMAT) {
    LOG("AAudio device doesn't support output format %d", fmt);
    return CUBEB_ERROR_INVALID_FORMAT;
  } else if (params->rate && res == AAUDIO_ERROR_INVALID_RATE) {
    // The requested rate is not supported.
    // Just try again with default rate, we create a resampler anyways
    WRAP(AAudioStreamBuilder_setSampleRate)(sb, AAUDIO_UNSPECIFIED);
    res = WRAP(AAudioStreamBuilder_openStream)(sb, stream);
  }

  // When the app has no permission to record audio (android.permission.RECORD_AUDIO)
  // but requested and input stream, this will return INVALID_ARGUMENT.
  if (res != AAUDIO_OK) {
    LOG("AAudioStreamBuilder_openStream: %s", WRAP(AAudio_convertResultToText)(res));
    return CUBEB_ERROR;
  }

  return CUBEB_OK;
}

static void
aaudio_stream_destroy(cubeb_stream * stm)
{
  pthread_mutex_lock(&stm->mutex);
  assert(stm->state == STREAM_STATE_STOPPED ||
      stm->state == STREAM_STATE_STOPPING ||
      stm->state == STREAM_STATE_INIT ||
      stm->state == STREAM_STATE_DRAINING ||
      stm->state == STREAM_STATE_ERROR ||
      stm->state == STREAM_STATE_SHUTDOWN);

  aaudio_result_t res;

  // No callbacks are triggered anymore when requestStop returns.
  // That is important as we otherwise might read from a closed istream
  // for a duplex stream.
  if (stm->ostream) {
    if (stm->state != STREAM_STATE_STOPPED &&
        stm->state != STREAM_STATE_STOPPING &&
        stm->state != STREAM_STATE_SHUTDOWN) {
      res = WRAP(AAudioStream_requestStop)(stm->ostream);
      if (res != AAUDIO_OK) {
        LOG("AAudioStreamBuilder_requestStop: %s", WRAP(AAudio_convertResultToText)(res));
      }
    }

    WRAP(AAudioStream_close)(stm->ostream);
    stm->ostream = NULL;
  }

  if (stm->istream) {
    if (stm->state != STREAM_STATE_STOPPED &&
        stm->state != STREAM_STATE_STOPPING &&
        stm->state != STREAM_STATE_SHUTDOWN) {
      res = WRAP(AAudioStream_requestStop)(stm->istream);
      if (res != AAUDIO_OK) {
        LOG("AAudioStreamBuilder_requestStop: %s", WRAP(AAudio_convertResultToText)(res));
      }
    }

    WRAP(AAudioStream_close)(stm->istream);
    stm->istream = NULL;
  }

  if (stm->resampler) {
    cubeb_resampler_destroy(stm->resampler);
  }
  if (stm->in_buf) {
    free(stm->in_buf);
  }

  atomic_store(&stm->state, STREAM_STATE_INIT);
  pthread_mutex_unlock(&stm->mutex);
  atomic_store(&stm->in_use, false);
}

static int
aaudio_stream_init(cubeb * ctx,
    cubeb_stream ** stream,
    char const * stream_name,
    cubeb_devid input_device,
    cubeb_stream_params * input_stream_params,
    cubeb_devid output_device,
    cubeb_stream_params * output_stream_params,
    unsigned int latency_frames,
    cubeb_data_callback data_callback,
    cubeb_state_callback state_callback,
    void * user_ptr)
{
  assert(!input_device);
  assert(!output_device);

  (void) stream_name;

  aaudio_result_t res;
  AAudioStreamBuilder * sb;
  res = WRAP(AAudio_createStreamBuilder)(&sb);
  if (res != AAUDIO_OK) {
    LOG("AAudio_createStreamBuilder: %s", WRAP(AAudio_convertResultToText)(res));
    return CUBEB_ERROR;
  }

  // atomically find a free stream.
  cubeb_stream * stm = NULL;
  for (unsigned i = 0u; i < MAX_STREAMS; ++i) {
    // This check is only an optimization, we don't strictly need it
    // since we check again after locking the mutex.
    if (atomic_load(&ctx->streams[i].in_use)) {
      continue;
    }

    // if this fails with EBUSY, another thread initialized this stream
    // between our check of in_use and this.
    int err = pthread_mutex_trylock(&ctx->streams[i].mutex);
    if (err != 0 || atomic_load(&ctx->streams[i].in_use)) {
      if (err && err != EBUSY) {
        LOG("pthread_mutex_trylock: %s", strerror(err));
      }

      continue;
    }

    stm = &ctx->streams[i];
    break;
  }

  if (!stm) {
    LOG("Error: maximum number of streams reached");
    return CUBEB_ERROR;
  }

  assert(atomic_load(&stm->state) == STREAM_STATE_INIT);
  atomic_store(&stm->in_use, true);

  unsigned res_err = CUBEB_ERROR;
  stm->user_ptr = user_ptr;
  stm->data_callback = data_callback;
  stm->state_callback = state_callback;
  stm->context = ctx;

  WRAP(AAudioStreamBuilder_setErrorCallback)(sb, aaudio_error_cb, stm);
  WRAP(AAudioStreamBuilder_setBufferCapacityInFrames)(sb, latency_frames);

  AAudioStream_dataCallback in_data_callback;
  AAudioStream_dataCallback out_data_callback;
  if (output_stream_params && input_stream_params) {
    out_data_callback = aaudio_duplex_data_cb;
    in_data_callback = NULL;
  } else if (input_stream_params) {
    in_data_callback = aaudio_input_data_cb;
  } else if (output_stream_params) {
    out_data_callback = aaudio_output_data_cb;
  } else {
    LOG("Tried to open stream without input or output parameters");
    goto error;
  }

#ifdef AAUDIO_EXCLUSIVE
  WRAP(AAudioStreamBuilder_setSharingMode)(sb, AAUDIO_SHARING_MODE_EXCLUSIVE);
#endif

#ifdef AAUDIO_LOW_LATENCY
  WRAP(AAudioStreamBuilder_setPerformanceMode)(sb, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
#elif defined(AAUDIO_POWER_SAVING)
  WRAP(AAudioStreamBuilder_setPerformanceMode)(sb, AAUDIO_PERFORMANCE_MODE_POWER_SAVING);
#endif

  unsigned frame_size;

  // initialize streams
  // output
  uint32_t target_sample_rate = 0;
  cubeb_stream_params out_params;
  if (output_stream_params) {
    WRAP(AAudioStreamBuilder_setDirection)(sb, AAUDIO_DIRECTION_OUTPUT);
    WRAP(AAudioStreamBuilder_setDataCallback)(sb, out_data_callback, stm);
    res_err = realize_stream(sb, output_stream_params, &stm->ostream, &frame_size);
    if (res_err) {
      goto error;
    }

    // output debug information
    aaudio_sharing_mode_t sm = WRAP(AAudioStream_getSharingMode)(stm->ostream);
    aaudio_performance_mode_t pm = WRAP(AAudioStream_getPerformanceMode)(stm->ostream);
    int bcap = WRAP(AAudioStream_getBufferCapacityInFrames)(stm->ostream);
    int bsize = WRAP(AAudioStream_getBufferSizeInFrames)(stm->ostream);
    int rate = WRAP(AAudioStream_getSampleRate)(stm->ostream);
    LOG("AAudio output stream sharing mode: %d", sm);
    LOG("AAudio output stream performance mode: %d", pm);
    LOG("AAudio output stream buffer capacity: %d", bcap);
    LOG("AAudio output stream buffer size: %d", bsize);
    LOG("AAudio output stream buffer rate: %d", rate);

    target_sample_rate = output_stream_params->rate;
    out_params = *output_stream_params;
    out_params.rate = rate;

    stm->out_channels = output_stream_params->channels;
    stm->out_format = output_stream_params->format;
    stm->out_frame_size = frame_size;
    atomic_store(&stm->volume, 1.f);
  }

  // input
  cubeb_stream_params in_params;
  if (input_stream_params) {
    WRAP(AAudioStreamBuilder_setDirection)(sb, AAUDIO_DIRECTION_INPUT);
    WRAP(AAudioStreamBuilder_setDataCallback)(sb, in_data_callback, stm);
    res_err = realize_stream(sb, input_stream_params, &stm->istream, &frame_size);
    if (res_err) {
      goto error;
    }

    // output debug information
    aaudio_sharing_mode_t sm = WRAP(AAudioStream_getSharingMode)(stm->istream);
    aaudio_performance_mode_t pm = WRAP(AAudioStream_getPerformanceMode)(stm->istream);
    int bcap = WRAP(AAudioStream_getBufferCapacityInFrames)(stm->istream);
    int bsize = WRAP(AAudioStream_getBufferSizeInFrames)(stm->istream);
    int rate = WRAP(AAudioStream_getSampleRate)(stm->istream);
    LOG("AAudio input stream sharing mode: %d", sm);
    LOG("AAudio input stream performance mode: %d", pm);
    LOG("AAudio input stream buffer capacity: %d", bcap);
    LOG("AAudio input stream buffer size: %d", bsize);
    LOG("AAudio input stream buffer rate: %d", rate);

    stm->in_buf = malloc(bcap * frame_size);

    // NOTE: not sure about this.
    // But when this stream contains input and output stream, their
    // sample rates have to match, don't they?
    assert(!target_sample_rate || target_sample_rate == input_stream_params->rate);

    target_sample_rate = input_stream_params->rate;
    in_params = *input_stream_params;
    in_params.rate = rate;
    stm->in_frame_size = frame_size;
  }

  // initialize resampler
  stm->resampler = cubeb_resampler_create(stm,
      input_stream_params ? &in_params : NULL,
      output_stream_params ? &out_params : NULL,
      target_sample_rate,
      data_callback,
      user_ptr,
      CUBEB_RESAMPLER_QUALITY_DEFAULT);

  if (!stm->resampler) {
    LOG("Failed to create resampler");
    goto error;
  }

  // the stream isn't started initially. We don't need to differntiate
  // between a stream that was just initialized and one that played
  // already but was stopped
  atomic_store(&stm->state, STREAM_STATE_STOPPED);
  LOG("Cubeb stream (%p) init success", (void*) stm);
  pthread_mutex_unlock(&stm->mutex);

  WRAP(AAudioStreamBuilder_delete)(sb);
  *stream = stm;
  return CUBEB_OK;

error:
  WRAP(AAudioStreamBuilder_delete)(sb);
  pthread_mutex_unlock(&stm->mutex);
  aaudio_stream_destroy(stm);
  return res_err;
}

static int
aaudio_stream_start(cubeb_stream * stm)
{
  pthread_mutex_lock(&stm->mutex);
  enum stream_state state = atomic_load(&stm->state);
  int istate = stm->istream ? WRAP(AAudioStream_getState)(stm->istream) : 0;
  int ostate = stm->ostream ? WRAP(AAudioStream_getState)(stm->ostream) : 0;
  LOGV("starting stream %p: %d (%d %d)", (void*) stm, state, istate, ostate);

  switch (state) {
    case STREAM_STATE_STARTED:
    case STREAM_STATE_STARTING:
      pthread_mutex_unlock(&stm->mutex);
      LOG("cubeb stream %p already starting/started", (void*) stm);
      return CUBEB_OK;
    case STREAM_STATE_ERROR:
    case STREAM_STATE_SHUTDOWN:
      return CUBEB_ERROR;
    case STREAM_STATE_INIT:
      pthread_mutex_unlock(&stm->mutex);
      assert(false && "Invalid stream");
      return CUBEB_ERROR;
    case STREAM_STATE_STOPPED:
    case STREAM_STATE_STOPPING:
    case STREAM_STATE_DRAINING:
      break;
  }

  aaudio_result_t res;

  // NOTE: aaudio docs don't state explicitly if we have to do this or
  // if we are allowed to call requestStart while the stream is
  // in the transient STOPPING state. Works without in testing though
  // if (ostate == AAUDIO_STREAM_STATE_STOPPING) {
  //     res = WRAP(AAudioStream_waitForStateChange)(stm->ostream,
  //       AAUDIO_STREAM_STATE_STOPPING, &ostate, INT64_MAX);
  //     if (res != AAUDIO_OK) {
  //       LOG("AAudioStream_waitForStateChanged: %s", WRAP(AAudio_convertResultToText)(res));
  //     }
  // }
  // if (istate == AAUDIO_STREAM_STATE_STOPPING) {
  //     res = WRAP(AAudioStream_waitForStateChange)(stm->istream,
  //       AAUDIO_STREAM_STATE_STOPPING, &istate, INT64_MAX);
  //     if (res != AAUDIO_OK) {
  //       LOG("AAudioStream_waitForStateChanged: %s", WRAP(AAudio_convertResultToText)(res));
  //     }
  // }

  // Important to start istream before ostream.
  // As soon as we start ostream, the callbacks might be triggered an we
  // might read from istream (on duplex). If istream wasn't started yet
  // this is a problem.
  if (stm->istream) {
    res = WRAP(AAudioStream_requestStart)(stm->istream);
    if (res != AAUDIO_OK) {
      LOG("AAudioStream_requestStart (istream): %s", WRAP(AAudio_convertResultToText)(res));
      atomic_store(&stm->state, STREAM_STATE_ERROR);
      pthread_mutex_unlock(&stm->mutex);
      return CUBEB_ERROR;
    }
  }

  if (stm->ostream) {
    res = WRAP(AAudioStream_requestStart)(stm->ostream);
    if (res != AAUDIO_OK) {
      LOG("AAudioStream_requestStart (ostream): %s", WRAP(AAudio_convertResultToText)(res));
      atomic_store(&stm->state, STREAM_STATE_ERROR);
      pthread_mutex_unlock(&stm->mutex);
      return CUBEB_ERROR;
    }
  }

  int ret = CUBEB_OK;
  bool success;
  while (!(success = atomic_compare_exchange_strong(&stm->state, &state, STREAM_STATE_STARTING))) {
    // we land here only if the state has changed in the meantime
    switch (state) {
      // If an error ocurred in the meantime, we can't change that.
      // The stream will be stopped when shut down.
      case STREAM_STATE_ERROR:
        ret = CUBEB_ERROR;
        break;
      // The only situation in which the state could have switched to draining
      // is if the callback was already fired and requested draining. Don't
      // overwrite that. It's not an error either though.
      case STREAM_STATE_DRAINING:
        break;

      // If the state switched [draining -> stopping] or [draining/stopping -> stopped]
      // in the meantime, we can simply overwrite that since we restarted the stream.
      case STREAM_STATE_STOPPING:
      case STREAM_STATE_STOPPED:
        continue;

      // There is no situation in which the state could have been valid before
      // but now in shutdown mode, since we hold the streams mutex.
      // There is also no way that it switched *into* starting or
      // started mode.
      default:
        assert(false && "Invalid state change");
        ret = CUBEB_ERROR;
        break;
    }

    break;
  }

  if(success) {
    atomic_store(&stm->context->state.waiting, true);
    pthread_cond_signal(&stm->context->state.cond);
  }

  pthread_mutex_unlock(&stm->mutex);
  return ret;
}

static int
aaudio_stream_stop(cubeb_stream * stm)
{
  pthread_mutex_lock(&stm->mutex);
  enum stream_state state = atomic_load(&stm->state);
  int istate = stm->istream ? WRAP(AAudioStream_getState)(stm->istream) : 0;
  int ostate = stm->ostream ? WRAP(AAudioStream_getState)(stm->ostream) : 0;
  LOGV("stopping stream %p: %d (%d %d)", (void*) stm, state, istate, ostate);

  switch (state) {
    case STREAM_STATE_STOPPED:
    case STREAM_STATE_STOPPING:
    case STREAM_STATE_DRAINING:
      pthread_mutex_unlock(&stm->mutex);
      LOG("cubeb stream %p already stopping/stopped", (void*) stm);
      return CUBEB_OK;
    case STREAM_STATE_ERROR:
    case STREAM_STATE_SHUTDOWN:
      pthread_mutex_unlock(&stm->mutex);
      return CUBEB_ERROR;
    case STREAM_STATE_INIT:
      pthread_mutex_unlock(&stm->mutex);
      assert(false && "Invalid stream");
      return CUBEB_ERROR;
    case STREAM_STATE_STARTED:
    case STREAM_STATE_STARTING:
      break;
  }

  aaudio_result_t res;

  // NOTE: aaudio docs don't state explicitly if we have to do this or
  // if we are allowed to call requestStop while the stream is
  // in the transient STARTING state. Works without in testing though.
  // if (ostate == AAUDIO_STREAM_STATE_STARTING) {
  //     res = WRAP(AAudioStream_waitForStateChange)(stm->ostream,
  //       AAUDIO_STREAM_STATE_STARTING, &ostate, INT64_MAX);
  //     if (res != AAUDIO_OK) {
  //       LOG("AAudioStream_waitForStateChanged: %s", WRAP(AAudio_convertResultToText)(res));
  //     }
  // }
  // if (istate == AAUDIO_STREAM_STATE_STARTING) {
  //     res = WRAP(AAudioStream_waitForStateChange)(stm->istream,
  //       AAUDIO_STREAM_STATE_STARTING, &istate, INT64_MAX);
  //     if (res != AAUDIO_OK) {
  //       LOG("AAudioStream_waitForStateChanged: %s", WRAP(AAudio_convertResultToText)(res));
  //     }
  // }

  // No callbacks are triggered anymore when requestStop returns.
  // That is important as we otherwise might read from a closed istream
  // for a duplex stream.
  // Therefor it is important to close ostream first.
  if (stm->ostream) {
    // Could use pause + flush here as well, the public cubeb interface
    // doesn't state behavior.
    res = WRAP(AAudioStream_requestStop)(stm->ostream);
    if (res != AAUDIO_OK) {
      LOG("AAudioStream_requestStop (ostream): %s", WRAP(AAudio_convertResultToText)(res));
      atomic_store(&stm->state, STREAM_STATE_ERROR);
      pthread_mutex_unlock(&stm->mutex);
      return CUBEB_ERROR;
    }
  }

  if (stm->istream) {
    res = WRAP(AAudioStream_requestStop)(stm->istream);
    if (res != AAUDIO_OK) {
      LOG("AAudioStream_requestStop (istream): %s", WRAP(AAudio_convertResultToText)(res));
      atomic_store(&stm->state, STREAM_STATE_ERROR);
      pthread_mutex_unlock(&stm->mutex);
      return CUBEB_ERROR;
    }
  }

  int ret = CUBEB_OK;
  bool success;
  while (!(success = atomic_compare_exchange_strong(&stm->state, &state, STREAM_STATE_STOPPING))) {
    // we land here only if the state has changed in the meantime
    switch (state) {
      // If an error ocurred in the meantime, we can't change that.
      // The stream will be stopped when shut down.
      case STREAM_STATE_ERROR:
        ret = CUBEB_ERROR;
        break;
      // If it was switched to draining in the meantime, it was or
      // will be stopped soon anyways. We don't interfere with
      // the draining process, no matter in which state.
      // Not an error
      case STREAM_STATE_DRAINING:
      case STREAM_STATE_STOPPING:
      case STREAM_STATE_STOPPED:
        break;

      // If the state switched from starting to started in the meantime
      // we can simply overwrite that since we just stopped it.
      case STREAM_STATE_STARTED:
        continue;

      // There is no situation in which the state could have been valid before
      // but now in shutdown mode, since we hold the streams mutex.
      // There is also no way that it switched *into* starting mode.
      default:
        assert(false && "Invalid state change");
        ret = CUBEB_ERROR;
        break;
    }

    break;
  }

  if(success) {
    atomic_store(&stm->context->state.waiting, true);
    pthread_cond_signal(&stm->context->state.cond);
  }

  pthread_mutex_unlock(&stm->mutex);
  return ret;
}

static int
aaudio_stream_get_position(cubeb_stream * stm, uint64_t * position)
{
  pthread_mutex_lock(&stm->mutex);
  enum stream_state state = atomic_load(&stm->state);
  AAudioStream * stream = stm->ostream ? stm->ostream : stm->istream;
  switch (state) {
    case STREAM_STATE_ERROR:
    case STREAM_STATE_SHUTDOWN:
      pthread_mutex_unlock(&stm->mutex);
      return CUBEB_ERROR;
    case STREAM_STATE_STOPPED:
    case STREAM_STATE_DRAINING:
    case STREAM_STATE_STOPPING:
      // getTimestamp is only valid when the stream is playing.
      // Simply return the number of frames passed to aaudio
      *position = WRAP(AAudioStream_getFramesRead)(stream);
      pthread_mutex_unlock(&stm->mutex);
      return CUBEB_OK;
    case STREAM_STATE_INIT:
      assert(false && "Invalid stream");
      pthread_mutex_unlock(&stm->mutex);
      return CUBEB_ERROR;
    case STREAM_STATE_STARTED:
    case STREAM_STATE_STARTING:
      break;
  }

  int64_t pos;
  int64_t ns;
  aaudio_result_t res;
  res = WRAP(AAudioStream_getTimestamp)(stream, CLOCK_MONOTONIC, &pos, &ns);
  if (res != AAUDIO_OK) {
    // when we are in 'starting' state we try it and hope that the stream
    // has internally started and gives us a valid timestamp.
    // If that is not the case (invalid_state is returned) we simply
    // fall back to the method we use for non-playing streams.
    if (res == AAUDIO_ERROR_INVALID_STATE && state == STREAM_STATE_STARTING) {
      *position = WRAP(AAudioStream_getFramesRead)(stream);
      pthread_mutex_unlock(&stm->mutex);
      return CUBEB_OK;
    }

    LOG("AAudioStream_getTimestamp: %s", WRAP(AAudio_convertResultToText)(res));
    pthread_mutex_unlock(&stm->mutex);
    return CUBEB_ERROR;
  }

  pthread_mutex_unlock(&stm->mutex);
  *position = pos;
  return CUBEB_OK;
}

static int
aaudio_stream_set_volume(cubeb_stream * stm, float volume)
{
  assert(stm && stm->ostream);
  atomic_store(&stm->volume, volume);
  return CUBEB_OK;
}

static const struct cubeb_ops aaudio_ops = {
  .init = aaudio_init,
  .get_backend_id = aaudio_get_backend_id,
  .get_max_channel_count = aaudio_get_max_channel_count,
  // NOTE: i guess we could support min_latency and preferred sample
  // rate via guessing, i.e. creating a dummy stream and check
  // its settings.
  .get_min_latency = NULL,
  .get_preferred_sample_rate = NULL,
  .enumerate_devices = NULL,
  .device_collection_destroy = NULL,
  .destroy = aaudio_destroy,
  .stream_init = aaudio_stream_init,
  .stream_destroy = aaudio_stream_destroy,
  .stream_start = aaudio_stream_start,
  .stream_stop = aaudio_stream_stop,
  .stream_reset_default_device = NULL,
  .stream_get_position = aaudio_stream_get_position,
  // NOTE: this could be implemented via means comparable to the
  // OpenSLES backend
  .stream_get_latency = NULL,
  .stream_set_volume = aaudio_stream_set_volume,
  .stream_get_current_device = NULL,
  .stream_device_destroy = NULL,
  .stream_register_device_changed_callback = NULL,
  .register_device_collection_changed = NULL
};

