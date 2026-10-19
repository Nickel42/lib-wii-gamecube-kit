// SPDX-License-Identifier: GPL-2.0-or-later

// Hooks the patched Dolphin sources call into libdolphinrvz (see vendor/PATCHES.md).

#pragma once

namespace drvz
{
// Compression threads for the MultithreadedCompressor being created on this
// thread: the calling context's setting, or all processors.
unsigned int CompressorThreadCount();
}  // namespace drvz
