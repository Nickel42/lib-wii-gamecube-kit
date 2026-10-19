# Changes to the vendored Dolphin sources

`vendor/dolphin/` is copied from Dolphin at the commit in `vendor/DOLPHIN_COMMIT`
by `scripts/vendor-dolphin.sh`, normalised to LF, and then patched with
`patches/*.patch`. Never edit `vendor/dolphin/` by hand: change the patch, and
re-run the script to check it applies.

Every change is behind `#ifdef DRVZ_BUILD` / `#ifndef DRVZ_BUILD`, so the
patched files still build inside Dolphin.

`patches/0001-libdolphinrvz-build.patch`:

| File | Change | Why |
|---|---|---|
| `DiscIO/Blob.cpp` | No `DirectoryBlobReader` fallback | Extracted-folder input pulls in much more of Dolphin, and NoIntrovert only reads image files |
| `DiscIO/Volume.cpp` | No `CreateDiscForCore`, WAD volumes or `CachedBlob` | They need Dolphin's config system and memory arenas; the library only opens discs |
| `DiscIO/MultithreadedCompressor.h` | Thread count from `drvz::CompressorThreadCount()` | Lets each context set its thread count (`drvz_context_set_num_threads`), instead of always using every core |
| `Core/IOS/ES/Formats.h/.cpp` | No `SharedContentMap` / `UIDSys` | They work on the emulated NAND; only ticket/TMD parsing is needed |
| `Core/IOS/IOSC.cpp` | Don't call `LoadEntries()` | Never read a `keys.bin` from a user directory; the built-in Wii common keys are enough for discs |

## Stand-ins (`src/shims/`)

These are libdolphinrvz files that take the place of Dolphin headers and
sources that are not vendored. The include path puts `src/shims` first.

| File | Replaces | Contents |
|---|---|---|
| `Core/IOS/Device.h` | Dolphin's emulated IOS device header | Only the `IOS::HLE::ReturnCode` enum, copied unchanged |
| `LogManagerShim.cpp` | `Common/Logging/LogManager.cpp` | `GenericLogFmtImpl` forwarding warnings and errors to the context's log callback |
| `drvz_hooks.h` | (new) | The hook the `MultithreadedCompressor.h` patch calls |

Dolphin's `Common/MsgHandler.cpp` is used unchanged: `drvz_context_create`
registers an alert handler, so `PanicAlert` messages become the context's
error text instead of message boxes.

## Upgrading Dolphin

1. Check out the new Dolphin commit with the four submodules listed in
   `scripts/vendor-dolphin.sh`, and update `DOLPHIN_COMMIT` there.
2. Run the script. If the patch no longer applies, fix it against the new
   sources (`diff -ruN` of a fresh copy against your edited copy, paths
   starting with `Source/`).
3. If the build then needs Dolphin files that aren't vendored, add them to
   `core_files` in the script, or stub them.
4. Rebuild and run `ctest`.
