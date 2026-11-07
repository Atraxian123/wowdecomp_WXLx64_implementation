// Spell area indicator, the engine-free part: shape rules, override file, textures, and the projection orientation.
#include "Engine/Source/WXL/SpellAreaShapes.h"

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
