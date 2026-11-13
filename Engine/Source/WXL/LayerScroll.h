// The animation of the second and third texture of a multi-textured particle emitter or ribbon emitter.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
//
//     [Particles]                         [Ribbons]
//     Texture2XTranslation=0.00           Texture2XTranslation=0.00
//     Texture2YTranslation=0.00           Texture2YTranslation=0.00
//     Texture3XTranslation=0.00           Texture3XTranslation=0.00
//     Texture3YTranslation=0.00           Texture3YTranslation=0.00
//
// Each value is a speed in texture lengths per second: 1.0 moves the picture across its own width once a second, 0.25
// once every four seconds. It may be negative, and a decimal comma (0,25) is accepted. Positive X moves the picture
// from left to right, positive Y from top to bottom. Texture 2 is the second texture of the emitter and texture 3 the
// third; the first texture never moves. All zero (the default, also for a missing key) leaves everything as it was.
//
// Read through wxl::ini, so a save of the file or `wxl set Particles Texture2XTranslation 0.25` takes effect on the
// next frame, with no restart.
#pragma once

#include <cmath>
#include <cstdint>

namespace wxl::layerscroll
{
    /// The speeds of one kind of emitter. Index 0 is texture 2, index 1 is texture 3.
    struct Speeds
    {
        float x[2] = {0.0f, 0.0f};
        float y[2] = {0.0f, 0.0f};

        /// True when any of the four is not zero.
        bool Any() const { return x[0] != 0.0f || x[1] != 0.0f || y[0] != 0.0f || y[1] != 0.0f; }

        /// True when texture `layer` (0 = first, 1 = second, 2 = third) moves.
        bool Moves(uint32_t layer) const { return layer >= 1 && layer <= 2 && (x[layer - 1] != 0.0f || y[layer - 1] != 0.0f); }
    };

    /// [Particles] / [Ribbons] as the ini has them now. Re-read when the ini changes; the reference stays valid.
    const Speeds& Particles();
    const Speeds& Ribbons();

    /// A clock for the animation, in seconds, the same for every emitter so their layers stay in step.
    double Seconds();

    /// How far to add to the texture coordinates of texture `layer` (0 = first) at `seconds`, so the picture moves at
    /// the configured speed. The first texture gets 0. The result is the fractional part of the travelled distance, in
    /// [0, 1): a texture that wraps repeats every whole length, and the small number keeps the precision at any uptime.
    inline void Offset(const Speeds& speeds, uint32_t layer, double seconds, float& du, float& dv)
    {
        du = 0.0f;
        dv = 0.0f;
        if (layer < 1 || layer > 2) return;
        // The picture moves toward +X when the coordinates it is looked up with move toward -X.
        const double u = -static_cast<double>(speeds.x[layer - 1]) * seconds;
        const double v = -static_cast<double>(speeds.y[layer - 1]) * seconds;
        du = static_cast<float>(u - std::floor(u));
        dv = static_cast<float>(v - std::floor(v));
    }
}
