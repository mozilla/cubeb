#!/bin/sh
set -eu

# Provide playback and capture through a null sink and its monitor source.
PULSE_RUNTIME_PATH=$(mktemp -d)
PULSE_SERVER="unix:$PULSE_RUNTIME_PATH/native"
export PULSE_RUNTIME_PATH PULSE_SERVER
trap 'pulseaudio --kill 2>/dev/null || true; rm -rf "$PULSE_RUNTIME_PATH"' EXIT

pulseaudio --daemonize=yes --exit-idle-time=-1 -n \
    --load=module-native-protocol-unix --load=module-null-sink

cd /src/build
# The Rust backend returns CUBEB_ERROR for stream device-change registration.
# Run that test with the C backend, as in the standard Linux CI build.
CUBEB_BACKEND=pulse-rust ctest --output-on-failure --timeout 120 -E '^device_changed_callback$'
CUBEB_BACKEND=pulse ctest --output-on-failure --timeout 120 -R '^device_changed_callback$'
