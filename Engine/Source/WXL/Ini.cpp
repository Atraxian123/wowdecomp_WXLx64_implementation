// WXLSetup.ini. See Ini.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)

#include "Engine/Source/WXL/Ini.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace wxl::ini
{
    namespace
    {
        constexpr uint64_t kUnset = ~0ull;
        constexpr uint64_t kRecheckMs = 500;     // how often the file's time is looked at for an outside save
        constexpr uint64_t kPathRecheckMs = 2000; // how often the WXL\ copy is looked for again

        std::mutex                g_lock;               // guards the overrides and the path cache
        std::vector<Entry>        g_overrides;
        std::string               g_path;
        uint64_t                  g_pathTick = 0;
        std::atomic<uint32_t>     g_generation{1};
        std::atomic<uint64_t>     g_lastCheck{0};
        std::atomic<uint64_t>     g_fileStamp{kUnset};

        uint64_t NowMs()
        {
#ifdef _WIN32
            return static_cast<uint64_t>(GetTickCount64());
#else
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
        }

        bool EqualNoCase(const std::string& a, const char* b)
        {
            const size_t n = std::strlen(b);
            if (a.size() != n) return false;
            for (size_t i = 0; i < n; ++i)
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
            return true;
        }

        std::string Trim(const std::string& s)
        {
            size_t b = 0, e = s.size();
            while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
            while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
            return s.substr(b, e - b);
        }

        // ---- where the file is ----------------------------------------------------------------------------------
        std::string ComputePath()
        {
            const char* env = std::getenv("WXL_INI");
            if (env && *env) return env;
#ifdef _WIN32
            char exePath[MAX_PATH] = {};
            const DWORD n = GetModuleFileNameA(nullptr, exePath, static_cast<DWORD>(sizeof(exePath)));
            if (n == 0 || n >= sizeof(exePath)) return {};
            char* slash = std::strrchr(exePath, '\\');
            if (!slash) return "WXLSetup.ini";
            const std::string dir(exePath, static_cast<size_t>(slash - exePath) + 1);   // keeps the trailing '\'
            const std::string inWxl = dir + "WXL\\WXLSetup.ini";
            // the ini lives in <root>\WXL; a copy left beside the executable (the old place) is still read
            if (GetFileAttributesA(inWxl.c_str()) != INVALID_FILE_ATTRIBUTES) return inWxl;
            return dir + "WXLSetup.ini";
#else
            return "WXLSetup.ini";
#endif
        }

        std::string CurrentPath()
        {
            std::lock_guard<std::mutex> g(g_lock);
            const uint64_t now = NowMs();
            if (g_path.empty() || now - g_pathTick >= kPathRecheckMs)
            {
                g_path = ComputePath();
                g_pathTick = now;
            }
            return g_path;
        }

        uint64_t FileStamp(const std::string& path)
        {
            if (path.empty()) return 0;
#ifdef _WIN32
            WIN32_FILE_ATTRIBUTE_DATA data = {};
            if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return 0;
            return (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
#else
            struct stat st;
            if (::stat(path.c_str(), &st) != 0) return 0;
            return static_cast<uint64_t>(st.st_mtim.tv_sec) * 1000000000ull + static_cast<uint64_t>(st.st_mtim.tv_nsec);
#endif
        }

        // ---- a parsed copy of the file, for listing (and for reading where there is no Win32 profile API) ---------
        struct Section_
        {
            std::string name;
            std::vector<std::pair<std::string, std::string>> keys;
        };
        using Parsed = std::vector<Section_>;

        Parsed ParseText(const std::string& text)
        {
            Parsed out;
            std::istringstream in(text);
            std::string line;
            bool first = true;
            while (std::getline(in, line))
            {
                if (first)
                {
                    first = false;
                    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB &&
                        static_cast<unsigned char>(line[2]) == 0xBF)
                        line.erase(0, 3);
                }
                const std::string t = Trim(line);
                if (t.empty() || t[0] == ';') continue;
                if (t[0] == '[')
                {
                    const size_t close = t.find(']');
                    if (close == std::string::npos) continue;
                    out.push_back({Trim(t.substr(1, close - 1)), {}});
                    continue;
                }
                const size_t eq = t.find('=');
                if (eq == std::string::npos || out.empty()) continue;
                std::string value = Trim(t.substr(eq + 1));
                // GetPrivateProfileString drops one pair of matching quotes around a value
                if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
                    value = value.substr(1, value.size() - 2);
                out.back().keys.push_back({Trim(t.substr(0, eq)), value});
            }
            return out;
        }

        std::string ReadFileText(const std::string& path)
        {
            std::ifstream f(path, std::ios::binary);
            if (!f) return {};
            std::ostringstream s;
            s << f.rdbuf();
            return s.str();
        }

        // The parsed file, cached until its time changes.
        Parsed ParsedFile()
        {
            static std::mutex lock;
            static Parsed cached;
            static std::string cachedPath;
            static uint64_t cachedStamp = kUnset;
            const std::string path = CurrentPath();
            const uint64_t stamp = FileStamp(path);
            std::lock_guard<std::mutex> g(lock);
            if (path != cachedPath || stamp != cachedStamp || stamp == 0)
            {
                cached = stamp ? ParseText(ReadFileText(path)) : Parsed{};
                cachedPath = path;
                cachedStamp = stamp;
            }
            return cached;
        }

        bool FileLookup(const char* section, const char* key, std::string& out)
        {
#ifdef _WIN32
            const std::string path = CurrentPath();
            if (path.empty()) return false;
            char buf[2048] = {};
            // A one-character sentinel nobody writes in an ini tells "key missing" from "key present and empty".
            const DWORD n = GetPrivateProfileStringA(section, key, "\x01", buf, static_cast<DWORD>(sizeof(buf)), path.c_str());
            if (n == 1 && buf[0] == '\x01') return false;
            out = buf;
            return true;
#else
            for (const Section_& s : ParsedFile())
            {
                if (!EqualNoCase(s.name, section)) continue;
                for (const auto& kv : s.keys)
                    if (EqualNoCase(kv.first, key)) { out = kv.second; return true; }
            }
            return false;
#endif
        }

        bool OverrideLookup(const char* section, const char* key, std::string& out)
        {
            std::lock_guard<std::mutex> g(g_lock);
            for (const Entry& e : g_overrides)
                if (EqualNoCase(e.section, section) && EqualNoCase(e.key, key)) { out = e.value; return true; }
            return false;
        }

        // The value as text: the session override first, then the file.
        bool Lookup(const char* section, const char* key, std::string& out)
        {
            if (!section || !key) return false;
            return OverrideLookup(section, key, out) || FileLookup(section, key, out);
        }

        // GetPrivateProfileInt's reading of a value: leading digits; anything else, and a negative number, is 0.
        int ParseInt(const std::string& s)
        {
            const std::string t = Trim(s);
            if (t.empty()) return 0;
            char* end = nullptr;
            const long v = std::strtol(t.c_str(), &end, 10);
            if (end == t.c_str() || v < 0) return 0;
            return v > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(v);
        }
    }

    const char* Path()
    {
        static thread_local std::string tl;
        tl = CurrentPath();
        return tl.c_str();
    }

    int GetInt(const char* section, const char* key, int defaultValue)
    {
        std::string v;
        return Lookup(section, key, v) ? ParseInt(v) : defaultValue;
    }

    bool GetBool(const char* section, const char* key, bool defaultValue)
    {
        return GetInt(section, key, defaultValue ? 1 : 0) != 0;
    }

    float GetFloat(const char* section, const char* key, float defaultValue)
    {
        std::string v;
        if (!Lookup(section, key, v)) return defaultValue;
        v = Trim(v);
        std::replace(v.begin(), v.end(), ',', '.');
        if (v.empty()) return defaultValue;
        char* end = nullptr;
        const double d = std::strtod(v.c_str(), &end);
        return end == v.c_str() ? defaultValue : static_cast<float>(d);
    }

    bool GetString(const char* section, const char* key, char* out, size_t cap)
    {
        if (!out || cap == 0) return false;
        out[0] = '\0';
        std::string v;
        if (!Lookup(section, key, v)) return false;
        std::strncpy(out, v.c_str(), cap - 1);
        out[cap - 1] = '\0';
        return true;
    }

    std::string GetText(const char* section, const char* key, const char* defaultValue)
    {
        std::string v;
        return Lookup(section, key, v) ? v : std::string(defaultValue ? defaultValue : "");
    }

    uint32_t Generation()
    {
        const uint64_t now = NowMs();
        uint64_t last = g_lastCheck.load(std::memory_order_relaxed);
        if (now - last >= kRecheckMs && g_lastCheck.compare_exchange_strong(last, now))
        {
            const uint64_t stamp = FileStamp(CurrentPath());
            const uint64_t previous = g_fileStamp.exchange(stamp);
            if (previous != kUnset && previous != stamp) g_generation.fetch_add(1);   // saved by something else
        }
        return g_generation.load();
    }

    uint64_t Stamp()
    {
        const uint32_t gen = Generation();
        return FileStamp(CurrentPath()) + gen;
    }

    bool Set(const char* section, const char* key, const char* value)
    {
        if (!section || !*section || !key || !*key) return false;
        {
            std::lock_guard<std::mutex> g(g_lock);
            for (Entry& e : g_overrides)
                if (EqualNoCase(e.section, section) && EqualNoCase(e.key, key))
                {
                    e.value = value ? value : "";
                    g_generation.fetch_add(1);
                    return true;
                }
            g_overrides.push_back({section, key, value ? value : ""});
            g_generation.fetch_add(1);
        }
        return true;
    }

    bool Unset(const char* section, const char* key)
    {
        if (!section || !key) return false;
        std::lock_guard<std::mutex> g(g_lock);
        for (auto it = g_overrides.begin(); it != g_overrides.end(); ++it)
            if (EqualNoCase(it->section, section) && EqualNoCase(it->key, key))
            {
                g_overrides.erase(it);
                g_generation.fetch_add(1);
                return true;
            }
        return false;
    }

    void ClearOverrides()
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (g_overrides.empty()) return;
        g_overrides.clear();
        g_generation.fetch_add(1);
    }

    std::vector<Entry> Overrides()
    {
        std::lock_guard<std::mutex> g(g_lock);
        return g_overrides;
    }

    namespace
    {
        // Rewrites `path` with one key set. Keeps every other line (comments included) exactly as it was; the key goes
        // on its own line in its section, or in a new section at the end. Used where there is no Win32 profile API.
        bool RewriteKey(const std::string& path, const Entry& e)
        {
            std::string text = ReadFileText(path);
            std::vector<std::string> lines;
            {
                std::istringstream in(text);
                std::string line;
                while (std::getline(in, line)) lines.push_back(line);
            }
            const bool crlf = text.find("\r\n") != std::string::npos;
            size_t sectionLine = std::string::npos, insertAt = std::string::npos;
            bool inSection = false, replaced = false;
            for (size_t i = 0; i < lines.size() && !replaced; ++i)
            {
                const std::string t = Trim(lines[i]);
                if (!t.empty() && t[0] == '[')
                {
                    if (inSection) { insertAt = i; inSection = false; }
                    const size_t close = t.find(']');
                    if (close != std::string::npos && EqualNoCase(Trim(t.substr(1, close - 1)), e.section.c_str()))
                    {
                        inSection = true;
                        sectionLine = i;
                    }
                    continue;
                }
                if (!inSection || t.empty() || t[0] == ';') continue;
                const size_t eq = t.find('=');
                if (eq != std::string::npos && EqualNoCase(Trim(t.substr(0, eq)), e.key.c_str()))
                {
                    lines[i] = e.key + "=" + e.value;
                    replaced = true;
                }
            }
            if (!replaced)
            {
                const std::string kv = e.key + "=" + e.value;
                if (sectionLine == std::string::npos)
                {
                    if (!lines.empty() && !Trim(lines.back()).empty()) lines.push_back("");
                    lines.push_back("[" + e.section + "]");
                    lines.push_back(kv);
                }
                else
                {
                    if (insertAt == std::string::npos) insertAt = lines.size();
                    while (insertAt > sectionLine + 1 && Trim(lines[insertAt - 1]).empty()) --insertAt;   // above trailing blanks
                    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertAt), kv);
                }
            }
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            for (const std::string& l : lines) out << l << (crlf ? "\r\n" : "\n");
            return static_cast<bool>(out);
        }
    }

    bool Save()
    {
        const std::vector<Entry> pending = Overrides();
        if (pending.empty()) return true;
        const std::string path = CurrentPath();
        if (path.empty()) return false;
        for (const Entry& e : pending)
        {
#ifdef _WIN32
            if (!WritePrivateProfileStringA(e.section.c_str(), e.key.c_str(), e.value.c_str(), path.c_str())) return false;
#else
            if (!RewriteKey(path, e)) return false;
#endif
        }
        {
            std::lock_guard<std::mutex> g(g_lock);
            // drop what was written, unless it was changed again meanwhile
            for (const Entry& e : pending)
                for (auto it = g_overrides.begin(); it != g_overrides.end(); ++it)
                    if (EqualNoCase(it->section, e.section.c_str()) && EqualNoCase(it->key, e.key.c_str()) && it->value == e.value)
                    {
                        g_overrides.erase(it);
                        break;
                    }
            g_generation.fetch_add(1);
        }
        return true;
    }

    std::vector<std::string> Sections()
    {
        std::vector<std::string> out;
        for (const Section_& s : ParsedFile())
            if (std::find_if(out.begin(), out.end(), [&](const std::string& n) { return EqualNoCase(n, s.name.c_str()); }) == out.end())
                out.push_back(s.name);
        for (const Entry& e : Overrides())
            if (std::find_if(out.begin(), out.end(), [&](const std::string& n) { return EqualNoCase(n, e.section.c_str()); }) == out.end())
                out.push_back(e.section);
        return out;
    }

    std::vector<std::pair<std::string, std::string>> Section(const char* section)
    {
        std::vector<std::pair<std::string, std::string>> out;
        if (!section) return out;
        for (const Section_& s : ParsedFile())
        {
            if (!EqualNoCase(s.name, section)) continue;
            for (const auto& kv : s.keys)
            {
                auto it = std::find_if(out.begin(), out.end(), [&](const auto& p) { return EqualNoCase(p.first, kv.first.c_str()); });
                if (it == out.end()) out.push_back(kv);
                else it->second = kv.second;   // the last one wins, as GetPrivateProfileString does
            }
        }
        for (const Entry& e : Overrides())
        {
            if (!EqualNoCase(e.section, section)) continue;
            auto it = std::find_if(out.begin(), out.end(), [&](const auto& p) { return EqualNoCase(p.first, e.key.c_str()); });
            if (it == out.end()) out.push_back({e.key, e.value});
            else it->second = e.value;
        }
        return out;
    }
}
