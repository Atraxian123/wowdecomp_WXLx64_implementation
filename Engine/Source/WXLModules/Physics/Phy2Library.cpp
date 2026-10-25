// Per-model physics files. See Phy2Library.hpp.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "Phy2Library.hpp"

#include <algorithm>
#include <cstring>

namespace wxl::equip::phy2
{
    namespace
    {
        constexpr size_t kMaxCached = 20000;   // models remembered (hits and misses); cleared when full
        constexpr uint32_t kMaxRows = 64;      // a per-model file holds a row or two

        uint32_t Rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
        uint16_t Rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

        void AddUnique(std::vector<std::string>& v, std::string s)
        {
            if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(std::move(s));
        }

        bool EndsWith(const std::string& s, const char* tail)
        {
            const size_t n = std::strlen(tail);
            return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
        }
    }

    std::string NormalizeModelPath(const char* path)
    {
        std::string s = path ? path : "";
        for (char& c : s)
        {
            if (c == '/') c = '\\';
            else if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        }
        return s;
    }

    std::vector<std::string> Candidates(const char* modelPath, bool includeGlobal)
    {
        std::vector<std::string> out;
        std::string p = NormalizeModelPath(modelPath);
        if (p.empty()) return out;

        // the path without its extension (a dot inside a folder name is not an extension)
        const size_t slash = p.find_last_of('\\');
        const size_t dot = p.find_last_of('.');
        std::string base = (dot != std::string::npos && (slash == std::string::npos || dot > slash)) ? p.substr(0, dot) : p;
        if (base.empty()) return out;

        // 1. the model itself
        AddUnique(out, base + ".phy2");

        // 2. a collection key: <stem>_wxl_...  -> the stem is the original model's path
        const size_t wxl = base.find("_wxl_");
        if (wxl != std::string::npos && wxl > 0)
            AddUnique(out, base.substr(0, wxl) + ".phy2");

        // 3. a global key: <stem>[_oh]_<displayId>  -> the stem, with and without the offhand tag
        size_t digits = base.size();
        while (digits > 0 && base[digits - 1] >= '0' && base[digits - 1] <= '9') --digits;
        if (includeGlobal && digits < base.size() && digits > 1 && base[digits - 1] == '_' && (slash == std::string::npos || digits - 1 > slash + 1))
        {
            const std::string stem = base.substr(0, digits - 1);
            AddUnique(out, stem + ".phy2");
            if (EndsWith(stem, "_oh") && stem.size() > 3)
                AddUnique(out, stem.substr(0, stem.size() - 3) + ".phy2");
        }
        return out;
    }

    bool Parse(const uint8_t* blob, size_t size, phys::Model& out, const char** why)
    {
        auto fail = [&](const char* r) { if (why) *why = r; return false; };
        if (!blob || size < 8 || std::memcmp(blob, "PHY2", 4) != 0) return fail("not a PHY2 file");
        const uint32_t rows = Rd32(blob + 4);
        if (rows == 0) return fail("no rows");
        if (rows > kMaxRows || size_t(8) + size_t(rows) * 8 > size) return fail("row table runs past the end of the file");

        // Every row of a per-model file describes the same model (the path key, and the name stored
        // inside the .m2), so the first row that decodes is the one.
        for (uint32_t i = 0; i < rows; ++i)
        {
            const uint32_t off = Rd32(blob + 8 + size_t(i) * 8 + 4);
            if (size_t(off) + 2 > size) continue;
            const uint16_t keyLen = Rd16(blob + off);
            if (keyLen == 0 || keyLen > 259 || size_t(off) + 2 + keyLen > size) continue;
            char key[300];
            std::memcpy(key, blob + off + 2, keyLen);
            key[keyLen] = 0;
            char norm[300];
            if (!phys::NormPath(key, norm, sizeof(norm))) continue;
            phys::Model m;
            if (phys::Find(blob, size, norm, m) && !m.bodies.empty())
            {
                out = std::move(m);
                return true;
            }
        }
        return fail("no row could be decoded (unsupported shape or joint, or a damaged file)");
    }

    Result Library::For(const char* modelPath, bool* fresh)
    {
        if (fresh) *fresh = false;
        const std::string key = NormalizeModelPath(modelPath);
        if (key.empty() || !m_read) return {};

        {
            std::lock_guard<std::mutex> g(m_lock);
            auto it = m_cache.find(key);
            if (it != m_cache.end()) return it->second;
        }

        // Not seen before: read from disk with no lock held (archive reads can be slow, and other
        // animate threads must not queue behind them). Two threads may both read the same model;
        // the second answer is identical and harmlessly replaces the first.
        Result r;
        std::vector<std::string> tried;
        auto attempt = [&](const std::vector<std::string>& cands) {
            for (const std::string& cand : cands)
            {
                if (std::find(tried.begin(), tried.end(), cand) != tried.end()) continue;
                tried.push_back(cand);
                std::vector<uint8_t> bytes;
                if (!m_read(cand.c_str(), bytes) || bytes.empty()) continue;
                auto model = std::make_shared<phys::Model>();
                const char* why = nullptr;
                if (Parse(bytes.data(), bytes.size(), *model, &why))
                {
                    r.model = std::move(model);
                    r.path = cand;
                    r.error.clear();
                    return true;
                }
                r.error = cand + ": " + (why ? why : "unusable");   // keep looking; report it if nothing better turns up
            }
            return false;
        };
        // The model itself, or the original of a collection key, first. Only if neither has a file and
        // the path is a genuine virtual key is the "<stem>_<displayId>" shape worth decoding.
        if (!attempt(Candidates(modelPath, false)) && m_isVirtual && m_isVirtual(modelPath))
            attempt(Candidates(modelPath, true));

        std::lock_guard<std::mutex> g(m_lock);
        if (m_cache.size() >= kMaxCached) m_cache.clear();
        m_cache[key] = r;
        if (fresh) *fresh = true;
        return r;
    }

    void Library::Clear()
    {
        std::lock_guard<std::mutex> g(m_lock);
        m_cache.clear();
    }

    size_t Library::Cached() const
    {
        std::lock_guard<std::mutex> g(m_lock);
        return m_cache.size();
    }

    size_t Library::Found() const
    {
        std::lock_guard<std::mutex> g(m_lock);
        size_t n = 0;
        for (const auto& kv : m_cache) if (kv.second.model) ++n;
        return n;
    }
}
