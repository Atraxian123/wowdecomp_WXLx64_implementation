#!/bin/sh
# Builds and runs the PNG -> BLP2 converter test from the repository root.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/png_to_blp_test"
mkdir -p "$OUT/zlib"
python3 "$ROOT/Tools/WxlModuleTest/make_png_cases.py" "$OUT/cases"
for f in adler32 crc32 inflate inftrees inffast uncompr zutil; do
    gcc -O1 -c -I "$ROOT/ThirdParty/zlib" "$ROOT/ThirdParty/zlib/$f.c" -o "$OUT/zlib/$f.o"
done
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -Wall -I "$ROOT" \
    "$ROOT/Tools/WxlModuleTest/png_to_blp_test.cpp" "$ROOT/Engine/Source/WXL/Blp/PngToBlp.cpp" "$OUT"/zlib/*.o -o "$OUT/png_to_blp_test"
"$OUT/png_to_blp_test" "$OUT"/cases/*.png
