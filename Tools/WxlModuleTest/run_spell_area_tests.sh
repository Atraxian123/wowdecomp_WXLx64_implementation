#!/bin/sh
# Builds and runs the spell area indicator's engine-free test from the repository root.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/spell_area_test"
mkdir -p "$OUT"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -Wall -I "$ROOT" \
    "$ROOT/Tools/WxlModuleTest/spell_area_test.cpp" "$ROOT/Engine/Source/WXL/SpellAreaShapes.cpp" -o "$OUT/spell_area_test"
"$OUT/spell_area_test"
