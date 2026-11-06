// What the core knows about the units behind model instances: for now, which of them are swimming.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
//
// The client decides that a unit swims in CGUnit_C::UpdateSwimmingStatus (the movement flag, or the client-side
// swim bit that is set as soon as the unit is deep enough). That function reports the result here, together with
// the unit's own model instances (its world model, and its mount model while mounted). Nothing else reads the
// client: IsSwimming answers from this table, on any thread, so the physics module can ask "is this model
// underwater?" from an animate thread without touching the unit or the map.
//
// Only swimming units are kept. A unit stops being listed when it reports it is not swimming, and when it is
// destroyed (ForgetUnit), so a freed model address is never mistaken for a swimming one.
#pragma once

namespace wxl::unitstate
{
    /// Called by the unit every time its swimming status is evaluated. `unit` is only an identity (never
    /// dereferenced); the two model pointers are the unit's world model and mount model (either may be null).
    void NoteUnit(const void* unit, const void* worldModel, const void* mountModel, bool swimming);

    /// The unit is gone (destructor): forget it.
    void ForgetUnit(const void* unit);

    /// True when `model` is the world model or the mount model of a unit that is swimming. Thread-safe.
    bool IsSwimming(const void* model);
}
