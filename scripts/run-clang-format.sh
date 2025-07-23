#!/bin/bash

# Check if there are uncommitted changes (excluding rust backends)
if [ -n "$(git status --porcelain | egrep -v '(cubeb-coreaudio-rs|cubeb-pulse-rs)')" ]; then
    echo "Not running clang-format -- commit changes and try again"
    exit 0
fi

# Find and format all C/C++ files
find "$1/src" "$1/include" "$1/test" \
    -type f \( -name "*.cpp" -o -name "*.c" -o -name "*.h" \) \
    -not -path "*/subprojects/speex/*" \
    -not -path "*/src/cubeb-coreaudio-rs/*" \
    -not -path "*/src/cubeb-pulse-rs/*" \
    -print0 | xargs -0 "${2:-clang-format}" -Werror -i
