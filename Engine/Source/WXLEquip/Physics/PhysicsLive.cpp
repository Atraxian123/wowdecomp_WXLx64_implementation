// Runtime physics for model instances. See PhysicsLive.hpp.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// What it does, per animated instance, each time the engine has built its bone palette:
//   * bone world = palette bone x the scene matrix (CM2Scene::m_viewInv, a MIRROR: never decomposed);
//     the placement = the instance's own matrix (CM2Model::m_matrixF4) x the scene matrix;
//   * each dynamic body's model-space bone goes back as bone x m_matrixF4, and every bone under a
//     moved one is carried by its parent's delta;
//   * time comes from the scene's clock (CM2Scene::m_time, ms); a second build in one frame takes 0 s;
//   * state per instance, forgotten when unseen for 2 s; every client read and write is guarded.

#include "Engine/Source/WXLEquip/Physics/PhysicsLive.hpp"

#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/WXL/Event.h"
#include "Engine/Source/WXL/Log.h"
#include "Engine/Source/WXLEquip/Physics/Phy2Library.hpp"
#include "Engine/Source/WXLEquip/Physics/physics_domino.h"
#include "Engine/Source/WXLEquip/VirtualPath.hpp"   // WxlIniGetBool, VPathIsVirtualKey

#include <windows.h>

#include <atomic>
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

    static_assert(sizeof(C44Matrix) == 64, "the bone palette and the matrices are 16 floats");

    namespace
    {
        constexpr uint32_t kMaxBones = 256;
        constexpr uint32_t kForgetMs = 2000;

        struct InstState
        {
            void*                            shared = nullptr;
            std::unique_ptr<phys::Instance>  sim;
            uint32_t lastClock = 0;       // scene clock at the last step
            uint32_t lastSeen  = 0;       // GetTickCount at the last build
            LARGE_INTEGER stillSince{};   // when the scene clock stopped moving
            LARGE_INTEGER lastQpc{};
            bool     haveClock = false;
        };

        struct ModelEntry
        {
            std::string                  stem;    // the path it was looked up under (a slot can be reused)
            std::shared_ptr<phys::Model> model;   // null: no physics for this model
        };

        std::mutex g_lock;                                            // guards the three below and g_stats
        std::unordered_map<void*, ModelEntry> g_models;               // shared model -> its physics
        std::unordered_map<void*, InstState> g_inst;                  // instance -> simulation
        Stats g_stats;
        uint32_t g_lastSweep = 0;
        std::atomic<bool> g_enabled{false};
        bool g_inited = false;
        LARGE_INTEGER g_qpcFreq{};

        // ---- where the .phy2 files are read from ----
        // The client's own open/size/read/close. Archives only first, then archives plus loose files
        // under Data\ (open flag 1), the same order as the equip extension's native reader, so a .phy2
        // dropped loose next to where the model lives is found too. No logging here: a miss is the
        // normal answer for almost every model.
        bool ReadArchiveFile(const char* name, std::vector<uint8_t>& out)
        {
            for (uint32_t flags = 0; flags <= 1; ++flags)
            {
                SFileImpl* file = nullptr;
                if (!SFile::OpenEx(nullptr, name, flags, &file) || !file) continue;
                uint32_t sizeHigh = 0;
                const uint32_t size = SFileGetFileSize(file, &sizeHigh);
                bool ok = false;
                if (size > 0 && size <= (4u << 20) && sizeHigh == 0)   // a model's physics is a few KB
                {
                    out.resize(size);
                    uint32_t got = 0;
                    ok = SFileReadFile(file, out.data(), size, &got, nullptr, 0) && got == size;
                    if (!ok) out.clear();
                }
                SFileCloseFile(file);
                if (ok) return true;
            }
            return false;
        }

        phy2::Library g_lib(&ReadArchiveFile, &wxl::scripts::equipextension::VPathIsVirtualKey);

        // ---- guarded client memory access (no C++ objects inside __try) ----
        bool SafeCopy(void* dst, const void* src, size_t n) noexcept
        {
            __try { std::memcpy(dst, src, n); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        // The instance's inputs, copied out of client memory in one guarded pass.
        struct Snapshot
        {
            CM2Shared*   shared = nullptr;
            const C44Matrix* palette = nullptr;   // the instance's live palette (written back to)
            const M2CompBone* bones = nullptr;    // the model's bone records
            uint32_t boneCount = 0;
            uint32_t clock = 0;
            float    sceneMat[16];
            float    ownMat[16];
            char     stem[260];
        };

        // Returns 0 when every input was read, else the stage that stopped it (for the log).
        int ReadSnapshot(void* instance, Snapshot& s) noexcept
        {
            const CM2Model* m = static_cast<const CM2Model*>(instance);
            const CM2Scene* scene = nullptr;
            uint32_t instFlags = 0;
            const M2Data* hdr = nullptr;
            uint32_t sharedFlags = 0;
            s.stem[0] = 0;

            if (!SafeCopy(&s.shared, &m->m_shared, sizeof(s.shared)) || !s.shared) return 1;
            if (!SafeCopy(s.stem, s.shared->m_fileName, sizeof(s.stem) - 1)) { s.stem[0] = 0; return 11; }
            s.stem[sizeof(s.stem) - 1] = 0;
            // The model must be fully loaded AND parsed: before that bit is set the header's arrays are
            // not relocated, and Data() points at garbage (see EquipExtension SharedFullyParsed).
            if (!SafeCopy(&sharedFlags, &s.shared->m_flags, 4) || !(sharedFlags & 1u)) return 12;
            if (!SafeCopy(&hdr, &s.shared->m_data, sizeof(hdr)) || !hdr) return 4;
            if (!SafeCopy(&s.boneCount, &hdr->m_bones.m_count, 4)) return 5;
            if (s.boneCount < 2 || s.boneCount > kMaxBones) return 6;   // one bone: nothing can swing
            if (!SafeCopy(&instFlags, &m->m_fFlags, 4) || !(instFlags & 1u)) return 13;   // animation data not ready
            if (!SafeCopy(&scene, &m->m_scene, sizeof(scene)) || !scene) return 2;
            if (!SafeCopy(&s.palette, &m->m_boneMatrices, sizeof(s.palette)) || !s.palette) return 3;
            s.bones = hdr->m_bones.Data();
            if (!s.bones) return 7;
            if (!SafeCopy(&s.clock, &scene->m_time, 4)) return 8;
            if (!SafeCopy(s.sceneMat, &scene->m_viewInv, sizeof(s.sceneMat))) return 9;
            if (!SafeCopy(s.ownMat, &m->m_matrixF4, sizeof(s.ownMat))) return 10;
            return 0;
        }

        bool ReadBones(const Snapshot& s, float* bones, int16_t* parents, phys::V3* pivots) noexcept
        {
            if (!SafeCopy(bones, s.palette, size_t(s.boneCount) * 64)) return false;
            for (uint32_t k = 0; k < s.boneCount; ++k)
            {
                uint16_t parent = 0xFFFF;
                C3Vector pivot{};
                if (!SafeCopy(&parent, &s.bones[k].m_parentIndex, 2)) return false;
                if (!SafeCopy(&pivot, &s.bones[k].m_pivot, sizeof(pivot))) return false;
                parents[k] = static_cast<int16_t>(parent);   // 0xFFFF = no parent, as the engine reads it
                pivots[k].x = pivot.x;
                pivots[k].y = pivot.y;
                pivots[k].z = pivot.z;
            }
            return true;
        }

        void SweepLocked(uint32_t now)
        {
            if (now - g_lastSweep < 500) return;
            g_lastSweep = now;
            for (auto it = g_inst.begin(); it != g_inst.end();)
            {
                if (now - it->second.lastSeen > kForgetMs) { it = g_inst.erase(it); ++g_stats.dropped; }
                else ++it;
            }
            g_stats.live = uint32_t(g_inst.size());
        }

        void OnBuildBonePalette(void* /*user*/, const void* args)
        {
            AfterBonePalette(static_cast<const wxl::events::BuildBonePaletteArgs*>(args)->renderCtx);
        }
    }

    void Init()
    {
        if (g_inited) return;
        g_inited = true;
        QueryPerformanceFrequency(&g_qpcFreq);
        g_enabled = wxl::scripts::equipextension::WxlIniGetBool("Physics", "Enabled", false);
        wxl::LogCat("PHYS", "physics: %s (WXLExtendedEquipment.ini [Physics] Enabled); per-model .phy2 beside each model",
                    g_enabled ? "on" : "off");
        // After the equip extension's handler (subscribers run in subscription order).
        wxl::events::Subscribe(wxl::events::Event::OnBuildBonePalette, &OnBuildBonePalette, nullptr);
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
        g_stats = Stats{};
        wxl::LogCat("PHYS", "physics: file cache cleared, .phy2 files will be read again");
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

        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);

        Snapshot s;
        static volatile long calls = 0;
        if (InterlockedIncrement(&calls) == 1) wxl::LogCat("PHYS", "physics: first bone build seen (hook is live)");
        const int stage = ReadSnapshot(instance, s);
        if (stage)
        {
            // Stages 6 (one bone), 12 and 13 (not loaded / animated yet) are the common, harmless cases.
            if (stage != 6 && stage != 12 && stage != 13 && s.shared && wxl::LogThrottle("phys.skip", 40, 500))
                wxl::LogCat("PHYS", "physics: skip '%s' at read stage %d", s.stem[0] ? s.stem : "(no path)", stage);
            return;
        }

        std::unique_lock<std::mutex> lk(g_lock);
        const uint32_t now = GetTickCount();
        SweepLocked(now);

        // This model's physics, looked up once (and again if the slot was reused by another file).
        // The lookup reads files, so it runs with the lock released: other animate threads keep going.
        std::shared_ptr<phys::Model> model;
        auto mi = g_models.find(s.shared);
        if (mi == g_models.end() || mi->second.stem != s.stem)
        {
            lk.unlock();
            bool fresh = false;
            const phy2::Result res = g_lib.For(s.stem, &fresh);
            if (fresh)
            {
                if (res.model)
                    wxl::LogCat("PHYS", "physics: MATCH '%s' <- %s (%u bones): %zu bodies, %zu joints", s.stem,
                                res.path.c_str(), s.boneCount, res.model->bodies.size(), res.model->joints.size());
                else if (!res.error.empty())
                    wxl::LogCat("PHYS", "physics: '%s' has a .phy2 that cannot be used: %s", s.stem, res.error.c_str());
                else if (std::strstr(s.stem, "objectcomponents") && wxl::LogThrottle("phys.nofile", 100, 0))
                    wxl::LogCat("PHYS", "physics: no .phy2 for item model '%s' (%u bones)", s.stem, s.boneCount);
            }
            lk.lock();
            g_models[s.shared] = ModelEntry{ s.stem, res.model };
            model = res.model;
        }
        else model = mi->second.model;
        if (!model) return;

        std::vector<float> before(size_t(s.boneCount) * 16);
        std::vector<int16_t> parents(s.boneCount);
        std::vector<phys::V3> pivots(s.boneCount);
        if (!ReadBones(s, before.data(), parents.data(), pivots.data())) { ++g_stats.faults; return; }
        for (const phys::BodyDef& b : model->bodies)
            if (b.bone != 0xFFFF && b.bone >= s.boneCount)
            {
                if (wxl::LogThrottle("phys.bones", 20, 0))
                    wxl::LogCat("PHYS", "physics: .phy2 for '%s' names bone %u but the model has %u", s.stem, b.bone, s.boneCount);
                return;   // the physics does not fit this model
            }

        InstState& st = g_inst[instance];
        if (st.shared != s.shared || !st.sim)
        {
            st = InstState{};
            st.shared = s.shared;
            st.sim = std::make_unique<phys::Instance>(model.get());
        }
        st.lastSeen = now;

        // the frame's time: the scene clock; a second build in one frame takes 0 s; a clock standing
        // still for 0.1 s (paused scenes, UI models) falls back to the high-resolution clock
        float dt = 0;
        if (st.haveClock)
        {
            if (s.clock != st.lastClock)
            {
                dt = float(s.clock - st.lastClock) * 0.001f;
                st.stillSince = t0;
            }
            else if (g_qpcFreq.QuadPart && double(t0.QuadPart - st.stillSince.QuadPart) / g_qpcFreq.QuadPart > 0.1)
            {
                dt = float(double(t0.QuadPart - st.lastQpc.QuadPart) / g_qpcFreq.QuadPart);
            }
        }
        else st.stillSince = t0;
        if (dt < 0 || dt > 1.0f) dt = 0;   // clock wrapped or the instance was away
        st.haveClock = true;
        st.lastClock = s.clock;
        st.lastQpc = t0;

        // inputs
        std::vector<double> world(size_t(s.boneCount) * 16);
        for (uint32_t k = 0; k < s.boneCount; ++k)
            phys::World(&before[size_t(k) * 16], s.sceneMat, &world[size_t(k) * 16]);
        phys::Frame f;
        f.boneWorld = reinterpret_cast<const double (*)[16]>(world.data());
        f.pivots = pivots.data();
        f.boneCount = s.boneCount;
        phys::World(s.ownMat, s.sceneMat, f.placement);
        f.dt = dt;
        st.sim->Step(f);
        if (dt > 0 && ++g_stats.steps == 1) wxl::LogCat("PHYS", "physics: first simulation step ran ('%s')", s.stem);

        // outputs: each dynamic body's bone, then everything under a moved bone
        std::vector<float> bones(before);
        std::vector<uint8_t> movedBytes(s.boneCount, 0);
        bool any = false;
        for (size_t b = 0; b < model->bodies.size(); ++b)
        {
            const uint16_t bone = model->bodies[b].bone;
            if (bone >= s.boneCount) continue;
            double M[16];
            if (!st.sim->Output(b, M)) continue;
            phys::FromModel(M, s.ownMat, &bones[size_t(bone) * 16]);
            movedBytes[bone] = 1;
            any = true;
        }
        if (!any) return;
        std::unique_ptr<bool[]> moved(new bool[s.boneCount]);
        for (uint32_t k = 0; k < s.boneCount; ++k) moved[k] = movedBytes[k] != 0;
        phys::Carry(parents.data(), s.boneCount, before.data(), bones.data(), moved.get());
        for (float v : bones)
            if (!(v == v)) { ++g_stats.faults; g_inst.erase(instance); return; }   // never write a NaN
        if (!SafeCopy(const_cast<C44Matrix*>(s.palette), bones.data(), bones.size() * sizeof(float)))
        {
            ++g_stats.faults;
            g_inst.erase(instance);
            return;
        }

        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        if (g_qpcFreq.QuadPart)
        {
            const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / g_qpcFreq.QuadPart;
            g_stats.lastFrameMs = ms;
            if (ms > g_stats.worstFrameMs) g_stats.worstFrameMs = ms;
        }
        g_stats.live = uint32_t(g_inst.size());
    }
}
