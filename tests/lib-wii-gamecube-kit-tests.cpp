// SPDX-License-Identifier: GPL-2.0-or-later

// lib-wii-gamecube-kit tests, run by ctest. Everything goes through the public C API;
// Dolphin's internals are only used to build the synthetic input discs.
//
//   lib-wii-gamecube-kit-tests <work dir>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "lib-wii-gamecube-kit.h"
#include "synthetic_discs.h"

namespace fs = std::filesystem;

namespace
{
int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                   \
  do                                                                                  \
  {                                                                                   \
    ++g_checks;                                                                       \
    if (!(cond))                                                                      \
    {                                                                                 \
      ++g_failures;                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
    }                                                                                 \
  } while (0)

#define CHECK_STATUS(expr, expected)                                                       \
  do                                                                                       \
  {                                                                                        \
    ++g_checks;                                                                            \
    const wii_gamecube_kit_status _s = (expr);                                                         \
    if (_s != (expected))                                                                  \
    {                                                                                      \
      ++g_failures;                                                                        \
      std::fprintf(stderr, "%s:%d: %s returned %d (%s), expected %d. last error: %s\n",    \
                   __FILE__, __LINE__, #expr, static_cast<int>(_s), wii_gamecube_kit_status_string(_s), \
                   static_cast<int>(expected), wii_gamecube_kit_last_error(g_ctx));                    \
    }                                                                                      \
  } while (0)

wii_gamecube_kit_context* g_ctx = nullptr;
fs::path g_dir;

// A path from a UTF-8 string (std::filesystem::u8path is deprecated in C++20).
fs::path U8(const std::string& s)
{
  return fs::path(std::u8string(s.begin(), s.end()));
}

std::string P(const char* name)
{
  const std::u8string u8 = (g_dir / fs::path(std::u8string(reinterpret_cast<const char8_t*>(name)))).u8string();
  return std::string(u8.begin(), u8.end());
}

// Logs and progress go here so tests can look at them.
std::vector<std::string> g_log;
std::vector<double> g_fractions;
double g_last_ratio = 0;  // last non-zero ratio reported
std::function<int(double)> g_progress_hook;

void WII_GAMECUBE_KIT_CALL OnLog(void*, wii_gamecube_kit_log_level level, const char* message)
{
  g_log.push_back(std::to_string(level) + ": " + message);
}

int WII_GAMECUBE_KIT_CALL OnProgress(void*, wii_gamecube_kit_phase, double fraction, double ratio)
{
  g_fractions.push_back(fraction);
  if (ratio > 0)
    g_last_ratio = ratio;
  return g_progress_hook ? g_progress_hook(fraction) : 0;
}

void ResetObservations()
{
  g_log.clear();
  g_fractions.clear();
  g_last_ratio = 0;
  g_progress_hook = nullptr;
}

wii_gamecube_kit_hash_result Hash(const std::string& path)
{
  wii_gamecube_kit_hash_result r;
  wii_gamecube_kit_hash_result_init(&r);
  CHECK_STATUS(wii_gamecube_kit_hash(g_ctx, path.c_str(), &r), WII_GAMECUBE_KIT_OK);
  return r;
}

wii_gamecube_kit_hash_result HashOnly(const std::string& path, unsigned hashes)
{
  wii_gamecube_kit_hash_options o;
  wii_gamecube_kit_hash_options_init(&o);
  o.hashes = hashes;
  wii_gamecube_kit_hash_result r;
  wii_gamecube_kit_hash_result_init(&r);
  CHECK_STATUS(wii_gamecube_kit_hash_ex(g_ctx, path.c_str(), &o, &r), WII_GAMECUBE_KIT_OK);
  return r;
}

bool SameHashes(const wii_gamecube_kit_hash_result& a, const wii_gamecube_kit_hash_result& b)
{
  return std::strcmp(a.sha1, b.sha1) == 0 && std::strcmp(a.md5, b.md5) == 0 &&
         std::strcmp(a.crc32, b.crc32) == 0 && a.size == b.size;
}

wii_gamecube_kit_status Convert(const std::string& in, const std::string& out, wii_gamecube_kit_format format,
                    wii_gamecube_kit_compression compression, int level, uint32_t block_size,
                    bool overwrite = true, bool scrub = false,
                    wii_gamecube_kit_hash_result* source = nullptr)
{
  wii_gamecube_kit_convert_options o;
  wii_gamecube_kit_convert_options_init(&o);
  o.input = in.c_str();
  o.output = out.c_str();
  o.format = format;
  o.compression = compression;
  o.compression_level = level;
  o.block_size = block_size;
  o.overwrite = overwrite;
  o.scrub = scrub;
  if (source != nullptr)
  {
    wii_gamecube_kit_hash_result_init(source);
    o.source_hashes = WII_GAMECUBE_KIT_HASH_ALL;
    o.source_hash = source;
  }
  return wii_gamecube_kit_convert(g_ctx, &o);
}

bool SameFile(const std::string& a, const std::string& b)
{
  const std::vector<u8> x = synth::ReadFile(a);
  const std::vector<u8> y = synth::ReadFile(b);
  return !x.empty() && x == y;
}

bool NoTempFiles()
{
  for (const auto& e : fs::directory_iterator(g_dir))
    if (e.path().extension() == ".lib-wii-gamecube-kit-tmp")
      return false;
  return true;
}

void TestBasics()
{
  CHECK(wii_gamecube_kit_abi_version() == WII_GAMECUBE_KIT_ABI_VERSION);
  CHECK(std::strstr(wii_gamecube_kit_version(), "lib-wii-gamecube-kit") != nullptr);
  CHECK(std::strcmp(wii_gamecube_kit_status_string(WII_GAMECUBE_KIT_OK), "Success") == 0);
  CHECK(std::strcmp(wii_gamecube_kit_status_string(static_cast<wii_gamecube_kit_status>(1234)), "Unknown status") == 0);

  wii_gamecube_kit_convert_options o;
  wii_gamecube_kit_convert_options_init(&o);
  CHECK(o.struct_size == sizeof(o));
  CHECK(o.format == WII_GAMECUBE_KIT_FORMAT_RVZ && o.compression == WII_GAMECUBE_KIT_COMPRESSION_ZSTD);
  CHECK(o.compression_level == 5 && o.block_size == 0x20000 && !o.scrub && !o.overwrite);

  // Block sizes (DiscUtils.h): RVZ 32 KiB..2 MiB powers of two, then multiples of 2 MiB.
  CHECK(wii_gamecube_kit_block_size_valid(WII_GAMECUBE_KIT_FORMAT_RVZ, 0x8000));
  CHECK(wii_gamecube_kit_block_size_valid(WII_GAMECUBE_KIT_FORMAT_RVZ, 0x20000));
  CHECK(!wii_gamecube_kit_block_size_valid(WII_GAMECUBE_KIT_FORMAT_RVZ, 0x4000));
  CHECK(!wii_gamecube_kit_block_size_valid(WII_GAMECUBE_KIT_FORMAT_RVZ, 0x30000));
  CHECK(wii_gamecube_kit_block_size_valid(WII_GAMECUBE_KIT_FORMAT_RVZ, 0x600000));
  CHECK(!wii_gamecube_kit_block_size_valid(WII_GAMECUBE_KIT_FORMAT_WIA, 0x20000));
  CHECK(wii_gamecube_kit_block_size_valid(WII_GAMECUBE_KIT_FORMAT_WIA, 0x200000));
  CHECK(wii_gamecube_kit_block_size_valid(WII_GAMECUBE_KIT_FORMAT_GCZ, 0x4000));
  CHECK(!wii_gamecube_kit_block_size_valid(WII_GAMECUBE_KIT_FORMAT_GCZ, 0x3000));
  CHECK(wii_gamecube_kit_block_size_valid(WII_GAMECUBE_KIT_FORMAT_ISO, 0));
  CHECK(!wii_gamecube_kit_block_size_valid(WII_GAMECUBE_KIT_FORMAT_RVZ, 0));

  int min = -1, max = -1;
  wii_gamecube_kit_compression_levels(WII_GAMECUBE_KIT_COMPRESSION_ZSTD, &min, &max);
  CHECK(min < 0 && max == 22);
  wii_gamecube_kit_compression_levels(WII_GAMECUBE_KIT_COMPRESSION_LZMA2, &min, &max);
  CHECK(min == 1 && max == 9);
  wii_gamecube_kit_compression_levels(WII_GAMECUBE_KIT_COMPRESSION_NONE, &min, &max);
  CHECK(min == 0 && max == 0);
}

void TestProbe(const std::string& gc_iso, const std::string& wii_iso)
{
  wii_gamecube_kit_disc_info info;
  wii_gamecube_kit_disc_info_init(&info);
  CHECK_STATUS(wii_gamecube_kit_probe(g_ctx, gc_iso.c_str(), &info), WII_GAMECUBE_KIT_OK);
  CHECK(info.container == WII_GAMECUBE_KIT_CONTAINER_PLAIN);
  CHECK(info.platform == WII_GAMECUBE_KIT_PLATFORM_GAMECUBE);
  CHECK(std::strcmp(info.game_id, "GTSE01") == 0);
  CHECK(std::strcmp(info.maker_id, "01") == 0);
  CHECK(std::strcmp(info.internal_name, "lib-wii-gamecube-kit GameCube test disc") == 0);
  CHECK(info.region == WII_GAMECUBE_KIT_REGION_NTSC_U);
  CHECK(info.data_size == fs::file_size(U8(gc_iso)) && info.data_size_is_exact);
  CHECK(!info.is_nkit && !info.is_datel && info.is_scrubbable);

  wii_gamecube_kit_disc_info_init(&info);
  CHECK_STATUS(wii_gamecube_kit_probe(g_ctx, wii_iso.c_str(), &info), WII_GAMECUBE_KIT_OK);
  CHECK(info.platform == WII_GAMECUBE_KIT_PLATFORM_WII);
  CHECK(std::strcmp(info.game_id, "RTSP01") == 0);

  // Older/smaller struct sizes are refused; a bigger one is accepted and written partly.
  wii_gamecube_kit_disc_info_init(&info);
  info.struct_size = sizeof(size_t);
  CHECK_STATUS(wii_gamecube_kit_probe(g_ctx, gc_iso.c_str(), &info), WII_GAMECUBE_KIT_ERR_ABI_MISMATCH);

  // Not a disc.
  const std::string junk = P("not-a-disc.iso");
  synth::WriteFile(junk, std::vector<u8>(0x10000, 0xAB));
  wii_gamecube_kit_disc_info_init(&info);
  CHECK_STATUS(wii_gamecube_kit_probe(g_ctx, junk.c_str(), &info), WII_GAMECUBE_KIT_ERR_NOT_A_DISC);
  CHECK(std::strstr(wii_gamecube_kit_last_error(g_ctx), "not a GameCube or Wii") != nullptr);
  CHECK_STATUS(wii_gamecube_kit_probe(g_ctx, P("missing.iso").c_str(), &info), WII_GAMECUBE_KIT_ERR_FILE_OPEN);
}

// ISO -> format -> ISO must give back the identical image, and the container's
// hashes must match the ISO's.
void RoundTrip(const std::string& iso, const char* tag, wii_gamecube_kit_format format,
               wii_gamecube_kit_compression compression, int level, uint32_t block_size)
{
  const std::string packed = P((std::string(tag) + ".packed").c_str());
  const std::string back = P((std::string(tag) + ".back.iso").c_str());
  ResetObservations();
  wii_gamecube_kit_hash_result source;
  CHECK_STATUS(Convert(iso, packed, format, compression, level, block_size, true, false, &source),
               WII_GAMECUBE_KIT_OK);
  CHECK(!g_fractions.empty() && g_fractions.back() == 1.0);
  CHECK(fs::exists(U8(packed)));

  wii_gamecube_kit_disc_info info;
  wii_gamecube_kit_disc_info_init(&info);
  CHECK_STATUS(wii_gamecube_kit_probe(g_ctx, packed.c_str(), &info), WII_GAMECUBE_KIT_OK);
  const wii_gamecube_kit_container expected = format == WII_GAMECUBE_KIT_FORMAT_RVZ ? WII_GAMECUBE_KIT_CONTAINER_RVZ :
                                  format == WII_GAMECUBE_KIT_FORMAT_WIA ? WII_GAMECUBE_KIT_CONTAINER_WIA :
                                  format == WII_GAMECUBE_KIT_FORMAT_GCZ ? WII_GAMECUBE_KIT_CONTAINER_GCZ :
                                                              WII_GAMECUBE_KIT_CONTAINER_PLAIN;
  CHECK(info.container == expected);
  if (format != WII_GAMECUBE_KIT_FORMAT_ISO)
    CHECK(info.block_size == block_size);

  const wii_gamecube_kit_hash_result a = Hash(iso);
  const wii_gamecube_kit_hash_result b = Hash(packed);
  CHECK(SameHashes(a, b));
  CHECK(SameHashes(a, source) && source.size_is_exact);
  // One decoder instead of several gives the same hashes.
  wii_gamecube_kit_context_set_num_threads(g_ctx, 1);
  CHECK(SameHashes(a, Hash(packed)));
  wii_gamecube_kit_context_set_num_threads(g_ctx, 0);

  CHECK_STATUS(Convert(packed, back, WII_GAMECUBE_KIT_FORMAT_ISO, WII_GAMECUBE_KIT_COMPRESSION_NONE, 0, 0,
                       true, false, &source),
               WII_GAMECUBE_KIT_OK);
  CHECK(SameFile(iso, back));
  CHECK(SameHashes(a, source));
  std::printf("  %-28s %9llu -> %9llu bytes\n", tag,
              static_cast<unsigned long long>(fs::file_size(U8(iso))),
              static_cast<unsigned long long>(fs::file_size(U8(packed))));
  fs::remove(U8(packed));
  fs::remove(U8(back));
}

void TestRoundTrips(const std::string& gc_iso, const std::string& wii_iso)
{
  for (const auto& [iso, name] : {std::pair{gc_iso, "gc"}, std::pair{wii_iso, "wii"}})
  {
    const std::string t = name;
    RoundTrip(iso, (t + "-rvz-zstd5-128k").c_str(), WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_ZSTD, 5, 0x20000);
    RoundTrip(iso, (t + "-rvz-zstd19-2m").c_str(), WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_ZSTD, 19, 0x200000);
    RoundTrip(iso, (t + "-rvz-lzma2-32k").c_str(), WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_LZMA2, 6, 0x8000);
    RoundTrip(iso, (t + "-rvz-none").c_str(), WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_NONE, 0, 0x20000);
    RoundTrip(iso, (t + "-wia-lzma").c_str(), WII_GAMECUBE_KIT_FORMAT_WIA, WII_GAMECUBE_KIT_COMPRESSION_LZMA, 5, 0x200000);
    RoundTrip(iso, (t + "-wia-bzip2").c_str(), WII_GAMECUBE_KIT_FORMAT_WIA, WII_GAMECUBE_KIT_COMPRESSION_BZIP2, 9, 0x200000);
    RoundTrip(iso, (t + "-wia-purge").c_str(), WII_GAMECUBE_KIT_FORMAT_WIA, WII_GAMECUBE_KIT_COMPRESSION_PURGE, 0, 0x200000);
    RoundTrip(iso, (t + "-gcz-32k").c_str(), WII_GAMECUBE_KIT_FORMAT_GCZ, WII_GAMECUBE_KIT_COMPRESSION_NONE, 0, 0x8000);
  }
}

void TestRvzPacksJunk(const std::string& gc_iso)
{
  // Junk is a lagged Fibonacci stream, which no compressor can shrink; RVZ stores
  // its seeds instead. Without that, most of this disc would stay incompressible.
  const std::string rvz = P("packing.rvz");
  ResetObservations();
  CHECK_STATUS(Convert(gc_iso, rvz, WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_ZSTD, 5, 0x20000), WII_GAMECUBE_KIT_OK);
  const auto iso_size = fs::file_size(U8(gc_iso));
  const auto rvz_size = fs::file_size(U8(rvz));
  CHECK(rvz_size * 10 < iso_size);
  CHECK(g_last_ratio > 0.0 && g_last_ratio < 0.2);
  fs::remove(U8(rvz));
}

void TestDeterministic(const std::string& wii_iso)
{
  // Dolphin writes groups in order, so the output must not depend on the thread count.
  const std::string a = P("threads1.rvz");
  const std::string b = P("threads7.rvz");
  wii_gamecube_kit_context_set_num_threads(g_ctx, 1);
  CHECK_STATUS(Convert(wii_iso, a, WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_ZSTD, 3, 0x20000), WII_GAMECUBE_KIT_OK);
  wii_gamecube_kit_context_set_num_threads(g_ctx, 7);
  CHECK_STATUS(Convert(wii_iso, b, WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_ZSTD, 3, 0x20000), WII_GAMECUBE_KIT_OK);
  wii_gamecube_kit_context_set_num_threads(g_ctx, 0);
  CHECK(SameFile(a, b));
  fs::remove(U8(a));
  fs::remove(U8(b));
}

void TestValidation(const std::string& gc_iso)
{
  const std::string out = P("validation.rvz");
  fs::remove(U8(out));
  CHECK_STATUS(Convert(gc_iso, out, WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_ZSTD, 5, 0x30000),
               WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT);
  CHECK_STATUS(Convert(gc_iso, out, WII_GAMECUBE_KIT_FORMAT_WIA, WII_GAMECUBE_KIT_COMPRESSION_ZSTD, 5, 0x200000),
               WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT);
  CHECK_STATUS(Convert(gc_iso, out, WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_PURGE, 0, 0x20000),
               WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT);
  CHECK_STATUS(Convert(gc_iso, out, WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_ZSTD, 23, 0x20000),
               WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT);
  CHECK_STATUS(Convert(gc_iso, out, WII_GAMECUBE_KIT_FORMAT_WIA, WII_GAMECUBE_KIT_COMPRESSION_LZMA2, 0, 0x200000),
               WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT);
  CHECK_STATUS(Convert(gc_iso, gc_iso, WII_GAMECUBE_KIT_FORMAT_ISO, WII_GAMECUBE_KIT_COMPRESSION_NONE, 0, 0),
               WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT);
  CHECK_STATUS(Convert(P("missing.iso"), out, WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_ZSTD, 5, 0x20000),
               WII_GAMECUBE_KIT_ERR_FILE_OPEN);
  CHECK(!fs::exists(U8(out)));

  wii_gamecube_kit_convert_options o;
  wii_gamecube_kit_convert_options_init(&o);
  CHECK_STATUS(wii_gamecube_kit_convert(g_ctx, &o), WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT);  // no paths
  o.struct_size = 8;
  CHECK_STATUS(wii_gamecube_kit_convert(g_ctx, &o), WII_GAMECUBE_KIT_ERR_ABI_MISMATCH);

  // Existing output: refused unless overwrite, and replaced whole when allowed.
  synth::WriteFile(out, std::vector<u8>(100, 1));
  CHECK_STATUS(Convert(gc_iso, out, WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_ZSTD, 5, 0x20000, false),
               WII_GAMECUBE_KIT_ERR_FILE_EXISTS);
  CHECK(fs::file_size(U8(out)) == 100);
  CHECK_STATUS(Convert(gc_iso, out, WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_ZSTD, 5, 0x20000, true),
               WII_GAMECUBE_KIT_OK);
  CHECK(fs::file_size(U8(out)) > 100);
  CHECK(std::strcmp(wii_gamecube_kit_last_error(g_ctx), "") == 0);
  fs::remove(U8(out));

  // Not a disc.
  CHECK_STATUS(Convert(P("not-a-disc.iso"), out, WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_ZSTD, 5, 0x20000),
               WII_GAMECUBE_KIT_ERR_NOT_A_DISC);

  // Warnings DolphinTool prints become log lines.
  ResetObservations();
  CHECK_STATUS(Convert(gc_iso, out, WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_ZSTD, 1, 0x400000), WII_GAMECUBE_KIT_OK);
  bool warned = false;
  for (const std::string& line : g_log)
    warned |= line.find("Block size is not ideal") != std::string::npos;
  CHECK(warned);
  fs::remove(U8(out));
  CHECK(NoTempFiles());
}

void TestCancel(const std::string& wii_iso)
{
  const std::string out = P("cancelled.rvz");
  fs::remove(U8(out));

  // From the progress callback, part way through.
  ResetObservations();
  g_progress_hook = [](double fraction) { return fraction > 0.3 ? 1 : 0; };
  wii_gamecube_kit_context_set_num_threads(g_ctx, 1);
  CHECK_STATUS(Convert(wii_iso, out, WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_ZSTD, 5, 0x8000),
               WII_GAMECUBE_KIT_ERR_CANCELLED);
  wii_gamecube_kit_context_set_num_threads(g_ctx, 0);
  CHECK(!fs::exists(U8(out)));
  CHECK(NoTempFiles());

  // Requested before the call: fails before any work, and the request is then cleared.
  ResetObservations();
  wii_gamecube_kit_context_cancel(g_ctx);
  CHECK_STATUS(Convert(wii_iso, out, WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_ZSTD, 5, 0x20000),
               WII_GAMECUBE_KIT_ERR_CANCELLED);
  CHECK(g_fractions.empty());
  wii_gamecube_kit_hash_result r;
  wii_gamecube_kit_hash_result_init(&r);
  CHECK_STATUS(wii_gamecube_kit_hash(g_ctx, wii_iso.c_str(), &r), WII_GAMECUBE_KIT_OK);

  // Cancelling a hash.
  g_progress_hook = [](double fraction) { return fraction > 0.1 ? 1 : 0; };
  CHECK_STATUS(wii_gamecube_kit_hash(g_ctx, wii_iso.c_str(), &r), WII_GAMECUBE_KIT_ERR_CANCELLED);
  ResetObservations();
}

void TestScrub(const std::string& gc_iso)
{
  const std::string out = P("scrubbed.rvz");
  const std::string iso = P("scrubbed.iso");
  wii_gamecube_kit_hash_result source;
  CHECK_STATUS(Convert(gc_iso, out, WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_ZSTD, 5, 0x20000, true, true,
                       &source),
               WII_GAMECUBE_KIT_OK);
  CHECK(SameHashes(source, Hash(out)));
  CHECK(std::strcmp(source.sha1, Hash(gc_iso).sha1) != 0);
  CHECK_STATUS(Convert(out, iso, WII_GAMECUBE_KIT_FORMAT_ISO, WII_GAMECUBE_KIT_COMPRESSION_NONE, 0, 0), WII_GAMECUBE_KIT_OK);
  // Scrubbing zeroes the junk, so the image changes but keeps its size and files.
  CHECK(fs::file_size(U8(iso)) == fs::file_size(U8(gc_iso)));
  CHECK(!SameFile(iso, gc_iso));
  const std::vector<u8> a = synth::ReadFile(gc_iso);
  const std::vector<u8> b = synth::ReadFile(iso);
  CHECK(a.size() == b.size() && std::equal(a.begin() + 0x100000, a.begin() + 0x280000, b.begin() + 0x100000));
  fs::remove(U8(out));
  fs::remove(U8(iso));
}

void TestHashOptions(const std::string& gc_iso, const std::string& wii_iso)
{
  for (const std::string& iso : {gc_iso, wii_iso})
  {
    const wii_gamecube_kit_hash_result all = Hash(iso);
    CHECK(std::strlen(all.crc32) == 8 && std::strlen(all.md5) == 32 && std::strlen(all.sha1) == 40);

    const wii_gamecube_kit_hash_result sha1 = HashOnly(iso, WII_GAMECUBE_KIT_HASH_SHA1);
    CHECK(std::strcmp(sha1.sha1, all.sha1) == 0 && sha1.md5[0] == '\0' && sha1.crc32[0] == '\0');
    CHECK(sha1.size == all.size && sha1.size_is_exact);
    const wii_gamecube_kit_hash_result md5 = HashOnly(iso, WII_GAMECUBE_KIT_HASH_MD5);
    CHECK(std::strcmp(md5.md5, all.md5) == 0 && md5.sha1[0] == '\0');
    const wii_gamecube_kit_hash_result crc = HashOnly(iso, WII_GAMECUBE_KIT_HASH_CRC32);
    CHECK(std::strcmp(crc.crc32, all.crc32) == 0 && crc.md5[0] == '\0');
    CHECK(SameHashes(HashOnly(iso, WII_GAMECUBE_KIT_HASH_ALL), all));

    wii_gamecube_kit_hash_result r;
    wii_gamecube_kit_hash_result_init(&r);
    CHECK_STATUS(wii_gamecube_kit_hash_ex(g_ctx, iso.c_str(), nullptr, &r), WII_GAMECUBE_KIT_OK);
    CHECK(SameHashes(r, all));
  }

  wii_gamecube_kit_hash_options o;
  wii_gamecube_kit_hash_options_init(&o);
  CHECK(o.struct_size == sizeof(o) && o.hashes == WII_GAMECUBE_KIT_HASH_ALL);
  wii_gamecube_kit_hash_result r;
  wii_gamecube_kit_hash_result_init(&r);
  o.hashes = 0;
  CHECK_STATUS(wii_gamecube_kit_hash_ex(g_ctx, gc_iso.c_str(), &o, &r), WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT);
  o.hashes = 8;
  CHECK_STATUS(wii_gamecube_kit_hash_ex(g_ctx, gc_iso.c_str(), &o, &r), WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT);
  o.hashes = WII_GAMECUBE_KIT_HASH_SHA1;
  o.struct_size = 4;
  CHECK_STATUS(wii_gamecube_kit_hash_ex(g_ctx, gc_iso.c_str(), &o, &r), WII_GAMECUBE_KIT_ERR_ABI_MISMATCH);

  // Source hashing: argument checks.
  const std::string out = P("source-hash.rvz");
  wii_gamecube_kit_convert_options c;
  wii_gamecube_kit_convert_options_init(&c);
  CHECK(c.source_hashes == 0 && c.source_hash == nullptr);
  c.input = gc_iso.c_str();
  c.output = out.c_str();
  c.overwrite = 1;
  c.source_hashes = WII_GAMECUBE_KIT_HASH_SHA1;
  CHECK_STATUS(wii_gamecube_kit_convert(g_ctx, &c), WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT);  // no source_hash
  c.source_hash = &r;
  c.source_hashes = 16;
  CHECK_STATUS(wii_gamecube_kit_convert(g_ctx, &c), WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT);
  CHECK(!fs::exists(U8(out)));

  // A caller built against ABI 1 (0.1.0) passes the shorter struct: the new fields are
  // ignored, whatever follows it in memory.
  c.struct_size = offsetof(wii_gamecube_kit_convert_options, source_hashes);
  c.source_hashes = 16;
  c.source_hash = nullptr;
  CHECK_STATUS(wii_gamecube_kit_convert(g_ctx, &c), WII_GAMECUBE_KIT_OK);
  c.struct_size = c.struct_size - 1;
  CHECK_STATUS(wii_gamecube_kit_convert(g_ctx, &c), WII_GAMECUBE_KIT_ERR_ABI_MISMATCH);

  // SHA-1 only, during a conversion.
  wii_gamecube_kit_convert_options_init(&c);
  c.input = wii_iso.c_str();
  c.output = out.c_str();
  c.overwrite = 1;
  c.source_hashes = WII_GAMECUBE_KIT_HASH_SHA1;
  c.source_hash = &r;
  CHECK_STATUS(wii_gamecube_kit_convert(g_ctx, &c), WII_GAMECUBE_KIT_OK);
  CHECK(std::strcmp(r.sha1, Hash(wii_iso).sha1) == 0 && r.md5[0] == '\0' && r.crc32[0] == '\0');
  CHECK(std::strcmp(r.sha1, HashOnly(out, WII_GAMECUBE_KIT_HASH_SHA1).sha1) == 0);

  // Cancelling a conversion that is hashing as well.
  ResetObservations();
  g_progress_hook = [](double fraction) { return fraction > 0.3 ? 1 : 0; };
  c.block_size = 0x8000;
  wii_gamecube_kit_context_set_num_threads(g_ctx, 1);
  CHECK_STATUS(wii_gamecube_kit_convert(g_ctx, &c), WII_GAMECUBE_KIT_ERR_CANCELLED);
  wii_gamecube_kit_context_set_num_threads(g_ctx, 0);
  ResetObservations();
  CHECK(NoTempFiles());
  fs::remove(U8(out));

  // Discs big enough for several decoders, each decoding its own 8 MiB pieces in parallel,
  // which for Wii means groups that straddle two pieces.
  const std::string big_wii = P("big-wii.iso");
  const std::string big_gc = P("big-gc.iso");
  synth::WriteFile(big_wii, synth::MakeWiiDisc(14, true));
  synth::WriteFile(big_gc, synth::MakeGameCubeDisc(0x2A00000));
  for (const std::string& iso : {big_wii, big_gc})
  {
    const wii_gamecube_kit_hash_result expected = Hash(iso);
    for (const auto format : {WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_FORMAT_WIA, WII_GAMECUBE_KIT_FORMAT_GCZ})
    {
      const auto compression = format == WII_GAMECUBE_KIT_FORMAT_RVZ ? WII_GAMECUBE_KIT_COMPRESSION_ZSTD :
                               format == WII_GAMECUBE_KIT_FORMAT_WIA ? WII_GAMECUBE_KIT_COMPRESSION_LZMA2 :
                                                                       WII_GAMECUBE_KIT_COMPRESSION_NONE;
      const uint32_t block = format == WII_GAMECUBE_KIT_FORMAT_RVZ ? 0x20000 :
                             format == WII_GAMECUBE_KIT_FORMAT_WIA ? 0x200000 : 0x8000;
      wii_gamecube_kit_hash_result source;
      CHECK_STATUS(Convert(iso, out, format, compression, 1, block, true, false, &source), WII_GAMECUBE_KIT_OK);
      CHECK(SameHashes(source, expected));
      CHECK(SameHashes(Hash(out), expected));
      wii_gamecube_kit_context_set_num_threads(g_ctx, 3);
      CHECK(SameHashes(Hash(out), expected));
      wii_gamecube_kit_context_set_num_threads(g_ctx, 0);
    }
  }
  fs::remove(U8(out));
  fs::remove(U8(big_wii));
  fs::remove(U8(big_gc));
}

void TestUnicodePaths(const std::string& gc_iso)
{
  const fs::path dir = g_dir / fs::path(u8"ゲーム テスト");
  fs::create_directories(dir);
  const std::u8string in8 = (dir / fs::path(u8"ディスク.iso")).u8string();
  const std::u8string out8 = (dir / fs::path(u8"ディスク.rvz")).u8string();
  const std::string in(in8.begin(), in8.end());
  const std::string out(out8.begin(), out8.end());
  fs::copy_file(U8(gc_iso), fs::path(in8), fs::copy_options::overwrite_existing);
  CHECK_STATUS(Convert(in, out, WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_ZSTD, 5, 0x20000), WII_GAMECUBE_KIT_OK);
  wii_gamecube_kit_disc_info info;
  wii_gamecube_kit_disc_info_init(&info);
  CHECK_STATUS(wii_gamecube_kit_probe(g_ctx, out.c_str(), &info), WII_GAMECUBE_KIT_OK);
  CHECK(info.container == WII_GAMECUBE_KIT_CONTAINER_RVZ);
  fs::remove_all(dir);
}
}  // namespace

int main(int argc, char** argv)
{
  g_dir = argc > 1 ? U8(argv[1]) : fs::current_path() / "wii_gamecube_kit_test_work";
  fs::create_directories(g_dir);

  if (wii_gamecube_kit_context_create(&g_ctx) != WII_GAMECUBE_KIT_OK)
    return 1;
  wii_gamecube_kit_context_set_log(g_ctx, OnLog, nullptr);
  wii_gamecube_kit_context_set_progress(g_ctx, OnProgress, nullptr);

  const std::string gc_iso = P("synthetic-gc.iso");
  const std::string wii_iso = P("synthetic-wii.iso");
  if (!synth::WriteFile(gc_iso, synth::MakeGameCubeDisc(0x800000)) ||
      !synth::WriteFile(wii_iso, synth::MakeWiiDisc(4, true)))
  {
    std::fprintf(stderr, "could not write the synthetic discs into %s\n", g_dir.string().c_str());
    return 1;
  }

  std::printf("%s\n", wii_gamecube_kit_version());
  TestBasics();
  TestProbe(gc_iso, wii_iso);
  TestRoundTrips(gc_iso, wii_iso);
  TestRvzPacksJunk(gc_iso);
  TestDeterministic(wii_iso);
  TestValidation(gc_iso);
  TestCancel(wii_iso);
  TestScrub(gc_iso);
  TestHashOptions(gc_iso, wii_iso);
  TestUnicodePaths(gc_iso);

  // A Wii disc with every hash correct converts too (no hash exceptions).
  const std::string clean = P("synthetic-wii-clean.iso");
  synth::WriteFile(clean, synth::MakeWiiDisc(3, false));
  RoundTrip(clean, "wii-clean-rvz", WII_GAMECUBE_KIT_FORMAT_RVZ, WII_GAMECUBE_KIT_COMPRESSION_ZSTD, 5, 0x20000);

  wii_gamecube_kit_context_destroy(g_ctx);
  std::printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
