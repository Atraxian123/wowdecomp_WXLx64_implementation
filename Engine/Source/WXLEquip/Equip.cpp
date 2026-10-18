#include "Engine/Source/WXLEquip/Equip.h"

#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/WXL/Event.h"
#include "Engine/Source/WXL/Log.h"
#include "Engine/Source/WXL/Storage.h"
#include "Engine/Source/WXLEquip/CreatureExtension.hpp"
#include "Engine/Source/WXLEquip/EquipExtension.hpp"
#include "Engine/Source/WXLEquip/VirtualPath.hpp"
#include "Engine/Source/WXLEquip/WeaponExtension.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace wxl::equip
{
    namespace
    {
        // Reads a whole file through the client's own open/size/read/close, the same calls the game uses.
        // Tries archives only first (the injected build's behaviour), then archives plus loose files under
        // Data\ (open flag 1), so a sidecar dropped loose into the client folder is found too.
        bool ReadThroughSFile(const char* name, std::vector<uint8_t>& out)
        {
            for (uint32_t flags = 0; flags <= 1; ++flags)
            {
                SFileImpl* file = nullptr;
                if (!SFile::OpenEx(nullptr, name, flags, &file) || !file) continue;

                uint32_t sizeHigh = 0;
                const uint32_t size = SFileGetFileSize(file, &sizeHigh);
                bool ok = false;

                if (size > 0 && sizeHigh == 0)
                {
                    out.resize(size);
                    uint32_t got = 0;
                    ok = SFileReadFile(file, out.data(), size, &got, nullptr, 0) && got == size;
                    if (!ok)
                    {
                        wxl::LogCat("EQUIP", "native read of '%s' opened (flags=%u) but read failed: size=%u got=%u",
                                    name, flags, size, got);
                        out.clear();
                    }
                }
                else
                {
                    wxl::LogCat("EQUIP", "native read of '%s' opened (flags=%u) but size is %u (high=%u): treated as empty",
                                name, flags, size, sizeHigh);
                }

                SFileCloseFile(file);
                if (ok) return true;
            }
            return false;
        }

        // Where do the sidecars live right now? Answers "does it actually see my CSVs" before any model
        // loads. Only the working directory can be checked this early: game archives are not mounted yet.
        void ReportSidecarsOnDisk()
        {
            static const char* const kNames[] = {
                "WXLItemDisplayModels.csv", "WXLItemDisplayModelMaterials.csv",
                "WXLWeaponModels.csv",      "WXLWeaponTextures.csv",
                "WXLCreatureModels.csv",    "WXLCreatureTextures.csv",
                "WXLHelmOffsets.csv",
            };
            wxl::LogCat("SIDECAR", "working directory: %s", wxl::WorkingDirectory());
            for (const char* name : kNames)
            {
                char path[512];
                std::snprintf(path, sizeof(path), "%s", name);
                bool found = false;
                for (int pass = 0; pass < 2 && !found; ++pass)
                {
                    if (pass == 1) std::snprintf(path, sizeof(path), "DBFilesClient\\%s", name);
                    if (FILE* f = std::fopen(path, "rb"))
                    {
                        std::fseek(f, 0, SEEK_END);
                        const long size = std::ftell(f);
                        std::fclose(f);
                        wxl::LogCat("SIDECAR", "  %-34s found on disk as '%s' (%ld bytes)", name, path, size);
                        found = true;
                    }
                }
                if (!found)
                    wxl::LogCat("SIDECAR", "  %-34s not in the working directory (it may still be inside a game archive or a Data\\*.MPQ folder: see the per-file lines at first model load)", name);
            }
        }
    }

    void Init()
    {
        wxl::LogCat("EQUIP", "Init: installing native reader, virtual path provider and extensions");
        wxl::storage::SetNativeReader(&ReadThroughSFile);
        wxl::scripts::equipextension::VPathRegister();
        wxl::scripts::equipextension::Create();
        wxl::scripts::weaponextension::Create();
        wxl::scripts::creatureextension::Create();
        ReportSidecarsOnDisk();

        // One glance at which events have handlers; "fired" counts follow once the game reaches each hook.
        wxl::events::DumpStats();
    }
}
