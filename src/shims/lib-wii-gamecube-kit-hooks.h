// SPDX-License-Identifier: GPL-2.0-or-later

// Hooks the patched Dolphin sources call into lib-wii-gamecube-kit (see vendor/PATCHES.md).

#pragma once

namespace wii_gamecube_kit
{
// Compression threads for the MultithreadedCompressor being created on this
// thread: the calling context's setting, or all processors.
unsigned int CompressorThreadCount();

// Threads VolumeWii::EncryptGroup may use for each Wii group it re-encrypts on this
// thread, or 0 for Dolphin's own scheduling (a task per block, and one per core). The
// library's parallel decoders set it, since several of them re-encrypt at once.
unsigned int WiiGroupThreadCount();
}  // namespace wii_gamecube_kit
