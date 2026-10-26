// The listfile: file id -> file path. Engine-free (no client type, no API table), so it is unit-testable.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// File format (what the community listfiles and the snippet use): one entry per line, "<id><delimiter><path>",
// delimiter ';' or ',', CRLF or LF, paths with spaces and forward slashes. A header row is optional: if the first
// non-blank line does not start with a number it is taken as the header and skipped. A UTF-8 BOM is skipped.
//
// Two ways to read it:
//   eager   the file is read once, at Configure(), into one string blob and a sorted array of (id, offset). A
//           lookup is a binary search: no allocation but the copy into the caller's string, no disk.
//   runtime nothing is read at Configure(). A lookup streams the file until it finds the id (first entry wins, as
//           in eager mode), then caches the answer, hits and misses both, so a repeated lookup costs nothing.
//           Reload() clears the cache.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl::listfile
{
    /// One finished log line, no newline. May be null.
    using LogFn = void (*)(const char* line);

    enum class LineKind { Data, Blank, Bad };

    /// Parses one line [b, e): "<id><;|,><path>". On Data, pb/pe delimit the path (trimmed, not NUL-terminated).
    /// Exposed for the tests.
    LineKind ParseLine(const char* b, const char* e, uint32_t& id, const char*& pb, const char*& pe);

    struct LoadStats
    {
        uint64_t lines      = 0;   ///< non-blank lines read (header excluded)
        uint64_t entries    = 0;   ///< distinct ids kept
        uint64_t duplicates = 0;   ///< ids that appeared again (the first entry is the one kept)
        uint64_t badLines   = 0;   ///< lines that were neither blank nor "<id><;|,><path>"
        bool     header     = false;
        bool     sorted     = true;///< the file was already in ascending id order (no sort needed)
        double   ms         = 0;   ///< time the eager build took
    };

    class Table
    {
    public:
        explicit Table(LogFn log = nullptr) : m_log(log) {}

        /// Sets the file and the mode. Eager: reads it now. @return false when eager and the file cannot be opened
        /// (the table stays empty and lookups answer "not found"); runtime mode always returns true.
        bool Configure(std::string path, bool eager);

        /// The path for an id, backslashes, as the file spells it otherwise. Thread-safe.
        bool Lookup(uint32_t id, std::string& outPath);

        /// Distinct ids in the eager table; 0 in runtime mode.
        uint32_t EntryCount() const;
        bool     Eager() const { return m_eager; }
        const std::string& Path() const { return m_path; }
        LoadStats Stats() const;

        /// Reads the file again (eager) or drops the cache (runtime). @return false when eager and unreadable.
        bool Reload();

    private:
        struct Entry { uint32_t id; uint32_t offset; };
        struct State { std::vector<Entry> entries; std::string blob; LoadStats stats; };

        bool Build(std::shared_ptr<State>& out);
        bool ScanFor(uint32_t id, std::string& outPath, bool& fileOpened);

        LogFn       m_log;
        std::string m_path;
        bool        m_eager = true;

        mutable std::mutex                m_lock;
        std::shared_ptr<const State>      m_state;          // eager table, immutable once published
        struct Cached { bool found; std::string path; };
        std::unordered_map<uint32_t, Cached> m_cache;       // runtime mode
        bool                              m_missingLogged = false;
    };
}
