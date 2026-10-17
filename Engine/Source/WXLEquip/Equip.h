// WarcraftXL equipment extension, built into the client.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
#pragma once

namespace wxl::equip
{
    /**
     * @brief Brings the equip extension up: installs the native file reader and registers the virtual
     *        path table with the storage layer. Call once at startup, after wxl::InitModules().
     */
    void Init();
}
