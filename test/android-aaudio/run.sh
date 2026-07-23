#!/bin/sh
# Cross-compile cubeb for Android with the NDK and run test_contract
# against the aaudio backend on a booted emulator or device (adb).
# Defaults match a Firefox ~/.mozbuild setup; override via environment:
#   ANDROID_NDK   path to the NDK (with build/cmake/android.toolchain.cmake)
#   ADB           path to adb
# Boot an emulator first, e.g.:
#   ANDROID_AVD_HOME=~/.mozbuild/android-device/avd \
#     ~/.mozbuild/android-sdk-macosx/emulator/emulator \
#     -avd mozemulator-arm64 -no-window -no-snapshot
set -ex
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ANDROID_NDK="${ANDROID_NDK:-$HOME/.mozbuild/android-ndk-r29}"
ADB="${ADB:-$HOME/.mozbuild/android-sdk-macosx/platform-tools/adb}"

cmake -S "$DIR" -B "$DIR/build-android" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 \
    -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTS=ON
cmake --build "$DIR/build-android" --target test_contract -j8

"$ADB" wait-for-device
"$ADB" push "$DIR/build-android/test_contract" /data/local/tmp/test_contract
"$ADB" shell "chmod +x /data/local/tmp/test_contract && \
    CUBEB_BACKEND=aaudio /data/local/tmp/test_contract"
