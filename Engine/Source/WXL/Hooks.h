// Helpers for placing WXL hook points inside decompiled functions.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
#pragma once

#include "Engine/Source/WXL/Event.h"

namespace wxl::events
{
    /**
     * @brief Emits an event when it goes out of scope, i.e. after the surrounding function has run
     *        and on every return path.
     *
     * The original WXL detours ran the native function and then emitted ("call original, then emit").
     * In source there is no wrapper to put the emit after, so the guard is declared on the first line
     * of the function and fires when the function exits, early returns included.
     *
     * Cheap on the hot path: nothing is emitted unless the event has a subscriber.
     */
    template <class Args>
    class EmitOnExit
    {
    public:
        EmitOnExit(Event e, const Args& args) : m_event(e), m_args(args) {}
        ~EmitOnExit()
        {
            if (Any(m_event)) Emit(m_event, &m_args);
        }
        EmitOnExit(const EmitOnExit&)            = delete;
        EmitOnExit& operator=(const EmitOnExit&) = delete;

    private:
        Event m_event;
        Args  m_args;
    };

    /// Emits at the call site, before the rest of the function. Skips the work when nothing listens.
    template <class Args>
    inline void EmitIfAny(Event e, const Args& args)
    {
        if (Any(e)) Emit(e, &args);
    }
}
