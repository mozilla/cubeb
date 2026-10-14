/*
 * Copyright © 2026 Mozilla Foundation
 *
 * This program is made available under an ISC-style license. See the
 * accompanying file LICENSE for details.
 */
#include "cubeb_wasapi_device_switching.h"
#include "gtest/gtest.h"

TEST(cubeb, device_switching_directions_and_roles)
{
  cubeb_stream_params input = {};
  cubeb_stream_params output = {};
  input.prefs = CUBEB_STREAM_PREF_VOICE;
  wasapi_device_switching_policy policy(nullptr, &input, nullptr, &output);
  EXPECT_TRUE(policy.follows_any_default());
  EXPECT_TRUE(policy.should_switch(CUBEB_DEVICE_TYPE_INPUT, true));
  EXPECT_FALSE(policy.should_switch(CUBEB_DEVICE_TYPE_INPUT, false));
  EXPECT_TRUE(policy.should_switch(CUBEB_DEVICE_TYPE_OUTPUT, false));
  EXPECT_FALSE(policy.should_switch(CUBEB_DEVICE_TYPE_OUTPUT, true));
}

TEST(cubeb, device_switching_pinned_sides)
{
  cubeb_stream_params input = {};
  cubeb_stream_params output = {};
  cubeb_devid explicit_device = reinterpret_cast<cubeb_devid>(1);
  for (bool disable_input : {false, true}) {
    for (bool disable_output : {false, true}) {
      input.prefs = disable_input ? CUBEB_STREAM_PREF_DISABLE_DEVICE_SWITCHING
                                  : CUBEB_STREAM_PREF_NONE;
      output.prefs = disable_output ? CUBEB_STREAM_PREF_DISABLE_DEVICE_SWITCHING
                                    : CUBEB_STREAM_PREF_NONE;
      for (bool explicit_input : {false, true}) {
        for (bool explicit_output : {false, true}) {
          wasapi_device_switching_policy policy(
              explicit_input ? explicit_device : nullptr, &input,
              explicit_output ? explicit_device : nullptr, &output);
          bool follow_input = !disable_input && !explicit_input;
          bool follow_output = !disable_output && !explicit_output;
          EXPECT_EQ(policy.should_switch(CUBEB_DEVICE_TYPE_INPUT, false),
                    follow_input);
          EXPECT_EQ(policy.should_switch(CUBEB_DEVICE_TYPE_OUTPUT, false),
                    follow_output);
          EXPECT_EQ(policy.follows_any_default(),
                    follow_input || follow_output);
        }
      }
    }
  }
}

TEST(cubeb, device_switching_single_direction_and_loopback)
{
  cubeb_stream_params params = {};
  wasapi_device_switching_policy input(nullptr, &params, nullptr, nullptr);
  EXPECT_TRUE(input.should_switch(CUBEB_DEVICE_TYPE_INPUT, false));
  EXPECT_FALSE(input.should_switch(CUBEB_DEVICE_TYPE_OUTPUT, false));
  wasapi_device_switching_policy output(nullptr, nullptr, nullptr, &params);
  EXPECT_FALSE(output.should_switch(CUBEB_DEVICE_TYPE_INPUT, false));
  EXPECT_TRUE(output.should_switch(CUBEB_DEVICE_TYPE_OUTPUT, false));
  params.prefs = CUBEB_STREAM_PREF_LOOPBACK;
  wasapi_device_switching_policy loopback(nullptr, &params, nullptr, nullptr);
  EXPECT_FALSE(loopback.should_switch(CUBEB_DEVICE_TYPE_INPUT, false));
  EXPECT_TRUE(loopback.should_switch(CUBEB_DEVICE_TYPE_OUTPUT, false));
}
