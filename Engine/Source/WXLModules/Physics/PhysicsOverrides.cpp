// See PhysicsOverrides.hpp.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "PhysicsOverrides.hpp"

#include <cctype>
#include <cstdio>
#include <vector>

namespace wxl::equip::physics::overrides
{
    namespace
    {
        std::string Trim(const std::string& s)
        {
            size_t b = 0, e = s.size();
            while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
            while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
            return s.substr(b, e - b);
        }

        bool EqualsNoCase(const std::string& a, const char* b)
        {
            size_t i = 0;
            for (; i < a.size() && b[i]; ++i)
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
            return i == a.size() && !b[i];
        }

        // One line into cells. A cell may be wrapped in double quotes ("" inside is a quote); the separator
        // inside quotes is part of the cell.
        std::vector<std::string> Split(const std::string& line, char sep)
        {
            std::vector<std::string> cells;
            std::string cur;
            bool quoted = false;
            for (size_t i = 0; i < line.size(); ++i)
            {
                const char c = line[i];
                if (quoted)
                {
                    if (c == '"')
                    {
                        if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
                        else quoted = false;
                    }
                    else cur += c;
                }
                else if (c == '"') quoted = true;
                else if (c == sep)
                {
                    cells.push_back(Trim(cur));
                    cur.clear();
                }
                else cur += c;
            }
            cells.push_back(Trim(cur));
            return cells;
        }

        bool ParseId(const std::string& text, uint32_t* out)
        {
            if (text.empty() || text.size() > 10) return false;
            uint64_t v = 0;
            for (char c : text)
            {
                if (c < '0' || c > '9') return false;
                v = v * 10 + uint64_t(c - '0');
            }
            if (v == 0 || v > 0x7FFFFFFFu) return false;
            *out = static_cast<uint32_t>(v);
            return true;
        }

        bool IsSilentColumn(const std::string& name)
        {
            static const char* const names[] = { "note", "notes", "comment", "comments", "name", "description" };
            for (const char* n : names)
                if (EqualsNoCase(name, n)) return true;
            return false;
        }

        struct Problems
        {
            std::string* out;
            void Add(size_t line, const char* fmt, const std::string& a = std::string(), const std::string& b = std::string())
            {
                if (!out) return;
                char buf[400];
                std::snprintf(buf, sizeof(buf), fmt, a.c_str(), b.c_str());
                *out += "PhysicsOverride.csv line " + std::to_string(line) + ": " + buf + "\n";
            }
        };
    }

    const tuning::Overrides* Table::Find(Kind kind, uint32_t displayId) const
    {
        const auto& map = kind == Kind::Item ? items : creatures;
        const auto it = map.find(displayId);
        return it == map.end() ? nullptr : &it->second;
    }

    Table Parse(const std::string& textIn, std::string* problemsOut)
    {
        Table table;
        Problems problems{ problemsOut };

        std::string text = textIn;
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF)
            text.erase(0, 3);   // a UTF-8 byte order mark, which a spreadsheet likes to write

        // header state
        bool haveHeader = false;
        char sep = ',';
        int kindCol = -1, idCol = -1;
        std::vector<int> keyOfColumn;   // column -> multiplier key index, -1 for none

        size_t lineNo = 0, pos = 0;
        while (pos <= text.size())
        {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            const std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            ++lineNo;

            const std::string trimmed = Trim(line);
            if (trimmed.empty() || trimmed[0] == '#' || trimmed.compare(0, 2, "//") == 0) continue;

            if (!haveHeader)
            {
                haveHeader = true;
                size_t commas = 0, semis = 0;
                for (char c : trimmed)
                {
                    if (c == ',') ++commas;
                    else if (c == ';') ++semis;
                }
                if (semis > 0 && commas == 0) sep = ';';

                const std::vector<std::string> cols = Split(trimmed, sep);
                keyOfColumn.assign(cols.size(), -1);
                for (size_t c = 0; c < cols.size(); ++c)
                {
                    if (EqualsNoCase(cols[c], "kind")) kindCol = int(c);
                    else if (EqualsNoCase(cols[c], "displayid") || EqualsNoCase(cols[c], "display_id")) idCol = int(c);
                    else if (cols[c].empty() || IsSilentColumn(cols[c])) continue;
                    else
                    {
                        const int key = tuning::FindKey(cols[c].c_str());
                        if (key >= 0) keyOfColumn[c] = key;
                        else problems.Add(lineNo, "unknown column '%s' ignored (the multipliers are named like the ini keys, e.g. GravityScale)", cols[c]);
                    }
                }
                if (kindCol < 0 || idCol < 0)
                {
                    problems.Add(lineNo, "the header needs the columns Kind and DisplayId (found: '%s'); nothing was read", trimmed);
                    return Table{};
                }
                continue;
            }

            const std::vector<std::string> cells = Split(trimmed, sep);
            if (size_t(kindCol) >= cells.size() || size_t(idCol) >= cells.size())
            {
                problems.Add(lineNo, "row too short (needs Kind and DisplayId); skipped%s", std::string());
                continue;
            }

            Kind kind;
            if (EqualsNoCase(cells[kindCol], "item")) kind = Kind::Item;
            else if (EqualsNoCase(cells[kindCol], "creature")) kind = Kind::Creature;
            else
            {
                problems.Add(lineNo, "unknown Kind '%s' (use Item or Creature); skipped", cells[kindCol]);
                continue;
            }
            uint32_t id = 0;
            if (!ParseId(cells[idCol], &id))
            {
                problems.Add(lineNo, "DisplayId '%s' is not a whole number above 0; skipped", cells[idCol]);
                continue;
            }

            tuning::Overrides row;
            for (size_t c = 0; c < cells.size() && c < keyOfColumn.size(); ++c)
            {
                const int key = keyOfColumn[c];
                if (key < 0 || cells[c].empty()) continue;
                float v = 1.0f;
                bool clamped = false;
                if (!tuning::ParseKeyValue(key, cells[c].c_str(), &v, &clamped))
                {
                    problems.Add(lineNo, (std::string(tuning::KeyName(key)) + "='%s' is not a number (use 1.25, or 1,25 in a ';' file); that cell is ignored%s").c_str(),
                                 cells[c]);
                    continue;
                }
                if (clamped)
                {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%g", v);
                    problems.Add(lineNo, (std::string(tuning::KeyName(key)) + "='%s' is outside its range; clamped to %s").c_str(), cells[c], buf);
                }
                tuning::SetOverride(row, key, v);
            }
            if (!row.Any())
            {
                problems.Add(lineNo, "no multiplier set for this row; skipped%s", std::string());
                continue;
            }

            auto& map = kind == Kind::Item ? table.items : table.creatures;
            if (map.count(id))
                problems.Add(lineNo, "this display is listed twice; the later row wins%s", std::string());
            map[id] = row;
        }
        if (!haveHeader && problems.out) *problems.out += "PhysicsOverride.csv: no header line; nothing was read\n";
        return table;
    }
}
