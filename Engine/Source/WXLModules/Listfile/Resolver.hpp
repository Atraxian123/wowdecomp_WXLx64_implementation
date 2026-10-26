// The module's public behaviour: look an id up, and hand out a not-found name when the file is not there.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// Engine-free: what it needs from the client (does this file exist? write a log line) comes in as two functions,
// so the tests drive it without one.
#pragma once

#include "ListfileTable.hpp"

#include "wxl_listfile_api.h"

#include <mutex>
#include <string>
#include <unordered_set>

namespace wxl::listfile
{
    struct Hooks
    {
        bool (*exists)(const char* clientPath) = nullptr;   ///< can the client open it? null = assume yes
        void (*log)(const char* line) = nullptr;
    };

    class Resolver
    {
    public:
        /// @param notFoundName the base of the name handed out for a file that is not found, e.g. "WXL\\NotFound"
        ///        (.blp / .m2 is appended by kind). It must not be a file the client has.
        void Configure(Table* table, Hooks hooks, bool checkExists, std::string notFoundName);

        int LookupPath(uint32_t fileId, char* out, size_t outSize);
        int Resolve(uint32_t fileId, uint32_t kind, char* out, size_t outSize);
        int ResolvePath(const char* path, uint32_t kind, char* out, size_t outSize);

    private:
        int ResolveImpl(const char* path, uint32_t kind, char* out, size_t outSize, const uint32_t* fileId);
        std::string NotFoundFor(uint32_t kind) const;
        int Deliver(const std::string& path, int result, char* out, size_t outSize);
        void LogMissing(const std::string& key, const char* what);   // once per key

        Table*      m_table = nullptr;
        Hooks       m_hooks;
        bool        m_checkExists = true;
        std::string m_notFoundName;

        std::mutex  m_lock;
        std::unordered_set<std::string> m_logged;
        bool        m_logFull = false;
    };
}
