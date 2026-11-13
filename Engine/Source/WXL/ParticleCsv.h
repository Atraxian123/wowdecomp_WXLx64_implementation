// Per-model multi-textured particle emitter tables, read from a CSV beside the model. Opt-in.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
//
// The client's particle emitter has one texture. Retail models can have emitters that blend three, and the
// converted .m2 keeps only one of them. When WXLSetup.ini says
//
//     [Particles]
//     MultiTextureCsv=1
//     MultiTextureBlend=Multiply     ; or Add, or Mask
//     MultiTextureGain=2             ; Multiply only: 1, 2 or 4
//
// the model loader looks for a CSV named like the model, in the model's folder, and for every emitter the CSV marks
// as multi-textured it gives the emitter all of that emitter's textures. An emitter the CSV does not mark, a model
// without a CSV, and the setting off all render as before.
//
// How the textures of a multi-textured emitter combine (MultiTextureBlend):
//   Multiply (default)  one pass: texture0 * texture1 * texture2 * vertex colour in colour, and texture0's alpha times
//                       the particle's own alpha track in alpha, then blended
//                       into the frame the way the CSV says the emitter blends: Blending 4 is "add"; any other value keeps
//                       the blend the converted model carries.
//                       The filler textures (white8x8, a 50 % grey) are neutral in it, and the particle's own colour and
//                       alpha tracks are kept.
//   Add                 one pass per texture, each added into the frame on its own. A solid filler texture then
//                       adds a solid square, so this only suits emitters whose layers are all soft masks.
//   Mask                one pass: texture 0 is only the shape (its alpha times the particle's alpha track) and the
//                       colour is texture 1 * texture 2 * the particle's colour. For a smoke puff or shockwave mask
//                       in front of two energy textures.
//
// The second and third texture can also move: [Particles] Texture2XTranslation and the three like it, see LayerScroll.h.
// The "Blending" column is the M2 blend mode as extracted: 4 is "add", 7 is "blend add" (colour added, the destination
// dimmed by the particle's alpha); another value keeps the blend the converted model carries.
// MultiTextureGain (Multiply): the colour of the product is doubled once (2, the default: a 50 % grey layer is then
// neutral), twice (4), or not at all (1). Alpha is never scaled.
//
// File format (one row per texture of an emitter; the header names are matched case-insensitively, and without it
// the columns are taken in this order):
//
//     Emitter ID,Particle texture ID,Texture path,Multitextured,Blending
//     1,0,spells/fairydust2offsettwinkle_2x2_a.blp,Yes,4
//     1,1,spells/white8x8.blp,Yes,4
//     1,2,spells/grey50percentsquare16x16.blp,Yes,4
//     2,0,spells/bubble_soft_distortion.blp,No,4
//
// "Emitter ID" is the emitter's index in the model's particle emitter list, "Particle texture ID" its texture slot
// (0..2). Only the rows of a "Yes" emitter with at least two texture paths are used.
//
// The file is looked for at <model path without extension>.csv. A model that comes from a virtual path (a collection
// key <stem>_wxl_..._cmo<hex>.m2, or a global key <stem>[_oh]_<displayId>.m2) uses the CSV of the original model:
// <stem>.csv, in the original model's folder.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wxl::particlecsv
{
    constexpr uint32_t kMaxLayers = 3;   // the texture slots of one emitter

    struct Emitter
    {
        std::string texture[kMaxLayers];   // path per slot, '\\' separators; empty when the CSV names none
        bool        multiTextured = false; // the CSV's Multitextured column
        int         blending = 0;          // the CSV's Blending column (the M2 blend mode as extracted); informational
        uint32_t    NamedSlots() const;    // how many slots have a path
        bool        Usable() const { return multiTextured && NamedSlots() >= 2; }
    };

    class Table
    {
    public:
        std::map<uint32_t, Emitter> emitters;   // by emitter ID
        std::string                 source;     // the file it was read from

        /// The emitter's entry when it is a usable multi-textured one, else null (not listed, not multi-textured,
        /// or fewer than two textures named): such an emitter renders normally.
        const Emitter* Find(uint32_t emitterId) const;
        size_t         UsableCount() const;
    };

    /// WXLSetup.ini [Particles] MultiTextureCsv (default 0). Read once.
    bool Enabled();

    /// How the textures of a multi-textured emitter combine. See the top of this file.
    enum class LayerBlend : uint32_t { Add = 0, Multiply = 1, Mask = 2 };

    /// WXLSetup.ini [Particles] MultiTextureBlend: "Add", "Multiply" (default) or "Mask". Anything else is Multiply.
    LayerBlend Blend();

    /// WXLSetup.ini [Particles] MultiTextureGain: 1, 2 (default) or 4; other values round to the nearest. Read once.
    uint32_t Gain();

    /// The table for a model, or null when the setting is off or no CSV exists for it (not an error: the model
    /// simply renders as before). Results, hits and misses alike, are cached by path. Thread-safe.
    std::shared_ptr<const Table> For(const char* modelPath);

    /// Parses CSV text. Null, with a reason in *why when given, when it holds no usable row at all.
    std::shared_ptr<Table> Parse(const char* text, size_t size, std::string* why = nullptr);

    /// The CSV paths to try for a model path, most specific first, no duplicates.
    ///   1. the path with its extension replaced by .csv (a normal model, or a virtual path with a file of its own);
    ///   2. a collection key <stem>_wxl_...  ->  <stem>.csv
    ///   3. a global key <stem>[_oh]_<displayId>  ->  <stem>.csv, and without a trailing _oh
    /// Step 3 is a guess from the shape of the name, and real models have that shape too (retail variants
    /// staff_01.m2, staff_02.m2 are different models), so it is only taken when isVirtualKey says the name is a
    /// virtual path that was really minted.
    std::vector<std::string> Candidates(const char* modelPath, bool isVirtualKey);

    /// Lower case, '\\' separators: the form the archives and the cache key use.
    std::string NormalizePath(const char* path);
}
