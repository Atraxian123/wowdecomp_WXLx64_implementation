// How the textures of a multi-textured ribbon emitter combine. Opt-in.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
//
// A ribbon emitter can name up to four texture layers. The stock client draws one pass per layer, each blended into
// the frame on its own with the blend mode of its own material. When WXLSetup.ini says
//
//     [Ribbons]
//     MultiTextureBlend=Multiply     ; Off (default), Multiply or Add
//     MultiTextureGain=2             ; Multiply only: 1, 2 or 4
//
// every ribbon with two or more textures, of a native Wrath model or a modern one alike, is drawn the chosen way. A
// ribbon with a single texture is never touched: it keeps the blending of its model.
//
//   Multiply   ONE pass: texture0 * texture1 [* texture2 [* texture3]] * the ribbon's vertex colour, in colour and in
//              alpha, blended into the frame the way the first layer's material says. The alpha is the plain product
//              of the layers' alphas (and the ribbon's own alpha); it is never scaled. A solid white layer is neutral.
//   Add        one pass per texture, each added into the frame on its own (blend mode "add", whatever the material
//              says), so the layers sum. A solid layer then adds a solid area: this suits ribbons whose layers are
//              all soft masks.
//
// The second, third and fourth texture of a multi-textured ribbon can also slide and turn: [Ribbons]
// Texture2XTranslation, Texture2Rotation and the others like them (see LayerScroll.h). That works in every mode, Off
// included.
//
// MultiTextureGain (Multiply): the colour of the product is multiplied once more by 2 (the last stage doubles: 2, the
// default, which makes a 50 percent grey layer neutral), by 4 (the last two stages double), or not at all (1).
//
// Off (the default) changes nothing from what the client did before this setting: a modern model's multi-textured
// ribbons are one multiplied pass as the model loader has always made them, and a native Wrath model's are drawn
// by the stock client, one pass per layer with the material's own blend mode.
#pragma once

#include <cstdint>
#include <string>

namespace wxl::ribbonblend
{
    enum class Mode { Off, Multiply, Add };

    /// "multiply" / "multiplicative", "add" / "additive", "off" / "0" / "no" / "none" / "" (any case) to the mode;
    /// anything else is Off and `recognized` (when given) is false.
    Mode ParseMode(const std::string& text, bool* recognized = nullptr);

    /// "1", "2" or "4" to itself, "" to 2; anything else is 2 and `recognized` (when given) is false.
    int ParseGain(const std::string& text, bool* recognized = nullptr);

    /// What one texture stage of the multiplied pass does with the result of the stage before it: modulate (multiply),
    /// or modulate and double.
    struct StageOp
    {
        bool colorDouble = false;
        bool alphaDouble = false;
    };

    /// The ops of the `layers` stages (1..4) of the multiplied pass; `out` has room for `layers`. `colorGain` is the
    /// factor of the whole chain on the colour: 1 no stage doubles, 2 the last one does, 4 the last two do. With
    /// `plainAlpha` the alpha never doubles; without it the alpha follows the colour (the stock fold, gain 4).
    void StageOps(uint32_t layers, int colorGain, bool plainAlpha, StageOp* out);

    /// WXLSetup.ini [Ribbons] MultiTextureBlend. Re-read when the ini changes; a ribbon takes it when its model loads.
    Mode MultiTextureBlend();

    /// WXLSetup.ini [Ribbons] MultiTextureGain (default 2). Re-read when the ini changes.
    int MultiTextureGain();
}
