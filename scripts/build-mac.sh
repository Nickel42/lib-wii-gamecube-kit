#!/bin/bash
# Builds lib-wii-gamecube-kit for macOS as one universal (arm64 + x86_64) dylib,
# runs the tests of the slice this Mac can run, and copies
# lib-wii-gamecube-kit.dylib into artifacts/native/osx.
#
#   scripts/build-mac.sh
#
# Dolphin picks CPU-specific code per architecture at compile time, so each
# architecture is its own CMake build; lipo merges them. Needs CMake 3.22+
# and Xcode with AppleClang 21+ (Dolphin's minimum).
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
out="$here/artifacts/native/osx"
jobs="$(sysctl -n hw.ncpu)"
native_arch="$(uname -m)"

for arch in arm64 x86_64; do
    build="$here/build-osx-$arch"
    tests=ON
    [ "$arch" = "$native_arch" ] || tests=OFF   # can't run the other slice's tests here
    cmake -S "$here" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$arch" \
        -DWII_GAMECUBE_KIT_BUILD_TESTS="$tests"
    cmake --build "$build" --parallel "$jobs"
    if [ "$tests" = ON ]; then ctest --test-dir "$build" --output-on-failure; fi
done

mkdir -p "$out"
lipo -create "$here/build-osx-arm64/lib-wii-gamecube-kit.dylib" "$here/build-osx-x86_64/lib-wii-gamecube-kit.dylib" \
    -output "$out/lib-wii-gamecube-kit.dylib"
lipo -info "$out/lib-wii-gamecube-kit.dylib"
echo "Built $out/lib-wii-gamecube-kit.dylib"
