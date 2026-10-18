// Event bus: subscriber storage + dispatch.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "Engine/Source/WXL/Event.h"
#include "Engine/Source/WXL/Log.h"

#include <atomic>
#include <vector>

namespace
{
    /// One subscriber: a handler function pointer and its opaque user pointer.
    struct Sub { wxl::events::Handler fn; void* user; };

    /**
     * @brief Returns the subscriber vector for an event, built on first use.
     * @param e  event whose bucket is requested.
     * @return reference to the event's subscriber vector.
     */
    std::vector<Sub>& Bucket(wxl::events::Event e)
    {
        static std::vector<Sub> buckets[static_cast<size_t>(wxl::events::Event::Count)];
        return buckets[static_cast<size_t>(e)];
    }
}

namespace wxl::events
{
    /**
     * @brief Appends a handler to an event's subscriber list.
     * @param e        event to subscribe to.
     * @param handler  function pointer invoked on Emit.
     * @param user     opaque pointer passed back to the handler.
     */
    void Subscribe(Event e, Handler handler, void* user)
    {
        if (e < Event::Count) Bucket(e).push_back({ handler, user });
    }

    /**
     * @brief Invokes every subscriber of an event in subscription order.
     * @param e     event to publish.
     * @param args  typed args struct for the event, passed by const pointer.
     */
    void Emit(Event e, const void* args)
    {
        if (e >= Event::Count) return;
        for (const Sub& s : Bucket(e)) s.fn(s.user, args);
    }

    /**
     * @brief Reports whether an event has at least one subscriber.
     * @param e  event to test.
     * @return true when Emit(e, ...) would invoke at least one handler.
     */
    bool Any(Event e)
    {
        return e < Event::Count && !Bucket(e).empty();
    }

    uint32_t SubscriberCount(Event e)
    {
        return e < Event::Count ? static_cast<uint32_t>(Bucket(e).size()) : 0;
    }

    const char* EventName(Event e)
    {
        switch (e)
        {
            case Event::OnModelLoadPre:      return "OnModelLoadPre";
            case Event::OnModelLoad:         return "OnModelLoad";
            case Event::OnM2SkinFinalize:    return "OnM2SkinFinalize";
            case Event::OnM2PerFrameUpdate:  return "OnM2PerFrameUpdate";
            case Event::OnBuildBonePalette:  return "OnBuildBonePalette";
            case Event::OnItemSlotChange:    return "OnItemSlotChange";
            case Event::OnItemSlotClear:     return "OnItemSlotClear";
            default:                         return "Event#?";
        }
    }

    namespace
    {
        std::atomic<uint32_t>& Fired(Event e)
        {
            static std::atomic<uint32_t> counts[static_cast<size_t>(Event::Count)];
            return counts[static_cast<size_t>(e)];
        }
    }

    void NoteFired(Event e)
    {
        if (e >= Event::Count) return;
        const uint32_t n = ++Fired(e);

        // The first firings of each event say "this hook point is alive"; the rare, important events are
        // also followed by a full counter dump so one glance shows the whole pipeline's state.
        if (n <= 3)
            wxl::LogCat("EVENT", "%s fired #%u (subscribers=%u)", EventName(e), n, SubscriberCount(e));

        const bool rare = e == Event::OnItemSlotChange || e == Event::OnItemSlotClear;
        if ((rare && n <= 6) || (e == Event::OnM2PerFrameUpdate && n % 50000 == 0))
            DumpStats();
    }

    void DumpStats()
    {
        for (uint32_t i = 0; i < static_cast<uint32_t>(Event::Count); ++i)
        {
            const Event e = static_cast<Event>(i);
            const uint32_t fired = Fired(e).load();
            const uint32_t subs  = SubscriberCount(e);
            if (fired == 0 && subs == 0) continue;
            wxl::LogCat("EVENT", "  stats %-20s fired=%-8u subscribers=%u%s", EventName(e), fired, subs,
                        (fired == 0 && subs != 0) ? "   <-- subscribed but the hook point has never run" : "");
        }
    }
}
