// Per-model multi-textured particle emitter tables. See ParticleCsv.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)

#include "Engine/Source/WXL/ParticleCsv.h"

#include "Engine/Source/WXL/Ini.h"
#include "Engine/Source/WXL/Log.h"
#include "Engine/Source/WXL/Storage.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace wxl::particlecsv
{
    namespace
    {
        constexpr size_t kMaxCached = 4096;      // models remembered (hits and misses); cleared when full
        constexpr uint32_t kMaxEmitterId = 4096; // a model has a handful of emitters; anything above is a bad row

        std::string Lower(std::string s)
        {
            for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            return s;
        }

        std::string Trim(const std::string& s)
        {
            size_t b = 0, e = s.size();
            while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
            while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
            return s.substr(b, e - b);
        }

        // One CSV line into fields: commas separate, a double-quoted field may hold commas and "" for a quote.
        std::vector<std::string> SplitLine(const std::string& line)
        {
            std::vector<std::string> out;
            std::string cur;
            bool quoted = false;
            for (size_t i = 0; i < line.size(); ++i)
            {
                const char c = line[i];
                if (quoted)
                {
                    if (c == '"')
                    {
                        if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
                        else quoted = false;
                    }
                    else cur += c;
                }
                else if (c == '"') quoted = true;
                else if (c == ',') { out.push_back(Trim(cur)); cur.clear(); }
                else cur += c;
            }
            out.push_back(Trim(cur));
            return out;
        }

        bool ParseUInt(const std::string& s, uint32_t& out)
        {
            if (s.empty()) return false;
            uint64_t v = 0;
            for (char c : s)
            {
                if (c < '0' || c > '9') return false;
                v = v * 10 + uint64_t(c - '0');
                if (v > 0xFFFFFFFFull) return false;
            }
            out = uint32_t(v);
            return true;
        }

        bool IsYes(const std::string& s)
        {
            const std::string v = Lower(s);
            return v == "yes" || v == "y" || v == "true" || v == "1";
        }

        // Header text to a column role, ignoring case, spaces and underscores. -1: not a column we use.
        int ColumnRole(const std::string& header)
        {
            std::string k;
            for (char c : Lower(header)) if (c != ' ' && c != '_' && c != '-') k += c;
            if (k == "emitterid" || k == "emitter") return 0;
            if (k == "particletextureid" || k == "textureid" || k == "textureslot" || k == "slot") return 1;
            if (k == "texturepath" || k == "texture" || k == "path") return 2;
            if (k == "multitextured" || k == "multitexture") return 3;
            if (k == "blending" || k == "blend" || k == "blendmode") return 4;
            return -1;
        }

        bool EndsWith(const std::string& s, const char* tail)
        {
            const size_t n = std::strlen(tail);
            return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
        }

        void AddUnique(std::vector<std::string>& v, std::string s)
        {
            if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(std::move(s));
        }

        std::mutex g_lock;
        std::unordered_map<std::string, std::shared_ptr<const Table>> g_cache;   // null value: looked, none there
    }

    uint32_t Emitter::NamedSlots() const
    {
        uint32_t n = 0;
        for (const std::string& t : texture) if (!t.empty()) ++n;
        return n;
    }

    const Emitter* Table::Find(uint32_t emitterId) const
    {
        const auto it = emitters.find(emitterId);
        return it != emitters.end() && it->second.Usable() ? &it->second : nullptr;
    }

    size_t Table::UsableCount() const
    {
        size_t n = 0;
        for (const auto& e : emitters) if (e.second.Usable()) ++n;
        return n;
    }

    std::string NormalizePath(const char* path)
    {
        std::string s = path ? path : "";
        for (char& c : s)
        {
            if (c == '/') c = '\\';
            else if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        }
        return s;
    }

    std::vector<std::string> Candidates(const char* modelPath, bool isVirtualKey)
    {
        std::vector<std::string> out;
        const std::string p = NormalizePath(modelPath);
        if (p.empty()) return out;

        // the path without its extension (a dot inside a folder name is not an extension)
        const size_t slash = p.find_last_of('\\');
        const size_t dot = p.find_last_of('.');
        const std::string base = (dot != std::string::npos && (slash == std::string::npos || dot > slash)) ? p.substr(0, dot) : p;
        if (base.empty()) return out;

        // 1. the model itself
        AddUnique(out, base + ".csv");

        // 2. a collection key: <stem>_wxl_...  -> the stem is the original model's path
        const size_t wxl = base.find("_wxl_");
        if (wxl != std::string::npos && wxl > 0)
            AddUnique(out, base.substr(0, wxl) + ".csv");

        // 3. a global key: <stem>[_oh]_<displayId>  -> the stem, with and without the offhand tag
        size_t digits = base.size();
        while (digits > 0 && base[digits - 1] >= '0' && base[digits - 1] <= '9') --digits;
        if (isVirtualKey && digits < base.size() && digits > 1 && base[digits - 1] == '_' &&
            (slash == std::string::npos || digits - 1 > slash + 1))
        {
            const std::string stem = base.substr(0, digits - 1);
            AddUnique(out, stem + ".csv");
            if (EndsWith(stem, "_oh") && stem.size() > 3)
                AddUnique(out, stem.substr(0, stem.size() - 3) + ".csv");
        }
        return out;
    }

    std::shared_ptr<Table> Parse(const char* text, size_t size, std::string* why)
    {
        auto fail = [&](const char* reason) -> std::shared_ptr<Table> { if (why) *why = reason; return nullptr; };
        if (!text || size == 0) return fail("empty file");

        // a UTF-8 byte order mark from a spreadsheet export
        if (size >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        {
            text += 3;
            size -= 3;
        }

        int column[5] = {0, 1, 2, 3, 4};   // role -> column index; the documented order unless a header says otherwise
        auto table = std::make_shared<Table>();
        bool first = true;
        size_t pos = 0;
        uint32_t rows = 0;
        while (pos < size)
        {
            size_t end = pos;
            while (end < size && text[end] != '\n') ++end;
            const std::string line = Trim(std::string(text + pos, end - pos));
            pos = end + 1;
            if (line.empty()) continue;

            const std::vector<std::string> f = SplitLine(line);
            uint32_t probe = 0;
            if (first)
            {
                first = false;
                if (!ParseUInt(f[0], probe))   // not a number: a header row
                {
                    int found[5] = {-1, -1, -1, -1, -1};
                    for (size_t i = 0; i < f.size(); ++i)
                    {
                        const int role = ColumnRole(f[i]);
                        if (role >= 0 && found[role] < 0) found[role] = int(i);
                    }
                    for (int r = 0; r < 5; ++r) if (found[r] >= 0) column[r] = found[r];
                    continue;
                }
            }

            auto field = [&](int role) -> const std::string& {
                static const std::string none;
                const int c = column[role];
                return c >= 0 && size_t(c) < f.size() ? f[size_t(c)] : none;
            };

            uint32_t id = 0, slot = 0;
            if (!ParseUInt(field(0), id) || id > kMaxEmitterId) continue;
            if (!ParseUInt(field(1), slot) || slot >= kMaxLayers) continue;
            std::string path = field(2);
            if (path.empty()) continue;
            for (char& c : path) if (c == '/') c = '\\';

            Emitter& e = table->emitters[id];
            e.texture[slot] = path;
            if (IsYes(field(3))) e.multiTextured = true;
            uint32_t blend = 0;
            if (ParseUInt(field(4), blend)) e.blending = int(blend);
            ++rows;
        }
        if (rows == 0) return fail("no row with an emitter ID, a texture slot 0..2 and a path");
        return table;
    }

    namespace
    {
        struct Settings
        {
            bool       enabled = false;
            LayerBlend blend = LayerBlend::Multiply;
            uint32_t   gain = 2;
        };

        // The three [Particles] settings as the ini has them now. Re-read when the ini changes (a save, or a console
        // override), so the CSVs can be switched on and off, and the blend and gain tried, without a restart. The old
        // Settings are never freed: a thread that fetched them a moment ago may still be reading them.
        const Settings& Current()
        {
            static std::atomic<const Settings*> current{nullptr};
            static std::atomic<uint32_t> seen{0xFFFFFFFFu};
            static std::mutex m;
            const uint32_t gen = wxl::ini::Generation();
            if (seen.load(std::memory_order_acquire) != gen || !current.load(std::memory_order_acquire))
            {
                std::lock_guard<std::mutex> lock(m);
                if (seen.load(std::memory_order_relaxed) != gen || !current.load(std::memory_order_relaxed))
                {
                    auto* s = new Settings();
                    s->enabled = wxl::ini::GetInt("Particles", "MultiTextureCsv", 0) != 0;

                    std::string v = Lower(Trim(wxl::ini::GetText("Particles", "MultiTextureBlend", "Multiply")));
                    // a trailing comment (";") is not part of the value; anything not clearly "add" is the default
                    const size_t semi = v.find(';');
                    if (semi != std::string::npos) v = Trim(v.substr(0, semi));
                    s->blend = (v == "add" || v == "additive" || v == "plain" || v == "sum") ? LayerBlend::Add
                             : (v == "mask") ? LayerBlend::Mask : LayerBlend::Multiply;

                    const int gain = wxl::ini::GetInt("Particles", "MultiTextureGain", 2);
                    s->gain = gain <= 1 ? 1u : (gain >= 3 ? 4u : 2u);

                    const Settings* previous = current.load(std::memory_order_relaxed);
                    if (!previous || previous->enabled != s->enabled || previous->blend != s->blend || previous->gain != s->gain)
                    {
                        wxl::LogCat("PARTICLE", "multi-textured particle CSVs: %s, textures combine by %s%s", s->enabled ? "on" : "off",
                                    s->blend == LayerBlend::Add ? "adding" : s->blend == LayerBlend::Mask ? "masking with texture 0" : "multiplying",
                                    s->blend != LayerBlend::Add ? (s->gain == 1 ? " (gain 1)" : s->gain == 4 ? " (gain 4)" : " (gain 2)") : "");
                    }
                    if (!previous || previous->enabled != s->enabled)
                    {
                        std::lock_guard<std::mutex> cacheLock(g_lock);
                        g_cache.clear();   // models seen while it was off were not cached; the ones seen on may now be stale
                    }

                    current.store(s, std::memory_order_release);
                    seen.store(gen, std::memory_order_release);
                }
            }
            return *current.load(std::memory_order_acquire);
        }
    }

    bool       Enabled() { return Current().enabled; }
    LayerBlend Blend() { return Current().blend; }
    uint32_t   Gain() { return Current().gain; }

    std::shared_ptr<const Table> For(const char* modelPath)
    {
        if (!Enabled() || !modelPath || !*modelPath) return nullptr;
        const std::string key = NormalizePath(modelPath);

        {
            std::lock_guard<std::mutex> g(g_lock);
            const auto it = g_cache.find(key);
            if (it != g_cache.end()) return it->second;
        }

        // Not seen before: read with no lock held (archive reads can be slow, and loader threads must not queue
        // behind them). Two threads may both read the same model; the answers are identical.
        std::shared_ptr<const Table> found;
        std::string why;
        for (const std::string& cand : Candidates(modelPath, wxl::storage::IsVirtualKey(modelPath)))
        {
            std::vector<uint8_t> bytes;
            if (!wxl::storage::ReadNativeQuiet(cand.c_str(), bytes) || bytes.empty()) continue;
            std::string reason;
            std::shared_ptr<Table> t = Parse(reinterpret_cast<const char*>(bytes.data()), bytes.size(), &reason);
            if (!t)
            {
                why = cand + ": " + reason;   // keep looking; report it if nothing better turns up
                continue;
            }
            t->source = cand;
            found = std::move(t);
            break;
        }

        if (found)
            wxl::LogCat("PARTICLE", "%s: %s -> %u of %u emitter(s) multi-textured", key.c_str(), found->source.c_str(),
                        unsigned(found->UsableCount()), unsigned(found->emitters.size()));
        else if (!why.empty())
            wxl::LogCat("PARTICLE", "%s: ignored, %s", key.c_str(), why.c_str());

        std::lock_guard<std::mutex> g(g_lock);
        if (g_cache.size() >= kMaxCached) g_cache.clear();
        g_cache[key] = found;
        return found;
    }
}
