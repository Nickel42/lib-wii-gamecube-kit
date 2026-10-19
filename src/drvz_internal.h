// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "dolphinrvz.h"

namespace drvz
{
// Both act on the context whose operation is running on this thread.
// Messages from other threads (Dolphin's compression workers) have no context
// and are dropped; Dolphin reports their failures again on the calling thread.

// Sends a line to the context's log callback.
void Log(drvz_log_level level, const std::string& message);
// Keeps a line for drvz_last_error() if the operation fails.
void AddError(const std::string& message);
}  // namespace drvz
