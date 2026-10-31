// Flight animations: which animation ids a unit in flight is played with, and how a model that never shipped
// a flight sequence falls back to its swim sequence.
//
// The client has always played the SWIM set (ANIM_SWIM_IDLE..ANIM_SWIM_BACKWARDS, 41-45) for a unit in flight:
// ChooseDefaultAnim and ChooseMovementAnim ask for it whenever MOVEMENTFLAG_SWIMMING or MOVEMENTFLAG_FLYING is
// set. The 3.3.5 animation table also carries a flight twin of that set at a fixed offset
// (ANIM_FLY_SWIM_IDLE..ANIM_FLY_SWIM_BACKWARDS, 0x10E-0x112) that nothing ever requested. Later-era models
// (Cataclysm and on) ship real flight sequences; the modern reader files them under those twin ids.
//
// The request stays a swim id (so every classifier keyed on the request keeps working); the resolver
// (CGUnit_C::ResolveAnimForModel) turns it into the flight twin when the unit is flying and not submerged and
// the model has that sequence natively, and into the plain swim sequence otherwise.
//
// Pure functions and constants only, so the engine layer (the modern reader's id remap) and the game layer (the
// resolver) share one definition and a host test can run the decision without the game.
#pragma once

#include <cstdint>

namespace wxl::flightanim
{
    // The swim set: idle, forward, left, right, backwards.
    constexpr uint32_t kSwimIdle      = 41;
    constexpr uint32_t kSwimBackwards = 45;

    // Its flight twin: the table's FLY family sits a fixed 229 ids above the whole base table.
    constexpr uint32_t kFlightOffset         = 229;
    constexpr uint32_t kFlightSwimIdle       = kSwimIdle + kFlightOffset;      // 0x10E
    constexpr uint32_t kFlightSwimBackwards  = kSwimBackwards + kFlightOffset; // 0x112
    static_assert(kFlightSwimIdle == 0x10E && kFlightSwimBackwards == 0x112, "flight twin of the swim set");

    constexpr bool IsSwimSet(uint32_t id)   { return id >= kSwimIdle && id <= kSwimBackwards; }
    constexpr bool IsFlightSet(uint32_t id) { return id >= kFlightSwimIdle && id <= kFlightSwimBackwards; }

    // Source (modern-era) ids of the flight set (MountFlightIdle / Run / Left / Right / WalkBackwards), in the same
    // idle / forward / left / right / backwards order as the swim set. The reader re-files them under the twins, so
    // a modern model and a model authored natively at the twin ids behave the same.
    constexpr uint32_t kSourceFlyIdle      = 548;
    constexpr uint32_t kSourceFly          = 556;
    constexpr uint32_t kSourceFlyLeft      = 552;
    constexpr uint32_t kSourceFlyRight     = 554;
    constexpr uint32_t kSourceFlyBackwards = 562;

    /// The client flight id a modern source id is re-filed under, or 0 when it is not part of the flight set.
    constexpr uint32_t FlightIdForSource(uint32_t sourceId)
    {
        switch (sourceId)
        {
        case kSourceFlyIdle:      return kSwimIdle + kFlightOffset;
        case kSourceFly:          return kSwimIdle + 1 + kFlightOffset;
        case kSourceFlyLeft:      return kSwimIdle + 2 + kFlightOffset;
        case kSourceFlyRight:     return kSwimIdle + 3 + kFlightOffset;
        case kSourceFlyBackwards: return kSwimBackwards + kFlightOffset;
        default:                  return 0;
        }
    }

    /// The animation id to resolve for a model.
    ///   * A swim-set request from a unit in flight (flying and not swimming): the flight twin when the model has
    ///     it natively (`has` is a native-presence test, not a fallback-aware one), else the swim id unchanged.
    ///   * A flight-twin request (a rider model resolved first and found one) for a model that lacks it: the swim id.
    ///   * Anything else: unchanged.
    template <class HasSequence>
    inline uint32_t Prefer(uint32_t animId, bool inFlight, HasSequence&& has)
    {
        if (IsSwimSet(animId))
        {
            const uint32_t flight = animId + kFlightOffset;
            return (inFlight && has(flight)) ? flight : animId;
        }

        if (IsFlightSet(animId) && !has(animId))
            return animId - kFlightOffset;

        return animId;
    }
}
