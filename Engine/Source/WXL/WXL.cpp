#include "Engine/Source/WXL/WXL.h"
#include "Engine/Source/WXL/Crash.h"
#include "Engine/Source/WXL/Log.h"

namespace wxl
{
    void InitModules()
    {
        wxl::Log("InitModules: WXL core starting (event bus, storage layer, BLP transform)");
        wxl::InstallCrashLogger();
        // In-tree modules are constructed here, e.g. equip extension:
        //     static wxl::scripts::equipextension::EquipExtension s_equip;
    }
}
