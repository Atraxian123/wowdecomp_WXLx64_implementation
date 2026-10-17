// WXL BLP load-time transform: the one entry point the client's texture loaders call.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
#pragma once

#include <cstdint>
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
}
