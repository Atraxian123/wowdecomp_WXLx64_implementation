#!/bin/sh
# Builds the modern M2 module (Engine/Source/WXLModules/ModernM2) as a shared library and the client's adapter
# (Engine/Source/Model2/ModernM2/ModernM2.cpp) with its host test, using g++ (Linux or MinGW), with address and
# undefined-behaviour sanitizers, and runs them: the test loads the module through the C ABI. Then it runs the
# layout test, which compares the module's own copy of the M2 record layout with the engine's M2Data.h.
# Synthetic data only; no game files needed.
#   ./run_modernm2_tests.sh        (exit code 0 = everything passed)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$HERE/../.." && pwd)
MOD=$R/Engine/Source/WXLModules/ModernM2
M=$R/Engine/Source/Model2/ModernM2
SDK=$R/Engine/Source/WXL/Sdk
OUT=${TMPDIR:-/tmp}/wxl_modernm2_test
mkdir -p "$OUT"

# the module: no engine header on its include path, only the SDK, its own folder and FlightAnim.h (constants)
g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-invalid-offsetof -fPIC -shared -fvisibility=hidden \
    -fsanitize=address,undefined -I "$SDK" -I "$MOD" -I "$M" "$MOD"/*.cpp -o "$OUT/libwxl_modernm2.so"

SHIM="-include $HERE/stub_m2/shims.h -I $HERE/stub_m2 -I $R"
g++ -std=c++20 -O1 -g -w -fpermissive -DWOWDECOMP_ARCH_64 -fsanitize=address,undefined -pthread $SHIM \
    "$HERE/modernm2_test.cpp" "$M/ModernM2.cpp" "$R/Engine/Source/WXL/Services.cpp" \
    -ldl -o "$OUT/modernm2_test"
"$OUT/modernm2_test" "$OUT/libwxl_modernm2.so"

g++ -std=c++20 -O1 -g -w -fpermissive -DWOWDECOMP_ARCH_64 $SHIM -I "$MOD" \
    "$HERE/modernm2_layout_test.cpp" -o "$OUT/modernm2_layout_test"
"$OUT/modernm2_layout_test"
