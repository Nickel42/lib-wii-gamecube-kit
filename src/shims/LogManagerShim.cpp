// SPDX-License-Identifier: GPL-2.0-or-later

// Stand-in for Dolphin's Common/Logging/LogManager.cpp. Dolphin's log manager
// pulls in its config system and log windows; libdolphinrvz only forwards
// warnings and errors to the calling context's log callback.

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "drvz_internal.h"

namespace Common::Log
{
void GenericLogFmtImpl(LogLevel level, LogType type, const char* file, int line,
                       fmt::string_view format, const fmt::format_args& args)
{
  (void)type;
  (void)file;
  (void)line;
  drvz_log_level out;
  switch (level)
  {
  case LogLevel::LERROR:
    out = DRVZ_LOG_ERROR;
    break;
  case LogLevel::LWARNING:
    out = DRVZ_LOG_WARNING;
    break;
  case LogLevel::LNOTICE:
  case LogLevel::LINFO:
    out = DRVZ_LOG_INFO;
    break;
  default:
    return;
  }
  drvz::Log(out, fmt::vformat(format, args));
}
}  // namespace Common::Log
