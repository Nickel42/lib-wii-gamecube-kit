// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// libdolphinrvz stand-in for Dolphin's Core/IOS/Device.h. The ticket, TMD and
// IOSC code only needs the IOS return codes, not the emulated IOS devices, so
// this header keeps just that enum (copied from Device.h unchanged).

#pragma once

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/IOS/Uids.h"

namespace IOS::HLE
{
enum ReturnCode : s32
{
  IPC_SUCCESS = 0,         // Success
  IPC_EACCES = -1,         // Permission denied
  IPC_EEXIST = -2,         // File exists
  IPC_STALL = -3,          // Fail stall
  IPC_EINVAL = -4,         // Invalid argument or fd
  IPC_EMAX = -5,           // Too many file descriptors open
  IPC_ENOENT = -6,         // File not found
  IPC_EQUEUEFULL = -8,     // Queue full
  IPC_UNKNOWN = -9,        // Unknown
  IPC_EIO = -12,           // ECC error
  IPC_ENOMEM = -22,        // Alloc failed during request
  FS_EINVAL = -101,        // Invalid path
  FS_EACCESS = -102,       // Permission denied
  FS_ECORRUPT = -103,      // Corrupted NAND
  FS_EEXIST = -105,        // File exists
  FS_ENOENT = -106,        // No such file or directory
  FS_ENFILE = -107,        // Too many fds open
  FS_EFBIG = -108,         // Max block count reached?
  FS_EFDEXHAUSTED = -109,  // Too many fds open
  FS_ENAMELEN = -110,      // Pathname is too long
  FS_EFDOPEN = -111,       // FD is already open
  FS_EIO = -114,           // ECC error
  FS_ENOTEMPTY = -115,     // Directory not empty
  FS_EDIRDEPTH = -116,     // Max directory depth exceeded
  FS_EBUSY = -118,         // Resource busy
  ES_SHORT_READ = -1009,   // Short read
  ES_EIO = -1010,          // Write failure
  ES_INVALID_SIGNATURE_TYPE = -1012,
  ES_FD_EXHAUSTED = -1016,  // Max of 3 ES handles exceeded
  ES_EINVAL = -1017,        // Invalid argument
  ES_DEVICE_ID_MISMATCH = -1020,
  ES_HASH_MISMATCH = -1022,  // Decrypted content hash doesn't match with the hash from the TMD
  ES_ENOMEM = -1024,         // Alloc failed during request
  ES_EACCES = -1026,         // Incorrect access rights (according to TMD)
  ES_UNKNOWN_ISSUER = -1027,
  ES_NO_TICKET = -1028,
  ES_INVALID_TICKET = -1029,
  IOSC_EACCES = -2000,
  IOSC_EEXIST = -2001,
  IOSC_EINVAL = -2002,
  IOSC_EMAX = -2003,
  IOSC_ENOENT = -2004,
  IOSC_INVALID_OBJTYPE = -2005,
  IOSC_INVALID_RNG = -2006,
  IOSC_INVALID_FLAG = -2007,
  IOSC_INVALID_FORMAT = -2008,
  IOSC_INVALID_VERSION = -2009,
  IOSC_INVALID_SIGNER = -2010,
  IOSC_FAIL_CHECKVALUE = -2011,
  IOSC_FAIL_INTERNAL = -2012,
  IOSC_FAIL_ALLOC = -2013,
  IOSC_INVALID_SIZE = -2014,
  IOSC_INVALID_ADDR = -2015,
  IOSC_INVALID_ALIGN = -2016,
  USB_ECANCELED = -7022,  // USB OH0 insertion hook cancelled
};
}  // namespace IOS::HLE
