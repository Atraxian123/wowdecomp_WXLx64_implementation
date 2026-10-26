// The module's side of the boundary: the API table and the few helpers that wrap it.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
#pragma once

#include "wxl_api.h"

#include <string>

namespace wxl::modules::host
{
    void SetApi(const WxlApi* api);
    const WxlApi* Api();

    /// One finished line in the client's log, category LISTFILE.
    void Log(const char* line);

    bool        IniGetBool(const char* section, const char* key, bool defaultValue);
    std::string IniGetString(const char* section, const char* key, const char* defaultValue);

    /// Can the client open this file (archives, loose files under Data\, virtual files)?
    bool FileExists(const char* name);

    /// The folder the client runs from, UTF-8, with a trailing backslash ("" when it cannot be told).
    std::string ClientFolder();
}
