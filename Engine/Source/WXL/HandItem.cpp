#include "Engine/Source/WXL/HandItem.h"
#include "Engine/Source/WXL/Log.h"

namespace wxl::hooks
{
    namespace
    {
        HandItemModelResolver g_resolver = nullptr;
        MirroredModelQuery g_mirrorQuery = nullptr;
    }

    void SetHandItemModelResolver(HandItemModelResolver fn)
    {
        g_resolver = fn;
        wxl::LogCat("EVENT", "hand-item model resolver %s", fn ? "installed" : "cleared");
    }

    bool ResolveHandItemModel(uint32_t displayId, uint32_t invSlot, char* out, size_t outSize)
    {
        // Counts calls so the log proves the engine reaches AddHandItem at all, whether or not a resolver
        // or a sidecar entry exists.
        if (wxl::LogThrottle("handitem.called", 12, 500))
            wxl::LogCat("EVENT", "AddHandItem reached: displayId=%u slot=%u resolver=%s", displayId, invSlot,
                        g_resolver ? "installed" : "NOT INSTALLED");
        if (!g_resolver || !out || outSize == 0) return false;
        out[0] = '\0';
        return g_resolver(displayId, invSlot, out, outSize);
    }

    void SetMirroredModelQuery(MirroredModelQuery fn) { g_mirrorQuery = fn; }

    bool IsModelMirrored(const void* model)
    {
        return g_mirrorQuery && model && g_mirrorQuery(model);
    }
}
