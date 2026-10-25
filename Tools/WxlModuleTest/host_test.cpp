// End-to-end test of a WXL module through the C ABI, without the client.
//
//   build the module as a shared library, then this host (g++, Linux or MinGW), and run it from a folder that has
//   data/item/objectcomponents/head/plume_test.phy2 (see make_test_data.py):
//
//   R=<repo root>; M=$R/Engine/Source/WXLModules/Physics
//   g++ -std=c++17 -O1 -fPIC -shared -fvisibility=hidden -I $R/Engine/Source/WXL/Sdk -I $M $M/*.cpp -o libwxl_physics.so
//   g++ -std=c++17 -O1 -I $R/Engine/Source/WXL/Sdk host_test.cpp -ldl -o host_test
//   ./host_test ./libwxl_physics.so         (exit code 0 = every check passed)
//
// The host plays the core: it implements WxlApi on top of a fake model instance (a plume hanging from an anchor
// bone), loads the module the way the client's loader does (ABI check, init, event subscription), fires
// OnBuildBonePalette once per frame as a character jumps and climbs stairs, and checks what the module wrote
// back into the palette.
#include "wxl_api.h"

#include <dlfcn.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace
{
    int g_failed = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) ++g_failed;
    }

    // ---- the fake client ------------------------------------------------------------------------
    struct FakeInstance
    {
        std::string path = "item\\objectcomponents\\head\\plume_test.m2";
        uint32_t    bones = 5;                 // 0 root, 1 anchor, 2..4 plume segments
        uint32_t    clockMs = 0;
        float       own[16];                   // row-vector layout, translation in [12..14]
        std::vector<float> palette;            // bones x 16
        int         readStage = 0;             // what readInstance reports (0 = fine)
        const void* modelId = reinterpret_cast<const void*>(0x1000);
    };
    FakeInstance* g_inst = nullptr;

    std::map<std::string, std::map<std::string, std::string>> g_ini;
    std::vector<std::string> g_log;
    struct Sub { std::string event; WxlEventHandler fn; void* user; };
    std::vector<Sub> g_subs;
    bool g_virtualKeys = false;
    int g_blobsAllocated = 0, g_blobsFreed = 0;

    void ApiLog(const char* cat, const char* msg) { g_log.push_back(std::string(cat) + ": " + msg); }
    int  ApiThrottle(const char*, uint32_t, uint32_t) { return 1; }
    int  ApiSubscribe(const char* name, WxlEventHandler fn, void* user)
    {
        if (std::strcmp(name, WXL_EVENT_BUILD_BONE_PALETTE) != 0) return 2;   // the fake core knows one event
        g_subs.push_back({ name, fn, user });
        return 0;
    }
    int ApiIniString(const char* sec, const char* key, char* out, size_t cap)
    {
        auto s = g_ini.find(sec);
        if (s == g_ini.end()) return 0;
        auto k = s->second.find(key);
        if (k == s->second.end()) return 0;
        std::snprintf(out, cap, "%s", k->second.c_str());
        return 1;
    }
    int ApiIniInt(const char* sec, const char* key, int def)
    {
        char buf[64];
        return ApiIniString(sec, key, buf, sizeof(buf)) ? std::atoi(buf) : def;
    }
    int ApiIniBool(const char* sec, const char* key, int def) { return ApiIniInt(sec, key, def) != 0; }
    uint64_t ApiIniStamp() { return 1; }

    int ApiReadFile(const char* name, WxlBuffer* out)
    {
        std::string p = std::string("data/") + name;
        for (char& c : p) if (c == '\\') c = '/';
        std::ifstream f(p, std::ios::binary);
        if (!f) return 0;
        std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
        if (bytes.empty()) return 0;
        out->data = std::malloc(bytes.size());
        std::memcpy(out->data, bytes.data(), bytes.size());
        out->size = bytes.size();
        ++g_blobsAllocated;
        return 1;
    }
    void ApiFree(WxlBuffer* b) { if (b && b->data) { std::free(b->data); b->data = nullptr; b->size = 0; ++g_blobsFreed; } }
    int  ApiIsVirtual(const char*) { return g_virtualKeys ? 1 : 0; }

    int ApiReadInstance(void* inst, WxlInstanceView* v)
    {
        auto* i = static_cast<FakeInstance*>(inst);
        if (!v || v->structSize < sizeof(*v)) return WXL_READ_NO_MODEL;
        std::snprintf(v->path, sizeof(v->path), "%s", i->path.c_str());
        v->model = i->modelId;
        v->boneCount = i->bones;
        if (i->readStage) return i->readStage;
        v->clockMs = i->clockMs;
        const float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        std::memcpy(v->sceneMatrix, identity, sizeof(identity));   // camera space == world space in this fake
        std::memcpy(v->ownMatrix, i->own, sizeof(i->own));
        return WXL_READ_OK;
    }
    int ApiReadBones(void* inst, uint32_t count, float* palette, int16_t* parents, float* pivots)
    {
        auto* i = static_cast<FakeInstance*>(inst);
        if (count != i->bones) return WXL_READ_BONE_COUNT;
        if (palette) std::memcpy(palette, i->palette.data(), size_t(count) * 64);
        for (uint32_t k = 0; k < count; ++k)
        {
            // bone 0 root; 1 hangs from 0; 2 from 1; 3 from 2; 4 from 3
            if (parents) parents[k] = k == 0 ? -1 : int16_t(k - 1);
            if (pivots)
            {
                pivots[k * 3 + 0] = 0; pivots[k * 3 + 1] = 0;
                pivots[k * 3 + 2] = k >= 2 ? -0.25f * float(k - 2) : 0.0f;   // matches the .phy2 bodies' rest origins
            }
        }
        return WXL_READ_OK;
    }
    int ApiWritePalette(void* inst, const float* palette, uint32_t count)
    {
        auto* i = static_cast<FakeInstance*>(inst);
        if (count != i->bones) return WXL_READ_BONE_COUNT;
        std::memcpy(i->palette.data(), palette, size_t(count) * 64);
        return WXL_READ_OK;
    }

    WxlApi MakeApi()
    {
        WxlApi a = {};
        a.abiVersion = WXL_ABI_VERSION;
        a.structSize = sizeof(WxlApi);
        a.log = ApiLog; a.logThrottle = ApiThrottle; a.subscribe = ApiSubscribe;
        a.iniGetString = ApiIniString; a.iniGetInt = ApiIniInt; a.iniGetBool = ApiIniBool; a.iniStamp = ApiIniStamp;
        a.readFile = ApiReadFile; a.freeBuffer = ApiFree; a.isVirtualKey = ApiIsVirtual;
        a.readInstance = ApiReadInstance; a.readBones = ApiReadBones; a.writePalette = ApiWritePalette;
        return a;
    }

    void SetOwn(FakeInstance& i, double x, double y, double z)
    {
        const float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, float(x),float(y),float(z),1 };
        std::memcpy(i.own, m, sizeof(m));
    }
    // a still model: every bone's palette matrix is the instance's own matrix (no animation)
    void ResetPalette(FakeInstance& i)
    {
        i.palette.assign(size_t(i.bones) * 16, 0.0f);
        for (uint32_t k = 0; k < i.bones; ++k) std::memcpy(&i.palette[size_t(k) * 16], i.own, sizeof(i.own));
    }

    void Fire(FakeInstance& i)
    {
        WxlBuildBonePaletteArgs args = { &i };
        for (const Sub& s : g_subs) s.fn(s.user, &args);
    }

    double Tip(const FakeInstance& i)   // world position of the last segment's origin (bone 4 translation row)
    {
        const float* m = &i.palette[4 * 16];
        const float* own = i.own;
        const double dx = m[12] - own[12], dy = m[13] - own[13], dz = m[14] - own[14];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    struct Module
    {
        void* h = nullptr;
        WxlModuleAbiVersionFn abi = nullptr;
        WxlModuleInitFn init = nullptr;
        WxlModuleShutdownFn shutdown = nullptr;
    };
    Module Load(const char* path)
    {
        Module m;
        m.h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!m.h) { std::printf("dlopen failed: %s\n", dlerror()); return m; }
        m.abi = reinterpret_cast<WxlModuleAbiVersionFn>(dlsym(m.h, "WxlModuleAbiVersion"));
        m.init = reinterpret_cast<WxlModuleInitFn>(dlsym(m.h, "WxlModuleInit"));
        m.shutdown = reinterpret_cast<WxlModuleShutdownFn>(dlsym(m.h, "WxlModuleShutdown"));
        return m;
    }

    // Plays a jump (and then a rest) and returns the largest the plume strayed from hanging straight.
    struct Run { double maxStray = 0, finalStray = 0; bool nan = false; bool anchorTouched = false; uint32_t changedBones = 0; };
    Run Play(FakeInstance& i, double fps, double jumpSeconds, double holdSeconds)
    {
        Run r;
        const double dt = 1.0 / fps;
        for (double t = 0; t <= jumpSeconds + holdSeconds + 1e-9; t += dt)
        {
            const double tt = std::min(t, jumpSeconds);
            double z = 7.9555 * tt - 0.5 * 19.29 * tt * tt;
            if (z < 0) z = 0;
            SetOwn(i, -8913.2 + 7 * tt, 543.7, 122.4 + z);   // a real map position
            ResetPalette(i);                                  // what the engine just built
            const std::vector<float> before = i.palette;
            i.clockMs += uint32_t(dt * 1000.0 + 0.5);
            Fire(i);
            for (float v : i.palette) if (!(v == v)) r.nan = true;
            for (uint32_t k = 0; k < i.bones; ++k)
                if (std::memcmp(&before[size_t(k) * 16], &i.palette[size_t(k) * 16], 64) != 0)
                {
                    if (k <= 1) r.anchorTouched = true;
                    if (k >= 2) ++r.changedBones;
                }
            const double d = Tip(i);
            r.maxStray = std::max(r.maxStray, d);
            if (t > jumpSeconds) r.finalStray = d;
        }
        return r;
    }
}

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "./libwxl_physics.so";
    std::printf("module: %s\n", path);

    std::printf("1. loading, as the client's loader does\n");
    Module m = Load(path);
    Check(m.h && m.abi && m.init, "exports WxlModuleAbiVersion and WxlModuleInit");
    if (!m.h || !m.abi || !m.init) return 1;
    Check(m.abi() == WXL_ABI_VERSION, "reports the ABI it was built for");
    Check(dlsym(m.h, "Instance") == nullptr && dlsym(m.h, "_ZN3wxl6LogCatEPKcS1_z") == nullptr,
          "exports nothing but the module entry points (engine and helper symbols are hidden)");

    WxlApi api = MakeApi();
    g_ini["Physics"]["Enabled"] = "1";
    WxlModuleInfo info = {}; info.structSize = sizeof(info);

    WxlApi tooNew = api; tooNew.abiVersion = 99;
    Check(m.init(&tooNew, &info) != 0, "refuses a core with another ABI version (and subscribes to nothing)");
    Check(g_subs.empty(), "...so no handler is left behind");
    WxlApi tooShort = api; tooShort.structSize = offsetof(WxlApi, readBones);
    Check(m.init(&tooShort, &info) != 0, "refuses a core whose table lacks the model-instance calls");

    Check(m.init(&api, &info) == 0, "initializes against a good core");
    Check(std::strcmp(info.name, "physics") == 0 && info.versionMajor == 1, "names itself");
    Check(g_subs.size() == 1, "subscribed to OnBuildBonePalette once");

    std::printf("2. a plume on a jumping character, found through its own .phy2\n");
    FakeInstance inst; inst.palette.assign(size_t(inst.bones) * 16, 0.0f);
    SetOwn(inst, -8913.2, 543.7, 122.4); ResetPalette(inst);
    {
        inst.readStage = WXL_READ_NOT_ANIMATED;
        const auto before = inst.palette;
        Fire(inst);
        Check(inst.palette == before, "an instance that is not animated yet is left alone");
        inst.readStage = WXL_READ_OK;
    }
    Run run = Play(inst, 60, 1.0, 5.0);
    std::printf("     plume strays up to %.2f yd from hanging straight; %.3f yd at the end\n", run.maxStray, run.finalStray);
    Check(run.changedBones > 100, "the module rewrote the dynamic bones' palette entries");
    Check(!run.anchorTouched, "the root and the anchor bone were never touched");
    Check(!run.nan, "no NaN in the palette");
    Check(run.maxStray > 0.02, "the plume actually swings during the jump");
    Check(run.finalStray < 0.3, "and hangs near straight again once the character stops");
    Check(g_blobsAllocated > 0 && g_blobsAllocated == g_blobsFreed, "every buffer the core handed out went back through freeBuffer");
    bool matched = false;
    for (const auto& l : g_log) if (l.find("MATCH") != std::string::npos) matched = true;
    Check(matched, "logged the match through the core's log");

    std::printf("3. a virtual path uses the .phy2 of the base model\n");
    {
        FakeInstance v; v.path = "item\\objectcomponents\\head\\plume_test_12345.m2"; v.modelId = reinterpret_cast<const void*>(0x2000);
        v.palette.assign(size_t(v.bones) * 16, 0.0f);
        g_virtualKeys = false;
        Run noKey = Play(v, 60, 0.6, 0.2);
        Check(noKey.changedBones == 0, "the same name is just another model when it is not a minted key (no .phy2 of its own)");
        // the core answers the same way for a name every time, and the module caches by name: a minted key is a new name
        FakeInstance v2; v2.path = "item\\objectcomponents\\head\\plume_test_67890.m2"; v2.modelId = reinterpret_cast<const void*>(0x3000);
        v2.palette.assign(size_t(v2.bones) * 16, 0.0f);
        g_virtualKeys = true;
        Run withKey = Play(v2, 60, 0.6, 0.2);
        g_virtualKeys = false;
        Check(withKey.changedBones > 20, "once the core says it is a minted key, plume_test.phy2 (the base model's) drives it");
    }

    std::printf("4. models that must be left alone\n");
    {
        FakeInstance other; other.path = "creature\\wolf\\wolf.m2"; other.modelId = reinterpret_cast<const void*>(0x4000);
        other.palette.assign(size_t(other.bones) * 16, 0.0f);
        Check(Play(other, 60, 0.5, 0.1).changedBones == 0, "a model with no .phy2 is not touched");
    }

    std::printf("5. switched off in the ini\n");
    {
        // a second copy of the module under another file name has its own state: init it with Enabled=0
        std::system((std::string("cp ") + path + " ./libwxl_physics_off.so").c_str());
        Module off = Load("./libwxl_physics_off.so");
        Check(off.init != nullptr, "a second copy of the module loads");
        g_subs.clear();
        g_ini["Physics"]["Enabled"] = "0";
        WxlModuleInfo info2 = {}; info2.structSize = sizeof(info2);
        Check(off.init && off.init(&api, &info2) == 0, "initializes with [Physics] Enabled=0");
        FakeInstance idle; idle.modelId = reinterpret_cast<const void*>(0x6000);
        idle.palette.assign(size_t(idle.bones) * 16, 0.0f);
        Check(Play(idle, 60, 0.4, 0.1).changedBones == 0, "and then changes no palette");
        g_ini["Physics"]["Enabled"] = "1";
    }

    std::printf("6. WxlModuleShutdown\n");
    {
        Check(m.shutdown != nullptr, "exports WxlModuleShutdown");
        if (m.shutdown) m.shutdown();
        // the first copy's handler was dropped from g_subs when section 5 cleared it: fire it directly the way the engine would
        FakeInstance late; late.modelId = reinterpret_cast<const void*>(0x5000);
        late.palette.assign(size_t(late.bones) * 16, 0.0f);
        g_subs.clear();
        WxlApi api3 = MakeApi();
        Module again = Load(path);                       // same handle: same state, already shut down
        WxlModuleInfo info3 = {}; info3.structSize = sizeof(info3);
        Check(again.init && again.init(&api3, &info3) == 0, "a second init on a running module is harmless");
        Check(g_subs.empty(), "...and subscribes nothing new");
    }

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
