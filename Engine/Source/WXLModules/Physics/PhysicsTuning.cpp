// Global multipliers for the physics of every model. See PhysicsTuning.hpp.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "PhysicsTuning.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace wxl::equip::physics::tuning
{
    namespace
    {
        struct Key
        {
            const char* name;
            float Tuning::* field;
            float lo, hi;
        };

        // Ranges: a value outside is clamped to it. 0 is meaningful for most (gravity 0 floats, damping 0 never
        // slows); inertia and mass must stay above 0 or the solver divides by nothing.
        constexpr Key kKeys[] = {
            {"GravityScale", &Tuning::gravity, 0.0f, 100.0f},
            {"LinearDampingScale", &Tuning::linearDamping, 0.0f, 100.0f},
            {"AngularDampingScale", &Tuning::angularDamping, 0.0f, 100.0f},
            {"InertiaScale", &Tuning::inertia, 0.01f, 100.0f},
            {"MassScale", &Tuning::mass, 0.01f, 100.0f},
            {"FrictionScale", &Tuning::friction, 0.0f, 100.0f},
            {"RestitutionScale", &Tuning::restitution, 0.0f, 100.0f},
            {"SwingLimitScale", &Tuning::swingLimit, 0.0f, 100.0f},
            {"TwistLimitScale", &Tuning::twistLimit, 0.0f, 100.0f},
            {"JointStiffnessScale", &Tuning::jointStiffness, 0.0f, 100.0f},
            {"JointDampingScale", &Tuning::jointDamping, 0.0f, 100.0f},
            {"MotorTorqueScale", &Tuning::motorTorque, 0.0f, 100.0f},
            {"FollowScale", &Tuning::follow, 0.0f, 100.0f},
        };

        constexpr float kMinHalfRangeDeg = 0.5f;   // a limit scaled to nothing stays a (tight) limit
        constexpr float kConeMinDeg = 10.0f;       // the engine clamps the cone to 10 .. 170 degrees itself
        constexpr float kConeMaxDeg = 170.0f;

        float Clamp(float v, float lo, float hi) { return std::max(lo, std::min(v, hi)); }

        // v = clamp(v * k, lo, hi), only when k != 1; counts a change
        void Scale(float& v, float k, float lo, float hi, int& changed)
        {
            if (k == 1.0f) return;
            const float n = Clamp(v * k, lo, hi);
            if (std::isfinite(n) && n != v)
            {
                v = n;
                ++changed;
            }
        }

        // A symmetric-or-not angular range [lo, hi] in degrees, scaled about its centre; only a real range
        // (lo < hi) is touched, since the engine reads lo >= hi as "no limit" and that must stay so.
        void ScaleRange(float& lo, float& hi, float k, float clampDeg, int& changed)
        {
            if (k == 1.0f || !(lo < hi)) return;
            const float centre = 0.5f * (lo + hi);
            const float half = std::max(0.5f * (hi - lo) * k, kMinHalfRangeDeg);
            const float nl = Clamp(centre - half, -clampDeg, clampDeg), nh = Clamp(centre + half, -clampDeg, clampDeg);
            if (std::isfinite(nl) && std::isfinite(nh) && nl < nh && (nl != lo || nh != hi))
            {
                lo = nl;
                hi = nh;
                ++changed;
            }
        }
    }

    bool Tuning::Identity() const
    {
        for (const Key& k : kKeys)
            if (this->*(k.field) != 1.0f) return false;
        return true;
    }

    bool ParseMultiplier(const char* text, float* out)
    {
        if (!text || !out) return false;
        while (*text == ' ' || *text == '\t') ++text;

        // "123", "1.25", "1,25", ".5": digits with at most one decimal separator. Done by hand because
        // strtod follows the process locale and a "," would stop it on an Italian system.
        double value = 0, scale = 1;
        bool digits = false, fraction = false;
        const char* p = text;
        for (; *p; ++p)
        {
            if (*p >= '0' && *p <= '9')
            {
                digits = true;
                if (!fraction) value = value * 10.0 + (*p - '0');
                else { scale *= 0.1; value += (*p - '0') * scale; }
                if (value > 1.0e12) return false;
            }
            else if ((*p == '.' || *p == ',') && !fraction)
                fraction = true;
            else
                break;
        }
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (!digits || *p != '\0') return false;   // nothing numeric, a sign, an exponent, or trailing junk
        *out = static_cast<float>(value);
        return std::isfinite(*out);
    }

    Tuning FromIni(ReadFn read, std::string* problems)
    {
        Tuning t;
        if (!read) return t;
        auto note = [&](const char* fmt, const char* a, const char* b, float c) {
            if (!problems) return;
            char buf[200];
            std::snprintf(buf, sizeof(buf), fmt, a, b, c);
            *problems += buf;
            *problems += '\n';
        };
        for (const Key& k : kKeys)
        {
            char text[64] = {};
            if (!read(k.name, text, sizeof(text))) continue;
            {   // a blank value ("GravityScale=") means "not set", not a mistake
                const char* p = text;
                while (*p == ' ' || *p == '\t') ++p;
                if (*p == '\0') continue;
            }
            float v = 1.0f;
            if (!ParseMultiplier(text, &v))
            {
                note("%s='%s' is not a number (use 1.25 or 1,25); ignored, stays %g", k.name, text, 1.0f);
                continue;
            }
            const float c = Clamp(v, k.lo, k.hi);
            if (c != v) note("%s='%s' is outside its range; clamped to %g", k.name, text, c);
            t.*(k.field) = c;
        }
        return t;
    }

    std::shared_ptr<phys::Model> Apply(const std::shared_ptr<phys::Model>& in, const Tuning& requested, int* changed)
    {
        if (changed) *changed = 0;
        if (!in || requested.Identity()) return in;

        // Safe whatever the caller passes: every multiplier is brought into its own range (a NaN becomes 1),
        // the same ranges FromIni uses. Zero inertia or mass would make the solver divide by nothing.
        Tuning t = requested;
        for (const Key& k : kKeys)
        {
            float& v = t.*(k.field);
            v = std::isfinite(v) ? Clamp(v, k.lo, k.hi) : 1.0f;
        }

        auto out = std::make_shared<phys::Model>(*in);
        int n = 0;

        for (phys::BodyDef& b : out->bodies)
        {
            if (b.type == phys::kBodyDynamic)
            {
                Scale(b.gravity, t.gravity, 0.0f, 1.0e4f, n);
                Scale(b.linDamp, t.linearDamping, 0.0f, 1.0e4f, n);
                Scale(b.angDamp, t.angularDamping, 0.0f, 1.0e4f, n);
                Scale(b.inertia, t.inertia, 0.0f, 1.0e4f, n);
            }
            else if (b.type == phys::kBodyKinematic)
            {
                Scale(b.carry, t.follow, 0.0f, 1.0f, n);
            }
        }

        for (phys::ShapeDef& s : out->shapes)
        {
            Scale(s.friction, t.friction, 0.0f, 10.0f, n);
            Scale(s.restitution, t.restitution, 0.0f, 1.0f, n);
            if (s.body < out->bodies.size() && out->bodies[s.body].type == phys::kBodyDynamic)
                Scale(s.density, t.mass, 1.0e-4f, 1.0e4f, n);
        }

        for (phys::JointDef& j : out->joints)
        {
            switch (j.type)
            {
            case phys::kJointShoulder:   // twistLo, twistHi, cone (deg), maxTorque, Hz, damping
                ScaleRange(j.f[0], j.f[1], t.twistLimit, 180.0f, n);
                Scale(j.f[2], t.swingLimit, kConeMinDeg, kConeMaxDeg, n);
                Scale(j.f[3], t.motorTorque, 0.0f, 1.0e6f, n);
                Scale(j.f[4], t.jointStiffness, 0.0f, 1.0e4f, n);
                Scale(j.f[5], t.jointDamping, 0.0f, 1.0e4f, n);
                break;
            case phys::kJointRevolute:   // lower, upper (deg), maxTorque, Hz, damping
                ScaleRange(j.f[0], j.f[1], t.twistLimit, 360.0f, n);
                Scale(j.f[2], t.motorTorque, 0.0f, 1.0e6f, n);
                Scale(j.f[3], t.jointStiffness, 0.0f, 1.0e4f, n);
                Scale(j.f[4], t.jointDamping, 0.0f, 1.0e4f, n);
                break;
            case phys::kJointWeld:       // angular Hz, angular damping, linear Hz, linear damping
                Scale(j.f[0], t.jointStiffness, 0.0f, 1.0e4f, n);
                Scale(j.f[1], t.jointDamping, 0.0f, 1.0e4f, n);
                Scale(j.f[2], t.jointStiffness, 0.0f, 1.0e4f, n);
                Scale(j.f[3], t.jointDamping, 0.0f, 1.0e4f, n);
                break;
            default:
                break;   // distance / prismatic / spherical: not touched
            }
        }

        if (changed) *changed = n;
        return out;
    }

    std::string Describe(const Tuning& t)
    {
        std::string s;
        for (const Key& k : kKeys)
        {
            const float v = t.*(k.field);
            if (v == 1.0f) continue;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s%s=%.3g", s.empty() ? "" : " ", k.name, v);
            s += buf;
        }
        return s.empty() ? "none" : s;
    }
}
