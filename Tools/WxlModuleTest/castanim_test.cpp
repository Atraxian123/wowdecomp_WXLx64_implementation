// Host-side test of the precast animation speed decision (Engine/Source/WXL/CastAnim.h). Needs no game data.
// Build and run it with run_castanim_tests.sh.
#include <cstdio>

#include "Engine/Source/WXL/CastAnim.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { if (cond) ++g_pass; else { ++g_fail; std::printf("  FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

int main()
{
    namespace ca = wxl::castanim;
    const float lo = 0.5f, hi = 2.0f;

    // 1000 ms sequence
    CHECK(ca::PrecastSpeed(1000, 1000, lo, hi) == 1.0f, "cast time equal to the sequence: authored speed");
    CHECK(ca::PrecastSpeed(1000, 2000, lo, hi) == 0.5f, "twice as long a cast: half speed");
    CHECK(ca::PrecastSpeed(1000, 500, lo, hi) == 2.0f, "half as long a cast: double speed");
    CHECK(ca::PrecastSpeed(1000, 1500, lo, hi) > 0.66f && ca::PrecastSpeed(1000, 1500, lo, hi) < 0.67f, "1.5 s cast: 2/3 speed");

    // clamping
    CHECK(ca::PrecastSpeed(1000, 10000, lo, hi) == lo, "very long cast: clamped to the minimum speed");
    CHECK(ca::PrecastSpeed(1000, 100, lo, hi) == hi, "very short cast: clamped to the maximum speed");

    // nothing to scale: the caller's speed comes back
    CHECK(ca::PrecastSpeed(1000, 0, lo, hi) == 1.0f, "instant cast: untouched");
    CHECK(ca::PrecastSpeed(1000, -5, lo, hi) == 1.0f, "negative cast time: untouched");
    CHECK(ca::PrecastSpeed(1000, 0x7FFFFFFF, lo, hi) == 1.0f, "until-cancelled placeholder: untouched");
    CHECK(ca::PrecastSpeed(0, 1500, lo, hi) == 1.0f, "sequence without a length: untouched");
    CHECK(ca::PrecastSpeed(0, 1500, lo, hi, 1.25f) == 1.25f, "the fallback is what is returned when nothing scales");

    // LoopShorterAnimations=1 (1000 ms sequence)
    auto L = [&](int32_t cast, float mod) { return ca::PrecastSpeedLooping(1000, cast, mod, lo, hi); };
    CHECK(L(3000, 1.0f) == 1.0f, "shorter than the cast, no haste: authored speed (loops)");
    CHECK(L(3000, 0.7f) == 1.0f, "shorter than the cast, haste: still authored speed (haste never speeds it up)");
    CHECK(L(1000, 1.0f) == 1.0f, "exactly the cast time: authored speed");
    CHECK(L(3000, 2.0f) == 0.5f, "negative haste x2, slowed animation (2000) still shorter: slowed by the factor, loops");
    CHECK(L(2000, 2.0f) == 0.5f, "slowed animation exactly the cast time: slowed by the factor");
    CHECK(L(1500, 2.0f) > 0.66f && L(1500, 2.0f) < 0.67f, "slowed animation (2000) longer than the 1500 ms cast: fitted to the cast time");
    CHECK(L(500, 1.0f) == 2.0f && L(100, 1.0f) == hi, "animation longer than the cast: fitted, clamped to the maximum");
    CHECK(L(500, 0.5f) == 2.0f, "animation longer than a hasted cast: fitted");
    CHECK(L(10000, 4.0f) == 0.25f, "large negative haste: the slow-down is not clamped (it still fits the cast)");
    CHECK(L(0, 2.0f) == 1.0f && L(0x7FFFFFFF, 2.0f) == 1.0f && ca::PrecastSpeedLooping(0, 1500, 2.0f, lo, hi) == 1.0f, "nothing to scale: untouched");

    // a degenerate range is the caller's to sanitise (the ini loader does); equal limits pin the speed
    CHECK(ca::PrecastSpeed(1000, 4000, 1.0f, 1.0f) == 1.0f, "min == max pins the speed");

#ifndef _WIN32
    // off by default, and the limits are valid, when there is no ini to read
    CHECK(!ca::ScalePrecastEnabled(), "opt-in: disabled without the ini switch");
    CHECK(ca::PrecastMinSpeed() > 0.0f && ca::PrecastMaxSpeed() >= ca::PrecastMinSpeed(), "default limits are a valid range");
#endif

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
