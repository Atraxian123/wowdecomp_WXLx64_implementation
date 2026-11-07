// WXL BLP load-time transform: the one entry point the client's texture loaders call.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wxl::blp
{
    /**
     * @brief Offers a just-read BLP file to the WXL pipeline before the client parses it.
     *
     * Today this palettizes DXT character component textures (Item\TextureComponents\...): the client
     * CPU-composites those into the character skin and can only read the native palettized encoding,
     * so a modern DXT component otherwise fails with "component texture saved as DXT compressed".
     * Every other file is left alone.
     *
     * @param name  virtual path the file was opened under; matched case-insensitively, either slash.
     * @param data  the whole file as read from the archive (BLP2 only; anything else returns false).
     * @param out   receives the replacement file when the function returns true.
     * @return true when out holds a replacement the caller must parse instead of data. The caller keeps
     *         out alive for as long as it uses the parsed image. false means data is used unchanged.
     */
    bool TransformForLoad(const char* name, const void* data, std::vector<uint8_t>& out);

    /**
     * @brief Converts a PNG file into an in-memory BLP2 the client's texture loader can read.
     *
     * Used when a texture asks for a .blp that does not exist and a .png of the same name does. The result is an
     * uncompressed ARGB8888 BLP2 (colorEncoding 3, preferredFormat ARGB8888), with a full mip chain when both
     * sides are powers of two (box-filtered, color weighted by alpha) and a single level otherwise. Every PNG
     * color type and bit depth is read, interlaced files too; 16-bit samples are reduced to 8 bits. Images
     * larger than 16 million pixels are refused.
     *
     * @param png    the whole .png file.
     * @param blp    receives the BLP2 file on success.
     * @param error  receives the reason on failure.
     */
    bool PngToBlp(const uint8_t* png, size_t size, std::vector<uint8_t>& blp, std::string& error);
}
