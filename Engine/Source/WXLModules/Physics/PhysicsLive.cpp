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
#include "PhysicsTuning.hpp"
#include "physics_domino.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl::equip::physics
{
    namespace phys = wow::ec::physics;
    namespace host = wxl::modules::host;

    static_assert(sizeof(phys::V3) == 12, "pivots are handed to the API as 3 floats per bone");

    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr uint32_t kForgetMs = 2000;

        uint32_t NowMs()
        {
            return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
        }

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
            std::shared_ptr<phys::Model> model;   // null: no physics for this model
        };

        std::mutex g_lock;                                                // guards the three below and g_stats
        std::unordered_map<const void*, ModelEntry> g_models;             // loaded model -> its physics
        std::unordered_map<void*, InstState> g_inst;                      // instance -> simulation
        std::unordered_map<void*, Penalty> g_penalty;                     // instance -> re-seed history
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
        bool g_watchIni = false;       // [Physics] WatchIni: re-read the multipliers when the ini is saved
        uint64_t g_iniStamp = 0;       // the ini's last-write time when the multipliers were last read
        uint32_t g_lastIniPoll = 0;    // NowMs of the last look at it

        bool ReadPhysicsKey(const char* key, char* out, size_t cap)
        {
            return host::IniGetString("Physics", key, out, cap);
        }

        void LoadTuning()
        {
            std::string problems;
            g_tuning = tuning::FromIni(&ReadPhysicsKey, &problems);
            g_iniStamp = host::IniStamp();
            // one log line per problem: a value that is not a number, or outside its range
            for (size_t b = 0; b < problems.size();)
            {
                const size_t e = problems.find('\n', b);
                wxl::LogCat("PHYS", "physics: ini: %s", problems.substr(b, e == std::string::npos ? e : e - b).c_str());
                if (e == std::string::npos) break;
                b = e + 1;
            }
            wxl::LogCat("PHYS", "physics: multipliers: %s", tuning::Describe(g_tuning).c_str());
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
        LoadTuning();
        if (g_watchIni) wxl::LogCat("PHYS", "physics: WatchIni=1, the multipliers are re-read whenever the ini is saved");

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
        wxl::LogCat("PHYS", "physics: file cache cleared, .phy2 files and the multipliers will be read again");
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
            if (stamp && stamp != g_iniStamp)
            {
                LoadTuning();
                g_models.clear();
                g_inst.clear();
                g_stats.live = 0;
                wxl::LogCat("PHYS", "physics: ini saved, multipliers applied to every model again");
            }
        }
        {
            // an instance that was re-seeded too often stays animated for a while
            auto pen = g_penalty.find(instance);
            if (pen != g_penalty.end() && pen->second.disabledUntil && int32_t(now - pen->second.disabledUntil) < 0) return;
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
            model = guard::PrepareModel(res.model, g_carryAnchors, &carried);
            int tuned = 0;
            model = tuning::Apply(model, g_tuning, &tuned);   // a copy only when a multiplier changes something
            if (tuned) ++g_stats.modelsTuned;
            if (carried)
            {
                ++g_stats.modelsCarried;
                wxl::LogCat("PHYS", "physics: '%s' has several anchors and no root body: %d anchor(s) now carry their followers",
                            stem, carried);
            }
            g_models[s.model] = ModelEntry{ stem, model };
        }
        else model = mi->second.model;
        if (!model) return;

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
        if (st.shared != s.model || !st.sim)
        {
            st = InstState{};
            st.shared = s.model;
            st.model = model;
            st.sim = std::make_unique<phys::Instance>(model.get());
            if (g_guardCfg.enabled) st.limits = guard::Build(*model, pivots.data(), boneCount, g_guardCfg);
        }
        st.lastSeen = now;

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
