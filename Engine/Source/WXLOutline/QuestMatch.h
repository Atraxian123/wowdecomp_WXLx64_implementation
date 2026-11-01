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
        /// A kill / interact objective still short of its count. The quest cache stores a creature as its entry id
        /// and a game object as the negated entry id (0 means no objective).
        void AddObjective(int32_t creatureOrGameObject)
        {
            if (creatureOrGameObject != 0) m_objectives.push_back(creatureOrGameObject);
        }

        /// A quest item still short of the count the quest wants.
        void AddItem(uint32_t itemId)
        {
            if (itemId != 0) m_items.push_back(itemId);
        }

        bool Empty() const { return m_objectives.empty() && m_items.empty(); }

        /// A creature counts for an open objective under its own entry or under one of its kill-credit entries.
        bool NeedsCreature(uint32_t entry, const uint32_t* killCredit, size_t killCreditCount) const
        {
            if (HasObjective(static_cast<int32_t>(entry))) return true;
            for (size_t i = 0; killCredit && i < killCreditCount; ++i)
                if (killCredit[i] && HasObjective(static_cast<int32_t>(killCredit[i]))) return true;
            return false;
        }

        bool NeedsGameObject(uint32_t entry) const
        {
            return entry != 0 && HasObjective(-static_cast<int32_t>(entry));
        }

        /// Whether any of the quest items a creature or game object can hold is one a quest still wants.
        bool NeedsItemFrom(const uint32_t* questItems, size_t count) const
        {
            for (size_t i = 0; questItems && i < count; ++i)
                if (questItems[i] && std::find(m_items.begin(), m_items.end(), questItems[i]) != m_items.end())
                    return true;
            return false;
        }

    private:
        bool HasObjective(int32_t id) const
        {
            return id != 0 && std::find(m_objectives.begin(), m_objectives.end(), id) != m_objectives.end();
        }

        std::vector<int32_t>  m_objectives;
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
