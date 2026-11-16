# Hand-over notes

For whoever continues lib-wii-gamecube-kit, person or Claude session. Read
`README.md` and `vendor/PATCHES.md` too. The overall plan, including the
NoIntrovert integration, is in `NOINTROVERT_RVZ_PLAN.md` (kept with the
Dolphin research checkout, `C:\Code\Claude\dolphintool`).

## Status (2026-10-18)

Done and verified on Windows 11 x64 with Visual Studio 2026 (MSVC, Ninja):

- `scripts\build-win.cmd x64` builds everything, runs `ctest` (4/4 pass, 264
  checks in `lib-wii-gamecube-kit-tests`), and copies `lib-wii-gamecube-kit.dll` and `lib-wii-gamecube-kit-cli.exe` to
  `native\out\win-x64`.
- `lib-wii-gamecube-kit.dll` exports exactly the 20 `wii_gamecube_kit_*` functions and depends only
  on KERNEL32, USER32 and ADVAPI32 (static CRT).
- Round trips (ISO -> format -> ISO, byte-identical, hashes equal) pass for
  GameCube and Wii synthetic discs with RVZ zstd/LZMA2/none, WIA
  LZMA/bzip2/purge and GCZ. The Wii disc has a deliberately wrong H0 hash,
  which survives the round trip (hash exceptions work).
- RVZ junk packing works: an 8 MiB synthetic GameCube disc that is mostly
  junk becomes 243 KB.
- Linux x64: `scripts/build-linux.sh` builds and passes ctest 4/4 with GCC 15
  (WSL Ubuntu 26.04). The .so exports only the 20 `wii_gamecube_kit_*` functions and,
  with libstdc++ linked statically, needs nothing but libc.
- The C# binding (`bindings/csharp/LibWiiGameCubeKit`) and the NoIntrovert
  integration are done; the app's self-test covers them on Windows x64.
- **Parity with stock DolphinTool: 51 of 51 outputs byte-identical**
  (`scripts/parity.py`), on the three synthetic discs (GameCube, Wii with a
  wrong hash, clean Wii) for RVZ zstd 1/5/19, LZMA, LZMA2, bzip2 and none,
  scrubbed RVZ, WIA LZMA/LZMA2/bzip2/none, GCZ 16/32 KiB and decoding back to
  ISO. DolphinTool was built from Dolphin at `vendor/DOLPHIN_COMMIT` with
  MSVC, target `dolphin-tool` only:

      git submodule update --init --depth 1   # all but Qt, FFmpeg-bin, mGBA, SDL, rcheevos, libadrenotools
      cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_QT=OFF -DENABLE_TESTS=OFF \
        -DENCODE_FRAMEDUMPS=OFF -DUSE_DISCORD_PRESENCE=OFF -DUSE_MGBA=OFF -DENABLE_AUTOUPDATE=OFF \
        -DUSE_RETRO_ACHIEVEMENTS=OFF -DENABLE_ANALYTICS=OFF -DUSE_UPNP=OFF -DENABLE_SDL=OFF \
        -DENABLE_LLVM=OFF -DENABLE_CUBEB=OFF
      cmake --build build --target dolphin-tool

**Not done yet:**

1. **Real discs.** Only synthetic discs have been tested. Try real GameCube
   and Wii dumps (single and dual layer), checking hashes against Redump.
2. **Windows ARM64 and macOS** have build scripts (`build-win.cmd arm64`,
   `build-mac.sh`: one CMake build per architecture, merged with `lipo`)
   and release-CI steps, but have not been built yet: this machine has no
   ARM64 MSVC tools and no Mac. The first CI run will tell. macOS needs
   AppleClang 21+ (Dolphin's minimum); CI picks the newest non-beta Xcode.
   Linux CI uses g++-12 on Ubuntu 22.04, which is older than the GCC 15 it
   was tried with here.

## Things worth knowing

- **WBFS, CISO and NFS can't be converted.** Dolphin's converters ASSERT that
  the data size is exact, and Dolphin's GUI only offers conversion for
  such images (`GameFile::ShouldAllowConversion`). The library returns
  `WII_GAMECUBE_KIT_ERR_UNSUPPORTED`. Supporting WBFS (common for Wii) would mean
  working out the real disc size (single/dual layer) ourselves, which
  Dolphin deliberately doesn't do. That changes the original plan's scope.
- **Errors** come from Dolphin's `PanicAlert`s. `wii_gamecube_kit_context_create`
  installs an alert handler that stores them per thread for
  `wii_gamecube_kit_last_error()`. Alerts on Dolphin's worker threads have no context and
  are dropped, but Dolphin reports those failures again on the calling thread.
- **Progress ratio** is parsed from Dolphin's progress text ("... Compression
  ratio N%"). The text is never translated here (`GetStringT` is the
  identity), so this is stable, but it's the one place that depends on a
  string.
- **Threads:** WIA/RVZ/GCZ use `wii_gamecube_kit_context_set_num_threads` (default: all
  cores). NoIntrovert runs several jobs in parallel, so it should set this
  from its "threads per job" setting.
- **Output** goes to `<output>.lib-wii-gamecube-kit-tmp` first and is renamed on success.
- **Hashing (0.2.0)** is `src/lib-wii-gamecube-kit-hashing.*`: one thread per
  hash, up to 6 decoders for GCZ/WIA/RVZ/NFS (one reader each, 8 MiB pieces,
  in order), and `HashingBlobReader`, which hashes what a conversion reads.
  On a 1.46 GB synthetic GameCube disc, convert + verify went from 6.3 s
  (convert, hash RVZ, hash ISO, all three hashes each) to 1.4 s (`convert -V`).
- **Read-ahead (0.2.1):** Dolphin's converters read their input on one thread,
  one block at a time, waiting for each read (128 KiB for a GameCube disc at the
  default RVZ block size, 2 MiB for Wii). For a plain ISO/GCM, `convert` reads
  through `ReadAheadBlobReader`: four more file handles read the disc ahead in
  4 MiB pieces, in order, and the converter is served from memory. Plain-image
  hashing reads whole pieces with four handles the same way. From a NAS
  (an SMB share that manages ~60 MB/s at best), ISO -> RVZ zstd 5 went from 20-22 to
  42-46 MB/s for GameCube and from 24 to 56 MB/s for Wii, with byte-identical
  output. From a warm file cache it's as fast as before (0.6 s GameCube, ~2 s Wii
  on 24 cores). Not used when scrubbing (ScrubbedBlob does its own reads).
- **Wii groups in parallel decoders (0.2.2):** a decoder re-encrypts and
  re-hashes every 2 MiB Wii group it reads. Dolphin's `EncryptGroup` starts 64
  hashing tasks and one AES task per core for each group, whatever the context's
  thread count, so six decoders asked for up to ~90 tasks each per group. The
  decoders now set `WiiGroupThreadCount()` (patch in `VolumeWii.cpp`) to their
  share of the context's threads; `EncryptGroup` then hashes 8 blocks per task
  while the decoder reads on, encrypts with that many tasks, and reuses its
  buffers. Every other caller (one reader alone, Dolphin's converters) keeps
  Dolphin's code. Their pieces also start on the game partition's group grid
  (a short first piece), so no group is decoded by two decoders. Hashing a real
  Wii RVZ (4.7 GB, 24 cores, warm cache) takes 2.3 s instead of 2.5 s (the
  SHA-1 thread sets the pace); with 4 threads it's as fast as before on 4 cores
  instead of all 24.
- **Dolphin quirk:** when one `Read` makes the WIA/RVZ reader re-encrypt more
  than one uncached Wii group, hash exceptions collected for the first group
  are applied to the next as well, giving wrong data. Sequential 2 MiB reads
  (`ConvertToPlain`) escape it through the group cache. The parallel decoders
  read one 32 KiB Wii block at a time for this reason; the test disc with a
  wrong H0 hash catches it.
