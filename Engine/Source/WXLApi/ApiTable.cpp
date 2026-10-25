// The table every module receives. See ApiHost.h and Engine/Source/WXL/Sdk/wxl_api.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// Every entry is a thin C-ABI face over something the core already has. Where a function takes or returns a
// C++ type (std::vector in the storage layer, bool in the ini helpers) this file is the only place that
// converts, so the types never reach a module.

#include "Engine/Source/WXLApi/ApiHost.h"

#include "Engine/Source/WXL/Event.h"
#include "Engine/Source/WXL/Log.h"
#include "Engine/Source/WXL/Storage.h"
#include "Engine/Source/WXLApi/ModelAccess.h"
#include "Engine/Source/WXLEquip/VirtualPath.hpp"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace wxl::api
{
    namespace
    {
        namespace eq = wxl::scripts::equipextension;

        void Log(const char* category, const char* message)
        {
            wxl::LogCat(category && *category ? category : "MODULE", "%s", message ? message : "");
        }

        int LogThrottle(const char* key, uint32_t first, uint32_t every)
        {
            return wxl::LogThrottle(key ? key : "", first, every) ? 1 : 0;
        }

        // Events are matched by name so a module survives the core adding or reordering events.
        int Subscribe(const char* eventName, WxlEventHandler handler, void* user)
        {
            if (!eventName || !handler) return 1;
            const uint32_t count = static_cast<uint32_t>(wxl::events::Event::Count);
            for (uint32_t i = 0; i < count; ++i)
            {
                const auto e = static_cast<wxl::events::Event>(i);
                if (std::strcmp(wxl::events::EventName(e), eventName) == 0)
                {
                    wxl::events::Subscribe(e, handler, user);   // same signature: void (*)(void*, const void*)
                    return 0;
                }
            }
            wxl::LogCat("MODULE", "subscribe: unknown event '%s'", eventName);
            return 2;
        }

        int IniGetString(const char* section, const char* key, char* out, size_t cap)
        {
            return eq::WxlIniGetString(section, key, out, cap) ? 1 : 0;
        }
        int IniGetInt(const char* section, const char* key, int defaultValue)
        {
            return eq::WxlIniGetInt(section, key, defaultValue);
        }
        int IniGetBool(const char* section, const char* key, int defaultValue)
        {
            return eq::WxlIniGetBool(section, key, defaultValue != 0) ? 1 : 0;
        }
        uint64_t IniStamp() { return eq::WxlIniStamp(); }

        int ReadFile(const char* name, WxlBuffer* out)
        {
            if (!name || !out) return 0;
            out->data = nullptr;
            out->size = 0;
            std::vector<uint8_t> bytes;
            if (!wxl::storage::ReadNativeQuiet(name, bytes) || bytes.empty()) return 0;
            void* copy = std::malloc(bytes.size());
            if (!copy) return 0;
            std::memcpy(copy, bytes.data(), bytes.size());
            out->data = copy;
            out->size = bytes.size();
            return 1;
        }

        void FreeBuffer(WxlBuffer* buffer)
        {
            if (!buffer) return;
            std::free(buffer->data);
            buffer->data = nullptr;
            buffer->size = 0;
        }

        int IsVirtualKey(const char* name) { return name && eq::VPathIsVirtualKey(name) ? 1 : 0; }

        const WxlApi g_table = {
            WXL_ABI_VERSION,
            sizeof(WxlApi),
            &Log,
            &LogThrottle,
            &Subscribe,
            &IniGetString,
            &IniGetInt,
            &IniGetBool,
            &IniStamp,
            &ReadFile,
            &FreeBuffer,
            &IsVirtualKey,
            &wxl::api::model::ReadInstance,
            &wxl::api::model::ReadBones,
            &wxl::api::model::WritePalette,
        };
    }

    const WxlApi* Table() { return &g_table; }
}
