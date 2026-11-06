// Per-display multipliers: PhysicsOverride.csv.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// The multipliers of PhysicsTuning.hpp apply to every model at once. PhysicsOverride.csv sets some of them for
// ONE item display (or, later, one creature display), so two ItemDisplayInfo rows that share a model file can
// move differently: one with dangling bits, the other with gravity turned around.
//
// Nothing here touches the engine: Parse() turns the text of the file into a Table, which the module consults
// with the display id of an instance. Unit-testable (Tools/PhysicsGuardTest/physics_override_test.cpp).
//
//     Kind,DisplayId,GravityScale,SwingLimitScale,MassScale
//     Item,12345,-1,,
//     Item,12346,,1.5,0.5
//     Creature,6789,0.5,,
//
//   * The first line that is not empty and not a comment is the header. It must have Kind and DisplayId; after
//     those come any of the [Physics] multiplier names of PhysicsTuning.hpp (GravityScale, LinearDampingScale,
//     AngularDampingScale, InertiaScale, MassScale, FrictionScale, RestitutionScale, SwingLimitScale,
//     TwistLimitScale, JointStiffnessScale, JointDampingScale, MotorTorqueScale, FollowScale), in any order and
//     any subset. Names and Kind are not case sensitive. A column called Note, Notes, Comment, Comments, Name or
//     Description is ignored without a word; any other unknown column is ignored with one log line.
//   * Kind is Item or Creature. Item rows are applied to held items (main hand, off hand, ranged, shield).
//     Creature rows are read and checked but not applied yet.
//   * DisplayId is the id in ItemDisplayInfo (Item) or CreatureDisplayInfo (Creature).
//   * An empty cell means "not set here": the global value ([Physics] in WXLExtendedEquipment.ini) stays. A filled
//     cell REPLACES the global value for that display (it is not multiplied with it). Ranges are those of the
//     ini keys; GravityScale may be negative.
//   * Lines starting with # or // are comments. The separator is a comma; if the header line has semicolons
//     and no commas, it is a semicolon (what a spreadsheet in an Italian locale writes), and then a decimal
//     comma ("1,25") works as well. A cell may be put in double quotes.
//   * A row with a problem is skipped or trimmed and described in the log; the rest of the file still works.
//     A display listed twice: the later row wins.
#pragma once

#include "PhysicsTuning.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace wxl::equip::physics::overrides
{
    enum class Kind { Item, Creature };

    struct Table
    {
        std::unordered_map<uint32_t, tuning::Overrides> items;       // ItemDisplayInfo id -> its multipliers
        std::unordered_map<uint32_t, tuning::Overrides> creatures;   // CreatureDisplayInfo id -> its multipliers

        /// The row of a display, or nullptr when it has none.
        const tuning::Overrides* Find(Kind kind, uint32_t displayId) const;
        size_t Rows() const { return items.size() + creatures.size(); }
        bool Empty() const { return items.empty() && creatures.empty(); }
    };

    /// Parses the text of PhysicsOverride.csv. Problems (an unknown kind, a bad id, a value that is not a
    /// number, a value out of range, a duplicate row) are described in *problems, one per line, when given.
    Table Parse(const std::string& text, std::string* problems = nullptr);
}
