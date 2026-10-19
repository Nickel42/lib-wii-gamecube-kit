#!/bin/bash
# Builds libdolphinrvz for Linux x64, runs its tests, and copies
# libdolphinrvz.so into native/out/linux-x64 (next to the archive engine).
#
#   native/libdolphinrvz/scripts/build-linux.sh
#
# Needs CMake 3.22+, make or Ninja, and GCC 12+ or Clang 18+ (Dolphin's
# minimums). Set CC/CXX to pick the compiler, e.g. CC=gcc-12 CXX=g++-12.
# The library links libstdc++ statically, so it only needs the glibc of the
# machine it was built on (or newer).
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
build="$here/build-linux-x64"
out="$here/../out/linux-x64"
jobs="$(nproc)"

generator=()
if command -v ninja > /dev/null; then generator=(-G Ninja); fi

cmake -S "$here" -B "$build" "${generator[@]}" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --parallel "$jobs"
ctest --test-dir "$build" --output-on-failure

mkdir -p "$out"
cp -L "$build/libdolphinrvz.so" "$out/libdolphinrvz.so"
cp "$build/drvz-cli" "$out/drvz-cli"
echo "Built $out/libdolphinrvz.so"
