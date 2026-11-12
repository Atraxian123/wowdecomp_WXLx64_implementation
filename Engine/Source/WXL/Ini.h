// WXLSetup.ini: the one place that finds it, reads it, notices that it changed, and lets the console override a value
// for the session. Everything in WXLCore, WXLEquip and (through the module API) every module reads its settings here.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
//
// Where the file is: <client folder>\WXL\WXLSetup.ini, or the copy beside the executable (the old place) when the WXL
// one does not exist. The environment variable WXL_INI, when set, names a file to use instead (for testing).
//
// Reading: a missing file, section or key gives the caller's default, never an error. A value overridden for the
// session (Set) wins over the file.
//
// Changing it live: Generation() goes up when the file is saved by anything, and when Set / Unset / ClearOverrides /
// Save change what a read returns. A module that caches settings keeps the number it read them at and re-reads when
// it differs:
//
//     static uint32_t seen = ~0u;  static Config cfg;
//     if (wxl::ini::Generation() != seen) { seen = wxl::ini::Generation(); cfg = ReadConfig(); }
//
// Generation() is cheap (an atomic load and, at most twice a second, one file-time query), so calling it every frame
// is fine. Stamp() is the same signal as a 64-bit number for callers that compare it (the module API's iniStamp).
//
// This file never logs: Log.cpp reads its own switches through it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wxl::ini
{
    /// The file in use ("" when it cannot be located). Valid until the next call.
    const char* Path();

    int         GetInt(const char* section, const char* key, int defaultValue);
    bool        GetBool(const char* section, const char* key, bool defaultValue);
    /// A decimal number written 1.25 or 1,25.
    float       GetFloat(const char* section, const char* key, float defaultValue);
    /// True when the key exists (even with an empty value). *out is "" otherwise.
    bool        GetString(const char* section, const char* key, char* out, size_t cap);
    std::string GetText(const char* section, const char* key, const char* defaultValue = "");

    uint32_t    Generation();
    uint64_t    Stamp();   // the file's last-write time with Generation() added: changes on a save and on an override

    // ---- overrides: live toggles that do not touch the file ----------------------------------------------------
    struct Entry
    {
        std::string section, key, value;
    };

    /// Makes reads of section/key return value until Unset / ClearOverrides / Save. False for an empty section or key.
    bool               Set(const char* section, const char* key, const char* value);
    bool               Unset(const char* section, const char* key);
    void               ClearOverrides();
    std::vector<Entry> Overrides();
    /// Writes every override into the file and drops them. False if the file could not be written (nothing is lost:
    /// the overrides stay).
    bool               Save();

    // ---- listing, for the console ----------------------------------------------------------------------------
    std::vector<std::string>                       Sections();
    /// The keys of a section as the file has them, overrides applied, in file order (keys that exist only as an
    /// override are appended).
    std::vector<std::pair<std::string, std::string>> Section(const char* section);
}
