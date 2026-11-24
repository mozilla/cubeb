# cubeb threat model

cubeb provides audio playback and capture for Firefox and other applications.
In Firefox, web content can influence the PCM audio and stream configurations
that reach cubeb through the media stack. Audio servers and devices also supply
runtime data and notifications to the backends.

## Scope

- Review the C API, the mixer and resampler, and the platform backends. Pay
  particular attention to frame counts, sample formats, channel layouts, and
  buffer sizes as they pass between the application and backend.
- Include stream teardown, callbacks, and device changes where state is shared
  across threads. Assume callers honor the documented pointer, thread, and
  callback contracts.
- Assess findings in cubeb’s source. Firefox's media pipeline, operating system
  audio services, and external dependencies have their own review scope.

## Security impact

Prioritize memory safety, unintended access to audio data, and failures that a
remote origin can trigger through a supported consumer. Report the required
caller behavior, platform, backend, and practical impact.

## Testing

Run `.oss-scanner/test.sh` in the Linux image to test Rust PulseAudio without
audio hardware. The suite skips the stream device-change callback registration
test because Rust PulseAudio leaves that operation unimplemented.
