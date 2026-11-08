// The spell area indicator, as a module. See SpellArea.md.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// Draws the ground area of spells being cast, channeled or lying on the ground (a dynamic object) as colored,
// terrain-hugging circles, cones and lines, with the client's own projected texture pass (the one that draws the spell
// targeting circle under the cursor). The core answers what the object manager and the spell data know and does the
// drawing (spellAreaDraw); everything else, the ini, the csv files, the shapes and their textures, lives here.
// Off unless [SpellArea] Enabled=1 in WXLSetup.ini.

#include "SpellAreaShapes.h"
#include "wxl_module.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace wxl::spellarea
{
    namespace
    {
        // ------------------------------------------------------------------------------------------------ config
        struct Config
        {
            bool  enabled      = false;
            bool  own          = true;    // the player's own casts and areas
            bool  others       = false;   // everybody else's too
            bool  enemiesOnly  = false;   // only what hostile units cast: own and friendly areas are left out
            bool  precast      = true;    // the area of a spell that is being cast
            bool  channel      = true;    // the area of a spell that is being channeled
            bool  persistent   = true;    // the dynamic objects (Blizzard, Consecration, Death and Decay ...)
            bool  cones        = true;
            float opacity      = 0.65f;
            float maxOpacity   = 1.0f;    // ceiling for every indicator, whatever else says
            bool  chain        = true;    // chain spells: the circle of the first jump around the target
            float chainRadius  = 12.5f;
            float harmful[3]   = { 1.0f, 0.235f, 0.157f };
            float helpful[3]   = { 0.235f, 1.0f, 0.353f };
            float neutral[3]   = { 1.0f, 0.784f, 0.235f };
            float coneAngle    = 90.0f;
            float minRadius    = 1.0f;
            float maxRadius    = 60.0f;
            int   maxAreas     = 16;
            std::string overrideFile = "SpellAreaOverride.csv";
            std::string jumpFile     = "WXL\\SpellJumpDistance.csv";   // export of the server's spell_jump_distance table
            std::string coneFile     = "WXL\\SpellCone.csv";           // spell id, cone degrees; no header
        };

        const char* const kSection = "SpellArea";

        Config            g_cfg;
        OverrideTable     g_overrides;
        JumpDistanceTable g_jump;
        ConeAngleTable    g_cone;
        bool              g_loaded = false;
        uint32_t          g_lastPoll = 0;
        uint64_t          g_iniStamp = 0, g_csvStamp = 0, g_jumpStamp = 0, g_coneStamp = 0;

        const WxlApi* Api() { return wxl::module::Api(); }

        // The folder the default files live under: the client's working directory, which is its executable's folder.
        bool FolderPath(const std::string& name, char* path, size_t cap)
        {
            if (name.find(':') != std::string::npos || name.find('\\') == 0)   // a full path as it is
                return std::snprintf(path, cap, "%s", name.c_str()) > 0;
            std::string dir = wxl::WorkingDirectory();
            if (dir.empty()) return false;
            if (dir.back() != '\\' && dir.back() != '/') dir += '\\';
            return std::snprintf(path, cap, "%s%s", dir.c_str(), name.c_str()) > 0;
        }

        uint64_t FileStamp(const char* path)
        {
#ifdef _WIN32
            WIN32_FILE_ATTRIBUTE_DATA d;
            if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return 0;
            return (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
#else
            std::ifstream f(path, std::ios::binary | std::ios::ate);   // no timestamp without the OS: size, so a change shows
            return f ? static_cast<uint64_t>(f.tellg()) + 1 : 0;
#endif
        }

        int IniInt(const char* key, int def) { return Api()->iniGetInt(kSection, key, def); }

        std::string IniStr(const char* key)
        {
            char out[260] = {};
            return Api()->iniGetString(kSection, key, out, sizeof(out)) ? std::string(out) : std::string();
        }

        float IniFloat(const char* key, float def)
        {
            std::string t = IniStr(key);
            if (t.empty()) return def;
            for (char& c : t) if (c == ',') c = '.';
            char* end = nullptr;
            const double v = std::strtod(t.c_str(), &end);
            return (end == t.c_str() || !std::isfinite(v)) ? def : static_cast<float>(v);
        }

        void ReadColor(const char* key, float rgb[3])
        {
            const std::string s = IniStr(key);
            if (s.empty()) return;
            float c[3];
            if (ParseColorHex(s.c_str(), c)) { rgb[0] = c[0]; rgb[1] = c[1]; rgb[2] = c[2]; }
            else wxl::LogCat("AOE", "[SpellArea] %s='%s' is not RRGGBB; ignored", key, s.c_str());
        }

        float Clamp(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

        // EnemiesOnly shows what hostile units cast and nothing else: your own areas are left out and everybody else's
        // are looked at (a hostile caster is one whose reaction toward the player is unfriendly or worse).
        bool OwnOn()    { return g_cfg.own && !g_cfg.enemiesOnly; }
        bool OthersOn() { return g_cfg.others || g_cfg.enemiesOnly; }

        void LoadConfig()
        {
            Config c;
            c.enabled     = IniInt("Enabled", 0) != 0;
            c.own         = IniInt("Own", 1) != 0;
            c.others      = IniInt("Others", 0) != 0;
            c.enemiesOnly = IniInt("EnemiesOnly", 0) != 0;
            c.precast     = IniInt("Precast", 1) != 0;
            c.channel     = IniInt("Channel", 1) != 0;
            c.persistent  = IniInt("Persistent", 1) != 0;
            c.cones       = IniInt("Cones", 1) != 0;
            c.opacity     = Clamp(IniFloat("Opacity", c.opacity), 0.05f, 1.0f);
            c.maxOpacity  = Clamp(IniFloat("MaxOpacity", c.maxOpacity), 0.05f, 1.0f);
            c.chain       = IniInt("Chain", 1) != 0;
            c.chainRadius = Clamp(IniFloat("ChainRadius", c.chainRadius), 1.0f, 60.0f);
            c.coneAngle   = Clamp(IniFloat("ConeAngle", c.coneAngle), 5.0f, 180.0f);
            c.minRadius   = Clamp(IniFloat("MinRadius", c.minRadius), 0.1f, 50.0f);
            c.maxRadius   = Clamp(IniFloat("MaxRadius", c.maxRadius), c.minRadius, 150.0f);
            c.maxAreas    = std::max(1, std::min(64, IniInt("MaxAreas", c.maxAreas)));
            ReadColor("HarmfulColor", c.harmful);
            ReadColor("HelpfulColor", c.helpful);
            ReadColor("NeutralColor", c.neutral);
            const std::string file = IniStr("OverrideFile");
            if (!file.empty()) c.overrideFile = file;
            const std::string jump = IniStr("JumpDistanceFile");
            if (!jump.empty()) c.jumpFile = jump;
            const std::string cone = IniStr("ConeFile");
            if (!cone.empty()) c.coneFile = cone;
            const bool was = g_cfg.enabled;
            g_cfg = c;
            if (!g_loaded || was != c.enabled)
                wxl::LogCat("AOE", "spell area indicator: %s (%s, precast %d, channel %d, persistent %d, cones %d, chain %d, opacity %.2f max %.2f)",
                            c.enabled ? "on" : "off",
                            c.enemiesOnly ? "enemies only" : c.own && c.others ? "own and others" : c.others ? "others" : c.own ? "own" : "nobody",
                            c.precast, c.channel, c.persistent, c.cones, c.chain, c.opacity, c.maxOpacity);
        }

        // One csv the module reads: its text, or false when the file is not there (they are all optional).
        bool ReadCsv(const std::string& name, uint64_t& stamp, std::string& text)
        {
            char path[1024] = {};
            if (!FolderPath(name, path, sizeof(path))) return false;
            stamp = FileStamp(path);
            if (!stamp) return false;
            std::ifstream f(path, std::ios::binary);
            text.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            return true;
        }

        void LogProblems(const std::string& name, const std::string& problems)
        {
            size_t at = 0;
            while (at < problems.size())
            {
                const size_t eol = problems.find('\n', at);
                wxl::LogCat("AOE", "%s %s", name.c_str(), problems.substr(at, eol - at).c_str());
                if (eol == std::string::npos) break;
                at = eol + 1;
            }
        }

        void LoadOverrides()
        {
            g_overrides = OverrideTable{};
            std::string text, problems;
            if (!ReadCsv(g_cfg.overrideFile, g_csvStamp, text)) return;
            g_overrides.Parse(text, problems);
            wxl::LogCat("AOE", "%s: %zu row(s)", g_cfg.overrideFile.c_str(), g_overrides.Rows());
            LogProblems(g_cfg.overrideFile, problems);
        }

        void LoadJumpDistances()
        {
            g_jump = JumpDistanceTable{};
            std::string text, problems;
            if (!ReadCsv(g_cfg.jumpFile, g_jumpStamp, text)) return;
            g_jump.Parse(text, problems);
            wxl::LogCat("AOE", "%s: %zu spell(s) with a jump distance", g_cfg.jumpFile.c_str(), g_jump.Rows());
            LogProblems(g_cfg.jumpFile, problems);
        }

        void LoadConeAngles()
        {
            g_cone = ConeAngleTable{};
            std::string text, problems;
            if (!ReadCsv(g_cfg.coneFile, g_coneStamp, text)) return;
            g_cone.Parse(text, problems);
            wxl::LogCat("AOE", "%s: %zu spell(s) with a cone angle", g_cfg.coneFile.c_str(), g_cone.Rows());
            LogProblems(g_cfg.coneFile, problems);
        }

        bool FileChanged(const std::string& name, uint64_t last)
        {
            char path[1024] = {};
            return FolderPath(name, path, sizeof(path)) && FileStamp(path) != last;
        }

        // Reads the ini (and the csv files) again whenever they are saved; looks once a second.
        void PollConfig(uint32_t now)
        {
            if (g_loaded && static_cast<int32_t>(now - g_lastPoll) < 1000) return;
            g_lastPoll = now;

            const uint64_t stamp = Api()->iniStamp ? Api()->iniStamp() : 0;
            const bool iniChanged = !g_loaded || stamp != g_iniStamp;
            if (iniChanged) { g_iniStamp = stamp; LoadConfig(); }

            if (iniChanged || FileChanged(g_cfg.overrideFile, g_csvStamp)) LoadOverrides();
            if (iniChanged || FileChanged(g_cfg.jumpFile, g_jumpStamp))    LoadJumpDistances();
            if (iniChanged || FileChanged(g_cfg.coneFile, g_coneStamp))    LoadConeAngles();
            g_loaded = true;
        }

        // --------------------------------------------------------------------------------------------- the areas
        struct Area
        {
            Shape    shape;
            bool     follow = false;     // the position (and facing) come from `guid` every frame
            uint64_t guid = 0;
            float    pos[3] = {};        // a fixed ground point, when !follow
            float    facing = 0;         // a fixed facing, when !follow
            float    strength = 1.0f;    // 0..1: how far the cast is (an area that is only being cast fades in)
        };

        std::vector<Area> g_areas;
        uint32_t          g_lastCollect = 0;

        constexpr uint32_t kMaxScan = 128;   // dynamic objects / casting units asked of the core per collection

        // The shapes a spell covers: the override file's rows when it has any for the spell, else the effects'.
        size_t ShapesOf(uint32_t spellId, uint64_t casterGuid, Shape out[8])
        {
            const ShapeRules rules{ g_cfg.coneAngle, g_cfg.minRadius, g_cfg.maxRadius, g_cfg.cones, g_cfg.chain, g_cfg.chainRadius };

            bool none = false;
            std::vector<Shape> rows;
            if (g_overrides.Find(spellId, none, rows))
            {
                size_t n = 0;
                for (const Shape& s : rows)
                    if (n < 8 && s.radius >= g_cfg.minRadius) { out[n] = s; out[n].radius = std::min(s.radius, g_cfg.maxRadius); ++n; }
                return n;
            }

            WxlSpellAreaEffects raw;
            if (!spellId || !Api()->spellAreaEffects(spellId, casterGuid, &raw)) return 0;
            SpellEffects effects;
            for (int i = 0; i < 3; ++i)
            {
                effects.effect[i]       = raw.effect[i];
                effects.targetA[i]      = raw.targetA[i];
                effects.targetB[i]      = raw.targetB[i];
                effects.radius[i]       = raw.radius[i];
                effects.chainTargets[i] = raw.chainTargets[i];
            }
            float jumpYards = 0;
            if (g_jump.Find(spellId, jumpYards)) effects.chainRadius = jumpYards;   // the server's own jump distance first
            float coneDegrees = 0;
            if (g_cone.Find(spellId, coneDegrees)) effects.coneAngle = coneDegrees; // and its own cone angle
            Shape derived[3];
            const size_t n = ShapesFromSpell(effects, rules, derived);
            for (size_t i = 0; i < n; ++i) out[i] = derived[i];
            return n;
        }

        struct DynKey { uint64_t caster; uint32_t spell; };

        void AddArea(const Area& a)
        {
            if (static_cast<int>(g_areas.size()) < g_cfg.maxAreas) g_areas.push_back(a);
        }

        void AddDynamicObject(const WxlSpellAreaDynObject& d, std::vector<DynKey>& dyn)
        {
            const bool own = d.casterIsPlayer != 0;
            if (own ? !OwnOn() : !OthersOn()) return;
            if (!(d.radius >= g_cfg.minRadius)) return;

            const bool hostile = d.casterKnown && d.casterHostile;
            if (g_cfg.enemiesOnly && !hostile) return;   // a caster that is gone is not known to be an enemy

            dyn.push_back(DynKey{ d.casterGuid, d.spellId });
            if (!g_cfg.persistent) return;

            Area a;
            a.shape.kind = ShapeKind::Circle;
            a.shape.radius = std::min(d.radius, g_cfg.maxRadius);
            a.shape.intent = Intent::Neutral;
            Shape derived[8];
            if (ShapesOf(d.spellId, d.casterGuid, derived) > 0) a.shape.intent = derived[0].intent;
            if (!own && hostile) a.shape.intent = Intent::Harmful;
            a.follow = true;
            a.guid = d.guid;
            AddArea(a);
        }

        // The area of what one unit is casting or channeling right now.
        void AddCasts(const WxlSpellAreaCaster& unit, const std::vector<DynKey>& dyn, uint32_t now)
        {
            struct Cast { uint32_t spell; float strength; bool precast; };
            Cast casts[2];
            int count = 0;
            if (g_cfg.precast && unit.castSpell && static_cast<int32_t>(now - unit.castEndMs) < 0)
            {
                const float total = static_cast<float>(unit.castEndMs - unit.castStartMs);
                const float done = static_cast<float>(now - unit.castStartMs);
                casts[count++] = Cast{ unit.castSpell, total > 1.0f ? Clamp(done / total, 0.0f, 1.0f) : 1.0f, true };
            }
            if (g_cfg.channel && unit.channelSpell && static_cast<int32_t>(now - unit.channelEndMs) < 0)
                casts[count++] = Cast{ unit.channelSpell, 1.0f, false };

            for (int k = 0; k < count; ++k)
            {
                Shape shapes[8];
                const size_t n = ShapesOf(casts[k].spell, unit.guid, shapes);
                if (!n) continue;

                bool lyingOnTheGround = false;   // a dynamic object of this cast exists: it draws the real area
                for (const DynKey& d : dyn)
                    if (d.caster == unit.guid && d.spell == casts[k].spell) { lyingOnTheGround = true; break; }

                WxlSpellAreaPending data = {};
                const bool haveData = Api()->spellAreaPendingCast(unit.guid, casts[k].spell, &data) != 0;

                for (size_t i = 0; i < n; ++i)
                {
                    if (lyingOnTheGround && shapes[i].kind == ShapeKind::Circle) continue;

                    Area a;
                    a.shape = shapes[i];
                    a.strength = casts[k].strength;
                    if (shapes[i].kind != ShapeKind::Circle || shapes[i].anchor == Anchor::Caster)
                    {
                        a.follow = true;
                        a.guid = unit.guid;
                    }
                    else if (haveData && (data.targetFlags & WXL_SPELLAREA_TARGET_DEST))   // a ground point: Blizzard, Flamestrike ...
                    {
                        std::memcpy(a.pos, data.dest, sizeof(a.pos));
                    }
                    else if (haveData && data.objectTarget)                                // around the target unit
                    {
                        a.follow = true;
                        a.guid = data.objectTarget;
                    }
                    else if (!casts[k].precast && unit.channelObject)                      // around the channel's target
                    {
                        a.follow = true;
                        a.guid = unit.channelObject;
                    }
                    else if (unit.target)                                                  // no cast data: around what the caster has targeted
                    {
                        a.follow = true;
                        a.guid = unit.target;
                    }
                    else
                    {
                        continue;                                                          // nothing to place it on
                    }
                    AddArea(a);
                }
            }
        }

        void Collect(uint32_t now)
        {
            g_areas.clear();

            static WxlSpellAreaDynObject dynBuf[kMaxScan];
            static WxlSpellAreaCaster    casterBuf[kMaxScan];

            std::vector<DynKey> dyn;
            const uint32_t dynCount = Api()->spellAreaDynamicObjects(dynBuf, kMaxScan);
            for (uint32_t i = 0; i < dynCount; ++i) AddDynamicObject(dynBuf[i], dyn);

            const uint32_t casterCount = Api()->spellAreaCasters(casterBuf, kMaxScan);   // the player first
            for (uint32_t i = 0; i < casterCount; ++i)
            {
                const WxlSpellAreaCaster& c = casterBuf[i];
                if (c.isPlayer ? !OwnOn() : !(OthersOn() && (!g_cfg.enemiesOnly || c.hostile))) continue;
                AddCasts(c, dyn, now);
            }
        }

        // ------------------------------------------------------------------------------------------- the textures
        constexpr int kTexSize = 256;

        std::map<uint64_t, void*> g_textures;   // shape kind + parameter -> the core's texture handle

        void* TextureFor(ShapeKind kind, uint32_t param)
        {
            const uint64_t key = (static_cast<uint64_t>(kind) << 32) | param;
            auto it = g_textures.find(key);
            if (it != g_textures.end()) return it->second;

            std::vector<uint32_t> pixels;
            switch (kind)
            {
            case ShapeKind::Circle: RenderCircleTexture(pixels, kTexSize); break;
            case ShapeKind::Cone:   RenderConeTexture(pixels, kTexSize, static_cast<float>(param)); break;
            case ShapeKind::Line:   RenderLineTexture(pixels, kTexSize, static_cast<float>(param) / 256.0f); break;
            }
            void* tex = Api()->spellAreaCreateTexture(pixels.data(), kTexSize, "WxlSpellArea");
            g_textures.emplace(key, tex);
            return tex;
        }

        // ------------------------------------------------------------------------------------------------ drawing
        void DrawArea(const Area& a, const float pos[3], float facing, uint32_t triFlags, uint32_t projectFlags)
        {
            const Shape& s = a.shape;
            const float half = s.radius;
            uint32_t param = 0;
            switch (s.kind)
            {
            case ShapeKind::Circle: break;
            case ShapeKind::Cone:   param = static_cast<uint32_t>(Clamp(std::floor(s.param + 0.5f), 1.0f, 359.0f)); break;
            case ShapeKind::Line:
            {
                const float frac = Clamp(s.param / (2.0f * s.radius), 1.0f / 256.0f, 1.0f);
                param = static_cast<uint32_t>(std::floor(frac * 256.0f + 0.5f));
                break;
            }
            }

            void* texture = TextureFor(s.kind, param);
            if (!texture || !(half > 0.0f)) return;

            const float* rgb = s.intent == Intent::Harmful ? g_cfg.harmful : s.intent == Intent::Helpful ? g_cfg.helpful : g_cfg.neutral;
            const float alpha = std::min(g_cfg.opacity * (0.45f + 0.55f * a.strength), g_cfg.maxOpacity);

            WxlSpellAreaDraw d = {};
            d.structSize   = sizeof(d);
            d.texture      = texture;
            d.pos[0]       = pos[0];
            d.pos[1]       = pos[1];
            d.pos[2]       = pos[2];
            d.half         = half;
            d.facing       = facing;
            d.rotate       = s.kind != ShapeKind::Circle ? 1u : 0u;
            d.b            = static_cast<uint8_t>(rgb[2] * 255.0f + 0.5f);
            d.g            = static_cast<uint8_t>(rgb[1] * 255.0f + 0.5f);
            d.r            = static_cast<uint8_t>(rgb[0] * 255.0f + 0.5f);
            d.a            = static_cast<uint8_t>(Clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
            d.triFlags     = triFlags;
            d.projectFlags = projectFlags;
            Api()->spellAreaDraw(&d);
        }

        // The world render's call: once after the opaque pass and once after the transparent ones.
        void OnRender(void*, uint32_t triFlags, uint32_t projectFlags, uint32_t now)
        {
            PollConfig(now);
            if (!g_cfg.enabled) return;

            if (static_cast<int32_t>(now - g_lastCollect) >= 66 || g_lastCollect == 0)
            {
                Collect(now);
                g_lastCollect = now ? now : 1;
            }

            for (const Area& a : g_areas)
            {
                float pos[3] = { a.pos[0], a.pos[1], a.pos[2] };
                float facing = a.facing;
                if (a.follow && !Api()->spellAreaObjectPose(a.guid, pos, &facing)) continue;
                DrawArea(a, pos, facing, triFlags, projectFlags);
            }
        }
    }

    bool Start()
    {
        Api()->setSpellAreaRenderer(&OnRender, nullptr);
        return true;
    }

    void Stop()
    {
        const WxlApi* api = Api();
        if (api && WXL_API_HAS(api, setSpellAreaRenderer)) api->setSpellAreaRenderer(nullptr, nullptr);
        g_areas.clear();
        g_textures.clear();
    }
}
