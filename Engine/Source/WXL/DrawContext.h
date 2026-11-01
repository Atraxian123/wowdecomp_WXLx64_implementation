// Which M2 model the renderer is drawing right now, for the device layer's OnM2BatchDraw emit.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
//
// The M2 renderer knows the model but not the D3D draw parameters; the device's Draw() knows the parameters but
// not the model. CM2SceneRender::DrawBatch holds a ScopedM2Model across its Draw() calls, and the device reads
// CurrentM2Model() right after its native DrawIndexedPrimitive, which is enough to emit M2BatchDrawArgs.
// Single-threaded on purpose: both sides run on the render thread.
#pragma once

namespace wxl::events
{
    inline void*& CurrentM2ModelSlot()
    {
        static void* s_model = nullptr;
        return s_model;
    }

    /// The model being drawn, or null outside a DrawBatch.
    inline void* CurrentM2Model() { return CurrentM2ModelSlot(); }

    class ScopedM2Model
    {
    public:
        explicit ScopedM2Model(void* model) : m_prev(CurrentM2ModelSlot()) { CurrentM2ModelSlot() = model; }
        ~ScopedM2Model() { CurrentM2ModelSlot() = m_prev; }
        ScopedM2Model(const ScopedM2Model&)            = delete;
        ScopedM2Model& operator=(const ScopedM2Model&) = delete;

    private:
        void* m_prev;
    };
}
