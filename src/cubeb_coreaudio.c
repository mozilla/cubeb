/*
 * Copyright © 2011 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <AudioToolbox/AudioToolbox.h>
#include "cubeb/cubeb.h"

#if !defined(MAC_OS_X_VERSION_10_6) || MAC_OS_X_VERSION_MAX_ALLOWED < MAC_OS_X_VERSION_10_6
enum {
  kAudioQueueErr_EnqueueDuringReset = -66632
};
#endif

#define NBUFS 4

struct cubeb_stream {
  AudioQueueRef queue;
  AudioQueueBufferRef buffers[NBUFS];
  cubeb_data_callback data_callback;
  cubeb_state_callback state_callback;
  void * user_ptr;
  AudioStreamBasicDescription sample_spec;
  int shutdown;
  int draining;
  int free_buffers;
};

static void
audio_queue_listener_callback(void * userptr, AudioQueueRef queue, AudioQueuePropertyID id)
{
  cubeb_stream * stm;
  OSStatus rv;
  UInt32 playing, playing_size;

  stm = userptr;

  assert(id == kAudioQueueProperty_IsRunning);

  playing_size = sizeof(playing);
  rv = AudioQueueGetProperty(queue, kAudioQueueProperty_IsRunning, &playing, &playing_size);
  assert(rv == 0);

  if (stm->draining && !playing) {
    stm->draining = 0;
    stm->shutdown = 1;
    stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_DRAINED);
  }
}

static void
audio_queue_output_callback(void * userptr, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
  cubeb_stream * stm;
  long got;
  OSStatus rv;

  stm = userptr;

  stm->free_buffers += 1;
  assert(stm->free_buffers > 0 && stm->free_buffers <= NBUFS);

  if (stm->draining || stm->shutdown)
    return;

  got = stm->data_callback(stm, stm->user_ptr, buffer->mAudioData,
                           buffer->mAudioDataBytesCapacity / stm->sample_spec.mBytesPerFrame);
  fprintf(stderr, "%p: %p (%p %u) got %ld\n", stm, buffer, buffer->mAudioData, 
buffer->mAudioDataByteSize, got);
  if (got < 0) {
    // XXX handle this case.
    assert(false);
    return;
  }

  buffer->mAudioDataByteSize = got * stm->sample_spec.mBytesPerFrame;
  if (got > 0) {
    rv = AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
    assert(rv == kAudioQueueErr_EnqueueDuringReset || rv == 0);
    stm->free_buffers -= 1;
    assert(stm->free_buffers >= 0);
  }

  if (got < buffer->mAudioDataBytesCapacity / stm->sample_spec.mBytesPerFrame) {
    stm->draining = 1;
    fprintf(stderr, "%p: stop queue %p (draining)\n", stm, stm->queue);
    rv = AudioQueueStop(queue, false);
    assert(rv == 0);
  }
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
  AudioStreamBasicDescription ss;
  cubeb_stream * stm;
  unsigned int buffer_size;
  OSStatus r;
  int i;

  memset(&ss, 0, sizeof(ss));
  ss.mFormatFlags = kAudioFormatFlagsAreAllClear;

  switch (stream_params.format) {
  case CUBEB_SAMPLE_S16LE:
    ss.mBitsPerChannel = 16;
    ss.mFormatFlags |= kAudioFormatFlagIsSignedInteger;
    break;
  case CUBEB_SAMPLE_FLOAT32LE:
    ss.mBitsPerChannel = 32;
    ss.mFormatFlags |= kAudioFormatFlagIsFloat;
    break;
  default:
    return CUBEB_ERROR_INVALID_FORMAT;
  }

  ss.mFormatID = kAudioFormatLinearPCM;
  ss.mFormatFlags |= kLinearPCMFormatFlagIsPacked;
  ss.mSampleRate = stream_params.rate;
  ss.mChannelsPerFrame = stream_params.channels;

  ss.mBytesPerFrame = (ss.mBitsPerChannel / 8) * ss.mChannelsPerFrame;
  ss.mFramesPerPacket = 1;
  ss.mBytesPerPacket = ss.mBytesPerFrame * ss.mFramesPerPacket;

  stm = calloc(1, sizeof(*stm));
  assert(stm);

  stm->data_callback = data_callback;
  stm->state_callback = state_callback;
  stm->user_ptr = user_ptr;

  stm->sample_spec = ss;

  r = AudioQueueNewOutput(&stm->sample_spec, audio_queue_output_callback,
                          stm, NULL, NULL, 0, &stm->queue);
  assert(r == 0);
  fprintf(stderr, "%p: new queue %p\n", stm, stm->queue);

  r = AudioQueueAddPropertyListener(stm->queue, kAudioQueueProperty_IsRunning,
                                    audio_queue_listener_callback, stm);
  assert(r == 0);

  buffer_size = ss.mSampleRate / 1000.0 * latency * ss.mBytesPerFrame / NBUFS;
  if (buffer_size % ss.mBytesPerFrame != 0) {
    buffer_size += ss.mBytesPerFrame - (buffer_size % ss.mBytesPerFrame);
  }
  assert(buffer_size % ss.mBytesPerFrame == 0);

  for (i = 0; i < NBUFS; ++i) {
    r = AudioQueueAllocateBuffer(stm->queue, buffer_size, &stm->buffers[i]);
    assert(r == 0);
    fprintf(stderr, "%p: %d: %p (%p %u)\n", stm, i, stm->buffers[i], stm->buffers[i]->mAudioData, buffer_size);

    audio_queue_output_callback(stm, stm->queue, stm->buffers[i]);
  }

  *stream = stm;

  return CUBEB_OK;
}

void
cubeb_stream_destroy(cubeb_stream * stm)
{
  OSStatus r;

  fprintf(stderr, "%p: stop queue %p (d=%d)\n", stm, stm->queue, stm->draining);

  r = AudioQueueRemovePropertyListener(stm->queue, kAudioQueueProperty_IsRunning,
                                       audio_queue_listener_callback, stm);
  assert(r == 0);

  stm->shutdown = 1;

  r = AudioQueueStop(stm->queue, true);
  assert(r == 0);

  assert(stm->free_buffers == NBUFS);

  r = AudioQueueDispose(stm->queue, true);
  assert(r == 0);

  free(stm);
}

int
cubeb_stream_start(cubeb_stream * stm)
{
  OSStatus r;
  fprintf(stderr, "%p: start queue %p\n", stm, stm->queue);
  r = AudioQueueStart(stm->queue, NULL);
  assert(r == 0);
  stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_STARTED);
  return CUBEB_OK;
}

int
cubeb_stream_stop(cubeb_stream * stm)
{
  OSStatus r;
  fprintf(stderr, "%p: pause queue %p\n", stm, stm->queue);
  r = AudioQueuePause(stm->queue);
  assert(r == 0);
  stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_STOPPED);
  return CUBEB_OK;
}

int
cubeb_stream_get_position(cubeb_stream * stm, uint64_t * position)
{
  AudioTimeStamp tstamp;
  OSStatus r = AudioQueueGetCurrentTime(stm->queue, NULL, &tstamp, NULL);
  if (r == kAudioQueueErr_InvalidRunState) {
    *position = 0;
    return CUBEB_OK;
  } else if (r != 0) {
    return CUBEB_ERROR;
  }
  assert(tstamp.mFlags & kAudioTimeStampSampleTimeValid);
  *position = tstamp.mSampleTime;
  /* XXX need to investigate why GetCurrentTime returns a "valid" negative time */
  if (tstamp.mSampleTime < 0) {
    *position = 0;
  }
  return CUBEB_OK;
}

