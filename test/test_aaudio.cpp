/*
 * Copyright © 2026 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

#include "gtest/gtest.h"
// Include the backend to replace its dynamically loaded AAudio entry points.
// No Android audio service or hardware is used by these tests.
#include "cubeb_aaudio.cpp"
#include <algorithm>
#include <functional>
#include <type_traits>
#include <utility>

struct AAudioStreamBuilderStruct {
  AAudioStream_dataCallback callback = nullptr;
  void * user_ptr = nullptr;
};

struct AAudioStreamStruct : AAudioStreamBuilderStruct {
  aaudio_stream_state_t state = AAUDIO_STREAM_STATE_OPEN;
  int64_t frames_written = 0;

  void
  fire(aaudio_data_callback_result_t expected = AAUDIO_CALLBACK_RESULT_CONTINUE)
  {
    if (callback) {
      float buffer[128] = {};
      EXPECT_EQ(callback(this, user_ptr, buffer, 128), expected);
      frames_written += 128;
    }
  }
};

template <typename T> struct AAudioStub;
template <typename R, typename... Args> struct AAudioStub<R (*)(Args...)> {
  static R call(Args...)
  {
    if constexpr (!std::is_void_v<R>) {
      return {};
    }
  }
};

enum class direction { INPUT, OUTPUT, DUPLEX };

class AAudioDrain : public testing::TestWithParam<direction> {
protected:
  static AAudioDrain * current;
  cubeb context;
  cubeb_stream * stm = nullptr;
  uint32_t device_rate = 48000;
  int callbacks = 0;
  bool short_return = false;
  bool return_zero = false;
  bool fail_callback = false;
  bool fire_on_start = true;
  aaudio_data_callback_result_t callback_result =
      AAUDIO_CALLBACK_RESULT_CONTINUE;
  bool fail_next_stop = false;
  int starts = 0;
  std::vector<cubeb_state> notifications;
  std::function<void(AAudioStream *)> before_stop;

  void SetUp() override
  {
    current = this;
    auto & api = AAudioLibrary::get();
#define STUB(x) api.x = AAudioStub<decltype(api.x)>::call;
    LIBAAUDIO_API_VISIT(STUB)
#undef STUB
    api.AAudio_createStreamBuilder = [](AAudioStreamBuilder ** builder) {
      *builder = new AAudioStreamBuilder;
      return aaudio_result_t{AAUDIO_OK};
    };
    api.AAudioStreamBuilder_delete = [](AAudioStreamBuilder * builder) {
      delete builder;
      return aaudio_result_t{AAUDIO_OK};
    };
    api.AAudioStreamBuilder_setDataCallback =
        [](AAudioStreamBuilder * builder, AAudioStream_dataCallback callback,
           void * user_ptr) {
          builder->callback = callback;
          builder->user_ptr = user_ptr;
        };
    api.AAudioStreamBuilder_openStream = [](AAudioStreamBuilder * builder,
                                            AAudioStream ** stream) {
      *stream = new AAudioStream;
      static_cast<AAudioStreamBuilder &>(**stream) = *builder;
      return aaudio_result_t{AAUDIO_OK};
    };
    api.AAudioStream_close = [](AAudioStream * stream) {
      delete stream;
      return aaudio_result_t{AAUDIO_OK};
    };
    api.AAudioStream_getSampleRate = [](AAudioStream *) -> int32_t {
      return current->device_rate;
    };
    api.AAudioStream_getFramesPerBurst = [](AAudioStream *) -> int32_t {
      return 128;
    };
    api.AAudioStream_getState = [](AAudioStream * stream) {
      return stream->state;
    };
    api.AAudioStream_waitForStateChange =
        [](AAudioStream * stream, aaudio_stream_state_t,
           aaudio_stream_state_t * state, int64_t) {
          *state = stream->state;
          return aaudio_result_t{AAUDIO_OK};
        };
    api.AAudioStream_requestStart = [](AAudioStream * stream) {
      ++current->starts;
      stream->state = AAUDIO_STREAM_STATE_STARTED;
      // Exercise callbacks both during and after requestStart.
      if (current->fire_on_start) {
        stream->fire(current->callback_result);
      }
      return aaudio_result_t{AAUDIO_OK};
    };
    api.AAudioStream_requestPause = [](AAudioStream * stream) {
      auto hook = std::exchange(current->before_stop, nullptr);
      if (hook) {
        hook(stream);
      }
      if (std::exchange(current->fail_next_stop, false)) {
        stream->state = AAUDIO_STREAM_STATE_DISCONNECTED;
        return aaudio_result_t{AAUDIO_ERROR_DISCONNECTED};
      }
      stream->state = AAUDIO_STREAM_STATE_PAUSED;
      return aaudio_result_t{AAUDIO_OK};
    };
    api.AAudioStream_requestStop = [](AAudioStream * stream) {
      auto hook = std::exchange(current->before_stop, nullptr);
      if (hook) {
        hook(stream);
      }
      if (std::exchange(current->fail_next_stop, false)) {
        stream->state = AAUDIO_STREAM_STATE_DISCONNECTED;
        return aaudio_result_t{AAUDIO_ERROR_DISCONNECTED};
      }
      stream->state = AAUDIO_STREAM_STATE_STOPPED;
      return aaudio_result_t{AAUDIO_OK};
    };
    api.AAudioStream_getFramesWritten = [](AAudioStream * stream) {
      return stream->frames_written;
    };
    api.AAudioStream_getFramesRead = api.AAudioStream_getFramesWritten;
    api.AAudioStream_getTimestamp = [](AAudioStream *, clockid_t, int64_t *,
                                       int64_t *) {
      return aaudio_result_t{AAUDIO_ERROR_INVALID_STATE};
    };
    api.AAudioStream_read = [](AAudioStream *, void * buffer, int32_t frames,
                               int64_t) {
      std::memset(buffer, 0, frames * sizeof(float));
      return frames;
    };
  }

  void TearDown() override
  {
    if (stm) {
      aaudio_stream_stop(stm);
      aaudio_stream_destroy(stm);
    }
    current = nullptr;
  }

  static long data_cb(cubeb_stream *, void * user_ptr, void const *,
                      void * output, long frames)
  {
    auto & self = *static_cast<AAudioDrain *>(user_ptr);
    ++self.callbacks;
    if (self.fail_callback) {
      return CUBEB_ERROR;
    }
    if (output) {
      std::memset(output, 0, frames * sizeof(float));
    }
    return self.short_return ? (self.return_zero ? 0 : frames - 1) : frames;
  }

  static void state_cb(cubeb_stream *, void * user_ptr, cubeb_state state)
  {
    static_cast<AAudioDrain *>(user_ptr)->notifications.push_back(state);
  }

  void init()
  {
    cubeb_stream_params params = {};
    params.rate = 48000;
    params.channels = 1;
    params.format = CUBEB_SAMPLE_FLOAT32NE;
    params.layout = CUBEB_LAYOUT_MONO;
    ASSERT_EQ(aaudio_stream_init(
                  &context, &stm, "drain", nullptr,
                  GetParam() == direction::OUTPUT ? nullptr : &params, nullptr,
                  GetParam() == direction::INPUT ? nullptr : &params, 128,
                  data_cb, state_cb, this),
              CUBEB_OK);
    ASSERT_EQ(aaudio_stream_start(stm), CUBEB_OK);
    update_state(stm);
    ASSERT_EQ(stm->state.load(), stream_state::STARTED);
    ASSERT_EQ(callbacks, 1);
  }

  AAudioStream * callback_stream()
  {
    return stm->ostream ? stm->ostream : stm->istream;
  }

  void reinit()
  {
    aaudio_error_cb(callback_stream(), stm, AAUDIO_ERROR_DISCONNECTED);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (stm->pending_reinit.load() &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(stm->pending_reinit.load(), 0);
  }

  void finish_drain(int expected_callbacks)
  {
    ASSERT_EQ(stm->state.load(), stream_state::DRAINING);
    // The first update requests stop; the next observes STOPPED.
    update_state(stm);
    update_state(stm);
    update_state(stm);
    EXPECT_EQ(stm->state.load(), stream_state::STOPPED);
    EXPECT_EQ(callbacks, expected_callbacks);
    EXPECT_EQ(std::count(notifications.begin(), notifications.end(),
                         CUBEB_STATE_DRAINED),
              1);
    EXPECT_EQ(std::count(notifications.begin(), notifications.end(),
                         CUBEB_STATE_ERROR),
              0);
  }

  void drain_before_device_change(bool zero)
  {
    init();
    short_return = true;
    return_zero = zero;
    callback_stream()->fire();
    ASSERT_EQ(callbacks, 2);
    ASSERT_EQ(stm->state.load(), stream_state::DRAINING);
    reinit();
    reinit();
    // The new device runs its callback, but the application must not run again.
    finish_drain(2);

    // An explicit restart enables application callbacks again.
    short_return = false;
    ASSERT_EQ(aaudio_stream_start(stm), CUBEB_OK);
    EXPECT_EQ(callbacks, 3);
  }
};

AAudioDrain * AAudioDrain::current = nullptr;

TEST_P(AAudioDrain, ShortReturnBeforeDeviceChange)
{
  drain_before_device_change(false);
}

TEST_P(AAudioDrain, ZeroReturnBeforeDeviceChange)
{
  drain_before_device_change(true);
}

TEST_P(AAudioDrain, ShortReturnDuringDeviceChange)
{
  init();
  before_stop = [this](AAudioStream * stream) {
    // Recovery has already snapshotted STARTED and decided to restart.
    short_return = true;
    stream->fire();
    EXPECT_EQ(callbacks, 2);
  };
  reinit();
  finish_drain(2);
}

TEST_P(AAudioDrain, ReuseDrainedStreamSlot)
{
  init();
  short_return = true;
  callback_stream()->fire();
  reinit();
  finish_drain(2);
  aaudio_stream_stop(stm);
  aaudio_stream_destroy(stm);
  stm = nullptr;
  short_return = false;
  callbacks = 0;
  notifications.clear();
  init();
}

TEST_P(AAudioDrain, ShortReturnDuringReplacementStart)
{
  init();
  short_return = true;
  reinit();
  if (stm->ostream) {
    // A drain requested inside requestStart must retain its target.
    EXPECT_EQ(stm->drain_target.load(), 127);
  }
  finish_drain(2);
}

TEST_P(AAudioDrain, ResamplerBufferedAfterShortReturn)
{
  if (GetParam() == direction::INPUT) {
    return;
  }
  device_rate = 44100;
  init();
  short_return = true;
  callback_stream()->fire();
  ASSERT_EQ(callbacks, 2);
  // The resampler's extra input frame lets it fill this output buffer even
  // though the application has already returned short.
  ASSERT_EQ(stm->state.load(), stream_state::STARTED);
  // Starting an already started stream is a no-op, not an explicit restart.
  ASSERT_EQ(aaudio_stream_start(stm), CUBEB_OK);
  reinit();
  finish_drain(2);
}

TEST_P(AAudioDrain, RestartResampledDrain)
{
  device_rate = 44100;
  init();
  short_return = true;
  callback_stream()->fire();
  for (int i = 0; i < 20 && stm->state.load() != stream_state::DRAINING; ++i) {
    callback_stream()->fire();
  }
  finish_drain(2);
  short_return = false;
  ASSERT_EQ(aaudio_stream_start(stm), CUBEB_OK);
  EXPECT_EQ(callbacks, 3);
  update_state(stm);
  EXPECT_EQ(stm->state.load(), stream_state::STARTED);
}

TEST_P(AAudioDrain, RestartWhileDraining)
{
  device_rate = 44100;
  init();
  short_return = true;
  callback_stream()->fire();
  for (int i = 0; i < 20 && stm->state.load() != stream_state::DRAINING; ++i) {
    callback_stream()->fire();
  }
  ASSERT_EQ(stm->state.load(), stream_state::DRAINING);
  short_return = false;
  ASSERT_EQ(aaudio_stream_start(stm), CUBEB_OK);
  callback_stream()->fire();
  EXPECT_GE(callbacks, 3);
  update_state(stm);
  EXPECT_EQ(stm->state.load(), stream_state::STARTED);
  EXPECT_EQ(std::count(notifications.begin(), notifications.end(),
                       CUBEB_STATE_DRAINED),
            0);
}

TEST_P(AAudioDrain, NoRestartNotificationWhileDraining)
{
  init();
  short_return = true;
  callback_stream()->fire();
  ASSERT_EQ(stm->state.load(), stream_state::DRAINING);
  int initial_starts = starts;
  notifications.clear();
  fire_on_start = false;
  reinit();
  update_state(stm);
  EXPECT_EQ(starts, initial_starts);
  EXPECT_EQ(std::count(notifications.begin(), notifications.end(),
                       CUBEB_STATE_STARTED),
            0);
  // Recovery may already have stopped the replacement streams.
  update_state(stm);
  update_state(stm);
  EXPECT_EQ(callbacks, 2);
  EXPECT_EQ(stm->state.load(), stream_state::STOPPED);
  EXPECT_EQ(std::count(notifications.begin(), notifications.end(),
                       CUBEB_STATE_DRAINED),
            1);
}

TEST_P(AAudioDrain, ErrorDuringDeviceChange)
{
  init();
  before_stop = [this](AAudioStream * stream) {
    fail_callback = true;
    stream->fire(AAUDIO_CALLBACK_RESULT_STOP);
    EXPECT_EQ(stm->state.load(), stream_state::ERROR);
  };
  reinit();
  update_state(stm);
  update_state(stm);
  EXPECT_EQ(callbacks, 2);
  EXPECT_EQ(
      std::count(notifications.begin(), notifications.end(), CUBEB_STATE_ERROR),
      1);
  EXPECT_EQ(std::count(notifications.begin(), notifications.end(),
                       CUBEB_STATE_DRAINED),
            0);
}

TEST_P(AAudioDrain, StopFailureDuringDeviceChange)
{
  init();
  fail_next_stop = true;
  reinit();
  update_state(stm);
  EXPECT_EQ(callbacks, 2);
  EXPECT_EQ(stm->state.load(), stream_state::STARTED);
  EXPECT_EQ(
      std::count(notifications.begin(), notifications.end(), CUBEB_STATE_ERROR),
      0);
}

TEST_P(AAudioDrain, ErrorBeforeDeviceChange)
{
  init();
  fail_callback = true;
  callback_stream()->fire(AAUDIO_CALLBACK_RESULT_STOP);
  reinit();
  update_state(stm);
  update_state(stm);
  EXPECT_EQ(callbacks, 2);
  EXPECT_EQ(
      std::count(notifications.begin(), notifications.end(), CUBEB_STATE_ERROR),
      1);
  EXPECT_EQ(std::count(notifications.begin(), notifications.end(),
                       CUBEB_STATE_DRAINED),
            0);
}

TEST_P(AAudioDrain, ErrorDuringReplacementStart)
{
  init();
  fail_callback = true;
  callback_result = AAUDIO_CALLBACK_RESULT_STOP;
  reinit();
  update_state(stm);
  update_state(stm);
  EXPECT_EQ(callbacks, 2);
  EXPECT_EQ(
      std::count(notifications.begin(), notifications.end(), CUBEB_STATE_ERROR),
      1);
  EXPECT_EQ(std::count(notifications.begin(), notifications.end(),
                       CUBEB_STATE_DRAINED),
            0);
}

TEST_P(AAudioDrain, DrainDuringStopFailure)
{
  init();
  before_stop = [this](AAudioStream * stream) {
    short_return = true;
    stream->fire();
    fail_next_stop = true;
  };
  reinit();
  finish_drain(2);
}

INSTANTIATE_TEST_CASE_P(Directions, AAudioDrain,
                        testing::Values(direction::INPUT, direction::OUTPUT,
                                        direction::DUPLEX));
