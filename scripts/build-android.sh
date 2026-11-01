#!/bin/bash
# Builds lib-wii-gamecube-kit for Android (arm64-v8a and x86_64) with the NDK
# and copies libwiigckit.so into artifacts/native/android-arm64 and
# artifacts/native/android-x64, the .NET runtime identifiers.
#
#   ANDROID_NDK_HOME=/path/to/ndk scripts/build-android.sh [abi...]
#
# Needs NDK r27 or newer (Clang 18+, Dolphin's minimum) and CMake 3.22+.
# The minimum Android version is 9 (API 28), the first with iconv in bionic,
# which Dolphin uses for Shift-JIS game titles. Override with ANDROID_API.
# The tests are built for the host, not here; they can't run on this machine.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
ndk="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-${ANDROID_NDK:-}}}"
if [ -z "$ndk" ] || [ ! -f "$ndk/build/cmake/android.toolchain.cmake" ]; then
    echo "Set ANDROID_NDK_HOME to an Android NDK (r27 or newer)." >&2
    exit 1
fi
api="${ANDROID_API:-28}"
abis=("$@")
[ ${#abis[@]} -gt 0 ] || abis=(arm64-v8a x86_64)

generator=()
if command -v ninja > /dev/null; then generator=(-G Ninja); fi

for abi in "${abis[@]}"; do
    case "$abi" in
        arm64-v8a) rid=android-arm64 ;;
        x86_64) rid=android-x64 ;;
        *) echo "Unsupported ABI $abi (arm64-v8a and x86_64 only)" >&2; exit 1 ;;
    esac
    build="$here/build-$rid"
    cmake -S "$here" -B "$build" "${generator[@]}" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$api" -DANDROID_STL=c++_static \
        -DDRVZ_BUILD_TESTS=OFF -DDRVZ_BUILD_CLI=OFF
    cmake --build "$build" --parallel --target wiigckit
    mkdir -p "$here/artifacts/native/$rid"
    cp -L "$build/libwiigckit.so" "$here/artifacts/native/$rid/libwiigckit.so"
    echo "Built $here/artifacts/native/$rid/libwiigckit.so"
done
