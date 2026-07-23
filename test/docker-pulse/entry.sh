#!/bin/sh
# Build cubeb with the pulse-rust backend and run test_contract against a
# PulseAudio daemon with a null sink (its monitor provides an input device).
set -ex

export XDG_RUNTIME_DIR=/tmp/xdg
mkdir -p "$XDG_RUNTIME_DIR"
git config --global --add safe.directory '*'

pulseaudio --daemonize=true --exit-idle-time=-1 --disallow-exit
pactl load-module module-null-sink sink_name=test_sink
pactl list short sinks
pactl list short sources

cmake -S /cubeb -B /cubeb/build-linux-pulse -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTS=ON -DBUILD_RUST_LIBS=ON
cmake --build /cubeb/build-linux-pulse --target test_contract -j"$(nproc)"

CUBEB_BACKEND=pulse-rust /cubeb/build-linux-pulse/test_contract
