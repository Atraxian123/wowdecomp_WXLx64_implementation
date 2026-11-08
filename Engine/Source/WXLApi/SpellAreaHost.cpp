// The core's side of the spell area module's engine access. See SpellAreaHost.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// What the spell area indicator did to the engine while it was compiled into the client (it was SpellArea.cpp in this
// folder): the object manager scan, the spell record reads and the projected texture draw. The decisions moved to the
// module; these functions only answer and draw.

#include "Engine/Source/WXLApi/SpellAreaHost.h"

#ifdef WXL_ALLOW_32BIT_HEADER   // the module API is 64-bit only: a 32-bit client keeps no-ops

namespace wxl::api::spellareahost
{
    uint32_t DynamicObjects(WxlSpellAreaDynObject*, uint32_t) { return 0; }
    uint32_t Casters(WxlSpellAreaCaster*, uint32_t) { return 0; }
    int      Effects(uint32_t, uint64_t, WxlSpellAreaEffects*) { return 0; }
    int      PendingCast(uint64_t, uint32_t, WxlSpellAreaPending*) { return 0; }
    int      ObjectPose(uint64_t, float*, float*) { return 0; }
    void*    CreateTexture(const uint32_t*, uint32_t, const char*) { return nullptr; }
    void     Draw(const WxlSpellAreaDraw*) {}
    void     SetRenderer(WxlSpellAreaRenderFn, void*) {}
    uint32_t CastEvents(uint64_t, WxlSpellAreaCastEvent*, uint32_t) { return 0; }
    int      Triggers(uint32_t, WxlSpellAreaTriggers*) { return 0; }
    uint32_t TriggerAuras(WxlSpellAreaAura*, uint32_t) { return 0; }
    void     Render(uint32_t, uint32_t) {}
    void     NoteSpellGo(uint64_t, uint32_t, uint32_t, uint32_t, const float*, uint64_t, uint32_t) {}
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
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/SpellCastTimesRec.h"
#include "WoW/Source/DB/Rec/SpellRadiusRec.h"
#include "WoW/Source/DB/Rec/SpellRec.h"
#include "WoW/Source/Game/GameClient/ProjectTex.h"
#include "WoW/Source/Magic/MagicClient/Spell_C.h"
#include "WoW/Source/Object/ObjectClient/DynamicObject_C.h"
#include "WoW/Source/Object/ObjectClient/Object_C.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace wxl::api::spellareahost
{
    namespace
    {
        WxlSpellAreaRenderFn g_renderer = nullptr;
        void*                g_rendererUser = nullptr;

        CGObject_C* Object(const WOWGUID& guid)
        {
            return ClntObjMgrObjectPtr(guid, static_cast<TypeMask>(TYPEMASK_OBJECT), __FILE__, __LINE__);
        }

        CGUnit_C* Unit(const WOWGUID& guid)
        {
            return static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(guid, TYPEMASK_UNIT, __FILE__, __LINE__));
        }

        CGUnit_C* PlayerUnit(WOWGUID& guid)
        {
            guid = ClntObjMgrGetActivePlayer();
            if (!guid) return nullptr;
            CGUnit_C* unit = Unit(guid);
            return unit && unit->m_unit ? unit : nullptr;
        }

        // A hostile caster is one whose reaction toward the player is unfriendly or worse.
        bool Hostile(CGUnit_C* caster, CGUnit_C* player)
        {
            if (!caster || !player || caster == player) return false;
            return static_cast<int>(caster->UnitReaction(player)) <= static_cast<int>(UNIT_REACTION_UNFRIENDLY);
        }

        // ---------------------------------------------------------------------------------------- the scans
        struct Scan
        {
            WOWGUID   player = {};
            CGUnit_C* playerUnit = nullptr;
            WxlSpellAreaAura*      auras = nullptr;
            WxlSpellAreaDynObject* dyn = nullptr;
            WxlSpellAreaCaster*    casters = nullptr;
            uint32_t  max = 0;
            uint32_t  count = 0;
        };

        void FillCaster(WxlSpellAreaCaster& c, const WOWGUID& guid, CGUnit_C* unit, const Scan& s)
        {
            c = {};
            c.guid          = guid.m_guid;
            c.isPlayer      = guid == s.player ? 1u : 0u;
            c.hostile       = Hostile(unit, s.playerUnit) ? 1u : 0u;
            c.castSpell     = unit->m_castingSpellId;
            c.castStartMs   = unit->m_castStartTime;
            c.castEndMs     = unit->m_castEndTime;
            c.channelSpell  = unit->m_channelSpellId;
            c.channelEndMs  = unit->m_channelEndTime;
            c.target        = unit->m_unit->m_unitFieldTarget.m_guid;
            c.channelObject = unit->m_unit->m_unitFieldChannelObject.m_guid;
        }

        int32_t DynCallback(WOWGUID guid, void* param)
        {
            auto* s = static_cast<Scan*>(param);
            if (s->count >= s->max || guid == s->player) return 1;

            CGObject_C* obj = Object(guid);
            if (!obj || !obj->m_obj) return 1;
            if (!(static_cast<uint32_t>(obj->m_obj->m_type) & TYPEMASK_DYNAMICOBJECT)) return 1;

            const CGDynamicObjectData* d = static_cast<CGDynamicObject_C*>(obj)->m_dynamicObject;
            if (!d || d->m_bytes[0] == 2) return 1;                  // type 2 is a far sight focus, not an area

            CGUnit_C* caster = Unit(d->m_caster);
            WxlSpellAreaDynObject& o = s->dyn[s->count++];
            o = {};
            o.guid           = guid.m_guid;
            o.casterGuid     = d->m_caster.m_guid;
            o.spellId        = d->m_spellID;
            o.radius         = d->m_radius;
            o.casterIsPlayer = d->m_caster == s->player ? 1u : 0u;
            o.casterKnown    = caster ? 1u : 0u;                     // a caster that is gone is not known to be an enemy
            o.casterHostile  = Hostile(caster, s->playerUnit) ? 1u : 0u;
            return 1;
        }

        int32_t CasterCallback(WOWGUID guid, void* param)
        {
            auto* s = static_cast<Scan*>(param);
            if (s->count >= s->max || guid == s->player) return 1;

            CGObject_C* obj = Object(guid);
            if (!obj || !obj->m_obj) return 1;
            if (!(static_cast<uint32_t>(obj->m_obj->m_type) & TYPEMASK_UNIT)) return 1;

            CGUnit_C* unit = static_cast<CGUnit_C*>(obj);
            if (!unit->m_unit || !(unit->m_castingSpellId || unit->m_channelSpellId)) return 1;
            FillCaster(s->casters[s->count++], guid, unit, *s);
            return 1;
        }

        // ------------------------------------------------------------------------------------ triggered spells
        // The spell data never changes while the client runs: what a spell triggers is worked out once.
        constexpr int32_t kAuraPeriodicTrigger = 23, kAuraPeriodicTriggerWithValue = 226;
        constexpr int32_t kEffectTriggerMissile = 32, kEffectTriggerSpell = 64, kEffectTriggerSpellWithValue = 142, kEffectTriggerMissileWithValue = 145;

        bool ReadTriggers(uint32_t spellId, WxlSpellAreaTriggers* out)
        {
            SpellRec spell;
            if (!spellId || !g_spellDB.GetRecord(static_cast<int32_t>(spellId), &spell)) return false;

            WxlSpellAreaTriggers t = {};
            t.speed = spell.m_speed > 0.0f ? spell.m_speed : 0.0f;
            for (uint32_t i = 0; i < 3; ++i)
            {
                const int32_t triggered = spell.m_effectTriggerSpell[i];
                if (triggered <= 0 || static_cast<uint32_t>(triggered) == spellId) continue;
                SpellRec target;
                if (!g_spellDB.GetRecord(triggered, &target)) continue;

                uint32_t kind = 0;
                const int32_t effect = spell.m_effect[i];
                if (effect == kEffectTriggerMissile || effect == kEffectTriggerMissileWithValue) kind = WXL_SPELLAREA_TRIGGER_MISSILE;
                else if (effect == kEffectTriggerSpell || effect == kEffectTriggerSpellWithValue) kind = WXL_SPELLAREA_TRIGGER_CAST;
                else if (spell.m_effectAura[i] == kAuraPeriodicTrigger || spell.m_effectAura[i] == kAuraPeriodicTriggerWithValue) kind = WXL_SPELLAREA_TRIGGER_PERIODIC;
                if (!kind) continue;

                WxlSpellAreaTrigger& e = t.trigger[t.count++];
                e.kind        = kind;
                e.spellId     = static_cast<uint32_t>(triggered);
                e.periodMs    = kind == WXL_SPELLAREA_TRIGGER_PERIODIC && spell.m_effectAuraPeriod[i] > 0 ? static_cast<uint32_t>(spell.m_effectAuraPeriod[i]) : 0;
                e.effectIndex = i;
            }
            if (!t.count) return false;
            *out = t;
            return true;
        }

        // Whether an aura's spell casts another periodically (cached: the scan asks for every aura of every unit).
        bool CastsPeriodically(int32_t spellId)
        {
            static std::unordered_map<int32_t, bool> cache;
            auto it = cache.find(spellId);
            if (it != cache.end()) return it->second;
            WxlSpellAreaTriggers t;
            bool yes = false;
            if (ReadTriggers(static_cast<uint32_t>(spellId), &t))
                for (uint32_t i = 0; i < t.count; ++i) if (t.trigger[i].kind == WXL_SPELLAREA_TRIGGER_PERIODIC) yes = true;
            cache.emplace(spellId, yes);
            return yes;
        }

        int32_t AuraCallback(WOWGUID guid, void* param)
        {
            auto* s = static_cast<Scan*>(param);
            if (s->count >= s->max) return 1;

            CGObject_C* obj = Object(guid);
            if (!obj || !obj->m_obj) return 1;
            if (!(static_cast<uint32_t>(obj->m_obj->m_type) & TYPEMASK_UNIT)) return 1;

            CGUnit_C* unit = static_cast<CGUnit_C*>(obj);
            if (!unit->m_unit) return 1;
            const uint32_t auraCount = unit->GetAuraCount();
            for (uint32_t i = 0; i < auraCount && s->count < s->max; ++i)
            {
                AuraData* aura = unit->GetAura(i);
                if (!aura || aura->m_spellId <= 0 || !CastsPeriodically(aura->m_spellId)) continue;

                CGUnit_C* caster = aura->m_creator.m_guid ? Unit(aura->m_creator) : nullptr;
                WxlSpellAreaAura& a = s->auras[s->count++];
                a = {};
                a.holderGuid     = guid.m_guid;
                a.casterGuid     = aura->m_creator.m_guid;
                a.spellId        = static_cast<uint32_t>(aura->m_spellId);
                a.casterIsPlayer = aura->m_creator == s->player ? 1u : 0u;
                a.casterHostile  = Hostile(caster, s->playerUnit) ? 1u : 0u;
            }
            return 1;
        }

        // ------------------------------------------------------------------------------------- the textures
        struct TexEntry
        {
            std::vector<uint32_t> pixels;
            CTexture*             tex = nullptr;
        };
        std::vector<std::unique_ptr<TexEntry>> g_textures;   // entries never move: the texture reads their pixels again

        void TextureFeed(EGxTexCommand command, uint32_t width, uint32_t, uint32_t, uint32_t, void* userArg, uint32_t& texelStrideInBytes, const void*& texels)
        {
            if (command == GxTex_Latch)
            {
                texelStrideInBytes = 4 * width;
                texels = userArg;     // the entry's pixels live as long as the texture does
            }
        }

        template <class T> T Clamp(T v, T lo, T hi) { return std::max(lo, std::min(hi, v)); }

        // ----------------------------------------------------------------------------------- the spell-go events
        constexpr uint32_t kEventRing = 64;
        WxlSpellAreaCastEvent g_events[kEventRing];
        uint64_t              g_eventCount = 0;   // events ever noted: the newest has this sequence

        // No cast time (no record, base and per-level 0) and not a channel (attributes ex 0x4 and 0x40).
        bool IsInstant(const SpellRec& spell)
        {
            if (spell.m_attributesEx & 0x44) return false;
            if (spell.m_castingTimeIndex == 0) return true;
            SpellCastTimesRec* castTime = g_spellCastTimesDB.GetRecord(spell.m_castingTimeIndex);
            return !castTime || (castTime->m_base <= 0 && castTime->m_perLevel <= 0);
        }
    }

    uint32_t DynamicObjects(WxlSpellAreaDynObject* out, uint32_t max)
    {
        if (!out || !max) return 0;
        Scan s;
        s.playerUnit = PlayerUnit(s.player);
        if (!s.playerUnit) return 0;
        s.dyn = out;
        s.max = max;
        ClntObjMgrEnumVisibleObjects(DynCallback, &s);
        return s.count;
    }

    uint32_t Casters(WxlSpellAreaCaster* out, uint32_t max)
    {
        if (!out || !max) return 0;
        Scan s;
        s.playerUnit = PlayerUnit(s.player);
        if (!s.playerUnit) return 0;
        s.casters = out;
        s.max = max;
        if (s.playerUnit->m_castingSpellId || s.playerUnit->m_channelSpellId)
            FillCaster(s.casters[s.count++], s.player, s.playerUnit, s);   // the player first
        ClntObjMgrEnumVisibleObjects(CasterCallback, &s);
        return s.count;
    }

    // What a spell's three effects say, with the radii worked out for this caster (level and, for the player, the
    // radius modifiers of talents and auras: the same ones the targeting circle uses).
    int Effects(uint32_t spellId, uint64_t casterGuid, WxlSpellAreaEffects* out)
    {
        if (!out) return 0;
        SpellRec spell;
        if (!spellId || !g_spellDB.GetRecord(static_cast<int32_t>(spellId), &spell)) return 0;

        CGUnit_C* caster = casterGuid ? Unit(WOWGUID{ casterGuid }) : nullptr;
        const int32_t level = caster && caster->m_unit ? static_cast<int32_t>(caster->m_unit->m_unitFieldLevel) : 0;
        const WOWGUID player = ClntObjMgrGetActivePlayer();
        const bool own = casterGuid && player && player.m_guid == casterGuid;
        int32_t flat = 0, pct = 100;
        const bool modified = own && Spell_C_GetSpellModifiers(&spell, 6, flat, pct);

        *out = {};
        for (int i = 0; i < 3; ++i)
        {
            out->effect[i]       = spell.m_effect[i];
            out->targetA[i]      = spell.m_implicitTargetA[i];
            out->targetB[i]      = spell.m_implicitTargetB[i];
            out->chainTargets[i] = spell.m_effectChainTargets[i];

            SpellRadiusRec* rec = g_spellRadiusDB.GetRecord(spell.m_effectRadiusIndex[i]);
            if (!rec) continue;
            float radius = rec->m_radius + static_cast<float>(level) * rec->m_radiusPerLevel;
            if (rec->m_radiusMax > 0.0f) radius = std::min(radius, rec->m_radiusMax);
            if (modified && radius > 0.0f) radius = (radius + static_cast<float>(flat)) * static_cast<float>(pct) * 0.01f;
            out->radius[i] = radius;
        }
        return 1;
    }

    int PendingCast(uint64_t casterGuid, uint32_t spellId, WxlSpellAreaPending* out)
    {
        if (!out) return 0;
        PendingSpellCastData data;
        if (!Spell_C_WxlFindPendingCast(WOWGUID{ casterGuid }, spellId, &data)) return 0;
        const C3Vector dest = data.GetDestLocation();
        out->targetFlags  = static_cast<uint32_t>(data.m_targetFlags);
        out->dest[0]      = dest.x;
        out->dest[1]      = dest.y;
        out->dest[2]      = dest.z;
        out->objectTarget = data.m_objectTarget.m_guid;
        return 1;
    }

    int ObjectPose(uint64_t guid, float* pos, float* facing)
    {
        if (!guid || !pos || !facing) return 0;
        CGObject_C* obj = Object(WOWGUID{ guid });
        if (!obj) return 0;
        const C3Vector p = obj->GetPosition();
        pos[0] = p.x; pos[1] = p.y; pos[2] = p.z;
        *facing = obj->GetFacing();
        return 1;
    }

    void* CreateTexture(const uint32_t* argbPixels, uint32_t size, const char* name)
    {
        if (!argbPixels || size < 2 || size > 2048) return nullptr;
        auto entry = std::make_unique<TexEntry>();
        entry->pixels.assign(argbPixels, argbPixels + static_cast<size_t>(size) * size);
        entry->tex = TextureCreate(GxTex_2d, size, size, 0, GxTex_Argb8888, GxTex_Argb8888, CGxTexFlags(GxTex_Linear),
                                   entry->pixels.data(), TextureFeed, name && *name ? name : "WxlSpellArea", 0);
        if (!entry->tex) return nullptr;
        g_textures.push_back(std::move(entry));
        return g_textures.back().get();
    }

    void Draw(const WxlSpellAreaDraw* d)
    {
        if (!d || d->structSize < sizeof(WxlSpellAreaDraw) || !d->texture || !(d->half > 0.0f)) return;
        auto* entry = static_cast<TexEntry*>(d->texture);
        CGxTex* texture = entry->tex ? TextureGetGxTex(entry->tex, 0, nullptr) : nullptr;
        if (!texture) return;

        CImVector color;
        color.b = d->b;
        color.g = d->g;
        color.r = d->r;
        color.a = d->a;

        // A square box around the shape's center (a cone's apex, a line's start); the texture is stretched over it
        // and the box's height is how far above and below the center the ground still takes the picture.
        const float half = d->half;
        const float hz = Clamp(half * 0.5f, 4.0f, 20.0f);
        CAaBox box(C3Vector(d->pos[0] - half, d->pos[1] - half, d->pos[2] - hz), C3Vector(d->pos[0] + half, d->pos[1] + half, d->pos[2] + hz));

        // The texture's front (row 0) points along the facing once rotated by -facing.
        C44Matrix rotation;
        const bool rotate = d->rotate != 0;
        if (rotate) rotation.RotateAroundZ(-d->facing);

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
        ProjectTex::Render2d(box, color, rotate ? &rotation : nullptr, 0.5f, d->triFlags, d->projectFlags, 0.4f);
        g_theGxDevicePtr->RsPop();
    }

    int Triggers(uint32_t spellId, WxlSpellAreaTriggers* out)
    {
        return out && ReadTriggers(spellId, out) ? 1 : 0;
    }

    uint32_t TriggerAuras(WxlSpellAreaAura* out, uint32_t max)
    {
        if (!out || !max) return 0;
        Scan s;
        s.playerUnit = PlayerUnit(s.player);
        if (!s.playerUnit) return 0;
        s.auras = out;
        s.max = max;
        ClntObjMgrEnumVisibleObjects(AuraCallback, &s);
        return s.count;
    }

    uint32_t CastEvents(uint64_t afterSequence, WxlSpellAreaCastEvent* out, uint32_t max)
    {
        if (!out || !max) return 0;
        const uint64_t oldest = g_eventCount > kEventRing ? g_eventCount - kEventRing + 1 : 1;
        uint64_t first = std::max(afterSequence + 1, oldest);
        uint32_t count = 0;
        for (uint64_t seq = first; seq <= g_eventCount && count < max; ++seq)
            out[count++] = g_events[(seq - 1) % kEventRing];
        return count;
    }

    void NoteSpellGo(uint64_t caster, uint32_t spellId, uint32_t castFlags, uint32_t targetFlags, const float* dest, uint64_t objectTarget, uint32_t delayMs)
    {
        SpellRec spell;
        if (!spellId || !g_spellDB.GetRecord(static_cast<int32_t>(spellId), &spell)) return;

        WxlSpellAreaCastEvent e = {};
        e.sequence     = g_eventCount + 1;
        e.casterGuid   = caster;
        e.objectTarget = objectTarget;
        e.spellId      = spellId;
        e.instant      = IsInstant(spell) ? 1u : 0u;
        e.castFlags    = castFlags;
        e.targetFlags  = targetFlags;
        if (dest) { e.dest[0] = dest[0]; e.dest[1] = dest[1]; e.dest[2] = dest[2]; }
        e.delayMs      = delayMs;
        e.atMs         = OsGetAsyncTimeMs();

        WOWGUID player;
        CGUnit_C* playerUnit = PlayerUnit(player);
        e.casterIsPlayer = caster && player.m_guid == caster ? 1u : 0u;
        e.casterHostile  = Hostile(caster ? Unit(WOWGUID{ caster }) : nullptr, playerUnit) ? 1u : 0u;

        g_events[g_eventCount % kEventRing] = e;
        ++g_eventCount;
    }

    void SetRenderer(WxlSpellAreaRenderFn fn, void* user)
    {
        g_renderer = fn;
        g_rendererUser = user;
    }

    void Render(uint32_t triFlags, uint32_t projectFlags)
    {
        if (g_renderer) g_renderer(g_rendererUser, triFlags, projectFlags, OsGetAsyncTimeMs());
    }
}

#endif
