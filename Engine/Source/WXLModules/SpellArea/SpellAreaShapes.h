// WXL spell area indicator: the parts that need no engine. See SpellArea.md.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
//
// What shape a spell's effects cover (circle, cone, line, and where it is anchored), the override file that can
// replace or add shapes per spell, and the textures the shapes are drawn with. Plain C++17, so it is tested on its own.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wxl::spellarea
{
    enum class ShapeKind : uint8_t { Circle, Cone, Line };

    /// Where a shape is centered: on the caster, or on the cast's destination (the ground point that was targeted, or
    /// the target unit when the spell has no ground point).
    enum class Anchor : uint8_t { Caster, Dest };

    enum class Intent : uint8_t { Harmful, Helpful, Neutral };

    struct Shape
    {
        ShapeKind kind   = ShapeKind::Circle;
        Anchor    anchor = Anchor::Caster;
        float     radius = 0;   // circle radius, cone radius, line length (yards)
        float     param  = 0;   // cone angle in degrees, line width in yards
        Intent    intent = Intent::Neutral;
    };

    /// The part of a spell record this needs: its three effects, the target types of each (Spell.dbc
    /// ImplicitTargetA / B), and each effect's radius in yards (already worked out from SpellRadius.dbc and the
    /// caster's level; 0 = no radius).
    struct SpellEffects
    {
        int32_t effect[3]  = {};
        int32_t targetA[3] = {};
        int32_t targetB[3] = {};
        float   radius[3]  = {};
        int32_t chainTargets[3] = {};   // Spell.dbc EffectChainTargets: how many targets the effect reaches by jumping
        float   chainRadius = 0;        // the spell's jump distance from the server's table, yards; 0 = not known
        float   coneAngle = 0;          // the spell's cone angle from SpellCone.csv, degrees; 0 = not listed
    };

    struct ShapeRules
    {
        float coneAngle = 90.0f;   // degrees, for the cone target types (the DBC does not carry one)
        float minRadius = 1.0f;    // a radius under this is not an area worth drawing
        float maxRadius = 60.0f;   // a radius over this is drawn at this size
        bool  cones     = true;
        bool  chain     = true;     // chain spells get the circle of their first jump around the target
        float chainRadius = 12.5f;  // yards, for a chain effect that has no radius of its own
    };

    // ---- target type classes (3.3.5 Spell.dbc implicit target ids) ----
    bool IsConeTarget(int32_t target);          // 24, 54, 59, 60, 104
    bool IsCasterAreaTarget(int32_t target);    // areas around the caster: 7, 11, 15, 20, 30, 33, 56
    bool IsDestAreaTarget(int32_t target);      // areas around the destination: 8, 16, 28, 29, 31, 34, 87, 91
    bool IsEnemyTarget(int32_t target);
    bool IsAllyTarget(int32_t target);

    /// The areas a spell covers, from its effects: at most one per effect, equal ones merged. `out` has room for 3.
    /// A cone target type gives a cone (angle from rules), an area around the caster or the destination gives a
    /// circle, and so do the area aura effects (35, 65, 119, 128, 129, 143: around the caster) and the persistent area
    /// aura (27: around the destination). A chain effect (more than one chain target) that is not an area gives the circle
    /// of its first jump, around the target. A spell with a cone angle (SpellEffects::coneAngle, from SpellCone.csv) is a
    /// cone: its cone target types use that angle instead of the default, and its areas around the caster become cones
    /// (the server applies the cone to them; the DBC does not say so). 360 degrees or more is a full circle. Its radius, in order: the spell's jump distance from the server's table
    /// (SpellEffects::chainRadius), the effect's own radius, ShapeRules::chainRadius. Lines cannot be told from the
    /// DBC: they come from the override file.
    size_t ShapesFromSpell(const SpellEffects& spell, const ShapeRules& rules, Shape out[3]);

    // ---- override file ----
    /// SpellAreaOverride.csv: `SpellId,Shape,Anchor,Radius,Param,Intent` (comma or semicolon; `#` starts a comment; a
    /// header line is skipped). Shape: circle, cone, line, none. Anchor: caster, dest. Radius: circle radius, cone
    /// radius or line length. Param: cone angle (degrees) or line width (yards). Intent: harmful, helpful, neutral.
    /// Rows of one spell all apply; `none` hides the spell. A spell with rows uses them INSTEAD of what its effects say.
    class OverrideTable
    {
    public:
        /// Parses `text`; a bad line is skipped and described in `problems` (one line each).
        void Parse(const std::string& text, std::string& problems);
        /// true when the spell has rows. `none` is set when it is hidden; otherwise `shapes` holds its rows.
        bool Find(uint32_t spellId, bool& none, std::vector<Shape>& shapes) const;
        size_t Rows() const { return m_rows.size(); }

    private:
        struct Row { uint32_t spell; bool none; Shape shape; };
        std::vector<Row> m_rows;
    };

    // ---- jump distances ----
    /// The server's chain jump distances, from an export of its `spell_jump_distance` table (acore_world): a file with
    /// a header line, tab, comma or semicolon separated. The spell id column is the one called id / spell id / spell;
    /// the distance column is the first whose name holds "distance", else "radius", else "jump". Without a header the
    /// first column is the spell id and the second the distance. Quotes and backticks around cells are dropped. A
    /// spell listed more than once keeps its largest distance (one row per effect, say).
    class JumpDistanceTable
    {
    public:
        void Parse(const std::string& text, std::string& problems);
        /// true when the spell is listed; `yards` is its jump distance.
        bool Find(uint32_t spellId, float& yards) const;
        size_t Rows() const { return m_rows.size(); }

    private:
        struct Row { uint32_t spell; float yards; };
        std::vector<Row> m_rows;   // sorted by spell id
    };

    // ---- cone angles ----
    /// SpellCone.csv: one line per spell, no header: `spellId,degrees` (comma, semicolon or tab; `#` starts a comment; a
    /// first line that is not numbers is taken for a header and skipped). Degrees are above 0 and up to 360.
    class ConeAngleTable
    {
    public:
        void Parse(const std::string& text, std::string& problems);
        bool Find(uint32_t spellId, float& degrees) const;
        size_t Rows() const { return m_rows.size(); }

    private:
        struct Row { uint32_t spell; float degrees; };
        std::vector<Row> m_rows;   // sorted by spell id
    };

    /// "RRGGBB" or "#RRGGBB" to 0..1 floats; false when it is not six hex digits.
    bool ParseColorHex(const char* text, float rgb[3]);

    // ---- textures: ARGB8888 (0xAARRGGBB), white with the shape in the alpha channel, row 0 = the shape's front ----
    void RenderCircleTexture(std::vector<uint32_t>& argb, int size);
    /// a cone with its apex at the center of the texture, opening toward row 0, `angleDeg` wide (1 .. 359)
    void RenderConeTexture(std::vector<uint32_t>& argb, int size, float angleDeg);
    /// a strip from the center of the texture to row 0, `widthFraction` of the texture wide (0..1)
    void RenderLineTexture(std::vector<uint32_t>& argb, int size, float widthFraction);
}
