// Runtime physics for model instances: the WXL "physics" module.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// Ported from wxl-equip-plus (PhysicsLive.cpp). The simulation is the retail "Domino" engine port in
// physics_domino.{h,cpp}; this file feeds the client's bone matrices into it and writes the simulated bones
// back. It is a DLL module (wxl_physics.dll, WXL\Modules\) and sees the client only through
// Engine/Source/WXL/Sdk/wxl_api.h.
//
// Where the physics comes from: NOT one table for every model. Each model's physics sits in a .phy2 file
// beside the model, under the same name and in the same folder:
//
//     item\objectcomponents\weapon\staff_2h_raidmidnight_d_02.m2
//     item\objectcomponents\weapon\staff_2h_raidmidnight_d_02.phy2
//
// A model reached through a virtual path (VirtualPath.hpp) uses the .phy2 of the ORIGINAL .m2 the virtual
// path was made from. A model with no .phy2 is left alone. The .phy2 files come from
// Tools/m2_extract_phys.py (retail .m2 -> .phys -> .phy2). Phy2Library.hpp has the details.
//
// Switched on by WXLExtendedEquipment.ini:   [Physics]  Enabled=1
//
// Stuck items (PhysicsGuard.hpp). Two more keys, both on by default:
//     CarryAnchors=1       a model with several kinematic anchors and no root body: every anchor carries
//                          the dynamic bodies hanging from it (retail carries only the first one, so the
//                          others trail behind during a jump or a flight of stairs)
//     Unstick=1            an instance whose bones stay far from where the animation puts them, or whose
//                          placement jumps (teleport), is dropped and re-seeded from the animated pose
//     UnstickReach=300     how far a bone may stray, in percent of its distance to the nearest anchor
//     TeleportYards=6      a placement move farther than this in one frame is a teleport
// and the multipliers of PhysicsTuning.hpp.
//
// The hook: an OnBuildBonePalette subscriber (the engine fires it at the end of CM2Model::AnimateMT).
// Modules load after the built-in extensions, so this subscriber runs after the equip extension's own and
// sees the palette after its remap/mirror. The event can fire on the client's animate threads: everything
// here is thread-safe.
#pragma once

#include "wxl_api.h"

#include <cstdint>

namespace wxl::equip::physics
{
    /// Reads the ini and subscribes to OnBuildBonePalette. Safe to call twice.
    bool Init(const WxlApi* api);

    bool Enabled();
    /// Switches physics on or off at run time; instances go back to their animated pose.
    void SetEnabled(bool on);
    /// Forgets every cached .phy2 and every simulation, so edited files are read again.
    void ReloadFiles();

    struct Stats
    {
        uint32_t modelsSeen = 0;      // distinct models looked up
        uint32_t modelsWithPhysics = 0;
        uint32_t live = 0;            // model instances being simulated now
        uint32_t steps = 0;           // simulation steps run (all instances)
        uint32_t faults = 0;          // reads/writes that hit bad memory (instance skipped)
        uint32_t dropped = 0;         // instances forgotten after 2 s unseen
        uint32_t unstuck = 0;         // instances re-seeded because a bone stayed far from its animated pose
        uint32_t teleports = 0;       // instances re-seeded because the placement jumped
        uint32_t disabled = 0;        // instances switched off for a while after re-seeding too often
        uint32_t modelsCarried = 0;   // models whose extra anchors were given a carry group
        uint32_t modelsTuned = 0;     // models that [Physics] *Scale multipliers changed (PhysicsTuning.hpp)
        double   lastFrameMs = 0;     // time spent in physics for the last simulated frame
        double   worstFrameMs = 0;
    };
    Stats GetStats();

    /// The hook body (public so tests can drive it): `instance` is the handle the event carried.
    void AfterBonePalette(void* instance);
}
