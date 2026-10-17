// Minimal thread-safe log for in-tree WXL code (WXL.log next to the executable).
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
#pragma once

namespace wxl
{
    /// printf-style line appended to WXL.log in the working directory. Safe from loader threads.
    void Log(const char* format, ...);
}
