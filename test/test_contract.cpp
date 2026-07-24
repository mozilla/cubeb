/*
 * Copyright © 2026 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

/* test_contract.cpp - verify the cubeb API contract described in
   docs/api-contract.md. Every test references the clause IDs it checks
   (THR/CTX/INIT/LIFE/DATA/STATE/DRAIN/POS/PROP/DEV/ERR).

   MUST clauses use fatal ASSERT semantics or hard EXPECT checks; SHOULD
   clauses and clauses with tracked backend deviations use EXPECT with a
   "[SHOULD ...]" or "known deviation" annotation so they surface without
   masking other failures. Tests requiring audio input are gated on
   can_run_audio_input_test(); output tests require a default output
   device, like the rest of this suite. */

#include "gtest/gtest.h"
#if !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 600
#endif
#include "cubeb/cubeb.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdio.h>
#include <thread>
#include <vector>

// #define ENABLE_NORMAL_LOG
// #define ENABLE_VERBOSE_LOG
#include "common.h"

#define CONTRACT_RATE 48000
#define CONTRACT_TIMEOUT_MS 10000

enum class EvKind { Data, State };

struct Event {
  EvKind kind;
  cubeb_state state;
  long nframes;
  long ret;
  size_t tid_hash;
};

static size_t const MAX_EVENTS = 65536;

/* Per-stream callback recorder and behavior harness. Passed as user_ptr.
   Callback behavior knobs are indices into the sequence of data callbacks;
   -1 disables the knob. Events are appended lock-free (slot reservation via
   fetch_add); the full log is only inspected after cubeb_stream_destroy,
   whose quiescence guarantee (LIFE-9) makes plain reads safe. */
struct StreamHarness {
  // Behavior knobs.
  std::atomic<long> short_return_at{-1};
  std::atomic<long> zero_return_at{-1};
  std::atomic<long> error_return_at{-1};
  std::atomic<int> sleep_ms{0};

  // Stream shape, set before open_stream().
  bool has_output = true;
  bool has_input = false;
  cubeb_sample_format format = CUBEB_SAMPLE_FLOAT32NE;
  uint32_t out_channels = 2;
  uint32_t in_channels = 1;

  // Recording.
  std::vector<Event> events{MAX_EVENTS};
  std::atomic<size_t> reserved{0};
  std::atomic<uint64_t> data_cbs{0};
  std::atomic<uint64_t> frames_written{0};
  std::atomic<int> started{0};
  std::atomic<int> stopped{0};
  std::atomic<int> drained{0};
  std::atomic<int> errored{0};

  // Violation counters, asserted zero by expect_no_violations().
  std::atomic<int> data_after_terminal{0};
  std::atomic<int> cb_after_destroy{0};
  std::atomic<int> bad_callback_args{0};
  std::atomic<int> nested_state_cb{0};

  std::atomic<bool> destroyed{false};
  std::atomic<bool> terminal_return_seen{false};
  std::atomic<int> data_cb_depth{0};
  std::atomic<size_t> data_cb_tid{0};

  void record(Event ev)
  {
    size_t i = reserved.fetch_add(1);
    if (i < MAX_EVENTS) {
      events[i] = ev;
    }
  }

  size_t event_count() const
  {
    size_t n = reserved.load();
    return n < MAX_EVENTS ? n : MAX_EVENTS;
  }
};

static size_t
tid_hash()
{
  return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

static long
contract_data_cb(cubeb_stream * stm, void * user_ptr, void const * input,
                 void * output, long nframes)
{
  StreamHarness * h = static_cast<StreamHarness *>(user_ptr);
  long index = static_cast<long>(h->data_cbs.fetch_add(1));

  if (h->destroyed.load()) {
    h->cb_after_destroy++; // LIFE-9
  }
  if (h->terminal_return_seen.load()) {
    h->data_after_terminal++; // DATA-4/DATA-5/DATA-6
  }
  // DATA-1: nframes >= 1, buffer non-nullness matches the stream shape.
  if (nframes < 1 || !stm || (h->has_output && !output) ||
      (!h->has_output && output) || (h->has_input && !input) ||
      (!h->has_input && input)) {
    h->bad_callback_args++;
  }

  h->data_cb_tid.store(tid_hash());
  h->data_cb_depth++;

  if (int ms = h->sleep_ms.load()) {
    delay(ms);
  }
  if (output) {
    size_t sample_size = h->format == CUBEB_SAMPLE_FLOAT32NE ? 4 : 2;
    memset(output, 0, nframes * h->out_channels * sample_size);
  }

  long ret = nframes;
  if (index == h->error_return_at.load()) {
    h->terminal_return_seen.store(true);
    ret = CUBEB_ERROR;
  } else if (index == h->zero_return_at.load()) {
    h->terminal_return_seen.store(true);
    ret = 0;
  } else if (index == h->short_return_at.load()) {
    h->terminal_return_seen.store(true);
    ret = nframes - 1;
  }
  if (ret > 0) {
    h->frames_written += ret;
  }

  h->data_cb_depth--;
  h->record(
      {EvKind::Data, CUBEB_STATE_ERROR /* unused */, nframes, ret, tid_hash()});
  return ret;
}

static void
contract_state_cb(cubeb_stream * /*stm*/, void * user_ptr, cubeb_state state)
{
  StreamHarness * h = static_cast<StreamHarness *>(user_ptr);
  if (h->destroyed.load()) {
    h->cb_after_destroy++; // LIFE-9
  }
  // THR-6: a state callback must not be nested inside this stream's data
  // callback on the same thread.
  if (h->data_cb_depth.load() > 0 && h->data_cb_tid.load() == tid_hash()) {
    h->nested_state_cb++;
  }
  switch (state) {
  case CUBEB_STATE_STARTED:
    h->started++;
    break;
  case CUBEB_STATE_STOPPED:
    h->stopped++;
    break;
  case CUBEB_STATE_DRAINED:
    h->drained++;
    break;
  case CUBEB_STATE_ERROR:
    h->errored++;
    break;
  }
  h->record({EvKind::State, state, 0, 0, tid_hash()});
}

/* SHOULD clauses and tracked deviations are reported without failing the
   test, per the baseline-plus-tightening model in docs/api-contract.md. */
#define CONTRACT_SHOULD(expr, msg)                                             \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "[CONTRACT SHOULD-DEVIATION] %s: %s\n", #expr, msg);     \
    }                                                                          \
  } while (0)

/* winmm is a legacy backend kept for reference; test_contract crashes it
   (fatal assert in cubeb_winmm.c teardown after a data-callback error
   return), so the suite skips it entirely rather than tracking it in the
   deviation ledger. */
#define SKIP_IF_LEGACY_BACKEND()                                               \
  do {                                                                         \
    char const * b = getenv("CUBEB_BACKEND");                                  \
    if (b && strcmp(b, "winmm") == 0) {                                        \
      GTEST_SKIP() << "winmm is not a contract-conforming backend";            \
    }                                                                          \
  } while (0)

static bool
backend_is(cubeb * ctx, char const * name)
{
  return strcmp(cubeb_get_backend_id(ctx), name) == 0;
}

/* Tracked per-backend deviations from docs/api-contract.md Annex A,
   confirmed empirically by this suite. Checks gated on an entry here are
   reported via CONTRACT_SHOULD instead of failing. Remove entries as
   backend fixes land; the corresponding checks then become fatal. */
static struct {
  char const * backend;
  char const * clause;
} const KNOWN_DEVIATIONS[] = {
    // stop() does not quiesce data callbacks.
    {"wasapi", "LIFE-6"},
    {"aaudio", "LIFE-6"},
    // stop() on an errored stream returns CUBEB_ERROR.
    {"aaudio", "LIFE-4"},
    // Position exceeds frames written after drain (the played-out
    // silence tail is counted by getFramesRead).
    {"aaudio", "POS-4"},
    // Position estimate keeps interpolating briefly after stop, even
    // once STOPPED has been delivered.
    {"aaudio", "POS-5"},
    // DRAINED delivered after stop returns / from the destroy path.
    {"wasapi", "STATE-4"},
    {"pulse", "STATE-4"},
    {"pulse-rust", "STATE-4"},
    // No DRAINED delivered for input-only short return.
    {"wasapi", "DATA-5"},
    {"pulse", "DATA-5"},
    {"pulse-rust", "DATA-5"},
    // Position nonzero before start; exceeds frames written; advances
    // while stopped (PA interpolated clock, no clamping).
    {"pulse", "POS-2"},
    {"pulse-rust", "POS-2"},
    {"pulse", "POS-4"},
    {"pulse-rust", "POS-4"},
    {"pulse", "POS-5"},
    {"pulse-rust", "POS-5"},
    // register_device_changed_callback returns ERROR, not NOT_SUPPORTED.
    {"pulse", "ERR-2"},
    {"pulse-rust", "ERR-2"},
    // Collection-changed re-registration silently replaces (pulse) or
    // asserts (audiounit) instead of returning INVALID_PARAMETER.
    {"pulse", "DEV-6"},
    {"pulse-rust", "DEV-6"},
    {"audiounit", "DEV-6"},
    // Device-changed double registration asserts instead of erroring.
    {"audiounit", "PROP-5"},
};

static bool
known_deviation(cubeb * ctx, char const * clause)
{
  char const * id = cubeb_get_backend_id(ctx);
  for (auto const & d : KNOWN_DEVIATIONS) {
    if (strcmp(d.backend, id) == 0 && strcmp(d.clause, clause) == 0) {
      return true;
    }
  }
  return false;
}

/* Assert `expr` unless `clause` is a tracked deviation for the current
   backend, in which case report without failing. */
#define CONTRACT_EXPECT(ctx, clause, expr, msg)                                \
  do {                                                                         \
    if (known_deviation(ctx, clause)) {                                        \
      CONTRACT_SHOULD(expr, msg);                                              \
    } else {                                                                   \
      EXPECT_TRUE(expr) << msg;                                                \
    }                                                                          \
  } while (0)

template <typename Pred>
static bool
wait_for(Pred pred, int timeout_ms = CONTRACT_TIMEOUT_MS)
{
  int waited = 0;
  while (!pred()) {
    if (waited >= timeout_ms) {
      return false;
    }
    delay(10);
    waited += 10;
  }
  return true;
}

static int
open_stream(cubeb * ctx, StreamHarness * h, cubeb_stream ** stm,
            char const * name)
{
  cubeb_stream_params out_params = {};
  out_params.format = h->format;
  out_params.rate = CONTRACT_RATE;
  out_params.channels = h->out_channels;
  out_params.layout = CUBEB_LAYOUT_UNDEFINED;
  out_params.prefs = CUBEB_STREAM_PREF_NONE;

  cubeb_stream_params in_params = {};
  in_params.format = h->format;
  in_params.rate = CONTRACT_RATE;
  in_params.channels = h->in_channels;
  in_params.layout = CUBEB_LAYOUT_UNDEFINED;
  in_params.prefs = CUBEB_STREAM_PREF_NONE;

  uint32_t latency = 1024;
  cubeb_get_min_latency(ctx, h->has_output ? &out_params : &in_params,
                        &latency);
  if (latency < 1 || latency > 96000) {
    latency = 1024;
  }

  return cubeb_stream_init(ctx, stm, name, NULL,
                           h->has_input ? &in_params : NULL, NULL,
                           h->has_output ? &out_params : NULL, latency,
                           contract_data_cb, contract_state_cb, h);
}

// Post-destroy log inspection helpers.
static long
first_index_of_state(StreamHarness const & h, cubeb_state s)
{
  for (size_t i = 0; i < h.event_count(); i++) {
    if (h.events[i].kind == EvKind::State && h.events[i].state == s) {
      return static_cast<long>(i);
    }
  }
  return -1;
}

static long
last_index_of_data(StreamHarness const & h)
{
  long last = -1;
  for (size_t i = 0; i < h.event_count(); i++) {
    if (h.events[i].kind == EvKind::Data) {
      last = static_cast<long>(i);
    }
  }
  return last;
}

static void
expect_no_violations(StreamHarness const & h)
{
  EXPECT_EQ(h.data_after_terminal.load(), 0)
      << "DATA-4/5/6: data callback fired after a terminal return";
  EXPECT_EQ(h.cb_after_destroy.load(), 0)
      << "LIFE-9: callback fired after cubeb_stream_destroy returned";
  EXPECT_EQ(h.bad_callback_args.load(), 0)
      << "DATA-1: bad nframes/buffer arguments in data callback";
  EXPECT_EQ(h.nested_state_cb.load(), 0)
      << "THR-6: state callback nested inside data callback";
}

// CTX-3, CTX-4: backend id stability; context query result domain.
TEST(cubeb, contract_context_queries)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);

  char const * id = cubeb_get_backend_id(ctx);
  ASSERT_NE(id, nullptr);
  ASSERT_GT(strlen(id), 0u);
  ASSERT_STREQ(id, cubeb_get_backend_id(ctx));

  uint32_t max_channels = 0;
  int r = cubeb_get_max_channel_count(ctx, &max_channels);
  ASSERT_TRUE(r == CUBEB_OK || r == CUBEB_ERROR_NOT_SUPPORTED ||
              r == CUBEB_ERROR);
  if (r == CUBEB_OK) {
    ASSERT_GT(max_channels, 0u);
  }

  cubeb_stream_params params = {};
  params.format = CUBEB_SAMPLE_FLOAT32NE;
  params.rate = CONTRACT_RATE;
  params.channels = 2;
  params.layout = CUBEB_LAYOUT_UNDEFINED;
  uint32_t latency = 0;
  r = cubeb_get_min_latency(ctx, &params, &latency);
  ASSERT_TRUE(r == CUBEB_OK || r == CUBEB_ERROR_NOT_SUPPORTED ||
              r == CUBEB_ERROR);
  if (r == CUBEB_OK) {
    ASSERT_GT(latency, 0u);
  }

  uint32_t rate = 0;
  r = cubeb_get_preferred_sample_rate(ctx, &rate);
  ASSERT_TRUE(r == CUBEB_OK || r == CUBEB_ERROR_NOT_SUPPORTED ||
              r == CUBEB_ERROR);
  if (r == CUBEB_OK) {
    ASSERT_GT(rate, 0u);
  }

  cubeb_destroy(ctx);
}

// INIT-1, ERR-4: common-layer validation returns the documented codes.
TEST(cubeb, contract_init_validation)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);

  cubeb_stream_params good = {};
  good.format = CUBEB_SAMPLE_FLOAT32NE;
  good.rate = CONTRACT_RATE;
  good.channels = 2;
  good.layout = CUBEB_LAYOUT_UNDEFINED;

  cubeb_stream * stm = nullptr;
  StreamHarness h;

  // NULL callbacks -> INVALID_PARAMETER.
  ASSERT_EQ(cubeb_stream_init(ctx, &stm, "c", NULL, NULL, NULL, &good, 1024,
                              NULL, contract_state_cb, &h),
            CUBEB_ERROR_INVALID_PARAMETER);
  ASSERT_EQ(cubeb_stream_init(ctx, &stm, "c", NULL, NULL, NULL, &good, 1024,
                              contract_data_cb, NULL, &h),
            CUBEB_ERROR_INVALID_PARAMETER);

  // Out-of-range rate/channels -> INVALID_FORMAT.
  cubeb_stream_params bad = good;
  bad.rate = 999;
  ASSERT_EQ(cubeb_stream_init(ctx, &stm, "c", NULL, NULL, NULL, &bad, 1024,
                              contract_data_cb, contract_state_cb, &h),
            CUBEB_ERROR_INVALID_FORMAT);
  bad = good;
  bad.rate = 768001;
  ASSERT_EQ(cubeb_stream_init(ctx, &stm, "c", NULL, NULL, NULL, &bad, 1024,
                              contract_data_cb, contract_state_cb, &h),
            CUBEB_ERROR_INVALID_FORMAT);
  bad = good;
  bad.channels = 0;
  ASSERT_EQ(cubeb_stream_init(ctx, &stm, "c", NULL, NULL, NULL, &bad, 1024,
                              contract_data_cb, contract_state_cb, &h),
            CUBEB_ERROR_INVALID_FORMAT);
  bad = good;
  bad.channels = 256;
  ASSERT_EQ(cubeb_stream_init(ctx, &stm, "c", NULL, NULL, NULL, &bad, 1024,
                              contract_data_cb, contract_state_cb, &h),
            CUBEB_ERROR_INVALID_FORMAT);

  // Out-of-range latency -> INVALID_PARAMETER.
  ASSERT_EQ(cubeb_stream_init(ctx, &stm, "c", NULL, NULL, NULL, &good, 0,
                              contract_data_cb, contract_state_cb, &h),
            CUBEB_ERROR_INVALID_PARAMETER);
  ASSERT_EQ(cubeb_stream_init(ctx, &stm, "c", NULL, NULL, NULL, &good, 96001,
                              contract_data_cb, contract_state_cb, &h),
            CUBEB_ERROR_INVALID_PARAMETER);

  // Duplex rate mismatch -> INVALID_FORMAT.
  cubeb_stream_params in_mismatch = good;
  in_mismatch.rate = CONTRACT_RATE / 2;
  in_mismatch.channels = 1;
  ASSERT_EQ(cubeb_stream_init(ctx, &stm, "c", NULL, &in_mismatch, NULL, &good,
                              1024, contract_data_cb, contract_state_cb, &h),
            CUBEB_ERROR_INVALID_FORMAT);

  ASSERT_EQ(stm, nullptr) << "INIT-8: failed init must not produce a stream";
  cubeb_destroy(ctx);
}

// INIT-7, DATA-1: user_ptr round-trip through the API and callbacks.
TEST(cubeb, contract_user_ptr_roundtrip)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  StreamHarness h;
  cubeb_stream * stm = nullptr;
  ASSERT_EQ(open_stream(ctx, &h, &stm, "contract user_ptr"), CUBEB_OK);

  ASSERT_EQ(cubeb_stream_user_ptr(stm), &h);

  ASSERT_EQ(cubeb_stream_start(stm), CUBEB_OK);
  EXPECT_TRUE(wait_for([&] { return h.data_cbs.load() >= 2; }))
      << "data callback never fired";
  ASSERT_EQ(cubeb_stream_stop(stm), CUBEB_OK);
  cubeb_stream_destroy(stm);
  h.destroyed.store(true);

  expect_no_violations(h);
  cubeb_destroy(ctx);
}

// LIFE-1, LIFE-5, STATE-1..3: basic start/stop transition set and order.
TEST(cubeb, contract_state_order_basic)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  StreamHarness h;
  cubeb_stream * stm = nullptr;
  ASSERT_EQ(open_stream(ctx, &h, &stm, "contract state order"), CUBEB_OK);

  ASSERT_EQ(cubeb_stream_start(stm), CUBEB_OK);
  EXPECT_TRUE(wait_for([&] { return h.started.load() >= 1; }))
      << "LIFE-1: STARTED not delivered";
  EXPECT_TRUE(wait_for([&] { return h.data_cbs.load() >= 3; }));
  ASSERT_EQ(cubeb_stream_stop(stm), CUBEB_OK);
  EXPECT_TRUE(wait_for([&] { return h.stopped.load() >= 1; }))
      << "LIFE-5: STOPPED not delivered";
  cubeb_stream_destroy(stm);
  h.destroyed.store(true);

  EXPECT_GE(h.started.load(), 1);
  EXPECT_GE(h.stopped.load(), 1);
  EXPECT_EQ(h.drained.load(), 0) << "STATE-6: spurious DRAINED";
  EXPECT_EQ(h.errored.load(), 0) << "STATE-6: spurious ERROR";
  long started_at = first_index_of_state(h, CUBEB_STATE_STARTED);
  long stopped_at = first_index_of_state(h, CUBEB_STATE_STOPPED);
  ASSERT_GE(started_at, 0);
  ASSERT_GE(stopped_at, 0);
  EXPECT_LT(started_at, stopped_at) << "STARTED must precede STOPPED";
  expect_no_violations(h);
  cubeb_destroy(ctx);
}

// LIFE-4, LIFE-7: stop is idempotent; duplicate STOPPED is legal.
TEST(cubeb, contract_stop_idempotent)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  StreamHarness h;
  cubeb_stream * stm = nullptr;
  ASSERT_EQ(open_stream(ctx, &h, &stm, "contract stop idempotent"), CUBEB_OK);

  ASSERT_EQ(cubeb_stream_start(stm), CUBEB_OK);
  EXPECT_TRUE(wait_for([&] { return h.data_cbs.load() >= 1; }));
  ASSERT_EQ(cubeb_stream_stop(stm), CUBEB_OK);
  ASSERT_EQ(cubeb_stream_stop(stm), CUBEB_OK) << "LIFE-4: repeated stop";
  ASSERT_EQ(cubeb_stream_stop(stm), CUBEB_OK) << "LIFE-4: repeated stop";
  cubeb_stream_destroy(stm);
  h.destroyed.store(true);

  expect_no_violations(h);
  cubeb_destroy(ctx);
}

// LIFE-3: double start must not crash or wedge the stream.
TEST(cubeb, contract_double_start)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  StreamHarness h;
  cubeb_stream * stm = nullptr;
  ASSERT_EQ(open_stream(ctx, &h, &stm, "contract double start"), CUBEB_OK);

  ASSERT_EQ(cubeb_stream_start(stm), CUBEB_OK);
  int r = cubeb_stream_start(stm);
  EXPECT_TRUE(r == CUBEB_OK || r == CUBEB_ERROR)
      << "LIFE-3: double start returned " << r;
  uint64_t before = h.data_cbs.load();
  EXPECT_TRUE(wait_for([&] { return h.data_cbs.load() > before; }))
      << "LIFE-3: stream wedged after double start";
  ASSERT_EQ(cubeb_stream_stop(stm), CUBEB_OK);
  cubeb_stream_destroy(stm);
  h.destroyed.store(true);

  expect_no_violations(h);
  cubeb_destroy(ctx);
}

// LIFE-6 (ratified strong stop): no data callback once stop has returned.
// Known deviations: wasapi, aaudio.
TEST(cubeb, contract_stop_quiescence)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  StreamHarness h;
  h.sleep_ms.store(5);
  cubeb_stream * stm = nullptr;
  ASSERT_EQ(open_stream(ctx, &h, &stm, "contract stop quiescence"), CUBEB_OK);

  ASSERT_EQ(cubeb_stream_start(stm), CUBEB_OK);
  EXPECT_TRUE(wait_for([&] { return h.data_cbs.load() >= 5; }));
  ASSERT_EQ(cubeb_stream_stop(stm), CUBEB_OK);
  uint64_t at_stop = h.data_cbs.load();
  delay(500);
  CONTRACT_EXPECT(ctx, "LIFE-6", h.data_cbs.load() == at_stop,
                  "LIFE-6: data callback fired after cubeb_stream_stop "
                  "returned");
  cubeb_stream_destroy(stm);
  h.destroyed.store(true);

  expect_no_violations(h);
  cubeb_destroy(ctx);
}

// LIFE-9, LIFE-10: destroy reaches quiescence, called from another thread
// while data callbacks are in flight.
TEST(cubeb, contract_destroy_quiescence)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  StreamHarness h;
  h.sleep_ms.store(20);
  cubeb_stream * stm = nullptr;
  ASSERT_EQ(open_stream(ctx, &h, &stm, "contract destroy quiescence"),
            CUBEB_OK);

  ASSERT_EQ(cubeb_stream_start(stm), CUBEB_OK);
  EXPECT_TRUE(wait_for([&] { return h.data_cbs.load() >= 3; }));

  std::thread t([&] {
    EXPECT_EQ(cubeb_stream_stop(stm), CUBEB_OK);
    cubeb_stream_destroy(stm);
    h.destroyed.store(true);
  });
  t.join();
  delay(500);

  expect_no_violations(h);
  cubeb_destroy(ctx);
}

// DATA-4, DRAIN-1..3, STATE-4: short return drains; DRAINED ordering and
// position convergence.
TEST(cubeb, contract_drain_short_return)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  StreamHarness h;
  h.short_return_at.store(5);
  cubeb_stream * stm = nullptr;
  ASSERT_EQ(open_stream(ctx, &h, &stm, "contract drain"), CUBEB_OK);

  ASSERT_EQ(cubeb_stream_start(stm), CUBEB_OK);
  ASSERT_TRUE(wait_for([&] { return h.drained.load() >= 1; }, 15000))
      << "DRAIN-2: DRAINED not delivered after short return";

  uint64_t position = 0;
  ASSERT_EQ(cubeb_stream_get_position(stm, &position), CUBEB_OK);
  uint64_t written = h.frames_written.load();
  CONTRACT_EXPECT(ctx, "POS-4", position <= written,
                  "POS-4: position beyond frames written");
  CONTRACT_SHOULD(position == written,
                  "DRAIN-3: position should converge to frames written "
                  "after drain");

  // POS-5: position is frozen after drain.
  uint64_t p1 = 0, p2 = 0;
  ASSERT_EQ(cubeb_stream_get_position(stm, &p1), CUBEB_OK);
  delay(200);
  ASSERT_EQ(cubeb_stream_get_position(stm, &p2), CUBEB_OK);
  CONTRACT_EXPECT(ctx, "POS-5", p1 == p2,
                  "POS-5: position moved after DRAINED");

  ASSERT_EQ(cubeb_stream_stop(stm), CUBEB_OK) << "DRAIN-4: stop after drain";
  cubeb_stream_destroy(stm);
  h.destroyed.store(true);

  EXPECT_EQ(h.drained.load(), 1) << "STATE-4: DRAINED delivered exactly once";
  EXPECT_EQ(h.errored.load(), 0);
  long drained_at = first_index_of_state(h, CUBEB_STATE_DRAINED);
  long last_data = last_index_of_data(h);
  ASSERT_GE(drained_at, 0);
  ASSERT_GE(last_data, 0);
  EXPECT_LT(last_data, drained_at)
      << "STATE-4: data callback recorded after DRAINED";
  expect_no_violations(h);
  cubeb_destroy(ctx);
}

// DRAIN-5, STATE-4: draining from the very first callback.
TEST(cubeb, contract_drain_immediate)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  StreamHarness h;
  h.zero_return_at.store(0);
  cubeb_stream * stm = nullptr;
  ASSERT_EQ(open_stream(ctx, &h, &stm, "contract drain immediate"), CUBEB_OK);

  ASSERT_EQ(cubeb_stream_start(stm), CUBEB_OK);
  ASSERT_TRUE(wait_for([&] { return h.drained.load() >= 1; }))
      << "DRAIN-5: DRAINED not delivered after immediate zero return";
  ASSERT_EQ(cubeb_stream_stop(stm), CUBEB_OK);
  cubeb_stream_destroy(stm);
  h.destroyed.store(true);

  EXPECT_EQ(h.data_cbs.load(), 1u)
      << "DATA-4: data callback invoked again after drain-triggering return";
  EXPECT_EQ(h.drained.load(), 1);
  expect_no_violations(h);
  cubeb_destroy(ctx);
}

// DATA-6, STATE-5, LIFE-11: CUBEB_ERROR return terminates the stream.
TEST(cubeb, contract_error_return_terminates)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  StreamHarness h;
  h.error_return_at.store(3);
  cubeb_stream * stm = nullptr;
  ASSERT_EQ(open_stream(ctx, &h, &stm, "contract error return"), CUBEB_OK);

  ASSERT_EQ(cubeb_stream_start(stm), CUBEB_OK);
  ASSERT_TRUE(wait_for([&] { return h.errored.load() >= 1; }))
      << "DATA-6: ERROR state not delivered after CUBEB_ERROR return";
  CONTRACT_EXPECT(ctx, "LIFE-4", cubeb_stream_stop(stm) == CUBEB_OK,
                  "LIFE-4/LIFE-11: stop on an errored stream must succeed");
  cubeb_stream_destroy(stm);
  h.destroyed.store(true);

  EXPECT_EQ(h.errored.load(), 1) << "STATE-5: ERROR delivered exactly once";
  EXPECT_EQ(h.drained.load(), 0);
  expect_no_violations(h);
  cubeb_destroy(ctx);
}

// POS-2: position is zero before the stream is started.
TEST(cubeb, contract_position_before_start)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  StreamHarness h;
  cubeb_stream * stm = nullptr;
  ASSERT_EQ(open_stream(ctx, &h, &stm, "contract position prestart"), CUBEB_OK);

  uint64_t position = 1;
  ASSERT_EQ(cubeb_stream_get_position(stm, &position), CUBEB_OK);
  CONTRACT_EXPECT(ctx, "POS-2", position == 0,
                  "POS-2: nonzero position before start");
  delay(200);
  ASSERT_EQ(cubeb_stream_get_position(stm, &position), CUBEB_OK);
  CONTRACT_EXPECT(ctx, "POS-2", position == 0,
                  "POS-2: position advanced without start");

  cubeb_stream_destroy(stm);
  h.destroyed.store(true);
  expect_no_violations(h);
  cubeb_destroy(ctx);
}

// POS-3, POS-4, THR-4: position monotonic and bounded by frames written,
// queried concurrently with the running data callback.
TEST(cubeb, contract_position_monotonic)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  StreamHarness h;
  cubeb_stream * stm = nullptr;
  ASSERT_EQ(open_stream(ctx, &h, &stm, "contract position monotonic"),
            CUBEB_OK);

  ASSERT_EQ(cubeb_stream_start(stm), CUBEB_OK);
  EXPECT_TRUE(wait_for([&] { return h.data_cbs.load() >= 1; }));

  uint64_t prev = 0;
  bool pos4_gated = known_deviation(ctx, "POS-4");
  bool pos4_reported = false;
  for (int i = 0; i < 200; i++) {
    uint64_t position = 0;
    ASSERT_EQ(cubeb_stream_get_position(stm, &position), CUBEB_OK);
    ASSERT_GE(position, prev) << "POS-3: position went backwards";
    // frames_written only grows, so sampling it after the position read
    // keeps this an upper bound.
    uint64_t written = h.frames_written.load();
    if (pos4_gated) {
      if (position > written && !pos4_reported) {
        CONTRACT_SHOULD(false, "POS-4: position beyond frames written");
        pos4_reported = true;
      }
    } else {
      ASSERT_LE(position, written) << "POS-4: position beyond frames written";
    }
    prev = position;
    delay(10);
  }
  EXPECT_GT(prev, 0u) << "POS-5: position never advanced while started";

  ASSERT_EQ(cubeb_stream_stop(stm), CUBEB_OK);
  cubeb_stream_destroy(stm);
  h.destroyed.store(true);
  expect_no_violations(h);
  cubeb_destroy(ctx);
}

// POS-5, POS-6: position frozen while stopped, resumes monotonically.
TEST(cubeb, contract_position_frozen_when_stopped)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  StreamHarness h;
  cubeb_stream * stm = nullptr;
  ASSERT_EQ(open_stream(ctx, &h, &stm, "contract position frozen"), CUBEB_OK);

  ASSERT_EQ(cubeb_stream_start(stm), CUBEB_OK);
  uint64_t position = 0;
  ASSERT_TRUE(wait_for([&] {
    return cubeb_stream_get_position(stm, &position) == CUBEB_OK &&
           position > 0;
  })) << "POS-5: position never advanced";

  ASSERT_EQ(cubeb_stream_stop(stm), CUBEB_OK);
  ASSERT_TRUE(wait_for([&] { return h.stopped.load() >= 1; }));
  uint64_t p1 = 0, p2 = 0;
  ASSERT_EQ(cubeb_stream_get_position(stm, &p1), CUBEB_OK);
  delay(300);
  ASSERT_EQ(cubeb_stream_get_position(stm, &p2), CUBEB_OK);
  CONTRACT_EXPECT(ctx, "POS-5", p1 == p2,
                  "POS-5: position moved while stopped");

  ASSERT_EQ(cubeb_stream_start(stm), CUBEB_OK);
  uint64_t p3 = 0;
  EXPECT_TRUE(wait_for([&] {
    return cubeb_stream_get_position(stm, &p3) == CUBEB_OK && p3 > p1;
  })) << "POS-6: position did not resume advancing after restart";
  EXPECT_GE(p3, p1) << "POS-3: position went backwards across stop/start";

  ASSERT_EQ(cubeb_stream_stop(stm), CUBEB_OK);
  cubeb_stream_destroy(stm);
  h.destroyed.store(true);
  expect_no_violations(h);
  cubeb_destroy(ctx);
}

// LAT-1, LAT-2: latency queries; input latency must fail on output-only.
TEST(cubeb, contract_latency_queries)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  StreamHarness h;
  cubeb_stream * stm = nullptr;
  ASSERT_EQ(open_stream(ctx, &h, &stm, "contract latency"), CUBEB_OK);

  ASSERT_EQ(cubeb_stream_start(stm), CUBEB_OK);
  EXPECT_TRUE(wait_for([&] { return h.data_cbs.load() >= 2; }));

  uint32_t latency = 0;
  int r = cubeb_stream_get_latency(stm, &latency);
  EXPECT_TRUE(r == CUBEB_OK || r == CUBEB_ERROR_NOT_SUPPORTED)
      << "LAT-1: get_latency returned " << r;
  if (r == CUBEB_OK) {
    CONTRACT_SHOULD(latency > 0, "LAT-1: zero reported output latency");
  }

  uint32_t input_latency = 0;
  ASSERT_NE(cubeb_stream_get_input_latency(stm, &input_latency), CUBEB_OK)
      << "LAT-2: get_input_latency must fail on an output-only stream";

  ASSERT_EQ(cubeb_stream_stop(stm), CUBEB_OK);
  cubeb_stream_destroy(stm);
  h.destroyed.store(true);
  expect_no_violations(h);
  cubeb_destroy(ctx);
}

// PROP-1: volume range validation is central and exact.
TEST(cubeb, contract_volume_validation)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  StreamHarness h;
  cubeb_stream * stm = nullptr;
  ASSERT_EQ(open_stream(ctx, &h, &stm, "contract volume"), CUBEB_OK);

  ASSERT_EQ(cubeb_stream_set_volume(stm, -0.5f), CUBEB_ERROR_INVALID_PARAMETER);
  ASSERT_EQ(cubeb_stream_set_volume(stm, 1.5f), CUBEB_ERROR_INVALID_PARAMETER);
  int r = cubeb_stream_set_volume(stm, 0.5f);
  EXPECT_TRUE(r == CUBEB_OK || r == CUBEB_ERROR_NOT_SUPPORTED)
      << "PROP-1: set_volume(0.5) returned " << r;

  cubeb_stream_destroy(stm);
  h.destroyed.store(true);
  expect_no_violations(h);
  cubeb_destroy(ctx);
}

// PROP-2, PROP-3, ERR-2: optional stream properties fail cleanly.
TEST(cubeb, contract_optional_properties)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  StreamHarness h;
  cubeb_stream * stm = nullptr;
  ASSERT_EQ(open_stream(ctx, &h, &stm, "contract optional props"), CUBEB_OK);

  int r = cubeb_stream_set_name(stm, "contract renamed");
  EXPECT_TRUE(r == CUBEB_OK || r == CUBEB_ERROR_NOT_SUPPORTED)
      << "PROP-2/ERR-2: set_name returned " << r;

  cubeb_device * device = nullptr;
  r = cubeb_stream_get_current_device(stm, &device);
  EXPECT_TRUE(r == CUBEB_OK || r == CUBEB_ERROR_NOT_SUPPORTED)
      << "PROP-3/ERR-2: get_current_device returned " << r;
  if (r == CUBEB_OK) {
    ASSERT_NE(device, nullptr);
    ASSERT_EQ(cubeb_stream_device_destroy(stm, device), CUBEB_OK);
  }

  cubeb_stream_destroy(stm);
  h.destroyed.store(true);
  expect_no_violations(h);
  cubeb_destroy(ctx);
}

static void
noop_device_changed_cb(void * /*user_ptr*/)
{
}

// PROP-5: device-changed callback registration bookkeeping.
TEST(cubeb, contract_device_changed_registration)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  StreamHarness h;
  cubeb_stream * stm = nullptr;
  ASSERT_EQ(open_stream(ctx, &h, &stm, "contract device changed"), CUBEB_OK);

  int r = cubeb_stream_register_device_changed_callback(stm,
                                                        noop_device_changed_cb);
  if (r == CUBEB_ERROR_NOT_SUPPORTED ||
      (r == CUBEB_ERROR && known_deviation(ctx, "ERR-2"))) {
    if (r == CUBEB_ERROR) {
      CONTRACT_SHOULD(false, "ERR-2: register_device_changed_callback "
                             "returns ERROR instead of NOT_SUPPORTED");
    }
    cubeb_stream_destroy(stm);
    h.destroyed.store(true);
    cubeb_destroy(ctx);
    GTEST_SKIP() << "register_device_changed_callback not supported";
  }
  ASSERT_EQ(r, CUBEB_OK);
  if (known_deviation(ctx, "PROP-5")) {
    // The C++ audiounit backend asserts (aborts in debug) on double
    // registration instead of returning an error; do not attempt it.
    CONTRACT_SHOULD(false, "PROP-5: double registration asserts instead "
                           "of returning INVALID_PARAMETER");
  } else {
    EXPECT_EQ(cubeb_stream_register_device_changed_callback(
                  stm, noop_device_changed_cb),
              CUBEB_ERROR_INVALID_PARAMETER)
        << "PROP-5: double registration must fail";
  }
  ASSERT_EQ(cubeb_stream_register_device_changed_callback(stm, NULL), CUBEB_OK);
  ASSERT_EQ(cubeb_stream_register_device_changed_callback(
                stm, noop_device_changed_cb),
            CUBEB_OK)
      << "PROP-5: re-registration after unregister must succeed";
  ASSERT_EQ(cubeb_stream_register_device_changed_callback(stm, NULL), CUBEB_OK);

  cubeb_stream_destroy(stm);
  h.destroyed.store(true);
  expect_no_violations(h);
  cubeb_destroy(ctx);
}

// DEV-1, DEV-2, DEV-3, DEV-5: enumeration snapshot semantics, devid
// stability, field population, and no perturbation from stream open/close.
TEST(cubeb, contract_enumeration)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);

  cubeb_device_collection first = {};
  int r = cubeb_enumerate_devices(ctx, CUBEB_DEVICE_TYPE_OUTPUT, &first);
  if (r == CUBEB_ERROR_NOT_SUPPORTED) {
    cubeb_destroy(ctx);
    GTEST_SKIP() << "device enumeration not supported";
  }
  ASSERT_EQ(r, CUBEB_OK);

  // DEV-1: destroying a zeroed collection succeeds (on backends that
  // support enumeration; without it the common layer returns
  // NOT_SUPPORTED before the empty short-circuit).
  cubeb_device_collection empty = {};
  ASSERT_EQ(cubeb_device_collection_destroy(ctx, &empty), CUBEB_OK);

  for (size_t i = 0; i < first.count; i++) {
    cubeb_device_info const & info = first.device[i];
    EXPECT_EQ(info.type, CUBEB_DEVICE_TYPE_OUTPUT);
    EXPECT_NE(info.device_id, nullptr) << "DEV-3: missing device_id";
    if (info.state == CUBEB_DEVICE_STATE_ENABLED) {
      EXPECT_GT(info.max_channels, 0u) << "DEV-3: enabled device without "
                                          "channels";
      EXPECT_LE(info.min_rate, info.max_rate) << "DEV-3: rate range inverted";
    }
  }

  // DEV-5: opening and closing a stream does not perturb the device set.
  StreamHarness h;
  cubeb_stream * stm = nullptr;
  if (open_stream(ctx, &h, &stm, "contract enumeration") == CUBEB_OK) {
    ASSERT_EQ(cubeb_stream_start(stm), CUBEB_OK);
    EXPECT_TRUE(wait_for([&] { return h.data_cbs.load() >= 1; }));
    ASSERT_EQ(cubeb_stream_stop(stm), CUBEB_OK);
    cubeb_stream_destroy(stm);
    h.destroyed.store(true);
  }

  cubeb_device_collection second = {};
  ASSERT_EQ(cubeb_enumerate_devices(ctx, CUBEB_DEVICE_TYPE_OUTPUT, &second),
            CUBEB_OK);
  EXPECT_EQ(first.count, second.count)
      << "DEV-5: device count changed across stream open/close";
  if (first.count == second.count) {
    for (size_t i = 0; i < first.count; i++) {
      // DEV-2: same enumeration order and stable devids while the device
      // set is unchanged.
      EXPECT_EQ(first.device[i].devid, second.device[i].devid)
          << "DEV-2: devid not stable across enumerations (index " << i << ")";
    }
  }

  ASSERT_EQ(cubeb_device_collection_destroy(ctx, &first), CUBEB_OK);
  ASSERT_EQ(cubeb_device_collection_destroy(ctx, &second), CUBEB_OK);
  cubeb_destroy(ctx);
}

static std::atomic<int> collection_changed_fires{0};

static void
counting_collection_changed_cb(cubeb * /*context*/, void * /*user_ptr*/)
{
  collection_changed_fires++;
}

// DEV-5, DEV-6: collection-changed registration bookkeeping and no
// spurious delivery on stream open/close.
TEST(cubeb, contract_collection_changed_registration)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  collection_changed_fires.store(0);

  int r = cubeb_register_device_collection_changed(
      ctx, CUBEB_DEVICE_TYPE_OUTPUT, counting_collection_changed_cb, nullptr);
  if (r == CUBEB_ERROR_NOT_SUPPORTED) {
    cubeb_destroy(ctx);
    GTEST_SKIP() << "device collection notifications not supported";
  }
  ASSERT_EQ(r, CUBEB_OK);

  if (known_deviation(ctx, "DEV-6")) {
    if (backend_is(ctx, "audiounit")) {
      // audiounit asserts (aborts in debug) on re-registration; do not
      // attempt it. pulse silently replaces the callback instead.
      CONTRACT_SHOULD(false, "DEV-6: re-registration asserts instead of "
                             "returning INVALID_PARAMETER");
    } else {
      CONTRACT_SHOULD(cubeb_register_device_collection_changed(
                          ctx, CUBEB_DEVICE_TYPE_OUTPUT,
                          counting_collection_changed_cb,
                          nullptr) == CUBEB_ERROR_INVALID_PARAMETER,
                      "DEV-6: re-registration without unregister must fail");
    }
  } else {
    EXPECT_EQ(cubeb_register_device_collection_changed(
                  ctx, CUBEB_DEVICE_TYPE_OUTPUT, counting_collection_changed_cb,
                  nullptr),
              CUBEB_ERROR_INVALID_PARAMETER)
        << "DEV-6: re-registration without unregister must fail";
  }

  // DEV-5: opening/closing a stream must not fire the callback.
  StreamHarness h;
  cubeb_stream * stm = nullptr;
  if (open_stream(ctx, &h, &stm, "contract collection changed") == CUBEB_OK) {
    ASSERT_EQ(cubeb_stream_start(stm), CUBEB_OK);
    EXPECT_TRUE(wait_for([&] { return h.data_cbs.load() >= 1; }));
    ASSERT_EQ(cubeb_stream_stop(stm), CUBEB_OK);
    cubeb_stream_destroy(stm);
    h.destroyed.store(true);
  }
  delay(500); // outlast notification debouncing (wasapi: 200 ms)
  EXPECT_EQ(collection_changed_fires.load(), 0)
      << "DEV-5: collection-changed fired on stream open/close";

  ASSERT_EQ(cubeb_register_device_collection_changed(
                ctx, CUBEB_DEVICE_TYPE_OUTPUT, nullptr, nullptr),
            CUBEB_OK)
      << "DEV-6: unregister";
  cubeb_destroy(ctx);
}

// STATE-4: stop cancels an in-progress drain; DRAINED must not be
// delivered after cubeb_stream_stop has returned.
TEST(cubeb, contract_stop_cancels_drain)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  StreamHarness h;
  h.short_return_at.store(0);
  cubeb_stream * stm = nullptr;
  ASSERT_EQ(open_stream(ctx, &h, &stm, "contract stop cancels drain"),
            CUBEB_OK);

  ASSERT_EQ(cubeb_stream_start(stm), CUBEB_OK);
  EXPECT_TRUE(wait_for([&] { return h.data_cbs.load() >= 1; }));
  ASSERT_EQ(cubeb_stream_stop(stm), CUBEB_OK);
  int drained_at_stop = h.drained.load();
  if (drained_at_stop > 0) {
    // DRAINED won the race and was delivered before stop returned; that
    // is legal and leaves nothing to check here.
    fprintf(stderr, "drain completed before stop returned; vacuous pass\n");
  }
  delay(500);
  CONTRACT_EXPECT(ctx, "STATE-4", h.drained.load() == drained_at_stop,
                  "STATE-4: DRAINED delivered after stop returned");
  cubeb_stream_destroy(stm);
  h.destroyed.store(true);
  CONTRACT_EXPECT(ctx, "STATE-4", h.drained.load() == drained_at_stop,
                  "STATE-4: DRAINED delivered during destroy");

  expect_no_violations(h);
  cubeb_destroy(ctx);
}

// DRAIN-4: restarting a drained stream is optional; whichever way a
// backend chooses, it must not crash or corrupt the stream. Records the
// observed behavior per backend.
TEST(cubeb, contract_restart_after_drain_probe)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  StreamHarness h;
  h.short_return_at.store(2);
  cubeb_stream * stm = nullptr;
  ASSERT_EQ(open_stream(ctx, &h, &stm, "contract restart probe"), CUBEB_OK);

  ASSERT_EQ(cubeb_stream_start(stm), CUBEB_OK);
  ASSERT_TRUE(wait_for([&] { return h.drained.load() >= 1; }, 15000));

  // Re-arm the harness so resumed callbacks are not counted as
  // contract violations.
  h.short_return_at.store(-1);
  h.terminal_return_seen.store(false);
  uint64_t cbs_at_drain = h.data_cbs.load();

  int r = cubeb_stream_start(stm);
  bool resumed = false;
  if (r == CUBEB_OK) {
    resumed = wait_for([&] { return h.data_cbs.load() > cbs_at_drain; }, 2000);
  }
  fprintf(stderr, "restart after drain on %s: start=%d callbacks_resumed=%d\n",
          cubeb_get_backend_id(ctx), r, resumed);

  ASSERT_EQ(cubeb_stream_stop(stm), CUBEB_OK);
  cubeb_stream_destroy(stm);
  h.destroyed.store(true);
  EXPECT_EQ(h.cb_after_destroy.load(), 0);
  EXPECT_EQ(h.nested_state_cb.load(), 0);
  cubeb_destroy(ctx);
}

// CTX-6: destroying a context with a collection-changed callback still
// registered must not abort. Disabled until the backend fixes land (the
// current Rust backends assert on drop and audiounit asserts on the
// listener bookkeeping); enable per backend as CTX-6 conformance lands.
TEST(cubeb, DISABLED_contract_destroy_with_collection_callback)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  int r = cubeb_register_device_collection_changed(
      ctx, CUBEB_DEVICE_TYPE_OUTPUT, counting_collection_changed_cb, nullptr);
  if (r == CUBEB_ERROR_NOT_SUPPORTED) {
    cubeb_destroy(ctx);
    GTEST_SKIP() << "device collection notifications not supported";
  }
  ASSERT_EQ(r, CUBEB_OK);
  cubeb_destroy(ctx);
}

// DATA-5: input-only short return stops capture and delivers DRAINED.
TEST(cubeb, contract_input_short_return)
{
  SKIP_IF_LEGACY_BACKEND();
  cubeb * ctx;
  ASSERT_EQ(common_init(&ctx, "test_contract"), CUBEB_OK);
  if (!can_run_audio_input_test(ctx)) {
    cubeb_destroy(ctx);
    GTEST_SKIP() << "no usable input device";
  }

  StreamHarness h;
  h.has_output = false;
  h.has_input = true;
  h.short_return_at.store(3);
  cubeb_stream * stm = nullptr;
  ASSERT_EQ(open_stream(ctx, &h, &stm, "contract input short return"),
            CUBEB_OK);

  ASSERT_EQ(cubeb_stream_start(stm), CUBEB_OK);
  bool got_drained = wait_for([&] { return h.drained.load() >= 1; });
  // pulse shuts the capture stream down without a state callback; wasapi
  // has no DRAINED delivery path for input-only streams (its two DRAINED
  // sites are the output-padding path and the shutdown path).
  CONTRACT_EXPECT(ctx, "DATA-5", got_drained,
                  "DATA-5: DRAINED not delivered after input-only short "
                  "return");

  ASSERT_EQ(cubeb_stream_stop(stm), CUBEB_OK);
  delay(200);
  cubeb_stream_destroy(stm);
  h.destroyed.store(true);

  expect_no_violations(h);
  cubeb_destroy(ctx);
}
