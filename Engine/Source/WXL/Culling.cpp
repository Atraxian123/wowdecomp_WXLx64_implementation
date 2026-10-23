// WXL culling switches and trace. See Culling.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)

#include "Engine/Source/WXL/Culling.h"

#include "Engine/Source/WXL/Log.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace wxl::culling
{
    namespace
    {
        // WXLExtendedEquipment.ini sits beside the client executable (the file the equip modules and the log
        // read; this module lives in WXLCore, which cannot call their helper, so the lookup is repeated, as
        // Log.cpp does). Missing file, section or key: the default.
        bool IniPath(char* path, size_t cap)
        {
#ifdef _WIN32
            const DWORD n = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(cap));
            if (n == 0 || n >= cap) return false;
            char* slash = std::strrchr(path, '\\');
            const char* name = "WXLExtendedEquipment.ini";
            if (!slash || static_cast<size_t>(slash - path) + 1 + std::strlen(name) >= cap) return false;
            std::strcpy(slash + 1, name);
            return true;
#else
            (void)path; (void)cap;
            return false;
#endif
        }

        int IniInt(const char* section, const char* key, int def)
        {
#ifdef _WIN32
            char path[MAX_PATH] = {};
            if (!IniPath(path, sizeof(path))) return def;
            return static_cast<int>(GetPrivateProfileIntA(section, key, def, path));
#else
            (void)section; (void)key;
            return def;
#endif
        }

        std::string IniStr(const char* section, const char* key)
        {
#ifdef _WIN32
            char path[MAX_PATH] = {};
            if (!IniPath(path, sizeof(path))) return {};
            char out[256] = {};
            GetPrivateProfileStringA(section, key, "", out, sizeof(out), path);
            return out;
#else
            (void)section; (void)key;
            return {};
#endif
        }

        std::string Lower(std::string s)
        {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
            size_t b = 0;
            while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
            return s.substr(b);
        }

        struct Config
        {
            bool master = false;
            bool viewerWmo = false;
            bool wmoOccluders = false;
            bool volumes = false;
            bool horizon = false;
            bool wmoFrustum = false;
            bool sceneBox = false;
            bool portals = false;
            std::string traceWmo;     // lower case; empty = off

            Config()
            {
                master = IniInt("Culling", "DisableWmoCulling", 0) != 0;
                viewerWmo = master || IniInt("Culling", "DisableViewerWmoCull", 0) != 0;
                wmoOccluders = master || IniInt("Culling", "DisableWmoOccluders", 0) != 0;
                volumes = master || IniInt("Culling", "DisableOcclusionVolumes", 0) != 0;
                horizon = master || IniInt("Culling", "DisableHorizonBuffer", 0) != 0;
                wmoFrustum = IniInt("Culling", "DisableWmoFrustumCull", 0) != 0;
                sceneBox = IniInt("Culling", "DisableSceneBoxCull", 0) != 0;
                portals = IniInt("Culling", "DisablePortalCulling", 0) != 0;
                if (wxl::LogEnabled()) traceWmo = Lower(IniStr("Culling", "TraceWmo"));
                if (viewerWmo || wmoOccluders || volumes || horizon || wmoFrustum || sceneBox || portals)
                    wxl::LogCat("CULL", "culling: master=%d -> exterior kept when inside a WMO=%d, WMO occluders off=%d, occlusion volumes off=%d, horizon buffer off=%d | frustum test off=%d, scene box test off=%d, portal culling off=%d",
                                master ? 1 : 0, viewerWmo ? 1 : 0, wmoOccluders ? 1 : 0, volumes ? 1 : 0, horizon ? 1 : 0,
                                wmoFrustum ? 1 : 0, sceneBox ? 1 : 0, portals ? 1 : 0);
                else
                    wxl::LogCat("CULL", "culling: stock behaviour (WXLExtendedEquipment.ini [Culling] switches are all 0 or absent)");
                if (!traceWmo.empty())
                    wxl::LogCat("CULL", "trace: TraceWmo='%s'", traceWmo.c_str());
            }
        };

        // Built on first use; C++11 guarantees the construction happens once even if several threads race.
        const Config& Cfg()
        {
            static const Config c;
            return c;
        }

        std::atomic<const void*> g_lastKey{ nullptr };
        std::atomic<int> g_lastAction{ -1 };

        bool Matches(const std::string& filter, const char* path)
        {
            if (filter.empty() || !path) return false;
            if (filter == "*") return true;
            const size_t fl = filter.size();
            for (const char* p = path; *p; ++p)
            {
                size_t i = 0;
                while (i < fl && p[i] && std::tolower(static_cast<unsigned char>(p[i])) == static_cast<unsigned char>(filter[i])) ++i;
                if (i == fl) return true;
            }
            return false;
        }

        const char* FileName(const char* path)
        {
            if (!path) return "?";
            const char* s = std::strrchr(path, '\\');
            const char* t = std::strrchr(path, '/');
            if (t && (!s || t > s)) s = t;
            return s ? s + 1 : path;
        }

        const char* GroupText(GroupVerdict v)
        {
            switch (v)
            {
            case GroupVerdict::NotPrepared: return "not prepared yet (WMO instance not initialised)";
            case GroupVerdict::NotLoaded: return "group file not loaded yet";
            case GroupVerdict::Disabled: return "WMO instance flagged disabled (MAPOBJ_FLAG_DISABLED)";
            case GroupVerdict::OutsideSceneBox: return "outside the scene bounding box";
            case GroupVerdict::Queued: return "queued for exterior culling";
            case GroupVerdict::CulledFrustum: return "CULLED: view frustum";
            case GroupVerdict::CulledVolume: return "CULLED: inside an occlusion volume";
            case GroupVerdict::CulledBuffer: return "CULLED: behind the horizon buffer";
            case GroupVerdict::PassedStage1: return "passed exterior culling";
            case GroupVerdict::CulledPortalFrustum: return "CULLED: portal-view frustum (stage 2)";
            case GroupVerdict::CulledPortalBuffer: return "CULLED: behind the horizon buffer (stage 2)";
            case GroupVerdict::RenderedDirect: return "VISIBLE (rendered directly)";
            case GroupVerdict::RenderedViaPortals: return "VISIBLE (entered through its exterior portals)";
            case GroupVerdict::NotRenderedFromExterior: return "survived culling but its group flags give the exterior no way to render it";
            }
            return "?";
        }

        bool IsVisible(GroupVerdict v) { return v == GroupVerdict::RenderedDirect || v == GroupVerdict::RenderedViaPortals; }

        struct GroupRec
        {
            std::string wmo;
            unsigned num = 0;
            std::string name;
            GroupVerdict v = GroupVerdict::NotPrepared;
        };
        constexpr size_t kMaxTracked = 20000;
        std::mutex g_traceLock;
        std::unordered_map<const void*, GroupRec> g_groupCur, g_groupPrev;
        std::map<std::string, int> g_wmoState;   // 0 unknown, 1 visible, 2 fully hidden
    }

    bool KeepExteriorWhenInsideWmo() { return Cfg().viewerWmo; }
    bool DisableWmoOccluders() { return Cfg().wmoOccluders; }
    bool DisableOcclusionVolumes() { return Cfg().volumes; }
    bool DisableHorizonBuffer() { return Cfg().horizon; }
    bool DisableWmoFrustumCull() { return Cfg().wmoFrustum; }
    bool DisableSceneBoxCull() { return Cfg().sceneBox; }
    bool DisablePortalCulling() { return Cfg().portals; }

    bool ViewerChanged(const void* viewerKey, ExteriorAction action)
    {
        if (!wxl::LogEnabled()) return false;
        const int a = static_cast<int>(action);
        if (g_lastKey.load(std::memory_order_relaxed) == viewerKey && g_lastAction.load(std::memory_order_relaxed) == a)
            return false;
        g_lastKey.store(viewerKey, std::memory_order_relaxed);
        g_lastAction.store(a, std::memory_order_relaxed);
        return true;
    }

    void LogViewer(ExteriorAction action, const char* wmo, const char* groups, float x, float y, float z)
    {
        const char* what = "?";
        switch (action)
        {
        case ExteriorAction::Outdoors: what = "camera outdoors, exterior culled normally"; break;
        case ExteriorAction::PortalCulled: what = "inside a WMO, exterior culled through its portals"; break;
        case ExteriorAction::Cleared: what = "inside a WMO with NO way out: whole exterior (WMOs, terrain, M2 objects) DROPPED"; break;
        case ExteriorAction::KeptByToggle: what = "inside a WMO: exterior KEPT (DisableViewerWmoCull)"; break;
        }
        if (action == ExteriorAction::Outdoors)
            wxl::LogCat("CULL", "viewer: %s at (%.1f, %.1f, %.1f)", what, x, y, z);
        else
            wxl::LogCat("CULL", "viewer: %s | wmo '%s' groups %s | camera (%.1f, %.1f, %.1f)", what, wmo ? wmo : "?",
                        groups ? groups : "?", x, y, z);
    }

    bool TraceWmoWanted(const char* wmoPath)
    {
        const Config& c = Cfg();
        return !c.traceWmo.empty() && Matches(c.traceWmo, wmoPath);
    }

    void TraceGroup(const void* key, const char* wmoPath, unsigned groupNum, const char* groupName, GroupVerdict v)
    {
        if (!key) return;
        std::lock_guard<std::mutex> g(g_traceLock);
        if (g_groupCur.size() > kMaxTracked) return;
        GroupRec& r = g_groupCur[key];
        if (r.wmo.empty()) r.wmo = FileName(wmoPath);
        r.num = groupNum;
        if (r.name.empty() && groupName) r.name = groupName;
        r.v = v;   // later stages of the frame overwrite earlier ones: the last verdict is the reason
    }

    void TraceFlush(float camX, float camY, float camZ)
    {
        const Config& c = Cfg();
        if (c.traceWmo.empty()) return;
        std::lock_guard<std::mutex> g(g_traceLock);

        if (g_groupPrev.size() > kMaxTracked) { g_groupPrev.clear(); g_wmoState.clear(); }

        // changed groups
        for (const auto& kv : g_groupCur)
        {
            const GroupRec& now = kv.second;
            auto it = g_groupPrev.find(kv.first);
            if (it == g_groupPrev.end() || it->second.v != now.v)
            {
                if (it == g_groupPrev.end())
                    wxl::LogCat("CULL", "trace: wmo '%s' group #%u '%s' first seen: %s | camera (%.1f, %.1f, %.1f)", now.wmo.c_str(),
                                now.num, now.name.empty() ? "?" : now.name.c_str(), GroupText(now.v), camX, camY, camZ);
                else
                    wxl::LogCat("CULL", "trace: wmo '%s' group #%u '%s': %s -> %s | camera (%.1f, %.1f, %.1f)", now.wmo.c_str(),
                                now.num, now.name.empty() ? "?" : now.name.c_str(), GroupText(it->second.v), GroupText(now.v), camX, camY, camZ);
            }
            g_groupPrev[kv.first] = now;
        }
        g_groupCur.clear();

        // whole-WMO summary: did the last visible group of a traced WMO just go away (or the first come back)?
        struct Tally { int total = 0; int visible = 0; std::map<int, int> reasons; };
        std::map<std::string, Tally> byWmo;
        for (const auto& kv : g_groupPrev)
        {
            Tally& t = byWmo[kv.second.wmo];
            ++t.total;
            if (IsVisible(kv.second.v)) ++t.visible;
            else ++t.reasons[static_cast<int>(kv.second.v)];
        }
        for (const auto& kv : byWmo)
        {
            const int state = kv.second.visible > 0 ? 1 : 2;
            int& last = g_wmoState[kv.first];
            if (last == state) continue;
            const bool first = last == 0;
            last = state;
            if (state == 2)
            {
                std::string why;
                for (const auto& r : kv.second.reasons)
                {
                    char one[160];
                    std::snprintf(one, sizeof(one), "%s%dx %s", why.empty() ? "" : "; ", r.second, GroupText(static_cast<GroupVerdict>(r.first)));
                    why += one;
                }
                wxl::LogCat("CULL", "trace: WMO '%s' %s: ALL %d traced groups hidden [%s] | camera (%.1f, %.1f, %.1f)", kv.first.c_str(),
                            first ? "starts hidden" : "has just DISAPPEARED", kv.second.total, why.c_str(), camX, camY, camZ);
            }
            else if (!first)
            {
                wxl::LogCat("CULL", "trace: WMO '%s' is visible again (%d of %d groups) | camera (%.1f, %.1f, %.1f)", kv.first.c_str(),
                            kv.second.visible, kv.second.total, camX, camY, camZ);
            }
        }
    }
}
