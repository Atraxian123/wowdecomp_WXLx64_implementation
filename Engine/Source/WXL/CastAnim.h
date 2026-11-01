// WXL cast animation scaling: stretches or squeezes the spell PRECAST animation to the spell's casting time.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
//
// OPT-IN. The stock client plays the precast (wind-up) sequence at its authored speed, whatever the cast time.
// With the switch below on, CGUnit_C::PlayBaseAnimation plays it at sequenceDuration / castTime, so the
// wind-up finishes when the cast bar does. Everything else is stock. In WXLExtendedEquipment.ini (beside the
// client executable):
//
//     [Animation]
//     ; 1 = scale the spell precast animation (SpellPrecast and the modern aliases of it) to the spell's cast
//     ; time. Haste is already part of that time. Channeled spells and instant casts are never scaled.
//     ScalePrecastToCastTime=0
//
//     ; Limits of the playback speed (1.0 = the authored speed). A cast outside the range plays at the limit:
//     ; a long cast ends its precast early and the ready pose that follows carries on to the end of the bar.
//     PrecastMinSpeed=0.5
//     PrecastMaxSpeed=2.0
//
//     ; 0 = every precast is fitted to the cast time, as described above.
//     ; 1 = a precast that is naturally SHORTER than the cast time is not stretched: it plays at its authored
//     ;     speed and loops, as the stock client does. Exception, negative haste (the cast takes longer than
//     ;     base): the animation is first slowed by the same factor as the cast, then looped. If even that
//     ;     slowed animation is longer than the cast time, it is fitted to the cast time like in mode 0.
//     ;     A precast longer than the cast time is always fitted. Only has an effect with the switch above on.
//     LoopShorterAnimations=0
//
// The values are read once, on first use. Edit the ini, restart the client.
//
// The decision is a pure function (PrecastSpeed) so a host test can run it without the game.
#pragma once

#include <algorithm>
#include <cstdint>

namespace wxl::castanim
{
    /// [Animation] ScalePrecastToCastTime. False (stock behaviour) unless the ini sets it.
    bool ScalePrecastEnabled();

    /// [Animation] PrecastMinSpeed / PrecastMaxSpeed, already sanitised (0 < min <= max).
    float PrecastMinSpeed();
    float PrecastMaxSpeed();

    /// [Animation] LoopShorterAnimations. False unless the ini sets it.
    bool LoopShorterAnimations();

    /// Playback speed that makes a sequence of `durationMs` last `castTimeMs`, clamped to [minSpeed, maxSpeed].
    /// Returns `fallback` (the speed the caller already has, normally 1.0) when there is nothing to scale: no
    /// sequence length, or a cast time that is not a real, finite duration (instant, negative, or the
    /// 0x7FFFFFFF placeholder the client uses for "until cancelled").
    inline float PrecastSpeed(uint32_t durationMs, int32_t castTimeMs, float minSpeed, float maxSpeed, float fallback = 1.0f)
    {
        constexpr int32_t kNoRealCastTime = 0x7FFFFFFF;
        if (durationMs == 0 || castTimeMs <= 0 || castTimeMs == kNoRealCastTime)
            return fallback;

        const float speed = static_cast<float>(durationMs) / static_cast<float>(castTimeMs);
        return std::min(std::max(speed, minSpeed), maxSpeed);
    }

    /// LoopShorterAnimations=1 variant of PrecastSpeed. `castSpeedMod` is the unit's cast time multiplier as
    /// it applies to this spell (1 = no haste, above 1 = negative haste, below 1 = haste; pass 1 for a spell
    /// that haste does not affect).
    ///   * The sequence, slowed by negative haste (never sped up by haste), still fits in the cast time:
    ///     speed 1 / castSpeedMod. The sequence is shorter than the cast, so the client loops it.
    ///   * Otherwise it is fitted to the cast time, clamped, exactly like PrecastSpeed.
    /// Nothing to scale (see PrecastSpeed): `fallback`.
    inline float PrecastSpeedLooping(uint32_t durationMs, int32_t castTimeMs, float castSpeedMod, float minSpeed, float maxSpeed, float fallback = 1.0f)
    {
        constexpr int32_t kNoRealCastTime = 0x7FFFFFFF;
        if (durationMs == 0 || castTimeMs <= 0 || castTimeMs == kNoRealCastTime)
            return fallback;

        const float slow = castSpeedMod > 1.0f ? castSpeedMod : 1.0f;
        if (static_cast<double>(durationMs) * slow <= static_cast<double>(castTimeMs))
            return 1.0f / slow;

        return PrecastSpeed(durationMs, castTimeMs, minSpeed, maxSpeed, fallback);
    }
}
