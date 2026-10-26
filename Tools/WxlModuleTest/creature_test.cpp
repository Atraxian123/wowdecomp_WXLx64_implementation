// End-to-end test of the creature module through the C ABI, with the project's own sidecar CSVs.
//
//   R=<repo root>; M=$R/Engine/Source/WXLModules/Creature; T=$R/Tools/WxlModuleTest
//   g++ -std=c++17 -O1 -fPIC -shared -fvisibility=hidden -I $R/Engine/Source/WXL/Sdk -I $M -I $T/stub $M/*.cpp -o libwxl_creature.so
//   g++ -std=c++17 -O1 -I $R/Engine/Source/WXL/Sdk $T/creature_test.cpp -ldl -o creature_test
//   ./creature_test ./libwxl_creature.so          (run it where data/DBFilesClient/WXLCreature*.csv are)
//
// The host plays the core: files come from data/, the virtual-path table is a recorder, and the test checks what
// the module asks the core to bake for display 33112 (Manawyrm mount: geosets 101 and 201, four texture layers).
#include "wxl_api.h"

#include <dlfcn.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    int g_failed = 0;
    void Check(bool ok, const char* what) { std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what); if (!ok) ++g_failed; }

    struct Bake { std::string path, tex, spec; uint32_t displayId; std::vector<uint16_t> geo; bool evictable; std::string pool; };
    std::vector<Bake> g_bakes;
    std::vector<std::string> g_log;
    WxlEventHandler g_modelLoadPre = nullptr; void* g_modelLoadPreUser = nullptr;
    int (*g_resolver)(const char*) = nullptr;
    int g_resolverRegistrations = 0;

    void ApiLog(const char* cat, const char* msg) { g_log.push_back(std::string(cat) + ": " + msg); }
    int  ApiThrottle(const char*, uint32_t, uint32_t) { return 1; }
    int  ApiSubscribe(const char* name, WxlEventHandler fn, void* user)
    {
        if (std::strcmp(name, WXL_EVENT_MODEL_LOAD_PRE) != 0) return 2;
        g_modelLoadPre = fn; g_modelLoadPreUser = user; return 0;
    }
    int ApiIniString(const char*, const char*, char*, size_t) { return 0; }
    int ApiIniInt(const char*, const char*, int def) { return def; }
    int ApiIniBool(const char*, const char*, int def) { return def; }
    uint64_t ApiIniStamp() { return 1; }
    int ApiReadFile(const char* name, WxlBuffer* out)
    {
        std::string p = std::string("data/") + name;
        for (char& c : p) if (c == '\\') c = '/';
        std::ifstream f(p, std::ios::binary);
        if (!f) return 0;
        std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
        if (bytes.empty()) return 0;
        out->data = std::malloc(bytes.size()); std::memcpy(out->data, bytes.data(), bytes.size()); out->size = bytes.size();
        return 1;
    }
    void ApiFree(WxlBuffer* b) { if (b && b->data) { std::free(b->data); b->data = nullptr; b->size = 0; } }
    int  ApiIsVirtual(const char*) { return 0; }
    int  ApiReadInstance(void*, WxlInstanceView*) { return WXL_READ_NO_MODEL; }
    int  ApiReadBones(void*, uint32_t, float*, int16_t*, float*) { return WXL_READ_NO_MODEL; }
    int  ApiWritePalette(void*, const float*, uint32_t) { return WXL_READ_NO_MODEL; }
    const char* ApiWorkingDirectory() { return "."; }

    int ApiVPathPopulateGlobal(const WxlVPathGlobal* p)
    {
        Bake b;
        b.path = p->realMdxPath ? p->realMdxPath : ""; b.tex = p->texPath ? p->texPath : ""; b.spec = p->materialPatchSpec ? p->materialPatchSpec : "";
        b.displayId = p->itemDisplayId; b.evictable = p->evictable != 0; b.pool = p->evictionPool ? p->evictionPool : "";
        for (uint32_t i = 0; i < p->geoCount; ++i) b.geo.push_back(p->geoIds[i]);
        g_bakes.push_back(b);
        if (p->outVirtualPath && p->outVirtualPathSz)
            std::snprintf(p->outVirtualPath, p->outVirtualPathSz, "creature\\manawyrm2mount\\manawyrm2mount_%u.m2", p->itemDisplayId);
        return 1;
    }
    int ApiVPathPopulateGlobalSharedKey(const char*, const char*, uint32_t, const char*, const char*, const uint16_t*, uint32_t, char*, size_t) { return 0; }
    int ApiVPathPopulate(void*, const char*, const uint16_t*, uint32_t, const char*, uint32_t, const char*) { return 0; }
    void ApiVPathEvictCmo(void*) {}
    void ApiVPathRegisterLazyResolver(WxlVPathLazyResolver r) { g_resolver = r; ++g_resolverRegistrations; }
    // "<dir>\<stem>_<displayId>.m2" -> "<dir>\<stem>.m2" + displayId, as the core's decoder does
    int ApiVPathDecode(const char* name, char* outNorm, size_t outSz, uint32_t* outDisplayId)
    {
        std::string s = name;
        const size_t dot = s.rfind(".m2");
        if (dot == std::string::npos) return 0;
        const size_t us = s.rfind('_', dot);
        if (us == std::string::npos || us + 1 >= dot) return 0;
        for (size_t i = us + 1; i < dot; ++i) if (s[i] < '0' || s[i] > '9') return 0;
        if (outDisplayId) *outDisplayId = uint32_t(std::strtoul(s.substr(us + 1, dot - us - 1).c_str(), nullptr, 10));
        std::snprintf(outNorm, outSz, "%s", (s.substr(0, us) + ".m2").c_str());
        return 1;
    }
    int ApiRegisterService(const char*, const void*, size_t) { return 0; }
    const void* ApiGetService(const char*, size_t* size) { if (size) *size = 0; return nullptr; }

    WxlApi MakeApi()
    {
        WxlApi a = {};
        a.abiVersion = WXL_ABI_VERSION; a.structSize = sizeof(WxlApi);
        a.log = ApiLog; a.logThrottle = ApiThrottle; a.subscribe = ApiSubscribe;
        a.iniGetString = ApiIniString; a.iniGetInt = ApiIniInt; a.iniGetBool = ApiIniBool; a.iniStamp = ApiIniStamp;
        a.readFile = ApiReadFile; a.freeBuffer = ApiFree; a.isVirtualKey = ApiIsVirtual;
        a.readInstance = ApiReadInstance; a.readBones = ApiReadBones; a.writePalette = ApiWritePalette;
        a.workingDirectory = ApiWorkingDirectory;
        a.vpathPopulateGlobal = ApiVPathPopulateGlobal; a.vpathPopulateGlobalSharedKey = ApiVPathPopulateGlobalSharedKey;
        a.vpathPopulate = ApiVPathPopulate; a.vpathEvictCmo = ApiVPathEvictCmo; a.vpathRegisterLazyResolver = ApiVPathRegisterLazyResolver;
        a.vpathDecodeGlobalVirtualKey = ApiVPathDecode; a.registerService = ApiRegisterService; a.getService = ApiGetService;
        return a;
    }

    bool LogHas(const char* needle) { for (const auto& l : g_log) if (l.find(needle) != std::string::npos) return true; return false; }
}

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "./libwxl_creature.so";
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { std::printf("dlopen failed: %s\n", dlerror()); return 1; }
    auto abi = reinterpret_cast<WxlModuleAbiVersionFn>(dlsym(h, "WxlModuleAbiVersion"));
    auto init = reinterpret_cast<WxlModuleInitFn>(dlsym(h, "WxlModuleInit"));

    std::printf("1. loading\n");
    Check(abi && init && abi() == WXL_ABI_VERSION, "exports the entry points and the right ABI");
    WxlApi api = MakeApi();
    WxlModuleInfo info = {}; info.structSize = sizeof(info);
    WxlApi old = api; old.structSize = offsetof(WxlApi, workingDirectory);
    Check(init(&old, &info) != 0 && !g_modelLoadPre, "refuses a core that lacks the vpath/service members (subscribes to nothing)");
    Check(init(&api, &info) == 0 && std::strcmp(info.name, "creature") == 0, "initializes against the full table");
    Check(g_modelLoadPre != nullptr, "binds its OnModelLoadPre handler through EventScript::on<>");
    Check(g_resolverRegistrations == 1 && g_resolver, "registers its lazy resolver once, through the trampoline");
    Check(g_bakes.empty(), "bakes nothing at init (no I/O before the client is up)");

    std::printf("2. the first model load reads the sidecars\n");
    WxlModelLoadArgs args = { nullptr };
    g_modelLoadPre(g_modelLoadPreUser, &args);
    Check(LogHas("creature sidecar table ready"), "loads WXLCreatureModels.csv and WXLCreatureTextures.csv");
    for (const auto& l : g_log) if (l.find("sidecar table ready") != std::string::npos) std::printf("     %s\n", l.c_str());

    std::printf("3. the lazy resolver bakes display 33112 when the client asks for its virtual path\n");
    const char* vname = "creature\\manawyrm2mount\\manawyrm2mount_33112.m2";
    Check(g_resolver(vname) == 1, "the resolver answers 1 (baked, the open can go on)");
    Check(g_bakes.size() >= 1, "the core was asked to bake");
    if (!g_bakes.empty())
    {
        const Bake& b = g_bakes.back();
        std::printf("     bake: display %u, model '%s', geosets", b.displayId, b.path.c_str());
        for (uint16_t g : b.geo) std::printf(" %u", g);
        std::printf(", pool '%s'\n     material spec: %s\n", b.pool.c_str(), b.spec.c_str());
        Check(b.displayId == 33112, "for display 33112");
        Check(b.path.find("manawyrm2mount") != std::string::npos, "from the model path in WXLCreatureModels.csv");
        bool has101 = false, has201 = false;
        for (uint16_t g : b.geo) { has101 |= g == 101; has201 |= g == 201; }
        Check(has101 && has201, "keeping geosets 101 and 201");
        Check(b.spec.find("11=") != std::string::npos && b.spec.find("manawyrm2mount_body_white.blp") != std::string::npos &&
              b.spec.find("4=") != std::string::npos && b.spec.find("manawyrm2mount_armor_1.blp") != std::string::npos,
              "with the texture layers of WXLCreatureTextures.csv (types 11, 12, 13 and the hardcoded 4)");
        Check(b.pool == "Creature", "in the Creature eviction pool");
    }
    const size_t before = g_bakes.size();
    Check(g_resolver("creature\\wolf\\wolf_999999.m2") == 0, "a display that is not in the sidecars is not baked");
    Check(g_bakes.size() == before, "...and nothing was asked of the core for it");
    Check(g_resolver("creature\\manawyrm2mount\\manawyrm2mount.m2") == 0, "a plain model name (no display id) is not a virtual key");

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
