#include "cubeb/cubeb.h"
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <iostream>
#ifdef _WIN32
#include <objbase.h> // Used by CoInitialize()
#endif

#ifndef M_PI
#define M_PI 3.14159263
#endif

// Default values if none specified
#define DEFAULT_RATE 44100
#define DEFAULT_CHANNELS 2

static const char* state_to_string(cubeb_state state) {
  switch (state) {
    case CUBEB_STATE_STARTED:
      return "CUBEB_STATE_STARTED";
    case CUBEB_STATE_STOPPED:
      return "CUBEB_STATE_STOPPED";
    case CUBEB_STATE_DRAINED:
      return "CUBEB_STATE_DRAINED";
    case CUBEB_STATE_ERROR:
      return "CUBEB_STATE_ERROR";
    default:
      return "Undefined state";
  }
}

void print_log(const char* msg, ...) {
  va_list args;
  va_start(args, msg);
  vprintf(msg, args);
  va_end(args);
}

class cubeb_client final {
public:
  cubeb_client() {}
  ~cubeb_client() {}

  bool init(char const * backend_name = nullptr);
  bool init_stream();
  bool start_stream() const;
  bool stop_stream() const;
  bool destroy_stream() const;
  bool destroy();
  bool activate_log(bool active);

  long user_data_cb(cubeb_stream* stm, void* user, const void* input_buffer,
                    void* output_buffer, long nframes);

  void user_state_cb(cubeb_stream* stm, void* user, cubeb_state state);

  bool register_device_collection_changed(cubeb_device_type devtype);
  void device_collection_changed_callback(cubeb* context, void* user);

  cubeb_stream_params output_params = {};
  cubeb_stream_params input_params = {};

private:
  bool has_input() { return input_params.rate != 0; }
  bool has_output() { return output_params.rate != 0; }

  cubeb* context = nullptr;

  cubeb_stream* stream = nullptr;
  cubeb_devid output_device = nullptr;
  cubeb_devid input_device = nullptr;

  /* Accessed only from client and audio thread. */
  std::atomic<uint32_t> _rate = {0};
  std::atomic<uint32_t> _channels = {0};

  /* Accessed only from audio thread. */
  uint32_t _total_frames = 0;
};

bool cubeb_client::init(char const * backend_name) {
  int rv = cubeb_init(&context, "Cubeb Test Application", nullptr);
  if (rv != CUBEB_OK) {
    fprintf(stderr, "Could not init cubeb\n");
    return false;
  }
  return true;
}

static long user_data_cb_s(cubeb_stream* stm, void* user,
                           const void* input_buffer, void* output_buffer,
                           long nframes) {
  assert(user);
  return static_cast<cubeb_client*>(user)->user_data_cb(stm, user, input_buffer,
                                                        output_buffer, nframes);
}

static void user_state_cb_s(cubeb_stream* stm, void* user, cubeb_state state) {
  assert(user);
  return static_cast<cubeb_client*>(user)->user_state_cb(stm, user, state);
}

void device_collection_changed_callback_s(cubeb* context, void* user) {
  assert(user);
  return static_cast<cubeb_client*>(user)->device_collection_changed_callback(
      context, user);
}

bool cubeb_client::init_stream() {
  assert(has_input() || has_output());

  _rate = has_output() ? output_params.rate : input_params.rate;
  _channels = has_output() ? output_params.channels : input_params.channels;

  int rv =
      cubeb_stream_init(context, &stream, "Stream", input_device,
                        has_input() ? &input_params : nullptr, output_device,
                        has_output() ? &output_params : nullptr, 512,
                        user_data_cb_s, user_state_cb_s, this);
  if (rv != CUBEB_OK) {
    fprintf(stderr, "Could not open the stream\n");
    return false;
  }
  return true;
}

bool cubeb_client::start_stream() const {
  int rv = cubeb_stream_start(stream);
  if (rv != CUBEB_OK) {
    fprintf(stderr, "Could not start the stream\n");
    return false;
  }
  return true;
}

bool cubeb_client::stop_stream() const {
  int rv = cubeb_stream_stop(stream);
  if (rv != CUBEB_OK) {
    fprintf(stderr, "Could not stop the stream\n");
    return false;
  }
  return true;
}

bool cubeb_client::destroy_stream() const {
  cubeb_stream_destroy(stream);
  return true;
}

bool cubeb_client::destroy() {
  cubeb_destroy(context);
  return true;
}

bool cubeb_client::activate_log(bool active) {
  cubeb_log_level log_level = CUBEB_LOG_DISABLED;
  cubeb_log_callback log_callback = nullptr;
  if (active) {
    log_level = CUBEB_LOG_NORMAL;
    log_callback = print_log;
  }

  if (cubeb_set_log_callback(log_level, log_callback) != CUBEB_OK) {
    fprintf(stderr, "Set log callback failed\n");
    return false;
  }
  return true;
}

static void fill_with_sine_tone(float* buf, uint32_t num_of_frames,
                               uint32_t num_of_channels, uint32_t frame_rate,
                               uint32_t position) {
  for (uint32_t i = 0; i < num_of_frames; ++i) {
    for (uint32_t c = 0; c < num_of_channels; ++c) {
      buf[i * num_of_channels + c] =
          0.2 * sin(2 * M_PI * (i + position) * 350 / frame_rate);
      buf[i * num_of_channels + c] +=
          0.2 * sin(2 * M_PI * (i + position) * 440 / frame_rate);
    }
  }
}

long cubeb_client::user_data_cb(cubeb_stream* stm, void* user,
                                const void* input_buffer, void* output_buffer,
                                long nframes) {
  if (input_buffer && output_buffer) {
    const float* in = static_cast<const float*>(input_buffer);
    float* out = static_cast<float*>(output_buffer);
    memcpy(out, in, sizeof(float) * nframes * _channels);
  } else if (output_buffer && !input_buffer) {
    fill_with_sine_tone(static_cast<float*>(output_buffer), nframes, _channels,
                       _rate, _total_frames);
  }

  _total_frames += nframes;
  return nframes;
}

void cubeb_client::user_state_cb(cubeb_stream* stm, void* user,
                                 cubeb_state state) {
  fprintf(stderr, "state is %s\n", state_to_string(state));
}

bool cubeb_client::register_device_collection_changed(
    cubeb_device_type devtype) {
  int r = 0;
  r = cubeb_register_device_collection_changed(
      context, devtype, device_collection_changed_callback_s, this);
  if (r != CUBEB_OK) {
    return false;
  }
  return true;
}

void cubeb_client::device_collection_changed_callback(cubeb* context,
                                                      void* user) {
  fprintf(stderr, "device_collection_changed_callback\n");
}

enum play_mode {
  RECORD,
  PLAYBACK,
  DUPLEX,
  COLLECTION_CHANGE,
};

bool choose_action(const cubeb_client& cl, play_mode pm, char c) {
  while (c == 10 || c == 32) {
    // Consume "enter and "space"
    c = getchar();
  }

  if (c == 's') {
    if (pm == PLAYBACK || pm == RECORD || pm == DUPLEX) {
      cl.stop_stream();
      cl.destroy_stream();
    }
    return false;
  }

  fprintf(stderr, "Error: %c is not a valid entry\n", c);
  return true;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
  CoInitialize(nullptr);
#endif

  play_mode pm = DUPLEX;
  if (argc > 1) {
    if ('r' == argv[1][0]) {
      pm = RECORD;
    } else if ('p' == argv[1][0]) {
      pm = PLAYBACK;
    } else if ('d' == argv[1][0]) {
      pm = DUPLEX;
    } else if ('c' == argv[1][0]) {
      pm = COLLECTION_CHANGE;
    }
  }
  uint32_t rate = DEFAULT_RATE;
  if (argc > 2) {
    rate = strtoul(argv[2], NULL, 0);
  }

  bool res = false;
  cubeb_client cl;
  cl.activate_log(true);
  cl.init();

  if (pm == COLLECTION_CHANGE) {
    res = cl.register_device_collection_changed(static_cast<cubeb_device_type>(
        CUBEB_DEVICE_TYPE_INPUT & CUBEB_DEVICE_TYPE_INPUT));
    if (res) {
      fprintf(stderr,
              "register_device_collection_changed for input+output devices "
              "success\n");
    } else {
      fprintf(stderr, "register_device_collection_changed failed\n");
    }
  } else {
    if (pm == PLAYBACK || pm == DUPLEX) {
      cl.output_params = {CUBEB_SAMPLE_FLOAT32NE, rate, DEFAULT_CHANNELS,
                          CUBEB_LAYOUT_UNDEFINED, CUBEB_STREAM_PREF_NONE};
    }
    if (pm == RECORD || pm == DUPLEX) {
      cl.input_params = {CUBEB_SAMPLE_FLOAT32NE, rate, DEFAULT_CHANNELS,
                         CUBEB_LAYOUT_UNDEFINED, CUBEB_STREAM_PREF_NONE};
    }
    res = cl.init_stream();
    if (!res) {
      return 1;
    }

    cl.start_stream();
  }

  // User input
  do {
    fprintf(stderr, "press `s` to stop the stream (if any) and abort\n");
  } while (choose_action(cl, pm, getchar()));

  cl.destroy();

  return 0;
}
