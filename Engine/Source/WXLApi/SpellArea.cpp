// The spell area indicator's engine side. See SpellArea.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "Engine/Source/WXLApi/SpellArea.h"

#ifdef WXL_ALLOW_32BIT_HEADER   // the module API is 64-bit only: a 32-bit client keeps a no-op

namespace wxl::spellarea
{
    void Render(uint32_t, uint32_t) {}
}

#else

#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gx/Types.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/CAaBox.h"
#include "Engine/Source/Tempest/CImVector.h"
#include "Engine/Source/WXL/Log.h"
#include "Engine/Source/WXL/SpellAreaShapes.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/SpellRadiusRec.h"
#include "WoW/Source/DB/Rec/SpellRec.h"
#include "WoW/Source/Game/GameClient/ProjectTex.h"
#include "WoW/Source/Magic/MagicClient/Spell_C.h"
#include "WoW/Source/Object/ObjectClient/DynamicObject_C.h"
#include "WoW/Source/Object/ObjectClient/Object_C.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"

#ifdef _WIN32
#include <windows.h>
#endif

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
            bool  precast      = true;    // the area of a spell that is being cast
            bool  channel      = true;    // the area of a spell that is being channeled
            bool  persistent   = true;    // the dynamic objects (Blizzard, Consecration, Death and Decay ...)
            bool  cones        = true;
            float opacity      = 0.65f;
            float harmful[3]   = { 1.0f, 0.235f, 0.157f };
            float helpful[3]   = { 0.235f, 1.0f, 0.353f };
            float neutral[3]   = { 1.0f, 0.784f, 0.235f };
            float coneAngle    = 90.0f;
            float minRadius    = 1.0f;
            float maxRadius    = 60.0f;
            int   maxAreas     = 16;
            std::string overrideFile = "SpellAreaOverride.csv";
        };

        Config                g_cfg;
        OverrideTable         g_overrides;
        bool                  g_loaded = false;
        uint32_t              g_lastPoll = 0;
        uint64_t              g_iniStamp = 0, g_csvStamp = 0;

#ifdef _WIN32
        bool ExeFolderPath(const char* name, char* path, size_t cap)
        {
            const DWORD n = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(cap));
            if (n == 0 || n >= cap) return false;
            char* slash = std::strrchr(path, '\\');
            if (!slash || static_cast<size_t>(slash - path) + 1 + std::strlen(name) >= cap) return false;
            std::strcpy(slash + 1, name);
            return true;
        }

        uint64_t FileStamp(const char* path)
        {
            WIN32_FILE_ATTRIBUTE_DATA d;
            if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return 0;
            return (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
        }

        int IniInt(const char* path, const char* key, int def)
        {
            return static_cast<int>(GetPrivateProfileIntA("SpellArea", key, def, path));
        }

        std::string IniStr(const char* path, const char* key)
        {
            char out[260] = {};
            GetPrivateProfileStringA("SpellArea", key, "", out, sizeof(out), path);
            return out;
        }
#endif

        float IniFloat(const char* path, const char* key, float def)
        {
#ifdef _WIN32
            const std::string s = IniStr(path, key);
            if (s.empty()) return def;
            std::string t = s;
            for (char& c : t) if (c == ',') c = '.';
            char* end = nullptr;
            const double v = std::strtod(t.c_str(), &end);
            return (end == t.c_str() || !std::isfinite(v)) ? def : static_cast<float>(v);
#else
            (void)path; (void)key;
            return def;
#endif
        }

        void ReadColor(const char* path, const char* key, float rgb[3])
        {
#ifdef _WIN32
            const std::string s = IniStr(path, key);
            if (s.empty()) return;
            float c[3];
            if (ParseColorHex(s.c_str(), c)) { rgb[0] = c[0]; rgb[1] = c[1]; rgb[2] = c[2]; }
            else wxl::LogCat("AOE", "[SpellArea] %s='%s' is not RRGGBB; ignored", key, s.c_str());
#else
            (void)path; (void)key; (void)rgb;
#endif
        }

        float Clamp(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

        void LoadConfig(const char* ini)
        {
#ifdef _WIN32
            Config c;
            c.enabled    = IniInt(ini, "Enabled", 0) != 0;
            c.own        = IniInt(ini, "Own", 1) != 0;
            c.others     = IniInt(ini, "Others", 0) != 0;
            c.precast    = IniInt(ini, "Precast", 1) != 0;
            c.channel    = IniInt(ini, "Channel", 1) != 0;
            c.persistent = IniInt(ini, "Persistent", 1) != 0;
            c.cones      = IniInt(ini, "Cones", 1) != 0;
            c.opacity    = Clamp(IniFloat(ini, "Opacity", c.opacity), 0.05f, 1.0f);
            c.coneAngle  = Clamp(IniFloat(ini, "ConeAngle", c.coneAngle), 5.0f, 180.0f);
            c.minRadius  = Clamp(IniFloat(ini, "MinRadius", c.minRadius), 0.1f, 50.0f);
            c.maxRadius  = Clamp(IniFloat(ini, "MaxRadius", c.maxRadius), c.minRadius, 150.0f);
            c.maxAreas   = std::max(1, std::min(64, IniInt(ini, "MaxAreas", c.maxAreas)));
            ReadColor(ini, "HarmfulColor", c.harmful);
            ReadColor(ini, "HelpfulColor", c.helpful);
            ReadColor(ini, "NeutralColor", c.neutral);
            const std::string file = IniStr(ini, "OverrideFile");
            if (!file.empty()) c.overrideFile = file;
            const bool was = g_cfg.enabled;
            g_cfg = c;
            if (!g_loaded || was != c.enabled)
                wxl::LogCat("AOE", "spell area indicator: %s (own %d, others %d, precast %d, channel %d, persistent %d, cones %d, opacity %.2f)",
                            c.enabled ? "on" : "off", c.own, c.others, c.precast, c.channel, c.persistent, c.cones, c.opacity);
#else
            (void)ini;
#endif
        }

        void LoadOverrides()
        {
#ifdef _WIN32
            g_overrides = OverrideTable{};
            char path[MAX_PATH] = {};
            const std::string& name = g_cfg.overrideFile;
            if (name.find(':') != std::string::npos || name.find('\\') == 0) std::snprintf(path, sizeof(path), "%s", name.c_str());
            else if (!ExeFolderPath(name.c_str(), path, sizeof(path))) return;

            g_csvStamp = FileStamp(path);
            if (!g_csvStamp) return;   // optional file
            std::ifstream f(path, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::string problems;
            g_overrides.Parse(text, problems);
            wxl::LogCat("AOE", "%s: %zu row(s)", name.c_str(), g_overrides.Rows());
            size_t at = 0;
            while (at < problems.size())
            {
                const size_t eol = problems.find('\n', at);
                wxl::LogCat("AOE", "%s %s", name.c_str(), problems.substr(at, eol - at).c_str());
                if (eol == std::string::npos) break;
                at = eol + 1;
            }
#endif
        }

        // Reads the ini (and the override file) again whenever they are saved; looks once a second.
        void PollConfig()
        {
#ifdef _WIN32
            const uint32_t now = OsGetAsyncTimeMs();
            if (g_loaded && static_cast<int32_t>(now - g_lastPoll) < 1000) return;
            g_lastPoll = now;

            char ini[MAX_PATH] = {};
            if (!ExeFolderPath("WXLExtendedEquipment.ini", ini, sizeof(ini))) { g_loaded = true; return; }
            const uint64_t stamp = FileStamp(ini);
            const bool iniChanged = !g_loaded || stamp != g_iniStamp;
            if (iniChanged) { g_iniStamp = stamp; LoadConfig(ini); }

            char csv[MAX_PATH] = {};
            const std::string& name = g_cfg.overrideFile;
            const bool haveCsv = (name.find(':') != std::string::npos || name.find('\\') == 0)
                                     ? (std::snprintf(csv, sizeof(csv), "%s", name.c_str()) > 0)
                                     : ExeFolderPath(name.c_str(), csv, sizeof(csv));
            if (iniChanged || (haveCsv && FileStamp(csv) != g_csvStamp)) LoadOverrides();
            g_loaded = true;
#else
            g_loaded = true;
#endif
        }

        // --------------------------------------------------------------------------------------------- the areas
        struct Area
        {
            Shape    shape;
            bool     follow = false;     // the position (and facing) come from `guid` every frame
            WOWGUID  guid;
            C3Vector pos;                // a fixed ground point, when !follow
            float    facing = 0;         // a fixed facing, when !follow
            float    strength = 1.0f;    // 0..1: how far the cast is (an area that is only being cast fades in)
        };

        std::vector<Area> g_areas;
        uint32_t          g_lastCollect = 0;

        CGObject_C* Object(const WOWGUID& guid)
        {
            return ClntObjMgrObjectPtr(guid, static_cast<TypeMask>(TYPEMASK_OBJECT), __FILE__, __LINE__);
        }

        bool Hostile(CGUnit_C* caster, CGUnit_C* player)
        {
            if (!caster || !player || caster == player) return false;
            return static_cast<int>(caster->UnitReaction(player)) <= static_cast<int>(UNIT_REACTION_UNFRIENDLY);
        }

        // What a spell's three effects say, with the radii worked out for this caster (level and, for the player,
        // the radius modifiers of talents and auras: the same ones the targeting circle uses).
        bool SpellEffectsOf(uint32_t spellId, CGUnit_C* caster, bool own, SpellEffects& out)
        {
            SpellRec spell;
            if (!spellId || !g_spellDB.GetRecord(static_cast<int32_t>(spellId), &spell)) return false;

            const int32_t level = caster && caster->m_unit ? static_cast<int32_t>(caster->m_unit->m_unitFieldLevel) : 0;
            int32_t flat = 0, pct = 100;
            const bool modified = own && Spell_C_GetSpellModifiers(&spell, 6, flat, pct);

            for (int i = 0; i < 3; ++i)
            {
                out.effect[i]  = spell.m_effect[i];
                out.targetA[i] = spell.m_implicitTargetA[i];
                out.targetB[i] = spell.m_implicitTargetB[i];
                out.radius[i]  = 0;

                SpellRadiusRec* rec = g_spellRadiusDB.GetRecord(spell.m_effectRadiusIndex[i]);
                if (!rec) continue;
                float radius = rec->m_radius + static_cast<float>(level) * rec->m_radiusPerLevel;
                if (rec->m_radiusMax > 0.0f) radius = std::min(radius, rec->m_radiusMax);
                if (modified && radius > 0.0f) radius = (radius + static_cast<float>(flat)) * static_cast<float>(pct) * 0.01f;
                out.radius[i] = radius;
            }
            return true;
        }

        // The shapes a spell covers: the override file's rows when it has any for the spell, else the effects'.
        size_t ShapesOf(uint32_t spellId, CGUnit_C* caster, bool own, Shape out[8])
        {
            const ShapeRules rules{ g_cfg.coneAngle, g_cfg.minRadius, g_cfg.maxRadius, g_cfg.cones };

            bool none = false;
            std::vector<Shape> rows;
            if (g_overrides.Find(spellId, none, rows))
            {
                size_t n = 0;
                for (const Shape& s : rows)
                    if (n < 8 && s.radius >= g_cfg.minRadius) { out[n] = s; out[n].radius = std::min(s.radius, g_cfg.maxRadius); ++n; }
                return n;
            }

            SpellEffects effects;
            if (!SpellEffectsOf(spellId, caster, own, effects)) return 0;
            Shape derived[3];
            const size_t n = ShapesFromSpell(effects, rules, derived);
            for (size_t i = 0; i < n; ++i) out[i] = derived[i];
            return n;
        }

        struct DynKey { WOWGUID caster; uint32_t spell; };

        struct Collector
        {
            WOWGUID player;
            CGUnit_C* playerUnit = nullptr;
            std::vector<DynKey> dyn;
            std::vector<WOWGUID> casters;     // other units that are casting or channeling
        };

        void AddArea(const Area& a)
        {
            if (static_cast<int>(g_areas.size()) < g_cfg.maxAreas) g_areas.push_back(a);
        }

        void AddDynamicObject(Collector& c, CGObject_C* obj, const WOWGUID& guid)
        {
            const CGDynamicObjectData* d = static_cast<CGDynamicObject_C*>(obj)->m_dynamicObject;
            if (!d || d->m_bytes[0] == 2) return;                    // type 2 is a far sight focus, not an area
            const bool own = d->m_caster == c.player;
            if (own ? !g_cfg.own : !g_cfg.others) return;
            if (!(d->m_radius >= g_cfg.minRadius)) return;

            c.dyn.push_back(DynKey{ d->m_caster, d->m_spellID });
            if (!g_cfg.persistent) return;

            CGUnit_C* caster = static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(d->m_caster, TYPEMASK_UNIT, __FILE__, __LINE__));
            Area a;
            a.shape.kind = ShapeKind::Circle;
            a.shape.radius = std::min(d->m_radius, g_cfg.maxRadius);
            a.shape.intent = Intent::Neutral;
            Shape derived[8];
            if (ShapesOf(d->m_spellID, caster, own, derived) > 0) a.shape.intent = derived[0].intent;
            if (!own && Hostile(caster, c.playerUnit)) a.shape.intent = Intent::Harmful;
            a.follow = true;
            a.guid = guid;
            AddArea(a);
        }

        int32_t CollectCallback(WOWGUID guid, void* param)
        {
            auto* c = static_cast<Collector*>(param);
            if (guid == c->player) return 1;

            CGObject_C* obj = Object(guid);
            if (!obj || !obj->m_obj) return 1;

            const uint32_t type = static_cast<uint32_t>(obj->m_obj->m_type);
            if (type & TYPEMASK_DYNAMICOBJECT)
            {
                AddDynamicObject(*c, obj, guid);
            }
            else if ((type & TYPEMASK_UNIT) && g_cfg.others)
            {
                CGUnit_C* unit = static_cast<CGUnit_C*>(obj);
                if (unit->m_castingSpellId || unit->m_channelSpellId) c->casters.push_back(guid);
            }
            return 1;
        }

        // The area of what one unit is casting or channeling right now.
        void AddCasts(Collector& c, const WOWGUID& guid, uint32_t now)
        {
            CGUnit_C* unit = static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(guid, TYPEMASK_UNIT, __FILE__, __LINE__));
            if (!unit || !unit->m_unit) return;
            const bool own = guid == c.player;

            struct Cast { uint32_t spell; float strength; bool precast; };
            Cast casts[2];
            int count = 0;
            if (g_cfg.precast && unit->m_castingSpellId && static_cast<int32_t>(now - unit->m_castEndTime) < 0)
            {
                const float total = static_cast<float>(unit->m_castEndTime - unit->m_castStartTime);
                const float done = static_cast<float>(now - unit->m_castStartTime);
                casts[count++] = Cast{ unit->m_castingSpellId, total > 1.0f ? Clamp(done / total, 0.0f, 1.0f) : 1.0f, true };
            }
            if (g_cfg.channel && unit->m_channelSpellId && static_cast<int32_t>(now - unit->m_channelEndTime) < 0)
                casts[count++] = Cast{ unit->m_channelSpellId, 1.0f, false };

            for (int k = 0; k < count; ++k)
            {
                Shape shapes[8];
                const size_t n = ShapesOf(casts[k].spell, unit, own, shapes);
                if (!n) continue;

                bool lyingOnTheGround = false;   // a dynamic object of this cast exists: it draws the real area
                for (const DynKey& d : c.dyn)
                    if (d.caster == guid && d.spell == casts[k].spell) { lyingOnTheGround = true; break; }

                PendingSpellCastData data;
                const bool haveData = Spell_C_WxlFindPendingCast(guid, casts[k].spell, &data);

                for (size_t i = 0; i < n; ++i)
                {
                    if (lyingOnTheGround && shapes[i].kind == ShapeKind::Circle) continue;

                    Area a;
                    a.shape = shapes[i];
                    a.strength = casts[k].strength;
                    if (shapes[i].kind != ShapeKind::Circle || shapes[i].anchor == Anchor::Caster)
                    {
                        a.follow = true;
                        a.guid = guid;
                    }
                    else if (haveData && (data.m_targetFlags & 0x40))          // a ground point: Blizzard, Flamestrike ...
                    {
                        a.pos = data.GetDestLocation();
                    }
                    else if (haveData && data.m_objectTarget.m_guid)           // around the target unit
                    {
                        a.follow = true;
                        a.guid = data.m_objectTarget;
                    }
                    else if (!casts[k].precast && unit->m_unit->m_unitFieldChannelObject.m_guid)   // around the channel's target
                    {
                        a.follow = true;
                        a.guid = unit->m_unit->m_unitFieldChannelObject;
                    }
                    else
                    {
                        continue;                                              // nothing to place it on
                    }
                    AddArea(a);
                }
            }
        }

        void Collect(uint32_t now)
        {
            g_areas.clear();

            Collector c;
            c.player = ClntObjMgrGetActivePlayer();
            if (!c.player) return;
            c.playerUnit = static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(c.player, TYPEMASK_UNIT, __FILE__, __LINE__));
            if (!c.playerUnit || !c.playerUnit->m_unit) return;

            ClntObjMgrEnumVisibleObjects(CollectCallback, &c);

            if (g_cfg.own) AddCasts(c, c.player, now);
            for (const WOWGUID& g : c.casters) AddCasts(c, g, now);
        }

        // ------------------------------------------------------------------------------------------- the textures
        constexpr int kTexSize = 256;

        struct TexEntry
        {
            std::vector<uint32_t> pixels;
            CTexture*             tex = nullptr;
        };
        std::map<uint64_t, TexEntry> g_textures;

        void TextureFeed(EGxTexCommand command, uint32_t width, uint32_t, uint32_t, uint32_t, void* userArg, uint32_t& texelStrideInBytes, const void*& texels)
        {
            if (command == GxTex_Latch)
            {
                texelStrideInBytes = 4 * width;
                texels = userArg;     // the entry's pixels live as long as the texture does
            }
        }

        CGxTex* TextureFor(ShapeKind kind, uint32_t param)
        {
            const uint64_t key = (static_cast<uint64_t>(kind) << 32) | param;
            auto it = g_textures.find(key);
            if (it == g_textures.end())
            {
                TexEntry entry;
                switch (kind)
                {
                case ShapeKind::Circle: RenderCircleTexture(entry.pixels, kTexSize); break;
                case ShapeKind::Cone:   RenderConeTexture(entry.pixels, kTexSize, static_cast<float>(param)); break;
                case ShapeKind::Line:   RenderLineTexture(entry.pixels, kTexSize, static_cast<float>(param) / 256.0f); break;
                }
                it = g_textures.emplace(key, std::move(entry)).first;
                TexEntry& e = it->second;
                e.tex = TextureCreate(GxTex_2d, kTexSize, kTexSize, 0, GxTex_Argb8888, GxTex_Argb8888, CGxTexFlags(GxTex_Linear),
                                      e.pixels.data(), TextureFeed, "WxlSpellArea", 0);
            }
            return it->second.tex ? TextureGetGxTex(it->second.tex, 0, nullptr) : nullptr;
        }

        // ------------------------------------------------------------------------------------------------ drawing
        void DrawArea(const Area& a, const C3Vector& pos, float facing, uint32_t triFlags, uint32_t projectFlags)
        {
            const Shape& s = a.shape;
            float half = s.radius;
            uint32_t param = 0;
            switch (s.kind)
            {
            case ShapeKind::Circle: break;
            case ShapeKind::Cone:   param = static_cast<uint32_t>(Clamp(std::floor(s.param + 0.5f), 1.0f, 180.0f)); break;
            case ShapeKind::Line:
            {
                const float frac = Clamp(s.param / (2.0f * s.radius), 1.0f / 256.0f, 1.0f);
                param = static_cast<uint32_t>(std::floor(frac * 256.0f + 0.5f));
                break;
            }
            }

            CGxTex* texture = TextureFor(s.kind, param);
            if (!texture || !(half > 0.0f)) return;

            const float* rgb = s.intent == Intent::Harmful ? g_cfg.harmful : s.intent == Intent::Helpful ? g_cfg.helpful : g_cfg.neutral;
            const float alpha = g_cfg.opacity * (0.45f + 0.55f * a.strength);
            CImVector color;
            color.b = static_cast<uint8_t>(rgb[2] * 255.0f + 0.5f);
            color.g = static_cast<uint8_t>(rgb[1] * 255.0f + 0.5f);
            color.r = static_cast<uint8_t>(rgb[0] * 255.0f + 0.5f);
            color.a = static_cast<uint8_t>(Clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);

            // A square box around the shape's center (a cone's apex, a line's start); the texture is stretched over it
            // and the box's height is how far above and below the center the ground still takes the picture.
            const float hz = Clamp(half * 0.5f, 4.0f, 20.0f);
            CAaBox box(C3Vector(pos.x - half, pos.y - half, pos.z - hz), C3Vector(pos.x + half, pos.y + half, pos.z + hz));

            // The texture's front (row 0) points along the facing once rotated by -facing (see SpellAreaShapes test 5).
            C44Matrix rotation;
            const bool rotate = s.kind != ShapeKind::Circle;
            if (rotate) rotation.RotateAroundZ(-facing);

            g_theGxDevicePtr->RsPush();
            GxRsSet(GxRs_Lighting, 0);
            GxRsSet(GxRs_DepthWrite, 0);
            GxRsSet(GxRs_Culling, 0);
            GxRsSet(GxRs_BlendingMode, 2);
            GxUpdateAlphaRef(GxRs_AlphaRef);
            g_theGxDevicePtr->RsSet(GxRs_Texture0, texture);
            g_theGxDevicePtr->RsSet(GxRs_Texture1, ProjectTex2dGetAddFadeTex());
            GxRsSet(GxRs_ColorOp0, 0);
            GxRsSet(GxRs_AlphaOp0, 0);
            GxRsSet(GxRs_ColorOp1, 0);
            GxRsSet(GxRs_AlphaOp1, 0);
            ProjectTex::Render2d(box, color, rotate ? &rotation : nullptr, 0.5f, triFlags, projectFlags, 0.4f);
            g_theGxDevicePtr->RsPop();
        }
    }

    void Render(uint32_t triFlags, uint32_t projectFlags)
    {
        PollConfig();
        if (!g_cfg.enabled) return;

        const uint32_t now = OsGetAsyncTimeMs();
        if (static_cast<int32_t>(now - g_lastCollect) >= 66 || g_lastCollect == 0)
        {
            Collect(now);
            g_lastCollect = now ? now : 1;
        }

        for (const Area& a : g_areas)
        {
            C3Vector pos = a.pos;
            float facing = a.facing;
            if (a.follow)
            {
                CGObject_C* obj = Object(a.guid);
                if (!obj) continue;
                pos = obj->GetPosition();
                facing = obj->GetFacing();
            }
            DrawArea(a, pos, facing, triFlags, projectFlags);
        }
    }
}

#endif
