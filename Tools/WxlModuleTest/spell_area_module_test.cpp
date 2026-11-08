// Spell area module through a fake module API: the module is linked in, the engine side is stubbed. Checks that it
// registers its renderer, collects a hostile dynamic object and the player's own cast, draws them with the right
// size and color, caches its textures, and unregisters the renderer on shutdown.
#include "wxl_api.h"

#include <cstdio>
#include <cstring>

extern "C" int  WxlModuleInit(const WxlApi*, WxlModuleInfo*);
extern "C" void WxlModuleShutdown();

static int g_failed = 0;
static void Check(bool ok, const char* what) { if (!ok) { ++g_failed; std::printf("  FAIL: %s\n", what); } }

static WxlSpellAreaRenderFn g_fn;
static void* g_user;
static int g_draws = 0, g_textures = 0, g_redHalf8 = 0, g_halfTen = 0;

int main()
{
    static WxlApi api = {};
    api.abiVersion = WXL_ABI_VERSION;
    api.structSize = sizeof(api);
    api.log = [](const char*, const char*) {};
    api.iniGetInt = [](const char*, const char* k, int d) { return !std::strcmp(k, "Enabled") ? 1 : !std::strcmp(k, "Others") ? 1 : d; };
    api.iniGetString = [](const char*, const char*, char*, size_t) { return 0; };
    api.iniStamp = []() -> uint64_t { return 1; };
    api.workingDirectory = []() -> const char* { return "/nonexistent"; };
    api.spellAreaDynamicObjects = [](WxlSpellAreaDynObject* o, uint32_t) -> uint32_t
    {
        o[0] = {}; o[0].guid = 5; o[0].casterGuid = 9; o[0].spellId = 10; o[0].radius = 8; o[0].casterKnown = 1; o[0].casterHostile = 1;
        return 1;
    };
    api.spellAreaCasters = [](WxlSpellAreaCaster* o, uint32_t) -> uint32_t
    {
        o[0] = {}; o[0].guid = 1; o[0].isPlayer = 1; o[0].castSpell = 20; o[0].castStartMs = 0; o[0].castEndMs = 5000; o[0].target = 7;
        return 1;
    };
    api.spellAreaEffects = [](uint32_t, uint64_t, WxlSpellAreaEffects* o) -> int
    {
        *o = {}; o->effect[0] = 2; o->targetA[0] = 15; o->radius[0] = 10;   // an area around the caster, 10 yards
        return 1;
    };
    api.spellAreaPendingCast = [](uint64_t, uint32_t, WxlSpellAreaPending*) -> int { return 0; };
    api.spellAreaObjectPose = [](uint64_t, float* p, float* f) -> int { p[0] = p[1] = p[2] = 0; *f = 0; return 1; };
    api.spellAreaCreateTexture = [](const uint32_t* px, uint32_t size, const char*) -> void* { ++g_textures; return px && size == 256 ? &g_textures : nullptr; };
    api.spellAreaDraw = [](const WxlSpellAreaDraw* d)
    {
        ++g_draws;
        if (d->half == 8.0f && d->r == 255 && d->g < 100) ++g_redHalf8;   // the enemy's ground effect: red
        if (d->half == 10.0f) ++g_halfTen;                                 // the player's own cast
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
    Check(g_draws == 4, "two areas drawn on each of the two calls");
    Check(g_redHalf8 == 2, "the enemy's dynamic object: 8 yards, red");
    Check(g_halfTen == 2, "the player's cast: 10 yards");
    Check(g_textures == 1, "one circle texture, made once");

    std::printf("3. a cast that is over\n");
    g_draws = 0;
    g_fn(g_user, 0x200122, 5, 2500 + 5000 + 1000);   // past the cast's end: only the dynamic object is left
    Check(g_draws == 1, "only the ground effect remains");

    std::printf("4. shutdown\n");
    WxlModuleShutdown();
    Check(g_fn == nullptr, "renderer unregistered");

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
