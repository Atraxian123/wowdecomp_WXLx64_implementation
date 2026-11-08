// Spell area module through a fake module API: the module is linked in, the engine side is stubbed. Checks that it
// registers its renderer, collects a hostile dynamic object and the player's own cast, draws them with the right
// size and color, caches its textures, and unregisters the renderer on shutdown.
#include "wxl_api.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

extern "C" int  WxlModuleInit(const WxlApi*, WxlModuleInfo*);
extern "C" void WxlModuleShutdown();

static int g_failed = 0;
static void Check(bool ok, const char* what) { if (!ok) { ++g_failed; std::printf("  FAIL: %s\n", what); } }

static WxlSpellAreaRenderFn g_fn;
static void* g_user;
static int g_instantOn = 0;
static float g_fadeSeconds = 2.0f;
static uint32_t g_eventAt = 0;
static uint64_t g_eventsMade = 0;
static int g_lastAlpha = -1, g_instantDraws = 0;
static uint32_t g_eventSpell = 40, g_eventDelay = 0, g_eventTargetFlags = 0;
static int g_eventInstant = 1, g_auraOn = 0, g_triggeredOn = 0;
static float g_eventDest[3] = {};
static int g_periodicDraws = 0, g_missileDraws = 0, g_missileAlpha = -1, g_periodicRed = 0;
static float g_periodicPos[3] = {}, g_missilePos[3] = {};
static int g_instantRotated = 0;
static int g_draws = 0, g_textures = 0, g_redHalf8 = 0, g_halfTen = 0, g_backCone = 0, g_frontLine = 0;
static std::string g_dir;

int main()
{
    // the module's files live under <working directory>\WXL: a back cone (spell 20) and a line (spell 30)
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "spell_area_module_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "WXL");
    { std::ofstream f(root / "WXL" / "SpellsCustomAttribute.csv"); f << "#spell_id,attributes\r\n20,2\r\n30,4\r\n"; }
    // a cone file that sits in the WXL folder while the ini names it bare (ConeFile=SpellsCone.csv): spell 40 is a 180 degree cone
    { std::ofstream f(root / "WXL" / "SpellsCone.csv"); f << "#ID,ConeDegrees\r\n40,180\r\n"; }
    g_dir = root.string();

    static WxlApi api = {};
    api.abiVersion = WXL_ABI_VERSION;
    api.structSize = sizeof(api);
    api.log = [](const char*, const char*) {};
    api.iniGetInt = [](const char*, const char* k, int d) { return !std::strcmp(k, "Enabled") ? 1 : !std::strcmp(k, "Others") ? 1 : !std::strcmp(k, "Instant") ? g_instantOn : !std::strcmp(k, "Triggered") ? g_triggeredOn : d; };
    api.iniGetString = [](const char*, const char* k, char* out, size_t cap) -> int
    {
        if (!std::strcmp(k, "ConeFile")) { std::snprintf(out, cap, "SpellsCone.csv"); return 1; }
        if (std::strcmp(k, "InstantFadeSeconds") != 0) return 0;
        std::snprintf(out, cap, "%.2f", g_fadeSeconds);
        return 1;
    };
    api.iniStamp = []() -> uint64_t { return 1 + g_instantOn + 1000 * g_triggeredOn + static_cast<uint64_t>(g_fadeSeconds * 100.0f); };   // saving the ini changes the stamp
    api.spellAreaCastEvents = [](uint64_t after, WxlSpellAreaCastEvent* out, uint32_t) -> uint32_t
    {
        if (!g_eventsMade || g_eventsMade <= after) return 0;
        out[0] = {}; out[0].sequence = g_eventsMade; out[0].casterGuid = 1; out[0].spellId = g_eventSpell; out[0].instant = g_eventInstant;
        out[0].casterIsPlayer = 1; out[0].atMs = g_eventAt; out[0].delayMs = g_eventDelay; out[0].targetFlags = g_eventTargetFlags;
        std::memcpy(out[0].dest, g_eventDest, sizeof(g_eventDest));
        return 1;
    };
    api.workingDirectory = []() -> const char* { return g_dir.c_str(); };
    api.spellAreaDynamicObjects = [](WxlSpellAreaDynObject* o, uint32_t) -> uint32_t
    {
        o[0] = {}; o[0].guid = 5; o[0].casterGuid = 9; o[0].spellId = 10; o[0].radius = 8; o[0].casterKnown = 1; o[0].casterHostile = 1;
        return 1;
    };
    api.spellAreaCasters = [](WxlSpellAreaCaster* o, uint32_t) -> uint32_t
    {
        o[0] = {}; o[0].guid = 1; o[0].isPlayer = 1; o[0].castSpell = 20; o[0].castStartMs = 0; o[0].castEndMs = 5000; o[0].target = 7;
        o[0].channelSpell = 30; o[0].channelEndMs = 5000;
        return 1;
    };
    api.spellAreaEffects = [](uint32_t sp, uint64_t, WxlSpellAreaEffects* o) -> int
    {
        *o = {}; o->effect[0] = 2; o->targetA[0] = 15; o->radius[0] = sp == 40 ? 6.0f : 10.0f;   // around the caster
        if (sp == 61) { o->targetA[0] = 16; o->radius[0] = 7.0f; }   // the periodic aura's spell: an area at the target
        if (sp == 71) { o->targetA[0] = 16; o->radius[0] = 5.0f; }   // the grenade's spell: an area at its destination
        if (sp == 60 || sp == 70 || sp == 80) o->effect[0] = 0;       // the triggering spells have no area of their own
        return 1;
    };
    api.spellAreaPendingCast = [](uint64_t, uint32_t, WxlSpellAreaPending*) -> int { return 0; };
    api.spellAreaObjectPose = [](uint64_t g, float* p, float* f) -> int { p[0] = p[1] = p[2] = 0; if (g == 50) { p[0] = 10; p[1] = 20; } *f = 0; return 1; };
    api.spellAreaTriggers = [](uint32_t sp, WxlSpellAreaTriggers* o) -> int
    {
        *o = {};
        if (sp == 60) { o->count = 1; o->trigger[0] = { WXL_SPELLAREA_TRIGGER_PERIODIC, 61, 2000, 0 }; return 1; }    // an aura casting 61 every 2 s
        if (sp == 70) { o->count = 1; o->trigger[0] = { WXL_SPELLAREA_TRIGGER_MISSILE, 71, 0, 0 }; return 1; }       // a grenade
        if (sp == 80) { o->speed = 20.0f; o->count = 1; o->trigger[0] = { WXL_SPELLAREA_TRIGGER_MISSILE, 71, 0, 0 }; return 1; }
        return 0;
    };
    api.spellAreaTriggerAuras = [](WxlSpellAreaAura* o, uint32_t) -> uint32_t
    {
        if (!g_auraOn) return 0;
        o[0] = {}; o[0].holderGuid = 50; o[0].casterGuid = 9; o[0].spellId = 60; o[0].casterHostile = 1;
        return 1;
    };
    api.spellAreaCreateTexture = [](const uint32_t* px, uint32_t size, const char*) -> void* { ++g_textures; return px && size == 256 ? &g_textures : nullptr; };
    api.spellAreaDraw = [](const WxlSpellAreaDraw* d)
    {
        ++g_draws;
        if (d->half == 8.0f && d->r == 255 && d->g < 100) ++g_redHalf8;   // the enemy's ground effect: red
        if (d->half == 10.0f) ++g_halfTen;                                 // the player's own cast
        if (d->half == 7.0f) { ++g_periodicDraws; std::memcpy(g_periodicPos, d->pos, sizeof(g_periodicPos)); if (d->r == 255 && d->g < 100) ++g_periodicRed; }
        if (d->half == 5.0f) { ++g_missileDraws; g_missileAlpha = d->a; std::memcpy(g_missilePos, d->pos, sizeof(g_missilePos)); }
        if (d->half == 6.0f) { ++g_instantDraws; g_lastAlpha = d->a; if (d->rotate) ++g_instantRotated; }     // the instant spell's area (spell 40)
        if (d->half == 10.0f && d->rotate && std::fabs(d->facing - 3.14159265f) < 0.001f) ++g_backCone;   // spell 20: behind the caster
        if (d->half == 10.0f && d->rotate && std::fabs(d->facing) < 0.001f) ++g_frontLine;               // spell 30: in front
    };
    api.setSpellAreaRenderer = [](WxlSpellAreaRenderFn f, void* u) { g_fn = f; g_user = u; };

    std::printf("1. init\n");
    WxlModuleInfo info = {};
    Check(WxlModuleInit(&api, &info) == 0, "init succeeds");
    Check(std::strcmp(info.name, "spellarea") == 0, "names itself");
    Check(g_fn != nullptr, "registers the renderer");

    std::printf("2. one frame (two render calls)\n");
    g_fn(g_user, 0x200122, 5, 2500);
    g_fn(g_user, 0x20000, 4, 2500);
    Check(g_draws == 6, "three areas drawn on each of the two calls: the ground effect, the cast, the channel");
    Check(g_redHalf8 == 2, "the enemy's dynamic object: 8 yards, red");
    Check(g_halfTen == 4, "the player's cast and channel: 10 yards");
    Check(g_backCone == 2, "spell 20 has CONE_BACK: its cone is turned around, behind the caster");
    Check(g_frontLine == 2, "spell 30 has CONE_LINE: a line along the facing");
    Check(g_textures == 3, "a circle, a cone and a line texture, each made once");

    std::printf("3. a cast that is over\n");
    g_draws = 0;
    g_fn(g_user, 0x200122, 5, 2500 + 5000 + 1000);   // past the cast's end: only the dynamic object is left
    Check(g_draws == 1, "only the ground effect remains");

    std::printf("4. instant spells (off by default, fading over 2 s when on)\n");
    g_instantOn = 0;
    g_eventsMade = 1; g_eventAt = 20000;
    g_instantDraws = 0;
    g_fn(g_user, 0x200122, 5, 20000);
    Check(g_instantDraws == 0, "nothing drawn with Instant=0");
    g_instantOn = 1;
    g_fn(g_user, 0x200122, 5, 21000);               // switching on: the event from before is not shown
    Check(g_instantDraws == 0, "events from before it was switched on are skipped");
    g_eventsMade = 2; g_eventAt = 30000;
    g_instantDraws = 0; g_lastAlpha = -1;
    g_fn(g_user, 0x200122, 5, 30000);
    Check(g_instantDraws == 1, "an instant spell is drawn the moment it goes off");
    const int full = g_lastAlpha;
    Check(full == 166, "at the full opacity (0.65)");
    Check(g_instantRotated == g_instantDraws && g_instantDraws > 0, "spell 40 is a cone: SpellsCone.csv was found in the WXL folder from a bare name");
    g_fn(g_user, 0x200122, 5, 31000);
    const int half = g_lastAlpha;
    Check(half > 0 && half < full && half >= full / 2 - 2 && half <= full / 2 + 2, "half way through it is half as strong");
    g_instantDraws = 0;
    g_fn(g_user, 0x200122, 5, 32100);
    Check(g_instantDraws == 0, "gone after the fade time");
    g_fadeSeconds = 0.5f; g_instantOn = 0; g_fn(g_user, 0x200122, 5, 33000); g_instantOn = 1; g_fn(g_user, 0x200122, 5, 34000);  // reload with 0.5 s
    g_eventsMade = 3; g_eventAt = 40000; g_instantDraws = 0;
    g_fn(g_user, 0x200122, 5, 40300);
    Check(g_instantDraws == 1, "a fraction of a second works");
    g_instantDraws = 0;
    g_fn(g_user, 0x200122, 5, 40600);
    Check(g_instantDraws == 0, "and is over after it");

    std::printf("5. an aura that casts a spell periodically (off until Triggered=1)\n");
    g_auraOn = 1;
    g_fn(g_user, 0x200122, 5, 44000);
    Check(g_periodicDraws == 0, "nothing with Triggered=0");
    g_triggeredOn = 1;
    g_fn(g_user, 0x200122, 5, 45000);
    Check(g_periodicDraws == 1, "the triggered spell's area is drawn for the aura");
    Check(g_periodicPos[0] == 10.0f && g_periodicPos[1] == 20.0f, "around the unit that carries the aura");
    Check(g_periodicRed == 1, "an enemy's aura: red");
    g_auraOn = 0; g_periodicDraws = 0;
    g_fn(g_user, 0x200122, 5, 46000);
    Check(g_periodicDraws == 0, "gone with the aura");

    std::printf("6. a missile that triggers a spell (the packet says how long it flies)\n");
    g_instantOn = 0;                                   // only the triggered spell is wanted
    g_fn(g_user, 0x200122, 5, 47000);
    g_eventInstant = 0; g_eventSpell = 70; g_eventDelay = 1000; g_eventTargetFlags = WXL_SPELLAREA_TARGET_DEST;
    g_eventDest[0] = 30; g_eventDest[1] = 40; g_eventDest[2] = 5;
    g_eventsMade = 10; g_eventAt = 50000;
    g_missileDraws = 0; g_missileAlpha = -1;
    g_fn(g_user, 0x200122, 5, 50000);
    Check(g_missileDraws == 1, "the area shows from the launch");
    Check(g_missilePos[0] == 30.0f && g_missilePos[1] == 40.0f, "where the missile will land");
    Check(g_missileAlpha == 75, "faint at the launch");
    g_fn(g_user, 0x200122, 5, 50500);
    Check(g_missileAlpha == 120, "stronger half way");
    g_fn(g_user, 0x200122, 5, 51000);
    Check(g_missileAlpha == 166, "full when it lands");
    g_fn(g_user, 0x200122, 5, 51250);
    Check(g_missileAlpha == 83, "then fading (0.5 s): half way");
    g_missileDraws = 0;
    g_fn(g_user, 0x200122, 5, 51600);
    Check(g_missileDraws == 0, "gone");

    std::printf("7. a missile whose flight time is its distance over the spell's speed\n");
    g_eventSpell = 80; g_eventDelay = 0; g_eventDest[0] = 40; g_eventDest[1] = 0; g_eventDest[2] = 0;
    g_eventsMade = 11; g_eventAt = 60000;
    g_missileAlpha = -1;
    g_fn(g_user, 0x200122, 5, 61000);
    Check(g_missileAlpha == 120, "40 yards at 20 yards per second: half way after a second");
    g_fn(g_user, 0x200122, 5, 62000);
    Check(g_missileAlpha == 166, "full after two");

    std::printf("8. shutdown\n");
    WxlModuleShutdown();
    Check(g_fn == nullptr, "renderer unregistered");

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
