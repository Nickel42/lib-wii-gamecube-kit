// SPDX-License-Identifier: GPL-2.0-or-later

// lib-wii-gamecube-kit C ABI: DolphinTool's convert command and Dolphin's disc probing
// as library calls. The checks in wii_gamecube_kit_convert follow DolphinTool's
// ConvertCommand.cpp; differences are noted where they occur.

#include "lib-wii-gamecube-kit.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"
#include "DiscIO/Blob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/ScrubbedBlob.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/WIABlob.h"
#include "lib-wii-gamecube-kit-hashing.h"
#include "lib-wii-gamecube-kit-hooks.h"
#include "lib-wii-gamecube-kit-internal.h"

struct wii_gamecube_kit_context
{
  wii_gamecube_kit_progress_fn progress = nullptr;
  void* progress_user = nullptr;
  wii_gamecube_kit_log_fn log = nullptr;
  void* log_user = nullptr;
  int num_threads = 0;
  std::atomic<bool> cancel{false};
  std::string last_error;
  std::vector<std::string> errors;
};

namespace
{
thread_local wii_gamecube_kit_context* t_ctx = nullptr;

bool AlertHandler(const char* caption, const char* text, bool yes_no, Common::MsgType style)
{
  (void)caption;
  (void)yes_no;
  (void)style;
  // ShowMessageAlert has already logged the text. Keep it for wii_gamecube_kit_last_error, and answer
  // "no" to any question (Dolphin's own behaviour when it has no UI).
  wii_gamecube_kit::AddError(text);
  return false;
}

void InstallHandlers()
{
  static std::once_flag once;
  std::call_once(once, [] {
    // Dolphin's default handler shows a message box on Windows.
    Common::RegisterMsgAlertHandler(AlertHandler);
  });
}

// Marks ctx as running an operation on this thread for as long as it lives.
class Operation
{
public:
  explicit Operation(wii_gamecube_kit_context* ctx) : m_ctx(ctx), m_previous(t_ctx)
  {
    t_ctx = ctx;
    ctx->last_error.clear();
    ctx->errors.clear();
  }
  ~Operation()
  {
    m_ctx->cancel.store(false);
    t_ctx = m_previous;
  }
  Operation(const Operation&) = delete;
  Operation& operator=(const Operation&) = delete;

  // Sets the context's error message and returns status.
  wii_gamecube_kit_status Fail(wii_gamecube_kit_status status, const std::string& message)
  {
    std::string text = message;
    for (const std::string& line : m_ctx->errors)
    {
      if (line == message)
        continue;
      text += text.empty() ? line : ": " + line;
    }
    m_ctx->last_error = text.empty() ? wii_gamecube_kit_status_string(status) : text;
    return status;
  }

  bool Cancelled() const { return m_ctx->cancel.load(); }

private:
  wii_gamecube_kit_context* m_ctx;
  wii_gamecube_kit_context* m_previous;
};

void Warn(const std::string& message)
{
  wii_gamecube_kit::Log(WII_GAMECUBE_KIT_LOG_WARNING, message);
}

// Copies src into a fixed-size char array, cutting at a UTF-8 character boundary.
template <size_t N>
void CopyString(char (&dst)[N], const std::string& src)
{
  size_t len = std::min(src.size(), N - 1);
  while (len > 0 && len < src.size() && (static_cast<unsigned char>(src[len]) & 0xC0) == 0x80)
    --len;
  std::memcpy(dst, src.data(), len);
  dst[len] = '\0';
}

// wii_gamecube_kit_convert_options as ABI 1 (0.1.0) defined it, before fields were appended.
constexpr size_t CONVERT_OPTIONS_ABI1_SIZE =
    offsetof(wii_gamecube_kit_convert_options, source_hashes);

// Reads a caller's options struct: at least min_size (the ABI 1 size), newer fields
// beyond the caller's struct_size keep the defaults init gives them.
template <typename T, typename Init>
bool ReadStruct(const T* in, T* out, size_t min_size, Init init)
{
  if (in->struct_size < min_size)
    return false;
  init(out);
  std::memcpy(out, in, std::min(in->struct_size, sizeof(T)));
  out->struct_size = sizeof(T);
  return true;
}

// Writes a result struct up to the caller's struct_size.
template <typename T>
void WriteStruct(T* out, const T& value)
{
  const size_t size = out->struct_size;
  std::memcpy(out, &value, std::min(size, sizeof(T)));
  out->struct_size = size;
}

std::filesystem::path U8Path(const std::string& path)
{
  return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

wii_gamecube_kit_container ToContainer(DiscIO::BlobType type)
{
  switch (type)
  {
  case DiscIO::BlobType::PLAIN:
  case DiscIO::BlobType::SPLIT_PLAIN:
    return WII_GAMECUBE_KIT_CONTAINER_PLAIN;
  case DiscIO::BlobType::GCZ:
    return WII_GAMECUBE_KIT_CONTAINER_GCZ;
  case DiscIO::BlobType::CISO:
    return WII_GAMECUBE_KIT_CONTAINER_CISO;
  case DiscIO::BlobType::WBFS:
    return WII_GAMECUBE_KIT_CONTAINER_WBFS;
  case DiscIO::BlobType::TGC:
    return WII_GAMECUBE_KIT_CONTAINER_TGC;
  case DiscIO::BlobType::WIA:
    return WII_GAMECUBE_KIT_CONTAINER_WIA;
  case DiscIO::BlobType::RVZ:
    return WII_GAMECUBE_KIT_CONTAINER_RVZ;
  case DiscIO::BlobType::NFS:
    return WII_GAMECUBE_KIT_CONTAINER_NFS;
  default:
    return WII_GAMECUBE_KIT_CONTAINER_OTHER;
  }
}

wii_gamecube_kit_status CheckReadable(Operation& op, const char* path, std::unique_ptr<DiscIO::BlobReader>* blob)
{
  if (!File::Exists(path))
    return op.Fail(WII_GAMECUBE_KIT_ERR_FILE_OPEN, fmt::format("The file \"{}\" does not exist.", path));
  *blob = DiscIO::CreateBlobReader(path);
  if (!*blob)
    return op.Fail(WII_GAMECUBE_KIT_ERR_FILE_OPEN, fmt::format("The file \"{}\" could not be opened.", path));
  return WII_GAMECUBE_KIT_OK;
}

// "N of M blocks. Compression ratio R%" (Dolphin's WIA/RVZ progress text) -> R / 100.
double ParseRatio(const std::string& text)
{
  const size_t pos = text.find("ratio ");
  if (pos == std::string::npos)
    return 0.0;
  int percent = 0;
  for (size_t i = pos + 6; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i)
    percent = percent * 10 + (text[i] - '0');
  return percent / 100.0;
}

void WriteHashResult(wii_gamecube_kit_hash_result* out, const wii_gamecube_kit::Digests& digests, u64 size,
                     bool size_is_exact)
{
  wii_gamecube_kit_hash_result result;
  wii_gamecube_kit_hash_result_init(&result);
  CopyString(result.crc32, digests.crc32);
  CopyString(result.md5, digests.md5);
  CopyString(result.sha1, digests.sha1);
  result.size = size;
  result.size_is_exact = size_is_exact;
  WriteStruct(out, result);
}

template <typename F>
wii_gamecube_kit_status Guard(wii_gamecube_kit_context* ctx, F&& body)
{
  try
  {
    return body();
  }
  catch (const std::bad_alloc&)
  {
    ctx->last_error = "Out of memory.";
    return WII_GAMECUBE_KIT_ERR_OUT_OF_MEMORY;
  }
  catch (const std::exception& e)
  {
    ctx->last_error = fmt::format("Internal error: {}", e.what());
    return WII_GAMECUBE_KIT_ERR_INTERNAL;
  }
  catch (...)
  {
    ctx->last_error = "Internal error.";
    return WII_GAMECUBE_KIT_ERR_INTERNAL;
  }
}
}  // namespace

namespace wii_gamecube_kit
{
void Log(wii_gamecube_kit_log_level level, const std::string& message)
{
  wii_gamecube_kit_context* ctx = t_ctx;
  if (ctx != nullptr && ctx->log != nullptr)
    ctx->log(ctx->log_user, level, message.c_str());
}

void AddError(const std::string& message)
{
  if (t_ctx != nullptr)
    t_ctx->errors.push_back(message);
}

unsigned int CompressorThreadCount()
{
  if (t_ctx != nullptr && t_ctx->num_threads > 0)
    return static_cast<unsigned int>(t_ctx->num_threads);
  return std::max<unsigned int>(1, std::thread::hardware_concurrency());
}
}  // namespace wii_gamecube_kit

extern "C" {

WII_GAMECUBE_KIT_API const char* WII_GAMECUBE_KIT_CALL wii_gamecube_kit_status_string(wii_gamecube_kit_status status)
{
  switch (status)
  {
  case WII_GAMECUBE_KIT_OK:
    return "Success";
  case WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT:
    return "Invalid argument";
  case WII_GAMECUBE_KIT_ERR_FILE_EXISTS:
    return "Output file already exists";
  case WII_GAMECUBE_KIT_ERR_FILE_OPEN:
    return "Could not open file";
  case WII_GAMECUBE_KIT_ERR_IO:
    return "Read or write error";
  case WII_GAMECUBE_KIT_ERR_NOT_A_DISC:
    return "Not a GameCube or Wii disc image";
  case WII_GAMECUBE_KIT_ERR_UNSUPPORTED:
    return "Not supported";
  case WII_GAMECUBE_KIT_ERR_CANCELLED:
    return "Cancelled";
  case WII_GAMECUBE_KIT_ERR_OUT_OF_MEMORY:
    return "Out of memory";
  case WII_GAMECUBE_KIT_ERR_INTERNAL:
    return "Internal error";
  case WII_GAMECUBE_KIT_ERR_ABI_MISMATCH:
    return "Struct size not recognised";
  case WII_GAMECUBE_KIT_ERR_CONVERSION:
    return "Conversion failed";
  }
  return "Unknown status";
}

WII_GAMECUBE_KIT_API const char* WII_GAMECUBE_KIT_CALL wii_gamecube_kit_version(void)
{
  return "lib-wii-gamecube-kit " WII_GAMECUBE_KIT_VERSION " (Dolphin " WII_GAMECUBE_KIT_DOLPHIN_COMMIT_SHORT ")";
}

WII_GAMECUBE_KIT_API int WII_GAMECUBE_KIT_CALL wii_gamecube_kit_abi_version(void)
{
  return WII_GAMECUBE_KIT_ABI_VERSION;
}

WII_GAMECUBE_KIT_API wii_gamecube_kit_status WII_GAMECUBE_KIT_CALL wii_gamecube_kit_context_create(wii_gamecube_kit_context** out_ctx)
{
  if (out_ctx == nullptr)
    return WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT;
  *out_ctx = nullptr;
  InstallHandlers();
  wii_gamecube_kit_context* ctx = new (std::nothrow) wii_gamecube_kit_context;
  if (ctx == nullptr)
    return WII_GAMECUBE_KIT_ERR_OUT_OF_MEMORY;
  *out_ctx = ctx;
  return WII_GAMECUBE_KIT_OK;
}

WII_GAMECUBE_KIT_API void WII_GAMECUBE_KIT_CALL wii_gamecube_kit_context_destroy(wii_gamecube_kit_context* ctx)
{
  delete ctx;
}

WII_GAMECUBE_KIT_API void WII_GAMECUBE_KIT_CALL wii_gamecube_kit_context_set_progress(wii_gamecube_kit_context* ctx, wii_gamecube_kit_progress_fn fn, void* user)
{
  if (ctx == nullptr)
    return;
  ctx->progress = fn;
  ctx->progress_user = user;
}

WII_GAMECUBE_KIT_API void WII_GAMECUBE_KIT_CALL wii_gamecube_kit_context_set_log(wii_gamecube_kit_context* ctx, wii_gamecube_kit_log_fn fn, void* user)
{
  if (ctx == nullptr)
    return;
  ctx->log = fn;
  ctx->log_user = user;
}

WII_GAMECUBE_KIT_API void WII_GAMECUBE_KIT_CALL wii_gamecube_kit_context_set_num_threads(wii_gamecube_kit_context* ctx, int count)
{
  if (ctx != nullptr)
    ctx->num_threads = std::max(0, count);
}

WII_GAMECUBE_KIT_API void WII_GAMECUBE_KIT_CALL wii_gamecube_kit_context_cancel(wii_gamecube_kit_context* ctx)
{
  if (ctx != nullptr)
    ctx->cancel.store(true);
}

WII_GAMECUBE_KIT_API const char* WII_GAMECUBE_KIT_CALL wii_gamecube_kit_last_error(const wii_gamecube_kit_context* ctx)
{
  return ctx != nullptr ? ctx->last_error.c_str() : "";
}

WII_GAMECUBE_KIT_API void WII_GAMECUBE_KIT_CALL wii_gamecube_kit_disc_info_init(wii_gamecube_kit_disc_info* info)
{
  if (info == nullptr)
    return;
  std::memset(info, 0, sizeof(*info));
  info->struct_size = sizeof(*info);
  info->region = WII_GAMECUBE_KIT_REGION_UNKNOWN;
}

WII_GAMECUBE_KIT_API wii_gamecube_kit_status WII_GAMECUBE_KIT_CALL wii_gamecube_kit_probe(wii_gamecube_kit_context* ctx, const char* path, wii_gamecube_kit_disc_info* info)
{
  if (ctx == nullptr)
    return WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT;
  return Guard(ctx, [&]() -> wii_gamecube_kit_status {
    Operation op(ctx);
    if (path == nullptr || info == nullptr)
      return op.Fail(WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT, "path and info are required.");
    if (info->struct_size < sizeof(wii_gamecube_kit_disc_info))
      return op.Fail(WII_GAMECUBE_KIT_ERR_ABI_MISMATCH, "wii_gamecube_kit_disc_info.struct_size is too small.");
    if (op.Cancelled())
      return op.Fail(WII_GAMECUBE_KIT_ERR_CANCELLED, "Cancelled.");

    std::unique_ptr<DiscIO::BlobReader> blob;
    if (const wii_gamecube_kit_status status = CheckReadable(op, path, &blob); status != WII_GAMECUBE_KIT_OK)
      return status;
    const std::unique_ptr<DiscIO::VolumeDisc> volume = DiscIO::CreateDisc(path);
    if (!volume)
    {
      return op.Fail(WII_GAMECUBE_KIT_ERR_NOT_A_DISC,
                     fmt::format("\"{}\" is not a GameCube or Wii disc image.", path));
    }

    wii_gamecube_kit_disc_info result;
    wii_gamecube_kit_disc_info_init(&result);
    result.container = ToContainer(blob->GetBlobType());
    switch (volume->GetVolumeType())
    {
    case DiscIO::Platform::WiiDisc:
      result.platform = WII_GAMECUBE_KIT_PLATFORM_WII;
      break;
    case DiscIO::Platform::Triforce:
      result.platform = WII_GAMECUBE_KIT_PLATFORM_TRIFORCE;
      break;
    default:
      result.platform = WII_GAMECUBE_KIT_PLATFORM_GAMECUBE;
      break;
    }
    CopyString(result.game_id, volume->GetGameID());
    CopyString(result.maker_id, volume->GetMakerID());
    CopyString(result.internal_name, volume->GetInternalName());
    result.disc_number = volume->GetDiscNumber().value_or(0);
    result.revision = volume->GetRevision().value_or(0);
    result.region = static_cast<wii_gamecube_kit_region>(volume->GetRegion());
    result.data_size = blob->GetDataSize();
    result.data_size_is_exact = blob->GetDataSizeType() == DiscIO::DataSizeType::Accurate;
    result.file_size = blob->GetRawSize();
    result.block_size = static_cast<uint32_t>(blob->GetBlockSize());
    CopyString(result.compression_method, blob->GetCompressionMethod());
    if (const std::optional<int> level = blob->GetCompressionLevel())
    {
      result.compression_level = *level;
      result.has_compression_level = 1;
    }
    result.is_nkit = volume->IsNKit();
    result.is_datel = volume->IsDatelDisc();
    result.is_scrubbable = !result.is_datel;
    WriteStruct(info, result);
    return WII_GAMECUBE_KIT_OK;
  });
}

WII_GAMECUBE_KIT_API void WII_GAMECUBE_KIT_CALL wii_gamecube_kit_convert_options_init(wii_gamecube_kit_convert_options* opts)
{
  if (opts == nullptr)
    return;
  std::memset(opts, 0, sizeof(*opts));
  opts->struct_size = sizeof(*opts);
  opts->format = WII_GAMECUBE_KIT_FORMAT_RVZ;
  opts->compression = WII_GAMECUBE_KIT_COMPRESSION_ZSTD;
  opts->compression_level = 5;
  opts->block_size = 0x20000;
}

WII_GAMECUBE_KIT_API int WII_GAMECUBE_KIT_CALL wii_gamecube_kit_block_size_valid(wii_gamecube_kit_format format, uint32_t block_size)
{
  switch (format)
  {
  case WII_GAMECUBE_KIT_FORMAT_ISO:
    return block_size == 0;
  case WII_GAMECUBE_KIT_FORMAT_GCZ:
  case WII_GAMECUBE_KIT_FORMAT_WIA:
  case WII_GAMECUBE_KIT_FORMAT_RVZ:
    if (block_size == 0 || block_size > static_cast<uint32_t>(std::numeric_limits<int>::max()))
      return 0;
    return DiscIO::IsDiscImageBlockSizeValid(
        static_cast<int>(block_size), format == WII_GAMECUBE_KIT_FORMAT_GCZ ? DiscIO::BlobType::GCZ :
                                      format == WII_GAMECUBE_KIT_FORMAT_WIA ? DiscIO::BlobType::WIA :
                                                                  DiscIO::BlobType::RVZ);
  }
  return 0;
}

WII_GAMECUBE_KIT_API void WII_GAMECUBE_KIT_CALL wii_gamecube_kit_compression_levels(wii_gamecube_kit_compression compression, int* min, int* max)
{
  std::pair<int, int> range{0, 0};
  if (compression != WII_GAMECUBE_KIT_COMPRESSION_NONE && compression != WII_GAMECUBE_KIT_COMPRESSION_PURGE &&
      compression >= WII_GAMECUBE_KIT_COMPRESSION_BZIP2 && compression <= WII_GAMECUBE_KIT_COMPRESSION_ZSTD)
  {
    range = DiscIO::GetAllowedCompressionLevels(
        static_cast<DiscIO::WIARVZCompressionType>(compression), false);
  }
  if (min != nullptr)
    *min = range.first;
  if (max != nullptr)
    *max = range.second;
}

WII_GAMECUBE_KIT_API wii_gamecube_kit_status WII_GAMECUBE_KIT_CALL wii_gamecube_kit_convert(wii_gamecube_kit_context* ctx, const wii_gamecube_kit_convert_options* in_opts)
{
  if (ctx == nullptr)
    return WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT;
  return Guard(ctx, [&]() -> wii_gamecube_kit_status {
    Operation op(ctx);
    wii_gamecube_kit_convert_options opts;
    if (in_opts == nullptr)
      return op.Fail(WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT, "options are required.");
    if (!ReadStruct(in_opts, &opts, CONVERT_OPTIONS_ABI1_SIZE, wii_gamecube_kit_convert_options_init))
      return op.Fail(WII_GAMECUBE_KIT_ERR_ABI_MISMATCH, "wii_gamecube_kit_convert_options.struct_size is too small.");
    if (opts.input == nullptr || opts.output == nullptr || !*opts.input || !*opts.output)
      return op.Fail(WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT, "input and output are required.");
    if (op.Cancelled())
      return op.Fail(WII_GAMECUBE_KIT_ERR_CANCELLED, "Cancelled.");

    const unsigned source_hashes = opts.source_hashes;
    if ((source_hashes & ~static_cast<unsigned>(wii_gamecube_kit::HASH_ALL)) != 0)
      return op.Fail(WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT, "Unknown hash in source_hashes.");
    if (source_hashes != 0 && opts.source_hash == nullptr)
      return op.Fail(WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT, "source_hashes needs source_hash.");
    if (source_hashes != 0 && opts.source_hash->struct_size < sizeof(wii_gamecube_kit_hash_result))
      return op.Fail(WII_GAMECUBE_KIT_ERR_ABI_MISMATCH, "wii_gamecube_kit_hash_result.struct_size is too small.");

    const std::string input = opts.input;
    const std::string output = opts.output;
    const wii_gamecube_kit_format format = opts.format;
    if (format < WII_GAMECUBE_KIT_FORMAT_ISO || format > WII_GAMECUBE_KIT_FORMAT_RVZ)
      return op.Fail(WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT, "Unknown output format.");

    {
      std::error_code ec;
      if (std::filesystem::equivalent(U8Path(input), U8Path(output), ec))
        return op.Fail(WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT, "The input and output are the same file.");
    }
    if (File::Exists(output) && !opts.overwrite)
    {
      return op.Fail(WII_GAMECUBE_KIT_ERR_FILE_EXISTS,
                     fmt::format("The output file \"{}\" already exists.", output));
    }

    std::unique_ptr<DiscIO::BlobReader> blob;
    if (const wii_gamecube_kit_status status = CheckReadable(op, input.c_str(), &blob); status != WII_GAMECUBE_KIT_OK)
      return status;

    // DolphinTool continues with a warning when the input isn't a disc; NoIntrovert only
    // converts discs, so this is an error here.
    const std::unique_ptr<DiscIO::VolumeDisc> volume = DiscIO::CreateDisc(input);
    if (!volume)
    {
      return op.Fail(WII_GAMECUBE_KIT_ERR_NOT_A_DISC,
                     fmt::format("\"{}\" is not a GameCube or Wii disc image.", input));
    }

    // Dolphin only converts images whose data size is known exactly (GameFile::
    // ShouldAllowConversion); the converters ASSERT it. CISO, WBFS and NFS don't qualify.
    if (blob->GetDataSizeType() != DiscIO::DataSizeType::Accurate)
    {
      return op.Fail(WII_GAMECUBE_KIT_ERR_UNSUPPORTED,
                     fmt::format("{} images can't be converted: the container doesn't record the "
                                 "disc's exact size.",
                                 DiscIO::GetName(blob->GetBlobType(), false)));
    }

    const bool scrub = opts.scrub != 0;
    if (scrub)
    {
      if (volume->IsDatelDisc())
        return op.Fail(WII_GAMECUBE_KIT_ERR_UNSUPPORTED, "Scrubbing a Datel disc is not supported.");
      blob = DiscIO::ScrubbedBlob::Create(input);
      if (!blob)
      {
        return op.Fail(WII_GAMECUBE_KIT_ERR_CONVERSION,
                       "Unable to process disc image. Try again without scrubbing.");
      }
      if (format == WII_GAMECUBE_KIT_FORMAT_RVZ)
      {
        Warn("Scrubbing an RVZ container does not offer significant space advantages. "
             "Continuing anyway.");
      }
      if (format == WII_GAMECUBE_KIT_FORMAT_ISO)
      {
        Warn("Scrubbing does not save space when converting to ISO unless using external "
             "compression. Continuing anyway.");
      }
    }

    const bool wii = volume->GetVolumeType() == DiscIO::Platform::WiiDisc;
    if (!scrub && format == WII_GAMECUBE_KIT_FORMAT_GCZ && wii && !volume->IsDatelDisc())
    {
      Warn("Converting Wii disc images to GCZ without scrubbing may not offer space advantages "
           "over ISO. Continuing anyway.");
    }
    if (volume->IsNKit())
      Warn("Converting an NKit file, output will still be NKit! Continuing anyway.");

    const int block_size = static_cast<int>(std::min<uint32_t>(
        opts.block_size, static_cast<uint32_t>(std::numeric_limits<int>::max())));
    if (format != WII_GAMECUBE_KIT_FORMAT_ISO)
    {
      if (!wii_gamecube_kit_block_size_valid(format, opts.block_size))
        return op.Fail(WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT, "Block size is not valid for this format.");
      if (block_size < DiscIO::PREFERRED_MIN_BLOCK_SIZE ||
          block_size > DiscIO::PREFERRED_MAX_BLOCK_SIZE)
      {
        Warn("Block size is not ideal for performance. Continuing anyway.");
      }
      if (format == WII_GAMECUBE_KIT_FORMAT_GCZ &&
          !DiscIO::IsGCZBlockSizeLegacyCompatible(block_size, volume->GetDataSize()))
      {
        Warn("For GCZs to be compatible with Dolphin < 5.0-11893, the file size must be an "
             "integer multiple of the block size and must not be an integer multiple of the "
             "block size multiplied by 32. Continuing anyway.");
      }
    }

    int level = opts.compression_level;
    const wii_gamecube_kit_compression compression = opts.compression;
    if (format == WII_GAMECUBE_KIT_FORMAT_WIA || format == WII_GAMECUBE_KIT_FORMAT_RVZ)
    {
      if (compression < WII_GAMECUBE_KIT_COMPRESSION_NONE || compression > WII_GAMECUBE_KIT_COMPRESSION_ZSTD)
        return op.Fail(WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT, "Unknown compression method.");
      if ((format == WII_GAMECUBE_KIT_FORMAT_WIA && compression == WII_GAMECUBE_KIT_COMPRESSION_ZSTD) ||
          (format == WII_GAMECUBE_KIT_FORMAT_RVZ && compression == WII_GAMECUBE_KIT_COMPRESSION_PURGE))
      {
        return op.Fail(WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT,
                       "Compression type is not supported for the container format.");
      }
      if (compression == WII_GAMECUBE_KIT_COMPRESSION_NONE || compression == WII_GAMECUBE_KIT_COMPRESSION_PURGE)
      {
        level = 0;
      }
      else
      {
        int min = 0, max = 0;
        wii_gamecube_kit_compression_levels(compression, &min, &max);
        if (level < min || level > max)
        {
          return op.Fail(WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT,
                         fmt::format("Compression level not in acceptable range ({}..{}).", min,
                                     max));
        }
      }
    }

    // Hash what the converter reads (after scrubbing, so it's what the output decodes to).
    wii_gamecube_kit::HashingBlobReader* hashing = nullptr;
    if (source_hashes != 0)
    {
      auto wrapper = std::make_unique<wii_gamecube_kit::HashingBlobReader>(std::move(blob), source_hashes);
      hashing = wrapper.get();
      blob = std::move(wrapper);
    }
    const u64 data_size = blob->GetDataSize();

    // Write to a temporary file next to the output and rename it on success, so a failed or
    // cancelled conversion never leaves a partial file, or destroys an output being replaced.
    const std::string temp = output + ".lib-wii-gamecube-kit-tmp";
    File::Delete(temp, File::IfAbsentBehavior::NoConsoleWarning);

    bool cancelled = false;
    const DiscIO::CompressCB callback = [&](const std::string& text, float percent) {
      if (ctx->progress != nullptr &&
          ctx->progress(ctx->progress_user, WII_GAMECUBE_KIT_PHASE_CONVERTING, percent, ParseRatio(text)) != 0)
      {
        ctx->cancel.store(true);
      }
      cancelled = ctx->cancel.load();
      return !cancelled;
    };

    bool success = false;
    switch (format)
    {
    case WII_GAMECUBE_KIT_FORMAT_ISO:
      success = DiscIO::ConvertToPlain(blob.get(), input, temp, callback);
      break;
    case WII_GAMECUBE_KIT_FORMAT_GCZ:
    {
      u32 sub_type = std::numeric_limits<u32>::max();
      if (volume->GetVolumeType() == DiscIO::Platform::GameCubeDisc)
        sub_type = 0;
      else if (wii)
        sub_type = 1;
      success = DiscIO::ConvertToGCZ(blob.get(), input, temp, sub_type, block_size, callback);
      break;
    }
    case WII_GAMECUBE_KIT_FORMAT_WIA:
    case WII_GAMECUBE_KIT_FORMAT_RVZ:
      success = DiscIO::ConvertToWIAOrRVZ(
          blob.get(), input, temp, format == WII_GAMECUBE_KIT_FORMAT_RVZ,
          static_cast<DiscIO::WIARVZCompressionType>(compression), level, block_size, callback);
      break;
    }
    wii_gamecube_kit::Digests digests;
    const bool hashed = !success || hashing == nullptr || hashing->Finish(&digests);
    hashing = nullptr;
    blob.reset();

    if (success && ctx->progress != nullptr)
      ctx->progress(ctx->progress_user, WII_GAMECUBE_KIT_PHASE_CONVERTING, 1.0, 0.0);

    if (!success)
    {
      File::Delete(temp, File::IfAbsentBehavior::NoConsoleWarning);
      if (cancelled || ctx->cancel.load())
        return op.Fail(WII_GAMECUBE_KIT_ERR_CANCELLED, "Cancelled.");
      return op.Fail(WII_GAMECUBE_KIT_ERR_CONVERSION, "");
    }
    if (!hashed)
    {
      File::Delete(temp, File::IfAbsentBehavior::NoConsoleWarning);
      return op.Fail(WII_GAMECUBE_KIT_ERR_IO, fmt::format("Failed to hash \"{}\".", input));
    }

    std::error_code ec;
    std::filesystem::rename(U8Path(temp), U8Path(output), ec);
    if (ec)
    {
      File::Delete(temp, File::IfAbsentBehavior::NoConsoleWarning);
      return op.Fail(WII_GAMECUBE_KIT_ERR_IO, fmt::format("Could not move the finished file to \"{}\": {}",
                                              output, ec.message()));
    }
    if (source_hashes != 0)
      WriteHashResult(opts.source_hash, digests, data_size, true);
    return WII_GAMECUBE_KIT_OK;
  });
}

WII_GAMECUBE_KIT_API void WII_GAMECUBE_KIT_CALL wii_gamecube_kit_hash_result_init(wii_gamecube_kit_hash_result* result)
{
  if (result == nullptr)
    return;
  std::memset(result, 0, sizeof(*result));
  result->struct_size = sizeof(*result);
}

WII_GAMECUBE_KIT_API void WII_GAMECUBE_KIT_CALL wii_gamecube_kit_hash_options_init(wii_gamecube_kit_hash_options* opts)
{
  if (opts == nullptr)
    return;
  std::memset(opts, 0, sizeof(*opts));
  opts->struct_size = sizeof(*opts);
  opts->hashes = WII_GAMECUBE_KIT_HASH_ALL;
}

WII_GAMECUBE_KIT_API wii_gamecube_kit_status WII_GAMECUBE_KIT_CALL wii_gamecube_kit_hash(wii_gamecube_kit_context* ctx, const char* path, wii_gamecube_kit_hash_result* out)
{
  return wii_gamecube_kit_hash_ex(ctx, path, nullptr, out);
}

WII_GAMECUBE_KIT_API wii_gamecube_kit_status WII_GAMECUBE_KIT_CALL wii_gamecube_kit_hash_ex(wii_gamecube_kit_context* ctx, const char* path, const wii_gamecube_kit_hash_options* in_opts, wii_gamecube_kit_hash_result* out)
{
  if (ctx == nullptr)
    return WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT;
  return Guard(ctx, [&]() -> wii_gamecube_kit_status {
    Operation op(ctx);
    if (path == nullptr || out == nullptr)
      return op.Fail(WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT, "path and result are required.");
    if (out->struct_size < sizeof(wii_gamecube_kit_hash_result))
      return op.Fail(WII_GAMECUBE_KIT_ERR_ABI_MISMATCH, "wii_gamecube_kit_hash_result.struct_size is too small.");
    wii_gamecube_kit_hash_options opts;
    wii_gamecube_kit_hash_options_init(&opts);
    if (in_opts != nullptr && !ReadStruct(in_opts, &opts, sizeof(opts), wii_gamecube_kit_hash_options_init))
      return op.Fail(WII_GAMECUBE_KIT_ERR_ABI_MISMATCH, "wii_gamecube_kit_hash_options.struct_size is too small.");
    if (opts.hashes == 0 || (opts.hashes & ~static_cast<unsigned>(wii_gamecube_kit::HASH_ALL)) != 0)
    {
      return op.Fail(WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT,
                     "hashes must be a non-empty set of WII_GAMECUBE_KIT_HASH_* bits.");
    }
    if (op.Cancelled())
      return op.Fail(WII_GAMECUBE_KIT_ERR_CANCELLED, "Cancelled.");

    std::unique_ptr<DiscIO::BlobReader> blob;
    if (const wii_gamecube_kit_status status = CheckReadable(op, path, &blob); status != WII_GAMECUBE_KIT_OK)
      return status;
    const u64 total = blob->GetDataSize();
    const bool size_is_exact = blob->GetDataSizeType() == DiscIO::DataSizeType::Accurate;

    // Decoding is the slow part of reading a compressed container (for Wii discs it
    // includes re-encrypting and re-hashing every partition group), so those get several
    // readers, each decoding its own pieces. Plain images are limited by the disk: one.
    const DiscIO::BlobType type = blob->GetBlobType();
    const bool compressed = type == DiscIO::BlobType::GCZ || type == DiscIO::BlobType::WIA ||
                            type == DiscIO::BlobType::RVZ || type == DiscIO::BlobType::NFS;
    constexpr unsigned MAX_DECODERS = 6;
    const unsigned decoders =
        compressed ? std::min(MAX_DECODERS, wii_gamecube_kit::CompressorThreadCount()) : 1;
    const u64 block_size = blob->GetBlockSize();
    std::vector<std::unique_ptr<DiscIO::BlobReader>> readers;
    readers.push_back(std::move(blob));
    while (readers.size() < decoders)
    {
      std::unique_ptr<DiscIO::BlobReader> reader = DiscIO::CreateBlobReader(path);
      if (!reader)
        break;
      readers.push_back(std::move(reader));
    }

    // Whole container blocks per piece where possible, like ConvertToPlain. Parallel
    // pieces are larger: a Wii group that straddles two pieces is decoded by both.
    const u64 desired = readers.size() > 1 ? 4 * wii_gamecube_kit::HASH_CHUNK_SIZE :
                                             wii_gamecube_kit::HASH_CHUNK_SIZE;
    u64 piece = block_size == 0 ? desired : block_size;
    while (piece < desired)
      piece *= 2;

    const std::shared_ptr<wii_gamecube_kit::ChunkPool> pool =
        wii_gamecube_kit::ChunkPool::Create(static_cast<size_t>(piece));
    wii_gamecube_kit::ParallelHasher hasher(opts.hashes, pool);
    wii_gamecube_kit::OrderedBlobReading reading(std::move(readers), total, pool);

    for (u64 i = 0; i < reading.PieceCount(); ++i)
    {
      const u64 pos = i * piece;
      if (ctx->progress != nullptr &&
          ctx->progress(ctx->progress_user, WII_GAMECUBE_KIT_PHASE_HASHING,
                        static_cast<double>(pos) / static_cast<double>(total), 0.0) != 0)
      {
        ctx->cancel.store(true);
      }
      if (ctx->cancel.load())
        return op.Fail(WII_GAMECUBE_KIT_ERR_CANCELLED, "Cancelled.");
      wii_gamecube_kit::Chunk chunk = reading.Next();
      if (!chunk)
        return op.Fail(WII_GAMECUBE_KIT_ERR_IO, fmt::format("Failed to read from \"{}\".", path));
      hasher.Add(std::move(chunk));
    }

    wii_gamecube_kit::Digests digests;
    if (!hasher.Finish(&digests))
      return op.Fail(WII_GAMECUBE_KIT_ERR_OUT_OF_MEMORY, "Out of memory.");
    if (ctx->progress != nullptr)
      ctx->progress(ctx->progress_user, WII_GAMECUBE_KIT_PHASE_HASHING, 1.0, 0.0);
    WriteHashResult(out, digests, total, size_is_exact);
    return WII_GAMECUBE_KIT_OK;
  });
}

}  // extern "C"
