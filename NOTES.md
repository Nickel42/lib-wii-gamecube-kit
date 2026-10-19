# Hand-over notes

For whoever continues libdolphinrvz, person or Claude session. Read
`README.md` and `vendor/PATCHES.md` too. The overall plan, including the
NoIntrovert integration, is in `NOINTROVERT_RVZ_PLAN.md` (kept with the
Dolphin research checkout, `C:\Code\Claude\dolphintool`).

## Status (2026-10-18)

Done and verified on Windows 11 x64 with Visual Studio 2026 (MSVC, Ninja):

- `scripts\build-win.cmd x64` builds everything, runs `ctest` (4/4 pass, 264
  checks in `drvz_tests`), and copies `dolphinrvz.dll` and `drvz-cli.exe` to
  `native\out\win-x64`.
- `dolphinrvz.dll` exports exactly the 18 `drvz_*` functions and depends only
  on KERNEL32, USER32 and ADVAPI32 (static CRT).
- Round trips (ISO -> format -> ISO, byte-identical, hashes equal) pass for
  GameCube and Wii synthetic discs with RVZ zstd/LZMA2/none, WIA
  LZMA/bzip2/purge and GCZ. The Wii disc has a deliberately wrong H0 hash,
  which survives the round trip (hash exceptions work).
- RVZ junk packing works: an 8 MiB synthetic GameCube disc that is mostly
  junk becomes 243 KB.
- Linux x64: `scripts/build-linux.sh` builds and passes ctest 4/4 with GCC 15
  (WSL Ubuntu 26.04). The .so exports only the 18 `drvz_*` functions and,
  with libstdc++ linked statically, needs nothing but libc.
- The C# binding (`bindings/csharp/LibDolphinRvz`) and the NoIntrovert
  integration are done; the app's self-test covers them on Windows x64.

**Not done yet:**

1. **Parity with stock DolphinTool** (byte-identical output for the same
   input and settings). It is the same code, but no reference DolphinTool was
   built or downloaded here. Build Dolphin's `dolphin-tool` at
   `vendor/DOLPHIN_COMMIT`, or use the matching official build, and compare
   on the synthetic discs and a few real ones.
2. **Real discs.** Only synthetic discs have been tested. Try real GameCube
   and Wii dumps (single and dual layer), checking hashes against Redump.
3. **Windows ARM64 and macOS** have build scripts (`build-win.cmd arm64`,
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
  `DRVZ_ERR_UNSUPPORTED`. Supporting WBFS (common for Wii) would mean
  working out the real disc size (single/dual layer) ourselves, which
  Dolphin deliberately doesn't do. That changes the original plan's scope.
- **Errors** come from Dolphin's `PanicAlert`s. `drvz_context_create`
  installs an alert handler that stores them per thread for
  `drvz_last_error()`. Alerts on Dolphin's worker threads have no context and
  are dropped, but Dolphin reports those failures again on the calling thread.
- **Progress ratio** is parsed from Dolphin's progress text ("... Compression
  ratio N%"). The text is never translated here (`GetStringT` is the
  identity), so this is stable, but it's the one place that depends on a
  string.
- **Threads:** WIA/RVZ/GCZ use `drvz_context_set_num_threads` (default: all
  cores). NoIntrovert runs several jobs in parallel, so it should set this
  from its "threads per job" setting.
- **Output** goes to `<output>.drvz-tmp` first and is renamed on success.
