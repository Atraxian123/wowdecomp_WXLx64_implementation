// Per-display physics parameters: PhysicsOverride.csv.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// The [Physics] keys of WXLExtendedEquipment.ini (PhysicsTuning.hpp) tune every model at once.
// PhysicsOverride.csv gives the same set of parameters for ONE item display or ONE creature display, and they
// REPLACE the ini's values for it (nothing is multiplied with the ini), so two ItemDisplayInfo (or
// CreatureDisplayInfo) rows that share a model file can move differently: one with dangling bits, the other
// with gravity turned around.
//
// Nothing here touches the engine: Parse() turns the text of the file into a Table, which the module consults
// with the display of an instance. Unit-testable (Tools/PhysicsGuardTest/physics_override_test.cpp).
//
//     Kind,DisplayId,GravityScale,LinearDampingScale,AngularDampingScale,InertiaScale,MassScale,FrictionScale,RestitutionScale,SwingLimitScale,TwistLimitScale,JointStiffnessScale,JointDampingScale,MotorTorqueScale,FollowScale
//     Item,12345,-1,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA
//     Creature,6789,0.5,Default,Default,Default,Default,100,0,Default,Default,Default,Default,Default,2
//
//   * The first line that is not empty and not a comment is the header. It must have Kind, DisplayId and the
//     WHOLE set of parameters, named exactly like the ini keys: GravityScale, LinearDampingScale,
//     AngularDampingScale, InertiaScale, MassScale, FrictionScale, RestitutionScale, SwingLimitScale,
//     TwistLimitScale, JointStiffnessScale, JointDampingScale, MotorTorqueScale, FollowScale. Any order; names
//     and Kind are not case sensitive. A header that lacks one is refused, naming what is missing, and nothing
//     is read. A column called Note, Notes, Comment, Comments, Name or Description is ignored without a word;
//     any other unknown column is ignored with one log line.
//   * Kind is Item or Creature. Item rows are applied to held items (main hand, off hand, ranged, shield).
//     Creature rows are applied to the model of a unit (a creature, a pet, a shapeshifted player) and to the
//     model of a mount. The two id spaces never meet: Item 1234 and Creature 1234 are different rows.
//   * DisplayId is the id in ItemDisplayInfo (Item) or CreatureDisplayInfo (Creature).
//   * A number in a parameter column is the value for that display. It replaces the ini's value (the ini key
//     as it is, or 1.0 when the ini has none); like the ini keys it is a factor of what the model's .phy2 says,
//     so 1 is "as the file has it". A cell that is empty, or says NA, N/A, #N/A, Default, Ini or a lone dash,
//     uses the ini's value. Ranges are those of the ini keys; GravityScale may be negative.
//   * Lines starting with # or // are comments. The separator is a comma; if the header line has semicolons
//     and no commas, it is a semicolon (what a spreadsheet in an Italian locale writes), and then a decimal
//     comma ("1,25") works as well. A cell may be put in double quotes.
//   * A row with a problem is skipped or trimmed and described in the log; the rest of the file still works.
//     A row whose parameters are all empty/NA/Default has nothing to override and is skipped. A display listed
//     twice: the later row wins.
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
