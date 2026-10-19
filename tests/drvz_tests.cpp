// SPDX-License-Identifier: GPL-2.0-or-later

// libdolphinrvz tests, run by ctest. Everything goes through the public C API;
// Dolphin's internals are only used to build the synthetic input discs.
//
//   drvz_tests <work dir>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "dolphinrvz.h"
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
    const drvz_status _s = (expr);                                                         \
    if (_s != (expected))                                                                  \
    {                                                                                      \
      ++g_failures;                                                                        \
      std::fprintf(stderr, "%s:%d: %s returned %d (%s), expected %d. last error: %s\n",    \
                   __FILE__, __LINE__, #expr, static_cast<int>(_s), drvz_status_string(_s), \
                   static_cast<int>(expected), drvz_last_error(g_ctx));                    \
    }                                                                                      \
  } while (0)

drvz_context* g_ctx = nullptr;
fs::path g_dir;

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

void DRVZ_CALL OnLog(void*, drvz_log_level level, const char* message)
{
  g_log.push_back(std::to_string(level) + ": " + message);
}

int DRVZ_CALL OnProgress(void*, drvz_phase, double fraction, double ratio)
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

drvz_hash_result Hash(const std::string& path)
{
  drvz_hash_result r;
  drvz_hash_result_init(&r);
  CHECK_STATUS(drvz_hash(g_ctx, path.c_str(), &r), DRVZ_OK);
  return r;
}

drvz_status Convert(const std::string& in, const std::string& out, drvz_format format,
                    drvz_compression compression, int level, uint32_t block_size,
                    bool overwrite = true, bool scrub = false)
{
  drvz_convert_options o;
  drvz_convert_options_init(&o);
  o.input = in.c_str();
  o.output = out.c_str();
  o.format = format;
  o.compression = compression;
  o.compression_level = level;
  o.block_size = block_size;
  o.overwrite = overwrite;
  o.scrub = scrub;
  return drvz_convert(g_ctx, &o);
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
    if (e.path().extension() == ".drvz-tmp")
      return false;
  return true;
}

void TestBasics()
{
  CHECK(drvz_abi_version() == DRVZ_ABI_VERSION);
  CHECK(std::strstr(drvz_version(), "libdolphinrvz") != nullptr);
  CHECK(std::strcmp(drvz_status_string(DRVZ_OK), "Success") == 0);
  CHECK(std::strcmp(drvz_status_string(static_cast<drvz_status>(1234)), "Unknown status") == 0);

  drvz_convert_options o;
  drvz_convert_options_init(&o);
  CHECK(o.struct_size == sizeof(o));
  CHECK(o.format == DRVZ_FORMAT_RVZ && o.compression == DRVZ_COMPRESSION_ZSTD);
  CHECK(o.compression_level == 5 && o.block_size == 0x20000 && !o.scrub && !o.overwrite);

  // Block sizes (DiscUtils.h): RVZ 32 KiB..2 MiB powers of two, then multiples of 2 MiB.
  CHECK(drvz_block_size_valid(DRVZ_FORMAT_RVZ, 0x8000));
  CHECK(drvz_block_size_valid(DRVZ_FORMAT_RVZ, 0x20000));
  CHECK(!drvz_block_size_valid(DRVZ_FORMAT_RVZ, 0x4000));
  CHECK(!drvz_block_size_valid(DRVZ_FORMAT_RVZ, 0x30000));
  CHECK(drvz_block_size_valid(DRVZ_FORMAT_RVZ, 0x600000));
  CHECK(!drvz_block_size_valid(DRVZ_FORMAT_WIA, 0x20000));
  CHECK(drvz_block_size_valid(DRVZ_FORMAT_WIA, 0x200000));
  CHECK(drvz_block_size_valid(DRVZ_FORMAT_GCZ, 0x4000));
  CHECK(!drvz_block_size_valid(DRVZ_FORMAT_GCZ, 0x3000));
  CHECK(drvz_block_size_valid(DRVZ_FORMAT_ISO, 0));
  CHECK(!drvz_block_size_valid(DRVZ_FORMAT_RVZ, 0));

  int min = -1, max = -1;
  drvz_compression_levels(DRVZ_COMPRESSION_ZSTD, &min, &max);
  CHECK(min < 0 && max == 22);
  drvz_compression_levels(DRVZ_COMPRESSION_LZMA2, &min, &max);
  CHECK(min == 1 && max == 9);
  drvz_compression_levels(DRVZ_COMPRESSION_NONE, &min, &max);
  CHECK(min == 0 && max == 0);
}

void TestProbe(const std::string& gc_iso, const std::string& wii_iso)
{
  drvz_disc_info info;
  drvz_disc_info_init(&info);
  CHECK_STATUS(drvz_probe(g_ctx, gc_iso.c_str(), &info), DRVZ_OK);
  CHECK(info.container == DRVZ_CONTAINER_PLAIN);
  CHECK(info.platform == DRVZ_PLATFORM_GAMECUBE);
  CHECK(std::strcmp(info.game_id, "GTSE01") == 0);
  CHECK(std::strcmp(info.maker_id, "01") == 0);
  CHECK(std::strcmp(info.internal_name, "libdolphinrvz GameCube test disc") == 0);
  CHECK(info.region == DRVZ_REGION_NTSC_U);
  CHECK(info.data_size == fs::file_size(fs::u8path(gc_iso)) && info.data_size_is_exact);
  CHECK(!info.is_nkit && !info.is_datel && info.is_scrubbable);

  drvz_disc_info_init(&info);
  CHECK_STATUS(drvz_probe(g_ctx, wii_iso.c_str(), &info), DRVZ_OK);
  CHECK(info.platform == DRVZ_PLATFORM_WII);
  CHECK(std::strcmp(info.game_id, "RTSP01") == 0);

  // Older/smaller struct sizes are refused; a bigger one is accepted and written partly.
  drvz_disc_info_init(&info);
  info.struct_size = sizeof(size_t);
  CHECK_STATUS(drvz_probe(g_ctx, gc_iso.c_str(), &info), DRVZ_ERR_ABI_MISMATCH);

  // Not a disc.
  const std::string junk = P("not-a-disc.iso");
  synth::WriteFile(junk, std::vector<u8>(0x10000, 0xAB));
  drvz_disc_info_init(&info);
  CHECK_STATUS(drvz_probe(g_ctx, junk.c_str(), &info), DRVZ_ERR_NOT_A_DISC);
  CHECK(std::strstr(drvz_last_error(g_ctx), "not a GameCube or Wii") != nullptr);
  CHECK_STATUS(drvz_probe(g_ctx, P("missing.iso").c_str(), &info), DRVZ_ERR_FILE_OPEN);
}

// ISO -> format -> ISO must give back the identical image, and the container's
// hashes must match the ISO's.
void RoundTrip(const std::string& iso, const char* tag, drvz_format format,
               drvz_compression compression, int level, uint32_t block_size)
{
  const std::string packed = P((std::string(tag) + ".packed").c_str());
  const std::string back = P((std::string(tag) + ".back.iso").c_str());
  ResetObservations();
  CHECK_STATUS(Convert(iso, packed, format, compression, level, block_size), DRVZ_OK);
  CHECK(!g_fractions.empty() && g_fractions.back() == 1.0);
  CHECK(fs::exists(fs::u8path(packed)));

  drvz_disc_info info;
  drvz_disc_info_init(&info);
  CHECK_STATUS(drvz_probe(g_ctx, packed.c_str(), &info), DRVZ_OK);
  const drvz_container expected = format == DRVZ_FORMAT_RVZ ? DRVZ_CONTAINER_RVZ :
                                  format == DRVZ_FORMAT_WIA ? DRVZ_CONTAINER_WIA :
                                  format == DRVZ_FORMAT_GCZ ? DRVZ_CONTAINER_GCZ :
                                                              DRVZ_CONTAINER_PLAIN;
  CHECK(info.container == expected);
  if (format != DRVZ_FORMAT_ISO)
    CHECK(info.block_size == block_size);

  const drvz_hash_result a = Hash(iso);
  const drvz_hash_result b = Hash(packed);
  CHECK(std::strcmp(a.sha1, b.sha1) == 0 && std::strcmp(a.md5, b.md5) == 0 &&
        std::strcmp(a.crc32, b.crc32) == 0 && a.size == b.size);

  CHECK_STATUS(Convert(packed, back, DRVZ_FORMAT_ISO, DRVZ_COMPRESSION_NONE, 0, 0), DRVZ_OK);
  CHECK(SameFile(iso, back));
  std::printf("  %-28s %9llu -> %9llu bytes\n", tag,
              static_cast<unsigned long long>(fs::file_size(fs::u8path(iso))),
              static_cast<unsigned long long>(fs::file_size(fs::u8path(packed))));
  fs::remove(fs::u8path(packed));
  fs::remove(fs::u8path(back));
}

void TestRoundTrips(const std::string& gc_iso, const std::string& wii_iso)
{
  for (const auto& [iso, name] : {std::pair{gc_iso, "gc"}, std::pair{wii_iso, "wii"}})
  {
    const std::string t = name;
    RoundTrip(iso, (t + "-rvz-zstd5-128k").c_str(), DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_ZSTD, 5, 0x20000);
    RoundTrip(iso, (t + "-rvz-zstd19-2m").c_str(), DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_ZSTD, 19, 0x200000);
    RoundTrip(iso, (t + "-rvz-lzma2-32k").c_str(), DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_LZMA2, 6, 0x8000);
    RoundTrip(iso, (t + "-rvz-none").c_str(), DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_NONE, 0, 0x20000);
    RoundTrip(iso, (t + "-wia-lzma").c_str(), DRVZ_FORMAT_WIA, DRVZ_COMPRESSION_LZMA, 5, 0x200000);
    RoundTrip(iso, (t + "-wia-bzip2").c_str(), DRVZ_FORMAT_WIA, DRVZ_COMPRESSION_BZIP2, 9, 0x200000);
    RoundTrip(iso, (t + "-wia-purge").c_str(), DRVZ_FORMAT_WIA, DRVZ_COMPRESSION_PURGE, 0, 0x200000);
    RoundTrip(iso, (t + "-gcz-32k").c_str(), DRVZ_FORMAT_GCZ, DRVZ_COMPRESSION_NONE, 0, 0x8000);
  }
}

void TestRvzPacksJunk(const std::string& gc_iso)
{
  // Junk is a lagged Fibonacci stream, which no compressor can shrink; RVZ stores
  // its seeds instead. Without that, most of this disc would stay incompressible.
  const std::string rvz = P("packing.rvz");
  ResetObservations();
  CHECK_STATUS(Convert(gc_iso, rvz, DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_ZSTD, 5, 0x20000), DRVZ_OK);
  const auto iso_size = fs::file_size(fs::u8path(gc_iso));
  const auto rvz_size = fs::file_size(fs::u8path(rvz));
  CHECK(rvz_size * 10 < iso_size);
  CHECK(g_last_ratio > 0.0 && g_last_ratio < 0.2);
  fs::remove(fs::u8path(rvz));
}

void TestDeterministic(const std::string& wii_iso)
{
  // Dolphin writes groups in order, so the output must not depend on the thread count.
  const std::string a = P("threads1.rvz");
  const std::string b = P("threads7.rvz");
  drvz_context_set_num_threads(g_ctx, 1);
  CHECK_STATUS(Convert(wii_iso, a, DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_ZSTD, 3, 0x20000), DRVZ_OK);
  drvz_context_set_num_threads(g_ctx, 7);
  CHECK_STATUS(Convert(wii_iso, b, DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_ZSTD, 3, 0x20000), DRVZ_OK);
  drvz_context_set_num_threads(g_ctx, 0);
  CHECK(SameFile(a, b));
  fs::remove(fs::u8path(a));
  fs::remove(fs::u8path(b));
}

void TestValidation(const std::string& gc_iso)
{
  const std::string out = P("validation.rvz");
  fs::remove(fs::u8path(out));
  CHECK_STATUS(Convert(gc_iso, out, DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_ZSTD, 5, 0x30000),
               DRVZ_ERR_INVALID_ARGUMENT);
  CHECK_STATUS(Convert(gc_iso, out, DRVZ_FORMAT_WIA, DRVZ_COMPRESSION_ZSTD, 5, 0x200000),
               DRVZ_ERR_INVALID_ARGUMENT);
  CHECK_STATUS(Convert(gc_iso, out, DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_PURGE, 0, 0x20000),
               DRVZ_ERR_INVALID_ARGUMENT);
  CHECK_STATUS(Convert(gc_iso, out, DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_ZSTD, 23, 0x20000),
               DRVZ_ERR_INVALID_ARGUMENT);
  CHECK_STATUS(Convert(gc_iso, out, DRVZ_FORMAT_WIA, DRVZ_COMPRESSION_LZMA2, 0, 0x200000),
               DRVZ_ERR_INVALID_ARGUMENT);
  CHECK_STATUS(Convert(gc_iso, gc_iso, DRVZ_FORMAT_ISO, DRVZ_COMPRESSION_NONE, 0, 0),
               DRVZ_ERR_INVALID_ARGUMENT);
  CHECK_STATUS(Convert(P("missing.iso"), out, DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_ZSTD, 5, 0x20000),
               DRVZ_ERR_FILE_OPEN);
  CHECK(!fs::exists(fs::u8path(out)));

  drvz_convert_options o;
  drvz_convert_options_init(&o);
  CHECK_STATUS(drvz_convert(g_ctx, &o), DRVZ_ERR_INVALID_ARGUMENT);  // no paths
  o.struct_size = 8;
  CHECK_STATUS(drvz_convert(g_ctx, &o), DRVZ_ERR_ABI_MISMATCH);

  // Existing output: refused unless overwrite, and replaced whole when allowed.
  synth::WriteFile(out, std::vector<u8>(100, 1));
  CHECK_STATUS(Convert(gc_iso, out, DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_ZSTD, 5, 0x20000, false),
               DRVZ_ERR_FILE_EXISTS);
  CHECK(fs::file_size(fs::u8path(out)) == 100);
  CHECK_STATUS(Convert(gc_iso, out, DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_ZSTD, 5, 0x20000, true),
               DRVZ_OK);
  CHECK(fs::file_size(fs::u8path(out)) > 100);
  CHECK(std::strcmp(drvz_last_error(g_ctx), "") == 0);
  fs::remove(fs::u8path(out));

  // Not a disc.
  CHECK_STATUS(Convert(P("not-a-disc.iso"), out, DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_ZSTD, 5, 0x20000),
               DRVZ_ERR_NOT_A_DISC);

  // Warnings DolphinTool prints become log lines.
  ResetObservations();
  CHECK_STATUS(Convert(gc_iso, out, DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_ZSTD, 1, 0x400000), DRVZ_OK);
  bool warned = false;
  for (const std::string& line : g_log)
    warned |= line.find("Block size is not ideal") != std::string::npos;
  CHECK(warned);
  fs::remove(fs::u8path(out));
  CHECK(NoTempFiles());
}

void TestCancel(const std::string& wii_iso)
{
  const std::string out = P("cancelled.rvz");
  fs::remove(fs::u8path(out));

  // From the progress callback, part way through.
  ResetObservations();
  g_progress_hook = [](double fraction) { return fraction > 0.3 ? 1 : 0; };
  drvz_context_set_num_threads(g_ctx, 1);
  CHECK_STATUS(Convert(wii_iso, out, DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_ZSTD, 5, 0x8000),
               DRVZ_ERR_CANCELLED);
  drvz_context_set_num_threads(g_ctx, 0);
  CHECK(!fs::exists(fs::u8path(out)));
  CHECK(NoTempFiles());

  // Requested before the call: fails before any work, and the request is then cleared.
  ResetObservations();
  drvz_context_cancel(g_ctx);
  CHECK_STATUS(Convert(wii_iso, out, DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_ZSTD, 5, 0x20000),
               DRVZ_ERR_CANCELLED);
  CHECK(g_fractions.empty());
  drvz_hash_result r;
  drvz_hash_result_init(&r);
  CHECK_STATUS(drvz_hash(g_ctx, wii_iso.c_str(), &r), DRVZ_OK);

  // Cancelling a hash.
  g_progress_hook = [](double fraction) { return fraction > 0.1 ? 1 : 0; };
  CHECK_STATUS(drvz_hash(g_ctx, wii_iso.c_str(), &r), DRVZ_ERR_CANCELLED);
  ResetObservations();
}

void TestScrub(const std::string& gc_iso)
{
  const std::string out = P("scrubbed.rvz");
  const std::string iso = P("scrubbed.iso");
  CHECK_STATUS(Convert(gc_iso, out, DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_ZSTD, 5, 0x20000, true, true),
               DRVZ_OK);
  CHECK_STATUS(Convert(out, iso, DRVZ_FORMAT_ISO, DRVZ_COMPRESSION_NONE, 0, 0), DRVZ_OK);
  // Scrubbing zeroes the junk, so the image changes but keeps its size and files.
  CHECK(fs::file_size(fs::u8path(iso)) == fs::file_size(fs::u8path(gc_iso)));
  CHECK(!SameFile(iso, gc_iso));
  const std::vector<u8> a = synth::ReadFile(gc_iso);
  const std::vector<u8> b = synth::ReadFile(iso);
  CHECK(a.size() == b.size() && std::equal(a.begin() + 0x100000, a.begin() + 0x280000, b.begin() + 0x100000));
  fs::remove(fs::u8path(out));
  fs::remove(fs::u8path(iso));
}

void TestUnicodePaths(const std::string& gc_iso)
{
  const fs::path dir = g_dir / fs::path(u8"ゲーム テスト");
  fs::create_directories(dir);
  const std::u8string in8 = (dir / fs::path(u8"ディスク.iso")).u8string();
  const std::u8string out8 = (dir / fs::path(u8"ディスク.rvz")).u8string();
  const std::string in(in8.begin(), in8.end());
  const std::string out(out8.begin(), out8.end());
  fs::copy_file(fs::u8path(gc_iso), fs::path(in8), fs::copy_options::overwrite_existing);
  CHECK_STATUS(Convert(in, out, DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_ZSTD, 5, 0x20000), DRVZ_OK);
  drvz_disc_info info;
  drvz_disc_info_init(&info);
  CHECK_STATUS(drvz_probe(g_ctx, out.c_str(), &info), DRVZ_OK);
  CHECK(info.container == DRVZ_CONTAINER_RVZ);
  fs::remove_all(dir);
}
}  // namespace

int main(int argc, char** argv)
{
  g_dir = argc > 1 ? fs::u8path(argv[1]) : fs::current_path() / "drvz_test_work";
  fs::create_directories(g_dir);

  if (drvz_context_create(&g_ctx) != DRVZ_OK)
    return 1;
  drvz_context_set_log(g_ctx, OnLog, nullptr);
  drvz_context_set_progress(g_ctx, OnProgress, nullptr);

  const std::string gc_iso = P("synthetic-gc.iso");
  const std::string wii_iso = P("synthetic-wii.iso");
  if (!synth::WriteFile(gc_iso, synth::MakeGameCubeDisc(0x800000)) ||
      !synth::WriteFile(wii_iso, synth::MakeWiiDisc(4, true)))
  {
    std::fprintf(stderr, "could not write the synthetic discs into %s\n", g_dir.string().c_str());
    return 1;
  }

  std::printf("%s\n", drvz_version());
  TestBasics();
  TestProbe(gc_iso, wii_iso);
  TestRoundTrips(gc_iso, wii_iso);
  TestRvzPacksJunk(gc_iso);
  TestDeterministic(wii_iso);
  TestValidation(gc_iso);
  TestCancel(wii_iso);
  TestScrub(gc_iso);
  TestUnicodePaths(gc_iso);

  // A Wii disc with every hash correct converts too (no hash exceptions).
  const std::string clean = P("synthetic-wii-clean.iso");
  synth::WriteFile(clean, synth::MakeWiiDisc(3, false));
  RoundTrip(clean, "wii-clean-rvz", DRVZ_FORMAT_RVZ, DRVZ_COMPRESSION_ZSTD, 5, 0x20000);

  drvz_context_destroy(g_ctx);
  std::printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
