// WarcraftXL in-tree entry point.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
#pragma once

namespace wxl
{
    /**
     * @brief Constructs every in-tree WXL module so it can subscribe to events.
     *
     * Modules are created here explicitly rather than through file-scope objects: the client links
     * its modules as static libraries, and the linker drops a library object that nothing references,
     * which would silently discard a self-registering module. Call once, before the first model is loaded.
     */
    void InitModules();
}
