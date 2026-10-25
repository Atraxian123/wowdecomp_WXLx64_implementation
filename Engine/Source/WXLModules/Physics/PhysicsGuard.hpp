// Keeps a simulated item from staying stuck. Engine-free: no client types, so the same code runs in the
// client (PhysicsLive.cpp) and in the test program (Tools/PhysicsGuardTest).
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// Two separate problems, two separate tools.
//
// 1. PrepareModel: a model with SEVERAL kinematic anchors and no root body (BDY4 group 0x4000) is carried by
//    the retail rules only through its FIRST anchor ("head", group 0x8000, picked by the engine when no body
//    has a group). A second anchor teleports with its bone, but the dynamic bodies hanging from it are not
//    carried, so the joints have to drag them: during a jump or a flight of stairs the bone moves faster than
//    the joint solver can follow, and that chain trails 1 yd or more behind the item for as long as the
//    motion lasts (measured by Tools/PhysicsGuardTest, scenario "second anchor"). PrepareModel gives every
//    such anchor the head group, so its followers move with it, the way the first anchor's do.
//
// 2. Judge: the safety net. Whatever the cause, a dynamic bone that stays much farther from where the
//    animation puts it than its own distance to the nearest anchor allows is stuck or has been flung. The
//    caller then drops the instance (it is re-seeded from the animated pose on the next frame) and does not
//    write the palette that frame. A placement that jumps far in one frame (teleport, mount, phase) takes
//    the same way out. Nothing here can put a body to sleep: the engine never sleeps an item world (see
//    physics_domino.cpp, "item bodies are made with sleeping off"), so "stuck" is never sleep.
#pragma once

#include "physics_domino.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace wxl::equip::physics::guard
{
    namespace phys = wow::ec::physics;

    struct Config
    {
        bool  enabled       = true;    ///< the Judge safety net
        float reach         = 3.0f;    ///< a bone may stray this many times its distance to the nearest anchor ...
        float slack         = 0.15f;   ///< ... plus this much (model units)
        float teleportYards = 6.0f;    ///< a placement that moves farther than this in one frame is a teleport
        float graceSec      = 0.25f;   ///< simulated time over the limit before the instance counts as stuck
    };

    /**
     * @brief The model the glue should run. Returns `in` itself when nothing needs changing.
     * @param carryAnchors  apply fix 1 above
     * @param changed       receives how many anchors were given the head group
     */
    std::shared_ptr<phys::Model> PrepareModel(const std::shared_ptr<phys::Model>& in, bool carryAnchors, int* changed = nullptr);

    /// What Judge needs to know about one model: its dynamic bones and how far each may stray.
    struct Limits
    {
        std::vector<uint16_t> bones;   ///< bone of each checked dynamic body
        std::vector<float>    limit;   ///< allowed distance from the animated pose, model units
    };

    /**
     * @brief Builds the limits from the model and the bones' pivots (model space).
     *        Empty when the model has no anchor to measure from: then nothing is judged.
     */
    Limits Build(const phys::Model& m, const phys::V3* pivots, size_t boneCount, const Config& cfg);

    struct State
    {
        bool   havePos = false;
        double lastPos[3] = {};
        float  overSec = 0;
    };

    enum class Verdict { Ok, Teleport, Stuck };

    /**
     * @brief Judges one simulated frame.
     * @param placement  the instance's world position this frame (yards)
     * @param disp       per entry of lim.bones: |simulated bone - animated bone| in model units
     * @param dt         simulated seconds this frame
     * @param worst      optional: receives the largest disp / limit, and `worstBone` its bone
     */
    Verdict Judge(const Limits& lim, State& st, const double placement[3], const float* disp, float dt,
                  const Config& cfg, float* worst = nullptr, uint16_t* worstBone = nullptr);
}
