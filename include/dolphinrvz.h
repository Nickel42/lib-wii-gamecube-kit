/*
 * dolphinrvz.h - C ABI for libdolphinrvz (Dolphin's GameCube/Wii disc image
 * conversion as a library: ISO/GCZ/CISO/WBFS/WIA/RVZ <-> RVZ/WIA/GCZ/ISO)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Conventions (the same as libchdman's chdman.h)
 * -----------
 *  - All strings are UTF-8 and NUL-terminated. Paths are UTF-8 on every
 *    platform (including Windows).
 *  - Every operation takes a drvz_context. A context is not thread-safe:
 *    use one context per thread. Separate contexts may run concurrently.
 *    drvz_context_cancel() is the one call that may be made from any thread.
 *  - Functions return drvz_status. On failure, drvz_last_error(ctx) returns a
 *    human-readable message that stays valid until the next call on the same
 *    context.
 *  - No exceptions, no stdout/stderr output, no dialogs, no process exit.
 *    Diagnostics go to the optional log callback.
 *  - Option and result structs carry a struct_size field and must be
 *    initialized with the matching drvz_*_init() function before use. New
 *    fields are only ever appended. The library accepts any struct_size from
 *    the ABI 1 size upward: fields beyond the caller's struct_size take their
 *    *_init() defaults, and a result struct is written only up to its
 *    struct_size. A struct_size below the ABI 1 size fails with
 *    DRVZ_ERR_ABI_MISMATCH.
 *  - No user directory, config file or keys.bin is read. The Wii common keys
 *    are built in.
 */
#ifndef DOLPHINRVZ_H
#define DOLPHINRVZ_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(DRVZ_BUILDING_SHARED)
#    define DRVZ_API __declspec(dllexport)
#  elif defined(DRVZ_STATIC)
#    define DRVZ_API
#  else
#    define DRVZ_API __declspec(dllimport)
#  endif
#  define DRVZ_CALL __cdecl
#else
#  if defined(DRVZ_BUILDING_SHARED) || defined(DRVZ_BUILDING_STATIC)
#    define DRVZ_API __attribute__((visibility("default")))
#  else
#    define DRVZ_API
#  endif
#  define DRVZ_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DRVZ_ABI_VERSION 1

/* ------------------------------------------------------------------ status */

typedef enum drvz_status {
	DRVZ_OK = 0,
	DRVZ_ERR_INVALID_ARGUMENT = 1, /* bad option value or combination */
	DRVZ_ERR_FILE_EXISTS = 2,      /* output exists and overwrite not set */
	DRVZ_ERR_FILE_OPEN = 3,        /* could not open an input or output */
	DRVZ_ERR_IO = 4,               /* read/write failure */
	DRVZ_ERR_NOT_A_DISC = 5,       /* not a GameCube or Wii disc image */
	DRVZ_ERR_UNSUPPORTED = 6,      /* e.g. scrubbing a Datel disc */
	DRVZ_ERR_CANCELLED = 7,        /* cancelled via callback or cancel() */
	DRVZ_ERR_OUT_OF_MEMORY = 8,
	DRVZ_ERR_INTERNAL = 9,
	DRVZ_ERR_ABI_MISMATCH = 10,    /* struct_size not recognised */
	DRVZ_ERR_CONVERSION = 11       /* Dolphin reported a failure (see message) */
} drvz_status;

/* Static English description of a status code. Never NULL. */
DRVZ_API const char *DRVZ_CALL drvz_status_string(drvz_status status);

/* Library version, e.g. "libdolphinrvz 0.1.0 (Dolphin 1822e385)". */
DRVZ_API const char *DRVZ_CALL drvz_version(void);
DRVZ_API int DRVZ_CALL drvz_abi_version(void);

/* ----------------------------------------------------------------- context */

typedef struct drvz_context drvz_context;

typedef enum drvz_phase {
	DRVZ_PHASE_CONVERTING = 0,
	DRVZ_PHASE_HASHING = 1
} drvz_phase;

/*
 * Progress callback. fraction is 0.0..1.0. ratio is the running
 * output/input size ratio while writing WIA or RVZ (0.0..1.0+), otherwise 0.
 * Return non-zero to request cancellation.
 * Called on the thread that invoked the operation.
 */
typedef int (DRVZ_CALL *drvz_progress_fn)(void *user, drvz_phase phase, double fraction, double ratio);

typedef enum drvz_log_level {
	DRVZ_LOG_INFO = 0,
	DRVZ_LOG_WARNING = 1,
	DRVZ_LOG_ERROR = 2   /* the cause of a failure, reported as it happens */
} drvz_log_level;

/*
 * Log callback: the warnings DolphinTool prints (e.g. converting an NKit
 * image), and Dolphin's own warnings and errors. Every error is also added
 * to drvz_last_error() if the operation fails. Treat unknown levels as
 * DRVZ_LOG_WARNING. Called on the thread that invoked the operation.
 */
typedef void (DRVZ_CALL *drvz_log_fn)(void *user, drvz_log_level level, const char *message);

DRVZ_API drvz_status DRVZ_CALL drvz_context_create(drvz_context **out_ctx);
DRVZ_API void DRVZ_CALL drvz_context_destroy(drvz_context *ctx);

DRVZ_API void DRVZ_CALL drvz_context_set_progress(drvz_context *ctx, drvz_progress_fn fn, void *user);
DRVZ_API void DRVZ_CALL drvz_context_set_log(drvz_context *ctx, drvz_log_fn fn, void *user);

/* Limit compression worker threads for WIA/RVZ (0 = all processors). */
DRVZ_API void DRVZ_CALL drvz_context_set_num_threads(drvz_context *ctx, int count);

/*
 * Request cancellation of the operation running on ctx. Thread-safe.
 * If no operation is running yet, the request is kept and the next operation
 * on ctx fails with DRVZ_ERR_CANCELLED before doing any work. The request is
 * cleared when an operation finishes, however it ends. A cancelled conversion
 * deletes its partial output.
 */
DRVZ_API void DRVZ_CALL drvz_context_cancel(drvz_context *ctx);

/* Message for the last failed call on ctx ("" after success). Never NULL. */
DRVZ_API const char *DRVZ_CALL drvz_last_error(const drvz_context *ctx);

/* ------------------------------------------------------------------- enums */

typedef enum drvz_container {
	DRVZ_CONTAINER_PLAIN = 0,   /* .iso / .gcm (also split .part0.iso sets) */
	DRVZ_CONTAINER_GCZ = 1,
	DRVZ_CONTAINER_CISO = 2,
	DRVZ_CONTAINER_WBFS = 3,
	DRVZ_CONTAINER_TGC = 4,
	DRVZ_CONTAINER_WIA = 5,
	DRVZ_CONTAINER_RVZ = 6,
	DRVZ_CONTAINER_NFS = 7,
	DRVZ_CONTAINER_OTHER = 99
} drvz_container;

typedef enum drvz_platform {
	DRVZ_PLATFORM_GAMECUBE = 0,
	DRVZ_PLATFORM_WII = 1,
	DRVZ_PLATFORM_TRIFORCE = 2  /* arcade discs, GameCube-based */
} drvz_platform;

typedef enum drvz_compression {
	DRVZ_COMPRESSION_NONE = 0,
	DRVZ_COMPRESSION_PURGE = 1, /* WIA only */
	DRVZ_COMPRESSION_BZIP2 = 2,
	DRVZ_COMPRESSION_LZMA = 3,
	DRVZ_COMPRESSION_LZMA2 = 4,
	DRVZ_COMPRESSION_ZSTD = 5   /* RVZ only */
} drvz_compression;

typedef enum drvz_region {
	DRVZ_REGION_NTSC_J = 0,
	DRVZ_REGION_NTSC_U = 1,
	DRVZ_REGION_PAL = 2,
	DRVZ_REGION_UNKNOWN = 3,
	DRVZ_REGION_NTSC_K = 4,
	DRVZ_REGION_DEV = 5         /* Triforce */
} drvz_region;

/* ------------------------------------------------------------------- probe */

typedef struct drvz_disc_info {
	size_t struct_size;
	drvz_container container;
	drvz_platform platform;
	char game_id[8];             /* e.g. "GALE01", "" if none */
	char maker_id[4];            /* e.g. "01" */
	char internal_name[256];     /* from the disc header, UTF-8 */
	int disc_number;             /* 0 = disc 1 */
	int revision;
	drvz_region region;
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
} drvz_disc_info;

DRVZ_API void DRVZ_CALL drvz_disc_info_init(drvz_disc_info *info);
/* Fails with DRVZ_ERR_NOT_A_DISC when path is not a GameCube/Wii image. */
DRVZ_API drvz_status DRVZ_CALL drvz_probe(drvz_context *ctx, const char *path, drvz_disc_info *info);

/* ----------------------------------------------------------------- convert */

typedef enum drvz_format {
	DRVZ_FORMAT_ISO = 0,
	DRVZ_FORMAT_GCZ = 1,
	DRVZ_FORMAT_WIA = 2,
	DRVZ_FORMAT_RVZ = 3
} drvz_format;

/*
 * Defaults from drvz_convert_options_init() match Dolphin's convert dialog:
 * RVZ, Zstandard level 5, 128 KiB blocks, no scrubbing.
 */
typedef struct drvz_convert_options {
	size_t struct_size;
	const char *input;           /* required */
	const char *output;          /* required */
	drvz_format format;
	drvz_compression compression; /* WIA/RVZ only */
	int compression_level;       /* see drvz_compression_levels() */
	uint32_t block_size;         /* GCZ/WIA/RVZ, see drvz_block_size_valid() */
	int scrub;                   /* non-zero: zero out unused data (DolphinTool warns it saves little in RVZ) */
	int overwrite;               /* non-zero: replace an existing output */
} drvz_convert_options;

DRVZ_API void DRVZ_CALL drvz_convert_options_init(drvz_convert_options *opts);
DRVZ_API drvz_status DRVZ_CALL drvz_convert(drvz_context *ctx, const drvz_convert_options *opts);

/* Non-zero if block_size is allowed for format (ISO: always 0). */
DRVZ_API int DRVZ_CALL drvz_block_size_valid(drvz_format format, uint32_t block_size);
/* The levels Dolphin allows; *min = *max = 0 for NONE and PURGE. */
DRVZ_API void DRVZ_CALL drvz_compression_levels(drvz_compression compression, int *min, int *max);

/* -------------------------------------------------------------------- hash */

/*
 * Hashes the decoded disc (the bytes a plain ISO of it would contain), for
 * comparing against Redump/No-Intro DATs or checking a conversion.
 */
typedef struct drvz_hash_result {
	size_t struct_size;
	char crc32[9];               /* lowercase hex */
	char md5[33];
	char sha1[41];
	uint64_t size;               /* bytes hashed */
	int size_is_exact;           /* 0: the container (e.g. CISO, WBFS) only knows an
	                                upper bound, so the hashes may include padding */
} drvz_hash_result;

DRVZ_API void DRVZ_CALL drvz_hash_result_init(drvz_hash_result *result);
DRVZ_API drvz_status DRVZ_CALL drvz_hash(drvz_context *ctx, const char *path, drvz_hash_result *result);

#ifdef __cplusplus
}
#endif

#endif /* DOLPHINRVZ_H */
