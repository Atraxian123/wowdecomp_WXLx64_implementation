#!/bin/sh
# Builds and runs the multi-textured ribbon setting test from the repository root.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/ribbon_blend_test"
mkdir -p "$OUT"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -Wall -I "$ROOT" \
    "$ROOT/Tools/WxlModuleTest/ribbon_blend_test.cpp" "$ROOT/Engine/Source/WXL/RibbonBlend.cpp" "$ROOT/Engine/Source/WXL/Log.cpp" -o "$OUT/ribbon_blend_test"
"$OUT/ribbon_blend_test"
