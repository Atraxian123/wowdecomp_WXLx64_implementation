// WXL: "<root>_csN" WMO groups, see MapObjClusters.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)

#include "WoW/Source/WorldClient/MapObjClusters.h"

#include <cctype>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Engine/Source/WXL/Ini.h"
#include "Engine/Source/WXL/Log.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "WoW/Source/WorldClient/MapObjGroup.h"

namespace wxl::wmocs
{
    namespace
    {
        struct State
        {
            uint64_t signature = ~0ull;                 // which groups were loaded when the table was built
            std::vector<uint16_t> root;                 // per group: its cluster's root (itself when none)
            std::vector<std::vector<uint16_t>> members; // per root with a cluster: the root, then its _csN groups
        };

        std::unordered_map<const CMapObj*, std::unique_ptr<State>> s_states;

        bool Enabled()
        {
            return wxl::ini::GetBool("WMO", "GroupClusters", true);
        }

        std::string Lower(const char* s)
        {
            std::string out(s);

            for (char& c : out) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }

            return out;
        }

        // "name_cs12" -> "name"; empty when the name does not end in "_cs<digits>" (or has nothing in front of it)
        std::string BaseOfCsName(const std::string& name)
        {
            size_t end = name.size();
            size_t digits = end;

            while (digits > 0 && std::isdigit(static_cast<unsigned char>(name[digits - 1]))) {
                digits--;
            }

            if (digits == end || digits < 4 || name.compare(digits - 3, 3, "_cs") != 0) {
                return std::string();
            }

            return name.substr(0, digits - 3);
        }
    }

    void Refresh(CMapObj* mapObj)
    {
        if (!Enabled() || !mapObj->m_isGroupLoaded || mapObj->m_groupInfoCount <= 0) {
            return;
        }

        const uint32_t count = static_cast<uint32_t>(mapObj->m_groupInfoCount);
        std::vector<const char*> names(count, nullptr);
        uint64_t signature = 1469598103934665603ull;

        for (uint32_t i = 0; i < count; i++) {
            const CMapObjGroup* group = mapObj->GetGroup(static_cast<int32_t>(i), 0);

            if (group && group->m_groupName) {
                names[i] = group->m_groupName;
                signature = (signature ^ (i + 1)) * 1099511628211ull;
            }
        }

        std::unique_ptr<State>& slot = s_states[mapObj];

        if (slot && slot->signature == signature) {
            return;
        }

        if (!slot) {
            slot = std::make_unique<State>();
        }

        State& st = *slot;
        st.signature = signature;
        st.root.resize(count);
        st.members.assign(count, std::vector<uint16_t>());

        std::unordered_map<std::string, uint32_t> byName; // first group of each name

        for (uint32_t i = 0; i < count; i++) {
            st.root[i] = static_cast<uint16_t>(i);

            if (names[i]) {
                byName.emplace(Lower(names[i]), i);
            }
        }

        for (uint32_t i = 0; i < count; i++) {
            if (!names[i]) {
                continue;
            }

            const std::string base = BaseOfCsName(Lower(names[i]));

            if (base.empty()) {
                continue;
            }

            const auto it = byName.find(base);

            if (it == byName.end() || it->second == i) {
                continue; // the root is not loaded (yet): the group stays on its own until it is
            }

            st.root[i] = static_cast<uint16_t>(it->second);
            std::vector<uint16_t>& m = st.members[it->second];

            if (m.empty()) {
                m.push_back(static_cast<uint16_t>(it->second));
            }

            m.push_back(static_cast<uint16_t>(i));
        }
    }

    void Unload(CMapObj* mapObj)
    {
        s_states.erase(mapObj);
    }

    uint32_t RootOf(const CMapObj* mapObj, uint32_t group)
    {
        if (s_states.empty()) {
            return group;
        }

        const auto it = s_states.find(mapObj);

        if (it == s_states.end() || group >= it->second->root.size()) {
            return group;
        }

        return it->second->root[group];
    }

    uint32_t MemberCount(const CMapObj* mapObj, uint32_t root)
    {
        if (s_states.empty()) {
            return 1;
        }

        const auto it = s_states.find(mapObj);

        if (it == s_states.end() || root >= it->second->members.size() || it->second->members[root].empty()) {
            return 1;
        }

        return static_cast<uint32_t>(it->second->members[root].size());
    }

    uint32_t Member(const CMapObj* mapObj, uint32_t root, uint32_t i)
    {
        const auto it = s_states.find(mapObj);

        if (i == 0 || it == s_states.end() || root >= it->second->members.size() || i >= it->second->members[root].size()) {
            return root;
        }

        return it->second->members[root][i];
    }
}
