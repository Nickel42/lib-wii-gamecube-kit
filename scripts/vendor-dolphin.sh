#!/usr/bin/env bash
# Copies the parts of Dolphin that libdolphinrvz builds from into vendor/.
#
#   scripts/vendor-dolphin.sh <dolphin checkout>
#
# The checkout must be at DOLPHIN_COMMIT with the zstd, bzip2, fmt and zlib-ng
# submodules initialised:
#   git clone https://github.com/dolphin-emu/dolphin.git && cd dolphin
#   git checkout $DOLPHIN_COMMIT
#   git submodule update --init --depth 1 Externals/zstd/zstd Externals/bzip2/bzip2 \
#     Externals/fmt/fmt Externals/zlib-ng/zlib-ng
#
# Vendored files are copied unmodified. Every change to them is applied by
# scripts/apply-patches.sh from patches/, and listed in vendor/PATCHES.md.
set -euo pipefail

DOLPHIN_COMMIT=1822e3859f061c6f81076c94eb4ba7e6e0a3706c

src="${1:?usage: vendor-dolphin.sh <dolphin checkout>}"
here="$(cd "$(dirname "$0")/.." && pwd)"
dst="$here/vendor"

actual="$(git -C "$src" rev-parse HEAD)"
if [ "$actual" != "$DOLPHIN_COMMIT" ]; then
  echo "error: $src is at $actual, expected $DOLPHIN_COMMIT" >&2
  exit 1
fi

rm -rf "$dst/dolphin" "$dst/externals"
mkdir -p "$dst/dolphin/Source/Core" "$dst/externals"

core_files=(
  # DiscIO: blob readers, volumes, file systems, scrubbing, WIA/RVZ
  DiscIO/Blob.cpp DiscIO/Blob.h
  DiscIO/CISOBlob.cpp DiscIO/CISOBlob.h
  DiscIO/CompressedBlob.cpp DiscIO/CompressedBlob.h
  DiscIO/DiscExtractor.cpp DiscIO/DiscExtractor.h
  DiscIO/DiscScrubber.cpp DiscIO/DiscScrubber.h
  DiscIO/DiscUtils.cpp DiscIO/DiscUtils.h
  DiscIO/Enums.cpp DiscIO/Enums.h
  DiscIO/FileBlob.cpp DiscIO/FileBlob.h
  DiscIO/FileSystemGCWii.cpp DiscIO/FileSystemGCWii.h
  DiscIO/Filesystem.cpp DiscIO/Filesystem.h
  DiscIO/LaggedFibonacciGenerator.cpp DiscIO/LaggedFibonacciGenerator.h
  DiscIO/MultithreadedCompressor.h
  DiscIO/NFSBlob.cpp DiscIO/NFSBlob.h
  DiscIO/ScrubbedBlob.cpp DiscIO/ScrubbedBlob.h
  DiscIO/SplitFileBlob.cpp DiscIO/SplitFileBlob.h
  DiscIO/TGCBlob.cpp DiscIO/TGCBlob.h
  DiscIO/Volume.cpp DiscIO/Volume.h
  DiscIO/VolumeDisc.cpp DiscIO/VolumeDisc.h
  DiscIO/VolumeGC.cpp DiscIO/VolumeGC.h
  DiscIO/VolumeWii.cpp DiscIO/VolumeWii.h
  DiscIO/WbfsBlob.cpp DiscIO/WbfsBlob.h
  DiscIO/WIABlob.cpp DiscIO/WIABlob.h
  DiscIO/WIACompression.cpp DiscIO/WIACompression.h
  DiscIO/WiiEncryptionCache.cpp DiscIO/WiiEncryptionCache.h
  DiscIO/WiiSaveBanner.cpp DiscIO/WiiSaveBanner.h
  # Common: the utilities DiscIO uses
  Common/Align.h Common/Assert.h Common/BitUtils.h Common/Buffer.h
  Common/CPUDetect.h Common/x64CPUDetect.cpp Common/ArmCPUDetect.cpp Common/GenericCPUDetect.cpp
  Common/ChunkFile.h Common/ColorUtil.cpp Common/ColorUtil.h Common/Common.h
  Common/CommonFuncs.cpp Common/CommonFuncs.h Common/CommonPaths.h Common/CommonTypes.h
  Common/Contains.h
  Common/Crypto/AES.cpp Common/Crypto/AES.h Common/Crypto/SHA1.cpp Common/Crypto/SHA1.h
  Common/Crypto/bn.cpp Common/Crypto/bn.h Common/Crypto/ec.cpp Common/Crypto/ec.h
  Common/DirectIOFile.cpp Common/DirectIOFile.h
  Common/DynamicLibrary.cpp Common/DynamicLibrary.h
  Common/EnumMap.h Common/Event.h Common/FileUtil.cpp Common/FileUtil.h Common/Flag.h
  Common/FormatUtil.h Common/Functional.h Common/Hash.cpp Common/Hash.h Common/HookableEvent.h
  Common/IOFile.cpp Common/IOFile.h Common/Inline.h Common/Intrinsics.h Common/Lazy.h
  Common/Logging/Log.h Common/MathUtil.h Common/MsgHandler.cpp Common/MsgHandler.h
  Common/Mutex.h Common/NandPaths.cpp Common/NandPaths.h Common/Projection.h
  Common/Random.cpp Common/Random.h Common/SPSCQueue.h Common/ScopeGuard.h
  Common/StringUtil.cpp Common/StringUtil.h Common/Swap.h Common/Timer.cpp Common/Timer.h
  Common/TypeUtils.h
  # Core: ticket/TMD parsing and the Wii common keys (both patched, see PATCHES.md)
  Core/CommonTitles.h
  Core/IOS/ES/Formats.cpp Core/IOS/ES/Formats.h
  Core/IOS/IOSC.cpp Core/IOS/IOSC.h Core/IOS/Uids.h
)
for f in "${core_files[@]}"; do
  mkdir -p "$dst/dolphin/Source/Core/$(dirname "$f")"
  cp "$src/Source/Core/$f" "$dst/dolphin/Source/Core/$f"
done
# Dolphin's sources are LF in its repository; undo a CRLF checkout so patches apply.
find "$dst/dolphin" -type f \( -name '*.cpp' -o -name '*.h' \) -exec sed -i 's/\r$//' {} +
cp "$src/COPYING" "$dst/dolphin/COPYING"
mkdir -p "$dst/dolphin/LICENSES"
cp "$src"/LICENSES/* "$dst/dolphin/LICENSES/"
cp "$src/docs/WiaAndRvz.md" "$dst/dolphin/WiaAndRvz.md"

# Externals: library sources and licenses only (no tests, docs, programs).
ext="$src/Externals"
copy_tree() { # <from> <to> [rsync-style excludes...]
  local from="$1" to="$2"; shift 2
  mkdir -p "$to"
  tar -C "$from" --exclude=.git "$@" -cf - . | tar -C "$to" -xf -
}
# The layout matches Dolphin's Externals/ so Dolphin's own wrapper CMakeLists.txt
# files (zstd, bzip2, liblzma, zlib-ng) work unchanged. fmt and mbedtls get
# small wrappers of ours in cmake/.
mkdir -p "$dst/externals/zstd/zstd/build" "$dst/externals/bzip2" "$dst/externals/zlib-ng" "$dst/externals/fmt/fmt"
cp "$ext/zstd/CMakeLists.txt" "$dst/externals/zstd/"
cp "$ext/zstd/zstd/CMakeLists.txt" "$dst/externals/zstd/zstd/"
copy_tree "$ext/zstd/zstd/lib" "$dst/externals/zstd/zstd/lib" --exclude=dll --exclude=legacy --exclude=deprecated --exclude=dictBuilder
copy_tree "$ext/zstd/zstd/build/cmake" "$dst/externals/zstd/zstd/build/cmake" --exclude=programs --exclude=tests --exclude=contrib
cp "$ext/zstd/zstd/LICENSE" "$ext/zstd/zstd/COPYING" "$dst/externals/zstd/zstd/"
cp "$ext/bzip2/CMakeLists.txt" "$dst/externals/bzip2/"
mkdir -p "$dst/externals/bzip2/bzip2"
for f in blocksort.c huffman.c crctable.c randtable.c compress.c decompress.c bzlib.c bzlib.h bzlib_private.h LICENSE; do
  cp "$ext/bzip2/bzip2/$f" "$dst/externals/bzip2/bzip2/"
done
copy_tree "$ext/liblzma" "$dst/externals/liblzma"
copy_tree "$ext/mbedtls" "$dst/externals/mbedtls" --exclude=programs --exclude=tests --exclude=docs --exclude=doxygen --exclude=visualc --exclude=configs --exclude=ChangeLog.d
copy_tree "$ext/fmt/fmt/include" "$dst/externals/fmt/fmt/include"
mkdir -p "$dst/externals/fmt/fmt/src"
cp "$ext/fmt/fmt/src/format.cc" "$ext/fmt/fmt/src/os.cc" "$dst/externals/fmt/fmt/src/"
cp "$ext/fmt/fmt/LICENSE"* "$dst/externals/fmt/fmt/"
cp "$ext/zlib-ng/CMakeLists.txt" "$ext/zlib-ng/zconf.h" "$ext/zlib-ng/zlib.h" "$ext/zlib-ng/zlib_name_mangling.h" "$dst/externals/zlib-ng/"
copy_tree "$ext/zlib-ng/zlib-ng" "$dst/externals/zlib-ng/zlib-ng" --exclude=test --exclude=doc --exclude=.github --exclude=tools
cp "$ext/licenses.md" "$dst/externals/dolphin-externals-licenses.md"

# Every bundled license in one file, shipped with release downloads.
{
  echo "Licenses of the code bundled in libdolphinrvz (see THIRD_PARTY_NOTICES.md)."
  for entry in \
      "Dolphin (COPYING):dolphin/COPYING" \
      "Dolphin (GPL-2.0-or-later):dolphin/LICENSES/GPL-2.0-or-later.txt" \
      "Dolphin (CC0-1.0, LaggedFibonacciGenerator):dolphin/LICENSES/CC0-1.0.txt" \
      "zstd:externals/zstd/zstd/LICENSE" \
      "bzip2:externals/bzip2/bzip2/LICENSE" \
      "liblzma (XZ Utils):externals/liblzma/COPYING" \
      "zlib-ng:externals/zlib-ng/zlib-ng/LICENSE.md" \
      "{fmt}:externals/fmt/fmt/LICENSE" \
      "Mbed TLS:externals/mbedtls/LICENSE"; do
    printf '\n\n==================== %s ====================\n\n' "${entry%%:*}"
    sed 's/\r$//' "$dst/${entry#*:}"
  done
} > "$dst/BUNDLED_LICENSES.txt"

# Our changes to the Dolphin sources (see vendor/PATCHES.md).
for p in "$here"/patches/*.patch; do
  [ -e "$p" ] || continue
  patch -d "$dst/dolphin" -p1 --no-backup-if-mismatch < "$p"
done

echo "$DOLPHIN_COMMIT" > "$dst/DOLPHIN_COMMIT"
echo "Vendored Dolphin $DOLPHIN_COMMIT into $dst"
