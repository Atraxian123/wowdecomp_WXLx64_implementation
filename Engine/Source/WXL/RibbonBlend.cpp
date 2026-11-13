// How the textures of a multi-textured ribbon emitter combine. See RibbonBlend.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)

#include "Engine/Source/WXL/RibbonBlend.h"

#include "Engine/Source/WXL/Ini.h"
#include "Engine/Source/WXL/Log.h"

#include <atomic>
#include <mutex>

namespace wxl::ribbonblend
{
    namespace
    {
        // The text of a [Ribbons] key, through the shared ini reader: found in the WXL folder or beside the executable,
        // and seeing a console override (`wxl set Ribbons MultiTextureBlend Add`) at once. Missing: the default.
        std::string IniString(const char* key, const char* def)
        {
            return wxl::ini::GetText("Ribbons", key, def);
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

    namespace
    {
        struct Settings
        {
            Mode mode = Mode::Off;
            int  gain = 2;
        };

        // The two settings as the ini has them now. Re-read when the ini changes (a save, or a console override); a
        // ribbon takes the mode when its model loads, so a change applies to models loaded afterwards. The old Settings are
        // never freed: a thread that fetched them a moment ago may still be reading them.
        const Settings& Current()
        {
            static std::atomic<const Settings*> current{nullptr};
            static std::atomic<uint32_t> seen{0xFFFFFFFFu};
            static std::mutex m;
            const uint32_t gen = wxl::ini::Generation();
            if (seen.load(std::memory_order_acquire) != gen || !current.load(std::memory_order_acquire))
            {
                std::lock_guard<std::mutex> lock(m);
                if (seen.load(std::memory_order_relaxed) != gen || !current.load(std::memory_order_relaxed))
                {
                    auto* s = new Settings();
                    const std::string modeText = IniString("MultiTextureBlend", "");
                    bool ok = true;
                    s->mode = ParseMode(modeText, &ok);
                    if (!ok) wxl::LogCat("RIBBON", "[Ribbons] MultiTextureBlend='%s' is not Off, Multiply or Add; Off", modeText.c_str());

                    const std::string gainText = IniString("MultiTextureGain", "");
                    ok = true;
                    s->gain = ParseGain(gainText, &ok);
                    if (!ok) wxl::LogCat("RIBBON", "[Ribbons] MultiTextureGain='%s' is not 1, 2 or 4; 2", gainText.c_str());

                    const Settings* previous = current.load(std::memory_order_relaxed);
                    if (!previous || previous->mode != s->mode)
                        wxl::LogCat("RIBBON", "multi-textured ribbons: %s", s->mode == Mode::Multiply ? "multiply" : s->mode == Mode::Add ? "add" : "off (as the model has it)");

                    current.store(s, std::memory_order_release);
                    seen.store(gen, std::memory_order_release);
                }
            }
            return *current.load(std::memory_order_acquire);
        }
    }

    Mode MultiTextureBlend() { return Current().mode; }

    int MultiTextureGain() { return Current().gain; }
}
