// Pure matching rules for the quest outline: "does this creature / object matter to my open quests".
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// Engine-free on purpose, so a host test can run it. Outline.cpp fills a QuestNeeds from the quest log and the
// quest cache (what is still missing), then asks it about each visible creature and object using what the client's
// own caches say about them (entry id, kill credits, the quest items they can hold).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wxl::outline
{
    class QuestNeeds
    {
    public:
        /// A kill / interact objective still short of its count, as the quest cache stores it: the creature entry
        /// itself, or a game object entry with the sign bit set (the client reads it back as `objective & 0x7FFFFFFF`).
        /// 0 means no objective.
        void AddObjective(uint32_t raw)
        {
            if (raw == 0) return;
            if (raw & 0x80000000u) m_gameObjects.push_back(raw & 0x7FFFFFFFu);
            else                   m_creatures.push_back(raw);
        }

        /// A quest item still short of the count the quest wants.
        void AddItem(uint32_t itemId)
        {
            if (itemId != 0) m_items.push_back(itemId);
        }

        bool Empty() const { return m_creatures.empty() && m_gameObjects.empty() && m_items.empty(); }

        /// A creature counts for an open objective under its own entry or under one of its kill-credit entries.
        bool NeedsCreature(uint32_t entry, const uint32_t* killCredit, size_t killCreditCount) const
        {
            if (Has(m_creatures, entry)) return true;
            for (size_t i = 0; killCredit && i < killCreditCount; ++i)
                if (Has(m_creatures, killCredit[i])) return true;
            return false;
        }

        bool NeedsGameObject(uint32_t entry) const { return Has(m_gameObjects, entry); }

        /// Whether any of the quest items a creature or game object can hold is one a quest still wants.
        bool NeedsItemFrom(const uint32_t* questItems, size_t count) const
        {
            for (size_t i = 0; questItems && i < count; ++i)
                if (Has(m_items, questItems[i])) return true;
            return false;
        }

    private:
        static bool Has(const std::vector<uint32_t>& v, uint32_t id)
        {
            return id != 0 && std::find(v.begin(), v.end(), id) != v.end();
        }

        std::vector<uint32_t> m_creatures;
        std::vector<uint32_t> m_gameObjects;
        std::vector<uint32_t> m_items;
    };

    /// 0xRRGGBB (or the bare hex digits) into the 0..1 floats the mask shader takes. False on anything else.
    inline bool ParseHexColor(const char* text, float* rgb)
    {
        if (!text) return false;
        if (text[0] == '#') ++text;
        else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;

        uint32_t v = 0;
        int digits = 0;
        for (; *text; ++text, ++digits)
        {
            const char c = *text;
            uint32_t d;
            if (c >= '0' && c <= '9')      d = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
            else return false;
            v = (v << 4) | d;
        }
        if (digits != 6) return false;
        rgb[0] = static_cast<float>((v >> 16) & 0xFF) / 255.0f;
        rgb[1] = static_cast<float>((v >> 8) & 0xFF) / 255.0f;
        rgb[2] = static_cast<float>(v & 0xFF) / 255.0f;
        return true;
    }
}
