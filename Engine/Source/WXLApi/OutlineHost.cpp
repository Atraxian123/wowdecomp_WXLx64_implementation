// The core's side of the outline module's engine access. See OutlineHost.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// The code here is what the unit outline did to the engine while it was compiled into the client (it was
// Engine/Source/WXLOutline/Outline.cpp): the mouseover / target pick, the quest scan, and the reads of the Gx
// device's state shadow. It moved here unchanged in behaviour; the drawing stayed in the module.

#include "Engine/Source/WXLApi/OutlineHost.h"

#ifndef WXL_ALLOW_32BIT_HEADER   // the module API is 64-bit only: a 32-bit client has only the declarations

#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Types.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/ShaderEffect/ShaderEffect.h"
#include "Engine/Source/WXLOutline/QuestMatch.h"
#include "WoW/Source/DB/DBClient/CreatureStats_C.h"
#include "WoW/Source/DB/DBClient/DBCacheInstances.h"
#include "WoW/Source/Object/GameObjectStats.h"
#include "WoW/Source/Object/ObjectClient/GameObject_C.h"
#include "WoW/Source/Object/ObjectClient/Object_C.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/Object/QuestCache.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Ui/GameUI.h"

#include <cstdint>

namespace wxl::api::outlinehost
{
    using wxl::outline::QuestNeeds;

    namespace
    {
        constexpr uint32_t kMaxQuestModels = 32;

        // One unit: the mouseover or the target. Units and players only (not objects, items, corpses), deduplicated by
        // model so the target and the mouseover on the same unit give one entry.
        void AddUnit(WxlOutlineUnit* out, uint32_t& count, uint32_t max, WOWGUID guid, CGUnit_C* player)
        {
            if (!guid || count >= max) return;

            CGObject_C* obj = ClntObjMgrObjectPtr(guid, static_cast<TypeMask>(TYPEMASK_OBJECT), __FILE__, __LINE__);
            if (!obj || !obj->m_obj) return;

            const uint32_t type = static_cast<uint32_t>(obj->m_obj->m_type);
            if (!(type & (TYPEMASK_UNIT | TYPEMASK_PLAYER))) return;

            const CM2Model* model = obj->GetObjectModel();
            if (!model) return;

            for (uint32_t i = 0; i < count; ++i)
                if (out[i].model == model) return;

            // The unit's own attitude toward the player, the same direction the client's highlight sound uses.
            const int reaction = player ? static_cast<int>(static_cast<CGUnit_C*>(obj)->UnitReaction(player)) : static_cast<int>(UNIT_REACTION_HONORED);
            out[count].model    = model;
            out[count].isPlayer = (type & TYPEMASK_PLAYER) != 0 ? 1u : 0u;
            // hated / hostile, then unfriendly / neutral, then friendly and up
            out[count].reaction = reaction < UNIT_REACTION_UNFRIENDLY ? 0u : reaction < UNIT_REACTION_FRIENDLY ? 1u : 2u;
            ++count;
        }

        // What the open quests still want, from the quest log (progress) and the quest cache (requirements).
        // A quest whose record is not cached yet is requested and skipped until it arrives.
        // Asking for a record with a callback is what makes the cache query the server for one it does not have; the
        // answer lands in the cache, and the next refresh finds it, so the callback has nothing left to do.
        void QuestRecordArrived(int32_t, const WOWGUID&, void*, bool) {}

        void CollectQuestNeeds(CGPlayer_C* player, QuestNeeds& needs)
        {
            const WOWGUID none = { 0 };
            for (uint32_t slot = 0; slot < 25; ++slot)
            {
                const PlayerQuestLogEntry& e = player->m_player->m_playerQuestLog[slot];
                if (!e.m_questId || (e.m_state & 0x3)) continue; // empty slot, complete (1) or failed (2)

                const QuestCache* quest = g_questDBCache.GetRecord(static_cast<int32_t>(e.m_questId), none, QuestRecordArrived, nullptr, false);
                if (!quest) continue;

                for (uint32_t i = 0; i < 4; ++i) // kill / interact objectives: creature entry, or game object entry with the sign bit set
                    if (quest->m_objectiveId[i] && e.m_counts[i] < quest->m_objectiveRequiredCount[i])
                        needs.AddObjective(quest->m_objectiveId[i]);

                for (uint32_t i = 0; i < 6; ++i) // items to collect
                {
                    const uint32_t item = quest->m_collectItemId[i];
                    if (item && player->m_bag.GetItemTypeCount(static_cast<int32_t>(item), 8) < static_cast<int32_t>(quest->m_collectItemCount[i]))
                        needs.AddItem(item);
                }
            }
        }

        struct QuestScan
        {
            const QuestNeeds* needs;
            const CM2Model*   models[kMaxQuestModels];
            uint32_t          limit;
            uint32_t          count;
        };

        // The server's quest giver status of an object (CGObject_C::m_questStatus, the "!" and "?" over its head).
        // The ones that mean "a quest of yours is complete, come and hand it in": 10 (the yellow "?"), 9 (the same
        // for a quest that gives no experience any more) and 6 (the blue "?" of a repeatable quest).
        bool IsTurnInStatus(uint32_t status)
        {
            return status == 6 || status == 9 || status == 10;
        }

        struct TurnInScan
        {
            const CM2Model* models[kMaxQuestModels];
            uint32_t        limit;
            uint32_t        count;
        };

        int32_t TurnInScanCallback(WOWGUID guid, void* param)
        {
            auto* scan = static_cast<TurnInScan*>(param);
            if (scan->count >= scan->limit) return 0;

            CGObject_C* obj = ClntObjMgrObjectPtr(guid, static_cast<TypeMask>(TYPEMASK_OBJECT), __FILE__, __LINE__);
            if (!obj || !obj->m_obj || !IsTurnInStatus(obj->m_questStatus)) return 1;

            const uint32_t type = static_cast<uint32_t>(obj->m_obj->m_type);
            if (type & TYPEMASK_PLAYER) return 1;
            if ((type & TYPEMASK_UNIT) && static_cast<CGUnit_C*>(obj)->IsDeadOrGhost()) return 1;
            if (!(type & (TYPEMASK_UNIT | TYPEMASK_GAMEOBJECT))) return 1;

            const CM2Model* model = obj->GetObjectModel();
            if (model) scan->models[scan->count++] = model;
            return 1;
        }

        // What the minimap tracks. The rules are the ones of the minimap's blip callback (UpdateObjectBlip in
        // MinimapFrame.cpp), so the outline marks what the player sees as a tracked blip:
        //   game object: IsTracked() (the tracking menu: mailboxes) and in reach, or CanTrackObject() (a tracking
        //                spell: the lock type of a node against the player's resource tracking mask);
        //   unit:        IsTracked() (the menu's NPC flag: vendors, trainers, ...) and in reach, or CanTrackUnit()
        //                (the creature types of the tracking spells, the server's tracking flag, hidden units).
        struct TrackedScan
        {
            CGPlayer_C*        player;
            C3Vector           position;
            float              range2;      // squared yards, 0 = no limit
            WxlOutlineTracked* out;
            uint32_t           limit;
            uint32_t           count;
        };

        int32_t TrackedScanCallback(WOWGUID guid, void* param)
        {
            auto* scan = static_cast<TrackedScan*>(param);
            if (scan->count >= scan->limit) return 0;
            if (guid == ClntObjMgrGetActivePlayer()) return 1;

            CGObject_C* obj = ClntObjMgrObjectPtr(guid, static_cast<TypeMask>(TYPEMASK_OBJECT), __FILE__, __LINE__);
            if (!obj || !obj->m_obj) return 1;

            const uint32_t type = static_cast<uint32_t>(obj->m_obj->m_type);
            uint32_t kind = 0;

            if (type & TYPEMASK_PLAYER)
            {
                return 1;   // tracked players are left to the reaction outline (and the scene hides them)
            }
            else if (type & TYPEMASK_GAMEOBJECT)
            {
                CGGameObject_C* go = static_cast<CGGameObject_C*>(obj);
                if ((go->IsTracked() && static_cast<CGUnit_C*>(scan->player)->CanInteract(go)) || scan->player->CanTrackObject(go))
                    kind = WXL_TRACKED_OBJECT;
            }
            else if (type & TYPEMASK_UNIT)
            {
                CGUnit_C* unit = static_cast<CGUnit_C*>(obj);
                const WOWGUID& controller = unit->m_unit->m_unitFieldCharmedby ? unit->m_unit->m_unitFieldCharmedby : unit->m_unit->m_unitFieldSummonedby;
                if (controller == ClntObjMgrGetActivePlayer()) return 1;                          // the player's own pet
                if (unit->m_unit->m_unitFieldHealth <= 0 && !unit->CanInteractWhileDead()) return 1;

                if (unit->IsTracked() && scan->player->CanInteract(unit)) kind = WXL_TRACKED_NPC;
                else if (scan->player->CanTrackUnit(unit))                kind = WXL_TRACKED_CREATURE;
            }
            if (!kind) return 1;

            if (scan->range2 > 0.0f)
            {
                const C3Vector p = obj->GetPosition();
                const float dx = p.x - scan->position.x, dy = p.y - scan->position.y, dz = p.z - scan->position.z;
                if (dx * dx + dy * dy + dz * dz > scan->range2) return 1;
            }

            const CM2Model* model = obj->GetObjectModel();
            if (model)
            {
                scan->out[scan->count].model = model;
                scan->out[scan->count].kind  = kind;
                ++scan->count;
            }
            return 1;
        }

        // Visible objects, one at a time. Returns non-zero to keep going.
        int32_t QuestScanCallback(WOWGUID guid, void* param)
        {
            auto* scan = static_cast<QuestScan*>(param);
            if (scan->count >= scan->limit) return 0;

            CGObject_C* obj = ClntObjMgrObjectPtr(guid, static_cast<TypeMask>(TYPEMASK_OBJECT), __FILE__, __LINE__);
            if (!obj || !obj->m_obj) return 1;

            const uint32_t type = static_cast<uint32_t>(obj->m_obj->m_type);
            const uint32_t entry = obj->m_obj->m_entryID;
            bool match = false;

            if (type & TYPEMASK_PLAYER)
            {
                return 1;
            }
            else if (type & TYPEMASK_UNIT)
            {
                CGUnit_C* unit = static_cast<CGUnit_C*>(obj);
                const CreatureStats_C* stats = unit->m_creatureCacheEntry;

                if (!unit->IsDeadOrGhost())
                {
                    match = scan->needs->NeedsCreature(entry, stats ? stats->m_killCredit : nullptr, 2)
                         || (stats && scan->needs->NeedsItemFrom(stats->m_questItem, 6));
                }
                else if ((unit->m_unit->m_unitDynamicFlags & 0x1) && stats) // a corpse still holding loot
                {
                    match = scan->needs->NeedsItemFrom(stats->m_questItem, 6);
                }
            }
            else if (type & TYPEMASK_GAMEOBJECT)
            {
                CGGameObject_C* go = static_cast<CGGameObject_C*>(obj);
                const GameObjectStats* stats = go->m_stats;
                const uint16_t dyn = go->m_gameObject ? go->m_gameObject->m_dynamicFlags : 0;

                // Besides the quest data, the server's own "sparkle" (0x8, unless the object cannot be used right now,
                // 0x4) is its statement that this object matters to this player's quests; the client draws the sparkle
                // from the same flag.
                match = scan->needs->NeedsGameObject(entry)
                     || (stats && scan->needs->NeedsItemFrom(stats->m_questItems, 6))
                     || ((dyn & 0x8) && !(dyn & 0x4));
            }

            if (match)
            {
                const CM2Model* model = obj->GetObjectModel();
                if (model) scan->models[scan->count++] = model;
            }
            return 1;
        }
    }

    uint32_t Units(WxlOutlineUnit* out, uint32_t max)
    {
        if (!out || max == 0) return 0;
        uint32_t count = 0;
        CGUnit_C* player = static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(ClntObjMgrGetActivePlayer(), static_cast<TypeMask>(TYPEMASK_PLAYER), __FILE__, __LINE__));
        AddUnit(out, count, max, CGGameUI::m_currentObjectTrack, player); // mouseover
        AddUnit(out, count, max, CGGameUI::m_lockedTarget,       player);
        return count;
    }

    // Rebuilds the quest target list from the quest log and the visible objects. The caller runs it on a timer, not
    // per frame: the log changes on kills and loot, and nothing here has to be frame-exact.
    uint32_t QuestTargets(const void** models, uint32_t max)
    {
        if (!models || max == 0) return 0;

        CGPlayer_C* player = static_cast<CGPlayer_C*>(ClntObjMgrObjectPtr(ClntObjMgrGetActivePlayer(), static_cast<TypeMask>(TYPEMASK_PLAYER), __FILE__, __LINE__));
        if (!player || !player->m_player) return 0;

        QuestNeeds needs;
        CollectQuestNeeds(player, needs);
        if (needs.Empty()) return 0;

        QuestScan scan{ &needs, {}, max < kMaxQuestModels ? max : kMaxQuestModels, 0 };
        ClntObjMgrEnumVisibleObjects(QuestScanCallback, &scan);

        // Pointers only: they are compared against the model being drawn, never dereferenced, so one that outlives
        // its object by a refresh interval is harmless.
        for (uint32_t i = 0; i < scan.count; ++i) models[i] = scan.models[i];
        return scan.count;
    }

    // The creatures and objects that wait for a completed quest to be handed in: the visible ones whose quest giver
    // status (sent by the server for this player) is a "?". The quest log is not needed for this, so it also covers
    // quests whose turn-in is somebody else than their giver.
    uint32_t QuestTurnIns(const void** models, uint32_t max)
    {
        if (!models || max == 0) return 0;

        // Like QuestTargets: with no active player there is nothing to scan, and the object manager may not even
        // exist yet (the login and character screens), so the walk must not start.
        CGPlayer_C* player = static_cast<CGPlayer_C*>(ClntObjMgrObjectPtr(ClntObjMgrGetActivePlayer(), static_cast<TypeMask>(TYPEMASK_PLAYER), __FILE__, __LINE__));
        if (!player || !player->m_player) return 0;

        TurnInScan scan{ {}, max < kMaxQuestModels ? max : kMaxQuestModels, 0 };
        ClntObjMgrEnumVisibleObjects(TurnInScanCallback, &scan);
        for (uint32_t i = 0; i < scan.count; ++i) models[i] = scan.models[i];
        return scan.count;
    }

    // The visible creatures and objects the minimap is tracking, with the reason (see WxlOutlineTracked).
    uint32_t TrackedTargets(WxlOutlineTracked* out, uint32_t max, float maxYards)
    {
        if (!out || max == 0) return 0;

        CGPlayer_C* player = static_cast<CGPlayer_C*>(ClntObjMgrObjectPtr(ClntObjMgrGetActivePlayer(), static_cast<TypeMask>(TYPEMASK_PLAYER), __FILE__, __LINE__));
        if (!player || !player->m_player || player->IsDeadOrGhost()) return 0;

        TrackedScan scan{ player, player->GetPosition(), maxYards > 0.0f ? maxYards * maxYards : 0.0f, out, max, 0 };
        ClntObjMgrEnumVisibleObjects(TrackedScanCallback, &scan);
        return scan.count;
    }

    // What the engine is doing for the batch it just drew: the blend mode and the depth write say whether it is an
    // opaque (or alpha-tested) batch, the alpha reference is the cutout the batch used.
    // Returns 1 for an opaque or alpha-tested batch that writes depth, 2 for any other batch (alpha-blended, additive
    // or drawn without a depth write: ghosts, spirits and other see-through models), 0 when there is no device. A
    // caller that only wants solid batches treats 2 as "skip".
    int BatchState(float* alphaRef)
    {
        if (alphaRef)
        {
            if (CShaderEffect::s_useAlphaRef && g_theGxDevicePtr)
                *alphaRef = static_cast<float>(g_theGxDevicePtr->m_hwRenderStates[GxRs_AlphaRef].m_data.i[0]) / 255.0f;
            else
                *alphaRef = CShaderEffect::s_fogColorAlphaRef.w;
        }
        if (!g_theGxDevicePtr) return 0;
        const int32_t blend  = g_theGxDevicePtr->m_hwRenderStates[GxRs_BlendingMode].m_data.i[0];
        const int32_t zwrite = g_theGxDevicePtr->m_hwRenderStates[GxRs_DepthWrite].m_data.i[0];
        return blend <= GxBlend_AlphaKey && zwrite != 0 ? 1 : 2;
    }
}

#endif // !WXL_ALLOW_32BIT_HEADER
