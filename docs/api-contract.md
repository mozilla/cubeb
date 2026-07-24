# cubeb API contract specification

Status: DRAFT for review. Not yet normative.

This document specifies the behavioral contract of the public cubeb C API
(`include/cubeb/cubeb.h`). It is derived from four sources of truth:

1. The documented semantics in `cubeb.h`.
2. The validation and dispatch behavior of the common layer (`src/cubeb.c`).
3. How Gecko consumes the API and what it implicitly relies on
   (`dom/media/CubebUtils.cpp`, `AudioStream.cpp`, `GraphDriver.cpp`,
   `CubebInputStream.cpp`, `AudioInputSource.cpp`, `AudioSink.cpp`,
   `webrtc/CubebDeviceEnumerator.cpp`, and the `MockCubeb` reference model),
   including the audioipc2 client/server, which both implements and consumes
   the API and therefore encodes the contract from both sides.
4. The observed behavior of the tier-1 backends: wasapi, aaudio (C++),
   cubeb-coreaudio, cubeb-pulse (Rust).

Review decisions ratified 2026-07-23: the spec follows the
baseline-plus-tightening model below; the contract test suite is developed
upstream in mozilla/cubeb (`test/test_contract.cpp`) and vendored into Gecko;
the canonical home of this document is the cubeb repository, with the key
clauses distilled into `cubeb.h` comments once ratified; and stop-quiescence
(LIFE-6) is ratified as a MUST even though wasapi and aaudio do not yet
conform. The remaining section 14 questions were resolved 2026-07-24; section
14 is now a decision log.

Conformance keywords MUST / MUST NOT / SHOULD / MAY are used per RFC 2119.
The intent of this draft:

- **MUST** clauses are what a caller can rely on from every tier-1 backend
  today, or what a backend can rely on from every serious caller. Violations
  are bugs. The contract gtest suite (`test/test_contract.cpp`, Annex C)
  asserts these fatally.
- **SHOULD** clauses are agreed direction that at least one tier-1 backend
  does not yet satisfy. Known deviations are listed in Annex A. Tests for
  these run as non-fatal / known-fail until the backends are fixed.
- All originally-open decisions are now resolved; section 14 is the
  decision log.

Each clause has a stable ID (e.g. `LIFE-6`) so tests and documentation can
reference it.

---

## 1. Definitions

- **Context**: a `cubeb *` returned by `cubeb_init`. Owns backend-global
  state and device notification machinery.
- **Stream**: a `cubeb_stream *` returned by `cubeb_stream_init`.
- **Output-capable stream**: a stream initialized with non-NULL
  `output_stream_params` (output-only or duplex).
- **Input-only stream**: non-NULL `input_stream_params`, NULL
  `output_stream_params`.
- **Control plane**: every public function except the user callbacks.
- **Data plane**: the data callback and the backend threads that drive it.
- **Frame**: one sample per channel at the stream's configured rate
  (`cubeb_stream_params.rate`), not the device's native rate.
- **Callback quiescence**: no user callback is executing and none will ever
  be invoked again for that stream.

## 2. Threading model (THR)

- **THR-1** The caller MUST NOT call any cubeb function from within any
  cubeb callback (data, state, device-changed, or device-collection-changed).
  Rationale: deadlocks on the pulse mainloop and the coreaudio dispatch
  queue; audioipc asserts this on every entry point. The wasapi backend
  happens to tolerate re-entry from the collection-changed callback (it is
  delivered from a dedicated worker thread); callers MUST NOT rely on this.
- **THR-2** Control-plane functions MAY be called from any thread, and from
  different threads over the lifetime of the object; a backend MUST NOT
  require caller thread affinity. Exception: on Windows, every calling
  thread MUST have COM initialized in MTA mode (cubeb.h note; Gecko wraps
  all calls in `mscom::EnsureMTA`).
- **THR-3** The caller MUST serialize mutating control-plane calls per
  stream (Gecko funnels them through a single "CubebOperation" thread;
  audioipc serializes all of them on one RPC thread). Concurrent mutating
  calls on the same stream are not required to be supported.
- **THR-4** `cubeb_stream_get_position`, `cubeb_stream_get_latency`, and
  `cubeb_stream_get_input_latency` MUST be callable from any thread
  concurrently with a running data callback, and MUST NOT block until the
  data callback completes. Rationale: Gecko queries the position for A/V
  sync from arbitrary threads while the callback runs; on macOS it does so
  without holding its own stream monitor.
- **THR-5** Data and state callbacks are invoked on backend-owned threads.
  The identity of the data-callback thread MAY change during the stream's
  lifetime (typically across a device switch or reinit). Gecko explicitly
  handles thread-id changes mid-stream.
- **THR-6** A state callback MUST NOT be invoked while the same stream's
  data callback is executing (no nesting). Gecko asserts this
  (`GraphDriver.cpp` `MOZ_ASSERT(!InIteration())`).
- **THR-7** Data callbacks of one stream MUST be serialized: at most one
  in flight at a time (audioipc assumes a single outstanding data callback
  per stream).

## 3. Context (CTX)

- **CTX-1** `cubeb_init` MAY fail (e.g. no audio hardware) with
  `CUBEB_ERROR`; on failure `*context` is not a valid context. Callers
  MUST handle a null/failed context (Gecko degrades gracefully).
- **CTX-2** Multiple live contexts MUST be supported simultaneously, with
  independent lifetimes and out-of-order destruction (asserted by
  `test_sanity`).
- **CTX-3** `cubeb_get_backend_id` MUST return a non-NULL pointer to a
  string that is valid and constant for the context's lifetime (audioipc
  caches it once at context creation).
- **CTX-4** `cubeb_get_max_channel_count`, `cubeb_get_min_latency`,
  `cubeb_get_preferred_sample_rate` MUST return `CUBEB_OK` with a value
  greater than 0, or `CUBEB_ERROR_NOT_SUPPORTED`, or `CUBEB_ERROR` when
  the underlying device query fails. These are snapshots: values MAY change
  when the default device changes. These calls MAY block (device I/O, IPC
  round-trip) and MUST NOT be called from real-time threads.
- **CTX-5** A latency of at least the value returned by
  `cubeb_get_min_latency` for given params MUST be accepted by
  `cubeb_stream_init` with those params ("guaranteed to work").
- **CTX-6** `cubeb_destroy` MUST only be called after every stream created
  from the context has been destroyed. Callers SHOULD unregister any
  device-collection-changed callback first; if one is still registered,
  `cubeb_destroy` MUST unregister it itself (a logged warning is
  appropriate) and MUST NOT abort (resolved 2026-07-24). Deviations:
  cubeb-pulse and cubeb-coreaudio currently abort via an assert on
  context drop; the upstream death test in `test_duplex` encoding the old
  crash behavior is to be retired once they are fixed.
- **CTX-7** After `cubeb_destroy` returns, no callback registered through
  that context (collection-changed) may fire.

## 4. Stream initialization (INIT)

- **INIT-1** Parameter validation is split between the common layer and the
  backend. The common layer (`cubeb.c`) rejects with
  `CUBEB_ERROR_INVALID_PARAMETER`: NULL context/stream/data
  callback/state callback, latency outside [1, 96000]; and with
  `CUBEB_ERROR_INVALID_FORMAT`: missing both param structs, rate outside
  [1000, 768000], channels outside [1, 255], or duplex rate/format
  mismatch. NOTE: `cubeb.h` documents rate range [1000, 384000] and
  channels [1, 8]; the code enforces wider bounds. (Resolved 2026-07-24:
  the header documentation is corrected to the enforced bounds.)
  Channel-count/layout consistency is NOT validated centrally.
- **INIT-2** Backends MUST support `CUBEB_SAMPLE_FLOAT32NE` and SHOULD
  support `CUBEB_SAMPLE_S16NE`. Non-native-endian formats MAY be rejected
  with `CUBEB_ERROR_INVALID_FORMAT` (all tier-1 backends reject them).
- **INIT-3** Backends MUST accept any rate within the valid range
  regardless of the device's native rate, resampling internally (all tier-1
  backends use the shared resampler or delegate to the server, as pulse
  does). Duplex streams MUST use the same rate on both sides (enforced
  centrally).
- **INIT-4** Device selection: a NULL `cubeb_devid` means "the OS default
  device for that direction, following default-device changes" (see DEV-8).
  A non-NULL devid pins the stream to that device forever; the stream MUST
  NOT be rerouted (see DEV-9). A devid that does not identify a currently
  available device MUST fail with `CUBEB_ERROR_DEVICE_UNAVAILABLE`
  (resolved 2026-07-24). Current deviations: cubeb-coreaudio panics across FFI on a stale devid;
  aaudio ignores explicit devids entirely (asserts in debug, silently uses
  the default in release); wasapi returns generic `CUBEB_ERROR`.
- **INIT-5** `latency_frames` is a hint. The backend MAY clamp it to a
  supported range (cubeb-coreaudio clamps to [128, 512] and forces all
  concurrent streams to share one latency; audioipc clamps to roughly
  [5 ms rounded to a power of two, 1 s]). The effective value is observable
  via `cubeb_stream_get_latency`.
- **INIT-6** Callers SHOULD pass a non-NULL `stream_name`; backends MUST
  NOT crash on a NULL name (deviation: cubeb-pulse panics).
- **INIT-7** `user_ptr` is passed verbatim to every callback and returned
  exactly by `cubeb_stream_user_ptr`. It MUST remain valid until
  `cubeb_stream_destroy` returns (caller obligation).
- **INIT-8** On failure, `cubeb_stream_init` MUST NOT leave a stream behind:
  no callback may ever fire and no device resources may remain claimed.
- **INIT-9** IPC glue and other wrappers MUST tolerate callbacks arriving
  as soon as `cubeb_stream_init` has been entered (audioipc drops
  early callbacks before its wiring completes). Backends SHOULD NOT
  invoke the data callback before the first `cubeb_stream_start`.
  NOTE: `cubeb.h` claimed the data callback "will be called to preroll
  data before playback is started by cubeb_stream_start"; no tier-1
  backend prerolls before start is *called*. (Resolved 2026-07-24: the
  sentence is deleted from the header.)

## 5. Stream lifecycle (LIFE)

Conceptual caller-visible state machine:

```
            init
             |
             v            start
        INITIALIZED ---------------> STARTED <---+
                                      |  ^       | start
                                 stop |  | start |
                                      v  |       |
                                     STOPPED     |
                                      |          |
             short data-cb return ----+--> DRAINING --> DRAINED
             CUBEB_ERROR return /                        (restart optional)
             fatal device loss  -----------------------> ERRORED (terminal)

   destroy: legal from any state, but stop MUST be called first (LIFE-8).
```

- **LIFE-1** `cubeb_stream_start` on success begins audio I/O and MUST
  eventually deliver `CUBEB_STATE_STARTED`. Delivery MAY be synchronous on
  the caller thread before start returns (wasapi, pulse, coreaudio) or
  asynchronous from a backend thread (aaudio, audioipc).
- **LIFE-2** There is NO ordering guarantee between the STARTED state
  callback and the first data callback; data callbacks MAY begin before
  `cubeb_stream_start` returns. Gecko sets its own state to "Starting"
  before calling start for exactly this reason, and creates its
  drain-promise before start because DRAINED can arrive near-instantly.
  (Resolved 2026-07-24: no ordering guarantee; callers MUST NOT rely on
  STARTED preceding the first data callback.)
- **LIFE-3** Calling `cubeb_stream_start` on an already-started stream is
  unspecified: it MUST NOT crash or corrupt the stream, but MAY return
  `CUBEB_ERROR` (wasapi) or `CUBEB_OK` (aaudio). Callers MUST NOT
  double-start. SHOULD (future): idempotent success.
- **LIFE-4** `cubeb_stream_stop` MUST be idempotent: calling it on a
  stopped (or drained, or errored) stream MUST succeed without side
  effects beyond possibly delivering an extra STOPPED callback. Gecko
  double-stops routinely (pause then shutdown), and stops errored
  streams before destroying them. Deviation: aaudio returns
  `CUBEB_ERROR` from stop on an errored stream (confirmed on the API 34
  emulator).
- **LIFE-5** `cubeb_stream_stop` MUST eventually deliver
  `CUBEB_STATE_STOPPED` and cause data callbacks to cease promptly.
- **LIFE-6** When `cubeb_stream_stop` returns successfully, no data
  callback may be executing and none may be invoked again until the next
  `cubeb_stream_start`. (Ratified 2026-07-23, resolving OPEN-2 in favor of
  strong stop.) pulse-rs (cork + operation_wait) and coreaudio-rs
  (AudioOutputUnitStop waits for in-flight callbacks) conform today;
  **wasapi and aaudio do not** (wasapi invokes the data callback outside
  its reset lock and stop does not synchronize with the render thread —
  Gecko bug 996162; aaudio merely *requests* stop and returns) — tracked
  deviations requiring backend fixes. Until they are fixed, callers SHOULD
  keep tolerating a straggler callback shortly after stop returns, as
  Gecko does. The STOPPED state callback itself is exempt from this
  quiescence bound: it MAY be delivered synchronously before stop returns
  or asynchronously after (STATE-3). `cubeb_stream_destroy` additionally
  guarantees full, permanent quiescence (LIFE-9). Note this guarantee has
  two separately testable halves: no data callback may BEGIN after stop
  returns, and no data callback may still be EXECUTING when stop returns
  (in flight across the return); test_contract detects both, the latter
  via boundary-crossing checks at callback exit.
- **LIFE-7** Callers MUST tolerate duplicate `CUBEB_STATE_STOPPED`
  deliveries for one logical stop (audioipc layering produces an extra
  STOPPED; Gecko bug 1801190 encodes this in MockCubeb). Backends SHOULD
  deliver exactly one per transition.
- **LIFE-8** The caller MUST call `cubeb_stream_stop` before
  `cubeb_stream_destroy` (documented precondition in `cubeb.h`), including
  after DRAINED or ERROR.
- **LIFE-9** `cubeb_stream_destroy` MUST NOT return until callback
  quiescence is reached: no user callback is executing and none will ever
  fire again, and `user_ptr` will never again be dereferenced. It MAY
  block (thread join, dispatch drain). This is the single strongest
  guarantee in the API: the audioipc server destroys streams from its RPC
  thread while a data callback may be in flight on the backend's audio
  thread and relies on destroy to synchronize; all four tier-1 backends
  implement it (render-thread join / two-phase teardown / cork-and-wait /
  dispatch-final).
- **LIFE-10** `cubeb_stream_destroy` MUST be callable from a thread other
  than the one that created or started the stream, while a data callback
  is concurrently executing, without deadlock (provided the caller is not
  inside a callback, THR-1).
- **LIFE-11** After `CUBEB_STATE_ERROR` the stream is dead: the backend
  MUST NOT deliver further data callbacks (STATE-5); the only meaningful
  caller actions are stop and destroy. Restarting an errored stream is
  unspecified; Gecko always creates a new stream.

## 6. Data callback (DATA)

- **DATA-1** `nframes` MUST be >= 1. `input_buffer` MUST be non-NULL iff
  the stream has an input side; `output_buffer` MUST be non-NULL iff the
  stream has an output side. Buffers are valid only for the duration of
  the call and do not alias.
- **DATA-2** For duplex streams the input and output sides of one callback
  refer to the same `nframes` frame count (the backend/resampler matches
  them). When the input side underruns (device starting, switching, or
  reinitializing), the backend MUST substitute silence rather than stall
  the output or shrink `nframes` to 0.
- **DATA-3** The return value MUST be in [0, nframes] or `CUBEB_ERROR`.
  Backends MAY treat any other value as `CUBEB_ERROR` (audioipc clamps and
  validates; aaudio and coreaudio treat out-of-range as error).
- **DATA-4** Output-capable stream, return `r < nframes`: the stream enters
  drain mode (section 8). The `r` frames returned are final audio. The
  data callback MUST NOT be invoked again (asserted by `test_sanity`).
- **DATA-5** Input-only stream, return `r < nframes`: the stream stops
  capturing, the data callback MUST NOT be invoked again, and
  `CUBEB_STATE_DRAINED` MUST be delivered exactly once (resolved
  2026-07-24: DRAINED, symmetric with output drain). aaudio and the C++
  audiounit backend already conform (the latter confirmed by
  test_contract). Deviations: cubeb-pulse shuts down without delivering a
  state callback (confirmed empirically); wasapi has no DRAINED delivery
  path for input-only streams at all — its only DRAINED sites are the
  output-padding path and the shutdown path, so DRAINED arrives at
  destroy or never (confirmed under Wine). The `cubeb.h` "stream being
  stopped" wording is rewritten accordingly (Annex B).
- **DATA-6** Return `CUBEB_ERROR`: the data callback MUST NOT be invoked
  again and `CUBEB_STATE_ERROR` MUST be delivered (all tier-1 backends and
  MockCubeb do).
- **DATA-7** Caller obligations: the callback MUST be non-blocking,
  bounded-time, and real-time safe, and MUST NOT call cubeb functions
  (THR-1). Under audioipc the platform audio thread is blocked for the
  full IPC round-trip of each callback, making this a hard real-time
  requirement.
- **DATA-8** Backend obligations: a slow (overloaded) data callback MUST
  cause only glitches — it MUST NOT corrupt state, deadlock, or spuriously
  drive the stream to DRAINED/ERROR (`test_overload_callback` encodes this
  for wasapi). Backends SHOULD NOT allocate, take blocking locks, or
  perform unbounded work on the data-callback thread (deviations: aaudio
  reallocates its duplex buffer and spawns a reinit thread from the RT
  callback; pulse may reallocate its linearization buffer).
- **DATA-9** Samples written by the callback are full-scale; the backend
  applies the `cubeb_stream_set_volume` gain (or hardware volume)
  afterwards.

## 7. State callback (STATE)

- **STATE-1** Only the four states STARTED, STOPPED, DRAINED, ERROR exist.
  The delivery thread is unspecified: it MAY be the thread inside
  `cubeb_stream_start`/`stop` (synchronous delivery) or a backend thread.
  Callers MUST NOT assume a stable state-callback thread.
- **STATE-2** STARTED: see LIFE-1/LIFE-2.
- **STATE-3** STOPPED: see LIFE-5/LIFE-6/LIFE-7. STOPPED *delivery* carries
  no quiescence guarantee of its own (wasapi dispatches STOPPED before
  callbacks have finished); quiescence is tied to `cubeb_stream_stop`
  returning (LIFE-6) and to destroy (LIFE-9).
- **STATE-4** DRAINED MUST be delivered exactly once per drain, strictly
  after the final data callback, and MUST NOT be followed by any data
  callback. DRAINED MAY be delivered very shortly after start (even
  before `cubeb_stream_start` returns) when the callback drains
  immediately. If the stream is stopped or destroyed while draining,
  play-out MAY be truncated; DRAINED MAY have been delivered before
  `cubeb_stream_stop` returned, but MUST NOT be delivered after it
  returns (resolved 2026-07-24: stop cancels drain). This makes the
  long-disabled upstream `stream_destroy_pending_drain` stub
  implementable. Deviations: wasapi delivers DRAINED from its shutdown
  path when destroy interrupts a drain (confirmed under Wine);
  cubeb-pulse's drain timer delivers DRAINED after stop has returned
  (confirmed empirically).
- **STATE-5** ERROR is terminal (LIFE-11) and MUST NOT be followed by any
  data callback. It MUST be delivered on: data-callback CUBEB_ERROR return
  (DATA-6), unrecoverable loss of a pinned device (DEV-9), failed
  transparent reroute (DEV-8), or unrecoverable backend failure.
- **STATE-6** Backends MUST NOT deliver DRAINED or ERROR spuriously — in
  particular not merely because a data callback ran long (DATA-8), and not
  during a successful transparent device switch (DEV-8).

## 8. Drain (DRAIN)

- **DRAIN-1** Drain is triggered only by a data-callback short return on an
  output-capable stream (DATA-4). There is no other drain API.
- **DRAIN-2** All frames delivered by the callback (including the final
  partial buffer) MUST be played out to the device before DRAINED is
  delivered. Implementations: wasapi waits until device padding reaches 0;
  aaudio waits until `framesRead >= drain_target` (explicitly to avoid
  AAudio truncating the tail); coreaudio plays one extra silent buffer.
  Deviation: pulse approximates with a timer at twice the stream latency,
  which is not exact play-out.
- **DRAIN-3** After DRAINED, `cubeb_stream_get_position` SHOULD equal the
  total number of frames written by the data callbacks, and MUST NOT
  exceed it, and MUST be frozen (POS-5). (Upstream `test_sanity` contains
  a commented-out assertion wishing to promote the "equal" half to MUST.)
- **DRAIN-4** A drained stream MUST still accept `cubeb_stream_stop`
  (LIFE-4) and `cubeb_stream_destroy` (after stop). Restart is NOT
  required (resolved 2026-07-24): `cubeb_stream_start` on a drained
  stream MAY fail, and callers MUST NOT rely on it — create a new stream
  instead. Backends MAY support restart as an extension; it MUST NOT crash either
  way. Observed: audiounit, coreaudio-rs, and cubeb-pulse resume
  callbacks after restart; wasapi returns CUBEB_OK from start but never
  resumes callbacks — worse than failing, and worth fixing to either
  error or resume.

## 9. Position and latency (POS / LAT)

- **POS-1** Position is a play cursor in frames at the stream's configured
  sample rate, zero-based from stream creation, representing frames
  presented to the listener. It lags the write cursor (sum of data
  callback return values) by approximately the output latency.
- **POS-2** Before the first start, position MUST be 0, and MUST remain 0
  until playback actually advances.
- **POS-3** Position MUST be monotonically non-decreasing for the life of
  the stream — across stop/start cycles, transparent device switches, and
  reroutes. wasapi, aaudio, and coreaudio all enforce this with an internal
  clamp (evidence the raw platform clocks are not monotonic); pulse does
  not (deviation). Empirically confirmed on cubeb-pulse: position is
  nonzero before start (POS-2), exceeds frames written (POS-4), and keeps
  advancing while stopped (POS-5) — the PA interpolated clock is used
  unfiltered. Gecko additionally clamps defensively (`AudioSink`) but
  asserts backends should not go backwards.
- **POS-4** Position MUST NOT exceed the total frames written by data
  callbacks at any time. Deviations: cubeb-pulse (unfiltered PA clock,
  see POS-3); aaudio exceeds frames written after a drain because the
  silence tail it plays out is counted by `getFramesRead` (confirmed on
  the API 34 emulator).
- **POS-5** While the stream is stopped, and after DRAINED, successive
  position reads MUST return the same value (frozen clock). Position MUST
  advance while started and playing. Deviations: cubeb-pulse (clock runs
  while corked, see POS-3); aaudio's interpolating position estimate can
  keep advancing briefly after stop, even once STOPPED has been
  delivered (confirmed on the API 34 emulator; related to its
  asynchronous stop, LIFE-6).
- **POS-6** Position MUST keep counting up across a long-running stream
  that is logically reused (Gecko seeks by rebasing its own clock without
  stopping the cubeb stream; the cubeb frame counter must keep climbing).
- **POS-7** Position is defined only for output-capable streams (resolved
  2026-07-24). For input-only streams `cubeb_stream_get_position` MAY
  return `CUBEB_ERROR` (wasapi and pulse do); backends MAY implement a
  capture position as an extension (aaudio does). Callers MUST NOT rely
  on input-only position.
- **LAT-1** `cubeb_stream_get_latency` reports output-path latency in
  frames. It MAY return `CUBEB_ERROR` before the stream has started
  playing. It MUST NOT crash on unusual device-reported latencies
  (deviation: pulse panics on negative PA latency).
- **LAT-2** `cubeb_stream_get_input_latency` reports input-path latency; it
  MUST return `CUBEB_ERROR` for output-only streams, and MAY return an
  error (wasapi) or 0 (aaudio) before the first input callback.

## 10. Stream properties (PROP)

- **PROP-1** `cubeb_stream_set_volume`: volume outside [0.0, 1.0] is
  rejected centrally with `CUBEB_ERROR_INVALID_PARAMETER`. The gain
  applies to the output within a bounded number of callbacks, MUST NOT
  affect position or drain behavior, and MUST survive transparent
  reroutes. Input-only streams: `CUBEB_ERROR` or NOT_SUPPORTED.
- **PROP-2** `cubeb_stream_set_name` is optional
  (`CUBEB_ERROR_NOT_SUPPORTED` allowed; only pulse implements it). Gecko
  tolerates NOT_SUPPORTED explicitly.
- **PROP-3** `cubeb_stream_get_current_device` /
  `cubeb_stream_device_destroy` are an optional pair; returned strings are
  owned by the library until `device_destroy`.
- **PROP-4** `cubeb_stream_set_input_mute` and
  `cubeb_stream_set_input_processing_params` are optional. A backend MUST
  accept at runtime any processing-param subset it advertises via
  `cubeb_get_supported_input_processing_params` (deviation: wasapi
  advertises all four but only applies params at init time and returns
  NOT_SUPPORTED from the setter).
- **PROP-5** `cubeb_stream_register_device_changed_callback` is optional.
  NULL unregisters. Registering while already registered MUST fail with
  `CUBEB_ERROR_INVALID_PARAMETER` (coreaudio and MockCubeb encode this).
  The callback MAY fire on any thread, MUST NOT fire after destroy
  returns, MUST NOT fire absent a real device change, and cubeb calls from
  within it are forbidden (THR-1).

## 11. Device enumeration and notifications (DEV)

- **DEV-1** `cubeb_enumerate_devices` returns `CUBEB_OK` or
  `CUBEB_ERROR_NOT_SUPPORTED` (aaudio). The collection is a snapshot,
  valid until `cubeb_device_collection_destroy`. Destroying an empty or
  zeroed collection MUST succeed on backends that support enumeration;
  on backends without it the common layer returns NOT_SUPPORTED before
  the empty-collection short-circuit.
- **DEV-2** `cubeb_devid` values MUST be stable: enumerating twice while
  the device set is unchanged yields equal devids for the same physical
  device (asserted by `test_sanity`), and a devid remains usable for
  `cubeb_stream_init` while the device is present and the context alive.
- **DEV-3** For each usable device the backend MUST populate: `devid`,
  `device_id`, `friendly_name`, `group_id`, `type`, `state`, `preferred`,
  `format`, `default_format`, `max_channels` (> 0), `default_rate`,
  `min_rate`, `max_rate`. `latency_lo`/`latency_hi` SHOULD be populated
  (deviation: pulse reports 0/0 and min_rate=1/max_rate=PA_RATE_MAX).
  Gecko consumes every one of these fields and skips devices with
  `max_channels == 0`.
- **DEV-4** Default devices are indicated by `preferred != NONE`; multiple
  devices MAY carry preference flags (Windows role-based defaults), ranked
  multimedia > voice > notification by Gecko.
- **DEV-5** Passive operations MUST NOT perturb device state: enumerating,
  and opening or closing streams, MUST NOT change the enumerated device
  set nor fire the collection-changed callback (asserted by `test_devices`
  and `test_duplex`).
- **DEV-6** `cubeb_register_device_collection_changed`: registration is
  per device type (INPUT, OUTPUT, or both). Re-registering without first
  unregistering MUST fail with `CUBEB_ERROR_INVALID_PARAMETER`. Passing a
  NULL callback unregisters. Callers SHOULD unregister before
  `cubeb_destroy`; a still-registered callback MUST be tolerated per
  CTX-6. Returns `CUBEB_ERROR_NOT_SUPPORTED` when
  unimplemented (aaudio) — Gecko then falls back to polling; pulse
  returning generic `CUBEB_ERROR` for the unimplemented per-stream
  device-changed callback is a deviation from this convention.
- **DEV-7** The collection-changed callback MUST fire on device addition,
  removal, and default-device change for the registered type; MUST fire
  from a non-real-time backend-owned thread; MAY be coalesced/debounced
  (wasapi debounces 200 ms). Cubeb calls from within it are forbidden
  (THR-1).
- **DEV-8** Default-follow reroute: a stream initialized with NULL devid
  (without `CUBEB_STREAM_PREF_DISABLE_DEVICE_SWITCHING`) MUST follow the
  OS default device for its direction. A successful reroute MUST be
  transparent: the stream keeps running, NO state callback fires, position
  continuity per POS-3, though the callback thread MAY change (THR-5) and
  the callback cadence MAY hiccup (Gecko: "no guarantee on audio callbacks
  coming in after a device change event"). A failed reroute MUST deliver
  ERROR. If a per-stream device-changed callback is registered it SHOULD
  fire. Known asymmetry: wasapi reroutes on *render* default changes and
  on device invalidation, but does not reroute a duplex stream's input
  when only the default *capture* device changes; coreaudio re-resolves
  both sides.
- **DEV-9** A stream pinned to an explicit devid MUST NOT be rerouted; if
  the device becomes unavailable the backend MUST deliver ERROR (STATE-5).
- **DEV-10** With `CUBEB_STREAM_PREF_DISABLE_DEVICE_SWITCHING`, a NULL
  devid stream behaves as if pinned to the default device resolved at init.

## 12. Error codes (ERR)

- **ERR-1** Every fallible function MUST return `CUBEB_OK` or one of the
  negative `CUBEB_ERROR*` codes from `cubeb.h` — never a raw platform code
  (deviation: wasapi `stream_init` returns a raw HRESULT on
  `CoCreateInstance` failure).
- **ERR-2** `CUBEB_ERROR_NOT_SUPPORTED` is reserved for "this backend does
  not implement this optional feature" — the common layer synthesizes it
  for NULL ops-table entries, and backends returning it directly MUST mean
  the same thing. It MUST NOT be used for transient failures. Conversely,
  unimplemented features SHOULD NOT return generic `CUBEB_ERROR`
  (deviation: pulse in several spots).
- **ERR-3** `CUBEB_ERROR_DEVICE_UNAVAILABLE` MUST be returned by
  `cubeb_stream_init` when the requested device does not exist or cannot
  be opened (resolved 2026-07-24; see INIT-4 for the per-backend
  deviations). It is currently returned by no tier-1 backend (dead code
  in practice; MockCubeb uses it, so Gecko handles it).
- **ERR-4** `CUBEB_ERROR_INVALID_PARAMETER` / `CUBEB_ERROR_INVALID_FORMAT`
  follow the split in INIT-1.
- **ERR-5** A backend MUST NOT abort the process or unwind across the C
  FFI boundary for any documented input, including caller contract
  violations it can detect. Rust backends: a `panic!`/`unwrap`/`assert!`
  reachable from any op is a contract violation — the `cubeb-backend` capi
  glue has no `catch_unwind`. Known reachable panics: coreaudio
  stream_init on stale/non-UTF-8 devid; pulse on NULL stream name,
  negative latency, `begin_write` failure, and context drop with a
  registered collection callback (also coreaudio for the latter).

## 13. Notes for IPC and wrapper layers

The audioipc2 layer (Gecko's sandboxed default) adds these observable
properties on top of any backend; new backends must be correct under them:

- All control-plane calls for all streams and all client processes are
  serialized on one server thread with one shared context (THR-2/THR-3 in
  the extreme).
- Data callbacks are delivered on a dedicated real-time client thread; the
  platform audio thread blocks for the full round-trip of each callback
  (DATA-7 is hard-real-time under IPC).
- `cubeb_stream_destroy` is issued from the server RPC thread while a data
  callback may be mid-flight (LIFE-9/LIFE-10 are load-bearing).
- The callback's `cubeb_stream *` argument is NULL under IPC (upstream
  issue #518); callers MUST NOT rely on it. Same for the `cubeb *`
  argument of the collection-changed callback.
- Latency hints are clamped to roughly [5 ms, 1 s] (INIT-5).
- An extra STOPPED state callback is produced by the layering (LIFE-7).
- Server death surfaces as `CUBEB_ERROR` from every subsequent call on the
  context and its streams; there is no reconnect.

## 14. Decision log (all resolved)

1. **STARTED ordering** (LIFE-2) — resolved 2026-07-24: no ordering
   guarantee between STARTED and the first data callback; callers MUST
   NOT rely on one.
2. **stop() quiescence** (LIFE-6) — resolved 2026-07-23: strong stop.
   `cubeb_stream_stop` must quiesce data callbacks before returning;
   wasapi and aaudio are tracked deviations to be fixed.
3. **Input-only short-return terminal state** (DATA-5) — resolved
   2026-07-24: DRAINED, exactly once, symmetric with output drain.
   cubeb-pulse (no state callback today) is a tracked deviation.
4. **Interrupted drain** (STATE-4) — resolved 2026-07-24: stop cancels
   drain; DRAINED must not be delivered after stop returns. wasapi's
   DRAINED-from-destroy path is a tracked deviation.
5. **Restart after DRAINED** (DRAIN-4) — resolved 2026-07-24: not
   required; start on a drained stream may fail and callers must create
   a new stream. Backends may support it as an extension.
6. **Param bounds** (INIT-1) — resolved 2026-07-24: correct `cubeb.h`
   docs to the enforced [1000, 768000] / [1, 255]; `cubeb.c` unchanged.
7. **Preroll sentence** (INIT-9) — resolved 2026-07-24: delete the false
   claim from `cubeb.h`.
8. **DEVICE_UNAVAILABLE adoption** (INIT-4/ERR-3) — resolved 2026-07-24:
   required for a devid that does not identify an available device at
   `cubeb_stream_init`; coreaudio-rs (panic), aaudio (ignores devid), and
   wasapi (generic ERROR) are tracked deviations.
9. **Destroy with registered collection callback** (CTX-6) — resolved
   2026-07-24: `cubeb_destroy` auto-unregisters and must not abort;
   unregistering first remains the documented caller obligation. The
   rust backends' drop asserts are tracked deviations; the `test_duplex`
   death test is to be retired.
10. **Input-only position** (POS-7) — resolved 2026-07-24: position is
    defined only for output-capable streams; input-only MAY return
    `CUBEB_ERROR`.

---

## Annex A. Tier-1 backend conformance snapshot (2026-07, vendored trees)

Verdicts: ok / dev (deviation) / broken / n-i (not implemented, allowed).

| Clause | wasapi | aaudio | coreaudio-rs | pulse-rs |
|---|---|---|---|---|
| LIFE-2 STARTED before 1st data cb | yes (sync) | no | no | yes |
| LIFE-3 double-start harmless | dev (errors) | ok (idempotent) | ok | ok |
| LIFE-6 stop quiesces callbacks | no | no | yes | yes |
| LIFE-9 destroy quiesces | ok (thread join) | ok (two-phase) | ok (run_final) | ok (cork+wait) |
| DATA-8 RT-safe callback path | ok | dev (alloc + thread spawn in RT cb) | dev (asserts on RT thread) | dev (panic paths, realloc) |
| DRAIN-2 full play-out | ok (padding==0) | ok (frames-read target) | ok | dev (2x-latency timer) |
| POS-3 monotonic position | ok (clamped) | ok (clamped) | ok (clamped) | dev (no clamp) |
| POS-7 input-only position | error | works | untested | error |
| INIT-4 honors explicit devid | ok | broken (ignored) | dev (panics on stale) | ok |
| ERR-1 no raw platform codes | broken (HRESULT leak) | ok | ok | ok |
| ERR-3 DEVICE_UNAVAILABLE used | no | no | no | no |
| ERR-5 no panic across FFI | n/a (C++) | n/a | broken | broken |
| PROP-2 set_name | n-i | n-i | n-i | ok |
| PROP-4 processing params coherent | dev (advertises, init-only) | n-i | ok (VPIO) | ok (none) |
| PROP-5 device_changed_callback | n-i | n-i | ok | dev (returns ERROR not NOT_SUPPORTED) |
| DEV-1..7 enumeration/collection | ok | n-i | ok | dev (latency/rate fields fake) |
| DEV-8 default-follow reroute | ok (render side only) | ok (reinit) | ok (both sides) | ok (PA auto-move) |

Empirical results from `test_contract` (2026-07-24):

- audiounit (macOS, C++): 23/23 pass; deviations logged: DRAIN-3
  (position does not converge after drain), PROP-5 and DEV-6
  (assert/abort on double registration).
- cubeb-coreaudio (macOS, `CUBEB_BACKEND=audiounit-rust`): 23/23 pass;
  only DRAIN-3 logged. Cleanest backend measured; supports
  restart-after-drain and input-only DRAINED.
- cubeb-pulse (Debian container via `test/docker-pulse/run.sh`,
  PulseAudio null sink): 23/23 with deviations logged: POS-2, POS-4,
  POS-5 (unfiltered PA clock), STATE-4 (DRAINED after stop), DATA-5 (no
  DRAINED on input-only short return), ERR-2, DEV-6 (silent
  re-registration), DRAIN-3.
- wasapi (mingw-w64 cross-build under Wine via
  `test/wine-wasapi/run.sh`; advisory only): 22/23 with deviations
  logged: STATE-4 (DRAINED during destroy), DATA-5 (no input-only
  DRAINED path); device-changed registration correctly NOT_SUPPORTED;
  restart-after-drain accepted but never resumes. LIFE-6 did not
  reproduce under Wine; needs real Windows.
- wasapi (real Windows, GitHub CI with VB-CABLE, Debug and Release
  configs): full suite green with the tracked deviations gated. The
  LIFE-6 deviation is empirically confirmed there in its in-flight form:
  no new data callback begins after stop returns, but a callback can
  still be executing when it returns (caught by the boundary-crossing
  check; Wine and the aaudio emulator never reproduced this). STATE-4
  (DRAINED during destroy) and DATA-5 also confirmed on real Windows.
- winmm (legacy, GitHub CI second pass): fatal assert in
  `cubeb_winmm.c:395` (`!InterlockedPopEntrySList(ctx->work)`) tearing
  down after a data-callback error return; test_contract now skips winmm
  entirely rather than tracking a backend that crashes.
- aaudio (API 34 arm64 emulator via the mozbuild AVD): 17 pass, 3
  correct skips (no enumeration); deviations logged: LIFE-4 (stop on an
  errored stream returns ERROR), POS-4 after drain (silence tail counted
  by getFramesRead), DRAIN-3, LAT-1 (zero reported latency). LIFE-6 did
  not reproduce on the emulator (stop quiesced in practice); the entry
  stays, based on the code reading (stop merely requests). Supports
  restart-after-drain.

Priority improvement list (from the full evaluations):

- wasapi: implement strong stop (LIFE-6) — synchronize stop with the
  render thread's callback; add a DRAINED delivery path for input-only
  streams (DATA-5); do not deliver DRAINED from the shutdown path
  (STATE-4); make start-after-drain either fail or resume (DRAIN-4); map
  the raw HRESULT at `cubeb_wasapi.cpp:2903` to `CUBEB_ERROR`; route
  input default-device changes; reconcile PROP-4; consider idempotent
  start.
- aaudio: implement strong stop (LIFE-6) — wait for AAudio's
  requestPause/requestStop to reach the stopped state before returning;
  make stop succeed on an errored stream (LIFE-4); clamp post-drain
  position to frames written (POS-4); freeze the position estimate once
  stop completes (POS-5); honor or reject explicit devids
  (INIT-4); pre-size the duplex buffer and move reinit thread-spawn off
  the RT callback (DATA-8); query real max channel count instead of
  hardcoding 2.
- coreaudio-rs: replace the stale-devid `unwrap()` in stream_init with
  `CUBEB_ERROR_DEVICE_UNAVAILABLE` (INIT-4/ERR-3); make render-thread
  asserts degrade gracefully (ERR-5); consider catch_unwind in capi glue.
- pulse-rs: fix the four reachable panics (ERR-5); fix the position
  domain — zero before start, clamped to frames written, frozen while
  stopped/drained (POS-2/POS-3/POS-4/POS-5); do not fire the drain timer
  after stop (STATE-4); deliver DRAINED on input-only short return
  (DATA-5); reject re-registration with INVALID_PARAMETER (DEV-6);
  exact-ish drain (DRAIN-2); NOT_SUPPORTED where accurate (ERR-2); fix
  LOOPBACK pref check (`==` vs `contains`).
- cubeb-backend glue: add `catch_unwind` in every capi shim (ERR-5).
- common layer: none outstanding (bounds resolved via header doc fix).
- audiounit (C++, not tier-1; found by test_contract): asserts (aborts in
  debug) on double registration of both the device-changed callback
  (PROP-5) and the collection-changed callback (DEV-6) instead of
  returning `CUBEB_ERROR_INVALID_PARAMETER`; position does not converge
  to frames written after drain (DRAIN-3).

## Annex B. cubeb.h documentation defects to fix

1. Rate/channel bounds contradict `cubeb.c` (INIT-1). Resolved: fix the
   docs to the enforced [1000, 768000] / [1, 255].
2. "data_callback will be called to preroll data before playback is
   started by cubeb_stream_start" — no backend does this before start is
   called. Resolved: delete the sentence.
3. `cubeb_stream_init` retvals omit `CUBEB_ERROR_INVALID_PARAMETER`,
   which the common layer returns for latency/pointer violations.
4. `cubeb_register_device_collection_changed` lists only
   `CUBEB_ERROR_NOT_SUPPORTED` as retval; `CUBEB_OK`,
   `CUBEB_ERROR_INVALID_PARAMETER` missing.
5. `cubeb_set_log_callback` doc mentions an "invalid context" but the
   function takes no context (it is global).
6. `cubeb_get_min_latency` takes a mutable `cubeb_stream_params *` for no
   reason (API wart; const-correctness).
7. Threading, state-callback ordering, position semantics, and destroy
   guarantees are entirely undocumented — sections 2, 5, 7, 9 of this
   document should be distilled into the header comments once ratified.
8. Input-only short-return wording "stream being stopped" is ambiguous
   . Resolved: rewrite to specify DRAINED (DATA-5).

## Annex C. Contract test suite plan (`test/test_contract.cpp`)

A new, self-contained gtest file, developed upstream in mozilla/cubeb as
`test/test_contract.cpp` and vendored into Gecko with the rest of the test
suite, structured so every test names the clause(s) it verifies. Design
rules:

- Black-box: only the public C API, so the same binary tests every
  backend, including audioipc client contexts and future Rust ports.
  Backend selectable via the existing `CUBEB_BACKEND` env var
  (`common.h::common_init`).
- Deterministic event log instead of sleeps: a per-stream lock-free
  recorder appends (event, thread-id, timestamp, payload) from the data
  and state callbacks; assertions inspect the log after quiescence.
  Condition-variable waits with generous timeouts replace fixed
  `delay()` calls wherever a state transition can be awaited.
- Hardware gating: input/duplex tests use `can_run_audio_input_test`;
  output tests require a default output device and skip (GTEST_SKIP)
  otherwise, so the suite is green on deviceless CI.
- MUST clauses assert fatally; SHOULD clauses use EXPECT with a clause-ID
  message and are grouped so known-failing backends can be tracked.

Planned tests (clause coverage):

| Test | Clauses |
|---|---|
| context_queries_contract | CTX-3, CTX-4, CTX-5 |
| init_validation_error_codes | INIT-1, ERR-1, ERR-4 (bad rate/channels/latency/format, NULL cbs) |
| init_bad_devid | INIT-4, ERR-3 (bogus devid: expect DEVICE_UNAVAILABLE, accept documented deviations) |
| init_failure_no_callbacks | INIT-8 |
| user_ptr_roundtrip | INIT-7 (cubeb_stream_user_ptr + callback user_ptr identity) |
| state_transition_order_basic | LIFE-1, LIFE-5, STATE-1..3 (start..stop; log shows STARTED before STOPPED, no ERROR/DRAINED) |
| stop_idempotent_and_dup_stopped | LIFE-4, LIFE-7 |
| double_start_no_crash | LIFE-3 |
| stop_then_callback_quiescence | LIFE-6 (no data cb begins after stop returns AND none in flight across the return; gated on wasapi/aaudio until fixed) |
| destroy_quiescence | LIFE-9, LIFE-10 (destroy from second thread during heavy callbacks; no callback begins after nor is in flight across destroy's return) |
| data_cb_buffer_contract | DATA-1, DATA-2 (nframes>=1, null-ness per direction, duplex silence fill) |
| drain_on_short_return | DATA-4, DRAIN-1..3, STATE-4 (exactly one DRAINED, after last data cb, position==written within tolerance, then frozen) |
| drain_immediate_after_start | STATE-4, DRAIN-5 (first callback returns 0) |
| error_return_terminates | DATA-6, STATE-5, LIFE-11 |
| input_only_short_return | DATA-5 (asserts DRAINED; pulse gated as tracked deviation) |
| stop_cancels_drain | STATE-4 (no DRAINED delivered after stop returns) |
| restart_after_drain_probe | DRAIN-4 (informational: restart is optional, must not crash) |
| destroy_with_collection_callback (DISABLED) | CTX-6 (no abort; enable per backend as fixes land) |
| position_before_start_zero | POS-2 |
| position_monotonic_bounded | POS-3, POS-4 (concurrent-thread hammering get_position during playback) |
| position_frozen_when_stopped | POS-5 (and across stop/start, POS-6) |
| latency_queries | LAT-1, LAT-2 (input latency errors on output-only) |
| volume_contract | PROP-1 (range validation central, silence check via loopback where available) |
| optional_props_error_codes | PROP-2..5, ERR-2 (NOT_SUPPORTED vs ERROR discipline) |
| device_changed_cb_registration | PROP-5 (double-register INVALID_PARAMETER, NULL unregister) |
| enumeration_snapshot_contract | DEV-1..5 (field population, devid stability, no perturbation from open/close) |
| collection_changed_registration | DEV-6 (per-type, re-register fails, unregister, no spurious fire) |
| state_cb_not_nested_in_data_cb | THR-6 (thread/nesting recorder) |
| callback_thread_may_change_doc | THR-5 (record thread ids; informational) |
| overload_callback_no_spurious_state | DATA-8, STATE-6 (generalized from wasapi-only test to all backends) |

Not testable without device manipulation (deferred; candidates for
virtual-device fixtures — pulse null-sink, virtual audio driver on
macOS/Windows): DEV-7 positive fire, DEV-8 reroute transparency, DEV-9
pinned-device loss, PROP-5 positive fire.
