// WXL log: one timestamped, thread-tagged, crash-durable file for every in-tree WXL module.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
//
// Output goes to WXL.log in the working directory (the client's folder), truncated at each launch.
// Every line is flushed as it is written, so the last line is the last thing that happened even when
// the client crashes.
//
// Logging is OPT-IN: nothing is written, and WXL.log is not even created, unless it is switched on in
// WXLExtendedEquipment.ini (beside the client executable):
//
//     [Log]
//     Enabled=1
//
// The environment variable WXL_LOG overrides the ini for one run: WXL_LOG=1 forces logging on,
// WXL_LOG=0 forces it off. The choice is made once, on the first line any module tries to write.
//
// Line format:  [HH:MM:SS.mmm] [T<thread id>] [CATEGORY] message
#pragma once

#include <cstdarg>

namespace wxl
{
    /// Whether logging is on for this run (decided once, on first use): env WXL_LOG, else the ini's
    /// [Log] Enabled, else off.
    bool LogEnabled();

    /// printf-style line in category "WXL".
    void Log(const char* format, ...);

    /// printf-style line in an explicit category (EQUIP, WEAPON, CREATURE, VPATH, STORAGE, BLP, EVENT ...).
    void LogCat(const char* category, const char* format, ...);

    /// The process working directory (where WXL.log is written and relative sidecar paths resolve).
    const char* WorkingDirectory();

    /// va_list form of LogCat, for modules that wrap it in their own printf-style helper.
    void LogCatV(const char* category, const char* format, va_list args);

    /**
     * @brief Rate limiter for lines that could fire every frame.
     * @param key    stable string identifying the call site (compared by content).
     * @param first  how many occurrences to allow before throttling starts.
     * @param every  after that, allow one occurrence per this many.
     * @return true when this occurrence should be logged.
     */
    bool LogThrottle(const char* key, unsigned first = 8, unsigned every = 1000);
}
