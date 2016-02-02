/*
 * Copyright © 2014 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#include <cmath>
#include <cassert>
#include <cstring>
#include <cstddef>
#include <cstdio>
#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif
#include "cubeb_resampler.h"
#include "cubeb-speex-resampler.h"
#include "cubeb_resampler_internal.h"
#include "cubeb_utils.h"

int
to_speex_quality(cubeb_resampler_quality q)
{
  switch(q) {
  case CUBEB_RESAMPLER_QUALITY_VOIP:
    return SPEEX_RESAMPLER_QUALITY_VOIP;
  case CUBEB_RESAMPLER_QUALITY_DEFAULT:
    return SPEEX_RESAMPLER_QUALITY_DEFAULT;
  case CUBEB_RESAMPLER_QUALITY_DESKTOP:
    return SPEEX_RESAMPLER_QUALITY_DESKTOP;
  default:
    assert(false);
    return 0XFFFFFFFF;
  }
}

template<typename T>
cubeb_resampler_speex_one_way<T>::cubeb_resampler_speex_one_way(int32_t channels,
                                                                int32_t source_rate,
                                                                int32_t target_rate,
                                                                int quality)
  : processor(channels)
  , resampling_ratio(static_cast<float>(source_rate) / target_rate)
  , additional_latency(0)
{
  int r;
  speex_resampler = speex_resampler_init(channels, source_rate,
                                         target_rate, quality, &r);
  assert(r == RESAMPLER_ERR_SUCCESS && "resampler allocation failure");
}

template<typename T>
cubeb_resampler_speex_one_way<T>::~cubeb_resampler_speex_one_way()
{
  speex_resampler_destroy(speex_resampler);
}

long noop_resampler::fill(void * input_buffer, long * input_frames_count,
                          void * output_buffer, long output_frames)
{
  assert(input_buffer && output_buffer &&
         *input_frames_count >= output_frames||
         !input_buffer && input_frames_count == 0 ||
         !output_buffer && output_frames== 0);

  if (*input_frames_count != output_frames) {
    assert(*input_frames_count > output_frames);
    *input_frames_count = output_frames;
  }

  return data_callback(stream, user_ptr,
                       input_buffer, output_buffer, output_frames);
}

namespace {

long
frame_count_at_rate(long frame_count, float rate)
{
  return static_cast<long>(ceilf(rate * frame_count) + 1);
}

} // end of anonymous namespace

cubeb_resampler_speex::cubeb_resampler_speex(SpeexResamplerState * r,
                                             cubeb_stream * s,
                                             cubeb_stream_params params,
                                             uint32_t out_rate,
                                             cubeb_data_callback cb,
                                             long max_count,
                                             void * ptr)
  : speex_resampler(r)
  , stream(s)
  , stream_params(params)
  , data_callback(cb)
  , user_ptr(ptr)
  , buffer_frame_count(max_count)
  , resampling_ratio(static_cast<float>(params.rate) / out_rate)
  , leftover_frame_size(static_cast<uint32_t>(ceilf(1 / resampling_ratio * 2) + 1))
  , leftover_frame_count(0)
  , leftover_frames_buffer(auto_array<uint8_t>(frames_to_bytes(params, leftover_frame_size)))
  , resampling_src_buffer(auto_array<uint8_t>(frames_to_bytes(params,
        frame_count_at_rate(buffer_frame_count, resampling_ratio))))
{
  assert(r);
}

cubeb_resampler_speex::~cubeb_resampler_speex()
{
  speex_resampler_destroy(speex_resampler);
}

long
cubeb_resampler_speex::fill(void * input_buffer, void * output_buffer, long frames_needed)
{
  // Use more input frames than strictly necessary, so in the worst case,
  // we have leftover unresampled frames at the end, that we can use
  // during the next iteration.
  assert(frames_needed <= buffer_frame_count);
  long before_resampling = frame_count_at_rate(frames_needed, resampling_ratio);
  long frames_requested = before_resampling - leftover_frame_count;

  // Copy the previous leftover frames to the front of the buffer.
  size_t leftover_bytes = frames_to_bytes(stream_params, leftover_frame_count);
  memcpy(resampling_src_buffer.get(), leftover_frames_buffer.get(), leftover_bytes);
  uint8_t * buffer_start = resampling_src_buffer.get() + leftover_bytes;

  long got = data_callback(stream, user_ptr, NULL, buffer_start, frames_requested);
  assert(got <= frames_requested);

  if (got < 0) {
    return CUBEB_ERROR;
  }

  uint32_t in_frames = leftover_frame_count + got;
  uint32_t out_frames = frames_needed;
  uint32_t old_in_frames = in_frames;

  if (stream_params.format == CUBEB_SAMPLE_FLOAT32NE) {
    float * in_buffer = reinterpret_cast<float *>(resampling_src_buffer.get());
    float * out_buffer = reinterpret_cast<float *>(output_buffer);
    speex_resampler_process_interleaved_float(speex_resampler, in_buffer, &in_frames,
                                              out_buffer, &out_frames);
  } else {
    short * in_buffer = reinterpret_cast<short *>(resampling_src_buffer.get());
    short * out_buffer = reinterpret_cast<short *>(output_buffer);
    speex_resampler_process_interleaved_int(speex_resampler, in_buffer, &in_frames,
                                            out_buffer, &out_frames);
  }

  // Copy the leftover frames to buffer for the next time.
  leftover_frame_count = old_in_frames - in_frames;
  assert(leftover_frame_count <= leftover_frame_size);

  size_t unresampled_bytes = frames_to_bytes(stream_params, leftover_frame_count);
  uint8_t * leftover_frames_start = resampling_src_buffer.get();
  leftover_frames_start += frames_to_bytes(stream_params, in_frames);
  memcpy(leftover_frames_buffer.get(), leftover_frames_start, unresampled_bytes);

  return out_frames;
}

cubeb_resampler *
cubeb_resampler_create(cubeb_stream * stream,
                       cubeb_stream_params params,
                       unsigned int out_rate,
                       cubeb_data_callback callback,
                       long buffer_frame_count,
                       void * user_ptr,
                       cubeb_resampler_quality quality)
{
  if (params.rate != out_rate) {
    SpeexResamplerState * resampler = NULL;
    resampler = speex_resampler_init(params.channels,
                                     params.rate,
                                     out_rate,
                                     to_speex_quality(quality),
                                     NULL);
    if (!resampler) {
      return NULL;
    }

    return new cubeb_resampler_speex(resampler, stream, params, out_rate,
                                     callback, buffer_frame_count, user_ptr);
  }

  return new noop_resampler(stream, callback, user_ptr);
}

long
cubeb_resampler_fill(cubeb_resampler * resampler,
                     void * input_buffer,
                     void * output_buffer,
                     long frames_needed)
{
  return resampler->fill(input_buffer, output_buffer, frames_needed);
}

void
cubeb_resampler_destroy(cubeb_resampler * resampler)
{
  delete resampler;
}
