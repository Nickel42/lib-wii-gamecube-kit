// SPDX-License-Identifier: GPL-2.0-or-later

// Builds small but well-formed GameCube and Wii disc images for the tests: a
// real FST with files, Nintendo-style junk padding (a lagged Fibonacci stream,
// which RVZ packs into seeds), and for Wii an encrypted, hashed partition with
// a ticket whose title key is wrapped with the retail common key. Optionally
// one H0 hash is made wrong, so the WIA/RVZ hash-exception path is exercised.

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace synth
{
std::vector<u8> MakeGameCubeDisc(u64 size);
std::vector<u8> MakeWiiDisc(u32 groups, bool bad_hash);
bool WriteFile(const std::string& path, const std::vector<u8>& data);
std::vector<u8> ReadFile(const std::string& path);
}  // namespace synth
