// Per-model multi-textured particle emitter tables, read from a CSV beside the model. Opt-in.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
//
// The client's particle emitter has one texture. Retail models can have emitters that blend three, and the
// converted .m2 keeps only one of them. When WXLSetup.ini says
//
//     [Particles]
//     MultiTextureCsv=1
//
// the model loader looks for a CSV named like the model, in the model's folder, and for every emitter the CSV marks
// as multi-textured it gives the emitter all of that emitter's textures, drawn on top of each other with additive
// blending. An emitter the CSV does not mark, a model without a CSV, and the setting off all render as before.
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
