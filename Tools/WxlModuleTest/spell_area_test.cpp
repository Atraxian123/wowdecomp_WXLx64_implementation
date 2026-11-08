// Spell area indicator, the engine-free part: shape rules, override file, textures, and the projection orientation.
#include "Engine/Source/WXLModules/SpellArea/SpellAreaShapes.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace wxl::spellarea;

static int g_failed = 0;
static void Check(bool ok, const char* what) { if (!ok) { ++g_failed; std::printf("  FAIL: %s\n", what); } }
static bool Near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

static unsigned AlphaAt(const std::vector<uint32_t>& t, int size, float x, float y)   // x right, y up, texture units -0.5..0.5
{
    const int col = static_cast<int>((x + 0.5f) * size), row = static_cast<int>((0.5f - y) * size);
    return t[static_cast<size_t>(row) * size + col] >> 24;
}

int main()
{
    std::printf("1. target classes\n");
    Check(IsConeTarget(24) && IsConeTarget(104) && !IsConeTarget(15), "cone targets");
    Check(IsCasterAreaTarget(15) && IsCasterAreaTarget(33) && !IsCasterAreaTarget(16), "caster area targets");
    Check(IsDestAreaTarget(16) && IsDestAreaTarget(31) && !IsDestAreaTarget(15), "destination area targets");
    Check(IsEnemyTarget(15) && IsAllyTarget(30) && !IsAllyTarget(15), "enemy / ally");

    std::printf("2. shapes from a spell\n");
    ShapeRules rules;
    Shape out[3];
    {
        SpellEffects s;            // Arcane Explosion style: damage, caster + src area enemy, 10 yards
        s.effect[0] = 2; s.targetA[0] = 22; s.targetB[0] = 15; s.radius[0] = 10;
        Check(ShapesFromSpell(s, rules, out) == 1 && out[0].kind == ShapeKind::Circle && out[0].anchor == Anchor::Caster &&
              Near(out[0].radius, 10) && out[0].intent == Intent::Harmful, "area around the caster: harmful circle");
    }
    {
        SpellEffects s;            // Flamestrike style: dest area enemy
        s.effect[0] = 2; s.targetA[0] = 16; s.targetB[0] = 0; s.radius[0] = 8;
        Check(ShapesFromSpell(s, rules, out) == 1 && out[0].anchor == Anchor::Dest && out[0].intent == Intent::Harmful, "area around the destination");
    }
    {
        SpellEffects s;            // Cone of Cold style
        s.effect[0] = 2; s.targetA[0] = 24; s.radius[0] = 10;
        Check(ShapesFromSpell(s, rules, out) == 1 && out[0].kind == ShapeKind::Cone && Near(out[0].param, 90) && out[0].anchor == Anchor::Caster, "cone: 90 degrees by default");
        rules.coneAngle = 60.0f;
        ShapesFromSpell(s, rules, out);
        Check(Near(out[0].param, 60), "cone angle follows the rules");
        rules.cones = false;
        Check(ShapesFromSpell(s, rules, out) == 0, "cones can be switched off");
        rules = ShapeRules{};
    }
    {
        SpellEffects s;            // an area aura around the caster: effect id alone says it
        s.effect[0] = 35; s.targetA[0] = 1; s.radius[0] = 30;
        Check(ShapesFromSpell(s, rules, out) == 1 && out[0].kind == ShapeKind::Circle && out[0].anchor == Anchor::Caster && out[0].intent == Intent::Helpful, "area aura effect: helpful circle on the caster");
        SpellEffects p;            // persistent area aura: at the destination
        p.effect[0] = 27; p.targetA[0] = 87; p.radius[0] = 8;
        Check(ShapesFromSpell(p, rules, out) == 1 && out[0].anchor == Anchor::Dest, "persistent area aura: circle at the destination");
    }
    {
        SpellEffects s;            // a single target spell and a spell without radius draw nothing
        s.effect[0] = 2; s.targetA[0] = 6; s.radius[0] = 0;
        s.effect[1] = 2; s.targetA[1] = 15; s.radius[1] = 0.2f;
        Check(ShapesFromSpell(s, rules, out) == 0, "no area, or a radius under the minimum: nothing");
    }
    {
        SpellEffects s;            // two effects on the same area merge, harmful wins
        s.effect[0] = 6; s.targetA[0] = 22; s.targetB[0] = 33; s.radius[0] = 12;
        s.effect[1] = 2; s.targetA[1] = 22; s.targetB[1] = 15; s.radius[1] = 12;
        Check(ShapesFromSpell(s, rules, out) == 1 && out[0].intent == Intent::Harmful, "equal areas merge, harmful wins");
        s.radius[0] = s.radius[1] = 500;
        ShapesFromSpell(s, rules, out);
        Check(Near(out[0].radius, rules.maxRadius), "a huge radius is capped");
    }

    std::printf("2b. chain spells\n");
    {
        SpellEffects s;               // Chain Lightning style: a single enemy target, two chain targets, no radius
        s.effect[0] = 2; s.targetA[0] = 6; s.chainTargets[0] = 2;
        Check(ShapesFromSpell(s, rules, out) == 1 && out[0].kind == ShapeKind::Circle && out[0].anchor == Anchor::Dest &&
              Near(out[0].radius, 12.5f) && out[0].intent == Intent::Harmful, "chain: the circle of the first jump around the target, 12.5 yards");
        SpellEffects h;               // Chain Heal style
        h.effect[0] = 10; h.targetA[0] = 45; h.chainTargets[0] = 3;
        Check(ShapesFromSpell(h, rules, out) == 1 && out[0].intent == Intent::Helpful && out[0].anchor == Anchor::Dest, "chain heal: helpful");
        SpellEffects own;             // a chain effect that carries a radius uses it
        own.effect[0] = 2; own.targetA[0] = 6; own.chainTargets[0] = 4; own.radius[0] = 8;
        Check(ShapesFromSpell(own, rules, out) == 1 && Near(out[0].radius, 8), "chain: the spell's own radius wins");
        ShapeRules custom = rules; custom.chainRadius = 20.0f;
        Check(ShapesFromSpell(s, custom, out) == 1 && Near(out[0].radius, 20), "chain radius follows the rules");
        custom.chain = false;
        Check(ShapesFromSpell(s, custom, out) == 0, "chain can be switched off");
        SpellEffects one;             // one target is not a chain
        one.effect[0] = 2; one.targetA[0] = 6; one.chainTargets[0] = 1;
        Check(ShapesFromSpell(one, rules, out) == 0, "a single target spell is not a chain");
        SpellEffects both;            // an area effect that also lists chain targets stays one area circle
        both.effect[0] = 2; both.targetA[0] = 22; both.targetB[0] = 15; both.radius[0] = 10; both.chainTargets[0] = 3;
        Check(ShapesFromSpell(both, rules, out) == 1 && out[0].anchor == Anchor::Caster && Near(out[0].radius, 10), "an area is not turned into a chain circle");
        SpellEffects big;             // the cap holds for the chain radius too
        big.effect[0] = 2; big.targetA[0] = 6; big.chainTargets[0] = 2;
        ShapeRules huge = rules; huge.chainRadius = 500.0f;
        Check(ShapesFromSpell(big, huge, out) == 1 && Near(out[0].radius, huge.maxRadius), "chain radius is capped");
    }

    std::printf("2c. the server's jump distances\n");
    {
        SpellEffects s;
        s.effect[0] = 2; s.targetA[0] = 6; s.chainTargets[0] = 2; s.radius[0] = 8;
        s.chainRadius = 15.0f;
        Check(ShapesFromSpell(s, rules, out) == 1 && Near(out[0].radius, 15.0f), "the table's jump distance comes before the effect's radius");
        s.chainRadius = 0;
        Check(ShapesFromSpell(s, rules, out) == 1 && Near(out[0].radius, 8.0f), "without it the effect's radius");
        s.radius[0] = 0;
        Check(ShapesFromSpell(s, rules, out) == 1 && Near(out[0].radius, 12.5f), "without either the default");
        s.chainRadius = 70.0f;
        Check(ShapesFromSpell(s, rules, out) == 1 && Near(out[0].radius, rules.maxRadius), "a table value is capped too");
        SpellEffects direct;      // a table value does not make a non-chain spell a chain
        direct.effect[0] = 2; direct.targetA[0] = 6; direct.chainTargets[0] = 1; direct.chainRadius = 15.0f;
        Check(ShapesFromSpell(direct, rules, out) == 0, "the table does not turn a single target spell into a chain");

        float y = 0; std::string problems;
        JumpDistanceTable t;
        t.Parse("ID\tMaxJumpDistance\tComment\n421\t12.5\tchain lightning\n1064\t10\tchain heal\n421\t15\tsecond row\n", problems);
        Check(t.Rows() == 2 && t.Find(421, y) && Near(y, 15.0f) && t.Find(1064, y) && Near(y, 10.0f) && !t.Find(7, y) && problems.empty(), "mysql --batch style: header, tabs, duplicates keep the largest");
        JumpDistanceTable c;
        c.Parse("`spell_id`,`jump_distance`\r\n\"421\",\"12.5\"\r\n", problems);
        Check(c.Rows() == 1 && c.Find(421, y) && Near(y, 12.5f), "csv export with quotes and backticks, CRLF");
        JumpDistanceTable h;
        h.Parse("# a comment\n421;11,5\n", problems);
        Check(h.Rows() == 1 && h.Find(421, y) && Near(y, 11.5f), "no header, ';' separated, decimal comma: 11,5 is 11.5");
        JumpDistanceTable h2;
        h2.Parse("421,11.5\n1064,9\n", problems);
        Check(h2.Rows() == 2 && h2.Find(1064, y) && Near(y, 9.0f), "no header: spell id, distance");
        JumpDistanceTable r;
        r.Parse("Entry,Radius,Flags\n421,20,0\n", problems);
        Check(r.Rows() == 0 && problems.find("no spell id / distance column") != std::string::npos && problems.find("Entry") != std::string::npos, "unknown header: not used, the headers are listed");
        JumpDistanceTable e;
        problems.clear();
        e.Parse("ID,Distance\n421,0\nabc,5\n1064,7\n", problems);
        Check(e.Rows() == 1 && e.Find(1064, y) && problems.find("line 2") != std::string::npos && problems.find("line 3") != std::string::npos, "zero and non-numeric rows are reported and skipped");
        Check(!JumpDistanceTable{}.Find(421, y), "an empty table lists nothing");
    }

    std::printf("2d. SpellCone.csv\n");
    {
        SpellEffects s;               // a spell the DBC only knows as "area around the caster"
        s.effect[0] = 2; s.targetA[0] = 22; s.targetB[0] = 15; s.radius[0] = 10;
        Check(ShapesFromSpell(s, rules, out) == 1 && out[0].kind == ShapeKind::Circle, "not listed: a circle");
        s.coneAngle = 60.0f;
        Check(ShapesFromSpell(s, rules, out) == 1 && out[0].kind == ShapeKind::Cone && Near(out[0].param, 60) && Near(out[0].radius, 10) && out[0].anchor == Anchor::Caster, "listed: the area around the caster is a cone of the listed angle");
        SpellEffects c;               // a cone target type takes the listed angle over the default
        c.effect[0] = 2; c.targetA[0] = 24; c.radius[0] = 12; c.coneAngle = 45.0f;
        Check(ShapesFromSpell(c, rules, out) == 1 && Near(out[0].param, 45), "cone target type: the listed angle beats ConeAngle");
        c.coneAngle = 360.0f;
        Check(ShapesFromSpell(c, rules, out) == 1 && out[0].kind == ShapeKind::Circle, "360 degrees is a circle");
        SpellEffects d;               // an area around the destination is not turned into a cone
        d.effect[0] = 2; d.targetA[0] = 16; d.radius[0] = 8; d.coneAngle = 60.0f;
        Check(ShapesFromSpell(d, rules, out) == 1 && out[0].kind == ShapeKind::Circle && out[0].anchor == Anchor::Dest, "a destination area stays a circle");
        ShapeRules nocone = rules; nocone.cones = false;
        Check(ShapesFromSpell(s, nocone, out) == 0, "cones off: a listed spell draws nothing rather than the wrong shape");

        ConeAngleTable t;
        std::string problems;
        t.Parse("# spell id, degrees\n2643,60\n12345;90\n23456\t180\n2643,75\n", problems);
        float deg = 0;
        Check(t.Rows() == 3 && t.Find(2643, deg) && Near(deg, 75) && t.Find(12345, deg) && Near(deg, 90) && t.Find(23456, deg) && Near(deg, 180) && !t.Find(1, deg) && problems.empty(), "headless file: comma, semicolon, tab; the last line of a spell counts");
        ConeAngleTable h;
        h.Parse("SpellId,Degrees\r\n100,30\r\n", problems);
        Check(h.Rows() == 1 && h.Find(100, deg) && Near(deg, 30) && problems.empty(), "a header line is skipped quietly");
        ConeAngleTable b;
        problems.clear();
        b.Parse("100,0\n101,361\nabc,45\n102\n103,45,extra\n", problems);
        Check(b.Rows() == 1 && b.Find(103, deg) && problems.find("line 1") != std::string::npos && problems.find("line 2") != std::string::npos &&
              problems.find("line 3") != std::string::npos && problems.find("line 4") != std::string::npos, "bad rows (0, over 360, text, missing degrees) are reported and skipped");
    }

    std::printf("2e. custom attributes: cone back, cone line\n");
    {
        SpellEffects cone;            // a cone target type
        cone.effect[0] = 2; cone.targetA[0] = 24; cone.radius[0] = 12;
        Check(ShapesFromSpell(cone, rules, out) == 1 && out[0].kind == ShapeKind::Cone && !out[0].back, "no attributes: a cone in front");

        SpellEffects back = cone; back.customAttributes = kAttrConeBack;
        Check(ShapesFromSpell(back, rules, out) == 1 && out[0].kind == ShapeKind::Cone && out[0].back && Near(out[0].param, 90) && Near(out[0].radius, 12) && out[0].anchor == Anchor::Caster,
              "CONE_BACK: the cone is behind the caster");
        back.coneAngle = 60.0f;
        Check(ShapesFromSpell(back, rules, out) == 1 && out[0].back && Near(out[0].param, 60), "CONE_BACK with a listed angle: behind, 60 degrees");

        SpellEffects line = cone; line.customAttributes = kAttrConeLine;
        Check(ShapesFromSpell(line, rules, out) == 1 && out[0].kind == ShapeKind::Line && !out[0].back && Near(out[0].radius, 12) && Near(out[0].param, 4.0f) && out[0].anchor == Anchor::Caster,
              "CONE_LINE: a line as long as the radius, 4 yards wide");
        line.coneAngle = 60.0f;
        Check(ShapesFromSpell(line, rules, out) == 1 && out[0].kind == ShapeKind::Line, "CONE_LINE ignores a listed angle");
        ShapeRules wide = rules; wide.lineWidth = 6.5f;
        Check(ShapesFromSpell(line, wide, out) == 1 && Near(out[0].param, 6.5f), "the line width follows the rules");

        SpellEffects both = cone; both.customAttributes = kAttrConeBack | kAttrConeLine;
        Check(ShapesFromSpell(both, rules, out) == 1 && out[0].kind == ShapeKind::Line && out[0].back, "both: a line behind the caster");

        SpellEffects area;            // the DBC only says "area around the caster"; the attribute is what makes it a cone
        area.effect[0] = 2; area.targetA[0] = 22; area.targetB[0] = 15; area.radius[0] = 10;
        area.customAttributes = kAttrConeBack;
        Check(ShapesFromSpell(area, rules, out) == 1 && out[0].kind == ShapeKind::Cone && out[0].back && out[0].intent == Intent::Harmful, "an area around the caster with CONE_BACK: a cone behind");
        area.customAttributes = kAttrConeLine;
        Check(ShapesFromSpell(area, rules, out) == 1 && out[0].kind == ShapeKind::Line, "an area around the caster with CONE_LINE: a line");

        SpellEffects dest;            // an area around a destination is not a cone, whatever the attributes say
        dest.effect[0] = 2; dest.targetA[0] = 16; dest.radius[0] = 8; dest.customAttributes = kAttrConeBack | kAttrConeLine;
        Check(ShapesFromSpell(dest, rules, out) == 1 && out[0].kind == ShapeKind::Circle && out[0].anchor == Anchor::Dest && !out[0].back, "a destination area stays a circle");

        SpellEffects other = cone; other.customAttributes = 0x8 | 0x40 | 0x80000000u;   // other custom attributes change nothing
        Check(ShapesFromSpell(other, rules, out) == 1 && out[0].kind == ShapeKind::Cone && !out[0].back, "unrelated attribute bits are ignored");

        ShapeRules nolines = rules; nolines.lines = false;
        Check(ShapesFromSpell(line, nolines, out) == 0, "Lines off: a line spell draws nothing");
        Check(ShapesFromSpell(back, nolines, out) == 1, "Lines off leaves cones alone");
        ShapeRules nocones = rules; nocones.cones = false;
        Check(ShapesFromSpell(back, nocones, out) == 0, "Cones off: a back cone draws nothing");
        Check(ShapesFromSpell(line, nocones, out) == 1 && out[0].kind == ShapeKind::Line, "Cones off leaves lines alone");

        SpellEffects full = cone; full.customAttributes = kAttrConeBack; full.coneAngle = 360.0f;
        Check(ShapesFromSpell(full, rules, out) == 1 && out[0].kind == ShapeKind::Circle && !out[0].back, "a 360 degree cone is a circle, front or back");

        CustomAttributeTable t;
        std::string problems;
        t.Parse("#spell_id,attributes\r\n12323,0\r\n26029,4\r\n24340,8\r\n4294967296,1\r\n99,2\r\n100,6\r\n101;0x2\r\n102\t2147483648\r\n26029,4\r\n", problems);
        uint32_t m = 0;
        Check(t.Find(26029, m) && m == 4 && t.Find(24340, m) && m == 8 && t.Find(12323, m) && m == 0 && !t.Find(1, m), "table: decimal masks, comments, the usual header line");
        Check(t.Find(101, m) && m == 2 && t.Find(102, m) && m == 0x80000000u, "table: hex mask, semicolon and tab, a mask above 2^31");
        Check(problems.find("line 5") != std::string::npos, "table: a spell id above 32 bits is reported");
        Check(t.CountWith(kAttrConeLine) == 2 && t.CountWith(kAttrConeBack) == 3 && t.CountWith(kAttrConeBack | kAttrConeLine) == 1, "table: counts by bit (26029 and 100 are lines; 99, 100 and 101 are back)");
        CustomAttributeTable bad;
        problems.clear();
        bad.Parse("spell_id,attributes\n7,notanumber\n8\n9,4294967296\nabc,4\n10,4\n", problems);
        Check(bad.Rows() == 1 && bad.Find(10, m) && m == 4 && problems.find("line 2") != std::string::npos && problems.find("line 3") != std::string::npos &&
              problems.find("line 4") != std::string::npos && problems.find("line 5") != std::string::npos, "table: bad rows are reported and skipped, a plain header line is skipped quietly");
    }

    std::printf("3. override file\n");
    {
        OverrideTable t;
        std::string problems;
        t.Parse("# comment\r\n"
                "SpellId,Shape,Anchor,Radius,Param,Intent\r\n"
                "1001,line,,30,6,harmful\r\n"
                "1001,circle,dest,5\r\n"
                "1002,none\r\n"
                "1003;cone;caster;12;45;helpful\r\n"
                "abc,circle,caster,5\r\n"
                "1004,square,caster,5\r\n"
                "1005,circle,caster,0\r\n", problems);
        bool none = false; std::vector<Shape> shapes;
        Check(t.Find(1001, none, shapes) && !none && shapes.size() == 2, "two rows of one spell");
        Check(shapes[0].kind == ShapeKind::Line && Near(shapes[0].radius, 30) && Near(shapes[0].param, 6) && shapes[0].intent == Intent::Harmful, "line row");
        Check(shapes[1].kind == ShapeKind::Circle && shapes[1].anchor == Anchor::Dest && Near(shapes[1].radius, 5), "circle row with a destination anchor");
        Check(t.Find(1002, none, shapes) && none && shapes.empty(), "none hides the spell");
        Check(t.Find(1003, none, shapes) && shapes.size() == 1 && shapes[0].kind == ShapeKind::Cone && Near(shapes[0].param, 45) && shapes[0].intent == Intent::Helpful, "';' separated cone row");
        Check(!t.Find(9999, none, shapes), "a spell without rows");
        Check(problems.find("abc") != std::string::npos && problems.find("square") != std::string::npos && problems.find("line 8") != std::string::npos, "bad lines are reported");
        Check(problems.find("SpellId") == std::string::npos, "the header line is not a problem");
        Check(!t.Find(1004, none, shapes) && !t.Find(1005, none, shapes), "bad rows are skipped");
    }
    {
        float c[3];
        Check(ParseColorHex("FF8000", c) && Near(c[0], 1) && Near(c[1], 0.502f) && Near(c[2], 0), "color");
        Check(ParseColorHex("#00ff7f", c) && Near(c[1], 1), "color with #");
        Check(!ParseColorHex("12345", c) && !ParseColorHex("GG0000", c) && !ParseColorHex(nullptr, c), "bad colors");
    }

    std::printf("4. textures\n");
    {
        const int N = 256;
        std::vector<uint32_t> t;
        RenderCircleTexture(t, N);
        Check(AlphaAt(t, N, 0, 0) > 40 && AlphaAt(t, N, 0, 0) < 90, "circle: translucent fill in the middle");
        Check(AlphaAt(t, N, 0.485f, 0) > 220 && AlphaAt(t, N, 0, -0.485f) > 220, "circle: bright rim");
        Check(AlphaAt(t, N, 0.45f, 0.45f) == 0 && AlphaAt(t, N, 0.499f, 0) < 100, "circle: clear outside, the outline does not run into the texture border");
        Check((t[0] & 0x00FFFFFF) == 0x00FFFFFF, "white color channels");

        // the outline's thickness: a fraction of the texture's width, 0 for the default
        std::vector<uint32_t> thick, thin, same;
        RenderCircleTexture(thick, N, 0.10f);
        RenderCircleTexture(thin, N, 0.008f);
        RenderCircleTexture(same, N, 0.025f);
        Check(AlphaAt(t, N, 0.45f, 0) < 90, "outline: the default is thin: fill only 0.035 inside the edge");
        Check(AlphaAt(thick, N, 0.45f, 0) > 220 && AlphaAt(thick, N, 0, -0.45f) > 220, "outline: 0.10 reaches 0.035 inside the edge");
        Check(AlphaAt(thick, N, 0, 0) > 40 && AlphaAt(thick, N, 0, 0) < 90, "outline: the fill in the middle does not change");
        Check(AlphaAt(thin, N, 0.485f, 0) > 150 && AlphaAt(thin, N, 0.47f, 0) < 90, "outline: 0.008 is a thin line on the edge");
        Check(AlphaAt(same, N, 0.485f, 0) == AlphaAt(t, N, 0.485f, 0) && AlphaAt(same, N, 0.47f, 0) == AlphaAt(t, N, 0.47f, 0), "outline: 0.025 is the default");
        RenderConeTexture(thick, N, 90, 0.10f);
        Check(AlphaAt(thick, N, 0, 0.485f) > 220 && AlphaAt(thick, N, 0, 0.44f) > 220, "outline: the cone's far edge gets thick as well");

        RenderConeTexture(t, N, 90);
        Check(AlphaAt(t, N, 0, 0.30f) > 40 && AlphaAt(t, N, 0, 0.30f) < 90, "cone: fill straight ahead");
        Check(AlphaAt(t, N, 0, -0.30f) == 0, "cone: nothing behind the apex");
        const float r = 0.3f, a60 = 60.0f * 3.14159265f / 180.0f;
        Check(AlphaAt(t, N, r * std::sin(a60), r * std::cos(a60)) == 0 && AlphaAt(t, N, -r * std::sin(a60), r * std::cos(a60)) == 0, "cone: nothing outside its 90 degrees");
        const float a30 = 30.0f * 3.14159265f / 180.0f;
        Check(AlphaAt(t, N, r * std::sin(a30), r * std::cos(a30)) > 40 && AlphaAt(t, N, -r * std::sin(a30), r * std::cos(a30)) > 40, "cone: fill inside, both sides alike");
        const float a45 = 45.0f * 3.14159265f / 180.0f;
        Check(AlphaAt(t, N, r * std::sin(a45), r * std::cos(a45)) > 200, "cone: bright side edge");
        Check(AlphaAt(t, N, 0, 0.485f) > 200, "cone: bright arc");
        RenderConeTexture(t, N, 180);
        Check(AlphaAt(t, N, 0.3f, 0.02f) > 40 && AlphaAt(t, N, 0.3f, -0.05f) == 0, "cone of 180 degrees is the front half");
        RenderConeTexture(t, N, 270);
        Check(AlphaAt(t, N, 0.3f, 0.0f) > 40 && AlphaAt(t, N, -0.3f, 0.0f) > 40, "cone of 270 degrees: both sides are in");
        Check(AlphaAt(t, N, 0.0f, -0.3f) == 0, "cone of 270 degrees: the wedge straight behind is out");
        Check(AlphaAt(t, N, 0.3f * std::sin(0.5f), -0.3f * std::cos(0.5f)) == 0 && AlphaAt(t, N, 0.3f * std::sin(1.2f), -0.3f * std::cos(1.2f)) > 40, "cone of 270 degrees: the wedge is 90 wide (45 each side of the back)");
        Check(AlphaAt(t, N, 0.3f * std::sin(0.785f), -0.3f * std::cos(0.785f)) > 200, "cone of 270 degrees: bright edge where the wedge starts");

        RenderLineTexture(t, N, 0.2f);
        Check(AlphaAt(t, N, 0, 0.25f) > 40 && AlphaAt(t, N, 0, 0.25f) < 90, "line: fill along the middle");
        Check(AlphaAt(t, N, 0, -0.25f) == 0 && AlphaAt(t, N, 0.25f, 0.25f) == 0, "line: nothing behind or beside");
        Check(AlphaAt(t, N, 0.1f, 0.25f) > 180 && AlphaAt(t, N, 0, 0.485f) > 200, "line: bright sides and end");
    }

    std::printf("5. projection orientation (the client's own texture matrix, evaluated here)\n");
    {
        // ProjectTex2dMakeMatrices: translation(-center) * scale(1/size) * spin(-90 deg about z) * rotation, then +0.5;
        // row vectors, v' = v * M, RotationAroundZ(a): x' = x cos a - y sin a, y' = x sin a + y cos a.
        auto rot = [](float a, float x, float y, float& ox, float& oy) { ox = x * std::cos(a) - y * std::sin(a); oy = x * std::sin(a) + y * std::cos(a); };
        const float R = 20.0f, size = 2.0f * R;
        for (float facing : { 0.0f, 0.5f, 1.5707964f, 3.5f, 5.9f })
        {
            for (float along : { 0.6f * R, -0.6f * R })
            {
                const float dx = std::cos(facing) * along, dy = std::sin(facing) * along;   // a world point `along` yards in front of / behind the caster
                float sx, sy;
                rot(-1.5707964f, dx / size, dy / size, sx, sy);   // scale, then spin
                float u, v;
                rot(-facing, sx, sy, u, v);                       // the rotation matrix the client passes: RotateAroundZ(-facing)
                u += 0.5f; v += 0.5f;
                const float expectV = along > 0 ? 0.5f - 0.3f : 0.5f + 0.3f;
                Check(Near(u, 0.5f, 0.001f) && Near(v, expectV, 0.001f), "a point in front of the caster lands toward row 0 (the texture's front)");
            }
        }
    }

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
