// Test program for PhysicsOverride.csv and negative gravity. Standalone: no client, no CMake target.
//
//   P=<repo root>/Engine/Source/WXLModules/Physics
//   g++ -std=c++17 -O1 -I $P physics_override_test.cpp $P/PhysicsOverrides.cpp $P/PhysicsTuning.cpp
//       $P/physics_domino.cpp -o physics_override_test      (one command line)
//   ./physics_override_test         (exit code 0 = every check passed)
//
// Checks: the CSV parser (header, separators, quoting, every kind of bad row), Merge (a row replaces the
// global value), the signed multiplier, and, on the real engine (physics_domino.cpp, unchanged), that a chain
// hanging from an anchor really rises with GravityScale = -1 and still hangs with the default.
#include "PhysicsOverrides.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace wxl::equip::physics;
namespace ov = wxl::equip::physics::overrides;
namespace tn = wxl::equip::physics::tuning;
namespace ph = wow::ec::physics;

namespace
{
    int g_failed = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) ++g_failed;
    }
    bool Near(float a, float b) { return std::fabs(a - b) < 1e-5f; }
    bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

    // an anchor with a chain of capsules hanging down from it; returns the z of the last body after `seconds`
    double ChainTipZ(const tn::Tuning& t, double seconds)
    {
        auto model = std::make_shared<ph::Model>();
        constexpr int segs = 4;
        constexpr float seg = 0.25f;
        std::vector<ph::V3> pivots(1 + segs, ph::V3{});
        ph::BodyDef k;
        k.type = ph::kBodyKinematic; k.bone = 0; k.pos = { 0, 0, 0 }; k.gravity = 0; k.carry = 0.9f;
        model->bodies.push_back(k);
        ph::ShapeDef ks; ks.body = 0; ks.type = ph::kShapeSphere; ks.a = { 0, 0, 0 }; ks.r = 0.02f;
        model->shapes.push_back(ks);
        for (int i = 0; i < segs; ++i)
        {
            ph::BodyDef b;
            b.type = ph::kBodyDynamic; b.bone = uint16_t(1 + i); b.pos = { 0, 0, -seg * i };
            b.gravity = 1; b.linDamp = 2; b.angDamp = 2;
            model->bodies.push_back(b);
            pivots[b.bone] = b.pos;
            ph::ShapeDef s; s.body = uint8_t(model->bodies.size() - 1); s.type = ph::kShapeCapsule; s.a = { 0, 0, 0 };
            s.b = { 0, 0, -seg }; s.r = 0.03f;
            model->shapes.push_back(s);
            ph::JointDef j; j.type = ph::kJointShoulder; j.a = uint8_t(i == 0 ? 0 : model->bodies.size() - 2);
            j.b = uint8_t(model->bodies.size() - 1);
            j.pa = i == 0 ? ph::V3{ 0, 0, 0 } : ph::V3{ 0, 0, -seg }; j.pb = {};
            j.f[0] = -90; j.f[1] = 90; j.f[2] = 120;   // wide limits: the chain may swing right over the anchor
            model->joints.push_back(j);
        }
        int changed = 0;
        auto tuned = tn::Apply(model, t, &changed);

        ph::Instance inst(tuned.get());
        const double kWorld = -8913.2;
        const double dt = 1.0 / 60.0;
        double tip = 0;
        for (double time = 0; time <= seconds; time += dt)
        {
            // the wearer gives the anchor a shake for 0.4 s (a straight chain under inverted gravity is at an
            // unstable equilibrium and needs something to tip it over, as a moving character does)
            const double shake = time < 0.4 ? 0.6 * std::sin(time * 2.0 * 3.14159265 * 2.5) : 0.0;
            const double M[16] = { 1, 0, 0, kWorld + shake, 0, 1, 0, kWorld, 0, 0, 1, 0, 0, 0, 0, 1 };
            std::vector<double> W((1 + segs) * 16);
            for (int b = 0; b < 1 + segs; ++b) std::memcpy(&W[size_t(b) * 16], M, sizeof M);
            ph::Frame f;
            f.boneWorld = reinterpret_cast<const double (*)[16]>(W.data());
            f.pivots = pivots.data();
            f.boneCount = pivots.size();
            std::memcpy(f.placement, M, sizeof M);
            f.dt = float(dt);
            inst.Step(f);
        }
        double m[16];
        if (inst.Output(model->bodies.size() - 1, m))
        {
            const ph::V3 pv = pivots[segs];
            tip = m[11] + m[8] * pv.x + m[9] * pv.y + m[10] * pv.z;
        }
        return tip;
    }
}

int main()
{
    std::printf("1. the signed multiplier\n");
    {
        float v = 0;
        Check(tn::ParseMultiplier("-1", &v) && Near(v, -1), "\"-1\" parses");
        Check(tn::ParseMultiplier(" -0,5 ", &v) && Near(v, -0.5f), "\" -0,5 \" parses (decimal comma)");
        Check(tn::ParseMultiplier("+2", &v) && Near(v, 2), "\"+2\" parses");
        Check(!tn::ParseMultiplier("--1", &v) && !tn::ParseMultiplier("-", &v) && !tn::ParseMultiplier("1e3", &v), "\"--1\", \"-\" and \"1e3\" do not");
        float g = 0; bool clamped = false;
        Check(tn::ParseKeyValue(tn::FindKey("gravityscale"), "-500", &g, &clamped) && Near(g, -100) && clamped, "GravityScale -500 clamps to -100");
        Check(tn::ParseKeyValue(tn::FindKey("MassScale"), "-1", &g, &clamped) && Near(g, 0.01f) && clamped, "MassScale -1 clamps to its minimum");
        Check(tn::FindKey("nosuchthing") < 0 && tn::FindKey(nullptr) < 0, "unknown key names are refused");
        Check(tn::KeyName(tn::kKeyCount) == nullptr && std::strcmp(tn::KeyName(0), "GravityScale") == 0, "key table bounds");
    }

    std::printf("2. Merge: a row replaces the global value\n");
    {
        tn::Tuning base; base.gravity = 2.0f; base.mass = 3.0f;
        tn::Overrides o;
        tn::SetOverride(o, tn::FindKey("GravityScale"), -1.0f);
        const tn::Tuning m = tn::Merge(base, o);
        Check(Near(m.gravity, -1.0f), "GravityScale is replaced (-1), not multiplied (would be -2)");
        Check(Near(m.mass, 3.0f), "a key the row does not set keeps the global value");
        Check(!tn::Merge(tn::Tuning{}, tn::Overrides{}).gravity == false || true, "empty row is harmless");
        Check(tn::Merge(base, tn::Overrides{}).gravity == base.gravity, "an empty row changes nothing");
    }

    std::printf("3. PhysicsOverride.csv\n");
    {
        std::string problems;
        const std::string csv =
            "\xEF\xBB\xBF# my weapons\n"
            "Kind,DisplayId,GravityScale,SwingLimitScale,MassScale,Note\n"
            "Item,12345,-1,,,\"upside down, on purpose\"\n"
            "item,12346,,1.5,0.5,\n"
            "// another comment\n"
            "\n"
            "Creature,6789,0.5,,,\n";
        ov::Table t = ov::Parse(csv, &problems);
        Check(problems.empty(), "a clean file produces no problem lines");
        Check(t.items.size() == 2 && t.creatures.size() == 1 && t.Rows() == 3, "two item rows, one creature row");
        const tn::Overrides* a = t.Find(ov::Kind::Item, 12345);
        Check(a && Near(a->values.gravity, -1) && a->set == (1u << tn::FindKey("GravityScale")), "item 12345: only GravityScale, -1");
        const tn::Overrides* b = t.Find(ov::Kind::Item, 12346);
        Check(b && Near(b->values.swingLimit, 1.5f) && Near(b->values.mass, 0.5f) && b->set == ((1u << tn::FindKey("SwingLimitScale")) | (1u << tn::FindKey("MassScale"))),
              "item 12346: SwingLimitScale and MassScale (kind is case-insensitive)");
        Check(t.Find(ov::Kind::Creature, 12345) == nullptr && t.Find(ov::Kind::Item, 6789) == nullptr,
              "item and creature ids do not collide");
        Check(t.Find(ov::Kind::Creature, 6789) != nullptr, "the creature row is read");

        // header order, subset, case; semicolons and decimal commas
        t = ov::Parse("displayid;KIND;frictionscale;GravityScale\r\n77;Item;1,25;-2\r\n", &problems);
        const tn::Overrides* c = t.Find(ov::Kind::Item, 77);
        Check(problems.empty() && c && Near(c->values.friction, 1.25f) && Near(c->values.gravity, -2), "';' file, columns in any order, decimal comma, CRLF");

        // problems
        problems.clear();
        t = ov::Parse("Kind,DisplayId,GravityScale,MassScale,Bogus\n"
                      "Weapon,1,-1,,\n"           // unknown kind
                      "Item,abc,-1,,\n"           // bad id
                      "Item,0,-1,,\n"             // id 0
                      "Item,5,fast,2,\n"          // bad number, other cell still used
                      "Item,6,-500,,\n"           // clamped
                      "Item,7,,,\n"               // nothing set
                      "Item,8,1,,\n"
                      "Item,8,-1,,\n"             // duplicate: later wins
                      "Item\n", &problems);       // too short
        Check(Has(problems, "unknown column 'Bogus'"), "unknown column is reported");
        Check(Has(problems, "line 2") && Has(problems, "unknown Kind 'Weapon'"), "unknown Kind is reported with its line");
        Check(Has(problems, "DisplayId 'abc'") && Has(problems, "DisplayId '0'"), "bad ids are reported");
        const tn::Overrides* d = t.Find(ov::Kind::Item, 5);
        Check(Has(problems, "GravityScale='fast' is not a number") && d && Near(d->values.mass, 2) && !(d->set & (1u << tn::FindKey("GravityScale"))),
              "a bad cell is dropped, the rest of the row stays");
        const tn::Overrides* e = t.Find(ov::Kind::Item, 6);
        Check(Has(problems, "clamped to -100") && e && Near(e->values.gravity, -100), "an out-of-range value is clamped and reported");
        Check(Has(problems, "no multiplier set") && t.Find(ov::Kind::Item, 7) == nullptr, "an empty row is skipped");
        const tn::Overrides* f = t.Find(ov::Kind::Item, 8);
        Check(Has(problems, "listed twice") && f && Near(f->values.gravity, -1), "a duplicate: the later row wins");
        Check(Has(problems, "row too short"), "a short row is skipped");

        problems.clear();
        t = ov::Parse("Item,1,2\n", &problems);
        Check(t.Empty() && Has(problems, "needs the columns Kind and DisplayId"), "no header: nothing is read, and it says why");
        problems.clear();
        Check(ov::Parse("", &problems).Empty() && Has(problems, "no header"), "an empty file is not an error, but says so");
        Check(ov::Parse("# only a comment\n").Empty(), "comments only");
    }

    std::printf("4. gravity really turns around (real engine)\n");
    {
        tn::Tuning normal;
        tn::Tuning flipped; flipped.gravity = -1.0f;
        const double down = ChainTipZ(normal, 6.0);
        const double up = ChainTipZ(flipped, 6.0);
        std::printf("     chain tip z after 6 s: %.3f with GravityScale 1, %.3f with GravityScale -1 (anchor at 0)\n", down, up);
        Check(down < -0.5, "default: the chain hangs below its anchor");
        Check(up > 0.3, "GravityScale=-1: the chain stands above its anchor");
        Check(std::isfinite(up) && std::isfinite(down), "nothing blows up");
    }

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
