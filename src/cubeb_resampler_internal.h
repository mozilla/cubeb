/*
 * Copyright © 2016 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

#if !defined(CUBEB_RESAMPLER_INTERNAL)
#define CUBEB_RESAMPLER_INTERNAL

#include <cmath>
#include <cassert>
#include <algorithm>
#include "cubeb/cubeb.h"
#include "cubeb_utils.h"
#include "cubeb-speex-resampler.h"
#include "cubeb_resampler.h"
#include <stdio.h>

/* This header file contains the internal C++ API of the resamplers, for testing. */

int to_speex_quality(cubeb_resampler_quality q);

template<typename T>
class cubeb_resampler_speex_one_way;

struct cubeb_resampler {
  virtual long fill(void * input_buffer, long * input_frames_count,
                    void * output_buffer, long frames_needed) = 0;
  virtual long latency() = 0;
  virtual ~cubeb_resampler() {}
};

class noop_resampler : public cubeb_resampler {
public:
  noop_resampler(cubeb_stream * s,
                 cubeb_data_callback cb,
                 void * ptr)
    : stream(s)
    , data_callback(cb)
    , user_ptr(ptr)
  {
  }

  virtual long fill(void * input_buffer, long * input_frames_count,
                    void * output_buffer, long output_frames);

  virtual long latency()
  {
    return 0;
  }

private:
  cubeb_stream * const stream;
  const cubeb_data_callback data_callback;
  void * const user_ptr;
};

/** Base class for processors. This is just used to share methods for now. */
class processor {
public:
  processor(uint32_t channels)
    : channels(channels)
  {}
protected:
  size_t frames_to_samples(size_t frames)
  {
    return frames * channels;
  }
  size_t samples_to_frames(size_t samples)
  {
    assert(!(samples % channels));
    return samples / channels;
  }
  /** The number of channel of the audio buffers to be resampled. */
  const uint32_t channels;
};

/** Handles one way of a (possibly) duplex resampler, working on interleaved
 * audio buffers of type T. This class is designed so that the number of frames
 * coming out of the resampler can be precisely controled. It manages its own
 * input buffer, and can use the caller's output buffer, or allocate its own. */
template<typename T>
class cubeb_resampler_speex_one_way : public processor {
public:
  /** The sample type of this resampler, either 16-bit integers or 32-bit
   * floats. */
  typedef T sample_type;
  /** Construct a resampler resampling from #source_rate to #target_rate, that
   * can be arbitrary, strictly positive number.
   * @parameter channels The number of channels this resampler will resample.
   * @parameter source_rate The sample-rate of the audio input.
   * @parameter target_rate The sample-rate of the audio output.
   * @parameter quality A number between 0 (fast, low quality) and 10 (slow,
   * high quality). */
  cubeb_resampler_speex_one_way(int32_t channels,
                                int32_t source_rate,
                                int32_t target_rate,
                                int quality);

  /** Destructor, deallocate the resampler */
  virtual ~cubeb_resampler_speex_one_way();

  /** Sometimes, it is necessary to add latency on one way of a two-way
   * resampler so that the stream are synchronized. This must be called only on
   * a fresh resampler, otherwise, silent samples will be inserted in the
   * stream.
   * @param frames the number of frames of latency to add. */
  void add_latency(size_t frames)
  {
    additional_latency += frames;
    resampling_in_buffer.push(frames_to_samples(frames));
  }

  /* Fill the resampler with `input_frame_count` frames. */
  void input(T * input_buffer, size_t input_frame_count)
  {
    resampling_in_buffer.push(input_buffer,
                              frames_to_samples(input_frame_count));
  }

  /** Outputs exactly `output_frame_count` into `output_buffer`.
    * `output_buffer` has to be at least `output_frame_count` long. */
  void output(T * output_buffer, size_t output_frame_count)
  {
    uint32_t in_len = samples_to_frames(resampling_in_buffer.length());
    uint32_t out_len = output_frame_count;

    speex_resample(resampling_in_buffer.data(), &in_len,
                   output_buffer, &out_len);

    assert(out_len == output_frame_count);

    /* This shifts back any unresampled samples to the beginning of the input
       buffer. */
    resampling_in_buffer.pop(nullptr, frames_to_samples(in_len));
  }

  /** Drains the resampler, emptying the input buffer, and returning the number
   * of frames written to `output_buffer`, that can be less than
   * `output_frame_count`. */
  size_t drain(T * output_buffer, size_t output_frame_count)
  {
    uint32_t in_len = samples_to_frames(resampling_in_buffer.length());
    uint32_t out_len = output_frame_count;

    speex_resample(resampling_in_buffer.data(), &in_len,
                   output_buffer, &out_len);

    /* This shifts back any unresampled samples to the beginning of the input
       buffer. */
    resampling_in_buffer.pop(nullptr, frames_to_samples(in_len));

    // assert(resampling_in_buffer.length() == 0);

    return out_len;
  }

  /** Returns a buffer containing exactly `output_frame_count` resampled frames.
    * The consumer should not hold onto the pointer. */
  T * output(size_t output_frame_count)
  {
    if (resampling_out_buffer.capacity() < frames_to_samples(output_frame_count)) {
      resampling_out_buffer.resize(frames_to_samples(output_frame_count));
    }

    uint32_t in_len = samples_to_frames(resampling_in_buffer.length());
    uint32_t out_len = output_frame_count;

    speex_resample(resampling_in_buffer.data(), &in_len,
                   resampling_out_buffer.data(), &out_len);

    assert(out_len == output_frame_count);

    /* This shifts back any unresampled samples to the beginning of the input
       buffer. */
    resampling_in_buffer.pop(nullptr, frames_to_samples(in_len));

    return resampling_out_buffer.data();
  }

  /** Get the l atency of the resampler, in output frames. */
  uint32_t latency() const
  {
    /* The documentation of the resampler talks about "samples" here, but it
     * only consider a single channel here so it's the same number of frames. */
    return speex_resampler_get_output_latency(speex_resampler) + additional_latency;
  }

  /** Returns the number of frames to pass in the input of the resampler to have
   * exactly `output_frame_count` resampled frames. This can return a number
   * slightly bigger than what is strictly necessary, but it guaranteed that the
   * number of output frames will be exactly equal. */
  uint32_t input_needed_for_output(uint32_t output_frame_count)
  {
    return ceil(output_frame_count * resampling_ratio) + 1
            - resampling_in_buffer.length() / channels;
  }

  /** Returns a pointer to the input buffer, that contains empty space for at
   * least `frame_count` elements. This is useful so that consumer can directly
   * write into the input buffer of the resampler. The pointer returned is
   * adjusted so that leftover data are not overwritten.
   */
  T * input_buffer(size_t frame_count)
  {
    size_t prev_length = resampling_in_buffer.length();
    resampling_in_buffer.push(frames_to_samples(frame_count));
    return resampling_in_buffer.data() + prev_length;
  }
private:
  /** Wrapper for the speex resampling functions to have a typed
    * interface. */
  void speex_resample(float * input_buffer, uint32_t * input_frame_count,
                      float * output_buffer, uint32_t * output_frame_count)
  {
    speex_resampler_process_interleaved_float(speex_resampler,
                                              input_buffer, input_frame_count,
                                              output_buffer, output_frame_count);
  }

  void speex_resample(short * input_buffer, uint32_t * input_frame_count,
                      short * output_buffer, uint32_t * output_frame_count)
  {
    speex_resampler_process_interleaved_int(speex_resampler,
                                            input_buffer, input_frame_count,
                                            output_buffer, output_frame_count);
  }
  /** The state for the speex resampler used internaly. */
  SpeexResamplerState * speex_resampler;
  /** Source rate / target rate. */
  const float resampling_ratio;
  /** Storage for the input frames, to be resampled. Also contains
   * any unresampled frames after resampling. */
  auto_array<T> resampling_in_buffer;
  /* Storage for the resampled frames, to be passed back to the caller. */
  auto_array<T> resampling_out_buffer;
  /** Additional latency inserted into the pipeline for synchronisation. */
  uint32_t additional_latency;
};

#endif /* CUBEB_RESAMPLER_INTERNAL */
