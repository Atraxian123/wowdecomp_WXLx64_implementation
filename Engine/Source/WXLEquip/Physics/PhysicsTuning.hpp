// Global multipliers for the physics of every model, from WXLExtendedEquipment.ini.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// A .phy2 holds one set of numbers per model (gravity, damping, joint limits, ...). These multipliers scale
// those numbers for every model at once, so the movement can be tuned (looser, heavier, floatier, stiffer)
// without editing the .phy2 files. 1.0 leaves a value as the file has it; a key that is missing or not a
// number is 1.0. Nothing here touches the engine: Apply() returns a scaled copy of a Model, and the engine
// reads that copy as it would read the original.
//
//     [Physics]
//     Enabled=1
//
//     ; ---- how the bones fall and settle (dynamic bodies) ----
//     GravityScale=1.0          ; how hard they are pulled down. 0 = they float where they are.
//     LinearDampingScale=1.0    ; air drag on movement. Up: slower, gooier. Down: swings longer.
//     AngularDampingScale=1.0   ; air drag on spinning/twisting. Up: stops turning sooner.
//     InertiaScale=1.0          ; resistance to being rotated. Up: lazier turns. (0.01 .. 100)
//     MassScale=1.0             ; weight of the parts (shape density). (0.01 .. 100)
//
//     ; ---- contacts (every shape) ----
//     FrictionScale=1.0         ; grip against the collider. Up: sticks, down: slides.
//     RestitutionScale=1.0      ; bounciness (the result stays within 0 .. 1)
//
//     ; ---- joint limits (how far a bone may bend) ----
//     SwingLimitScale=1.0       ; shoulder-joint cone: the result stays within 10 .. 170 degrees
//     TwistLimitScale=1.0       ; shoulder twist range and revolute range, scaled about their centre. 0 = tight
//                               ; (about half a degree), never "no limit"
//
//     ; ---- joint springs (only joints whose file value has a motor torque; most have none) ----
//     JointStiffnessScale=1.0   ; spring frequency (Hz) of shoulder, revolute and weld joints
//     JointDampingScale=1.0     ; spring damping
//     MotorTorqueScale=1.0      ; strongest push the motor may make
//
//     ; ---- following a fast move ----
//     FollowScale=1.0           ; how much of a fast move of the item the anchors take at once (carry, 0 .. 1)
//
//     ; Re-read these keys whenever the ini is saved, without restarting the client (the parts snap back to
//     ; their pose for a moment). Default 0.
//     WatchIni=0
//
// Every multiplier is read as a decimal number, "1.25" or "1,25"; a value outside its range is clamped to it
// (and logged), a value that is not a number is ignored (and logged). Ranges are 0 .. 100 unless listed above.
#pragma once

#include "Engine/Source/WXLEquip/Physics/physics_domino.h"

#include <cstddef>
#include <memory>
#include <string>

namespace wxl::equip::physics::tuning
{
    namespace phys = wow::ec::physics;

    struct Tuning
    {
        float gravity = 1, linearDamping = 1, angularDamping = 1, inertia = 1, mass = 1;
        float friction = 1, restitution = 1;
        float swingLimit = 1, twistLimit = 1;
        float jointStiffness = 1, jointDamping = 1, motorTorque = 1;
        float follow = 1;

        /// True when every multiplier is exactly 1 (Apply then returns its input untouched).
        bool Identity() const;
    };

    /// Reads one key of [Physics]: copies its text into out and returns true when the key exists.
    using ReadFn = bool (*)(const char* key, char* out, size_t cap);

    /// Parses "1.25" / "1,25" (surrounding spaces ignored). False for anything that is not a finite,
    /// non-negative number; *out is untouched then.
    bool ParseMultiplier(const char* text, float* out);

    /// Reads every multiplier through `read`. Problems (a value that is not a number, or out of range) are
    /// described in *problems, one per line, when given. Missing keys stay at 1.
    Tuning FromIni(ReadFn read, std::string* problems = nullptr);

    /// A scaled copy of `in`; `in` itself when the tuning is the identity or `in` is null. *changed (when
    /// given) counts the values that ended up different. Only what the .phy2 already uses is touched:
    /// gravity, damping, inertia on dynamic bodies; density on their shapes; friction and restitution on all
    /// shapes; limits and springs on shoulder, revolute and weld joints; carry on kinematic bodies.
    std::shared_ptr<phys::Model> Apply(const std::shared_ptr<phys::Model>& in, const Tuning& t, int* changed = nullptr);

    /// "GravityScale=0.5 SwingLimitScale=1.2 ..." for every multiplier that is not 1 ("none" otherwise).
    std::string Describe(const Tuning& t);
}
