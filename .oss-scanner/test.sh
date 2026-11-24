#!/bin/sh
set -eu

# Provide playback and capture through a null sink and its monitor source.
PULSE_RUNTIME_PATH=$(mktemp -d)
PULSE_SERVER="unix:$PULSE_RUNTIME_PATH/native"
export PULSE_RUNTIME_PATH PULSE_SERVER
trap 'pulseaudio --kill 2>/dev/null || true; rm -rf "$PULSE_RUNTIME_PATH"' EXIT

pulseaudio --daemonize=yes --exit-idle-time=-1 -n \
    --load=module-native-protocol-unix --load=module-null-sink

# Stream device-change callback registration is unimplemented in Rust PulseAudio.
CUBEB_BACKEND=pulse-rust ctest --test-dir /src/build --output-on-failure \
    --timeout 120 -E '^device_changed_callback$'
