#!/bin/sh
# Builds the listfile module and its host test with g++ (Linux or MinGW) and runs every scenario.
#   ./run_listfile_tests.sh <listfile snippet.csv>        (exit code 0 = everything passed)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$HERE/../.." && pwd)
M=$R/Engine/Source/WXLModules/Listfile
SNIPPET=${1:?usage: run_listfile_tests.sh <listfile snippet.csv>}
OUT=${TMPDIR:-/tmp}/wxl_listfile_test
mkdir -p "$OUT"
g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -I "$R/Engine/Source/WXL/Sdk" -I "$M" "$M"/*.cpp -o "$OUT/libwxl_listfile.so"
g++ -std=c++17 -O2 -pthread -I "$R/Engine/Source/WXL/Sdk" "$HERE/listfile_host_test.cpp" -ldl -o "$OUT/listfile_host_test"
for scenario in refusals eager lazy formats noexists customname missingfile scale scalelazy; do
    echo "== $scenario"
    "$OUT/listfile_host_test" "$OUT/libwxl_listfile.so" "$SNIPPET" "$scenario"
done
echo "all scenarios passed"
