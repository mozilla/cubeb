/*
 * Copyright © 2016 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

#define OUTSIDE_SPEEX
#define RANDOM_PREFIX speex

#include "cubeb/cubeb.h"
#include "cubeb_utils.h"
#include "cubeb_resampler.h"
#include "cubeb_resampler_internal.h"
#include <assert.h>
#include <stdio.h>
#include <algorithm>
#include <iostream>

/* Windows cmath USE_MATH_DEFINE thing... */
const float PI = 3.14159265359f;
/* Some standard sample rates we're testing with. */
const int sample_rates[] = {
    8000,
   16000,
   32000,
   44100,
   48000,
   88200,
   96000,
  192000
};
/* The maximum number of channels we're resampling. */
const uint32_t max_channels = 2;
/* The minimum an maximum number of milliseconds we're resampling for. This is
 * used to simulate the fact that the audio stream is resampled in chunks,
 * because audio is delivered using callbacks. */
const uint32_t min_chunks = 10; /* ms */
const uint32_t max_chunks = 30; /* ms */

#define DUMP_ARRAYS
#ifdef DUMP_ARRAYS
/**
 * Files produced by dump(...) can be converted to .wave files using:
 *
 * sox -c <channel_count> -r <rate> -e float -b 32  file.raw file.wav
 *
 * for floating-point audio, or:
 *
 * sox -c <channel_count> -r <rate> -e unsigned -b 16  file.raw file.wav
 *
 * for 16bit integer audio.
 */

/* Use the correct implementation of fopen, depending on the platform. */
void fopen_portable(FILE ** f, const char * name, const char * mode)
{
#ifdef WIN32
  fopen_s(f, name, mode);
#else
  *f = fopen(name, mode);
#endif
}

template<typename T>
void dump(const char * name, T * frames, size_t count)
{
  FILE * file;
  fopen_portable(&file, name, "wb");

  if (!file) {
    fprintf(stderr, "error opening %s\n", name);
    return;
  }

  if (count != fwrite(frames, sizeof(T), count, file)) {
    fprintf(stderr, "error writing to %s\n", name);
    return;
  }
  fclose(file);
}
#else
template<typename T>
void dump(const char * name, T * frames, size_t count)
{ }
#endif

// The more the ratio is far from 1, the more we accept a big error.
float epsilon_tweak_ratio(float ratio)
{
  return ratio >= 1 ? ratio : 1 / ratio;
}

// Epsilon values for comparing resampled data to expected data.
// The bigger the resampling ratio is, the more lax we are about errors.
template<typename T>
T epsilon(float ratio);

template<>
float epsilon(float ratio) {
  return 0.08f * epsilon_tweak_ratio(ratio);
}

template<>
int16_t epsilon(float ratio) {
  return static_cast<int16_t>(10 * epsilon_tweak_ratio(ratio));
}

void test_delay_lines(uint32_t delay_frames, uint32_t channels, uint32_t chunk_ms)
{
  const size_t length_s = 2;
  const size_t rate = 44100;
  const size_t length_frames = rate * length_s;
  delay_line<float> delay(delay_frames, channels);
  auto_array<float> input;
  auto_array<float> output;
  uint32_t chunk_length = channels * chunk_ms * rate / 1000;
  uint32_t output_offset = 0;
  uint32_t channel = 0;

  /** Generate diracs every 100 frames, and check they are delayed. */
  input.push(length_frames * channels);
  for (uint32_t i = 0; i < input.length() - 1; i+=100) {
    input.data()[i + channel] = 0.5;
    channel = (channel + 1) % channels;
  }
  dump("input.raw", input.data(), input.length());
  while(input.length()) {
    uint32_t to_pop = std::min<uint32_t>(input.length(), chunk_length * channels);
    float * in = delay.input_buffer(to_pop / channels);
    input.pop(in, to_pop);
    output.push(to_pop);
    delay.output(output.data() + output_offset, to_pop / channels);
    output_offset += to_pop;
  }

  // Check the diracs have been shifted by `delay_frames` frames.
  for (uint32_t i = 0; i < output.length() - delay_frames * channels + 1; i+=100) {
    assert(output.data()[i + channel + delay_frames * channels] == 0.5);
    channel = (channel + 1) % channels;
  }

  dump("output.raw", output.data(), output.length());
}
/**
 * This takes sine waves with a certain `channels` count, `source_rate`, and
 * resample them, by chunk of `chunk_duration` milliseconds, to `target_rate`.
 * Then a sample-wise comparison is performed against a sine wave generated at
 * the correct rate.
 */
template<typename T>
void test_resampler_one_way(uint32_t channels, int32_t source_rate, int32_t target_rate, float chunk_duration)
{
  size_t chunk_duration_in_source_frames = static_cast<uint32_t>(ceil(chunk_duration * source_rate / 1000.));
  float resampling_ratio = static_cast<float>(source_rate) / target_rate;
  cubeb_resampler_speex_one_way<T> resampler(channels, source_rate, target_rate, 3);
  auto_array<T> source(channels * source_rate * 10);
  auto_array<T> destination(channels * target_rate * 10);
  auto_array<T> expected(channels * target_rate * 10);
  uint32_t phase_index = 0;
  uint32_t offset = 0;
  const uint32_t buf_len = 2; /* seconds */

  // generate a sine wave in each channel, at the source sample rate
  source.push(channels * source_rate * buf_len);
  while(offset != source.length()) {
    float  p = phase_index++ / static_cast<float>(source_rate);
    for (uint32_t j = 0; j < channels; j++) {
      source.data()[offset++] = 0.5 * sin(440. * 2 * PI * p);
    }
  }

  dump("input.raw", source.data(), source.length());

  expected.push(channels * target_rate * buf_len);
  // generate a sine wave in each channel, at the target sample rate.
  // Insert silent samples at the beginning to account for the resampler latency.
  offset = resampler.latency() * channels;
  for (uint32_t i = 0; i < offset; i++) {
    expected.data()[i] = 0.0f;
  }
  phase_index = 0;
  while (offset != expected.length()) {
    float  p = phase_index++ / static_cast<float>(target_rate);
    for (uint32_t j = 0; j < channels; j++) {
      expected.data()[offset++] = 0.5 * sin(440. * 2 * PI * p);
    }
  }

  dump("expected.raw", expected.data(), expected.length());

  // resample by chunk
  uint32_t write_offset = 0;
  destination.push(channels * target_rate * buf_len);
  while (write_offset < destination.length())
  {
    size_t output_frames = static_cast<uint32_t>(floor(chunk_duration_in_source_frames / resampling_ratio));
    uint32_t input_frames = resampler.input_needed_for_output(output_frames);
    resampler.input(source.data(), input_frames);
    source.pop(nullptr, input_frames * channels);
    resampler.output(destination.data() + write_offset,
                     std::min(output_frames, (destination.length() - write_offset) / channels));
    write_offset += output_frames * channels;
  }

  dump("output.raw", destination.data(), expected.length());

  // compare, taking the latency into account
  bool fuzzy_equal = true;
  for (uint32_t i = resampler.latency() + 1; i < expected.length(); i++) {
    float diff = abs(expected.data()[i] - destination.data()[i]);
    if (diff > epsilon<T>(resampling_ratio)) {
      fprintf(stderr, "divergence at %d: %f %f (delta %f)\n", i, expected.data()[i], destination.data()[i], diff);
      fuzzy_equal = false;
    }
  }
  assert(fuzzy_equal);
}

template<typename T>
cubeb_sample_format cubeb_format();

template<>
cubeb_sample_format cubeb_format<float>()
{
  return CUBEB_SAMPLE_FLOAT32NE;
}

template<>
cubeb_sample_format cubeb_format<short>()
{
  return CUBEB_SAMPLE_S16NE;
}


#define array_size(x) (sizeof(x) / sizeof(x[0]))

void test_resamplers_one_way()
{
  /* Test one way resamplers */
  for (uint32_t channels = 1; channels <= max_channels; channels++) {
    for (uint32_t source_rate = 0; source_rate < array_size(sample_rates); source_rate++) {
      for (uint32_t dest_rate = 0; dest_rate < array_size(sample_rates); dest_rate++) {
        for (uint32_t chunk_duration = min_chunks; chunk_duration < max_chunks; chunk_duration++) {
          printf("one_way: channels: %d, source_rate: %d, dest_rate: %d, chunk_duration: %d\n",
                  channels, sample_rates[source_rate], sample_rates[dest_rate], chunk_duration);
          test_resampler_one_way<float>(channels, sample_rates[source_rate],
                                        sample_rates[dest_rate], chunk_duration);
        }
      }
    }
  }
}

void test_delay_line()
{
  for (uint32_t channel = 1; channel <= 2; channel++) {
    for (uint32_t delay_frames = 4; delay_frames <= 40; delay_frames++) {
      for (uint32_t chunk_size = 10; chunk_size <= 30; chunk_size++) {
       printf("channel: %d, delay_frames: %d, chunk_size: %d\n",
              channel, delay_frames, chunk_size);
        test_delay_lines(delay_frames, channel, chunk_size);
      }
    }
  }
}

int main()
{
  test_resamplers_one_way();
  test_delay_line();

  return 0;
}
