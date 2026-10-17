// WXL virtual-file layer: lets in-tree modules answer a file open with bytes they hold in memory.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
//
// The injected WXL detoured the Storm open/size/read/seek/close entry points and built its own handle
// type. In source none of that is needed: SFileImpl already has an in-memory kind (type 5), so
// SFile::OpenEx asks TryServe first and, on a hit, returns an ordinary in-memory SFileImpl. Size, read,
// seek and close then need no change at all.
#pragma once

#include <cstdint>
#include <vector>

namespace wxl::storage
{
    /**
     * @brief Callback type for a virtual file provider.
     * @param name  file name as the engine requested it
     * @param out   receives the file bytes when the provider claims the name
     * @return true if this provider supplied bytes for name
     */
    using ProviderFn = bool (*)(const char* name, std::vector<uint8_t>& out);

    /// Registers a provider. The first provider that claims a name wins. Not thread-safe against
    /// concurrent opens: register during startup, from wxl::InitModules() or a module's Init().
    void RegisterProvider(ProviderFn fn);

    /// True when at least one provider is registered, so the open path can skip all work otherwise.
    bool HasProviders();

    /**
     * @brief Asks the providers for a file. Called by SFile::OpenEx before it looks in the archives.
     * @param name  file name from the open call (either slash style, any case)
     * @param out   receives the bytes on a hit
     * @return true when a provider claimed the name
     */
    bool TryServe(const char* name, std::vector<uint8_t>& out);

    /// Reads a file the way the client would, bypassing nothing: providers see this open too, so a
    /// provider must not claim a name it is itself trying to read. Installed by the module that links
    /// the engine (see wxl::equip::Init); WXLCore itself cannot depend on BlizzardCore.
    using NativeReadFn = bool (*)(const char* name, std::vector<uint8_t>& out);
    void SetNativeReader(NativeReadFn fn);
    bool ReadNative(const char* name, std::vector<uint8_t>& out);
}
