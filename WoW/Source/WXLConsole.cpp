// The "wxl" console command: look at WXLSetup.ini and change it live.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
//   wxl                                   this list
//   wxl list                              the sections of the ini
//   wxl get <Section>                     every key of a section ("*" marks one that is overridden)
//   wxl get <Section> <Key>   |   wxl <Section>.<Key>
//   wxl set <Section> <Key> <value>       |   wxl <Section>.<Key> <value>
//                                         changes it for this session; nothing is written to the file
//   wxl toggle <Section> <Key>            flips a 0 / 1 switch
//   wxl unset <Section> <Key>             drops one override
//   wxl overrides                         what is overridden now
//   wxl reset                             drops every override: the file's values again
//   wxl save                              writes the overrides into WXLSetup.ini (comments and layout are kept)
//   wxl path                              which file is in use
//
// Everything that reads its settings through wxl::ini notices the change by itself (see Engine/Source/WXL/Ini.h):
// the log switches, [Culling], [Animation], the [Particles] settings (for models loaded afterwards) and, through the
// module API, every module that re-reads the ini when it is saved. A setting a module only reads at start-up needs a
// restart; `wxl save` first if it should be kept.

#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "WoW/Source/WXLConsole.h"
#include "Engine/Source/ConsoleBase/ConsoleCommand.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/WXL/Ini.h"

namespace
{
    void Say(COLOR_T color, const char* format, ...)
    {
        char line[1024];
        va_list args;
        va_start(args, format);
        std::vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        ConsoleWrite(line, color);
    }

    // "set  Outline  Quest 0" -> { "set", "Outline", "Quest", "0" }; a double-quoted word may hold spaces.
    std::vector<std::string> Words(const char* text)
    {
        std::vector<std::string> out;
        std::string cur;
        bool quoted = false, any = false;
        for (const char* p = text ? text : ""; *p; ++p)
        {
            if (*p == '"') { quoted = !quoted; any = true; continue; }
            if (!quoted && (*p == ' ' || *p == '\t'))
            {
                if (any) out.push_back(cur);
                cur.clear();
                any = false;
                continue;
            }
            cur += *p;
            any = true;
        }
        if (any) out.push_back(cur);
        return out;
    }

    // The text after the first n words, as typed (a value may hold spaces).
    std::string Rest(const char* text, size_t skipWords)
    {
        const char* p = text ? text : "";
        for (size_t i = 0; i < skipWords; ++i)
        {
            while (*p == ' ' || *p == '\t') ++p;
            while (*p && *p != ' ' && *p != '\t') ++p;
        }
        while (*p == ' ' || *p == '\t') ++p;
        std::string v = p;
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        return v;
    }

    bool IsOverridden(const char* section, const char* key)
    {
        for (const wxl::ini::Entry& e : wxl::ini::Overrides())
        {
            auto same = [](const std::string& a, const char* b) {
                if (a.size() != std::strlen(b)) return false;
                for (size_t i = 0; i < a.size(); ++i)
                    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
                return true;
            };
            if (same(e.section, section) && same(e.key, key)) return true;
        }
        return false;
    }

    void Help()
    {
        Say(HIGHLIGHT_COLOR, "wxl: WXLSetup.ini, live");
        Say(DEFAULT_COLOR, "  wxl list                          the sections");
        Say(DEFAULT_COLOR, "  wxl get <Section> [<Key>]         show a section, or one key  (also: wxl <Section>.<Key>)");
        Say(DEFAULT_COLOR, "  wxl set <Section> <Key> <value>  change it for this session  (also: wxl <Section>.<Key> <value>)");
        Say(DEFAULT_COLOR, "  wxl toggle <Section> <Key>       flip a 0/1 switch");
        Say(DEFAULT_COLOR, "  wxl unset <Section> <Key>        drop one override;  wxl reset  drops them all");
        Say(DEFAULT_COLOR, "  wxl overrides                    what is overridden now");
        Say(DEFAULT_COLOR, "  wxl save                         write the overrides into the file");
        Say(DEFAULT_COLOR, "  wxl path                         which file is in use");
    }

    void ShowKey(const char* section, const char* key)
    {
        char value[1024];
        if (!wxl::ini::GetString(section, key, value, sizeof(value)))
            Say(DEFAULT_COLOR, "[%s] %s is not set (the module's default applies)", section, key);
        else
            Say(DEFAULT_COLOR, "[%s] %s = %s%s", section, key, value, IsOverridden(section, key) ? "   (overridden this session)" : "");
    }

    void SetKey(const char* section, const char* key, const std::string& value)
    {
        if (!wxl::ini::Set(section, key, value.c_str()))
        {
            Say(ERROR_COLOR, "wxl: cannot set [%s] %s", section, key);
            return;
        }
        Say(HIGHLIGHT_COLOR, "[%s] %s = %s   (this session; `wxl save` writes it to the file)", section, key, value.c_str());
    }

    int32_t WxlCommand(const char* command, const char* arguments)
    {
        (void)command;
        std::vector<std::string> w = Words(arguments);
        if (w.empty() || w[0] == "help" || w[0] == "?") { Help(); return 1; }

        // wxl Section.Key [value]
        if (w[0].find('.') != std::string::npos && w[0] != "." )
        {
            const size_t dot = w[0].find('.');
            const std::string section = w[0].substr(0, dot), key = w[0].substr(dot + 1);
            if (section.empty() || key.empty()) { Help(); return 1; }
            if (w.size() == 1) ShowKey(section.c_str(), key.c_str());
            else SetKey(section.c_str(), key.c_str(), Rest(arguments, 1));
            return 1;
        }

        const std::string& verb = w[0];
        if (verb == "list")
        {
            for (const std::string& s : wxl::ini::Sections()) Say(DEFAULT_COLOR, "  [%s]", s.c_str());
        }
        else if (verb == "get")
        {
            if (w.size() < 2) { Say(ERROR_COLOR, "usage: wxl get <Section> [<Key>]"); return 1; }
            if (w.size() >= 3) { ShowKey(w[1].c_str(), w[2].c_str()); return 1; }
            const auto keys = wxl::ini::Section(w[1].c_str());
            if (keys.empty()) Say(WARNING_COLOR, "[%s]: no such section, or it has no keys (`wxl list`)", w[1].c_str());
            for (const auto& kv : keys)
                Say(DEFAULT_COLOR, "  %s%s = %s", IsOverridden(w[1].c_str(), kv.first.c_str()) ? "*" : " ", kv.first.c_str(), kv.second.c_str());
        }
        else if (verb == "set")
        {
            if (w.size() < 4) { Say(ERROR_COLOR, "usage: wxl set <Section> <Key> <value>"); return 1; }
            SetKey(w[1].c_str(), w[2].c_str(), Rest(arguments, 3));
        }
        else if (verb == "toggle")
        {
            if (w.size() < 3) { Say(ERROR_COLOR, "usage: wxl toggle <Section> <Key>"); return 1; }
            const bool now = wxl::ini::GetInt(w[1].c_str(), w[2].c_str(), 0) != 0;
            SetKey(w[1].c_str(), w[2].c_str(), now ? "0" : "1");
        }
        else if (verb == "unset")
        {
            if (w.size() < 3) { Say(ERROR_COLOR, "usage: wxl unset <Section> <Key>"); return 1; }
            if (wxl::ini::Unset(w[1].c_str(), w[2].c_str())) ShowKey(w[1].c_str(), w[2].c_str());
            else Say(WARNING_COLOR, "[%s] %s was not overridden", w[1].c_str(), w[2].c_str());
        }
        else if (verb == "overrides")
        {
            const auto list = wxl::ini::Overrides();
            if (list.empty()) Say(DEFAULT_COLOR, "nothing is overridden: every value is the file's");
            for (const wxl::ini::Entry& e : list) Say(DEFAULT_COLOR, "  [%s] %s = %s", e.section.c_str(), e.key.c_str(), e.value.c_str());
        }
        else if (verb == "reset" || verb == "reload")
        {
            wxl::ini::ClearOverrides();
            Say(HIGHLIGHT_COLOR, "overrides dropped: the file's values apply");
        }
        else if (verb == "save")
        {
            const size_t n = wxl::ini::Overrides().size();
            if (n == 0) Say(DEFAULT_COLOR, "nothing to save");
            else if (wxl::ini::Save()) Say(HIGHLIGHT_COLOR, "%u value(s) written to %s", static_cast<unsigned>(n), wxl::ini::Path());
            else Say(ERROR_COLOR, "could not write %s (nothing was changed; the overrides are still active)", wxl::ini::Path());
        }
        else if (verb == "path")
        {
            Say(DEFAULT_COLOR, "%s   (settings generation %u)", wxl::ini::Path()[0] ? wxl::ini::Path() : "(not found)", static_cast<unsigned>(wxl::ini::Generation()));
        }
        else
        {
            Say(ERROR_COLOR, "wxl: unknown subcommand '%s'. Type `wxl` for the list.", verb.c_str());
        }
        return 1;
    }
}

void WxlConsoleInitialize()
{
    ConsoleCommandRegister("wxl", WxlCommand, CATEGORY_DEBUG, nullptr);
}
