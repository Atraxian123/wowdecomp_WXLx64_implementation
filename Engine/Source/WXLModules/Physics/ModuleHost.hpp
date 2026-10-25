// The module's side of the boundary: the API table, and the few helpers that wrap it.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// wxl::LogCat / wxl::LogThrottle are defined here for THIS module only (they are not exported), so code
// moved out of the client keeps calling them unchanged while they end up in the core's log through the API.
#pragma once

#include "wxl_api.h"

#include <cstdint>
#include <vector>

namespace wxl
{
    void LogCat(const char* category, const char* format, ...);
    bool LogThrottle(const char* key, unsigned first = 8, unsigned every = 1000);
}

namespace wxl::modules::host
{
    void SetApi(const WxlApi* api);
    const WxlApi* Api();

    bool     IniGetBool(const char* section, const char* key, bool defaultValue);
    int      IniGetInt(const char* section, const char* key, int defaultValue);
    bool     IniGetString(const char* section, const char* key, char* out, size_t cap);
    uint64_t IniStamp();

    /// A whole file through the client's own reader (quiet on a miss); false when it is not there.
    bool ReadFile(const char* name, std::vector<uint8_t>& out);
    bool IsVirtualKey(const char* name);
}
