// The calls the client makes for the spell area indicator. See SpellAreaHost.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// Kept apart from SpellAreaHost.h so the client's UI code can include it without the module API header (which refuses
// a 32-bit build unless told it only declares).
#pragma once

#include <cstdint>

namespace wxl::api::spellareahost
{
    /// Called by the world render beside the client's own spell cursor shadow, once after the opaque pass with
    /// (0x200122, 5) and once after the transparent ones with (0x20000, 4): the ProjectTex triangle flags and render
    /// flags of those two calls (the second leaves the doodads out so nothing is drawn twice). Calls the spell area
    /// module's renderer when there is one; does nothing when that module is not loaded (or declined to load).
    void Render(uint32_t triFlags, uint32_t projectFlags);

    /// Called when a spell-go packet has been read (PendingSpellCast::OnSpellGo): the caster, the spell, its cast flags
    /// and what it was aimed at. `dest` is the ground point (three floats) or null. Kept for the spell area module's
    /// instant-cast indicator and the triggered spells (`delayMs` is the missile's flight time when the packet had one); costs a few stores when nobody asks.
    void NoteSpellGo(uint64_t caster, uint32_t spellId, uint32_t castFlags, uint32_t targetFlags, const float* dest, uint64_t objectTarget, uint32_t delayMs);
}
