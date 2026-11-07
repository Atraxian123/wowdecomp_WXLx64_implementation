// WXL spell area indicator: shape rules, override file, textures. See SpellAreaShapes.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)

#include "Engine/Source/WXL/SpellAreaShapes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wxl::spellarea
{
    namespace
    {
        bool InSet(int32_t v, const int32_t* set, size_t n)
        {
            for (size_t i = 0; i < n; ++i)
                if (set[i] == v) return true;
            return false;
        }
        template <size_t N> bool In(int32_t v, const int32_t (&set)[N]) { return InSet(v, set, N); }

        std::string Trim(const std::string& s)
        {
            size_t a = 0, b = s.size();
            while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
            while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
            return s.substr(a, b - a);
        }

        std::string Lower(std::string s)
        {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        bool ParseFloat(const std::string& s, float& out)
        {
            std::string t = s;
            for (char& c : t) if (c == ',') c = '.';   // never reached with a comma separator, kept for ';' files
            char* end = nullptr;
            const double v = std::strtod(t.c_str(), &end);
            if (end == t.c_str() || !std::isfinite(v)) return false;
            out = static_cast<float>(v);
            return true;
        }

        float Smooth(float x) { x = std::max(0.0f, std::min(1.0f, x)); return x * x * (3.0f - 2.0f * x); }

        // alpha of a shape drawn as a translucent fill with a bright outline, from the signed distance to its border
        // (positive inside, in texture units)
        float ShapeAlpha(float signedDist, float pixel)
        {
            const float fill = 0.24f * Smooth(0.5f + signedDist / pixel);
            const float ringHalf = 0.0125f;
            const float ring = 1.0f - Smooth((std::fabs(signedDist) - ringHalf) / pixel + 0.5f);
            return std::max(fill, ring);
        }

        uint32_t PackWhite(float alpha)
        {
            const uint32_t a = static_cast<uint32_t>(std::max(0.0f, std::min(1.0f, alpha)) * 255.0f + 0.5f);
            return (a << 24) | 0x00FFFFFFu;
        }
    }

    bool IsConeTarget(int32_t t)         { static const int32_t s[] = { 24, 54, 59, 60, 104 }; return In(t, s); }
    bool IsCasterAreaTarget(int32_t t)   { static const int32_t s[] = { 7, 11, 15, 20, 30, 33, 56 }; return In(t, s); }
    bool IsDestAreaTarget(int32_t t)     { static const int32_t s[] = { 8, 16, 28, 29, 31, 34, 87, 91 }; return In(t, s); }
    bool IsEnemyTarget(int32_t t)        { static const int32_t s[] = { 2, 6, 15, 16, 24, 28, 53, 54, 104, 93 }; return In(t, s); }
    bool IsAllyTarget(int32_t t)         { static const int32_t s[] = { 3, 4, 20, 21, 29, 30, 31, 33, 34, 35, 56, 57, 58, 59, 45, 37 }; return In(t, s); }

    size_t ShapesFromSpell(const SpellEffects& spell, const ShapeRules& rules, Shape out[3])
    {
        static const int32_t kAreaAuraCaster[] = { 35, 65, 119, 128, 129, 143 };
        static const int32_t kAreaAuraHelpful[] = { 35, 65, 119, 128, 143 };

        size_t n = 0;
        for (int i = 0; i < 3; ++i)
        {
            if (!spell.effect[i]) continue;
            float r = spell.radius[i];
            if (!(r >= rules.minRadius)) continue;
            r = std::min(r, rules.maxRadius);

            const int32_t a = spell.targetA[i], b = spell.targetB[i], eff = spell.effect[i];
            const bool cone = IsConeTarget(a) || IsConeTarget(b);
            bool casterArea = IsCasterAreaTarget(a) || IsCasterAreaTarget(b);
            bool destArea = IsDestAreaTarget(a) || IsDestAreaTarget(b);
            if (!cone && !casterArea && !destArea)
            {
                if (In(eff, kAreaAuraCaster)) casterArea = true;
                else if (eff == 27) destArea = true;       // persistent area aura: the area is at the destination
            }
            if (!cone && !casterArea && !destArea) continue;
            if (cone && !rules.cones) continue;

            Shape s;
            s.radius = r;
            if (cone)            { s.kind = ShapeKind::Cone; s.anchor = Anchor::Caster; s.param = rules.coneAngle; }
            else if (casterArea) { s.kind = ShapeKind::Circle; s.anchor = Anchor::Caster; }
            else                 { s.kind = ShapeKind::Circle; s.anchor = Anchor::Dest; }

            if (IsEnemyTarget(a) || IsEnemyTarget(b) || eff == 129) s.intent = Intent::Harmful;
            else if (IsAllyTarget(a) || IsAllyTarget(b) || In(eff, kAreaAuraHelpful)) s.intent = Intent::Helpful;
            else s.intent = Intent::Neutral;

            bool dup = false;
            for (size_t k = 0; k < n; ++k)
                if (out[k].kind == s.kind && out[k].anchor == s.anchor && std::fabs(out[k].radius - s.radius) < 0.01f)
                {
                    // a damaging effect and a helpful one on the same spot: the damage is what to be careful about
                    if (s.intent == Intent::Harmful) out[k].intent = Intent::Harmful;
                    dup = true;
                    break;
                }
            if (!dup) out[n++] = s;
        }
        return n;
    }

    void OverrideTable::Parse(const std::string& text, std::string& problems)
    {
        m_rows.clear();
        size_t pos = 0;
        int lineNo = 0;
        while (pos <= text.size())
        {
            size_t eol = text.find('\n', pos);
            if (eol == std::string::npos) eol = text.size();
            std::string line = text.substr(pos, eol - pos);
            pos = eol + 1;
            ++lineNo;

            if (!line.empty() && line.back() == '\r') line.pop_back();
            line = Trim(line);
            if (line.empty() || line[0] == '#') continue;

            const char sep = line.find(';') != std::string::npos ? ';' : ',';
            std::vector<std::string> cells;
            size_t a = 0;
            for (;;)
            {
                const size_t b = line.find(sep, a);
                cells.push_back(Trim(line.substr(a, b == std::string::npos ? std::string::npos : b - a)));
                if (b == std::string::npos) break;
                a = b + 1;
            }

            char* end = nullptr;
            const unsigned long id = std::strtoul(cells[0].c_str(), &end, 10);
            if (cells[0].empty() || *end != '\0')
            {
                if (lineNo != 1 && !Lower(cells[0]).starts_with("spell"))
                    problems += "line " + std::to_string(lineNo) + ": the spell id '" + cells[0] + "' is not a number; skipped\n";
                continue;   // the header line, or a bad one
            }

            Row row{};
            row.spell = static_cast<uint32_t>(id);
            const std::string shape = cells.size() > 1 ? Lower(cells[1]) : "";
            if (shape == "none") { row.none = true; m_rows.push_back(row); continue; }

            if (shape == "circle") row.shape.kind = ShapeKind::Circle;
            else if (shape == "cone") row.shape.kind = ShapeKind::Cone;
            else if (shape == "line") row.shape.kind = ShapeKind::Line;
            else { problems += "line " + std::to_string(lineNo) + ": shape '" + shape + "' is not circle, cone, line or none; skipped\n"; continue; }

            const std::string anchor = cells.size() > 2 ? Lower(cells[2]) : "";
            row.shape.anchor = (anchor == "dest" || anchor == "destination") && row.shape.kind == ShapeKind::Circle ? Anchor::Dest : Anchor::Caster;

            float radius = 0;
            if (cells.size() < 4 || !ParseFloat(cells[3], radius) || radius <= 0)
            {
                problems += "line " + std::to_string(lineNo) + ": the radius / length is missing or not above 0; skipped\n";
                continue;
            }
            row.shape.radius = radius;

            float param = row.shape.kind == ShapeKind::Cone ? 90.0f : 4.0f;
            if (cells.size() > 4 && !cells[4].empty() && !ParseFloat(cells[4], param))
            {
                problems += "line " + std::to_string(lineNo) + ": the angle / width '" + cells[4] + "' is not a number; skipped\n";
                continue;
            }
            row.shape.param = param;

            const std::string intent = cells.size() > 5 ? Lower(cells[5]) : "";
            row.shape.intent = intent == "harmful" ? Intent::Harmful : intent == "helpful" ? Intent::Helpful : Intent::Neutral;
            m_rows.push_back(row);
        }
    }

    bool OverrideTable::Find(uint32_t spellId, bool& none, std::vector<Shape>& shapes) const
    {
        none = false;
        shapes.clear();
        bool found = false;
        for (const Row& r : m_rows)
        {
            if (r.spell != spellId) continue;
            found = true;
            if (r.none) none = true;
            else shapes.push_back(r.shape);
        }
        if (none) shapes.clear();
        return found;
    }

    bool ParseColorHex(const char* text, float rgb[3])
    {
        if (!text) return false;
        if (*text == '#') ++text;
        if (std::strlen(text) < 6) return false;
        unsigned v[3] = {};
        for (int i = 0; i < 6; ++i)
            if (!std::isxdigit(static_cast<unsigned char>(text[i]))) return false;
        if (std::sscanf(text, "%2x%2x%2x", &v[0], &v[1], &v[2]) != 3) return false;
        for (int i = 0; i < 3; ++i) rgb[i] = static_cast<float>(v[i]) / 255.0f;
        return true;
    }

    void RenderCircleTexture(std::vector<uint32_t>& argb, int size)
    {
        argb.assign(static_cast<size_t>(size) * size, 0);
        const float pixel = 1.0f / static_cast<float>(size);
        const float R = 0.485f;
        for (int row = 0; row < size; ++row)
            for (int col = 0; col < size; ++col)
            {
                const float x = (col + 0.5f) * pixel - 0.5f, y = 0.5f - (row + 0.5f) * pixel;
                argb[static_cast<size_t>(row) * size + col] = PackWhite(ShapeAlpha(R - std::sqrt(x * x + y * y), pixel));
            }
    }

    void RenderConeTexture(std::vector<uint32_t>& argb, int size, float angleDeg)
    {
        argb.assign(static_cast<size_t>(size) * size, 0);
        const float pixel = 1.0f / static_cast<float>(size);
        const float R = 0.485f;
        const float kPi = 3.14159265358979f;
        const float half = std::max(1.0f, std::min(180.0f, angleDeg)) * 0.5f * kPi / 180.0f;
        for (int row = 0; row < size; ++row)
            for (int col = 0; col < size; ++col)
            {
                const float x = (col + 0.5f) * pixel - 0.5f, y = 0.5f - (row + 0.5f) * pixel;
                const float r = std::sqrt(x * x + y * y);
                const float theta = std::atan2(std::fabs(x), y);      // angle from the front, 0..pi
                float edge;                                           // signed distance to the straight sides
                if (theta <= half) edge = r * std::sin(half - theta);
                else if (theta - half > kPi * 0.5f) edge = -r;        // behind the apex: far from both sides
                else edge = r * std::sin(half - theta);
                const float d = std::min(R - r, edge);
                argb[static_cast<size_t>(row) * size + col] = PackWhite(ShapeAlpha(d, pixel));
            }
    }

    void RenderLineTexture(std::vector<uint32_t>& argb, int size, float widthFraction)
    {
        argb.assign(static_cast<size_t>(size) * size, 0);
        const float pixel = 1.0f / static_cast<float>(size);
        const float halfW = std::max(pixel * 2.0f, std::min(1.0f, widthFraction)) * 0.5f;
        const float top = 0.485f;
        for (int row = 0; row < size; ++row)
            for (int col = 0; col < size; ++col)
            {
                const float x = (col + 0.5f) * pixel - 0.5f, y = 0.5f - (row + 0.5f) * pixel;
                const float dx = halfW - std::fabs(x);
                const float dy = std::min(y, top - y);
                argb[static_cast<size_t>(row) * size + col] = PackWhite(ShapeAlpha(std::min(dx, dy), pixel));
            }
    }
}
