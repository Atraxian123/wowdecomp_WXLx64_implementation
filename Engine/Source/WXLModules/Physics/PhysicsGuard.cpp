// See PhysicsGuard.hpp.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "PhysicsGuard.hpp"

#include <algorithm>
#include <cmath>

namespace wxl::equip::physics::guard
{
    namespace
    {
        constexpr uint16_t kGroupRoot = 0x4000;
        constexpr uint16_t kGroupHead = 0x8000;

        float Dist(const phys::V3& a, const phys::V3& b)
        {
            const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
            return std::sqrt(x * x + y * y + z * z);
        }
    }

    std::shared_ptr<phys::Model> PrepareModel(const std::shared_ptr<phys::Model>& in, bool carryAnchors, int* changed)
    {
        if (changed) *changed = 0;
        if (!in || !carryAnchors) return in;

        // With a root body the retail loop already carries every anchor by the root's delta.
        bool hasRoot = false;
        int kinematic = 0, loose = 0;
        for (const phys::BodyDef& b : in->bodies)
        {
            if (b.group == kGroupRoot) hasRoot = true;
            if (b.type == phys::kBodyKinematic)
            {
                ++kinematic;
                if (b.group == 0) ++loose;
            }
        }
        // One anchor is the head the engine already picks. Two or more: the second one on is not carried.
        if (hasRoot || kinematic < 2 || loose == 0) return in;

        auto out = std::make_shared<phys::Model>(*in);
        int n = 0;
        for (phys::BodyDef& b : out->bodies)
        {
            if (b.type == phys::kBodyKinematic && b.group == 0)
            {
                b.group = kGroupHead;
                ++n;
            }
        }
        if (changed) *changed = n;
        return out;
    }

    Limits Build(const phys::Model& m, const phys::V3* pivots, size_t boneCount, const Config& cfg)
    {
        Limits lim;
        if (!pivots) return lim;

        std::vector<phys::V3> anchors;
        for (const phys::BodyDef& b : m.bodies)
        {
            const bool driven = b.type != phys::kBodyDynamic || b.group == kGroupRoot || b.group == kGroupHead;
            if (driven && b.bone < boneCount) anchors.push_back(pivots[b.bone]);
        }
        if (anchors.empty()) return lim;

        for (const phys::BodyDef& b : m.bodies)
        {
            if (b.type != phys::kBodyDynamic || b.group == kGroupRoot || b.group == kGroupHead) continue;
            if (b.bone >= boneCount) continue;
            float chain = 1e30f;
            for (const phys::V3& a : anchors) chain = std::min(chain, Dist(pivots[b.bone], a));
            lim.bones.push_back(b.bone);
            lim.limit.push_back(cfg.reach * chain + cfg.slack);
        }
        return lim;
    }

    Verdict Judge(const Limits& lim, State& st, const double placement[3], const float* disp, float dt,
                  const Config& cfg, float* worst, uint16_t* worstBone)
    {
        if (worst) *worst = 0;
        Verdict v = Verdict::Ok;

        // a teleport: the placement itself moved farther in one frame than anything walks, rides or falls
        if (st.havePos)
        {
            const double dx = placement[0] - st.lastPos[0], dy = placement[1] - st.lastPos[1], dz = placement[2] - st.lastPos[2];
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > double(cfg.teleportYards)) v = Verdict::Teleport;
        }
        st.havePos = true;
        st.lastPos[0] = placement[0];
        st.lastPos[1] = placement[1];
        st.lastPos[2] = placement[2];
        if (v == Verdict::Teleport) { st.overSec = 0; return v; }

        // a stuck or flung bone: over its limit for graceSec of simulated time
        float ratio = 0;
        uint16_t bone = 0xFFFF;
        bool bad = false;
        for (size_t i = 0; i < lim.bones.size(); ++i)
        {
            const float r = lim.limit[i] > 0 ? disp[i] / lim.limit[i] : 0;
            if (!(r == r)) { ratio = 1e9f; bone = lim.bones[i]; bad = true; break; }   // NaN
            if (r > ratio) { ratio = r; bone = lim.bones[i]; }
        }
        if (ratio > 1.0f) bad = true;
        if (worst) *worst = ratio;
        if (worstBone) *worstBone = bone;

        const float step = std::min(std::max(dt, 0.0f), 0.1f);
        if (bad) st.overSec += step;
        else st.overSec = std::max(0.0f, st.overSec - 2.0f * step);   // swinging back in is forgiven quickly
        return st.overSec >= cfg.graceSec ? Verdict::Stuck : Verdict::Ok;
    }
}
