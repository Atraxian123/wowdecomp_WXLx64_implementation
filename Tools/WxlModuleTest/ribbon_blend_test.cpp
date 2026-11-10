// Multi-textured ribbons: the [Ribbons] setting values and the stage ops of the multiplied pass.
#include "Engine/Source/WXL/RibbonBlend.h"

#include <cstdio>

using namespace wxl::ribbonblend;

static int g_failed = 0;
static void Check(bool ok, const char* what) { if (!ok) { ++g_failed; std::printf("  FAIL: %s\n", what); } }

static int Doubled(const StageOp* ops, uint32_t n, bool alpha)
{
    int d = 0;
    for (uint32_t k = 0; k < n; ++k) d += (alpha ? ops[k].alphaDouble : ops[k].colorDouble) ? 1 : 0;
    return d;
}

int main()
{
    std::printf("1. MultiTextureBlend values\n");
    {
        bool ok = false;
        Check(ParseMode("", &ok) == Mode::Off && ok, "empty: Off");
        Check(ParseMode("Off", &ok) == Mode::Off && ok && ParseMode("0", &ok) == Mode::Off && ok && ParseMode(" no ", &ok) == Mode::Off && ok, "Off, 0, no");
        Check(ParseMode("Multiply", &ok) == Mode::Multiply && ok && ParseMode("MULTIPLICATIVE", &ok) == Mode::Multiply && ok, "Multiply in any case");
        Check(ParseMode("add", &ok) == Mode::Add && ok && ParseMode("  Additive\r", &ok) == Mode::Add && ok, "Add, with spaces and a stray CR");
        Check(ParseMode("screen", &ok) == Mode::Off && !ok, "unknown: Off, not recognized");
        Check(ParseMode("multiply") == Mode::Multiply, "works without the flag too");
    }
    std::printf("2. MultiTextureGain values\n");
    {
        bool ok = false;
        Check(ParseGain("", &ok) == 2 && ok, "empty: 2");
        Check(ParseGain("1", &ok) == 1 && ok && ParseGain("2", &ok) == 2 && ok && ParseGain(" 4 ", &ok) == 4 && ok, "1, 2, 4");
        Check(ParseGain("3", &ok) == 2 && !ok && ParseGain("two", &ok) == 2 && !ok, "anything else: 2, not recognized");
    }
    std::printf("3. stage ops\n");
    {
        StageOp ops[4];
        for (uint32_t layers = 2; layers <= 4; ++layers)
        {
            StageOps(layers, 1, true, ops);
            Check(Doubled(ops, layers, false) == 0 && Doubled(ops, layers, true) == 0, "gain 1: nothing doubles");
            StageOps(layers, 2, true, ops);
            Check(Doubled(ops, layers, false) == 1 && ops[layers - 1].colorDouble && Doubled(ops, layers, true) == 0, "gain 2: the last stage doubles the colour, alpha stays plain");
            StageOps(layers, 4, true, ops);
            Check(Doubled(ops, layers, false) == 2 && ops[layers - 1].colorDouble && ops[layers - 2].colorDouble && Doubled(ops, layers, true) == 0, "gain 4: the last two double the colour, alpha stays plain");
            StageOps(layers, 4, false, ops);
            Check(Doubled(ops, layers, false) == 2 && Doubled(ops, layers, true) == 2, "the stock fold: colour and alpha double on the last two stages, as before");
        }
        StageOps(2, 4, false, ops);
        Check(ops[0].colorDouble && ops[1].colorDouble && ops[0].alphaDouble && ops[1].alphaDouble, "two layers, stock fold: both stages double, the x4 it always had");
    }
    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
