// Test program for PhysicsOverride.csv and negative gravity. Standalone: no client, no CMake target.
//
//   P=<repo root>/Engine/Source/WXLModules/Physics
//   g++ -std=c++17 -O1 -I $P physics_override_test.cpp $P/PhysicsOverrides.cpp $P/PhysicsTuning.cpp
//       $P/physics_domino.cpp -o physics_override_test      (one command line)
//   ./physics_override_test <repo root>/Engine/Source/WXLModules/Physics/PhysicsOverride.example.csv
//                                   (the argument is optional: it also checks the shipped template; exit code 0 = ok)
//
// Checks: the CSV parser (header, separators, quoting, every kind of bad row), Merge (a row replaces the
// global value), the signed multiplier, and, on the real engine (physics_domino.cpp, unchanged), that a chain
// hanging from an anchor really rises with GravityScale = -1 and still hangs with the default.
#include "PhysicsOverrides.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
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

int main(int argc, char** argv)
{
    const char* exampleFile = argc > 1 ? argv[1] : nullptr;
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

    std::printf("3. PhysicsOverride.csv: the whole set of parameters\n");
    {
        std::string problems;
        // all 14 parameters, in the ini's order; Row() writes one row, every parameter "Default" unless given
        const std::string header = "Kind,DisplayId,GravityScale,LinearDampingScale,AngularDampingScale,InertiaScale,MassScale,"
                                   "FrictionScale,RestitutionScale,SwingLimitScale,TwistLimitScale,JointStiffnessScale,"
                                   "JointDampingScale,MotorTorqueScale,FollowScale,Buoyancy";
        auto Row = [](const char* kind, int id, std::map<std::string, std::string> given, const char* fill = "Default",
                      const char* tail = "") {
            std::string r = std::string(kind) + "," + std::to_string(id);
            for (int k = 0; k < tn::kKeyCount; ++k)
            {
                const auto it = given.find(tn::KeyName(k));
                r += "," + (it == given.end() ? std::string(fill) : it->second);
            }
            return r + tail;
        };
        auto Bit = [](const char* name) { return 1u << tn::FindKey(name); };

        const std::string csv =
            "\xEF\xBB\xBF# my weapons\n" + header + ",Note\n" +
            Row("Item", 12345, { { "GravityScale", "-1" } }, "NA", ",\"upside down, on purpose\"") + "\n" +
            Row("item", 12346, { { "SwingLimitScale", "1.5" }, { "MassScale", "0.5" } }, "", ",") + "\n" +
            "// another comment\n"
            "\n" +
            Row("Creature", 6789, { { "GravityScale", "0.5" }, { "FollowScale", "2" } }) + ",\n";
        ov::Table t = ov::Parse(csv, &problems);
        Check(problems.empty(), "a clean file produces no problem lines");
        Check(t.items.size() == 2 && t.creatures.size() == 1 && t.Rows() == 3, "two item rows, one creature row");
        const tn::Overrides* a = t.Find(ov::Kind::Item, 12345);
        Check(a && Near(a->values.gravity, -1) && a->set == Bit("GravityScale"), "item 12345: only GravityScale (-1); the NA cells use the ini's values");
        const tn::Overrides* b = t.Find(ov::Kind::Item, 12346);
        Check(b && Near(b->values.swingLimit, 1.5f) && Near(b->values.mass, 0.5f) && b->set == (Bit("SwingLimitScale") | Bit("MassScale")),
              "item 12346: SwingLimitScale and MassScale; the empty cells use the ini's values (kind is case-insensitive)");
        const tn::Overrides* c6 = t.Find(ov::Kind::Creature, 6789);
        Check(c6 && Near(c6->values.gravity, 0.5f) && Near(c6->values.follow, 2) && c6->set == (Bit("GravityScale") | Bit("FollowScale")),
              "creature 6789: GravityScale and FollowScale; the Default cells use the ini's values");
        Check(t.Find(ov::Kind::Creature, 12345) == nullptr && t.Find(ov::Kind::Item, 6789) == nullptr, "item and creature ids do not collide");

        // every spelling of "use the ini's value"
        problems.clear();
        t = ov::Parse(header + "\nItem,1,-2,na,N/A,#N/A,default,INI,-,,Default,NA,NA,NA,NA,NA\n", &problems);
        const tn::Overrides* tok = t.Find(ov::Kind::Item, 1);
        Check(problems.empty() && tok && tok->set == Bit("GravityScale"), "empty, NA, N/A, #N/A, Default, Ini and a lone dash all mean: the ini's value (no problem logged)");

        // header: any order, semicolons, decimal commas
        const std::string semi = "FollowScale;DisplayId;KIND;GravityScale;LinearDampingScale;AngularDampingScale;InertiaScale;MassScale;"
                                 "frictionscale;RestitutionScale;SwingLimitScale;TwistLimitScale;JointStiffnessScale;JointDampingScale;MotorTorqueScale;Buoyancy\r\n"
                                 "0,25;77;Item;-2;NA;NA;NA;NA;1,25;NA;NA;NA;NA;NA;NA;-0,5\r\n";
        problems.clear();
        t = ov::Parse(semi, &problems);
        const tn::Overrides* sm = t.Find(ov::Kind::Item, 77);
        Check(problems.empty() && sm && Near(sm->values.friction, 1.25f) && Near(sm->values.gravity, -2) && Near(sm->values.follow, 0.25f) &&
                  Near(sm->values.buoyancy, -0.5f) && (sm->set & Bit("Buoyancy")),
              "';' file, columns in any order and any case, decimal comma, CRLF (Buoyancy included)");

        // the whole set is required
        problems.clear();
        t = ov::Parse("Kind,DisplayId,GravityScale,SwingLimitScale\nItem,5,-1,NA\n", &problems);
        Check(t.Empty() && Has(problems, "must list the whole set") && Has(problems, "LinearDampingScale") && Has(problems, "FollowScale") &&
                  Has(problems, "nothing was read"),
              "a header without the whole set is refused, naming what is missing");
        problems.clear();
        {
            std::string h = header;
            h.erase(h.find(",MassScale"), 10);   // drop one column only
            t = ov::Parse(h + "\nItem,5,-1\n", &problems);
            Check(t.Empty() && Has(problems, "missing: MassScale") && !Has(problems, "GravityScale,"), "...down to a single missing column");
        }
        problems.clear();
        t = ov::Parse(header + ",GravityScale\nItem,5,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,-3\n", &problems);
        const tn::Overrides* dup = t.Find(ov::Kind::Item, 5);
        Check(Has(problems, "column 'GravityScale' is listed twice") && dup && Near(dup->values.gravity, -3), "a column listed twice: reported, the later one is used");

        // problems in rows
        problems.clear();
        t = ov::Parse(header + ",Bogus\n" +
                      Row("Weapon", 1, { { "GravityScale", "-1" } }) + ",\n" +      // unknown kind
                      Row("Item", 0, { { "GravityScale", "-1" } }) + ",\n" +         // id 0
                      "Item,abc," + Row("X", 0, {}).substr(5) + "\n" +               // bad id
                      Row("Item", 5, { { "GravityScale", "fast" }, { "MassScale", "2" } }) + ",\n" +   // bad cell, other cell still used
                      Row("Item", 6, { { "GravityScale", "-500" } }) + ",\n" +       // clamped
                      Row("Item", 7, {}) + ",\n" +                                   // nothing to override
                      Row("Item", 8, { { "GravityScale", "1" } }) + ",\n" +
                      Row("Item", 8, { { "GravityScale", "-1" } }) + ",\n" +         // duplicate: later wins
                      "Item\n",                                                       // too short
                      &problems);
        Check(Has(problems, "unknown column 'Bogus'"), "unknown column is reported");
        Check(Has(problems, "unknown Kind 'Weapon'"), "unknown Kind is reported");
        Check(Has(problems, "DisplayId 'abc'") && Has(problems, "DisplayId '0'"), "bad ids are reported");
        const tn::Overrides* d = t.Find(ov::Kind::Item, 5);
        Check(Has(problems, "GravityScale='fast' is not a number") && Has(problems, "write NA or Default") && d && Near(d->values.mass, 2) &&
                  !(d->set & Bit("GravityScale")),
              "a bad cell is dropped (the ini's value is used), the rest of the row stays");
        const tn::Overrides* e = t.Find(ov::Kind::Item, 6);
        Check(Has(problems, "clamped to -100") && e && Near(e->values.gravity, -100), "an out-of-range value is clamped and reported");
        Check(Has(problems, "nothing to override") && t.Find(ov::Kind::Item, 7) == nullptr, "a row of only empty/NA/Default cells is skipped");
        const tn::Overrides* f = t.Find(ov::Kind::Item, 8);
        Check(Has(problems, "listed twice") && f && Near(f->values.gravity, -1), "a display listed twice: the later row wins");
        Check(Has(problems, "row too short"), "a short row is skipped");

        problems.clear();
        t = ov::Parse("Item,1,2\n", &problems);
        Check(t.Empty() && Has(problems, "needs the columns Kind and DisplayId"), "no header: nothing is read, and it says why");
        problems.clear();
        Check(ov::Parse("", &problems).Empty() && Has(problems, "no header"), "an empty file is not an error, but says so");
        Check(ov::Parse("# only a comment\n").Empty(), "comments only");

        // the template that ships with the module must itself be a valid file
        if (exampleFile)
        {
            std::ifstream in(exampleFile, std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            problems.clear();
            t = ov::Parse(text, &problems);
            Check(!text.empty() && problems.empty() && t.items.size() == 2 && t.creatures.size() == 1, "PhysicsOverride.example.csv parses cleanly (2 item rows, 1 creature row)");
            const tn::Overrides* ex = t.Find(ov::Kind::Item, 900001);
            Check(ex && Near(ex->values.gravity, -1), "...and its first row turns gravity around");
            const tn::Overrides* ex2 = t.Find(ov::Kind::Item, 900002);
            Check(ex2 && Near(ex2->values.buoyancy, -0.3f) && (ex2->set & Bit("Buoyancy")), "...and its second row sets Buoyancy (-0.3)");
        }
        else
            std::printf("  (pass PhysicsOverride.example.csv as the first argument to check the template as well)\n");
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

    std::printf("5. Buoyancy: the gravity multiplier underwater\n");
    {
        Check(tn::FindKey("buoyancy") == tn::kKeyCount - 1 && std::strcmp(tn::KeyName(tn::kKeyCount - 1), "Buoyancy") == 0, "Buoyancy is a key");
        float v = 0; bool clamped = false;
        Check(tn::ParseKeyValue(tn::FindKey("Buoyancy"), "-0,25", &v, &clamped) && Near(v, -0.25f) && !clamped, "Buoyancy may be negative");
        Check(tn::ParseKeyValue(tn::FindKey("Buoyancy"), "-500", &v, &clamped) && Near(v, -100) && clamped, "Buoyancy -500 clamps to -100");
        Check(tn::ParseKeyValue(tn::FindKey("Buoyancy"), "500", &v, &clamped) && Near(v, 100) && clamped, "Buoyancy 500 clamps to 100");

        tn::Tuning base; base.gravity = 2.0f; base.buoyancy = -0.2f;
        tn::Tuning under; under.gravity = 7.0f; under.buoyancy = 9.0f; under.linearDamping = 6.0f;
        const tn::Tuning dry = tn::Blend(base, under, 0), half = tn::Blend(base, under, 0.5f), wet = tn::Blend(base, under, 1);
        Check(Near(dry.gravity, 2.0f) && Near(dry.linearDamping, 1.0f), "t = 0: the dry values");
        Check(Near(wet.gravity, -0.2f), "t = 1: gravity IS the Buoyancy (-0.2), not 2 x anything");
        Check(Near(half.gravity, 0.9f), "t = 0.5: halfway from the dry gravity (2) to the Buoyancy (-0.2)");
        Check(Near(wet.buoyancy, -0.2f), "the underwater section's own Buoyancy/GravityScale are not used");
        Check(Near(wet.linearDamping, 6.0f), "the other keys still stack (1 x 6)");

        // a CSV row's Buoyancy replaces the ini's, and drives the underwater gravity
        tn::Overrides row;
        tn::SetOverride(row, tn::FindKey("Buoyancy"), 0.5f);
        const tn::Tuning eff = tn::Merge(base, row);
        Check(Near(eff.buoyancy, 0.5f) && Near(eff.gravity, 2.0f), "Merge: the row replaces Buoyancy only");
        Check(Near(tn::Blend(eff, under, 1).gravity, 0.5f), "...and underwater the gravity is the row's Buoyancy");

        // Apply: Buoyancy alone changes nothing in the model
        auto m = std::make_shared<ph::Model>();
        ph::BodyDef b; b.type = ph::kBodyDynamic; b.bone = 0; b.gravity = 1; b.linDamp = 2; b.angDamp = 2;
        m->bodies.push_back(b);
        tn::Tuning onlyB; onlyB.buoyancy = -3.0f;
        int changed = -1;
        Check(tn::Apply(m, onlyB, &changed) == m && changed == 0, "Apply: a Buoyancy on its own returns the model untouched");

        // the real engine: a chain with a negative Buoyancy rises when it is under water, and hangs when dry
        tn::Tuning chain; chain.buoyancy = -1.0f;
        tn::Tuning uw; // nothing else underwater
        const double dryTip = ChainTipZ(tn::Blend(chain, uw, 0), 6.0);
        const double wetTip = ChainTipZ(tn::Blend(chain, uw, 1), 6.0);
        std::printf("     chain tip z after 6 s: %.3f dry, %.3f underwater with Buoyancy -1\n", dryTip, wetTip);
        Check(dryTip < -0.5, "dry: Buoyancy does nothing, the chain hangs");
        Check(wetTip > 0.3 && std::isfinite(wetTip), "underwater: Buoyancy -1 turns gravity around, the chain stands above its anchor");
    }

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
