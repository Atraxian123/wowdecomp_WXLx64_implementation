// Test glue: binds the equip module's view of the module API (wxl_module.hpp) to the table the test builds.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
#include "wxl_module.hpp"

void BindEquipModuleApi(const WxlApi* api)
{
    wxl::module::Bind(api);
}
