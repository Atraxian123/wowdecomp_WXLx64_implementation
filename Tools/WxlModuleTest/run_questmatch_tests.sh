#!/bin/sh
# Builds the quest outline's matching rules (Engine/Source/WXLOutline/QuestMatch.h) with their host test using g++,
# with address and undefined-behaviour sanitizers, and runs it. No game files needed.
#   ./run_questmatch_tests.sh        (exit code 0 = everything passed)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$HERE/../.." && pwd)
OUT=${TMPDIR:-/tmp}/wxl_questmatch_test
mkdir -p "$OUT"
g++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I "$R" \
    "$HERE/questmatch_test.cpp" -o "$OUT/questmatch_test"
"$OUT/questmatch_test"
