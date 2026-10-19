# Third-party notices

libdolphinrvz is licensed under the GNU General Public License, version 2 or
(at your option) any later version (`LICENSE`), because most of it is Dolphin's
code. It bundles the following, all built statically into `dolphinrvz.dll` /
`libdolphinrvz.so` / `libdolphinrvz.dylib`.

| Component | Version | License | License text |
|---|---|---|---|
| [Dolphin](https://github.com/dolphin-emu/dolphin) (DiscIO, parts of Common and Core/IOS) | commit in `vendor/DOLPHIN_COMMIT` | GPL-2.0-or-later; `LaggedFibonacciGenerator.*` CC0-1.0 | `vendor/dolphin/COPYING`, `vendor/dolphin/LICENSES/` |
| [zstd](https://github.com/facebook/zstd) | Dolphin's pinned submodule | BSD-3-Clause (or GPL-2.0) | `vendor/externals/zstd/zstd/LICENSE` |
| [bzip2](https://sourceware.org/bzip2/) | Dolphin's pinned submodule | bzip2 license (BSD-like) | `vendor/externals/bzip2/bzip2/LICENSE` |
| [liblzma (XZ Utils)](https://tukaani.org/xz/) | as vendored by Dolphin | 0BSD / public domain | `vendor/externals/liblzma/COPYING` |
| [zlib-ng](https://github.com/zlib-ng/zlib-ng) | Dolphin's pinned submodule | zlib license | `vendor/externals/zlib-ng/zlib-ng/LICENSE.md` |
| [{fmt}](https://github.com/fmtlib/fmt) | Dolphin's pinned submodule | MIT | `vendor/externals/fmt/fmt/LICENSE` |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | 2.28, as vendored by Dolphin | Apache-2.0 | `vendor/externals/mbedtls/LICENSE` |

All of these licenses are compatible with the GPL (Apache-2.0 with GPL version
3, so a combined work that includes Mbed TLS is distributed under GPL-3.0, as
NoIntrovert is).

The library contains the Wii common keys (from Dolphin's `Core/IOS/IOSC.cpp`),
which are needed to read the encrypted partitions of Wii discs.
