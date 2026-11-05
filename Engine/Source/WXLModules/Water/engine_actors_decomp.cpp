// Contact selection for the ripple pass. The selection rules are the standalone module's engine_actors.cpp
// unchanged; reading the visible units out of the client is bridge.cpp's job now.
// Copyright (C) 2026 WarcraftXL (GPLv3, see LICENSE)

#include "engine_actors.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr float kMaxWorldCoordinate = 100000.0f;
constexpr float kMaxCollisionRadius = 50.0f;
constexpr float kMaxCollisionHeight = 100.0f;
constexpr float kMinClientRippleDepthLimit = 1.0f;
constexpr float kClientRippleDepthLimitPerHeight = 2.0f;

bool FiniteWithin(float value, float minimum, float maximum)
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool PlausibleContact(const WaterContact& c)
{
    for (float axis : c.position)
        if (!FiniteWithin(axis, -kMaxWorldCoordinate, kMaxWorldCoordinate))
            return false;
    return FiniteWithin(c.surface, -kMaxWorldCoordinate, kMaxWorldCoordinate) && c.radius > 0.0f &&
           FiniteWithin(c.radius, 0.0f, kMaxCollisionRadius) && c.height > 0.0f &&
           FiniteWithin(c.height, 0.0f, kMaxCollisionHeight) && WaterContactDepth(c) < ClientRippleDepthLimit(c.height);
}

double SquaredGroundDistance(const WaterContact& c, const float centre[3])
{
    const double dx = static_cast<double>(c.position[0]) - centre[0];
    const double dy = static_cast<double>(c.position[1]) - centre[1];
    return dx * dx + dy * dy;
}

bool Precedes(const WaterContact& a, const WaterContact& b, const float centre[3])
{
    const double aDistance = SquaredGroundDistance(a, centre);
    const double bDistance = SquaredGroundDistance(b, centre);
    if (aDistance != bDistance)
        return aDistance < bDistance;
    return a.guid < b.guid;
}

bool ValidCentre(const float centre[3])
{
    for (int i = 0; i < 3; ++i)
        if (!FiniteWithin(centre[i], -kMaxWorldCoordinate, kMaxWorldCoordinate))
            return false;
    return true;
}
}

float WaterContactDepth(const WaterContact& contact)
{
    return contact.surface - contact.position[2];
}

float ClientRippleDepthLimit(float height)
{
    return std::max(kMinClientRippleDepthLimit, kClientRippleDepthLimitPerHeight * height);
}

namespace engine
{
bool SelectWaterContact(WaterContactFrame& frame, const WaterContact& contact, const float centre[3])
{
    if (frame.count > kMaxWaterContacts || !ValidCentre(centre) || !PlausibleContact(contact) ||
        SquaredGroundDistance(contact, centre) > static_cast<double>(kWaterContactRange) * kWaterContactRange)
        return false;
    uint32_t index = 0;
    while (index < frame.count && !Precedes(contact, frame.contacts[index], centre))
        ++index;
    if (index == kMaxWaterContacts)
        return false;
    const uint32_t count = std::min(frame.count + 1, kMaxWaterContacts);
    for (uint32_t i = count - 1; i > index; --i)
        frame.contacts[i] = frame.contacts[i - 1];
    frame.contacts[index] = contact;
    frame.count = count;
    return true;
}
}
