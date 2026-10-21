// Test program for the physics "unstick" work. Standalone: no client, no CMake target.
//
//   g++ -std=c++17 -O1 -I <repo root> physics_guard_test.cpp \
//       <repo root>/Engine/Source/WXLEquip/Physics/PhysicsGuard.cpp \
//       <repo root>/Engine/Source/WXLEquip/Physics/physics_domino.cpp -o physics_guard_test
//   ./physics_guard_test            (exit code 0 = every check passed)
//
// It drives the real engine (physics_domino.cpp, unchanged) with synthetic items and a character that
// walks, jumps, climbs a flight of stairs and teleports, and checks:
//   1. the problem: a second kinematic anchor with no carry group lags far behind during a jump or a
//      flight of stairs; PrepareModel fixes it;
//   2. Judge never fires on a healthy item (no false positives);
//   3. Judge does fire on a flung chain (stuck) and on a teleport.
#include "Engine/Source/WXLEquip/Physics/PhysicsGuard.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

using namespace wow::ec::physics;
namespace gd = wxl::equip::physics::guard;

namespace
{
    int g_failed = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) ++g_failed;
    }

    struct Pose { double x, y, z, yaw; };
    void Mat(const Pose& p, double M[16])   // column form, model -> world
    {
        const double c = std::cos(p.yaw), s = std::sin(p.yaw);
        const double T[16] = { c, -s, 0, p.x, s, c, 0, p.y, 0, 0, 1, p.z, 0, 0, 0, 1 };
        std::memcpy(M, T, sizeof T);
    }

    // chains hanging from kinematic anchors: anchor c sits at x = 0.4 * c; its chain follows it in body order
    struct Rig
    {
        Model model;
        std::vector<V3> pivots;     // per bone: 0 = root, 1.. = anchors, then the dynamic bones
        int anchors = 1, segs = 4;
        float seg = 0.25f;
    };

    Rig MakeRig(int anchors, int segs, float carry, float damp, float headRadius = 0.02f)
    {
        Rig r;
        r.anchors = anchors;
        r.segs = segs;
        r.pivots.assign(1 + anchors + anchors * segs, V3{});
        for (int c = 0; c < anchors; ++c)
        {
            const float ox = 0.4f * c;
            BodyDef k;
            k.type = kBodyKinematic; k.bone = uint16_t(1 + c); k.pos = { ox, 0, 0 }; k.gravity = 0; k.carry = carry;
            const int kidx = int(r.model.bodies.size());
            r.model.bodies.push_back(k);
            r.pivots[1 + c] = { ox, 0, 0 };
            ShapeDef ks; ks.body = uint8_t(kidx); ks.type = kShapeSphere; ks.a = { ox, 0, 0 }; ks.r = headRadius;
            r.model.shapes.push_back(ks);
            for (int i = 0; i < segs; ++i)
            {
                BodyDef b;
                b.type = kBodyDynamic; b.bone = uint16_t(1 + anchors + c * segs + i); b.pos = { ox, 0, -r.seg * i };
                b.gravity = 1; b.linDamp = damp; b.angDamp = damp;
                const int bi = int(r.model.bodies.size());
                r.model.bodies.push_back(b);
                r.pivots[b.bone] = b.pos;
                ShapeDef s; s.body = uint8_t(bi); s.type = kShapeCapsule; s.a = { 0, 0, 0 }; s.b = { 0, 0, -r.seg }; s.r = 0.03f;
                r.model.shapes.push_back(s);
                JointDef j; j.type = kJointShoulder; j.a = uint8_t(i == 0 ? kidx : bi - 1); j.b = uint8_t(bi);
                j.pa = i == 0 ? V3{ 0, 0, 0 } : V3{ 0, 0, -r.seg }; j.pb = {};
                j.f[0] = -45; j.f[1] = 45; j.f[2] = 45;
                r.model.joints.push_back(j);
            }
        }
        return r;
    }

    struct Outcome
    {
        double tipLag = 0;          // chain of the LAST anchor: farthest its tip got from the rest-hang tip
        double finalErr = 0;        // ... after the hold
        int stuck = 0, teleports = 0, firstStuckFrame = -1, firstTeleportFrame = -1;
        float worstRatio = 0;
    };

    // Runs a rig through a motion, optionally with the guard.
    Outcome Run(const std::shared_ptr<Model>& model, const Rig& rig, double ox, double fps, double moveEnd, double hold,
                const std::function<Pose(double)>& poseAt, bool useGuard)
    {
        Outcome o;
        Instance inst(model.get());
        const gd::Config cfg;
        const gd::Limits lim = gd::Build(*model, rig.pivots.data(), rig.pivots.size(), cfg);
        gd::State st;
        const double dt = 1.0 / fps;
        const int lastChain = rig.anchors - 1;
        int frame = 0;
        for (double t = 0; t <= moveEnd + hold + 1e-9; t += dt, ++frame)
        {
            Pose p = poseAt(std::min(t, moveEnd));
            p.x += ox;
            std::vector<double> W(rig.pivots.size() * 16);
            for (size_t b = 0; b < rig.pivots.size(); ++b) Mat(p, &W[b * 16]);
            Frame f;
            f.boneWorld = reinterpret_cast<const double (*)[16]>(W.data());
            f.pivots = rig.pivots.data();
            f.boneCount = rig.pivots.size();
            Mat(p, f.placement);
            f.dt = float(dt);
            inst.Step(f);

            // displacement of every dynamic bone from its animated pose (model space), like the glue
            std::vector<float> disp(lim.bones.size(), 0.0f);
            for (size_t i = 0; i < lim.bones.size(); ++i)
            {
                size_t body = 0;
                for (size_t b = 0; b < model->bodies.size(); ++b)
                    if (model->bodies[b].bone == lim.bones[i]) body = b;
                double M[16];
                if (!inst.Output(body, M)) continue;
                const V3 pv = rig.pivots[lim.bones[i]];
                const double px = M[3] + M[0] * pv.x + M[1] * pv.y + M[2] * pv.z;
                const double py = M[7] + M[4] * pv.x + M[5] * pv.y + M[6] * pv.z;
                const double pz = M[11] + M[8] * pv.x + M[9] * pv.y + M[10] * pv.z;
                disp[i] = float(std::sqrt((px - pv.x) * (px - pv.x) + (py - pv.y) * (py - pv.y) + (pz - pv.z) * (pz - pv.z)));
                if (int(lim.bones[i]) == int(1 + rig.anchors + lastChain * rig.segs + rig.segs - 1))
                {
                    o.tipLag = std::max(o.tipLag, double(disp[i]));
                    if (t > moveEnd) o.finalErr = disp[i];
                }
            }
            if (useGuard)
            {
                const double pos[3] = { f.placement[3], f.placement[7], f.placement[11] };
                float worst = 0;
                const gd::Verdict v = gd::Judge(lim, st, pos, disp.data(), float(dt), cfg, &worst);
                o.worstRatio = std::max(o.worstRatio, worst);
                if (v == gd::Verdict::Stuck) { if (!o.stuck++) o.firstStuckFrame = frame; st = gd::State{}; }
                if (v == gd::Verdict::Teleport) { if (!o.teleports++) o.firstTeleportFrame = frame; }
            }
        }
        return o;
    }

    Pose Jump(double t)
    {
        double z = 7.9555 * t - 0.5 * 19.29 * t * t;
        if (z < 0) z = 0;
        return { 7 * t, 0, z, 0.3 };
    }
    Pose Stairs(double t) { return { 5 * t, 0, 0.4 * std::floor(t / 0.15), 0.3 }; }
    Pose SteepStairs(double t) { return { 5 * t, 0, 0.7 * std::floor(t / 0.2), 0.3 }; }
    Pose Walk(double t) { return { 7 * t, 0, 0, 0.3 }; }
    Pose Turn(double t) { return { 0, 0, 0, 6.28 * t }; }
    Pose Teleport(double t) { return { t < 0.5 ? 0.0 : 30.0, 0, t < 0.5 ? 0.0 : 6.0, 0.3 }; }
}

int main()
{
    const double kWorld = -8913.2;   // a real map position: the engine's floats are world-sized
    struct Motion { const char* name; double fps, end; std::function<Pose(double)> f; };
    const Motion motions[] = {
        { "jump", 60, 1.0, Jump }, { "stairs 0.4yd", 60, 2.0, Stairs }, { "steep stairs 0.7yd", 60, 2.0, SteepStairs },
        { "jump @20fps", 20, 1.0, Jump },
    };

    std::printf("1. a second kinematic anchor with no carry group (the stuck chain)\n");
    {
        Rig rig = MakeRig(2, 4, 0.9f, 2.0f);
        auto raw = std::make_shared<Model>(rig.model);
        int changed = 0;
        auto fixed = gd::PrepareModel(raw, true, &changed);
        Check(changed == 2 && fixed != raw, "PrepareModel names both anchors head (the engine only names the first)");
        Check(gd::PrepareModel(raw, false) == raw, "PrepareModel does nothing when switched off");
        for (const Motion& m : motions)
        {
            const Outcome a = Run(raw, rig, kWorld, m.fps, m.end, 5.0, m.f, false);
            const Outcome b = Run(fixed, rig, kWorld, m.fps, m.end, 5.0, m.f, false);
            std::printf("     %-20s tip strays up to %.2f yd as shipped, %.2f yd with the fix\n", m.name, a.tipLag, b.tipLag);
            if (m.fps >= 60) Check(a.tipLag > 3 * b.tipLag + 0.05, m.name);
            else Check(b.tipLag <= a.tipLag + 1e-3, m.name);
            Check(b.finalErr < 0.05, "    ... and it hangs straight again after the hold");
        }
        // single anchor / explicit root: nothing to fix
        Rig one = MakeRig(1, 4, 0.9f, 2.0f);
        Check(gd::PrepareModel(std::make_shared<Model>(one.model), true) != nullptr &&
              gd::PrepareModel(std::make_shared<Model>(one.model), true)->bodies[0].group == 0, "a single-anchor model is left alone");
        Rig rooted = MakeRig(2, 4, 0.9f, 2.0f);
        rooted.model.bodies[0].group = 0x4000;
        auto rp = std::make_shared<Model>(rooted.model);
        Check(gd::PrepareModel(rp, true) == rp, "a model with a root body is left alone");
    }

    std::printf("2. the judge on healthy items: never fires\n");
    {
        const Motion all[] = {
            { "idle", 60, 0.1, [](double) { return Pose{ 0, 0, 0, 0.3 }; } }, { "walk", 60, 3.0, Walk },
            { "jump", 60, 1.0, Jump }, { "stairs", 60, 2.0, Stairs }, { "steep stairs", 60, 2.0, SteepStairs },
            { "jump @20fps", 20, 1.0, Jump }, { "turn 360deg/s", 60, 2.0, Turn },
        };
        for (float damp : { 0.3f, 2.0f })
            for (int anchors : { 1, 2 })
            {
                Rig rig = MakeRig(anchors, 4, 0.9f, damp);
                auto model = gd::PrepareModel(std::make_shared<Model>(rig.model), true);
                int fired = 0;
                float worst = 0;
                for (const Motion& m : all)
                {
                    const Outcome o = Run(model, rig, kWorld, m.fps, m.end, 3.0, m.f, true);
                    fired += o.stuck + o.teleports;
                    worst = std::max(worst, o.worstRatio);
                }
                std::printf("     %d anchor(s), damping %.1f: worst displacement is %.0f%% of its limit\n", anchors, damp, worst * 100);
                Check(fired == 0, "no stuck / teleport verdict on any motion");
            }
    }

    std::printf("3. the judge on failures\n");
    {
        // a chain that was flung: the head sphere is fatter than the first segment's anchor distance, so
        // the contact solver throws the chain away (the same state a real model reaches when it is stuck)
        Rig rig = MakeRig(1, 4, 0.9f, 2.0f, 0.15f);
        for (auto& j : rig.model.joints) { j.type = kJointWeld; j.f[0] = 1; j.f[1] = 0.2f; j.f[2] = 1; j.f[3] = 0.2f; }
        auto model = std::make_shared<Model>(rig.model);
        const Outcome o = Run(model, rig, kWorld, 60, 0.1, 3.0, [](double) { return Pose{ 0, 0, 0, 0.3 }; }, true);
        std::printf("     flung chain: strays %.2f yd, judged stuck at frame %d (%.2f s), %d verdict(s)\n", o.tipLag, o.firstStuckFrame,
                    o.firstStuckFrame / 60.0, o.stuck);
        Check(o.tipLag > 1.0, "the test chain really was flung (> 1 yd away)");
        Check(o.stuck >= 1 && o.firstStuckFrame >= 0 && o.firstStuckFrame / 60.0 < 1.0, "judged stuck within a second");

        Rig healthy = MakeRig(1, 4, 0.9f, 2.0f);
        auto hm = std::make_shared<Model>(healthy.model);
        const Outcome t = Run(hm, healthy, kWorld, 60, 1.0, 1.0, Teleport, true);
        std::printf("     teleport 30 yd: verdict at frame %d (the move is at frame 30)\n", t.firstTeleportFrame);
        Check(t.teleports == 1 && (t.firstTeleportFrame == 30 || t.firstTeleportFrame == 31), "a 30 yd jump of the placement is a teleport, once");
        Check(t.stuck == 0, "...and not also stuck");
    }

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
