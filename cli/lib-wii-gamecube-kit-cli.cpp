// SPDX-License-Identifier: GPL-2.0-or-later

// lib-wii-gamecube-kit-cli: a small DolphinTool-style front end over the lib-wii-gamecube-kit C API,
// for trying the library out and for the tests.
//
//   lib-wii-gamecube-kit-cli probe <image>
//   lib-wii-gamecube-kit-cli hash <image> [-k crc32,md5,sha1] [-t threads]
//   lib-wii-gamecube-kit-cli convert -i <in> -o <out> [-f rvz|wia|gcz|iso] [-c none|purge|bzip2|lzma|lzma2|zstd]
//                    [-l level] [-b block_size] [-s] [-y] [-t threads] [-q]
//                    [-k crc32,md5,sha1] [-V]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "lib-wii-gamecube-kit.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
bool g_quiet = false;
int g_last_percent = -1;

void WII_GAMECUBE_KIT_CALL OnLog(void*, wii_gamecube_kit_log_level level, const char* message)
{
  const char* prefix = level == WII_GAMECUBE_KIT_LOG_ERROR ? "error" : level == WII_GAMECUBE_KIT_LOG_WARNING ? "warning" : "info";
  std::fprintf(stderr, "%s: %s\n", prefix, message);
}

int WII_GAMECUBE_KIT_CALL OnProgress(void*, wii_gamecube_kit_phase phase, double fraction, double ratio)
{
  if (g_quiet)
    return 0;
  const int percent = static_cast<int>(fraction * 100);
  if (percent != g_last_percent)
  {
    g_last_percent = percent;
    if (ratio > 0)
      std::fprintf(stderr, "\r%s %3d%%  ratio %3d%%", phase == WII_GAMECUBE_KIT_PHASE_HASHING ? "hashing" : "converting",
                   percent, static_cast<int>(ratio * 100));
    else
      std::fprintf(stderr, "\r%s %3d%%", phase == WII_GAMECUBE_KIT_PHASE_HASHING ? "hashing" : "converting", percent);
    if (percent >= 100)
      std::fprintf(stderr, "\n");
  }
  return 0;
}

int Usage()
{
  std::fprintf(stderr,
               "%s\n"
               "usage: lib-wii-gamecube-kit-cli probe <image>\n"
               "       lib-wii-gamecube-kit-cli hash <image> [-k crc32,md5,sha1] [-t threads]\n"
               "       lib-wii-gamecube-kit-cli convert -i <in> -o <out> [-f rvz|wia|gcz|iso]\n"
               "                [-c none|purge|bzip2|lzma|lzma2|zstd] [-l level] [-b block_size]\n"
               "                [-s scrub] [-y overwrite] [-t threads] [-q quiet]\n"
               "                [-k crc32,md5,sha1: hash the source while converting]\n"
               "                [-V verify: SHA-1 of the output must match the source's]\n",
               wii_gamecube_kit_version());
  return 2;
}

// "crc32,md5,sha1" -> WII_GAMECUBE_KIT_HASH_* bits; 0 if something isn't a hash name.
unsigned ParseHashes(const std::string& list)
{
  unsigned bits = 0;
  size_t start = 0;
  while (start <= list.size())
  {
    const size_t comma = list.find(',', start);
    const std::string name = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (name == "crc32")
      bits |= WII_GAMECUBE_KIT_HASH_CRC32;
    else if (name == "md5")
      bits |= WII_GAMECUBE_KIT_HASH_MD5;
    else if (name == "sha1")
      bits |= WII_GAMECUBE_KIT_HASH_SHA1;
    else if (name == "all")
      bits |= WII_GAMECUBE_KIT_HASH_ALL;
    else
      return 0;
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  return bits;
}

void PrintHashes(const char* title, const wii_gamecube_kit_hash_result& r)
{
  if (title != nullptr)
    std::printf("%s\n", title);
  if (r.crc32[0])
    std::printf("crc32 %s\n", r.crc32);
  if (r.md5[0])
    std::printf("md5   %s\n", r.md5);
  if (r.sha1[0])
    std::printf("sha1  %s\n", r.sha1);
  std::printf("size  %llu%s\n", static_cast<unsigned long long>(r.size), r.size_is_exact ? "" : " (upper bound)");
}

const char* PlatformName(wii_gamecube_kit_platform p)
{
  switch (p)
  {
  case WII_GAMECUBE_KIT_PLATFORM_GAMECUBE:
    return "GameCube";
  case WII_GAMECUBE_KIT_PLATFORM_WII:
    return "Wii";
  case WII_GAMECUBE_KIT_PLATFORM_TRIFORCE:
    return "Triforce";
  }
  return "?";
}

const char* ContainerName(wii_gamecube_kit_container c)
{
  switch (c)
  {
  case WII_GAMECUBE_KIT_CONTAINER_PLAIN:
    return "ISO";
  case WII_GAMECUBE_KIT_CONTAINER_GCZ:
    return "GCZ";
  case WII_GAMECUBE_KIT_CONTAINER_CISO:
    return "CISO";
  case WII_GAMECUBE_KIT_CONTAINER_WBFS:
    return "WBFS";
  case WII_GAMECUBE_KIT_CONTAINER_TGC:
    return "TGC";
  case WII_GAMECUBE_KIT_CONTAINER_WIA:
    return "WIA";
  case WII_GAMECUBE_KIT_CONTAINER_RVZ:
    return "RVZ";
  case WII_GAMECUBE_KIT_CONTAINER_NFS:
    return "NFS";
  default:
    return "other";
  }
}

int Fail(wii_gamecube_kit_context* ctx, wii_gamecube_kit_status status)
{
  std::fprintf(stderr, "error: %s (%s)\n", wii_gamecube_kit_last_error(ctx), wii_gamecube_kit_status_string(status));
  wii_gamecube_kit_context_destroy(ctx);
  return 1;
}

int Run(int argc, char** argv)
{
  if (argc < 3)
    return Usage();
  const std::string command = argv[1];

  wii_gamecube_kit_context* ctx = nullptr;
  if (wii_gamecube_kit_context_create(&ctx) != WII_GAMECUBE_KIT_OK)
    return 1;
  wii_gamecube_kit_context_set_log(ctx, OnLog, nullptr);
  wii_gamecube_kit_context_set_progress(ctx, OnProgress, nullptr);

  if (command == "probe")
  {
    wii_gamecube_kit_disc_info info;
    wii_gamecube_kit_disc_info_init(&info);
    const wii_gamecube_kit_status status = wii_gamecube_kit_probe(ctx, argv[2], &info);
    if (status != WII_GAMECUBE_KIT_OK)
      return Fail(ctx, status);
    std::printf("container:   %s\n", ContainerName(info.container));
    std::printf("platform:    %s\n", PlatformName(info.platform));
    std::printf("game id:     %s\n", info.game_id);
    std::printf("maker id:    %s\n", info.maker_id);
    std::printf("name:        %s\n", info.internal_name);
    std::printf("disc:        %d\n", info.disc_number + 1);
    std::printf("revision:    %d\n", info.revision);
    std::printf("region:      %d\n", static_cast<int>(info.region));
    std::printf("data size:   %llu%s\n", static_cast<unsigned long long>(info.data_size),
                info.data_size_is_exact ? "" : " (upper bound)");
    std::printf("file size:   %llu\n", static_cast<unsigned long long>(info.file_size));
    std::printf("block size:  %u\n", info.block_size);
    std::printf("compression: %s", info.compression_method[0] ? info.compression_method : "none");
    if (info.has_compression_level)
      std::printf(" (level %d)", info.compression_level);
    std::printf("\nnkit:        %s\ndatel:       %s\n", info.is_nkit ? "yes" : "no",
                info.is_datel ? "yes" : "no");
  }
  else if (command == "hash")
  {
    wii_gamecube_kit_hash_options opts;
    wii_gamecube_kit_hash_options_init(&opts);
    for (int i = 3; i < argc; ++i)
    {
      const std::string arg = argv[i];
      if (i + 1 >= argc)
        return Usage();
      if (arg == "-k" && (opts.hashes = ParseHashes(argv[++i])) != 0)
        continue;
      if (arg == "-t")
      {
        wii_gamecube_kit_context_set_num_threads(ctx, std::atoi(argv[++i]));
        continue;
      }
      return Usage();
    }
    wii_gamecube_kit_hash_result result;
    wii_gamecube_kit_hash_result_init(&result);
    const wii_gamecube_kit_status status = wii_gamecube_kit_hash_ex(ctx, argv[2], &opts, &result);
    if (status != WII_GAMECUBE_KIT_OK)
      return Fail(ctx, status);
    PrintHashes(nullptr, result);
  }
  else if (command == "convert")
  {
    wii_gamecube_kit_convert_options opts;
    wii_gamecube_kit_convert_options_init(&opts);
    wii_gamecube_kit_hash_result source;
    wii_gamecube_kit_hash_result_init(&source);
    bool verify = false;
    for (int i = 2; i < argc; ++i)
    {
      const std::string arg = argv[i];
      auto value = [&]() -> const char* {
        if (i + 1 >= argc)
        {
          std::fprintf(stderr, "error: %s needs a value\n", arg.c_str());
          std::exit(2);
        }
        return argv[++i];
      };
      if (arg == "-i")
        opts.input = value();
      else if (arg == "-o")
        opts.output = value();
      else if (arg == "-f")
      {
        const std::string f = value();
        if (f == "rvz")
          opts.format = WII_GAMECUBE_KIT_FORMAT_RVZ;
        else if (f == "wia")
          opts.format = WII_GAMECUBE_KIT_FORMAT_WIA;
        else if (f == "gcz")
          opts.format = WII_GAMECUBE_KIT_FORMAT_GCZ;
        else if (f == "iso")
          opts.format = WII_GAMECUBE_KIT_FORMAT_ISO;
        else
          return Usage();
        if (opts.format == WII_GAMECUBE_KIT_FORMAT_ISO)
          opts.block_size = 0;
        if (opts.format == WII_GAMECUBE_KIT_FORMAT_WIA && opts.compression == WII_GAMECUBE_KIT_COMPRESSION_ZSTD)
        {
          opts.compression = WII_GAMECUBE_KIT_COMPRESSION_LZMA2;
          opts.block_size = 0x200000;
        }
        if (opts.format == WII_GAMECUBE_KIT_FORMAT_GCZ)
          opts.block_size = 0x8000;
      }
      else if (arg == "-c")
      {
        const std::string c = value();
        if (c == "none")
          opts.compression = WII_GAMECUBE_KIT_COMPRESSION_NONE;
        else if (c == "purge")
          opts.compression = WII_GAMECUBE_KIT_COMPRESSION_PURGE;
        else if (c == "bzip2")
          opts.compression = WII_GAMECUBE_KIT_COMPRESSION_BZIP2;
        else if (c == "lzma")
          opts.compression = WII_GAMECUBE_KIT_COMPRESSION_LZMA;
        else if (c == "lzma2")
          opts.compression = WII_GAMECUBE_KIT_COMPRESSION_LZMA2;
        else if (c == "zstd")
          opts.compression = WII_GAMECUBE_KIT_COMPRESSION_ZSTD;
        else
          return Usage();
      }
      else if (arg == "-l")
        opts.compression_level = std::atoi(value());
      else if (arg == "-b")
        opts.block_size = static_cast<uint32_t>(std::strtoul(value(), nullptr, 0));
      else if (arg == "-s")
        opts.scrub = 1;
      else if (arg == "-y")
        opts.overwrite = 1;
      else if (arg == "-t")
        wii_gamecube_kit_context_set_num_threads(ctx, std::atoi(value()));
      else if (arg == "-q")
        g_quiet = true;
      else if (arg == "-k")
      {
        const unsigned kinds = ParseHashes(value());
        if (kinds == 0)
          return Usage();
        opts.source_hashes |= kinds;
      }
      else if (arg == "-V")
        verify = true;
      else
        return Usage();
    }
    if (verify)
      opts.source_hashes |= WII_GAMECUBE_KIT_HASH_SHA1;
    if (opts.source_hashes != 0)
      opts.source_hash = &source;
    const wii_gamecube_kit_status status = wii_gamecube_kit_convert(ctx, &opts);
    if (status != WII_GAMECUBE_KIT_OK)
      return Fail(ctx, status);
    if (opts.source_hashes != 0)
      PrintHashes("source:", source);
    if (verify)
    {
      wii_gamecube_kit_hash_options hash_opts;
      wii_gamecube_kit_hash_options_init(&hash_opts);
      hash_opts.hashes = WII_GAMECUBE_KIT_HASH_SHA1;
      wii_gamecube_kit_hash_result made;
      wii_gamecube_kit_hash_result_init(&made);
      const wii_gamecube_kit_status hashed = wii_gamecube_kit_hash_ex(ctx, opts.output, &hash_opts, &made);
      if (hashed != WII_GAMECUBE_KIT_OK)
        return Fail(ctx, hashed);
      if (std::strcmp(made.sha1, source.sha1) != 0 || made.size != source.size)
      {
        std::fprintf(stderr, "error: verification failed: output SHA-1 %s, source %s\n", made.sha1, source.sha1);
        wii_gamecube_kit_context_destroy(ctx);
        return 1;
      }
      std::printf("verified\n");
    }
  }
  else
  {
    wii_gamecube_kit_context_destroy(ctx);
    return Usage();
  }
  wii_gamecube_kit_context_destroy(ctx);
  return 0;
}
}  // namespace

#ifdef _WIN32
// Windows: take the command line as UTF-16 and pass UTF-8 on, as the API expects.
int wmain(int argc, wchar_t** wargv)
{
  std::string storage[64];
  char* argv[64];
  if (argc > 64)
    argc = 64;
  for (int i = 0; i < argc; ++i)
  {
    const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
    storage[i].resize(n > 0 ? n - 1 : 0);
    if (n > 1)
      WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, storage[i].data(), n, nullptr, nullptr);
    argv[i] = storage[i].data();
  }
  SetConsoleOutputCP(CP_UTF8);
  return Run(argc, argv);
}
#else
int main(int argc, char** argv)
{
  return Run(argc, argv);
}
#endif
