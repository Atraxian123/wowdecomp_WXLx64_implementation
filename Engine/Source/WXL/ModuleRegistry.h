// What modules register with the core: virtual file providers and virtual-key queries. (Services live in Services.h.)
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// Engine-free on purpose (it needs the storage layer's provider hook and the log, nothing else): it lives in WXLCore so
// WXLEquip can reach it, and it can be built and tested without the client. WXLApi/ApiTable.cpp puts a C-ABI face on it.
#pragma once

#include "Engine/Source/WXL/Sdk/wxl_api.h"

namespace wxl::registry
{
    /// Adds a module's file provider. The first one registered also installs the single core provider
    /// (wxl::storage::RegisterProvider) that fans a file open out to every module provider, in registration
    /// order. Meant for WxlModuleInit time: reads are lock-free, so registering while the client is already
    /// opening files from loader threads is allowed but the new provider only becomes visible afterwards.
    /// @return 0 on success, 1 for a null function, 2 when the table of providers is full.
    int RegisterFileProvider(WxlFileProviderFn fn, void* user);

    /// @return 0 on success, 1 for a null function, 2 when the table is full.
    int RegisterVirtualKeyQuery(WxlVirtualKeyFn fn, void* user);

    /// 1 when any registered query claims the name.
    int IsVirtualKey(const char* name);
}
