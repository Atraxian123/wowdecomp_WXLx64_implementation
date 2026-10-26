// See ListfileTable.hpp.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "ListfileTable.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX   // windows.h must not turn std::min / std::max into macros
#endif
#include <windows.h>
#endif

namespace wxl::listfile
{
    namespace
    {
        constexpr size_t   kChunk         = 1u << 20;      // streaming read size
        constexpr size_t   kMaxCache      = 65536;         // runtime-mode cache entries before it is cleared
        constexpr uint64_t kMaxBlobBytes  = 0xFFFFFFF0ull; // offsets are 32-bit

        // A UTF-8 path to a FILE*: the client folder may hold characters outside the ANSI code page.
        FILE* OpenRead(const std::string& utf8)
        {
#if defined(_WIN32)
            const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
            if (n <= 0) return nullptr;
            std::wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], n);
            return _wfopen(w.c_str(), L"rb");
#else
            return std::fopen(utf8.c_str(), "rb");
#endif
        }

        void LogF(LogFn log, const char* fmt, ...)
        {
            if (!log) return;
            char line[512];
            va_list ap;
            va_start(ap, fmt);
            std::vsnprintf(line, sizeof(line), fmt, ap);
            va_end(ap);
            log(line);
        }

        // Streams the file line by line (no line is ever longer than the carry buffer can grow: it is a std::string).
        // fn(begin, end) returns false to stop. A UTF-8 BOM at the start is skipped. @return false when it cannot open.
        template <class Fn>
        bool ForEachLine(const std::string& path, Fn&& fn, uint64_t* fileSize = nullptr)
        {
            FILE* f = OpenRead(path);
            if (!f) return false;
            if (fileSize)
            {
                *fileSize = 0;
                if (std::fseek(f, 0, SEEK_END) == 0)
                {
                    const long sz = std::ftell(f);
                    if (sz > 0) *fileSize = static_cast<uint64_t>(sz);
                    std::fseek(f, 0, SEEK_SET);
                }
            }

            std::vector<char> buf(kChunk);
            std::string carry;
            bool first = true, stop = false;
            size_t got;
            while (!stop && (got = std::fread(buf.data(), 1, buf.size(), f)) > 0)
            {
                const char* p   = buf.data();
                const char* end = p + got;
                if (first)
                {
                    first = false;
                    if (got >= 3 && static_cast<unsigned char>(p[0]) == 0xEF && static_cast<unsigned char>(p[1]) == 0xBB &&
                        static_cast<unsigned char>(p[2]) == 0xBF) p += 3;
                }
                while (p < end)
                {
                    const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
                    if (!nl) { carry.append(p, end); break; }
                    if (carry.empty())
                    {
                        if (!fn(p, nl)) { stop = true; break; }
                    }
                    else
                    {
                        carry.append(p, nl);
                        const bool go = fn(carry.data(), carry.data() + carry.size());
                        carry.clear();
                        if (!go) { stop = true; break; }
                    }
                    p = nl + 1;
                }
            }
            if (!stop && !carry.empty()) fn(carry.data(), carry.data() + carry.size());   // last line without a newline
            std::fclose(f);
            return true;
        }
    }

    LineKind ParseLine(const char* b, const char* e, uint32_t& id, const char*& pb, const char*& pe)
    {
        while (b < e && (*b == ' ' || *b == '\t')) ++b;
        while (e > b && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t')) --e;
        if (b == e) return LineKind::Blank;

        uint64_t value = 0;
        const char* p = b;
        while (p < e && *p >= '0' && *p <= '9')
        {
            value = value * 10 + static_cast<uint64_t>(*p - '0');
            if (value > 0xFFFFFFFFull) return LineKind::Bad;
            ++p;
        }
        if (p == b || p >= e || (*p != ';' && *p != ',')) return LineKind::Bad;
        ++p;
        while (p < e && (*p == ' ' || *p == '\t')) ++p;
        if (p == e) return LineKind::Bad;

        id = static_cast<uint32_t>(value);
        pb = p;
        pe = e;
        return LineKind::Data;
    }

    bool Table::Build(std::shared_ptr<State>& out)
    {
        const auto t0 = std::chrono::steady_clock::now();
        auto st = std::make_shared<State>();
        LoadStats& s = st->stats;

        bool firstLine = true;
        uint32_t prevId = 0;
        uint64_t fileSize = 0;
        bool truncated = false, reserved = false;
        uint64_t badLogged = 0;

        const bool opened = ForEachLine(m_path, [&](const char* b, const char* e) {
            uint32_t id = 0;
            const char *pb = nullptr, *pe = nullptr;
            const LineKind kind = ParseLine(b, e, id, pb, pe);
            if (kind == LineKind::Blank) return true;
            if (kind == LineKind::Bad)
            {
                if (firstLine) { s.header = true; firstLine = false; return true; }   // "fileId;path" and the like
                ++s.badLines;
                if (badLogged < 3)
                {
                    ++badLogged;
                    LogF(m_log, "listfile: skipped a line that is not '<id>;<path>' (line %llu)",
                         static_cast<unsigned long long>(s.lines + s.badLines + (s.header ? 1 : 0)));
                }
                return true;
            }
            firstLine = false;
            ++s.lines;

            if (st->blob.size() + static_cast<size_t>(pe - pb) + 1 > kMaxBlobBytes) { truncated = true; return false; }
            if (!reserved && fileSize)
            {
                // ~40 bytes a line is typical; the blob holds the paths, which are most of the file
                reserved = true;
                st->entries.reserve(static_cast<size_t>(fileSize / 40 + 16));
                st->blob.reserve(static_cast<size_t>(std::min<uint64_t>(fileSize * 85 / 100, kMaxBlobBytes)));
            }
            const uint32_t offset = static_cast<uint32_t>(st->blob.size());
            for (const char* c = pb; c < pe; ++c) st->blob.push_back(*c == '/' ? '\\' : *c);
            st->blob.push_back('\0');
            if (!st->entries.empty() && id < prevId) s.sorted = false;
            prevId = id;
            st->entries.push_back({ id, offset });
            return true;
        }, &fileSize);
        if (!opened) return false;
        if (truncated) LogF(m_log, "listfile: the paths do not fit a 4 GB table: the rest of the file was not read");

        if (!s.sorted)
            std::stable_sort(st->entries.begin(), st->entries.end(),
                             [](const Entry& a, const Entry& b) { return a.id < b.id; });
        // keep the first of equal ids (stable sort kept file order among them)
        auto last = std::unique(st->entries.begin(), st->entries.end(),
                                [](const Entry& a, const Entry& b) { return a.id == b.id; });
        s.duplicates = static_cast<uint64_t>(st->entries.end() - last);
        st->entries.erase(last, st->entries.end());
        st->entries.shrink_to_fit();
        s.entries = st->entries.size();
        s.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        out = std::move(st);
        return true;
    }

    bool Table::Configure(std::string path, bool eager)
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_path = std::move(path);
            m_eager = eager;
            m_state.reset();
            m_cache.clear();
            m_missingLogged = false;
        }
        return Reload();
    }

    bool Table::Reload()
    {
        if (!m_eager)
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_cache.clear();
            m_missingLogged = false;
            return true;
        }
        std::shared_ptr<State> st;
        if (!Build(st))
        {
            LogF(m_log, "listfile: cannot open '%s': every lookup will be answered 'not found'", m_path.c_str());
            std::lock_guard<std::mutex> lock(m_lock);
            m_state = std::make_shared<State>();   // an empty table, so Lookup never has to special-case it
            return false;
        }
        const LoadStats& s = st->stats;
        LogF(m_log, "listfile: eager table built from '%s': %llu entries from %llu lines in %.1f ms "
                    "(header %s, %s, %llu duplicate id(s), %llu bad line(s))",
             m_path.c_str(), static_cast<unsigned long long>(s.entries), static_cast<unsigned long long>(s.lines), s.ms,
             s.header ? "skipped" : "none", s.sorted ? "already sorted" : "sorted on load",
             static_cast<unsigned long long>(s.duplicates), static_cast<unsigned long long>(s.badLines));
        std::lock_guard<std::mutex> lock(m_lock);
        m_state = std::move(st);
        return true;
    }

    uint32_t Table::EntryCount() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m_eager && m_state ? static_cast<uint32_t>(m_state->entries.size()) : 0;
    }

    LoadStats Table::Stats() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m_state ? m_state->stats : LoadStats{};
    }

    bool Table::ScanFor(uint32_t id, std::string& outPath, bool& fileOpened)
    {
        bool found = false;
        fileOpened = ForEachLine(m_path, [&](const char* b, const char* e) {
            uint32_t lineId = 0;
            const char *pb = nullptr, *pe = nullptr;
            const LineKind kind = ParseLine(b, e, lineId, pb, pe);
            if (kind == LineKind::Blank) return true;
            if (kind == LineKind::Bad) return true;                           // header or junk: never an entry
            if (lineId != id) return true;
            outPath.assign(pb, pe);
            for (char& c : outPath) if (c == '/') c = '\\';
            found = true;
            return false;                                                     // first entry wins, like the eager table
        });
        return found;
    }

    bool Table::Lookup(uint32_t id, std::string& outPath)
    {
        if (m_eager)
        {
            std::shared_ptr<const State> st;
            { std::lock_guard<std::mutex> lock(m_lock); st = m_state; }
            if (!st) return false;
            auto it = std::lower_bound(st->entries.begin(), st->entries.end(), id,
                                       [](const Entry& e, uint32_t v) { return e.id < v; });
            if (it == st->entries.end() || it->id != id) return false;
            outPath.assign(st->blob.c_str() + it->offset);
            return true;
        }

        {
            std::lock_guard<std::mutex> lock(m_lock);
            auto it = m_cache.find(id);
            if (it != m_cache.end())
            {
                if (it->second.found) outPath = it->second.path;
                return it->second.found;
            }
        }
        // No lock held while the file is read: lookups of other ids are not queued behind this scan.
        std::string path;
        bool opened = false;
        const bool found = ScanFor(id, path, opened);

        std::lock_guard<std::mutex> lock(m_lock);
        if (!opened)
        {
            if (!m_missingLogged)
            {
                m_missingLogged = true;
                LogF(m_log, "listfile: cannot open '%s': every lookup will be answered 'not found'", m_path.c_str());
            }
            return false;                                   // not cached: the file may appear later
        }
        if (m_cache.size() >= kMaxCache) m_cache.clear();
        m_cache[id] = Cached{ found, path };
        if (found) outPath = std::move(path);
        return found;
    }
}
