// PNG -> BLP2 conversion, so a texture that has a .png but no .blp can still be loaded by the client.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
//
// The client's own texture loader reads the BLP this produces: uncompressed ARGB8888 (colorEncoding 3) with
// preferredFormat ARGB8888, so no format conversion or compression is involved. Decoding is plain PNG:
// chunk walk, zlib inflate (the tree's own zlib), scanline unfiltering, optional Adam7 de-interlacing.

#include "Engine/Source/WXL/Blp.h"
#include "ThirdParty/zlib/zlib.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint32_t kMaxPixels = 16u * 1024u * 1024u;
    constexpr size_t   kBlpHeaderSize = 0x94 + 1024;   // header + (unused) palette: where the first mip starts

    uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

    void Put32(std::vector<uint8_t>& v, size_t at, uint32_t x)
    {
        v[at] = uint8_t(x); v[at + 1] = uint8_t(x >> 8); v[at + 2] = uint8_t(x >> 16); v[at + 3] = uint8_t(x >> 24);
    }

    bool IsPow2(uint32_t v) { return v && !(v & (v - 1)); }

    struct Header
    {
        uint32_t width = 0, height = 0;
        uint8_t  depth = 0, colorType = 0, interlace = 0;
        uint32_t channels = 0, bitsPerPixel = 0;
    };

    struct Pass { uint32_t x0, y0, dx, dy, w, h; size_t rowBytes; size_t offset; };

    size_t RowBytes(const Header& h, uint32_t w) { return (size_t(w) * h.bitsPerPixel + 7) / 8; }

    int Paeth(int a, int b, int c)
    {
        const int p = a + b - c;
        const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
        if (pa <= pb && pa <= pc) return a;
        return pb <= pc ? b : c;
    }

    // `rows` scanlines of (1 filter byte + rowBytes) at `data`, unfiltered in place. false on a bad filter type.
    bool Unfilter(uint8_t* data, size_t rowBytes, size_t bpp, uint32_t rows)
    {
        const size_t stride = rowBytes + 1;
        for (uint32_t y = 0; y < rows; ++y)
        {
            uint8_t* row = data + y * stride;
            const uint8_t type = row[0];
            uint8_t* cur = row + 1;
            const uint8_t* prev = y ? data + (y - 1) * stride + 1 : nullptr;
            switch (type)
            {
            case 0: break;
            case 1: for (size_t i = bpp; i < rowBytes; ++i) cur[i] = uint8_t(cur[i] + cur[i - bpp]); break;
            case 2: if (prev) for (size_t i = 0; i < rowBytes; ++i) cur[i] = uint8_t(cur[i] + prev[i]); break;
            case 3:
                for (size_t i = 0; i < rowBytes; ++i)
                {
                    const int left = i >= bpp ? cur[i - bpp] : 0, up = prev ? prev[i] : 0;
                    cur[i] = uint8_t(cur[i] + ((left + up) >> 1));
                }
                break;
            case 4:
                for (size_t i = 0; i < rowBytes; ++i)
                {
                    const int left = i >= bpp ? cur[i - bpp] : 0, up = prev ? prev[i] : 0, ul = (prev && i >= bpp) ? prev[i - bpp] : 0;
                    cur[i] = uint8_t(cur[i] + Paeth(left, up, ul));
                }
                break;
            default: return false;
            }
        }
        return true;
    }

    // Sample `index` (0-based, in samples) of a scanline, at the file's bit depth; 16-bit samples as 16 bits.
    uint32_t Sample(const uint8_t* row, uint32_t index, uint8_t depth)
    {
        switch (depth)
        {
        case 8:  return row[index];
        case 16: return (uint32_t(row[index * 2]) << 8) | row[index * 2 + 1];
        default:
        {
            const uint32_t bit = index * depth;
            const uint32_t shift = 8 - depth - (bit & 7);
            return (row[bit >> 3] >> shift) & ((1u << depth) - 1);
        }
        }
    }
}

namespace wxl::blp
{
    bool PngToBlp(const uint8_t* png, size_t size, std::vector<uint8_t>& blp, std::string& error)
    {
        static const uint8_t kSignature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
        if (!png || size < 8 + 25 || std::memcmp(png, kSignature, 8) != 0) { error = "not a PNG file"; return false; }

        // ---- chunks ----
        Header hd;
        bool haveHeader = false, sawEnd = false;
        uint8_t palette[256][3] = {};
        uint8_t paletteAlpha[256];
        std::memset(paletteAlpha, 255, sizeof(paletteAlpha));
        uint32_t paletteCount = 0;
        bool haveTrns = false;
        uint32_t trnsKey[3] = {};          // gray: [0]; rgb: r, g, b (as stored, 16-bit)
        std::vector<uint8_t> idat;

        size_t pos = 8;
        while (pos + 12 <= size && !sawEnd)
        {
            const uint32_t len = Be32(png + pos);
            const char* type = reinterpret_cast<const char*>(png + pos + 4);
            if (len > size - pos - 12) { error = "truncated PNG chunk"; return false; }
            const uint8_t* body = png + pos + 8;

            if (!std::memcmp(type, "IHDR", 4))
            {
                if (len != 13) { error = "bad IHDR"; return false; }
                hd.width = Be32(body); hd.height = Be32(body + 4);
                hd.depth = body[8]; hd.colorType = body[9]; hd.interlace = body[12];
                if (body[10] != 0 || body[11] != 0 || hd.interlace > 1) { error = "unsupported PNG compression/filter/interlace method"; return false; }
                if (!hd.width || !hd.height || uint64_t(hd.width) * hd.height > kMaxPixels) { error = "image size is zero or above 16 million pixels"; return false; }
                switch (hd.colorType)
                {
                case 0: hd.channels = 1; break;
                case 2: hd.channels = 3; break;
                case 3: hd.channels = 1; break;
                case 4: hd.channels = 2; break;
                case 6: hd.channels = 4; break;
                default: error = "bad PNG color type"; return false;
                }
                const bool depthOk = hd.colorType == 0 ? (hd.depth == 1 || hd.depth == 2 || hd.depth == 4 || hd.depth == 8 || hd.depth == 16)
                                   : hd.colorType == 3 ? (hd.depth == 1 || hd.depth == 2 || hd.depth == 4 || hd.depth == 8)
                                   : (hd.depth == 8 || hd.depth == 16);
                if (!depthOk) { error = "bad PNG bit depth for its color type"; return false; }
                hd.bitsPerPixel = hd.channels * hd.depth;
                haveHeader = true;
            }
            else if (!std::memcmp(type, "PLTE", 4))
            {
                if (len % 3 || len > 768 || len == 0) { error = "bad PLTE"; return false; }
                paletteCount = len / 3;
                for (uint32_t i = 0; i < paletteCount; ++i) { palette[i][0] = body[i * 3]; palette[i][1] = body[i * 3 + 1]; palette[i][2] = body[i * 3 + 2]; }
            }
            else if (!std::memcmp(type, "tRNS", 4) && haveHeader)
            {
                if (hd.colorType == 3) { for (uint32_t i = 0; i < len && i < 256; ++i) paletteAlpha[i] = body[i]; haveTrns = true; }
                else if (hd.colorType == 0 && len >= 2) { trnsKey[0] = (uint32_t(body[0]) << 8) | body[1]; haveTrns = true; }
                else if (hd.colorType == 2 && len >= 6) { for (int i = 0; i < 3; ++i) trnsKey[i] = (uint32_t(body[i * 2]) << 8) | body[i * 2 + 1]; haveTrns = true; }
            }
            else if (!std::memcmp(type, "IDAT", 4))
            {
                idat.insert(idat.end(), body, body + len);
            }
            else if (!std::memcmp(type, "IEND", 4))
            {
                sawEnd = true;
            }
            pos += 12 + size_t(len);
        }
        if (!haveHeader) { error = "PNG has no IHDR"; return false; }
        if (idat.empty()) { error = "PNG has no image data"; return false; }
        if (hd.colorType == 3 && !paletteCount) { error = "palette PNG without a PLTE"; return false; }

        // ---- pass layout (one pass, or Adam7's seven) ----
        std::vector<Pass> passes;
        size_t rawSize = 0;
        if (!hd.interlace)
        {
            Pass p{ 0, 0, 1, 1, hd.width, hd.height, RowBytes(hd, hd.width), 0 };
            passes.push_back(p);
            rawSize = size_t(hd.height) * (p.rowBytes + 1);
        }
        else
        {
            static const uint32_t X0[7] = { 0, 4, 0, 2, 0, 1, 0 }, Y0[7] = { 0, 0, 4, 0, 2, 0, 1 };
            static const uint32_t DX[7] = { 8, 8, 4, 4, 2, 2, 1 }, DY[7] = { 8, 8, 8, 4, 4, 2, 2 };
            for (int i = 0; i < 7; ++i)
            {
                if (hd.width <= X0[i] || hd.height <= Y0[i]) continue;
                Pass p{ X0[i], Y0[i], DX[i], DY[i], (hd.width - X0[i] + DX[i] - 1) / DX[i], (hd.height - Y0[i] + DY[i] - 1) / DY[i], 0, rawSize };
                p.rowBytes = RowBytes(hd, p.w);
                rawSize += size_t(p.h) * (p.rowBytes + 1);
                passes.push_back(p);
            }
        }

        // ---- inflate ----
        std::vector<uint8_t> raw(rawSize);
        uLongf rawLen = static_cast<uLongf>(rawSize);
        const int zr = uncompress(raw.data(), &rawLen, idat.data(), static_cast<uLong>(idat.size()));
        if (zr != Z_OK || rawLen != rawSize) { error = "PNG image data is damaged or truncated"; return false; }

        // ---- unfilter + convert to BGRA ----
        const uint32_t W = hd.width, H = hd.height;
        std::vector<uint8_t> level0(size_t(W) * H * 4);
        const size_t bpp = std::max<size_t>(1, hd.bitsPerPixel / 8);
        bool anyAlpha = false;
        const bool wide = hd.depth == 16;
        const uint32_t maxVal = hd.depth < 8 ? (1u << hd.depth) - 1 : 255;

        for (const Pass& p : passes)
        {
            uint8_t* base = raw.data() + p.offset;
            if (!Unfilter(base, p.rowBytes, bpp, p.h)) { error = "bad PNG scanline filter"; return false; }
            for (uint32_t py = 0; py < p.h; ++py)
            {
                const uint8_t* row = base + size_t(py) * (p.rowBytes + 1) + 1;
                const uint32_t y = p.y0 + py * p.dy;
                for (uint32_t px = 0; px < p.w; ++px)
                {
                    const uint32_t x = p.x0 + px * p.dx;
                    uint32_t r, g, b, a = 255;
                    switch (hd.colorType)
                    {
                    case 0:
                    {
                        const uint32_t v = Sample(row, px, hd.depth);
                        if (haveTrns && v == trnsKey[0]) a = 0;
                        r = g = b = wide ? v >> 8 : (hd.depth < 8 ? v * 255 / maxVal : v);
                        break;
                    }
                    case 2:
                    {
                        const uint32_t sr = Sample(row, px * 3, hd.depth), sg = Sample(row, px * 3 + 1, hd.depth), sb = Sample(row, px * 3 + 2, hd.depth);
                        if (haveTrns && sr == trnsKey[0] && sg == trnsKey[1] && sb == trnsKey[2]) a = 0;
                        r = wide ? sr >> 8 : sr; g = wide ? sg >> 8 : sg; b = wide ? sb >> 8 : sb;
                        break;
                    }
                    case 3:
                    {
                        const uint32_t idx = Sample(row, px, hd.depth);
                        if (idx >= paletteCount) { error = "palette index outside the PLTE"; return false; }
                        r = palette[idx][0]; g = palette[idx][1]; b = palette[idx][2];
                        a = paletteAlpha[idx];
                        break;
                    }
                    case 4:
                    {
                        const uint32_t v = Sample(row, px * 2, hd.depth), sa = Sample(row, px * 2 + 1, hd.depth);
                        r = g = b = wide ? v >> 8 : v;
                        a = wide ? sa >> 8 : sa;
                        break;
                    }
                    default: // 6
                    {
                        const uint32_t sr = Sample(row, px * 4, hd.depth), sg = Sample(row, px * 4 + 1, hd.depth),
                                       sb = Sample(row, px * 4 + 2, hd.depth), sa = Sample(row, px * 4 + 3, hd.depth);
                        r = wide ? sr >> 8 : sr; g = wide ? sg >> 8 : sg; b = wide ? sb >> 8 : sb; a = wide ? sa >> 8 : sa;
                        break;
                    }
                    }
                    uint8_t* out = &level0[(size_t(y) * W + x) * 4];
                    out[0] = uint8_t(b); out[1] = uint8_t(g); out[2] = uint8_t(r); out[3] = uint8_t(a);
                    if (a != 255) anyAlpha = true;
                }
            }
        }

        // ---- mip chain (only for power-of-two sizes, the only ones the client mips) ----
        std::vector<std::vector<uint8_t>> levels;
        std::vector<std::pair<uint32_t, uint32_t>> dims;
        levels.push_back(std::move(level0));
        dims.push_back({ W, H });
        const bool mips = IsPow2(W) && IsPow2(H);
        while (mips && (dims.back().first > 1 || dims.back().second > 1) && levels.size() < 16)
        {
            const uint32_t sw = dims.back().first, sh = dims.back().second;
            const uint32_t dw = std::max(1u, sw >> 1), dh = std::max(1u, sh >> 1);
            const std::vector<uint8_t>& src = levels.back();
            std::vector<uint8_t> dst(size_t(dw) * dh * 4);
            for (uint32_t y = 0; y < dh; ++y)
            {
                const uint32_t y0 = std::min(y * 2, sh - 1), y1 = std::min(y * 2 + 1, sh - 1);
                for (uint32_t x = 0; x < dw; ++x)
                {
                    const uint32_t x0 = std::min(x * 2, sw - 1), x1 = std::min(x * 2 + 1, sw - 1);
                    const uint8_t* px[4] = { &src[(size_t(y0) * sw + x0) * 4], &src[(size_t(y0) * sw + x1) * 4],
                                             &src[(size_t(y1) * sw + x0) * 4], &src[(size_t(y1) * sw + x1) * 4] };
                    uint32_t sumA = 0, c[3] = { 0, 0, 0 }, plain[3] = { 0, 0, 0 };
                    for (const uint8_t* p : px)
                    {
                        sumA += p[3];
                        for (int k = 0; k < 3; ++k) { c[k] += uint32_t(p[k]) * p[3]; plain[k] += p[k]; }
                    }
                    uint8_t* o = &dst[(size_t(y) * dw + x) * 4];
                    for (int k = 0; k < 3; ++k) o[k] = uint8_t(sumA ? (c[k] + sumA / 2) / sumA : (plain[k] + 2) / 4);   // color weighted by alpha
                    o[3] = uint8_t((sumA + 2) / 4);
                }
            }
            levels.push_back(std::move(dst));
            dims.push_back({ dw, dh });
        }

        // ---- the BLP2 ----
        size_t total = kBlpHeaderSize;
        for (const auto& l : levels) total += l.size();
        blp.assign(total, 0);
        blp[0] = 'B'; blp[1] = 'L'; blp[2] = 'P'; blp[3] = '2';
        Put32(blp, 4, 1);                       // version
        blp[8]  = 3;                            // colorEncoding: uncompressed ARGB8888
        blp[9]  = anyAlpha ? 8 : 0;             // alpha bits
        blp[10] = 2;                            // preferredFormat: PIXEL_ARGB8888
        blp[11] = mips ? 1 : 0;                 // hasMips
        Put32(blp, 12, W);
        Put32(blp, 16, H);
        size_t at = kBlpHeaderSize;
        for (size_t i = 0; i < levels.size(); ++i)
        {
            Put32(blp, 0x14 + i * 4, uint32_t(at));
            Put32(blp, 0x54 + i * 4, uint32_t(levels[i].size()));
            std::memcpy(blp.data() + at, levels[i].data(), levels[i].size());
            at += levels[i].size();
        }
        return true;
    }
}
