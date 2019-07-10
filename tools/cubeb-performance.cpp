#undef NDEBUG
#include "cubeb/cubeb.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <objbase.h> // Used by CoInitialize()
#endif

#define DEBUG false
#define DEBUG_PRINT(...)                                                       \
  do {                                                                         \
    if (DEBUG)                                                                 \
      fprintf(stderr, __VA_ARGS__);                                            \
  } while (0)

using std::cin;
using std::cout;
using std::endl;
using std::unique_ptr;
using std::vector;

const uint32_t STREAM_LATENCY = 256;

const cubeb_sample_format STREAM_FORMAT = CUBEB_SAMPLE_FLOAT32LE;
const uint32_t STREAM_RATE = 48000;
const cubeb_stream_prefs STREAM_PREFS = CUBEB_STREAM_PREF_NONE;

const cubeb_stream_params STREAM_OUTPUT_PARAMS = { STREAM_FORMAT, STREAM_RATE,
                                                   2, CUBEB_LAYOUT_STEREO,
                                                   STREAM_PREFS };

const cubeb_stream_params STREAM_INPUT_PARAMS = { STREAM_FORMAT, STREAM_RATE, 1,
                                                  CUBEB_LAYOUT_MONO,
                                                  STREAM_PREFS };

typedef uint32_t stream_id;

int
init_backend(cubeb** ctx, char const* ctx_name)
{
  int r;
  char const* backend = nullptr;
  char const* ctx_backend = nullptr;

  backend = getenv("CUBEB_BACKEND");
  r = cubeb_init(ctx, ctx_name, backend);
  if (r == CUBEB_OK && backend) {
    ctx_backend = cubeb_get_backend_id(*ctx);
    if (strcmp(backend, ctx_backend) != 0) {
      DEBUG_PRINT("Requested backend `%s', got `%s'\n", backend, ctx_backend);
      return CUBEB_ERROR;
    }
  }

  return r;
}

long
data_cb(cubeb_stream* stream, void* user, const void* inputbuffer,
        void* outputbuffer, long nframes)
{
  if (!stream) {
    return CUBEB_ERROR;
  }

  if (outputbuffer) {
    memset(outputbuffer, 0,
           nframes * sizeof(float) * STREAM_OUTPUT_PARAMS.channels);
  }

  return nframes;
}

void
state_cb(cubeb_stream* stream, void* user, cubeb_state state)
{
  assert(stream);
  assert(user);
  stream_id* id = static_cast<stream_id*>(user);

  switch (state) {
    case CUBEB_STATE_STARTED:
      DEBUG_PRINT("stream %d started\n", *id);
      break;
    case CUBEB_STATE_STOPPED:
      DEBUG_PRINT("stream %d stopped\n", *id);
      break;
    case CUBEB_STATE_DRAINED:
      DEBUG_PRINT("stream %d drained\n", *id);
      break;
    default:
      DEBUG_PRINT("unknown stream %d state %d\n", *id, state);
      break;
  }

  return;
}

void
create_and_start_stream(cubeb* ctx, uint32_t latency_frames, stream_id* id,
                        cubeb_device_type type, cubeb_stream* streams[],
                        size_t index)
{
  cubeb_stream_params input_stream_params = STREAM_INPUT_PARAMS;
  cubeb_stream_params output_stream_params = STREAM_OUTPUT_PARAMS;

  cubeb_stream_params* in_stm_params = nullptr;
  if (type & CUBEB_DEVICE_TYPE_INPUT) {
    in_stm_params = &input_stream_params;
  }

  cubeb_stream_params* out_stm_params = nullptr;
  if (type & CUBEB_DEVICE_TYPE_OUTPUT) {
    out_stm_params = &output_stream_params;
  }

  cubeb_stream* stream;
  int r = cubeb_stream_init(ctx, &stream, "Cubeb stream", NULL, in_stm_params,
                            NULL, out_stm_params, latency_frames, data_cb,
                            state_cb, static_cast<void*>(id));
  assert(r == CUBEB_OK && "Error initializing cubeb stream");
  DEBUG_PRINT("Start stream %d @ %p, latency: %d, context @ %p\n", *id, stream,
              latency_frames, ctx);
  cubeb_stream_start(stream);
  streams[index] = stream;
}

void
stop_and_destroy_stream(cubeb_stream* stream)
{
  int* id = static_cast<int*>(cubeb_stream_user_ptr(stream));
  DEBUG_PRINT("Stop stream %d @ %p\n", *id, stream);
  cubeb_stream_stop(stream);
  cubeb_stream_destroy(stream);
}

void
run_streams_in_parallel(cubeb_device_type type, size_t number_of_streams,
                        vector<std::chrono::duration<double>>& times)
{
  DEBUG_PRINT("\n----- start -----\n");

  unique_ptr<stream_id[]> ids(new stream_id[number_of_streams]());
  for (size_t i = 0; i < number_of_streams; ++i) {
    ids[i] = static_cast<stream_id>(i + 1);
  }

  unique_ptr<cubeb_stream* []> streams(new cubeb_stream*[number_of_streams]());
  for (size_t i = 0; i < number_of_streams; ++i) {
    streams[i] = nullptr;
  }

  auto start = std::chrono::high_resolution_clock::now();

  cubeb* ctx;
  int r = init_backend(&ctx, "Cubeb duplex example");
  assert(r == CUBEB_OK && "Error initializing cubeb stream");
  unique_ptr<cubeb, decltype(&cubeb_destroy)> cleanup_cubeb_at_exit(
    ctx, cubeb_destroy);

  vector<std::thread> threads1;
  for (size_t i = 0; i < number_of_streams; ++i) {
    stream_id* id = &ids[i];
    threads1.push_back(std::thread(create_and_start_stream, ctx, STREAM_LATENCY,
                                   id, type, streams.get(), i));
  }

  for (std::thread& th : threads1) {
    assert(th.joinable());
    th.join();
  }

  vector<std::thread> threads2;
  for (size_t i = 0; i < number_of_streams; ++i) {
    cubeb_stream* stream = streams[i];
    threads2.push_back(std::thread(stop_and_destroy_stream, stream));
  }

  for (std::thread& th : threads2) {
    assert(th.joinable());
    th.join();
  }

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed = end - start;
  times.push_back(elapsed);

  DEBUG_PRINT("------ end ------\nTime: %lf\n", elapsed.count() * 1000);
}

template <class T>
T
average(vector<T>& values)
{
  T avg(0);
  for (size_t i = 0; i < values.size(); ++i) {
    avg += (values[i] - avg) / (i + 1);
  }
  return avg;
}

template <class T>
void
remove_extrema(vector<T>& values)
{
  // Truncate max element.
  auto max_iter = std::max_element(values.begin(), values.end());
  std::iter_swap(max_iter, std::prev(values.end()));
  values.pop_back();

  // Truncate min element.
  auto min_iter = std::min_element(values.begin(), values.end());
  std::iter_swap(min_iter, std::prev(values.end()));
  values.pop_back();
}

template <class T>
T
truncated_mean(vector<T>& values)
{
  remove_extrema(values);
  return average(values);
}

cubeb_device_type
get_stream_type_from_user_input()
{
  cout << "What type of stream you want to test?\n1: Input\n2: Output\n3: "
          "Duplex\nYour choice: ";
  int32_t type = -1;
  while (true) {
    cin >> type;
    if (type < 1 || type > 3) {
      cout << "Invalid type. Try again: ";
      continue;
    }
    break;
  }
  return static_cast<cubeb_device_type>(type);
}

uint32_t
get_streams_from_user_input()
{
  cout << "How many streams will be opened in parallel: ";
  int32_t streams = 0;
  while (true) {
    cin >> streams;
    if (streams < 1) {
      cout << "Minimal is 1. Try again: ";
      continue;
    }
    break;
  }
  return static_cast<uint32_t>(streams);
}

uint32_t
get_rounds_from_user_input()
{
  cout << "How many rounds of the test: ";
  int32_t rounds = 0;
  while (true) {
    cin >> rounds;
    if (rounds < 3) {
      cout << "Minimal is 3. Try again: ";
      continue;
    }
    break;
  }
  return static_cast<uint32_t>(rounds);
}

int
main(int argc, char* argv[])
{
#ifdef _WIN32
  CoInitialize(nullptr);
#endif

  cubeb_device_type type = get_stream_type_from_user_input();
  uint32_t streams = get_streams_from_user_input();
  uint32_t rounds = get_rounds_from_user_input();

  vector<std::chrono::duration<double>> times;
  while (rounds--) {
    run_streams_in_parallel(type, static_cast<size_t>(streams),
                            std::ref(times));
  }
  double avg = truncated_mean(times).count() * 1000;
  cout << "Average time: " << avg << " ms" << std::endl;

  return 0;
}