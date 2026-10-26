// Services: how one extension offers a function table to another without either knowing where the other lives.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// A provider (a module, or code compiled into the client) registers a table of C function pointers under a
// short name; a consumer looks the table up when it needs it. The core stores the pointer and size and never
// looks inside. The table must be plain C (see Sdk/wxl_api.h for the ones that exist) and must stay valid for
// the life of the process. Consumers look up at the time of the call, not at startup, so load order between
// the two does not matter, and a missing provider is just a null answer.
#pragma once

#include <cstddef>

namespace wxl::services
{
    /// Registers (or replaces) a table. Returns false for an empty name or a null table.
    bool Register(const char* name, const void* table, size_t size);

    /// The table registered under `name`, or null; *size receives its size (0 when absent).
    const void* Get(const char* name, size_t* size = nullptr);
}
