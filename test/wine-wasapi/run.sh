#!/bin/sh
# Cross-compile cubeb for Windows with mingw-w64 and run test_contract
# against the wasapi backend under Wine. Wine's mmdevapi emulation is
# incomplete, so treat failures here as advisory unless reproduced on
# real Windows.
set -ex
DIR="$(cd "$(dirname "$0")/../.." && pwd)"

cmake -S "$DIR" -B "$DIR/build-mingw" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTS=ON \
    -DCMAKE_TOOLCHAIN_FILE="$DIR/test/wine-wasapi/mingw-toolchain.cmake"
cmake --build "$DIR/build-mingw" --target test_contract -j8

CUBEB_BACKEND=wasapi wine "$DIR/build-mingw/test_contract.exe"
