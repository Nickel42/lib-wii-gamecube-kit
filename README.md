# lib-wii-gamecube-kit

Dolphin's GameCube/Wii disc image conversion as a library with a C ABI. It
does what `DolphinTool convert` does, plus probing and hashing, without
Dolphin's emulator, config system, user directory or UI.

- **Reads:** ISO/GCM (including split `.part0.iso` sets), GCZ, WIA, RVZ and TGC.
  CISO, WBFS and NFS can be probed and hashed, but **not converted**: like
  Dolphin, the library only converts images whose container records the
  disc's exact size.
- **Writes:** RVZ, WIA, GCZ and ISO.
- **Compression:** Zstandard (RVZ), LZMA, LZMA2 and bzip2 (WIA and RVZ),
  purge (WIA), or none. Optional scrubbing.
- **Wii:** partitions are decrypted with the built-in common keys, and hash
  mismatches are kept as hash exceptions, so every conversion round-trips
  byte for byte.

The code is Dolphin's own (vendored from one pinned commit and lightly
patched, see `vendor/PATCHES.md`), so its output is identical to
DolphinTool's for the same settings (checked byte for byte, see `scripts/parity.py`).

## API

`include/dolphinrvz.h` follows the same conventions as lib-chd-kit's
`chdman.h`: UTF-8 paths, one `drvz_context` per thread, `struct_size`-versioned
option and result structs, a progress callback that can cancel, a log
callback, and `drvz_last_error()`.

```c
drvz_context *ctx;
drvz_context_create(&ctx);

drvz_disc_info info;
drvz_disc_info_init(&info);
drvz_probe(ctx, "game.iso", &info);        /* GameCube or Wii? game ID, sizes... */

drvz_convert_options opts;
drvz_convert_options_init(&opts);          /* RVZ, zstd 5, 128 KiB: Dolphin's defaults */
opts.input = "game.iso";
opts.output = "game.rvz";
if (drvz_convert(ctx, &opts) != DRVZ_OK)
    puts(drvz_last_error(ctx));

drvz_hash_result hash;                     /* CRC32/MD5/SHA-1 of the plain disc */
drvz_hash_result_init(&hash);
drvz_hash(ctx, "game.rvz", &hash);
drvz_context_destroy(ctx);
```

Output is written to `<output>.drvz-tmp` and renamed when complete, so a
failed or cancelled conversion leaves nothing behind.

## Platforms

| Platform | .NET runtime identifier | Native library | Built by |
|---|---|---|---|
| Windows x64 | `win-x64` | `wiigckit.dll` | `scriptsuild-win.cmd x64` |
| Windows ARM64 | `win-arm64` | `wiigckit.dll` | `scriptsuild-win.cmd arm64` |
| macOS 11+ (arm64 + x86_64) | `osx-arm64`, `osx-x64` | `libwiigckit.dylib` (universal) | `scripts/build-mac.sh` |
| Linux x64 / arm64 (glibc 2.35+) | `linux-x64`, `linux-arm64` | `libwiigckit.so` | `scripts/build-linux.sh` |
| Android 9+ (API 28) arm64-v8a / x86_64 | `android-arm64`, `android-x64` | `libwiigckit.so` | `scripts/build-android.sh` |

Every script puts its output in `artifacts/native/<rid>`. Each library exports
only the `drvz_*` functions and links its C/C++ runtime statically, so it
needs nothing beyond the OS's own libraries.

## .NET

`bindings/csharp/LibWiiGameCubeKit` is the .NET binding (`net8.0`, so any
.NET 8+ app on any of the platforms above, Android included):

```csharp
using LibWiiGameCubeKit;

using var drvz = new DolphinRvz(threads: 0);       // 0 = all cores
drvz.Progress = new Progress<DrvzProgress>(p => Console.Write($"{p.Fraction:P0}"));

DiscInfo info = drvz.Probe("game.iso");            // GameCube or Wii, game ID, sizes...
if (info.CanConvert)
    drvz.Convert("game.iso", "game.rvz", new ConvertSettings());  // RVZ, zstd 5, 128 KiB
DiscHashes hashes = drvz.Hash("game.rvz");         // CRC32/MD5/SHA-1 of the plain disc
```

Operations block (run them on a worker thread), take a `CancellationToken`,
and throw `DolphinRvzException` with Dolphin's message on failure. One
instance runs one operation at a time; use several for parallel work.

Two ways to consume it:

- **NuGet package.** `dotnet pack bindings/csharp/LibWiiGameCubeKit` packs
  every native library found in `artifacts/native` as `runtimes/<rid>/native`,
  and NuGet picks the right one per platform (including into Android APKs).
  The Build workflow (`.github/workflows/build.yml`, run by hand or on a `v*`
  tag) builds all platforms and packs the complete package.
- **Project reference** from a checkout or git submodule. Reference
  `LibWiiGameCubeKit.csproj` and import
  `bindings/csharp/LibWiiGameCubeKit/build/LibWiiGameCubeKit.targets`, which
  copies the native library for the platform being built (from
  `artifacts/native/<rid>`) next to the app, also in single-file publishes.

## Building

CMake 3.22+ and a C++23 compiler (Visual Studio 2022 17.10+ or 2026, GCC 12+,
Clang 18+, AppleClang 21+ or Android NDK r27+, as Dolphin needs). All
dependencies are in `vendor/`.

```
scriptsuild-win.cmd x64                             # or arm64
scripts/build-mac.sh
scripts/build-linux.sh
ANDROID_NDK_HOME=/path/to/ndk scripts/build-android.sh  # arm64-v8a and x86_64
```

or by hand:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The build produces `wiigckit` (shared), `wiigckit_static`, `drvz-cli` and
the tests. Only the `drvz_*` functions are exported.

On Android, Dolphin's code is built as plain POSIX code (Dolphin's own
Android paths go through its app's JNI layer for `content://` URIs), so the
library takes ordinary file paths, such as those in the app's storage.

`drvz-cli` is a small front end for trying things out:

```
drvz-cli probe game.rvz
drvz-cli hash game.rvz
drvz-cli convert -i game.iso -o game.rvz -f rvz -c zstd -l 5 -b 131072
```

## Tests

`tests/drvz_tests` generates synthetic GameCube and Wii discs (a real FST,
Nintendo-style junk padding, and an encrypted, hashed Wii partition with one
deliberately wrong hash), then checks round trips through every format and
compression method, deterministic output across thread counts, option
validation, cancellation, scrubbing and Unicode paths.

## License

GPL-2.0-or-later, see `LICENSE` and `THIRD_PARTY_NOTICES.md`.
