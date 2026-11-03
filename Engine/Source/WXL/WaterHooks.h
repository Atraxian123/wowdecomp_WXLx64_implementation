// Engine call sites for the modern water module (Engine/Source/WXLWater).
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
//
// The water module needs five moments of the world frame that the event bus does not name: the opaque M2 pass
// finished, the transparent liquid pass starting and ending, each water material draw, and the end of the world
// render. The client calls the functions below at those places. They do nothing until the module fills the
// table, so a client without the module (or with it switched off) pays one null check per call.
//
// Single-threaded on purpose: every call comes from the render thread.
#pragma once

namespace wxl::waterhooks
{
    struct Callbacks
    {
        void (*opaqueDone)()                    = nullptr; // after scene->Draw(M2PASS_0)
        void (*passBegin)(const void* renderer) = nullptr; // before Liquid::CRenderer::Flush(pass 1)
        void (*passEnd)()                       = nullptr; // after it
        int  (*tagDraw)(const void* settings)   = nullptr; // a water material is about to draw; non-zero = tagged
        void (*untagDraw)()                     = nullptr; // and it has drawn
        void (*frameEnd)()                      = nullptr; // the world render is over
    };

    inline Callbacks& Table()
    {
        static Callbacks s_table;
        return s_table;
    }

    inline void OpaqueDone()                      { if (auto f = Table().opaqueDone) f(); }
    inline void PassBegin(const void* renderer)   { if (auto f = Table().passBegin) f(renderer); }
    inline void PassEnd()                         { if (auto f = Table().passEnd) f(); }
    inline void FrameEnd()                        { if (auto f = Table().frameEnd) f(); }

    /// Brackets one water material draw: tags it on entry, untags it on every way out of the function.
    class ScopedDrawTag
    {
    public:
        explicit ScopedDrawTag(const void* settings)
            : m_tagged(Table().tagDraw ? Table().tagDraw(settings) != 0 : false)
        {
        }
        ~ScopedDrawTag()
        {
            if (m_tagged && Table().untagDraw)
                Table().untagDraw();
        }
        ScopedDrawTag(const ScopedDrawTag&)            = delete;
        ScopedDrawTag& operator=(const ScopedDrawTag&) = delete;

    private:
        bool m_tagged;
    };
}
