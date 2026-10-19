// SPDX-License-Identifier: GPL-2.0-or-later

#include "synthetic_discs.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>

#include "Common/Crypto/AES.h"
#include "Common/IOFile.h"
#include "Common/Swap.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/IOSC.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/LaggedFibonacciGenerator.h"
#include "DiscIO/VolumeWii.h"

namespace synth
{
namespace
{
void Put32(std::vector<u8>& d, u64 offset, u32 value)
{
  const u32 be = Common::swap32(value);
  std::memcpy(&d[offset], &be, 4);
}

void PutBytes(std::vector<u8>& d, u64 offset, const void* src, size_t size)
{
  std::memcpy(&d[offset], src, size);
}

// Fills [begin, end) with junk the way Nintendo's mastering tools do: a lagged
// Fibonacci stream restarted every 32 KiB from a seed that depends on the position.
void FillJunk(std::vector<u8>& d, u64 begin, u64 end, u32 disc_seed)
{
  constexpr u64 BLOCK = 0x8000;
  for (u64 block_start = begin - begin % BLOCK; block_start < end; block_start += BLOCK)
  {
    u32 seed[DiscIO::LaggedFibonacciGenerator::SEED_SIZE];
    for (size_t i = 0; i < std::size(seed); ++i)
      seed[i] = disc_seed * 0x9E3779B1u ^ static_cast<u32>(block_start / BLOCK) * 0x85EBCA6Bu ^
                static_cast<u32>(i) * 0xC2B2AE35u;
    DiscIO::LaggedFibonacciGenerator lfg;
    lfg.SetSeed(seed);
    const u64 from = std::max(begin, block_start);
    const u64 to = std::min(end, block_start + BLOCK);
    lfg.Forward(from - block_start);
    lfg.GetBytes(to - from, &d[from]);
  }
}

struct File
{
  const char* name;
  u64 offset;
  u32 size;
  u8 seed;
};

// Disc header plus FST for a GameCube-layout volume (a GC disc, or the inside of
// a Wii partition). Wii stores the FST offsets shifted right by 2.
void WriteVolumeHeader(std::vector<u8>& d, const char* game_id, const char* name, bool wii,
                       u64 fst_offset, const std::vector<File>& files)
{
  std::memcpy(&d[0], game_id, 6);
  if (wii)
    Put32(d, 0x18, DiscIO::WII_DISC_MAGIC);
  else
    Put32(d, 0x1C, DiscIO::GAMECUBE_DISC_MAGIC);
  std::strncpy(reinterpret_cast<char*>(&d[0x20]), name, 0x3E0 - 1);

  const int shift = wii ? 2 : 0;
  // A minimal DOL (empty header) right after the apploader area.
  const u64 dol_offset = 0x8000;
  Put32(d, 0x420, static_cast<u32>(dol_offset >> shift));

  std::vector<u8> fst((files.size() + 1) * 12);
  std::string strings;
  fst[0] = 1;  // root directory
  Put32(fst, 8, static_cast<u32>(files.size() + 1));
  for (size_t i = 0; i < files.size(); ++i)
  {
    const size_t e = (i + 1) * 12;
    Put32(fst, e, static_cast<u32>(strings.size()));  // flags 0 = file, name offset
    Put32(fst, e + 4, static_cast<u32>(files[i].offset >> shift));
    Put32(fst, e + 8, files[i].size);
    strings += files[i].name;
    strings += '\0';
  }
  fst.insert(fst.end(), strings.begin(), strings.end());
  PutBytes(d, fst_offset, fst.data(), fst.size());
  Put32(d, 0x424, static_cast<u32>(fst_offset >> shift));
  Put32(d, 0x428, static_cast<u32>(fst.size() >> shift) + 1);
  Put32(d, 0x42C, static_cast<u32>(fst.size() >> shift) + 1);

  for (const File& f : files)
  {
    // Compressible but not trivial file contents.
    for (u32 i = 0; i < f.size; ++i)
      d[f.offset + i] = static_cast<u8>((i * 7 + f.seed) ^ (i >> 9));
  }
}

// Junk everywhere outside the header area, the FST and the files.
void FillGaps(std::vector<u8>& d, u64 data_start, u64 fst_offset, u64 fst_end,
              const std::vector<File>& files, u32 disc_seed)
{
  std::vector<std::pair<u64, u64>> used = {{0, data_start}, {fst_offset, fst_end}};
  for (const File& f : files)
    used.emplace_back(f.offset, f.offset + f.size);
  std::sort(used.begin(), used.end());
  u64 pos = 0;
  for (const auto& [b, e] : used)
  {
    if (b > pos)
      FillJunk(d, pos, b, disc_seed);
    pos = std::max(pos, e);
  }
  if (pos < d.size())
    FillJunk(d, pos, d.size(), disc_seed);
}
}  // namespace

std::vector<u8> MakeGameCubeDisc(u64 size)
{
  std::vector<u8> d(size, 0);
  const std::vector<File> files = {
      {"opening.bnr", 0x40000, 0x1960, 1},
      {"data.bin", 0x100000, 0x180000, 2},
      {"stream.adp", 0x400000, 0x123457, 3},
  };
  const u64 fst_offset = 0x20000;
  WriteVolumeHeader(d, "GTSE01", "libdolphinrvz GameCube test disc", false, fst_offset, files);
  Put32(d, 0x458, 1);  // bi2.bin region: NTSC-U
  const u64 fst_end = fst_offset + (files.size() + 1) * 12 + 64;
  FillGaps(d, 0x8100, fst_offset, fst_end, files, 0x1234);
  return d;
}

std::vector<u8> MakeWiiDisc(u32 groups, bool bad_hash)
{
  using DiscIO::VolumeWii;
  constexpr u64 PARTITION = 0x50000;
  constexpr u64 TMD_OFFSET = 0x2C0;
  constexpr u64 H3_OFFSET = 0x8000;
  constexpr u64 DATA_OFFSET = 0x20000;
  const u64 data_size = groups * VolumeWii::GROUP_TOTAL_SIZE;
  const u64 decrypted_size = groups * VolumeWii::GROUP_DATA_SIZE;
  const u64 disc_size = PARTITION + DATA_OFFSET + data_size + 0x100000;

  std::vector<u8> d(disc_size, 0);
  // Outer disc header: Wii magic, hashes and encryption on (0x60/0x61 = 0).
  std::memcpy(&d[0], "RTSP01", 6);
  Put32(d, 0x18, DiscIO::WII_DISC_MAGIC);
  std::strncpy(reinterpret_cast<char*>(&d[0x20]), "libdolphinrvz Wii test disc", 0x3E0 - 1);
  Put32(d, 0x40000, 1);                 // one partition in group 0
  Put32(d, 0x40004, 0x40020 >> 2);      // its table
  Put32(d, 0x40020, PARTITION >> 2);    // partition offset
  Put32(d, 0x40024, 0);                 // type 0 = game
  Put32(d, 0x4E000, 1);                 // region info: NTSC-U

  // Ticket, title key wrapped with the retail common key.
  const std::array<u8, 16> title_key = {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,
                                        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
  const u64 title_id = 0x00010000'52545350ULL;  // 00010000-RTSP
  IOS::ES::Ticket ticket{};
  ticket.signature.type = static_cast<decltype(ticket.signature.type)>(Common::swap32(0x00010001));
  std::strcpy(ticket.signature.issuer, "Root-CA00000001-XS00000003");
  ticket.title_id = Common::swap64(title_id);
  ticket.common_key_index = 0;
  {
    u8 iv[16] = {};
    const u64 be = Common::swap64(title_id);
    std::memcpy(iv, &be, 8);
    IOS::HLE::IOSC iosc;
    iosc.Encrypt(IOS::HLE::IOSC::HANDLE_COMMON_KEY, iv, title_key.data(), 16, ticket.title_key,
                 IOS::PID_ES);
  }
  PutBytes(d, PARTITION, &ticket, sizeof(ticket));

  // TMD with one content.
  constexpr u32 TMD_SIZE = sizeof(IOS::ES::TMDHeader) + sizeof(IOS::ES::Content);
  IOS::ES::TMDHeader tmd{};
  tmd.signature.type = static_cast<decltype(ticket.signature.type)>(Common::swap32(0x00010001));
  std::strcpy(tmd.signature.issuer, "Root-CA00000001-CP00000004");
  tmd.title_id = Common::swap64(title_id);
  tmd.ios_id = Common::swap64(0x00000001'00000024ULL);
  tmd.num_contents = Common::swap16(1);
  PutBytes(d, PARTITION + TMD_OFFSET, &tmd, sizeof(tmd));
  Put32(d, PARTITION + DiscIO::WII_PARTITION_TMD_SIZE_ADDRESS, TMD_SIZE);
  Put32(d, PARTITION + DiscIO::WII_PARTITION_TMD_OFFSET_ADDRESS, TMD_OFFSET >> 2);
  Put32(d, PARTITION + DiscIO::WII_PARTITION_H3_OFFSET_ADDRESS, H3_OFFSET >> 2);
  Put32(d, PARTITION + 0x2B8, DATA_OFFSET >> 2);
  Put32(d, PARTITION + 0x2BC, static_cast<u32>(data_size >> 2));

  // The partition's decrypted contents: a GameCube-layout volume.
  std::vector<u8> inner(decrypted_size, 0);
  const std::vector<File> files = {
      {"opening.bnr", 0x30000, 0x5000, 4},
      {"main.dol.bin", 0x60000, 0x123456, 5},
      {"sound.brsar", decrypted_size - 0x300000, 0x111111, 6},
  };
  const u64 fst_offset = 0x10000;
  WriteVolumeHeader(inner, "RTSP01", "libdolphinrvz Wii test disc", true, fst_offset, files);
  const u64 fst_end = fst_offset + (files.size() + 1) * 12 + 64;
  FillGaps(inner, 0x8100, fst_offset, fst_end, files, 0x5678);

  // Hash and encrypt each group like Dolphin's VolumeWii::EncryptGroup.
  const auto aes = Common::AES::CreateContextEncrypt(title_key.data());
  std::vector<std::array<u8, VolumeWii::BLOCK_DATA_SIZE>> blocks(VolumeWii::BLOCKS_PER_GROUP);
  std::vector<VolumeWii::HashBlock> hashes(VolumeWii::BLOCKS_PER_GROUP);
  for (u32 g = 0; g < groups; ++g)
  {
    for (u32 b = 0; b < VolumeWii::BLOCKS_PER_GROUP; ++b)
    {
      std::memcpy(blocks[b].data(),
                  &inner[g * VolumeWii::GROUP_DATA_SIZE + b * VolumeWii::BLOCK_DATA_SIZE],
                  VolumeWii::BLOCK_DATA_SIZE);
    }
    VolumeWii::HashGroup(blocks.data(), hashes.data());
    if (bad_hash && g == 1)
      hashes[5].h0[3][7] ^= 0x5A;  // a hash that doesn't match its data
    for (u32 b = 0; b < VolumeWii::BLOCKS_PER_GROUP; ++b)
    {
      u8* out = &d[PARTITION + DATA_OFFSET + g * VolumeWii::GROUP_TOTAL_SIZE +
                   b * VolumeWii::BLOCK_TOTAL_SIZE];
      aes->CryptIvZero(reinterpret_cast<const u8*>(&hashes[b]), out, VolumeWii::BLOCK_HEADER_SIZE);
      aes->Crypt(out + 0x3D0, blocks[b].data(), out + VolumeWii::BLOCK_HEADER_SIZE,
                 VolumeWii::BLOCK_DATA_SIZE);
    }
  }
  // The space after the partition is left as zeros, like many real discs.
  (void)H3_OFFSET;
  return d;
}

bool WriteFile(const std::string& path, const std::vector<u8>& data)
{
  ::File::IOFile f(path, "wb");
  return f.WriteBytes(data.data(), data.size());
}

std::vector<u8> ReadFile(const std::string& path)
{
  ::File::IOFile f(path, "rb");
  std::vector<u8> data(f.GetSize());
  if (!f.ReadBytes(data.data(), data.size()))
    data.clear();
  return data;
}
}  // namespace synth
