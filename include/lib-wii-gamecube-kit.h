/*
 * lib-wii-gamecube-kit.h - C ABI for lib-wii-gamecube-kit (Dolphin's GameCube/Wii disc image
 * conversion as a library: ISO/GCZ/CISO/WBFS/WIA/RVZ <-> RVZ/WIA/GCZ/ISO)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Conventions (the same as lib-chd-kit's chdman.h)
 * -----------
 *  - All strings are UTF-8 and NUL-terminated. Paths are UTF-8 on every
 *    platform (including Windows).
 *  - Every operation takes a wii_gamecube_kit_context. A context is not thread-safe:
 *    use one context per thread. Separate contexts may run concurrently.
 *    wii_gamecube_kit_context_cancel() is the one call that may be made from any thread.
 *  - Functions return wii_gamecube_kit_status. On failure, wii_gamecube_kit_last_error(ctx) returns a
 *    human-readable message that stays valid until the next call on the same
 *    context.
 *  - No exceptions, no stdout/stderr output, no dialogs, no process exit.
 *    Diagnostics go to the optional log callback.
 *  - Option and result structs carry a struct_size field and must be
 *    initialized with the matching wii_gamecube_kit_*_init() function before use. New
 *    fields are only ever appended. The library accepts any struct_size from
 *    the ABI 1 size upward: fields beyond the caller's struct_size take their
 *    *_init() defaults, and a result struct is written only up to its
 *    struct_size. A struct_size below the ABI 1 size fails with
 *    WII_GAMECUBE_KIT_ERR_ABI_MISMATCH.
 *  - No user directory, config file or keys.bin is read. The Wii common keys
 *    are built in.
 */
#ifndef LIB_WII_GAMECUBE_KIT_H
#define LIB_WII_GAMECUBE_KIT_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(WII_GAMECUBE_KIT_BUILDING_SHARED)
#    define WII_GAMECUBE_KIT_API __declspec(dllexport)
#  elif defined(WII_GAMECUBE_KIT_STATIC)
#    define WII_GAMECUBE_KIT_API
#  else
#    define WII_GAMECUBE_KIT_API __declspec(dllimport)
#  endif
#  define WII_GAMECUBE_KIT_CALL __cdecl
#else
#  if defined(WII_GAMECUBE_KIT_BUILDING_SHARED) || defined(WII_GAMECUBE_KIT_BUILDING_STATIC)
#    define WII_GAMECUBE_KIT_API __attribute__((visibility("default")))
#  else
#    define WII_GAMECUBE_KIT_API
#  endif
#  define WII_GAMECUBE_KIT_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define WII_GAMECUBE_KIT_ABI_VERSION 1

/* ------------------------------------------------------------------ status */

typedef enum wii_gamecube_kit_status {
	WII_GAMECUBE_KIT_OK = 0,
	WII_GAMECUBE_KIT_ERR_INVALID_ARGUMENT = 1, /* bad option value or combination */
	WII_GAMECUBE_KIT_ERR_FILE_EXISTS = 2,      /* output exists and overwrite not set */
	WII_GAMECUBE_KIT_ERR_FILE_OPEN = 3,        /* could not open an input or output */
	WII_GAMECUBE_KIT_ERR_IO = 4,               /* read/write failure */
	WII_GAMECUBE_KIT_ERR_NOT_A_DISC = 5,       /* not a GameCube or Wii disc image */
	WII_GAMECUBE_KIT_ERR_UNSUPPORTED = 6,      /* e.g. scrubbing a Datel disc */
	WII_GAMECUBE_KIT_ERR_CANCELLED = 7,        /* cancelled via callback or cancel() */
	WII_GAMECUBE_KIT_ERR_OUT_OF_MEMORY = 8,
	WII_GAMECUBE_KIT_ERR_INTERNAL = 9,
	WII_GAMECUBE_KIT_ERR_ABI_MISMATCH = 10,    /* struct_size not recognised */
	WII_GAMECUBE_KIT_ERR_CONVERSION = 11       /* Dolphin reported a failure (see message) */
} wii_gamecube_kit_status;

/* Static English description of a status code. Never NULL. */
WII_GAMECUBE_KIT_API const char *WII_GAMECUBE_KIT_CALL wii_gamecube_kit_status_string(wii_gamecube_kit_status status);

/* Library version, e.g. "lib-wii-gamecube-kit 0.1.0 (Dolphin 1822e385)". */
WII_GAMECUBE_KIT_API const char *WII_GAMECUBE_KIT_CALL wii_gamecube_kit_version(void);
WII_GAMECUBE_KIT_API int WII_GAMECUBE_KIT_CALL wii_gamecube_kit_abi_version(void);

/* ----------------------------------------------------------------- context */

typedef struct wii_gamecube_kit_context wii_gamecube_kit_context;

typedef enum wii_gamecube_kit_phase {
	WII_GAMECUBE_KIT_PHASE_CONVERTING = 0,
	WII_GAMECUBE_KIT_PHASE_HASHING = 1
} wii_gamecube_kit_phase;

/*
 * Progress callback. fraction is 0.0..1.0. ratio is the running
 * output/input size ratio while writing WIA or RVZ (0.0..1.0+), otherwise 0.
 * Return non-zero to request cancellation.
 * Called on the thread that invoked the operation.
 */
typedef int (WII_GAMECUBE_KIT_CALL *wii_gamecube_kit_progress_fn)(void *user, wii_gamecube_kit_phase phase, double fraction, double ratio);

typedef enum wii_gamecube_kit_log_level {
	WII_GAMECUBE_KIT_LOG_INFO = 0,
	WII_GAMECUBE_KIT_LOG_WARNING = 1,
	WII_GAMECUBE_KIT_LOG_ERROR = 2   /* the cause of a failure, reported as it happens */
} wii_gamecube_kit_log_level;

/*
 * Log callback: the warnings DolphinTool prints (e.g. converting an NKit
 * image), and Dolphin's own warnings and errors. Every error is also added
 * to wii_gamecube_kit_last_error() if the operation fails. Treat unknown levels as
 * WII_GAMECUBE_KIT_LOG_WARNING. Called on the thread that invoked the operation.
 */
typedef void (WII_GAMECUBE_KIT_CALL *wii_gamecube_kit_log_fn)(void *user, wii_gamecube_kit_log_level level, const char *message);

WII_GAMECUBE_KIT_API wii_gamecube_kit_status WII_GAMECUBE_KIT_CALL wii_gamecube_kit_context_create(wii_gamecube_kit_context **out_ctx);
WII_GAMECUBE_KIT_API void WII_GAMECUBE_KIT_CALL wii_gamecube_kit_context_destroy(wii_gamecube_kit_context *ctx);

WII_GAMECUBE_KIT_API void WII_GAMECUBE_KIT_CALL wii_gamecube_kit_context_set_progress(wii_gamecube_kit_context *ctx, wii_gamecube_kit_progress_fn fn, void *user);
WII_GAMECUBE_KIT_API void WII_GAMECUBE_KIT_CALL wii_gamecube_kit_context_set_log(wii_gamecube_kit_context *ctx, wii_gamecube_kit_log_fn fn, void *user);

/* Limit compression worker threads for WIA/RVZ (0 = all processors). */
WII_GAMECUBE_KIT_API void WII_GAMECUBE_KIT_CALL wii_gamecube_kit_context_set_num_threads(wii_gamecube_kit_context *ctx, int count);

/*
 * Request cancellation of the operation running on ctx. Thread-safe.
 * If no operation is running yet, the request is kept and the next operation
 * on ctx fails with WII_GAMECUBE_KIT_ERR_CANCELLED before doing any work. The request is
 * cleared when an operation finishes, however it ends. A cancelled conversion
 * deletes its partial output.
 */
WII_GAMECUBE_KIT_API void WII_GAMECUBE_KIT_CALL wii_gamecube_kit_context_cancel(wii_gamecube_kit_context *ctx);

/* Message for the last failed call on ctx ("" after success). Never NULL. */
WII_GAMECUBE_KIT_API const char *WII_GAMECUBE_KIT_CALL wii_gamecube_kit_last_error(const wii_gamecube_kit_context *ctx);

/* ------------------------------------------------------------------- enums */

typedef enum wii_gamecube_kit_container {
	WII_GAMECUBE_KIT_CONTAINER_PLAIN = 0,   /* .iso / .gcm (also split .part0.iso sets) */
	WII_GAMECUBE_KIT_CONTAINER_GCZ = 1,
	WII_GAMECUBE_KIT_CONTAINER_CISO = 2,
	WII_GAMECUBE_KIT_CONTAINER_WBFS = 3,
	WII_GAMECUBE_KIT_CONTAINER_TGC = 4,
	WII_GAMECUBE_KIT_CONTAINER_WIA = 5,
	WII_GAMECUBE_KIT_CONTAINER_RVZ = 6,
	WII_GAMECUBE_KIT_CONTAINER_NFS = 7,
	WII_GAMECUBE_KIT_CONTAINER_OTHER = 99
} wii_gamecube_kit_container;

typedef enum wii_gamecube_kit_platform {
	WII_GAMECUBE_KIT_PLATFORM_GAMECUBE = 0,
	WII_GAMECUBE_KIT_PLATFORM_WII = 1,
	WII_GAMECUBE_KIT_PLATFORM_TRIFORCE = 2  /* arcade discs, GameCube-based */
} wii_gamecube_kit_platform;

typedef enum wii_gamecube_kit_compression {
	WII_GAMECUBE_KIT_COMPRESSION_NONE = 0,
	WII_GAMECUBE_KIT_COMPRESSION_PURGE = 1, /* WIA only */
	WII_GAMECUBE_KIT_COMPRESSION_BZIP2 = 2,
	WII_GAMECUBE_KIT_COMPRESSION_LZMA = 3,
	WII_GAMECUBE_KIT_COMPRESSION_LZMA2 = 4,
	WII_GAMECUBE_KIT_COMPRESSION_ZSTD = 5   /* RVZ only */
} wii_gamecube_kit_compression;

typedef enum wii_gamecube_kit_region {
	WII_GAMECUBE_KIT_REGION_NTSC_J = 0,
	WII_GAMECUBE_KIT_REGION_NTSC_U = 1,
	WII_GAMECUBE_KIT_REGION_PAL = 2,
	WII_GAMECUBE_KIT_REGION_UNKNOWN = 3,
	WII_GAMECUBE_KIT_REGION_NTSC_K = 4,
	WII_GAMECUBE_KIT_REGION_DEV = 5         /* Triforce */
} wii_gamecube_kit_region;

/* ------------------------------------------------------------------- probe */

typedef struct wii_gamecube_kit_disc_info {
	size_t struct_size;
	wii_gamecube_kit_container container;
	wii_gamecube_kit_platform platform;
	char game_id[8];             /* e.g. "GALE01", "" if none */
	char maker_id[4];            /* e.g. "01" */
	char internal_name[256];     /* from the disc header, UTF-8 */
	int disc_number;             /* 0 = disc 1 */
	int revision;
	wii_gamecube_kit_region region;
	uint64_t data_size;          /* size of the plain ISO */
	uint64_t file_size;          /* size of the file(s) on disk */
	uint32_t block_size;         /* container block/chunk size, 0 = none */
	char compression_method[32]; /* e.g. "Zstandard", "" if uncompressed */
	int compression_level;       /* only when has_compression_level */
	int has_compression_level;
	int is_nkit;                 /* NKit images can't be restored losslessly */
	int is_datel;                /* unlicensed Datel disc: can't be scrubbed */
	int is_scrubbable;           /* scrub is allowed for this disc */
	int data_size_is_exact;      /* 0 if the container only knows an upper bound */
} wii_gamecube_kit_disc_info;

WII_GAMECUBE_KIT_API void WII_GAMECUBE_KIT_CALL wii_gamecube_kit_disc_info_init(wii_gamecube_kit_disc_info *info);
/* Fails with WII_GAMECUBE_KIT_ERR_NOT_A_DISC when path is not a GameCube/Wii image. */
WII_GAMECUBE_KIT_API wii_gamecube_kit_status WII_GAMECUBE_KIT_CALL wii_gamecube_kit_probe(wii_gamecube_kit_context *ctx, const char *path, wii_gamecube_kit_disc_info *info);

/* ----------------------------------------------------------------- convert */

typedef enum wii_gamecube_kit_format {
	WII_GAMECUBE_KIT_FORMAT_ISO = 0,
	WII_GAMECUBE_KIT_FORMAT_GCZ = 1,
	WII_GAMECUBE_KIT_FORMAT_WIA = 2,
	WII_GAMECUBE_KIT_FORMAT_RVZ = 3
} wii_gamecube_kit_format;

/*
 * Defaults from wii_gamecube_kit_convert_options_init() match Dolphin's convert dialog:
 * RVZ, Zstandard level 5, 128 KiB blocks, no scrubbing.
 */
typedef struct wii_gamecube_kit_convert_options {
	size_t struct_size;
	const char *input;           /* required */
	const char *output;          /* required */
	wii_gamecube_kit_format format;
	wii_gamecube_kit_compression compression; /* WIA/RVZ only */
	int compression_level;       /* see wii_gamecube_kit_compression_levels() */
	uint32_t block_size;         /* GCZ/WIA/RVZ, see wii_gamecube_kit_block_size_valid() */
	int scrub;                   /* non-zero: zero out unused data (DolphinTool warns it saves little in RVZ) */
	int overwrite;               /* non-zero: replace an existing output */
} wii_gamecube_kit_convert_options;

WII_GAMECUBE_KIT_API void WII_GAMECUBE_KIT_CALL wii_gamecube_kit_convert_options_init(wii_gamecube_kit_convert_options *opts);
WII_GAMECUBE_KIT_API wii_gamecube_kit_status WII_GAMECUBE_KIT_CALL wii_gamecube_kit_convert(wii_gamecube_kit_context *ctx, const wii_gamecube_kit_convert_options *opts);

/* Non-zero if block_size is allowed for format (ISO: always 0). */
WII_GAMECUBE_KIT_API int WII_GAMECUBE_KIT_CALL wii_gamecube_kit_block_size_valid(wii_gamecube_kit_format format, uint32_t block_size);
/* The levels Dolphin allows; *min = *max = 0 for NONE and PURGE. */
WII_GAMECUBE_KIT_API void WII_GAMECUBE_KIT_CALL wii_gamecube_kit_compression_levels(wii_gamecube_kit_compression compression, int *min, int *max);

/* -------------------------------------------------------------------- hash */

/*
 * Hashes the decoded disc (the bytes a plain ISO of it would contain), for
 * comparing against Redump/No-Intro DATs or checking a conversion.
 */
typedef struct wii_gamecube_kit_hash_result {
	size_t struct_size;
	char crc32[9];               /* lowercase hex */
	char md5[33];
	char sha1[41];
	uint64_t size;               /* bytes hashed */
	int size_is_exact;           /* 0: the container (e.g. CISO, WBFS) only knows an
	                                upper bound, so the hashes may include padding */
} wii_gamecube_kit_hash_result;

WII_GAMECUBE_KIT_API void WII_GAMECUBE_KIT_CALL wii_gamecube_kit_hash_result_init(wii_gamecube_kit_hash_result *result);
WII_GAMECUBE_KIT_API wii_gamecube_kit_status WII_GAMECUBE_KIT_CALL wii_gamecube_kit_hash(wii_gamecube_kit_context *ctx, const char *path, wii_gamecube_kit_hash_result *result);

#ifdef __cplusplus
}
#endif

#endif /* LIB_WII_GAMECUBE_KIT_H */
