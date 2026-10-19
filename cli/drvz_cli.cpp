// SPDX-License-Identifier: GPL-2.0-or-later

// drvz-cli: a small DolphinTool-style front end over the libdolphinrvz C API,
// for trying the library out and for the tests.
//
//   drvz-cli probe <image>
//   drvz-cli hash <image>
//   drvz-cli convert -i <in> -o <out> [-f rvz|wia|gcz|iso] [-c none|purge|bzip2|lzma|lzma2|zstd]
//                    [-l level] [-b block_size] [-s] [-y] [-t threads] [-q]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "dolphinrvz.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
bool g_quiet = false;
int g_last_percent = -1;

void DRVZ_CALL OnLog(void*, drvz_log_level level, const char* message)
{
  const char* prefix = level == DRVZ_LOG_ERROR ? "error" : level == DRVZ_LOG_WARNING ? "warning" : "info";
  std::fprintf(stderr, "%s: %s\n", prefix, message);
}

int DRVZ_CALL OnProgress(void*, drvz_phase phase, double fraction, double ratio)
{
  if (g_quiet)
    return 0;
  const int percent = static_cast<int>(fraction * 100);
  if (percent != g_last_percent)
  {
    g_last_percent = percent;
    if (ratio > 0)
      std::fprintf(stderr, "\r%s %3d%%  ratio %3d%%", phase == DRVZ_PHASE_HASHING ? "hashing" : "converting",
                   percent, static_cast<int>(ratio * 100));
    else
      std::fprintf(stderr, "\r%s %3d%%", phase == DRVZ_PHASE_HASHING ? "hashing" : "converting", percent);
    if (percent >= 100)
      std::fprintf(stderr, "\n");
  }
  return 0;
}

int Usage()
{
  std::fprintf(stderr,
               "%s\n"
               "usage: drvz-cli probe <image>\n"
               "       drvz-cli hash <image>\n"
               "       drvz-cli convert -i <in> -o <out> [-f rvz|wia|gcz|iso]\n"
               "                [-c none|purge|bzip2|lzma|lzma2|zstd] [-l level] [-b block_size]\n"
               "                [-s scrub] [-y overwrite] [-t threads] [-q quiet]\n",
               drvz_version());
  return 2;
}

const char* PlatformName(drvz_platform p)
{
  switch (p)
  {
  case DRVZ_PLATFORM_GAMECUBE:
    return "GameCube";
  case DRVZ_PLATFORM_WII:
    return "Wii";
  case DRVZ_PLATFORM_TRIFORCE:
    return "Triforce";
  }
  return "?";
}

const char* ContainerName(drvz_container c)
{
  switch (c)
  {
  case DRVZ_CONTAINER_PLAIN:
    return "ISO";
  case DRVZ_CONTAINER_GCZ:
    return "GCZ";
  case DRVZ_CONTAINER_CISO:
    return "CISO";
  case DRVZ_CONTAINER_WBFS:
    return "WBFS";
  case DRVZ_CONTAINER_TGC:
    return "TGC";
  case DRVZ_CONTAINER_WIA:
    return "WIA";
  case DRVZ_CONTAINER_RVZ:
    return "RVZ";
  case DRVZ_CONTAINER_NFS:
    return "NFS";
  default:
    return "other";
  }
}

int Fail(drvz_context* ctx, drvz_status status)
{
  std::fprintf(stderr, "error: %s (%s)\n", drvz_last_error(ctx), drvz_status_string(status));
  drvz_context_destroy(ctx);
  return 1;
}

int Run(int argc, char** argv)
{
  if (argc < 3)
    return Usage();
  const std::string command = argv[1];

  drvz_context* ctx = nullptr;
  if (drvz_context_create(&ctx) != DRVZ_OK)
    return 1;
  drvz_context_set_log(ctx, OnLog, nullptr);
  drvz_context_set_progress(ctx, OnProgress, nullptr);

  if (command == "probe")
  {
    drvz_disc_info info;
    drvz_disc_info_init(&info);
    const drvz_status status = drvz_probe(ctx, argv[2], &info);
    if (status != DRVZ_OK)
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
    drvz_hash_result result;
    drvz_hash_result_init(&result);
    const drvz_status status = drvz_hash(ctx, argv[2], &result);
    if (status != DRVZ_OK)
      return Fail(ctx, status);
    std::printf("crc32 %s\nmd5   %s\nsha1  %s\nsize  %llu%s\n", result.crc32, result.md5,
                result.sha1, static_cast<unsigned long long>(result.size),
                result.size_is_exact ? "" : " (upper bound)");
  }
  else if (command == "convert")
  {
    drvz_convert_options opts;
    drvz_convert_options_init(&opts);
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
          opts.format = DRVZ_FORMAT_RVZ;
        else if (f == "wia")
          opts.format = DRVZ_FORMAT_WIA;
        else if (f == "gcz")
          opts.format = DRVZ_FORMAT_GCZ;
        else if (f == "iso")
          opts.format = DRVZ_FORMAT_ISO;
        else
          return Usage();
        if (opts.format == DRVZ_FORMAT_ISO)
          opts.block_size = 0;
        if (opts.format == DRVZ_FORMAT_WIA && opts.compression == DRVZ_COMPRESSION_ZSTD)
        {
          opts.compression = DRVZ_COMPRESSION_LZMA2;
          opts.block_size = 0x200000;
        }
        if (opts.format == DRVZ_FORMAT_GCZ)
          opts.block_size = 0x8000;
      }
      else if (arg == "-c")
      {
        const std::string c = value();
        if (c == "none")
          opts.compression = DRVZ_COMPRESSION_NONE;
        else if (c == "purge")
          opts.compression = DRVZ_COMPRESSION_PURGE;
        else if (c == "bzip2")
          opts.compression = DRVZ_COMPRESSION_BZIP2;
        else if (c == "lzma")
          opts.compression = DRVZ_COMPRESSION_LZMA;
        else if (c == "lzma2")
          opts.compression = DRVZ_COMPRESSION_LZMA2;
        else if (c == "zstd")
          opts.compression = DRVZ_COMPRESSION_ZSTD;
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
        drvz_context_set_num_threads(ctx, std::atoi(value()));
      else if (arg == "-q")
        g_quiet = true;
      else
        return Usage();
    }
    const drvz_status status = drvz_convert(ctx, &opts);
    if (status != DRVZ_OK)
      return Fail(ctx, status);
  }
  else
  {
    drvz_context_destroy(ctx);
    return Usage();
  }
  drvz_context_destroy(ctx);
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
