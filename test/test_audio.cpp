/*
 * Copyright © 2013 Sebastien Alaiwan <sebastien.alaiwan@gmail.com>
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

/* libcubeb api/function exhaustive test. Plays a series of tones in different
 * conditions. */
#include "gtest/gtest.h"
#if !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 600
#endif
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "cubeb/cubeb.h"
#include "common.h"

#define MAX_NUM_CHANNELS 32

#if !defined(M_PI)
#define M_PI 3.14159265358979323846
#endif

#define NELEMS(x) ((int) (sizeof(x) / sizeof(x[0])))
#define VOLUME 0.2

float get_frequency(int channel_index)
{
  return 220.0f * (channel_index+1);
}

typedef struct {
  char const * name;
  unsigned int const channels;
  cubeb_channel_layout const layout;
} layout_info;

const layout_info layout_infos[CUBEB_LAYOUT_MAX] = {
  { "undefined",      0,  CUBEB_LAYOUT_UNDEFINED },
  { "dual mono",      2,  CUBEB_LAYOUT_DUAL_MONO },
  { "dual mono lfe",  3,  CUBEB_LAYOUT_DUAL_MONO_LFE },
  { "mono",           1,  CUBEB_LAYOUT_MONO },
  { "mono lfe",       2,  CUBEB_LAYOUT_MONO_LFE },
  { "stereo",         2,  CUBEB_LAYOUT_STEREO },
  { "stereo lfe",     3,  CUBEB_LAYOUT_STEREO_LFE },
  { "3f",             3,  CUBEB_LAYOUT_3F },
  { "3f lfe",         4,  CUBEB_LAYOUT_3F_LFE },
  { "2f1",            3,  CUBEB_LAYOUT_2F1 },
  { "2f1 lfe",        4,  CUBEB_LAYOUT_2F1_LFE },
  { "3f1",            4,  CUBEB_LAYOUT_3F1 },
  { "3f1 lfe",        5,  CUBEB_LAYOUT_3F1_LFE },
  { "2f2",            4,  CUBEB_LAYOUT_2F2 },
  { "2f2 lfe",        5,  CUBEB_LAYOUT_2F2_LFE },
  { "3f2",            5,  CUBEB_LAYOUT_3F2 },
  { "3f2 lfe",        6,  CUBEB_LAYOUT_3F2_LFE },
  { "3f3r lfe",       7,  CUBEB_LAYOUT_3F3R_LFE },
  { "3f4 lfe",        8,  CUBEB_LAYOUT_3F4_LFE }
};

/* store the phase of the generated waveform */
typedef struct {
  int num_channels;
  float phase[MAX_NUM_CHANNELS];
  float sample_rate;
} synth_state;

synth_state* synth_create(int num_channels, float sample_rate)
{
  synth_state* synth = (synth_state *) malloc(sizeof(synth_state));
  if (!synth)
    return NULL;
  for(int i=0;i < MAX_NUM_CHANNELS;++i)
    synth->phase[i] = 0.0f;
  synth->num_channels = num_channels;
  synth->sample_rate = sample_rate;
  return synth;
}

void synth_destroy(synth_state* synth)
{
  free(synth);
}

void synth_run_float(synth_state* synth, float* audiobuffer, long nframes)
{
  for(int c=0;c < synth->num_channels;++c) {
    float freq = get_frequency(c);
    float phase_inc = 2.0 * M_PI * freq / synth->sample_rate;
    for(long n=0;n < nframes;++n) {
      audiobuffer[n*synth->num_channels+c] = sin(synth->phase[c]) * VOLUME;
      synth->phase[c] += phase_inc;
    }
  }
}

long data_cb_float(cubeb_stream * /*stream*/, void * user, const void * /*inputbuffer*/, void * outputbuffer, long nframes)
{
  synth_state *synth = (synth_state *)user;
  synth_run_float(synth, (float*)outputbuffer, nframes);
  return nframes;
}

void synth_run_16bit(synth_state* synth, short* audiobuffer, long nframes)
{
  for(int c=0;c < synth->num_channels;++c) {
    float freq = get_frequency(c);
    float phase_inc = 2.0 * M_PI * freq / synth->sample_rate;
    for(long n=0;n < nframes;++n) {
      audiobuffer[n*synth->num_channels+c] = sin(synth->phase[c]) * VOLUME * 32767.0f;
      synth->phase[c] += phase_inc;
    }
  }
}

long data_cb_short(cubeb_stream * /*stream*/, void * user, const void * /*inputbuffer*/, void * outputbuffer, long nframes)
{
  synth_state *synth = (synth_state *)user;
  synth_run_16bit(synth, (short*)outputbuffer, nframes);
  return nframes;
}

void state_cb_audio(cubeb_stream * /*stream*/, void * /*user*/, cubeb_state /*state*/)
{
}

/* Our android backends don't support float, only int16. */
int supports_float32(const char* backend_id)
{
  return (strcmp(backend_id, "opensl") != 0 &&
          strcmp(backend_id, "audiotrack") != 0);
}

/* The WASAPI backend only supports float. */
int supports_int16(const char* backend_id)
{
  return strcmp(backend_id, "wasapi") != 0;
}

/* Some backends don't have code to deal with more than mono or stereo. */
int supports_channel_count(const char* backend_id, int nchannels)
{
  return nchannels <= 2 ||
    (strcmp(backend_id, "opensl") != 0 && strcmp(backend_id, "audiotrack") != 0);
}

int run_test(int num_channels, layout_info layout, int sampling_rate, int is_float)
{
  int r = CUBEB_OK;

  cubeb *ctx = NULL;
  synth_state* synth = NULL;
  cubeb_stream *stream = NULL;
  const char * backend_id = NULL;

  r = cubeb_init(&ctx, "Cubeb audio test: channels");
  if (r != CUBEB_OK) {
    fprintf(stderr, "Error initializing cubeb library\n");
    goto cleanup;
  }

  backend_id = cubeb_get_backend_id(ctx);

  if ((is_float && !supports_float32(backend_id)) ||
      (!is_float && !supports_int16(backend_id)) ||
      !supports_channel_count(backend_id, num_channels)) {
    /* don't treat this as a test failure. */
    goto cleanup;
  }

  fprintf(stderr, "Testing %d channel(s), layout: %s, %d Hz, %s (%s)\n", num_channels, layout.name, sampling_rate, is_float ? "float" : "short", cubeb_get_backend_id(ctx));

  cubeb_stream_params params;
  params.format = is_float ? CUBEB_SAMPLE_FLOAT32NE : CUBEB_SAMPLE_S16NE;
  params.rate = sampling_rate;
  params.channels = num_channels;
  params.layout = layout.layout;

  synth = synth_create(params.channels, params.rate);
  if (synth == NULL) {
    fprintf(stderr, "Out of memory\n");
    goto cleanup;
  }

  r = cubeb_stream_init(ctx, &stream, "test tone", NULL, NULL, NULL, &params,
                        4096, is_float ? data_cb_float : data_cb_short, state_cb_audio, synth);
  if (r != CUBEB_OK) {
    fprintf(stderr, "Error initializing cubeb stream: %d\n", r);
    goto cleanup;
  }

  cubeb_stream_start(stream);
  delay(200);
  cubeb_stream_stop(stream);

cleanup:
  cubeb_stream_destroy(stream);
  cubeb_destroy(ctx);
  synth_destroy(synth);

  return r;
}

int run_panning_volume_test(int is_float)
{
  int r = CUBEB_OK;

  cubeb *ctx = NULL;
  synth_state* synth = NULL;
  cubeb_stream *stream = NULL;
  const char * backend_id = NULL;

  r = cubeb_init(&ctx, "Cubeb audio test");
  if (r != CUBEB_OK) {
    fprintf(stderr, "Error initializing cubeb library\n");
    goto cleanup;
  }
  backend_id = cubeb_get_backend_id(ctx);

  if ((is_float && !supports_float32(backend_id)) ||
      (!is_float && !supports_int16(backend_id))) {
    /* don't treat this as a test failure. */
    goto cleanup;
  }

  cubeb_stream_params params;
  params.format = is_float ? CUBEB_SAMPLE_FLOAT32NE : CUBEB_SAMPLE_S16NE;
  params.rate = 44100;
  params.channels = 2;
  params.layout = CUBEB_LAYOUT_STEREO;

  synth = synth_create(params.channels, params.rate);
  if (synth == NULL) {
    fprintf(stderr, "Out of memory\n");
    goto cleanup;
  }

  r = cubeb_stream_init(ctx, &stream, "test tone", NULL, NULL, NULL, &params,
                        4096, is_float ? data_cb_float : data_cb_short,
                        state_cb_audio, synth);
  if (r != CUBEB_OK) {
    fprintf(stderr, "Error initializing cubeb stream: %d\n", r);
    goto cleanup;
  }

  fprintf(stderr, "Testing: volume\n");
  for(int i=0;i <= 4; ++i)
  {
    fprintf(stderr, "Volume: %d%%\n", i*25);

    cubeb_stream_set_volume(stream, i/4.0f);
    cubeb_stream_start(stream);
    delay(400);
    cubeb_stream_stop(stream);
    delay(100);
  }

  fprintf(stderr, "Testing: panning\n");
  for(int i=-4;i <= 4; ++i)
  {
    fprintf(stderr, "Panning: %.2f\n", i/4.0f);

    cubeb_stream_set_panning(stream, i/4.0f);
    cubeb_stream_start(stream);
    delay(400);
    cubeb_stream_stop(stream);
    delay(100);
  }

cleanup:
  cubeb_stream_destroy(stream);
  cubeb_destroy(ctx);
  synth_destroy(synth);

  return r;
}

TEST(cubeb, run_panning_volume_test_short)
{
  ASSERT_EQ(run_panning_volume_test(0), CUBEB_OK);
}

TEST(cubeb, run_panning_volume_test_float)
{
  ASSERT_EQ(run_panning_volume_test(1), CUBEB_OK);
}

TEST(cubeb, run_channel_rate_test)
{
  unsigned int channel_values[] = {
    1,
    2,
    3,
    4,
    6,
  };

  int freq_values[] = {
    16000,
    24000,
    44100,
    48000,
  };

  for(int j = 0; j < NELEMS(channel_values); ++j) {
    for(int i = 0; i < NELEMS(freq_values); ++i) {
      ASSERT_TRUE(channel_values[j] < MAX_NUM_CHANNELS);
      fprintf(stderr, "--------------------------\n");
      for (int k = 0 ; k < NELEMS(layout_infos); ++k ) {
        if (layout_infos[k].channels == channel_values[j]) {
          ASSERT_EQ(run_test(channel_values[j], layout_infos[k], freq_values[i], 0), CUBEB_OK);
          ASSERT_EQ(run_test(channel_values[j], layout_infos[k], freq_values[i], 1), CUBEB_OK);
        }
      }
    }
  }
}
