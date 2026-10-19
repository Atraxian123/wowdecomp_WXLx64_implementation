// BlpDecode.hpp - decode any BLP2 (palettized / DXT1,3,5 / BGRA) to RGBA8 at a requested size.
// Intended use: feed the character-skin compositor, so layers no longer have to be palettized.
// Header-only, no dependencies beyond the STL. Builds as C++17 (the project standard): it uses its own
// ByteSpan instead of C++20's std::span, which is unavailable there and broke the WoW/Common build.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace blpdec
{
    // Minimal read-only (pointer, size) view; the only members the decoder needs. C++17 stand-in for std::span.
    struct ByteSpan
    {
        const uint8_t* ptr;
        size_t         len;
        constexpr ByteSpan(const uint8_t* p, size_t n) noexcept : ptr(p), len(n) {}
        constexpr const uint8_t* data() const noexcept { return ptr; }
        constexpr size_t size() const noexcept { return len; }
        constexpr const uint8_t& operator[](size_t i) const noexcept { return ptr[i]; }
    };

    struct Rgba
    {
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> px; // R,G,B,A (non-premultiplied), row-major
    };

    namespace detail
    {
        inline uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
        inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
        inline uint32_t mx(uint32_t a, uint32_t b) { return a > b ? a : b; }

        // 565 -> 888 with bit replication (matches GPU decode better than the <<3 truncation).
        inline void c565(uint16_t c, int& r, int& g, int& b)
        {
            r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31;
            r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
        }

        // colorBlock = 8 bytes (c0,c1,indices). opaque4 = force 4-colour mode (DXT3/5, or DXT1 w/o alpha).
        inline void DecodeColor(const uint8_t* cb, bool opaque4, uint8_t out[16][4])
        {
            const uint16_t c0 = rd16(cb), c1 = rd16(cb + 2);
            int p[4][4];
            c565(c0, p[0][0], p[0][1], p[0][2]); p[0][3] = 255;
            c565(c1, p[1][0], p[1][1], p[1][2]); p[1][3] = 255;
            if (opaque4 || c0 > c1)
            {
                for (int k = 0; k < 3; ++k)
                {
                    p[2][k] = (2 * p[0][k] + p[1][k]) / 3;
                    p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
                }
                p[2][3] = p[3][3] = 255;
            }
            else
            {
                for (int k = 0; k < 3; ++k) p[2][k] = (p[0][k] + p[1][k]) / 2;
                p[2][3] = 255;
                p[3][0] = p[3][1] = p[3][2] = p[3][3] = 0;
            }
            const uint32_t idx = rd32(cb + 4);
            for (int i = 0; i < 16; ++i)
                for (int k = 0; k < 4; ++k) out[i][k] = uint8_t(p[(idx >> (2 * i)) & 3][k]);
        }

        inline void DecodeAlphaBC3(const uint8_t* ab, uint8_t out[16])
        {
            uint8_t a[8];
            a[0] = ab[0]; a[1] = ab[1];
            if (a[0] > a[1]) for (int i = 1; i < 7; ++i) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
            else { for (int i = 1; i < 5; ++i) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5); a[6] = 0; a[7] = 255; }
            uint64_t bits = 0;
            for (int i = 0; i < 6; ++i) bits |= uint64_t(ab[2 + i]) << (8 * i);
            for (int i = 0; i < 16; ++i) out[i] = a[(bits >> (3 * i)) & 7];
        }

        // Decode one specific mip level to RGBA. Returns false on any bounds problem.
        inline bool DecodeMip(ByteSpan in, uint32_t mip, Rgba& img)
        {
            const uint8_t* b = in.data();
            const uint32_t n = uint32_t(in.size());
            const uint8_t enc = b[8], alphaDepth = b[9], alphaType = b[10];
            const uint32_t W = rd32(b + 0x0C), H = rd32(b + 0x10);
            const uint32_t w = mx(1, W >> mip), h = mx(1, H >> mip);
            const uint32_t off = rd32(b + 0x14 + mip * 4), size = rd32(b + 0x54 + mip * 4);
            if (!off || !size || off > n || size > n - off) return false;
            const uint8_t* s = b + off;
            img.w = w; img.h = h; img.px.assign(size_t(w) * h * 4, 255);
            const size_t np = size_t(w) * h;

            if (enc == 3) // BGRA
            {
                if (size < np * 4) return false;
                for (size_t i = 0; i < np; ++i)
                {
                    img.px[i * 4 + 0] = s[i * 4 + 2]; img.px[i * 4 + 1] = s[i * 4 + 1];
                    img.px[i * 4 + 2] = s[i * 4 + 0]; img.px[i * 4 + 3] = alphaDepth ? s[i * 4 + 3] : 255;
                }
                return true;
            }
            if (enc == 1) // palettized: header(0x94) + 256*BGRA palette, then index plane, then alpha plane
            {
                if (n < 0x94 + 1024 || size < np) return false;
                const uint8_t* pal = b + 0x94;
                const uint8_t* ap = s + np;
                const size_t alphaBytes = alphaDepth == 8 ? np : alphaDepth == 4 ? (np + 1) / 2 : alphaDepth == 1 ? (np + 7) / 8 : 0;
                if (size < np + alphaBytes) return false;
                for (size_t i = 0; i < np; ++i)
                {
                    const uint8_t* c = pal + s[i] * 4;
                    uint8_t a = 255;
                    if (alphaDepth == 8) a = ap[i];
                    else if (alphaDepth == 4) a = uint8_t(((i & 1) ? (ap[i / 2] >> 4) : (ap[i / 2] & 15)) * 17);
                    else if (alphaDepth == 1) a = ((ap[i / 8] >> (i & 7)) & 1) ? 255 : 0;
                    img.px[i * 4 + 0] = c[2]; img.px[i * 4 + 1] = c[1]; img.px[i * 4 + 2] = c[0]; img.px[i * 4 + 3] = a;
                }
                return true;
            }
            if (enc == 2) // DXT
            {
                if (alphaType != 0 && alphaType != 1 && alphaType != 7) return false;
                const uint32_t bb = alphaType == 0 ? 8 : 16, bx = (w + 3) / 4, by = (h + 3) / 4;
                if (uint64_t(bx) * by * bb > size) return false;
                for (uint32_t ty = 0; ty < by; ++ty)
                    for (uint32_t tx = 0; tx < bx; ++tx)
                    {
                        const uint8_t* blk = s + (size_t(ty) * bx + tx) * bb;
                        uint8_t col[16][4], al[16];
                        std::fill(al, al + 16, uint8_t(255));
                        if (alphaType == 0) DecodeColor(blk, alphaDepth == 0, col);
                        else
                        {
                            if (alphaType == 1)
                                for (int i = 0; i < 16; ++i) al[i] = uint8_t(((i & 1) ? (blk[i / 2] >> 4) : (blk[i / 2] & 15)) * 17);
                            else DecodeAlphaBC3(blk, al);
                            DecodeColor(blk + 8, true, col);
                        }
                        for (uint32_t py = 0; py < 4; ++py)
                            for (uint32_t px = 0; px < 4; ++px)
                            {
                                const uint32_t x = tx * 4 + px, y = ty * 4 + py;
                                if (x >= w || y >= h) continue;
                                const int i = py * 4 + px;
                                uint8_t* d = &img.px[(size_t(y) * w + x) * 4];
                                d[0] = col[i][0]; d[1] = col[i][1]; d[2] = col[i][2];
                                d[3] = alphaType == 0 ? col[i][3] : al[i];
                            }
                    }
                return true;
            }
            return false;
        }
    }

    // Alpha-weighted bilinear resample (no dark/bright fringes where alpha varies). Works for up and down.
    inline void Resize(const Rgba& s, uint32_t dw, uint32_t dh, Rgba& d)
    {
        d.w = dw; d.h = dh; d.px.assign(size_t(dw) * dh * 4, 0);
        for (uint32_t y = 0; y < dh; ++y)
        {
            const float fy = std::max(0.f, (y + 0.5f) * s.h / dh - 0.5f);
            const uint32_t y0 = std::min(s.h - 1, uint32_t(fy)), y1 = std::min(s.h - 1, y0 + 1);
            const float ty = fy - y0;
            for (uint32_t x = 0; x < dw; ++x)
            {
                const float fx = std::max(0.f, (x + 0.5f) * s.w / dw - 0.5f);
                const uint32_t x0 = std::min(s.w - 1, uint32_t(fx)), x1 = std::min(s.w - 1, x0 + 1);
                const float tx = fx - x0;
                const uint32_t xs[4] = { x0, x1, x0, x1 }, ys[4] = { y0, y0, y1, y1 };
                const float wt[4] = { (1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty };
                float acc[3] = {}, aSum = 0;
                for (int k = 0; k < 4; ++k)
                {
                    const uint8_t* p = &s.px[(size_t(ys[k]) * s.w + xs[k]) * 4];
                    const float wa = wt[k] * p[3];
                    acc[0] += wa * p[0]; acc[1] += wa * p[1]; acc[2] += wa * p[2]; aSum += wa;
                }
                uint8_t* o = &d.px[(size_t(y) * dw + x) * 4];
                if (aSum > 0.f) for (int k = 0; k < 3; ++k) o[k] = uint8_t(std::min(255.f, acc[k] / aSum + 0.5f));
                o[3] = uint8_t(std::min(255.f, aSum + 0.5f));
            }
        }
    }

    // Decode `in` and return it at exactly dstW x dstH. Picks the smallest stored mip that is still >= the
    // target on both axes (so a 1024 layer going into a 256 slot is just read from its 256 mip: no real
    // downscale), then does a final resample only if the sizes still differ (non-matching aspect, or a
    // source smaller than the slot -> upscale).
    inline bool DecodeBlp2ToSize(ByteSpan in, uint32_t dstW, uint32_t dstH, Rgba& out)
    {
        using namespace detail;
        if (in.size() < 0x94 || rd32(in.data()) != 0x32504C42 || rd32(in.data() + 4) != 1) return false;
        const uint32_t W = rd32(in.data() + 0x0C), H = rd32(in.data() + 0x10);
        if (!W || !H || W > 8192 || H > 8192 || !dstW || !dstH) return false;

        uint32_t mip = 0;
        while (mip < 15 && (W >> (mip + 1)) >= dstW && (H >> (mip + 1)) >= dstH && (W >> (mip + 1)) && (H >> (mip + 1))) ++mip;

        Rgba img;
        if (!DecodeMip(in, mip, img))
        {
            if (mip == 0 || !DecodeMip(in, 0, img)) return false; // fall back to the full-size level
        }
        if (img.w == dstW && img.h == dstH) { out = std::move(img); return true; }
        Resize(img, dstW, dstH, out);
        return true;
    }
}

namespace blpdec
{
    // Rewrites a whole BLP2 (palettized / DXT / BGRA) as a BLP2 encoding-3 file (BGRA8888) with a complete
    // mip chain, keeping width/height/alphaDepth, so the character compositor can read every layer the
    // same way. Levels the source lacks are box-filtered from the previous level. The 0x494-byte header
    // (palette area zeroed) matches BLPHeader, so the client's own parsing works unchanged.
    inline bool ConvertToBgraBlp(ByteSpan in, std::vector<uint8_t>& out)
    {
        using namespace detail;
        if (in.size() < 0x94 || rd32(in.data()) != 0x32504C42 || rd32(in.data() + 4) != 1) return false;
        const uint8_t enc = in[8];
        if (enc != 1 && enc != 2 && enc != 3) return false;
        const uint32_t W = rd32(in.data() + 0x0C), H = rd32(in.data() + 0x10);
        if (!W || !H || W > 8192 || H > 8192) return false;

        uint32_t levels = 1;
        for (uint32_t w = W, h = H; w > 1 || h > 1; ++levels) { w = mx(1, w >> 1); h = mx(1, h >> 1); }
        if (levels > 16) return false;

        std::vector<Rgba> mips(levels);
        if (!DecodeMip(in, 0, mips[0])) return false;
        for (uint32_t m = 1; m < levels; ++m)
        {
            if (DecodeMip(in, m, mips[m]) && mips[m].w == mx(1, W >> m) && mips[m].h == mx(1, H >> m)) continue;
            const Rgba& s = mips[m - 1];
            Rgba& d = mips[m];
            d.w = mx(1, s.w >> 1); d.h = mx(1, s.h >> 1); d.px.assign(size_t(d.w) * d.h * 4, 0);
            for (uint32_t y = 0; y < d.h; ++y)
                for (uint32_t x = 0; x < d.w; ++x)
                {
                    uint32_t acc[4] = {};
                    for (uint32_t k = 0; k < 4; ++k)
                    {
                        const uint32_t sx = std::min(s.w - 1, x * 2 + (k & 1)), sy = std::min(s.h - 1, y * 2 + (k >> 1));
                        const uint8_t* p = &s.px[(size_t(sy) * s.w + sx) * 4];
                        for (int c = 0; c < 4; ++c) acc[c] += p[c];
                    }
                    for (int c = 0; c < 4; ++c) d.px[(size_t(y) * d.w + x) * 4 + c] = uint8_t((acc[c] + 2) / 4);
                }
        }

        out.assign(0x94 + 1024, 0);
        std::memcpy(out.data(), in.data(), 0x08);          // magic + version
        out[8] = 3;                                         // uncompressed BGRA
        out[9] = in[9];                                     // alpha depth as authored (0 => treated opaque)
        out[10] = in[9] ? 8 : 0;
        out[11] = levels > 1 ? 1 : 0;
        std::memcpy(out.data() + 0x0C, in.data() + 0x0C, 8); // width, height
        for (uint32_t m = 0; m < levels; ++m)
        {
            const Rgba& r = mips[m];
            const uint32_t off = uint32_t(out.size());
            out.resize(out.size() + size_t(r.w) * r.h * 4);
            uint8_t* d = out.data() + off;
            for (size_t i = 0; i < size_t(r.w) * r.h; ++i)
            {
                d[i * 4 + 0] = r.px[i * 4 + 2]; d[i * 4 + 1] = r.px[i * 4 + 1];
                d[i * 4 + 2] = r.px[i * 4 + 0]; d[i * 4 + 3] = r.px[i * 4 + 3];
            }
            const uint32_t sz = uint32_t(size_t(r.w) * r.h * 4);
            for (int b = 0; b < 4; ++b) { out[0x14 + m * 4 + b] = uint8_t(off >> (8 * b)); out[0x54 + m * 4 + b] = uint8_t(sz >> (8 * b)); }
        }
        return true;
    }
}
