#!/bin/sh
# Builds the precast animation scaling decision (Engine/Source/WXL/CastAnim) with its host test using g++,
# with address and undefined-behaviour sanitizers, and runs it. No game files needed.
#   ./run_castanim_tests.sh        (exit code 0 = everything passed)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$HERE/../.." && pwd)
OUT=${TMPDIR:-/tmp}/wxl_castanim_test
mkdir -p "$OUT"
g++ -std=c++20 -O1 -g -w -fsanitize=address,undefined -pthread -I "$R" \
    "$HERE/castanim_test.cpp" "$R/Engine/Source/WXL/CastAnim.cpp" "$R/Engine/Source/WXL/Log.cpp" \
    -o "$OUT/castanim_test"
"$OUT/castanim_test"
