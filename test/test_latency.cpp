#include <stdio.h>
#include <stdlib.h>
#include "gtest/gtest.h"
#include "cubeb/cubeb.h"

#define LOG(msg) fprintf(stderr, "%s\n", msg);

TEST(latency, main)
{
  cubeb * ctx = NULL;
  int r;
  uint32_t max_channels;
  uint32_t preferred_rate;
  uint32_t latency_frames;

  LOG("latency_test start");
  r = cubeb_init(&ctx, "Cubeb audio test");
  ASSERT_EQ(r, CUBEB_OK);
  LOG("cubeb_init ok");

  r = cubeb_get_max_channel_count(ctx, &max_channels);
  ASSERT_TRUE(r == CUBEB_OK || r == CUBEB_ERROR_NOT_SUPPORTED);
  if (r == CUBEB_OK) {
    ASSERT_GT(max_channels, 0u);
    LOG("cubeb_get_max_channel_count ok");
  }

  r = cubeb_get_preferred_sample_rate(ctx, &preferred_rate);
  ASSERT_TRUE(r == CUBEB_OK || r == CUBEB_ERROR_NOT_SUPPORTED);
  if (r == CUBEB_OK) {
    ASSERT_GT(preferred_rate, 0u);
    LOG("cubeb_get_preferred_sample_rate ok");
  }

  cubeb_stream_params params = {
    CUBEB_SAMPLE_FLOAT32NE,
    preferred_rate,
    max_channels
  };
  r = cubeb_get_min_latency(ctx, params, &latency_frames);
  ASSERT_TRUE(r == CUBEB_OK || r == CUBEB_ERROR_NOT_SUPPORTED);
  if (r == CUBEB_OK) {
    ASSERT_GT(latency_frames, 0u);
    LOG("cubeb_get_min_latency ok");
  }

  cubeb_destroy(ctx);
  LOG("cubeb_destroy ok");
}
