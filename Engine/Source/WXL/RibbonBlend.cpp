// How the textures of a multi-textured ribbon emitter combine. See RibbonBlend.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)

#include "Engine/Source/WXL/RibbonBlend.h"

#include "Engine/Source/WXL/Log.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdio>
#include <cstring>

namespace wxl::ribbonblend
{
    namespace
    {
        // WXLSetup.ini sits in the WXL folder beside the client executable; a copy left beside the executable (the
        // old place) is still read. The lookup is repeated here because WXLCore keeps no shared ini helper, as in
        // ParticleCsv.cpp and Culling.cpp. Missing file, section or key: the default.
        std::string IniString(const char* section, const char* key, const char* def)
        {
#ifdef _WIN32
            char path[MAX_PATH] = {};
            const DWORD n = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(sizeof(path)));
            if (n == 0 || n >= sizeof(path)) return def;
            char* slash = std::strrchr(path, '\\');
            const char* name = "WXLSetup.ini";
            if (!slash || static_cast<size_t>(slash - path) + 1 + std::strlen(name) >= sizeof(path)) return def;
            std::strcpy(slash + 1, name);
            char inWxl[MAX_PATH] = {};
            if (static_cast<size_t>(slash - path) + 5 + std::strlen(name) < sizeof(inWxl))
            {
                std::snprintf(inWxl, sizeof(inWxl), "%.*sWXL\\%s", static_cast<int>(slash - path) + 1, path, name);
                if (GetFileAttributesA(inWxl) != INVALID_FILE_ATTRIBUTES) std::strcpy(path, inWxl);
            }
            char value[64] = {};
            GetPrivateProfileStringA(section, key, def, value, static_cast<DWORD>(sizeof(value)), path);
            return value;
#else
            (void)section; (void)key;
            return def;
#endif
        }

        std::string Normalize(const std::string& s)
        {
            size_t b = 0, e = s.size();
            while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
            while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
            std::string out = s.substr(b, e - b);
            for (char& c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            return out;
        }
    }

    Mode ParseMode(const std::string& text, bool* recognized)
    {
        const std::string t = Normalize(text);
        if (recognized) *recognized = true;
        if (t.empty() || t == "off" || t == "0" || t == "no" || t == "none" || t == "false") return Mode::Off;
        if (t == "multiply" || t == "multiplicative" || t == "mul") return Mode::Multiply;
        if (t == "add" || t == "additive") return Mode::Add;
        if (recognized) *recognized = false;
        return Mode::Off;
    }

    int ParseGain(const std::string& text, bool* recognized)
    {
        const std::string t = Normalize(text);
        if (recognized) *recognized = true;
        if (t.empty()) return 2;
        if (t == "1") return 1;
        if (t == "2") return 2;
        if (t == "4") return 4;
        if (recognized) *recognized = false;
        return 2;
    }

    void StageOps(uint32_t layers, int colorGain, bool plainAlpha, StageOp* out)
    {
        const uint32_t doubling = colorGain >= 4 ? 2u : colorGain == 2 ? 1u : 0u;   // how many of the last stages double the colour
        for (uint32_t k = 0; k < layers; ++k)
        {
            out[k].colorDouble = k + doubling >= layers;
            out[k].alphaDouble = !plainAlpha && out[k].colorDouble;
        }
    }

    Mode MultiTextureBlend()
    {
        static const Mode mode = [] {
            const std::string text = IniString("Ribbons", "MultiTextureBlend", "");
            bool ok = true;
            const Mode m = ParseMode(text, &ok);
            if (!ok) wxl::LogCat("RIBBON", "[Ribbons] MultiTextureBlend='%s' is not Off, Multiply or Add; Off", text.c_str());
            wxl::LogCat("RIBBON", "multi-textured ribbons: %s", m == Mode::Multiply ? "multiply" : m == Mode::Add ? "add" : "off (as the model has it)");
            return m;
        }();
        return mode;
    }

    int MultiTextureGain()
    {
        static const int gain = [] {
            const std::string text = IniString("Ribbons", "MultiTextureGain", "");
            bool ok = true;
            const int g = ParseGain(text, &ok);
            if (!ok) wxl::LogCat("RIBBON", "[Ribbons] MultiTextureGain='%s' is not 1, 2 or 4; 2", text.c_str());
            return g;
        }();
        return gain;
    }
}
