// Routing half of the BLP pipeline: decides whether a just-read .blp needs transforming.
// Derived from WarcraftXL's BlpHost.cpp (GPLv3, Copyright (C) 2026 WarcraftXL). The storage-hook
// registration of the injected build is gone: the client's own loaders call TransformForLoad directly.
//
// Only the character-component palettize path is routed. TranscodeBlp and CapBlpMips are compiled in
// (BlpTranscode.cpp) but not routed yet: their output is DXT, which the client may keep pointing into
// the file buffer after loading, so serving it needs an ownership story first.

#include "Engine/Source/WXL/Blp.h"
#include "Engine/Source/WXL/Log.h"

#include "BlpTranscode.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
    namespace transcode = wxl::modern::assets::textures::blp;

    uint32_t Rd32(const uint8_t* p)
    {
        return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
    }

    /// Bytes of the file that the header accounts for (end of the furthest mip), or 0 when this is not
    /// a BLP2. The loaders hand over a bare buffer, so the size comes from the header.
    size_t Blp2Extent(const uint8_t* b)
    {
        if (Rd32(b) != 0x32504C42 || Rd32(b + 4) != 1) return 0;
        uint64_t end = 0x94;
        for (int i = 0; i < 16; ++i)
        {
            const uint64_t off = Rd32(b + 0x14 + i * 4), size = Rd32(b + 0x54 + i * 4);
            if (size != 0 && off + size > end) end = off + size;
        }
        return end > 0xFFFFFFFFull ? 0 : static_cast<size_t>(end);
    }

    bool StartsWithCI(const char* s, const char* prefix)
    {
        for (; *prefix; ++s, ++prefix)
        {
            char a = *s, b = *prefix;
            if (a == '/') a = '\\';
            if (b == '/') b = '\\';
            if (std::tolower(static_cast<unsigned char>(a)) != std::tolower(static_cast<unsigned char>(b)))
                return false;
        }
        return true;
    }

    bool IsTextureComponent(const char* name)
    {
        return StartsWithCI(name, "item\\texturecomponents\\");
    }

    std::string ReadTextFile(const char* path)
    {
        FILE* f = std::fopen(path, "rb");
        if (!f) return {};
        std::string content;
        char buf[4096];
        for (;;)
        {
            const size_t n = std::fread(buf, 1, sizeof(buf), f);
            if (n) content.append(buf, n);
            if (n < sizeof(buf)) break;
        }
        std::fclose(f);
        return content;
    }

    int FindConfigInt(const std::string& cfg, const char* key, int fallback)
    {
        const std::string needle = std::string("SET ") + key;
        size_t pos = cfg.find(needle);
        if (pos == std::string::npos) return fallback;
        pos += needle.size();
        while (pos < cfg.size() && std::isspace(static_cast<unsigned char>(cfg[pos]))) ++pos;
        if (pos < cfg.size() && cfg[pos] == '"') ++pos;
        bool neg = false;
        if (pos < cfg.size() && cfg[pos] == '-') { neg = true; ++pos; }
        int value = 0;
        bool any = false;
        while (pos < cfg.size() && std::isdigit(static_cast<unsigned char>(cfg[pos])))
        {
            any = true;
            value = value * 10 + int(cfg[pos] - '0');
            ++pos;
        }
        if (!any) return fallback;
        return neg ? -value : value;
    }

    /// Long-edge budget for a palettized component: 256 natively, 512/1024 for componentTextureLevel 8/9+.
    /// The client's working directory is its own folder, so WTF\Config.wtf is the client's config.
    uint32_t ComponentTextureMaxEdge()
    {
        static const uint32_t maxEdge = []() {
            const int level = FindConfigInt(ReadTextFile("WTF\\Config.wtf"), "componentTextureLevel", 0);
            uint32_t edge = 256;
            if (level >= 9) edge = 1024;
            else if (level >= 8) edge = 512;
            wxl::LogCat("BLP", "componentTextureLevel=%d componentMaxEdge=%u", level, edge);
            return edge;
        }();
        return maxEdge;
    }
}

namespace wxl::blp
{
    bool TransformForLoad(const char* name, const void* data, std::vector<uint8_t>& out)
    {
        if (!name || !data) return false;

        // The first BLP names the client offers, so the log shows whether this hook is reached at all and how
        // the names are spelled. Without this, a silent hook and a non-matching name look identical.
        if (wxl::LogThrottle("blp.seen", 80, 0))
            wxl::LogCat("BLP", "offered '%s' (%s)", name, IsTextureComponent(name) ? "character component" : "not a component");

        if (!IsTextureComponent(name)) return false;

        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        const size_t   size  = Blp2Extent(bytes);
        if (size == 0)
        {
            if (wxl::LogThrottle("blp.notblp2", 20, 100))
                wxl::LogCat("BLP", "component '%s': not a BLP2 (magic/version mismatch), left untouched", name);
            return false;
        }

        // Header fields that decide what the client does with the file.
        const unsigned encoding = bytes[8], alphaDepth = bytes[9], alphaType = bytes[10], hasMips = bytes[11];
        const uint32_t width = Rd32(bytes + 0xC), height = Rd32(bytes + 0x10);

        const uint32_t maxEdge = ComponentTextureMaxEdge();
        std::vector<uint8_t> palettized;
        if (!transcode::TextureComponentToPaletted({ bytes, size }, palettized, maxEdge))
        {
            // Encoding 1 is the native palettized format the client reads itself. Anything else that lands
            // here is a DXT or other encoding the palettizer could not convert.
            if (wxl::LogThrottle(encoding == 1 ? "blp.native" : "blp.cantconvert", 40, 100))
                wxl::LogCat("BLP", "component '%s': %ux%u encoding=%u alphaDepth=%u alphaType=%u mips=%u -> %s", name,
                            width, height, encoding, alphaDepth, alphaType, hasMips,
                            encoding == 1 ? "already palettized, left as is"
                                          : "NOT CONVERTED: encoding unsupported by the palettizer (the client will fall back to green if it needs palettized data)");
            return false;
        }

        wxl::LogCat("BLP", "component '%s': %ux%u encoding=%u alphaType=%u -> PALETTIZED (maxEdge=%u, %u -> %u bytes, out header enc=%u alphaDepth=%u alphaType=%u mips=%u %ux%u)",
                    name, width, height, encoding, alphaType, maxEdge, uint32_t(size), uint32_t(palettized.size()),
                    unsigned(palettized[8]), unsigned(palettized[9]), unsigned(palettized[10]), unsigned(palettized[11]),
                    Rd32(palettized.data() + 0xC), Rd32(palettized.data() + 0x10));
        out = std::move(palettized);
        return true;
    }
}
