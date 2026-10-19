# libdolphinrvz

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
patched, see `vendor/PATCHES.md`), so its output should be identical to
DolphinTool's for the same settings.

## API

`include/dolphinrvz.h` follows the same conventions as libchdman's
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

## Building

CMake 3.25+ and a C++23 compiler (Visual Studio 2022 17.10+ or 2026, recent
Clang or GCC). All dependencies are in `vendor/`.

```
scripts\build-win.cmd x64          # or arm64; output in ..\out\win-<arch>
```

or by hand:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The build produces `dolphinrvz` (shared), `dolphinrvz_static`, `drvz-cli` and
the tests. Only the `drvz_*` functions are exported.

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
