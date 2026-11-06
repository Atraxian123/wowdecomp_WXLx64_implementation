// Runtime physics for model instances. See PhysicsLive.hpp.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// What it does, per animated instance, each time the engine has built its bone palette:
//   * bone world = palette bone x the scene matrix (a MIRROR: never decomposed); the placement = the
//     instance's own matrix x the scene matrix;
//   * each dynamic body's model-space bone goes back as bone x own matrix, and every bone under a moved
//     one is carried by its parent's delta;
//   * time comes from the scene's clock (ms); a second build in one frame takes 0 s;
//   * state per instance, forgotten when unseen for 2 s;
//   * underwater: a unit that swims (WxlApi::instanceSwimming, which also answers for an item in its hand) gets
//     the [Physics.Underwater] multipliers on top of the [Physics] ones, blended in over BlendSeconds. The
//     blend is quantised into kUwLevels steps; each step is a tuned copy of the model (built once, cached) and
//     a running instance is switched to it with Instance::Retune, which keeps its pose and velocities;
//   * stuck items: see PhysicsGuard.hpp. The model is prepared once (extra anchors carry their followers),
//     and each frame the simulated bones are compared with the animated ones; an instance that stays far
//     off, or whose placement teleports, is dropped (it is re-seeded from the animated pose next frame)
//     and the palette is left as the engine built it for that frame. Re-seeding too often switches the
//     instance's physics off for 10 s. There is no sleeping to wake: the engine never sleeps an item world.
//
// Every read and write of client memory happens inside the core (WxlApi::readInstance / readBones /
// writePalette, guarded there): nothing in this file can fault on a stale instance.

#include "PhysicsLive.hpp"

#include "ModuleHost.hpp"
#include "Phy2Library.hpp"
#include "PhysicsGuard.hpp"
#include "PhysicsOverrides.hpp"
#include "PhysicsTuning.hpp"
#include "physics_domino.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl::equip::physics
{
    // A member the core's table has AND filled in: WXL_API_HAS only says the table is long enough.
    #define PHYS_HAS(api, member) (WXL_API_HAS(api, member) && (api)->member)

    namespace phys = wow::ec::physics;
    namespace host = wxl::modules::host;

    static_assert(sizeof(phys::V3) == 12, "pivots are handed to the API as 3 floats per bone");

    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr uint32_t kForgetMs = 2000;
        constexpr int kUwLevels = 8;   // steps of the underwater blend: level 0 = dry, kUwLevels = fully under

        uint32_t NowMs()
        {
            return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
        }

        // What an instance shows, for PhysicsOverride.csv: a kind (the id spaces of ItemDisplayInfo and
        // CreatureDisplayInfo overlap) and an id. id 0 = nothing known, the global multipliers apply.
        struct Display
        {
            overrides::Kind kind = overrides::Kind::Item;
            uint32_t        id = 0;
            bool operator==(const Display& o) const { return kind == o.kind && id == o.id; }
        };

        struct InstState
        {
            const void*                      shared = nullptr;
            std::unique_ptr<phys::Instance>  sim;
            uint32_t lastClock = 0;       // scene clock at the last step
            uint32_t lastSeen  = 0;       // NowMs at the last build
            Clock::time_point stillSince{};   // when the scene clock stopped moving
            Clock::time_point lastTime{};
            bool     haveClock = false;
            std::shared_ptr<phys::Model> model;   // keeps the model alive while the instance runs
            guard::Limits limits;                 // how far each dynamic bone may stray
            guard::State  judge;
            Display  display;                     // the item or creature display this instance shows (id 0: none known)
            uint32_t bindGen = 0;                 // g_bindGen when `display` was worked out
            std::shared_ptr<phys::Model> dry;     // the dry variant `model` was made from (the instance is rebuilt when it changes)
            float uwBlend = 0;                    // 0 = dry .. 1 = fully underwater, eased towards the target
            int   uwLevel = 0;                    // the blend step `model` is at (0 = dry, kUwLevels = fully under)
            bool  uwRetuneFailed = false;         // Retune refused this model once: stop trying
        };

        // Instances that had to be re-seeded: too many times in a short while switches them off for a bit.
        struct Penalty
        {
            uint32_t windowStart = 0;
            uint32_t count = 0;
            uint32_t disabledUntil = 0;
            uint32_t lastTouched = 0;
        };

        struct ModelEntry
        {
            std::string                  stem;    // the path it was looked up under (a slot can be reused)
            std::shared_ptr<phys::Model> base;    // null: no physics for this model. Carry groups added, no multipliers yet
            // base with the multipliers laid on: key 0 = the global ones, (1 << 32 | id) = the global ones with that
            // item display's PhysicsOverride.csv row on top, (2 << 32 | id) the same for a creature display.
            // Bits 48 and up hold the underwater blend step (0 = dry, see kUwLevels): the same multipliers with
            // [Physics.Underwater] blended in that far.
            std::unordered_map<uint64_t, std::shared_ptr<phys::Model>> variants;
        };

        // A held item's instance, as the OnHandItemAttached event told us (see OnHandItemAttached below)
        struct Binding
        {
            void*    wearer = nullptr;   // item: the character's model instance the weapon is attached to
            uint32_t slot = 0;           // item: inventory slot 15 main hand, 16 off hand, 17 ranged; creature: 1 = a mount
            uint32_t displayId = 0;      // ItemDisplayInfo id (item) or CreatureDisplayInfo id (creature)
            overrides::Kind kind = overrides::Kind::Item;
            std::string path;            // creature: the normalized model file the instance was built from
        };

        std::mutex g_lock;                                                // guards the three below and g_stats
        std::unordered_map<const void*, ModelEntry> g_models;             // loaded model -> its physics
        std::unordered_map<void*, InstState> g_inst;                      // instance -> simulation
        std::unordered_map<void*, Penalty> g_penalty;                     // instance -> re-seed history
        std::unordered_map<void*, Binding> g_bind;                        // held-item / creature instance -> its display
        size_t   g_pruneAt = 256;                                         // g_bind size at which dead entries are looked for
        uint32_t g_bindGen = 1;                                           // bumped whenever g_bind changes
        guard::Config g_guardCfg;
        bool g_carryAnchors = true;
        constexpr uint32_t kPenaltyWindowMs = 5000, kPenaltyMax = 4, kPenaltyOffMs = 10000;
        Stats g_stats;
        uint32_t g_lastSweep = 0;
        std::atomic<bool> g_enabled{false};
        bool g_inited = false;

        // ---- where the .phy2 files are read from ----
        // The core's reader: the client's own archives, then loose files under Data\. A model's physics is a
        // few KB; a miss is the normal answer for almost every model and costs no log line.
        bool ReadArchiveFile(const char* name, std::vector<uint8_t>& out)
        {
            if (!host::ReadFile(name, out)) return false;
            if (out.empty() || out.size() > (4u << 20)) { out.clear(); return false; }
            return true;
        }

        phy2::Library g_lib(&ReadArchiveFile, &host::IsVirtualKey);

        void SweepLocked(uint32_t now)
        {
            if (now - g_lastSweep < 500) return;
            g_lastSweep = now;
            for (auto it = g_inst.begin(); it != g_inst.end();)
            {
                if (now - it->second.lastSeen > kForgetMs) { it = g_inst.erase(it); ++g_stats.dropped; }
                else ++it;
            }
            for (auto it = g_penalty.begin(); it != g_penalty.end();)
            {
                if (now - it->second.lastTouched > 30000) it = g_penalty.erase(it);
                else ++it;
            }
            g_stats.live = uint32_t(g_inst.size());
        }

        void OnBuildBonePalette(void* /*user*/, const void* args)
        {
            if (args) AfterBonePalette(static_cast<const WxlBuildBonePaletteArgs*>(args)->instance);
        }
    }

    namespace
    {
        // [Physics] ...Scale multipliers (PhysicsTuning.hpp). Written by Init (before any hook can run), by
        // ReloadFiles and by the WatchIni poll, the last two under g_lock; read under g_lock.
        tuning::Tuning g_tuning;
        // [Physics.Underwater]: the same multipliers, stacked on the effective ones while a unit swims. Same locking.
        tuning::Tuning g_underwater;
        bool  g_uwOn = true;             // [Physics.Underwater] Enabled
        bool  g_uwActive = false;        // Enabled=1 and the water changes at least one multiplier (see UpdateUnderwaterActive)
        float g_uwBlendSec = 0.4f;       // BlendSeconds
        bool  g_hasSwimApi = false;      // the core offers WxlApi::instanceSwimming
        bool g_watchIni = false;       // [Physics] WatchIni: re-read the multipliers when the ini is saved
        uint64_t g_iniStamp = 0;       // the ini's last-write time when the multipliers were last read
        uint32_t g_lastIniPoll = 0;    // NowMs of the last look at it

        // ---- PhysicsOverride.csv (PhysicsOverrides.hpp) ----
        // Written by Init, ReloadFiles and the WatchIni poll (the last two under g_lock); read under g_lock.
        overrides::Table g_ovr;
        std::string g_ovrWhere;        // where it was read from ("" = there is no file)
        uint64_t g_ovrStamp = 0;       // its last-write time when it was read (0 = not on disk)

        constexpr const char* kOverrideName = "PhysicsOverride.csv";

        // The places it is looked for, in order: beside the client, then DBFilesClient\ beside the client; the
        // same two names are then tried through the client's reader (loose files under Data\ and the archives)
        // so a patch archive can ship one.
        std::vector<std::string> OverrideDiskCandidates()
        {
            std::vector<std::string> v;
            const WxlApi* api = host::Api();
            std::string base;
            if (api && PHYS_HAS(api, workingDirectory) && api->workingDirectory())
            {
                base = api->workingDirectory();
                if (!base.empty() && base.back() != '\\' && base.back() != '/') base += '/';
            }
            v.push_back(base + kOverrideName);
            v.push_back(base + "DBFilesClient/" + kOverrideName);
            return v;
        }

        // last-write time of the file on disk (0 when there is none); it changes when the file is saved
        uint64_t OverrideStamp()
        {
            for (const std::string& path : OverrideDiskCandidates())
            {
                std::error_code ec;
                const auto t = std::filesystem::last_write_time(path, ec);
                if (ec) continue;
                const uintmax_t size = std::filesystem::file_size(path, ec);
                return (uint64_t(t.time_since_epoch().count()) ^ (uint64_t(ec ? 0 : size) << 1)) | 1u;   // never 0 for a file
            }
            return 0;
        }

        bool ReadOverrideText(std::string& text, std::string& where)
        {
            for (const std::string& path : OverrideDiskCandidates())
            {
                if (FILE* f = std::fopen(path.c_str(), "rb"))
                {
                    char buf[4096];
                    size_t n;
                    text.clear();
                    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                    {
                        text.append(buf, n);
                        if (text.size() > (4u << 20)) break;   // not a plausible size: stop reading
                    }
                    std::fclose(f);
                    where = path;
                    return true;
                }
            }
            const char* names[] = { "DBFilesClient\\PhysicsOverride.csv", "PhysicsOverride.csv" };
            for (const char* name : names)
            {
                std::vector<uint8_t> raw;
                if (host::ReadFile(name, raw) && !raw.empty() && raw.size() <= (4u << 20))
                {
                    text.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
                    where = std::string(name) + " (client data)";
                    return true;
                }
            }
            return false;
        }

        // Does the water change anything? Yes when [Physics.Underwater] sets a multiplier other than gravity and
        // Buoyancy (those two are not used there: underwater, the gravity multiplier IS the Buoyancy), or when
        // the Buoyancy of the global values or of any PhysicsOverride.csv row differs from the gravity multiplier
        // it replaces. Written after LoadTuning and LoadOverrides, under the same locking.
        void UpdateUnderwaterActive()
        {
            bool matters = false;
            tuning::Tuning others = g_underwater;
            others.gravity = 1.0f;
            others.buoyancy = 1.0f;
            if (!others.Identity()) matters = true;
            if (g_tuning.buoyancy != g_tuning.gravity) matters = true;
            auto rowMatters = [&](const tuning::Overrides& row) {
                const tuning::Tuning m = tuning::Merge(g_tuning, row);
                return m.buoyancy != m.gravity;
            };
            for (const auto& kv : g_ovr.items)     if (!matters && rowMatters(kv.second)) matters = true;
            for (const auto& kv : g_ovr.creatures) if (!matters && rowMatters(kv.second)) matters = true;

            const bool was = g_uwActive;
            g_uwActive = g_uwOn && matters;
            if (g_uwActive != was || g_uwActive)
                wxl::LogCat("PHYS", "physics: underwater: %s; gravity underwater = Buoyancy (global %.3g)%s",
                            g_uwActive ? "on" : "off (nothing differs in the water)", g_tuning.buoyancy,
                            g_uwActive && !g_hasSwimApi ? " - but this client cannot tell who swims, so it is NOT applied" : "");
        }

        void LoadOverrides()
        {
            g_ovr = overrides::Table{};
            g_ovrWhere.clear();
            g_ovrStamp = OverrideStamp();
            std::string text;
            if (!ReadOverrideText(text, g_ovrWhere))
            {
                wxl::LogCat("PHYS", "physics: no %s (optional): no per-display multipliers", kOverrideName);
                UpdateUnderwaterActive();
                return;
            }
            std::string problems;
            g_ovr = overrides::Parse(text, &problems);
            for (size_t b = 0; b < problems.size();)
            {
                const size_t e = problems.find('\n', b);
                wxl::LogCat("PHYS", "physics: %s", problems.substr(b, e == std::string::npos ? e : e - b).c_str());
                if (e == std::string::npos) break;
                b = e + 1;
            }
            wxl::LogCat("PHYS", "physics: %s: %zu item row(s), %zu creature row(s)%s", g_ovrWhere.c_str(), g_ovr.items.size(),
                        g_ovr.creatures.size(), "");
            g_stats.overrideRows = uint32_t(g_ovr.Rows());
            UpdateUnderwaterActive();
        }

        // The multipliers a model gets for one display (item or creature): the global ones, with that display's row of
        // PhysicsOverride.csv on top. Built once per (model, display) and kept. Caller holds g_lock.
        // `level` (0 .. kUwLevels) is how far [Physics.Underwater] is blended in on top of those effective
        // values: 0 is the dry model, kUwLevels the fully underwater one.
        std::shared_ptr<phys::Model> VariantLocked(ModelEntry& e, const Display& display, const char* stem, int level = 0)
        {
            level = g_uwActive ? std::max(0, std::min(level, kUwLevels)) : 0;
            const tuning::Overrides* row = display.id ? g_ovr.Find(display.kind, display.id) : nullptr;
            const uint64_t key = (row ? ((uint64_t(display.kind == overrides::Kind::Creature ? 2 : 1) << 32) | display.id) : 0) |
                                 (uint64_t(level) << 48);
            const auto it = e.variants.find(key);
            if (it != e.variants.end()) return it->second;

            tuning::Tuning effective = row ? tuning::Merge(g_tuning, *row) : g_tuning;
            if (level > 0) effective = tuning::Blend(effective, g_underwater, float(level) / float(kUwLevels));
            int tuned = 0;
            std::shared_ptr<phys::Model> model = tuning::Apply(e.base, effective, &tuned);   // a copy only when something changes
            if (tuned && level == 0) ++g_stats.modelsTuned;
            if (row && level == 0)
            {
                ++g_stats.modelsOverridden;
                wxl::LogCat("PHYS", "physics: %s display %u on '%s' uses its PhysicsOverride.csv row: %s",
                            display.kind == overrides::Kind::Creature ? "creature" : "item", display.id, stem,
                            tuning::Describe(effective).c_str());
            }
            e.variants[key] = model;
            return model;
        }

        // Which display a model instance shows; id 0 when nobody said. No lock held.
        //  1. A binding an event made (g_bind): a held weapon (OnHandItemAttached) or a creature's or a mount's
        //     model (OnCreatureModelBuilt). Works for two displays that share one model file and carry no
        //     texture or geoset change. A binding is trusted only while it still describes the instance: a
        //     weapon must still be its wearer's child, a creature must still show the file it was built from.
        //  2. A baked variant: its virtual path is "<original stem>_<displayId>.m2" and the core can decode it.
        //     The id spaces of items and creatures overlap and the shape is the same, so the folder decides:
        //     item\ is an item display, creature\ a creature display, anything else is nobody's.
        Display ResolveDisplay(const WxlApi* api, void* instance, const char* stem)
        {
            Binding b;
            bool have = false;
            {
                std::lock_guard<std::mutex> g(g_lock);
                const auto it = g_bind.find(instance);
                if (it != g_bind.end()) { b = it->second; have = true; }
            }
            const std::string norm = phy2::NormalizeModelPath(stem);
            if (have)
            {
                bool valid;
                if (b.kind == overrides::Kind::Item)
                {
                    // a recycled instance address must not inherit an old binding: the weapon is still the wearer's child
                    WxlModelInfo mi = {};
                    mi.structSize = sizeof(mi);
                    valid = PHYS_HAS(api, modelInfo) && api->modelInfo(instance, &mi) == WXL_READ_OK && mi.parent == b.wearer;
                }
                else
                    valid = norm == b.path;   // the address now belongs to another model: its file is another one
                if (valid) return Display{ b.kind, b.displayId };
                std::lock_guard<std::mutex> g(g_lock);
                const auto it = g_bind.find(instance);
                if (it != g_bind.end() && it->second.kind == b.kind && it->second.displayId == b.displayId)
                {
                    g_bind.erase(it);
                    ++g_bindGen;
                }
            }
            if (PHYS_HAS(api, vpathDecodeGlobalVirtualKey) && host::IsVirtualKey(stem))
            {
                const bool item = norm.compare(0, 5, "item\\") == 0;
                const bool creature = norm.compare(0, 9, "creature\\") == 0;
                if (item || creature)
                {
                    char original[WXL_PATH_MAX];
                    uint32_t id = 0;
                    if (api->vpathDecodeGlobalVirtualKey(stem, original, sizeof(original), &id) && id)
                        return Display{ item ? overrides::Kind::Item : overrides::Kind::Creature, id };
                }
            }
            return Display{};
        }

        // Drops the bindings whose model is gone (the wearer or the creature was freed without a "removed" event).
        // Looks again only when the map has doubled since the last time, so a zone full of creatures costs
        // nothing per spawn.
        void PruneBindings(const WxlApi* api)
        {
            struct Entry { void* instance; Binding b; };
            std::vector<Entry> all;
            {
                std::lock_guard<std::mutex> g(g_lock);
                for (const auto& kv : g_bind)
                {
                    Binding copy = kv.second;
                    copy.path.clear();   // not needed here
                    all.push_back({ kv.first, std::move(copy) });
                }
            }
            std::vector<void*> dead;
            for (const Entry& e : all)
            {
                WxlModelInfo mi = {};
                mi.structSize = sizeof(mi);
                const bool ok = PHYS_HAS(api, modelInfo) && api->modelInfo(e.instance, &mi) == WXL_READ_OK;
                const bool alive = ok && (e.b.kind == overrides::Kind::Item ? mi.parent == e.b.wearer : mi.shared != nullptr);
                if (!alive) dead.push_back(e.instance);
            }
            std::lock_guard<std::mutex> g(g_lock);
            for (void* d : dead) g_bind.erase(d);
            if (!dead.empty()) ++g_bindGen;
            g_pruneAt = std::max<size_t>(256, g_bind.size() * 2);
        }

        // OnHandItemAttached (fired by CCharacterComponent::AddHandItem after it attached the weapon, and by
        // RemoveHandItem with removed=1): `model` is the WEARER, so the weapon's own instance is found as its
        // child at the attach link whose file stem is the display's model. A baked variant has another stem and
        // is not found here; ResolveDisplay decodes its path instead.
        void OnHandItemAttached(void* /*user*/, const void* args)
        {
            if (!args) return;
            const WxlHandItemAttachedArgs a = *static_cast<const WxlHandItemAttachedArgs*>(args);
            const WxlApi* api = host::Api();
            if (!api || !a.model) return;

            if (a.removed)
            {
                std::lock_guard<std::mutex> g(g_lock);
                for (auto it = g_bind.begin(); it != g_bind.end();)
                    if (it->second.wearer == a.model && it->second.slot == a.invSlot) it = g_bind.erase(it);
                    else ++it;
                ++g_bindGen;
                return;
            }
            if (!a.displayId || a.attachLink < 0) return;
            if (!PHYS_HAS(api, findAttachedChild) || !PHYS_HAS(api, itemDisplayInfo)) return;

            WxlItemDisplayInfo info = {};
            info.structSize = sizeof(info);
            if (!api->itemDisplayInfo(a.displayId, &info) || !info.modelName[0][0]) return;
            void* child = api->findAttachedChild(a.model, static_cast<uint32_t>(a.attachLink), info.modelName[0]);

            bool prune;
            {
                std::lock_guard<std::mutex> g(g_lock);
                for (auto it = g_bind.begin(); it != g_bind.end();)   // the slot's earlier weapon
                    if (it->second.wearer == a.model && it->second.slot == a.invSlot) it = g_bind.erase(it);
                    else ++it;
                if (child)
                {
                    Binding b;
                    b.wearer = a.model;
                    b.slot = a.invSlot;
                    b.displayId = a.displayId;
                    g_bind[child] = std::move(b);
                }
                ++g_bindGen;
                prune = g_bind.size() > g_pruneAt;
            }
            if (prune) PruneBindings(api);
        }

        // OnCreatureModelBuilt (fired by CGUnit_C::UpdateDisplayInfo for a unit's model and by CreateUnitMount for
        // its mount's): `model` is the new instance itself, so the binding is direct. A new instance cannot have a
        // PhysicsLive entry yet, so unlike the held-item case the generation is bumped only when this address
        // already has one (an old model's address reused), not for every spawn in a busy zone.
        void OnCreatureModelBuilt(void* /*user*/, const void* args)
        {
            if (!args) return;
            const WxlCreatureModelBuiltArgs a = *static_cast<const WxlCreatureModelBuiltArgs*>(args);
            const WxlApi* api = host::Api();
            if (!api || !a.model || !a.displayId || !a.fileName || !a.fileName[0]) return;

            Binding b;
            b.slot = a.mount ? 1u : 0u;
            b.displayId = a.displayId;
            b.kind = overrides::Kind::Creature;
            b.path = phy2::NormalizeModelPath(a.fileName);

            bool prune;
            {
                std::lock_guard<std::mutex> g(g_lock);
                g_bind[a.model] = std::move(b);
                if (g_inst.count(a.model)) ++g_bindGen;
                prune = g_bind.size() > g_pruneAt;
            }
            if (prune) PruneBindings(api);
        }


        bool ReadPhysicsKey(const char* key, char* out, size_t cap)
        {
            return host::IniGetString("Physics", key, out, cap);
        }

        bool ReadUnderwaterKey(const char* key, char* out, size_t cap)
        {
            return host::IniGetString("Physics.Underwater", key, out, cap);
        }

        // one log line per problem: a value that is not a number, or outside its range
        void LogProblems(const char* section, const std::string& problems)
        {
            for (size_t b = 0; b < problems.size();)
            {
                const size_t e = problems.find('\n', b);
                wxl::LogCat("PHYS", "physics: ini [%s]: %s", section, problems.substr(b, e == std::string::npos ? e : e - b).c_str());
                if (e == std::string::npos) break;
                b = e + 1;
            }
        }

        void LoadTuning()
        {
            std::string problems;
            g_tuning = tuning::FromIni(&ReadPhysicsKey, &problems);
            g_iniStamp = host::IniStamp();
            LogProblems("Physics", problems);
            wxl::LogCat("PHYS", "physics: multipliers: %s", tuning::Describe(g_tuning).c_str());

            problems.clear();
            g_underwater = tuning::FromIni(&ReadUnderwaterKey, &problems);
            LogProblems("Physics.Underwater", problems);
            g_uwBlendSec = 0.4f;
            char text[64] = {};
            if (ReadUnderwaterKey("BlendSeconds", text, sizeof(text)) && text[0])
            {
                float v = 0;
                if (tuning::ParseMultiplier(text, &v)) g_uwBlendSec = std::max(0.0f, std::min(v, 10.0f));
                else wxl::LogCat("PHYS", "physics: ini [Physics.Underwater]: BlendSeconds='%s' is not a number; ignored, stays %.2f", text, g_uwBlendSec);
            }
            g_uwOn = host::IniGetBool("Physics.Underwater", "Enabled", true);
            if (g_underwater.gravity != 1.0f || g_underwater.buoyancy != 1.0f)
                wxl::LogCat("PHYS", "physics: ini [Physics.Underwater]: GravityScale and Buoyancy are not used there; underwater the "
                                    "gravity multiplier is the Buoyancy of [Physics] (or of the PhysicsOverride.csv row)");
            wxl::LogCat("PHYS", "physics: underwater multipliers (on top of the above): %s, blend %.2f s, Buoyancy %.3g%s",
                        tuning::Describe(g_underwater).c_str(), g_uwBlendSec, g_tuning.buoyancy, g_uwOn ? "" : " (Enabled=0)");
            // g_uwActive is settled by UpdateUnderwaterActive once PhysicsOverride.csv has been read too
            g_uwActive = false;
        }
    }

    bool Init(const WxlApi* api)
    {
        if (g_inited) return true;
        if (!api || api->abiVersion != WXL_ABI_VERSION || !WXL_API_HAS(api, writePalette)) return false;
        host::SetApi(api);
        g_inited = true;

        g_enabled = host::IniGetBool("Physics", "Enabled", false);
        wxl::LogCat("PHYS", "physics: %s (WXLExtendedEquipment.ini [Physics] Enabled); per-model .phy2 beside each model",
                    g_enabled ? "on" : "off");
        g_carryAnchors = host::IniGetBool("Physics", "CarryAnchors", true);
        g_guardCfg.enabled = host::IniGetBool("Physics", "Unstick", true);
        g_guardCfg.reach = float(std::max(host::IniGetInt("Physics", "UnstickReach", 300), 100)) / 100.0f;
        g_guardCfg.teleportYards = float(std::max(host::IniGetInt("Physics", "TeleportYards", 6), 1));
        wxl::LogCat("PHYS", "physics: CarryAnchors=%d Unstick=%d UnstickReach=%.0f%% TeleportYards=%.0f",
                    g_carryAnchors ? 1 : 0, g_guardCfg.enabled ? 1 : 0, g_guardCfg.reach * 100.0f, g_guardCfg.teleportYards);
        g_watchIni = host::IniGetBool("Physics", "WatchIni", false);
        g_hasSwimApi = WXL_API_HAS(api, instanceSwimming) && api->instanceSwimming != nullptr;
        LoadTuning();
        LoadOverrides();
        if (g_watchIni) wxl::LogCat("PHYS", "physics: WatchIni=1, the multipliers and %s are re-read whenever they are saved", kOverrideName);

        // Which display a held weapon shows: the equip extension says so when it attaches one (see the handler).
        if (api->subscribe(WXL_EVENT_HAND_ITEM_ATTACHED, &OnHandItemAttached, nullptr) != 0)
            wxl::LogCat("PHYS", "physics: cannot follow held items; PhysicsOverride.csv item rows then only reach baked variants");
        // ...and which display a creature's (or a mount's) model was built for.
        if (api->subscribe(WXL_EVENT_CREATURE_MODEL_BUILT, &OnCreatureModelBuilt, nullptr) != 0)
            wxl::LogCat("PHYS", "physics: cannot follow creature models; PhysicsOverride.csv creature rows then only reach baked variants");

        // After the built-in extensions' handlers (subscribers run in subscription order; modules load last).
        return api->subscribe(WXL_EVENT_BUILD_BONE_PALETTE, &OnBuildBonePalette, nullptr) == 0;
    }

    bool Enabled() { return g_enabled; }

    void SetEnabled(bool on)
    {
        std::lock_guard<std::mutex> g(g_lock);
        g_enabled = on;
        g_inst.clear();   // parts go back to their pose next frame
        g_stats.live = 0;
    }

    void ReloadFiles()
    {
        g_lib.Clear();
        std::lock_guard<std::mutex> g(g_lock);
        g_models.clear();
        g_inst.clear();
        g_penalty.clear();
        g_stats = Stats{};
        LoadTuning();
        LoadOverrides();
        wxl::LogCat("PHYS", "physics: file cache cleared, .phy2 files, the multipliers and %s will be read again", kOverrideName);
    }

    Stats GetStats()
    {
        std::lock_guard<std::mutex> g(g_lock);
        Stats s = g_stats;
        s.modelsSeen = uint32_t(g_lib.Cached());
        s.modelsWithPhysics = uint32_t(g_lib.Found());
        return s;
    }

    void AfterBonePalette(void* instance)
    {
        if (!g_enabled || !instance) return;
        const WxlApi* api = host::Api();
        if (!api) return;

        const Clock::time_point t0 = Clock::now();

        static std::atomic<int> calls{0};
        if (calls.fetch_add(1) == 0) wxl::LogCat("PHYS", "physics: first bone build seen (hook is live)");

        WxlInstanceView s = {};
        s.structSize = sizeof(s);
        const int stage = api->readInstance(instance, &s);
        if (stage)
        {
            // Stages 6 (one bone), 12 and 13 (not loaded / animated yet) are the common, harmless cases.
            if (stage != WXL_READ_BONE_COUNT && stage != WXL_READ_NOT_PARSED && stage != WXL_READ_NOT_ANIMATED &&
                s.model && wxl::LogThrottle("phys.skip", 40, 500))
                wxl::LogCat("PHYS", "physics: skip '%s' at read stage %d", s.path[0] ? s.path : "(no path)", stage);
            return;
        }
        const char* stem = s.path;
        const uint32_t boneCount = s.boneCount;

        std::unique_lock<std::mutex> lk(g_lock);
        const uint32_t now = NowMs();
        SweepLocked(now);

        // WatchIni: the ini was saved, so read the multipliers again. Every model is prepared again with them
        // (the parsed .phy2 files stay cached) and every instance starts over from its animated pose.
        if (g_watchIni && now - g_lastIniPoll >= 1000)
        {
            g_lastIniPoll = now;
            const uint64_t stamp = host::IniStamp();
            const bool iniSaved = stamp && stamp != g_iniStamp;
            const bool csvSaved = OverrideStamp() != g_ovrStamp;
            if (iniSaved || csvSaved)
            {
                if (iniSaved) LoadTuning();
                if (csvSaved) LoadOverrides();
                else UpdateUnderwaterActive();   // LoadTuning cleared g_uwActive; the csv rows in memory still count
                g_models.clear();
                g_inst.clear();
                g_stats.live = 0;
                wxl::LogCat("PHYS", "physics: %s saved, multipliers applied to every model again",
                            iniSaved && csvSaved ? "ini and csv" : iniSaved ? "ini" : "csv");
            }
        }
        {
            // an instance that was re-seeded too often stays animated for a while
            auto pen = g_penalty.find(instance);
            if (pen != g_penalty.end() && pen->second.disabledUntil && int32_t(now - pen->second.disabledUntil) < 0) return;
        }

        uint32_t pendingGen = g_bindGen;   // the binding generation `display` below belongs to
        // The display this instance shows, for PhysicsOverride.csv. Worked out when the instance is new or
        // when the held items changed (g_bindGen); calls into the core, so with the lock released.
        Display display;
        {
            const auto cur = g_inst.find(instance);
            if (cur != g_inst.end() && cur->second.shared == s.model && cur->second.bindGen == g_bindGen)
                display = cur->second.display;
            else
            {
                const uint32_t gen = g_bindGen;
                lk.unlock();
                display = ResolveDisplay(api, instance, stem);
                lk.lock();
                pendingGen = gen;
            }
        }

        // This model's physics, looked up once (and again if the slot was reused by another file).
        // The lookup reads files, so it runs with the lock released: other animate threads keep going.
        std::shared_ptr<phys::Model> model;
        auto mi = g_models.find(s.model);
        if (mi == g_models.end() || mi->second.stem != stem)
        {
            lk.unlock();
            bool fresh = false;
            const phy2::Result res = g_lib.For(stem, &fresh);
            if (fresh)
            {
                if (res.model)
                    wxl::LogCat("PHYS", "physics: MATCH '%s' <- %s (%u bones): %zu bodies, %zu joints", stem,
                                res.path.c_str(), boneCount, res.model->bodies.size(), res.model->joints.size());
                else if (!res.error.empty())
                    wxl::LogCat("PHYS", "physics: '%s' has a .phy2 that cannot be used: %s", stem, res.error.c_str());
                else if (std::strstr(stem, "objectcomponents") && wxl::LogThrottle("phys.nofile", 100, 0))
                    wxl::LogCat("PHYS", "physics: no .phy2 for item model '%s' (%u bones)", stem, boneCount);
            }
            lk.lock();
            // extra anchors carry their followers (PhysicsGuard.hpp); a model that needs nothing is used as is
            int carried = 0;
            ModelEntry entry;
            entry.stem = stem;
            entry.base = guard::PrepareModel(res.model, g_carryAnchors, &carried);
            if (carried)
            {
                ++g_stats.modelsCarried;
                wxl::LogCat("PHYS", "physics: '%s' has several anchors and no root body: %d anchor(s) now carry their followers",
                            stem, carried);
            }
            mi = g_models.insert_or_assign(s.model, std::move(entry)).first;
        }
        if (mi->second.base) model = VariantLocked(mi->second, display, stem);
        if (!model) return;

        // Underwater: is this instance, or the unit it hangs on, swimming? (the core answers from the unit's own
        // swim state, so this is a flag read, not a world query)
        float uwTarget = 0;
        if (g_uwActive && g_hasSwimApi) uwTarget = api->instanceSwimming(instance) ? 1.0f : 0.0f;

        std::vector<float> before(size_t(boneCount) * 16);
        std::vector<int16_t> parents(boneCount);
        std::vector<phys::V3> pivots(boneCount);
        if (api->readBones(instance, boneCount, before.data(), parents.data(), reinterpret_cast<float*>(pivots.data())) != WXL_READ_OK)
        {
            ++g_stats.faults;
            return;
        }
        for (const phys::BodyDef& b : model->bodies)
            if (b.bone != 0xFFFF && b.bone >= boneCount)
            {
                if (wxl::LogThrottle("phys.bones", 20, 0))
                    wxl::LogCat("PHYS", "physics: .phy2 for '%s' names bone %u but the model has %u", stem, b.bone, boneCount);
                return;   // the physics does not fit this model
            }

        InstState& st = g_inst[instance];
        if (st.shared != s.model || !st.sim || st.dry != model)
        {
            st = InstState{};
            st.shared = s.model;
            st.dry = model;
            st.uwBlend = uwTarget;   // an instance that appears in the water starts with the water's constants
            st.uwLevel = g_uwActive ? int(std::lround(uwTarget * kUwLevels)) : 0;
            st.model = st.uwLevel ? VariantLocked(mi->second, display, stem, st.uwLevel) : model;   // keeps it alive
            st.sim = std::make_unique<phys::Instance>(st.model.get());
            if (g_guardCfg.enabled) st.limits = guard::Build(*model, pivots.data(), boneCount, g_guardCfg);
        }
        st.lastSeen = now;
        st.display = display;
        st.bindGen = pendingGen;

        // the frame's time: the scene clock; a second build in one frame takes 0 s; a clock standing
        // still for 0.1 s (paused scenes, UI models) falls back to the high-resolution clock
        float dt = 0;
        if (st.haveClock)
        {
            if (s.clockMs != st.lastClock)
            {
                dt = float(s.clockMs - st.lastClock) * 0.001f;
                st.stillSince = t0;
            }
            else if (std::chrono::duration<double>(t0 - st.stillSince).count() > 0.1)
            {
                dt = float(std::chrono::duration<double>(t0 - st.lastTime).count());
            }
        }
        else st.stillSince = t0;
        if (dt < 0 || dt > 1.0f) dt = 0;   // clock wrapped or the instance was away
        st.haveClock = true;
        st.lastClock = s.clockMs;
        st.lastTime = t0;

        // Underwater blend: ease towards the target (0 dry, 1 swimming) over BlendSeconds, and when the step
        // changes, switch the running instance to that level's constants. Retune keeps the pose and velocities.
        if (g_uwActive || st.uwLevel != 0)
        {
            const float step = g_uwBlendSec > 0.0f ? dt / g_uwBlendSec : 1.0f;
            st.uwBlend += std::max(-step, std::min(uwTarget - st.uwBlend, step));
            const int level = int(std::lround(std::max(0.0f, std::min(st.uwBlend, 1.0f)) * kUwLevels));
            if (level != st.uwLevel && !st.uwRetuneFailed)
            {
                const std::shared_ptr<phys::Model> lm = VariantLocked(mi->second, display, stem, level);
                if (lm && st.sim->Retune(lm.get()))
                {
                    if ((level == 0) != (st.uwLevel == 0) && wxl::LogThrottle("phys.uw", 40, 0))
                        wxl::LogCat("PHYS", "physics: '%s' %s the water (blend step %d of %d)", stem,
                                    level > st.uwLevel ? "enters" : "leaves", level, kUwLevels);
                    st.model = lm;   // the instance now reads this copy: keep it alive
                    st.uwLevel = level;
                }
                else
                {
                    st.uwRetuneFailed = true;
                    if (wxl::LogThrottle("phys.uwfail", 10, 0))
                        wxl::LogCat("PHYS", "physics: '%s' cannot switch to its underwater constants (layout differs)", stem);
                }
            }
        }

        // inputs
        std::vector<double> world(size_t(boneCount) * 16);
        for (uint32_t k = 0; k < boneCount; ++k)
            phys::World(&before[size_t(k) * 16], s.sceneMatrix, &world[size_t(k) * 16]);
        phys::Frame f;
        f.boneWorld = reinterpret_cast<const double (*)[16]>(world.data());
        f.pivots = pivots.data();
        f.boneCount = boneCount;
        phys::World(s.ownMatrix, s.sceneMatrix, f.placement);
        f.dt = dt;
        st.sim->Step(f);
        if (dt > 0 && ++g_stats.steps == 1) wxl::LogCat("PHYS", "physics: first simulation step ran ('%s')", stem);

        // outputs: each dynamic body's bone, then everything under a moved bone
        std::vector<float> bones(before);
        std::vector<uint8_t> movedBytes(boneCount, 0);
        bool any = false;
        for (size_t b = 0; b < model->bodies.size(); ++b)
        {
            const uint16_t bone = model->bodies[b].bone;
            if (bone >= boneCount) continue;
            double M[16];
            if (!st.sim->Output(b, M)) continue;
            phys::FromModel(M, s.ownMatrix, &bones[size_t(bone) * 16]);
            movedBytes[bone] = 1;
            any = true;
        }
        if (!any) return;
        std::unique_ptr<bool[]> moved(new bool[boneCount]);
        for (uint32_t k = 0; k < boneCount; ++k) moved[k] = movedBytes[k] != 0;
        phys::Carry(parents.data(), boneCount, before.data(), bones.data(), moved.get());
        for (float v : bones)
            if (!(v == v)) { ++g_stats.faults; g_inst.erase(instance); return; }   // never write a NaN

        // The safety net: is this instance stuck or flung, or did it teleport? If so, leave the palette as
        // the engine built it and start the simulation over from the animated pose on the next frame.
        if (g_guardCfg.enabled)
        {
            // |simulated bone - animated bone| at each checked bone's pivot, in model units (the palette is
            // camera-relative and carries the model's scale, so the distance is divided by that scale)
            const float* own = s.ownMatrix;
            const float scale = std::max(std::sqrt(own[4] * own[4] + own[5] * own[5] + own[6] * own[6]), 0.016f);
            float dispBuf[64];
            std::vector<float> dispHeap;
            float* disp = dispBuf;
            if (st.limits.bones.size() > 64) { dispHeap.resize(st.limits.bones.size()); disp = dispHeap.data(); }
            for (size_t i = 0; i < st.limits.bones.size(); ++i)
            {
                const uint16_t b = st.limits.bones[i];
                const phys::V3 pv = pivots[b];
                const float* A = &before[size_t(b) * 16];
                const float* B = &bones[size_t(b) * 16];
                const float ax = pv.x * A[0] + pv.y * A[4] + pv.z * A[8] + A[12];
                const float ay = pv.x * A[1] + pv.y * A[5] + pv.z * A[9] + A[13];
                const float az = pv.x * A[2] + pv.y * A[6] + pv.z * A[10] + A[14];
                const float bx = pv.x * B[0] + pv.y * B[4] + pv.z * B[8] + B[12];
                const float by = pv.x * B[1] + pv.y * B[5] + pv.z * B[9] + B[13];
                const float bz = pv.x * B[2] + pv.y * B[6] + pv.z * B[10] + B[14];
                disp[i] = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay) + (bz - az) * (bz - az)) / scale;
            }
            const double where[3] = { f.placement[3], f.placement[7], f.placement[11] };
            float worst = 0;
            uint16_t worstBone = 0xFFFF;
            const guard::Verdict verdict = guard::Judge(st.limits, st.judge, where, disp, dt, g_guardCfg, &worst, &worstBone);
            if (verdict != guard::Verdict::Ok)
            {
                const bool teleport = verdict == guard::Verdict::Teleport;
                if (teleport) ++g_stats.teleports; else ++g_stats.unstuck;
                if (wxl::LogThrottle("phys.unstick", 60, 50))
                    wxl::LogCat("PHYS", "physics: %s '%s': %s, re-seeding (bone %u at %.0f%% of its limit, dt %.3f s)",
                                teleport ? "teleport" : "stuck", stem, teleport ? "placement moved far in one frame" : "bones far from the animated pose",
                                unsigned(worstBone), worst * 100.0f, dt);

                Penalty& pen = g_penalty[instance];
                pen.lastTouched = now;
                if (now - pen.windowStart > kPenaltyWindowMs) { pen.windowStart = now; pen.count = 0; }
                if (++pen.count >= kPenaltyMax)
                {
                    pen.count = 0;
                    pen.disabledUntil = now + kPenaltyOffMs;
                    ++g_stats.disabled;
                    if (wxl::LogThrottle("phys.disabled", 20, 0))
                        wxl::LogCat("PHYS", "physics: '%s' re-seeded %u times in %u s: physics off for it for %u s", stem, kPenaltyMax,
                                    kPenaltyWindowMs / 1000, kPenaltyOffMs / 1000);
                }
                g_inst.erase(instance);
                return;
            }
        }
        if (api->writePalette(instance, bones.data(), boneCount) != WXL_READ_OK)
        {
            ++g_stats.faults;
            g_inst.erase(instance);
            return;
        }

        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        g_stats.lastFrameMs = ms;
        if (ms > g_stats.worstFrameMs) g_stats.worstFrameMs = ms;
        g_stats.live = uint32_t(g_inst.size());
    }
}
