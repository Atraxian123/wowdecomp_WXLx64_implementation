// The spell area module's entry points. See Engine/Source/WXL/Sdk/wxl_api.h and SpellArea.md.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "wxl_module.hpp"

#include <cstring>

namespace wxl::spellarea
{
    bool Start();   // SpellArea.cpp: registers the render callback; reads [SpellArea] now and whenever the ini is saved
    void Stop();    // unregisters the render callback
}

WXL_DEFINE_MODULE_ABI()

WXL_MODULE_API int WxlModuleInit(const WxlApi* api, WxlModuleInfo* info)
{
    // spellAreaTriggerAuras is the last member of the engine access added for this module: its presence means all of
    // them are.
    if (!api || api->abiVersion != WXL_ABI_VERSION || !WXL_API_HAS(api, spellAreaTriggerAuras)) return 1;
    wxl::module::Bind(api);
    if (info)
    {
        std::strncpy(info->name, "spellarea", sizeof(info->name) - 1);
        info->versionMajor = 1;
        info->versionMinor = 0;
    }
    // Stays loaded with [SpellArea] Enabled=0 (it is the default): saving the ini with Enabled=1 turns the indicator on
    // without a restart, as it always did, and while it is off a frame costs one integer compare.
    return wxl::spellarea::Start() ? 0 : 1;
}

WXL_MODULE_API void WxlModuleShutdown(void)
{
    wxl::spellarea::Stop();
}
