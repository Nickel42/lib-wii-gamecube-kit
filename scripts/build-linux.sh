#!/bin/bash
# Builds lib-wii-gamecube-kit for Linux on this machine's architecture (x64 or
# arm64), runs its tests, and copies libwiigckit.so and drvz-cli into
# artifacts/native/linux-<arch>.
#
#   scripts/build-linux.sh
#
# Needs CMake 3.22+, make or Ninja, and GCC 12+ or Clang 18+ (Dolphin's
# minimums). Set CC/CXX to pick the compiler, e.g. CC=gcc-12 CXX=g++-12.
# The library links libstdc++ statically, so it only needs the glibc of the
# machine it was built on (or newer).
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
case "$(uname -m)" in
    x86_64) arch=x64 ;;
    aarch64|arm64) arch=arm64 ;;
    *) echo "Unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac
build="$here/build-linux-$arch"
out="$here/artifacts/native/linux-$arch"
jobs="$(nproc)"

generator=()
if command -v ninja > /dev/null; then generator=(-G Ninja); fi

cmake -S "$here" -B "$build" "${generator[@]}" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --parallel "$jobs"
ctest --test-dir "$build" --output-on-failure

mkdir -p "$out"
cp -L "$build/libwiigckit.so" "$out/libwiigckit.so"
cp "$build/drvz-cli" "$out/drvz-cli"
echo "Built $out/libwiigckit.so"
