// PngToBlp: every case from make_png_cases.py must decode to the exact pixels, and the BLP2 around them must be
// what the client's loader needs (header, mip chain, sizes/offsets). Build: see run_png_to_blp_tests.sh.
#include "Engine/Source/WXL/Blp.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int g_failed = 0;
static void Check(bool ok, const char* what) { if (!ok) { ++g_failed; std::printf("  FAIL: %s\n", what); } }

static std::vector<uint8_t> Slurp(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static uint32_t Rd(const std::vector<uint8_t>& v, size_t at) { return v[at] | (v[at + 1] << 8) | (v[at + 2] << 16) | (uint32_t(v[at + 3]) << 24); }

int main(int argc, char** argv)
{
    for (int a = 1; a < argc; ++a)
    {
        const std::string png = argv[a], stem = png.substr(0, png.size() - 4);
        const auto bytes = Slurp(png), expect = Slurp(stem + ".rgba");
        unsigned w = 0, h = 0;
        { std::ifstream t(stem + ".txt"); t >> w >> h; }

        std::vector<uint8_t> blp; std::string err;
        const bool ok = wxl::blp::PngToBlp(bytes.data(), bytes.size(), blp, err);
        std::printf("%-22s %ux%u %s\n", png.substr(png.find_last_of("/\\") + 1).c_str(), w, h, ok ? "" : err.c_str());
        Check(ok, "converts");
        if (!ok) continue;

        Check(blp.size() >= 1172 && std::memcmp(blp.data(), "BLP2", 4) == 0 && Rd(blp, 4) == 1, "BLP2 magic, version 1, header room");
        Check(blp[8] == 3 && blp[10] == 2, "colorEncoding 3 (ARGB8888), preferredFormat 2");
        Check(Rd(blp, 12) == w && Rd(blp, 16) == h, "dimensions");
        const bool pot = !(w & (w - 1)) && !(h & (h - 1));
        Check((blp[11] == 1) == pot, "hasMips only for power-of-two sizes");

        // levels: offsets contiguous from 1172, sizes w*h*4 halving, ending 1x1 when mipped
        size_t at = 1172; unsigned lw = w, lh = h; int levels = 0;
        for (int i = 0; i < 16 && Rd(blp, 0x54 + i * 4); ++i)
        {
            Check(Rd(blp, 0x14 + i * 4) == at && Rd(blp, 0x54 + i * 4) == lw * lh * 4, "mip offset/size");
            at += size_t(lw) * lh * 4; ++levels;
            lw = lw > 1 ? lw >> 1 : 1; lh = lh > 1 ? lh >> 1 : 1;
        }
        Check(at == blp.size(), "no bytes beyond the last mip");
        if (pot) { unsigned ew = w, eh = h, n = 1; while (ew > 1 || eh > 1) { ew = ew > 1 ? ew >> 1 : 1; eh = eh > 1 ? eh >> 1 : 1; ++n; } Check(levels == int(n), "full mip chain down to 1x1"); }
        else Check(levels == 1, "single level for a non-power-of-two size");

        // level 0 against the expected pixels (BLP stores B,G,R,A)
        bool same = expect.size() == size_t(w) * h * 4;
        bool anyAlpha = false;
        for (size_t i = 0; same && i < size_t(w) * h; ++i)
        {
            const uint8_t* p = &blp[1172 + i * 4];
            const uint8_t* e = &expect[i * 4];
            if (p[0] != e[2] || p[1] != e[1] || p[2] != e[0] || p[3] != e[3]) same = false;
            if (e[3] != 255) anyAlpha = true;
        }
        Check(same, "level 0 pixels are exactly what the PNG holds");
        Check((blp[9] == 8) == anyAlpha, "alpha bits set exactly when some pixel is not opaque");

        // top of the chain: one pixel, alpha-weighted average of the whole image
        if (pot && levels > 1)
        {
            const size_t lastOff = Rd(blp, 0x14 + (levels - 1) * 4);
            double sumA = 0, c[3] = { 0, 0, 0 };
            for (size_t i = 0; i < size_t(w) * h; ++i) { const double al = expect[i * 4 + 3]; sumA += al; for (int k = 0; k < 3; ++k) c[k] += expect[i * 4 + k] * al; }
            const double meanA = sumA / (double(w) * h);
            Check(std::abs(int(blp[lastOff + 3]) - int(meanA + 0.5)) <= 6, "1x1 mip alpha is the image's mean alpha (box filtered)");
            if (sumA > 0)
                for (int k = 0; k < 3; ++k)
                    Check(std::abs(int(blp[lastOff + 2 - k]) - int(c[k] / sumA + 0.5)) <= 6, "1x1 mip color is the alpha-weighted mean");
        }
    }

    // refusals
    std::vector<uint8_t> out; std::string err;
    const uint8_t junk[40] = { 1, 2, 3 };
    Check(!wxl::blp::PngToBlp(junk, sizeof(junk), out, err) && !err.empty(), "junk is refused with a reason");
    const auto good = Slurp(argc > 1 ? argv[1] : "");
    if (good.size() > 40)
    {
        std::vector<uint8_t> cut(good.begin(), good.begin() + good.size() / 2);
        Check(!wxl::blp::PngToBlp(cut.data(), cut.size(), out, err), "a truncated PNG is refused");
        std::vector<uint8_t> bad = good; bad[good.size() / 2] ^= 0xFF; bad[good.size() / 2 + 1] ^= 0xFF; bad[good.size() / 2 + 2] ^= 0xFF;
        (void)wxl::blp::PngToBlp(bad.data(), bad.size(), out, err);   // damaged data: must not crash, either answer is fine
    }

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
