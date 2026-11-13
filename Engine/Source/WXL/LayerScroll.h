// The animation of the second, third and fourth texture of a multi-textured particle emitter or ribbon emitter:
// they can slide and they can turn.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
//
//     [Particles]                         [Ribbons]
//     Texture2XTranslation=0.00           Texture2XTranslation=0.00
//     Texture2YTranslation=0.00           Texture2YTranslation=0.00
//     Texture2Rotation=0.00               Texture2Rotation=0.00
//     Texture3XTranslation=0.00           ... and the same for Texture3 and Texture4
//     Texture3YTranslation=0.00
//     Texture3Rotation=0.00
//     Texture4XTranslation=0.00
//     Texture4YTranslation=0.00
//     Texture4Rotation=0.00
//
// Translation is a speed in texture lengths per second: 1.0 moves the picture across its own width once a second, 0.25
// once every four seconds. Positive X moves the picture from left to right, positive Y from top to bottom. It may be
// negative. The picture moves along its own axes, so a texture that also turns drifts along its own turning axes.
//
// Rotation is a speed in whole turns per second: 1.0 is one full turn a second, 0.1 one turn in ten seconds. The turn
// is about the centre of the texture; positive is clockwise, negative counterclockwise. It is done in texture
// coordinates, so on a stretched surface (a long ribbon) the picture stretches with it, and on a particle that uses
// one cell of a picture atlas the centre is the centre of the whole picture, not of the cell.
//
// A decimal comma (0,25) is accepted. Texture 2 is the second texture of the emitter, 3 the third, 4 the fourth; the
// first texture never moves. Everything zero (the default, also for a missing key) leaves everything as it was.
//
// Read through wxl::ini, so a save of the file or `wxl set Particles Texture2Rotation 0.25` takes effect on the next
// frame, with no restart.
#pragma once

#include <cmath>
#include <cstdint>

namespace wxl::layerscroll
{
    constexpr uint32_t kMovingTextures = 3;   // textures 2, 3 and 4

    /// The motion of one kind of emitter. Index 0 is texture 2, 1 is texture 3, 2 is texture 4.
    struct Speeds
    {
        float x[kMovingTextures]   = {0.0f, 0.0f, 0.0f};   // lengths per second, + = left to right
        float y[kMovingTextures]   = {0.0f, 0.0f, 0.0f};   // lengths per second, + = top to bottom
        float turn[kMovingTextures] = {0.0f, 0.0f, 0.0f};  // whole turns per second, + = clockwise

        /// True when anything moves.
        bool Any() const
        {
            for (uint32_t i = 0; i < kMovingTextures; ++i)
                if (x[i] != 0.0f || y[i] != 0.0f || turn[i] != 0.0f) return true;
            return false;
        }

        /// True when texture `layer` (0 = first, 1 = second, ... 3 = fourth) moves or turns.
        bool Moves(uint32_t layer) const
        {
            return layer >= 1 && layer <= kMovingTextures &&
                   (x[layer - 1] != 0.0f || y[layer - 1] != 0.0f || turn[layer - 1] != 0.0f);
        }
    };

    /// [Particles] / [Ribbons] as the ini has them now. Re-read when the ini changes; the reference stays valid.
    const Speeds& Particles();
    const Speeds& Ribbons();

    /// A clock for the animation, in seconds, the same for every emitter so their layers stay in step.
    double Seconds();

    /// A texture's UV transform as the six numbers that matter for 2D coordinates:
    ///     u' = a0 * u + b0 * v + d0
    ///     v' = a1 * u + b1 * v + d1
    /// which are the fields of the same names in C44Matrix (and the rows of the shader constants SetTexMtx writes).
    /// The identity is a0 = 1, b0 = 0, d0 = 0, a1 = 0, b1 = 1, d1 = 0.
    struct Uv
    {
        float a0 = 1.0f, b0 = 0.0f, d0 = 0.0f;
        float a1 = 0.0f, b1 = 1.0f, d1 = 0.0f;
    };

    /// The transform of texture `layer` (0 = first) at `seconds`: a turn about the centre of the texture, then the
    /// slide. The first texture, and any other that does not move, get the identity. The turn and the slide are
    /// reduced to their last turn and last whole length in double precision, so they stay exact after days of uptime.
    inline Uv Transform(const Speeds& speeds, uint32_t layer, double seconds)
    {
        Uv uv;
        if (layer < 1 || layer > kMovingTextures) return uv;
        const uint32_t i = layer - 1;

        // The picture moves toward +X when the coordinates it is looked up with move toward -X; the same for Y.
        const double su = -static_cast<double>(speeds.x[i]) * seconds;
        const double sv = -static_cast<double>(speeds.y[i]) * seconds;
        const float du = static_cast<float>(su - std::floor(su));
        const float dv = static_cast<float>(sv - std::floor(sv));

        // The picture turns clockwise (V runs down the screen) when the coordinates it is looked up with turn the other
        // way: u' - 0.5 = cos * (u - 0.5) + sin * (v - 0.5), v' - 0.5 = -sin * (u - 0.5) + cos * (v - 0.5).
        const double turns = static_cast<double>(speeds.turn[i]) * seconds;
        const double angle = (turns - std::floor(turns)) * 6.283185307179586;
        const float c = static_cast<float>(std::cos(angle));
        const float s = static_cast<float>(std::sin(angle));

        uv.a0 = c;
        uv.b0 = s;
        uv.a1 = -s;
        uv.b1 = c;
        uv.d0 = 0.5f - 0.5f * c - 0.5f * s + du;
        uv.d1 = 0.5f + 0.5f * s - 0.5f * c + dv;
        return uv;
    }

    /// The translation alone, for callers that only slide (kept for the code that was written first).
    inline void Offset(const Speeds& speeds, uint32_t layer, double seconds, float& du, float& dv)
    {
        du = 0.0f;
        dv = 0.0f;
        if (layer < 1 || layer > kMovingTextures) return;
        const double u = -static_cast<double>(speeds.x[layer - 1]) * seconds;
        const double v = -static_cast<double>(speeds.y[layer - 1]) * seconds;
        du = static_cast<float>(u - std::floor(u));
        dv = static_cast<float>(v - std::floor(v));
    }
}
