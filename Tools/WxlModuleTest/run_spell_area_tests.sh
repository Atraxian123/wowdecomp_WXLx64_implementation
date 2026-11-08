#!/bin/sh
# Builds and runs the spell area indicator's engine-free test from the repository root.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/spell_area_test"
mkdir -p "$OUT"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -Wall -I "$ROOT" \
    "$ROOT/Tools/WxlModuleTest/spell_area_test.cpp" "$ROOT/Engine/Source/WXLModules/SpellArea/SpellAreaShapes.cpp" -o "$OUT/spell_area_test"
"$OUT/spell_area_test"

# the module itself, with a stubbed module API (the engine side is not needed)
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -Wall -I "$ROOT/Engine/Source/WXL/Sdk" -I "$ROOT/Engine/Source/WXLModules/SpellArea" \
    "$ROOT/Tools/WxlModuleTest/spell_area_module_test.cpp" "$ROOT/Engine/Source/WXLModules/SpellArea"/*.cpp -o "$OUT/spell_area_module_test"
"$OUT/spell_area_module_test"
