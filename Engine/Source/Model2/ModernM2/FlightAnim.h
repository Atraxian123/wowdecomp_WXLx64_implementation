// Flight animations: which animation ids a unit in flight is played with, and how a model that never shipped
// a flight sequence falls back to its swim sequence.
//
// The client has always played the SWIM set (ANIM_SWIM_IDLE..ANIM_SWIM_BACKWARDS, 41-45) for a unit in flight:
// ChooseDefaultAnim and ChooseMovementAnim ask for it whenever MOVEMENTFLAG_SWIMMING or MOVEMENTFLAG_FLYING is
// set. The 3.3.5 animation table also carries a flight twin of that set at a fixed offset
// (ANIM_FLY_SWIM_IDLE..ANIM_FLY_SWIM_BACKWARDS, 0x10E-0x112) that nothing ever requested. Later-era models
// (Cataclysm and on) ship real flight sequences; the modern reader files them under those twin ids.
//
// A model of ANY era can also carry the mount flight set itself (MOUNT_FLIGHT_IDLE..WALK_BACKWARDS, ids 548, 556, 552,
// 554, 562, the ids the modern models use): a native .m2 can be authored with them, since a sequence is found by id.
//
// The request stays a swim id (so every classifier keyed on the request keeps working); the resolver
// (CGUnit_C::ResolveAnimForModel) turns it, for a unit that is flying and not submerged, into the first of these
// that the model has natively: the MOUNT_FLIGHT sequence, then the FLY_SWIM twin, then the plain swim sequence.
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

    // The MOUNT_FLIGHT set, in the same idle / forward / left / right / backwards order as the swim set.
    constexpr uint32_t kMountFlightIdle      = 548;
    constexpr uint32_t kMountFlight          = 556;
    constexpr uint32_t kMountFlightLeft      = 552;
    constexpr uint32_t kMountFlightRight     = 554;
    constexpr uint32_t kMountFlightBackwards = 562;

    constexpr bool IsMountFlightSet(uint32_t id)
    {
        return id == kMountFlightIdle || id == kMountFlight || id == kMountFlightLeft || id == kMountFlightRight || id == kMountFlightBackwards;
    }

    /// The MOUNT_FLIGHT id that goes with a swim id (41..45), 0 for anything else.
    constexpr uint32_t MountFlightForSwim(uint32_t swimId)
    {
        switch (swimId - kSwimIdle)
        {
        case 0:  return kMountFlightIdle;
        case 1:  return kMountFlight;
        case 2:  return kMountFlightLeft;
        case 3:  return kMountFlightRight;
        case 4:  return kMountFlightBackwards;
        default: return 0;
        }
    }

    /// The swim id (41..45) a swim, flight-twin or MOUNT_FLIGHT id belongs to, 0 for any other id.
    constexpr uint32_t SwimIdFor(uint32_t id)
    {
        if (IsSwimSet(id)) return id;
        if (IsFlightSet(id)) return id - kFlightOffset;
        switch (id)
        {
        case kMountFlightIdle:      return kSwimIdle;
        case kMountFlight:          return kSwimIdle + 1;
        case kMountFlightLeft:      return kSwimIdle + 2;
        case kMountFlightRight:     return kSwimIdle + 3;
        case kMountFlightBackwards: return kSwimBackwards;
        default:                    return 0;
        }
    }

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
    ///   * A swim-set request from a unit in flight (flying and not swimming), or a request already in the flight family
    ///     (a rider model resolved first and found one): the first id of the same slot that the model has natively
    ///     (`has` is a native-presence test, not a fallback-aware one), in this order:
    ///         1. MOUNT_FLIGHT_*   2. ANIM_FLY_SWIM_* (the twin)   3. the plain swim id.
    ///   * A swim-set request from a unit that is not in flight (swimming): unchanged, always.
    ///   * Anything else: unchanged.
    template <class HasSequence>
    inline uint32_t Prefer(uint32_t animId, bool inFlight, HasSequence&& has)
    {
        const uint32_t swim = SwimIdFor(animId);
        if (!swim) return animId;
        if (IsSwimSet(animId) && !inFlight) return animId;

        const uint32_t mount = MountFlightForSwim(swim);
        if (has(mount)) return mount;

        const uint32_t twin = swim + kFlightOffset;
        if (has(twin)) return twin;

        return swim;
    }
}
