#include "Engine/Source/WXLEquip/Equip.h"

#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/WXL/Storage.h"
#include "Engine/Source/WXLEquip/CreatureExtension.hpp"
#include "Engine/Source/WXLEquip/EquipExtension.hpp"
#include "Engine/Source/WXLEquip/WeaponExtension.hpp"
#include "Engine/Source/WXLEquip/VirtualPath.hpp"

#include <cstdint>
#include <vector>

namespace wxl::equip
{
    namespace
    {
        // Reads a whole file through the client's own open/size/read/close, the same calls the game uses.
        bool ReadThroughSFile(const char* name, std::vector<uint8_t>& out)
        {
            SFileImpl* file = nullptr;
            if (!SFile::OpenEx(nullptr, name, 0, &file) || !file) return false;

            uint32_t sizeHigh = 0;
            const uint32_t size = SFileGetFileSize(file, &sizeHigh);
            bool ok = false;

            if (size > 0 && sizeHigh == 0)
            {
                out.resize(size);
                uint32_t got = 0;
                ok = SFileReadFile(file, out.data(), size, &got, nullptr, 0) && got == size;
                if (!ok) out.clear();
            }

            SFileCloseFile(file);
            return ok;
        }
    }

    void Init()
    {
        wxl::storage::SetNativeReader(&ReadThroughSFile);
        wxl::scripts::equipextension::VPathRegister();
        wxl::scripts::equipextension::Create();
        wxl::scripts::weaponextension::Create();
        wxl::scripts::creatureextension::Create();
    }
}
