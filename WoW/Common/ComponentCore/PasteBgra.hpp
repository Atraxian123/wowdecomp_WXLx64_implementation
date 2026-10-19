// Compositor paste routines for 32-bit (BGRA) component layers. Header-only and engine-independent so they can
// be unit tested: callers pass the source mip pointers and destination mip pointers explicitly.
#pragma once
#include <cmath>
#include <cstdint>

namespace pastebgra
{
    inline int ClampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // dst pixel <- src pixel blended by src alpha (if useAlpha), dst alpha forced opaque. Exact /255 rounding.
    inline void Blend(uint8_t* d, const uint8_t* s, bool useAlpha)
    {
        const uint32_t a = useAlpha ? s[3] : 255u;
        if (a == 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
        else if (a != 0)
        {
            for (int c = 0; c < 3; ++c)
            {
                const uint32_t v = d[c] * (255 - a) + s[c] * a + 127;
                d[c] = uint8_t(v / 255);
            }
        }
        d[3] = 0xFF;
    }

    // One level, 1:1 copy of a width x height rectangle. srcRow/dstRow point at the rectangle's first pixel.
    inline void PasteLevel(const uint8_t* srcRow, uint32_t srcStride, uint8_t* dstRow, uint32_t dstStride,
                           uint32_t width, uint32_t height, bool useAlpha)
    {
        for (uint32_t y = 0; y < height; ++y, srcRow += srcStride, dstRow += dstStride)
            for (uint32_t x = 0; x < width; ++x) Blend(dstRow + x * 4, srcRow + x * 4, useAlpha);
    }

    // One level, 2x bilinear upscale (source rectangle srcW x srcH -> dst 2*srcW x 2*srcH), alpha-weighted taps.
    inline void PasteLevelScale2x(const uint8_t* src, uint32_t srcStride, uint32_t srcW, uint32_t srcH,
                                  uint8_t* dstRow, uint32_t dstStride, uint32_t dstW, uint32_t dstH, bool useAlpha)
    {
        for (uint32_t y = 0; y < dstH; ++y, dstRow += dstStride)
        {
            const uint32_t y0 = y >> 1;
            int y1 = int(y0) + ((y & 1) ? 1 : -1);
            y1 = ClampI(y1, 0, int(srcH) - 1);
            for (uint32_t x = 0; x < dstW; ++x)
            {
                const uint32_t x0 = x >> 1;
                int x1 = int(x0) + ((x & 1) ? 1 : -1);
                x1 = ClampI(x1, 0, int(srcW) - 1);
                const uint8_t* t[4] = { src + y0 * srcStride + x0 * 4, src + y0 * srcStride + x1 * 4,
                                        src + y1 * srcStride + x0 * 4, src + y1 * srcStride + x1 * 4 };
                const uint32_t w[4] = { 9, 3, 3, 1 };
                uint32_t acc[3] = {}, aw = 0, asum = 0;
                for (int k = 0; k < 4; ++k)
                {
                    const uint32_t a = useAlpha ? t[k][3] : 255u;
                    for (int c = 0; c < 3; ++c) acc[c] += w[k] * a * t[k][c];
                    aw += w[k] * a; asum += w[k] * a;
                }
                uint8_t px[4] = { 0, 0, 0, uint8_t((asum + 8) / 16) };
                if (aw) for (int c = 0; c < 3; ++c) px[c] = uint8_t((acc[c] + aw / 2) / aw);
                Blend(dstRow + x * 4, px, useAlpha);
            }
        }
    }

    // ---- dynamic layer sizing -------------------------------------------------------------------------
    enum class Mode { Direct, Scale2x, Resample };

    // How a layer of W x H must be fitted into a slot of sw x sh (all in mip-0 pixels).
    //   Direct   : same aspect, power-of-two larger or equal -> existing mip-select paste (mipShift = log2 ratio)
    //   Scale2x  : exactly half the slot in both axes       -> existing 2x upscale paste
    //   Resample : anything else (4x smaller, non power-of-two, odd aspect) -> generic resampler below
    inline Mode Classify(uint32_t W, uint32_t H, uint32_t sw, uint32_t sh, uint32_t& mipShift)
    {
        mipShift = 0;
        if (!W || !H || !sw || !sh) return Mode::Resample;
        if (W >= sw && H >= sh && W % sw == 0 && H % sh == 0 && W / sw == H / sh)
        {
            uint32_t r = W / sw;
            if ((r & (r - 1)) == 0)
            {
                while ((1u << mipShift) < r) ++mipShift;
                return Mode::Direct;
            }
        }
        if (W * 2 == sw && H * 2 == sh) return Mode::Scale2x;
        return Mode::Resample;
    }

    // Bilinear (alpha-weighted) resample of a source rectangle into one destination level.
    //   fetch(x, y, rgba[4]) : straight-alpha texel of the chosen source mip
    //   rx,ry,rw,rh          : source rectangle in that mip's pixels (float)
    //   cx0..cy1             : inclusive clamp box (keeps taps from bleeding in from neighbouring sections)
    template <class Fetch>
    inline void ResampleLevel(Fetch&& fetch, float rx, float ry, float rw, float rh,
                              int cx0, int cy0, int cx1, int cy1,
                              uint8_t* dstRow, uint32_t dstStride, uint32_t tw, uint32_t th, bool useAlpha)
    {
        for (uint32_t y = 0; y < th; ++y, dstRow += dstStride)
        {
            const float v = ry + (y + 0.5f) * rh / th - 0.5f;
            const int fy0 = int(std::floor(v));
            const float ty = v - float(fy0);
            const int ya = ClampI(fy0, cy0, cy1), yb = ClampI(fy0 + 1, cy0, cy1);
            for (uint32_t x = 0; x < tw; ++x)
            {
                const float u = rx + (x + 0.5f) * rw / tw - 0.5f;
                const int fx0 = int(std::floor(u));
                const float tx = u - float(fx0);
                const int xa = ClampI(fx0, cx0, cx1), xb = ClampI(fx0 + 1, cx0, cx1);
                const int xs[4] = { xa, xb, xa, xb }, ys[4] = { ya, ya, yb, yb };
                const float wt[4] = { (1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty };
                float acc[3] = {}, asum = 0.f, aw = 0.f;
                for (int k = 0; k < 4; ++k)
                {
                    uint8_t t[4];
                    fetch(xs[k], ys[k], t);
                    const float a = useAlpha ? float(t[3]) : 255.f;
                    const float wa = wt[k] * a;
                    for (int c = 0; c < 3; ++c) acc[c] += wa * t[c];
                    aw += wa; asum += wa;
                }
                uint8_t px[4] = { 0, 0, 0, uint8_t(asum + 0.5f > 255.f ? 255.f : asum + 0.5f) };
                if (aw > 0.f) for (int c = 0; c < 3; ++c) px[c] = uint8_t(acc[c] / aw + 0.5f);
                Blend(dstRow + x * 4, px, useAlpha);
            }
        }
    }
}
