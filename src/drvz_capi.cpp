// SPDX-License-Identifier: GPL-2.0-or-later

// libdolphinrvz C ABI: DolphinTool's convert command and Dolphin's disc probing
// as library calls. The checks in drvz_convert follow DolphinTool's
// ConvertCommand.cpp; differences are noted where they occur.

#include "dolphinrvz.h"

#include <algorithm>
#include <atomic>
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
#include <mbedtls/md5.h>
#include <zlib.h>

#include "Common/Crypto/SHA1.h"
#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"
#include "DiscIO/Blob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/ScrubbedBlob.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/WIABlob.h"
#include "drvz_hooks.h"
#include "drvz_internal.h"

struct drvz_context
{
  drvz_progress_fn progress = nullptr;
  void* progress_user = nullptr;
  drvz_log_fn log = nullptr;
  void* log_user = nullptr;
  int num_threads = 0;
  std::atomic<bool> cancel{false};
  std::string last_error;
  std::vector<std::string> errors;
};

namespace
{
thread_local drvz_context* t_ctx = nullptr;

bool AlertHandler(const char* caption, const char* text, bool yes_no, Common::MsgType style)
{
  (void)caption;
  (void)yes_no;
  (void)style;
  // ShowMessageAlert has already logged the text. Keep it for drvz_last_error, and answer
  // "no" to any question (Dolphin's own behaviour when it has no UI).
  drvz::AddError(text);
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
  explicit Operation(drvz_context* ctx) : m_ctx(ctx), m_previous(t_ctx)
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
  drvz_status Fail(drvz_status status, const std::string& message)
  {
    std::string text = message;
    for (const std::string& line : m_ctx->errors)
    {
      if (line == message)
        continue;
      text += text.empty() ? line : ": " + line;
    }
    m_ctx->last_error = text.empty() ? drvz_status_string(status) : text;
    return status;
  }

  bool Cancelled() const { return m_ctx->cancel.load(); }

private:
  drvz_context* m_ctx;
  drvz_context* m_previous;
};

void Warn(const std::string& message)
{
  drvz::Log(DRVZ_LOG_WARNING, message);
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

// Reads a caller's options struct: at least the ABI 1 size, newer fields default.
template <typename T>
bool ReadStruct(const T* in, T* out)
{
  if (in->struct_size < sizeof(T))  // ABI 1 is the current layout
    return false;
  std::memcpy(out, in, sizeof(T));
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

drvz_container ToContainer(DiscIO::BlobType type)
{
  switch (type)
  {
  case DiscIO::BlobType::PLAIN:
  case DiscIO::BlobType::SPLIT_PLAIN:
    return DRVZ_CONTAINER_PLAIN;
  case DiscIO::BlobType::GCZ:
    return DRVZ_CONTAINER_GCZ;
  case DiscIO::BlobType::CISO:
    return DRVZ_CONTAINER_CISO;
  case DiscIO::BlobType::WBFS:
    return DRVZ_CONTAINER_WBFS;
  case DiscIO::BlobType::TGC:
    return DRVZ_CONTAINER_TGC;
  case DiscIO::BlobType::WIA:
    return DRVZ_CONTAINER_WIA;
  case DiscIO::BlobType::RVZ:
    return DRVZ_CONTAINER_RVZ;
  case DiscIO::BlobType::NFS:
    return DRVZ_CONTAINER_NFS;
  default:
    return DRVZ_CONTAINER_OTHER;
  }
}

drvz_status CheckReadable(Operation& op, const char* path, std::unique_ptr<DiscIO::BlobReader>* blob)
{
  if (!File::Exists(path))
    return op.Fail(DRVZ_ERR_FILE_OPEN, fmt::format("The file \"{}\" does not exist.", path));
  *blob = DiscIO::CreateBlobReader(path);
  if (!*blob)
    return op.Fail(DRVZ_ERR_FILE_OPEN, fmt::format("The file \"{}\" could not be opened.", path));
  return DRVZ_OK;
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

template <typename F>
drvz_status Guard(drvz_context* ctx, F&& body)
{
  try
  {
    return body();
  }
  catch (const std::bad_alloc&)
  {
    ctx->last_error = "Out of memory.";
    return DRVZ_ERR_OUT_OF_MEMORY;
  }
  catch (const std::exception& e)
  {
    ctx->last_error = fmt::format("Internal error: {}", e.what());
    return DRVZ_ERR_INTERNAL;
  }
  catch (...)
  {
    ctx->last_error = "Internal error.";
    return DRVZ_ERR_INTERNAL;
  }
}
}  // namespace

namespace drvz
{
void Log(drvz_log_level level, const std::string& message)
{
  drvz_context* ctx = t_ctx;
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
}  // namespace drvz

extern "C" {

DRVZ_API const char* DRVZ_CALL drvz_status_string(drvz_status status)
{
  switch (status)
  {
  case DRVZ_OK:
    return "Success";
  case DRVZ_ERR_INVALID_ARGUMENT:
    return "Invalid argument";
  case DRVZ_ERR_FILE_EXISTS:
    return "Output file already exists";
  case DRVZ_ERR_FILE_OPEN:
    return "Could not open file";
  case DRVZ_ERR_IO:
    return "Read or write error";
  case DRVZ_ERR_NOT_A_DISC:
    return "Not a GameCube or Wii disc image";
  case DRVZ_ERR_UNSUPPORTED:
    return "Not supported";
  case DRVZ_ERR_CANCELLED:
    return "Cancelled";
  case DRVZ_ERR_OUT_OF_MEMORY:
    return "Out of memory";
  case DRVZ_ERR_INTERNAL:
    return "Internal error";
  case DRVZ_ERR_ABI_MISMATCH:
    return "Struct size not recognised";
  case DRVZ_ERR_CONVERSION:
    return "Conversion failed";
  }
  return "Unknown status";
}

DRVZ_API const char* DRVZ_CALL drvz_version(void)
{
  return "libdolphinrvz " DRVZ_VERSION " (Dolphin " DRVZ_DOLPHIN_COMMIT_SHORT ")";
}

DRVZ_API int DRVZ_CALL drvz_abi_version(void)
{
  return DRVZ_ABI_VERSION;
}

DRVZ_API drvz_status DRVZ_CALL drvz_context_create(drvz_context** out_ctx)
{
  if (out_ctx == nullptr)
    return DRVZ_ERR_INVALID_ARGUMENT;
  *out_ctx = nullptr;
  InstallHandlers();
  drvz_context* ctx = new (std::nothrow) drvz_context;
  if (ctx == nullptr)
    return DRVZ_ERR_OUT_OF_MEMORY;
  *out_ctx = ctx;
  return DRVZ_OK;
}

DRVZ_API void DRVZ_CALL drvz_context_destroy(drvz_context* ctx)
{
  delete ctx;
}

DRVZ_API void DRVZ_CALL drvz_context_set_progress(drvz_context* ctx, drvz_progress_fn fn, void* user)
{
  if (ctx == nullptr)
    return;
  ctx->progress = fn;
  ctx->progress_user = user;
}

DRVZ_API void DRVZ_CALL drvz_context_set_log(drvz_context* ctx, drvz_log_fn fn, void* user)
{
  if (ctx == nullptr)
    return;
  ctx->log = fn;
  ctx->log_user = user;
}

DRVZ_API void DRVZ_CALL drvz_context_set_num_threads(drvz_context* ctx, int count)
{
  if (ctx != nullptr)
    ctx->num_threads = std::max(0, count);
}

DRVZ_API void DRVZ_CALL drvz_context_cancel(drvz_context* ctx)
{
  if (ctx != nullptr)
    ctx->cancel.store(true);
}

DRVZ_API const char* DRVZ_CALL drvz_last_error(const drvz_context* ctx)
{
  return ctx != nullptr ? ctx->last_error.c_str() : "";
}

DRVZ_API void DRVZ_CALL drvz_disc_info_init(drvz_disc_info* info)
{
  if (info == nullptr)
    return;
  std::memset(info, 0, sizeof(*info));
  info->struct_size = sizeof(*info);
  info->region = DRVZ_REGION_UNKNOWN;
}

DRVZ_API drvz_status DRVZ_CALL drvz_probe(drvz_context* ctx, const char* path, drvz_disc_info* info)
{
  if (ctx == nullptr)
    return DRVZ_ERR_INVALID_ARGUMENT;
  return Guard(ctx, [&]() -> drvz_status {
    Operation op(ctx);
    if (path == nullptr || info == nullptr)
      return op.Fail(DRVZ_ERR_INVALID_ARGUMENT, "path and info are required.");
    if (info->struct_size < sizeof(drvz_disc_info))
      return op.Fail(DRVZ_ERR_ABI_MISMATCH, "drvz_disc_info.struct_size is too small.");
    if (op.Cancelled())
      return op.Fail(DRVZ_ERR_CANCELLED, "Cancelled.");

    std::unique_ptr<DiscIO::BlobReader> blob;
    if (const drvz_status status = CheckReadable(op, path, &blob); status != DRVZ_OK)
      return status;
    const std::unique_ptr<DiscIO::VolumeDisc> volume = DiscIO::CreateDisc(path);
    if (!volume)
    {
      return op.Fail(DRVZ_ERR_NOT_A_DISC,
                     fmt::format("\"{}\" is not a GameCube or Wii disc image.", path));
    }

    drvz_disc_info result;
    drvz_disc_info_init(&result);
    result.container = ToContainer(blob->GetBlobType());
    switch (volume->GetVolumeType())
    {
    case DiscIO::Platform::WiiDisc:
      result.platform = DRVZ_PLATFORM_WII;
      break;
    case DiscIO::Platform::Triforce:
      result.platform = DRVZ_PLATFORM_TRIFORCE;
      break;
    default:
      result.platform = DRVZ_PLATFORM_GAMECUBE;
      break;
    }
    CopyString(result.game_id, volume->GetGameID());
    CopyString(result.maker_id, volume->GetMakerID());
    CopyString(result.internal_name, volume->GetInternalName());
    result.disc_number = volume->GetDiscNumber().value_or(0);
    result.revision = volume->GetRevision().value_or(0);
    result.region = static_cast<drvz_region>(volume->GetRegion());
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
    return DRVZ_OK;
  });
}

DRVZ_API void DRVZ_CALL drvz_convert_options_init(drvz_convert_options* opts)
{
  if (opts == nullptr)
    return;
  std::memset(opts, 0, sizeof(*opts));
  opts->struct_size = sizeof(*opts);
  opts->format = DRVZ_FORMAT_RVZ;
  opts->compression = DRVZ_COMPRESSION_ZSTD;
  opts->compression_level = 5;
  opts->block_size = 0x20000;
}

DRVZ_API int DRVZ_CALL drvz_block_size_valid(drvz_format format, uint32_t block_size)
{
  switch (format)
  {
  case DRVZ_FORMAT_ISO:
    return block_size == 0;
  case DRVZ_FORMAT_GCZ:
  case DRVZ_FORMAT_WIA:
  case DRVZ_FORMAT_RVZ:
    if (block_size == 0 || block_size > static_cast<uint32_t>(std::numeric_limits<int>::max()))
      return 0;
    return DiscIO::IsDiscImageBlockSizeValid(
        static_cast<int>(block_size), format == DRVZ_FORMAT_GCZ ? DiscIO::BlobType::GCZ :
                                      format == DRVZ_FORMAT_WIA ? DiscIO::BlobType::WIA :
                                                                  DiscIO::BlobType::RVZ);
  }
  return 0;
}

DRVZ_API void DRVZ_CALL drvz_compression_levels(drvz_compression compression, int* min, int* max)
{
  std::pair<int, int> range{0, 0};
  if (compression != DRVZ_COMPRESSION_NONE && compression != DRVZ_COMPRESSION_PURGE &&
      compression >= DRVZ_COMPRESSION_BZIP2 && compression <= DRVZ_COMPRESSION_ZSTD)
  {
    range = DiscIO::GetAllowedCompressionLevels(
        static_cast<DiscIO::WIARVZCompressionType>(compression), false);
  }
  if (min != nullptr)
    *min = range.first;
  if (max != nullptr)
    *max = range.second;
}

DRVZ_API drvz_status DRVZ_CALL drvz_convert(drvz_context* ctx, const drvz_convert_options* in_opts)
{
  if (ctx == nullptr)
    return DRVZ_ERR_INVALID_ARGUMENT;
  return Guard(ctx, [&]() -> drvz_status {
    Operation op(ctx);
    drvz_convert_options opts;
    if (in_opts == nullptr)
      return op.Fail(DRVZ_ERR_INVALID_ARGUMENT, "options are required.");
    if (!ReadStruct(in_opts, &opts))
      return op.Fail(DRVZ_ERR_ABI_MISMATCH, "drvz_convert_options.struct_size is too small.");
    if (opts.input == nullptr || opts.output == nullptr || !*opts.input || !*opts.output)
      return op.Fail(DRVZ_ERR_INVALID_ARGUMENT, "input and output are required.");
    if (op.Cancelled())
      return op.Fail(DRVZ_ERR_CANCELLED, "Cancelled.");

    const std::string input = opts.input;
    const std::string output = opts.output;
    const drvz_format format = opts.format;
    if (format < DRVZ_FORMAT_ISO || format > DRVZ_FORMAT_RVZ)
      return op.Fail(DRVZ_ERR_INVALID_ARGUMENT, "Unknown output format.");

    {
      std::error_code ec;
      if (std::filesystem::equivalent(U8Path(input), U8Path(output), ec))
        return op.Fail(DRVZ_ERR_INVALID_ARGUMENT, "The input and output are the same file.");
    }
    if (File::Exists(output) && !opts.overwrite)
    {
      return op.Fail(DRVZ_ERR_FILE_EXISTS,
                     fmt::format("The output file \"{}\" already exists.", output));
    }

    std::unique_ptr<DiscIO::BlobReader> blob;
    if (const drvz_status status = CheckReadable(op, input.c_str(), &blob); status != DRVZ_OK)
      return status;

    // DolphinTool continues with a warning when the input isn't a disc; NoIntrovert only
    // converts discs, so this is an error here.
    const std::unique_ptr<DiscIO::VolumeDisc> volume = DiscIO::CreateDisc(input);
    if (!volume)
    {
      return op.Fail(DRVZ_ERR_NOT_A_DISC,
                     fmt::format("\"{}\" is not a GameCube or Wii disc image.", input));
    }

    // Dolphin only converts images whose data size is known exactly (GameFile::
    // ShouldAllowConversion); the converters ASSERT it. CISO, WBFS and NFS don't qualify.
    if (blob->GetDataSizeType() != DiscIO::DataSizeType::Accurate)
    {
      return op.Fail(DRVZ_ERR_UNSUPPORTED,
                     fmt::format("{} images can't be converted: the container doesn't record the "
                                 "disc's exact size.",
                                 DiscIO::GetName(blob->GetBlobType(), false)));
    }

    const bool scrub = opts.scrub != 0;
    if (scrub)
    {
      if (volume->IsDatelDisc())
        return op.Fail(DRVZ_ERR_UNSUPPORTED, "Scrubbing a Datel disc is not supported.");
      blob = DiscIO::ScrubbedBlob::Create(input);
      if (!blob)
      {
        return op.Fail(DRVZ_ERR_CONVERSION,
                       "Unable to process disc image. Try again without scrubbing.");
      }
      if (format == DRVZ_FORMAT_RVZ)
      {
        Warn("Scrubbing an RVZ container does not offer significant space advantages. "
             "Continuing anyway.");
      }
      if (format == DRVZ_FORMAT_ISO)
      {
        Warn("Scrubbing does not save space when converting to ISO unless using external "
             "compression. Continuing anyway.");
      }
    }

    const bool wii = volume->GetVolumeType() == DiscIO::Platform::WiiDisc;
    if (!scrub && format == DRVZ_FORMAT_GCZ && wii && !volume->IsDatelDisc())
    {
      Warn("Converting Wii disc images to GCZ without scrubbing may not offer space advantages "
           "over ISO. Continuing anyway.");
    }
    if (volume->IsNKit())
      Warn("Converting an NKit file, output will still be NKit! Continuing anyway.");

    const int block_size = static_cast<int>(std::min<uint32_t>(
        opts.block_size, static_cast<uint32_t>(std::numeric_limits<int>::max())));
    if (format != DRVZ_FORMAT_ISO)
    {
      if (!drvz_block_size_valid(format, opts.block_size))
        return op.Fail(DRVZ_ERR_INVALID_ARGUMENT, "Block size is not valid for this format.");
      if (block_size < DiscIO::PREFERRED_MIN_BLOCK_SIZE ||
          block_size > DiscIO::PREFERRED_MAX_BLOCK_SIZE)
      {
        Warn("Block size is not ideal for performance. Continuing anyway.");
      }
      if (format == DRVZ_FORMAT_GCZ &&
          !DiscIO::IsGCZBlockSizeLegacyCompatible(block_size, volume->GetDataSize()))
      {
        Warn("For GCZs to be compatible with Dolphin < 5.0-11893, the file size must be an "
             "integer multiple of the block size and must not be an integer multiple of the "
             "block size multiplied by 32. Continuing anyway.");
      }
    }

    int level = opts.compression_level;
    const drvz_compression compression = opts.compression;
    if (format == DRVZ_FORMAT_WIA || format == DRVZ_FORMAT_RVZ)
    {
      if (compression < DRVZ_COMPRESSION_NONE || compression > DRVZ_COMPRESSION_ZSTD)
        return op.Fail(DRVZ_ERR_INVALID_ARGUMENT, "Unknown compression method.");
      if ((format == DRVZ_FORMAT_WIA && compression == DRVZ_COMPRESSION_ZSTD) ||
          (format == DRVZ_FORMAT_RVZ && compression == DRVZ_COMPRESSION_PURGE))
      {
        return op.Fail(DRVZ_ERR_INVALID_ARGUMENT,
                       "Compression type is not supported for the container format.");
      }
      if (compression == DRVZ_COMPRESSION_NONE || compression == DRVZ_COMPRESSION_PURGE)
      {
        level = 0;
      }
      else
      {
        int min = 0, max = 0;
        drvz_compression_levels(compression, &min, &max);
        if (level < min || level > max)
        {
          return op.Fail(DRVZ_ERR_INVALID_ARGUMENT,
                         fmt::format("Compression level not in acceptable range ({}..{}).", min,
                                     max));
        }
      }
    }

    // Write to a temporary file next to the output and rename it on success, so a failed or
    // cancelled conversion never leaves a partial file, or destroys an output being replaced.
    const std::string temp = output + ".drvz-tmp";
    File::Delete(temp, File::IfAbsentBehavior::NoConsoleWarning);

    bool cancelled = false;
    const DiscIO::CompressCB callback = [&](const std::string& text, float percent) {
      if (ctx->progress != nullptr &&
          ctx->progress(ctx->progress_user, DRVZ_PHASE_CONVERTING, percent, ParseRatio(text)) != 0)
      {
        ctx->cancel.store(true);
      }
      cancelled = ctx->cancel.load();
      return !cancelled;
    };

    bool success = false;
    switch (format)
    {
    case DRVZ_FORMAT_ISO:
      success = DiscIO::ConvertToPlain(blob.get(), input, temp, callback);
      break;
    case DRVZ_FORMAT_GCZ:
    {
      u32 sub_type = std::numeric_limits<u32>::max();
      if (volume->GetVolumeType() == DiscIO::Platform::GameCubeDisc)
        sub_type = 0;
      else if (wii)
        sub_type = 1;
      success = DiscIO::ConvertToGCZ(blob.get(), input, temp, sub_type, block_size, callback);
      break;
    }
    case DRVZ_FORMAT_WIA:
    case DRVZ_FORMAT_RVZ:
      success = DiscIO::ConvertToWIAOrRVZ(
          blob.get(), input, temp, format == DRVZ_FORMAT_RVZ,
          static_cast<DiscIO::WIARVZCompressionType>(compression), level, block_size, callback);
      break;
    }
    blob.reset();

    if (success && ctx->progress != nullptr)
      ctx->progress(ctx->progress_user, DRVZ_PHASE_CONVERTING, 1.0, 0.0);

    if (!success)
    {
      File::Delete(temp, File::IfAbsentBehavior::NoConsoleWarning);
      if (cancelled || ctx->cancel.load())
        return op.Fail(DRVZ_ERR_CANCELLED, "Cancelled.");
      return op.Fail(DRVZ_ERR_CONVERSION, "");
    }

    std::error_code ec;
    std::filesystem::rename(U8Path(temp), U8Path(output), ec);
    if (ec)
    {
      File::Delete(temp, File::IfAbsentBehavior::NoConsoleWarning);
      return op.Fail(DRVZ_ERR_IO, fmt::format("Could not move the finished file to \"{}\": {}",
                                              output, ec.message()));
    }
    return DRVZ_OK;
  });
}

DRVZ_API void DRVZ_CALL drvz_hash_result_init(drvz_hash_result* result)
{
  if (result == nullptr)
    return;
  std::memset(result, 0, sizeof(*result));
  result->struct_size = sizeof(*result);
}

DRVZ_API drvz_status DRVZ_CALL drvz_hash(drvz_context* ctx, const char* path, drvz_hash_result* out)
{
  if (ctx == nullptr)
    return DRVZ_ERR_INVALID_ARGUMENT;
  return Guard(ctx, [&]() -> drvz_status {
    Operation op(ctx);
    if (path == nullptr || out == nullptr)
      return op.Fail(DRVZ_ERR_INVALID_ARGUMENT, "path and result are required.");
    if (out->struct_size < sizeof(drvz_hash_result))
      return op.Fail(DRVZ_ERR_ABI_MISMATCH, "drvz_hash_result.struct_size is too small.");
    if (op.Cancelled())
      return op.Fail(DRVZ_ERR_CANCELLED, "Cancelled.");

    std::unique_ptr<DiscIO::BlobReader> blob;
    if (const drvz_status status = CheckReadable(op, path, &blob); status != DRVZ_OK)
      return status;

    const u64 total = blob->GetDataSize();
    // Read whole container blocks where possible, like ConvertToPlain.
    constexpr u64 DESIRED_BUFFER_SIZE = 0x200000;
    u64 buffer_size = blob->GetBlockSize();
    if (buffer_size == 0)
      buffer_size = DESIRED_BUFFER_SIZE;
    while (buffer_size < DESIRED_BUFFER_SIZE)
      buffer_size *= 2;
    std::vector<u8> buffer(buffer_size);

    uLong crc = crc32(0L, Z_NULL, 0);
    mbedtls_md5_context md5;
    mbedtls_md5_init(&md5);
    mbedtls_md5_starts_ret(&md5);
    const std::unique_ptr<Common::SHA1::Context> sha1 = Common::SHA1::CreateContext();

    for (u64 pos = 0; pos < total;)
    {
      if (ctx->progress != nullptr &&
          ctx->progress(ctx->progress_user, DRVZ_PHASE_HASHING,
                        static_cast<double>(pos) / static_cast<double>(total), 0.0) != 0)
      {
        ctx->cancel.store(true);
      }
      if (ctx->cancel.load())
      {
        mbedtls_md5_free(&md5);
        return op.Fail(DRVZ_ERR_CANCELLED, "Cancelled.");
      }
      const u64 size = std::min(buffer_size, total - pos);
      if (!blob->Read(pos, size, buffer.data()))
      {
        mbedtls_md5_free(&md5);
        return op.Fail(DRVZ_ERR_IO, fmt::format("Failed to read from \"{}\".", path));
      }
      crc = crc32(crc, buffer.data(), static_cast<uInt>(size));
      mbedtls_md5_update_ret(&md5, buffer.data(), static_cast<size_t>(size));
      sha1->Update(buffer.data(), static_cast<size_t>(size));
      pos += size;
    }

    unsigned char md5_digest[16];
    mbedtls_md5_finish_ret(&md5, md5_digest);
    mbedtls_md5_free(&md5);
    const Common::SHA1::Digest sha1_digest = sha1->Finish();

    drvz_hash_result result;
    drvz_hash_result_init(&result);
    CopyString(result.crc32, fmt::format("{:08x}", static_cast<u32>(crc)));
    std::string hex;
    for (unsigned char b : md5_digest)
      hex += fmt::format("{:02x}", b);
    CopyString(result.md5, hex);
    hex.clear();
    for (u8 b : sha1_digest)
      hex += fmt::format("{:02x}", b);
    CopyString(result.sha1, hex);
    result.size = total;
    result.size_is_exact = blob->GetDataSizeType() == DiscIO::DataSizeType::Accurate;
    if (ctx->progress != nullptr)
      ctx->progress(ctx->progress_user, DRVZ_PHASE_HASHING, 1.0, 0.0);
    WriteStruct(out, result);
    return DRVZ_OK;
  });
}

}  // extern "C"
