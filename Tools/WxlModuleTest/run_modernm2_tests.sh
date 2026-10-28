#!/bin/sh
# Builds the modern M2 reader (Engine/Source/Model2/ModernM2) with its host test using g++ (Linux or MinGW),
# with address and undefined-behaviour sanitizers, and runs it. Synthetic data only; no game files needed.
#   ./run_modernm2_tests.sh        (exit code 0 = everything passed)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$HERE/../.." && pwd)
M=$R/Engine/Source/Model2/ModernM2
OUT=${TMPDIR:-/tmp}/wxl_modernm2_test
mkdir -p "$OUT"
g++ -std=c++20 -O1 -g -w -fpermissive -DWOWDECOMP_ARCH_64 -fsanitize=address,undefined -pthread \
    -include "$HERE/stub_m2/shims.h" -I "$HERE/stub_m2" -I "$R" \
    "$HERE/modernm2_test.cpp" "$M/ModernM2Load.cpp" "$M/ModernM2Skin.cpp" "$R/Engine/Source/WXL/Services.cpp" \
    -o "$OUT/modernm2_test"
"$OUT/modernm2_test"
