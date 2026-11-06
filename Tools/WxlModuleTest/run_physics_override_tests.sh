#!/bin/sh
# Builds the physics module as a shared library and the host test of PhysicsOverride.csv with g++ (Linux or
# MinGW), makes the synthetic test data, and runs it. No game files needed.
#   ./run_physics_override_tests.sh        (exit code 0 = everything passed)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$HERE/../.." && pwd)
M=$R/Engine/Source/WXLModules/Physics
SDK=$R/Engine/Source/WXL/Sdk
OUT=${TMPDIR:-/tmp}/wxl_physics_override_test
rm -rf "$OUT"; mkdir -p "$OUT"
cd "$OUT"

g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -fPIC -shared -fvisibility=hidden -fsanitize=address,undefined -I "$SDK" -I "$M" "$M"/*.cpp -o libwxl_physics.so
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -I "$SDK" "$HERE/physics_override_host_test.cpp" -ldl -o physics_override_host_test

# the plume the other host test uses, once under item\ and once under creature\ (for the creature-path check)
python3 "$HERE/make_test_data.py" > /dev/null
mkdir -p data/creature/foo
cp data/item/objectcomponents/head/plume_test.phys data/creature/foo/
python3 "$R/Tools/m2_extract_phys.py" data/creature/foo/plume_test.phys --overwrite > /dev/null

./physics_override_host_test ./libwxl_physics.so
