// Integration test of the weapon module and the in-tree side of its "weapon" service, end to end:
//   equip extension side:  wxl::scripts::weaponextension::WeaponGetVirtualPath(...)        (WXLEquip/WeaponService.cpp)
//        -> core service registry (WXL/Services.cpp)  -> module's WxlWeaponService  -> its sidecar + the vpath table
//
//   R=<repo root>; M=$R/Engine/Source/WXLModules/Weapon; T=$R/Tools/WxlModuleTest
//   g++ -std=c++17 -O1 -fPIC -shared -fvisibility=hidden -I $R/Engine/Source/WXL/Sdk -I $M -I $T/stub $M/*.cpp -o libwxl_weapon.so
//   g++ -std=c++17 -O1 -I $R -I $R/Engine/Source/WXL/Sdk $T/weapon_service_test.cpp $R/Engine/Source/WXL/Services.cpp \
//       $R/Engine/Source/WXL/Log.cpp $R/Engine/Source/WXLEquip/WeaponService.cpp -ldl -o weapon_service_test
//   ./weapon_service_test ./libwxl_weapon.so      (run it where data/WXLWeaponModels.csv exists: see make_weapon_data below)
#include "Engine/Source/WXL/Services.h"
#include "Engine/Source/WXLEquip/WeaponService.hpp"

#include "wxl_api.h"

#include <dlfcn.h>

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

    WxlEventHandler g_modelLoadPre = nullptr; void* g_user = nullptr;
    std::vector<std::string> g_bakedPaths;

    void ApiLog(const char*, const char*) {}
    int  ApiThrottle(const char*, uint32_t, uint32_t) { return 1; }
    int  ApiSubscribe(const char* name, WxlEventHandler fn, void* user)
    {
        if (std::strcmp(name, WXL_EVENT_MODEL_LOAD_PRE) != 0) return 2;
        g_modelLoadPre = fn; g_user = user; return 0;
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
        g_bakedPaths.push_back(p->realMdxPath ? p->realMdxPath : "");
        if (p->outVirtualPath && p->outVirtualPathSz)
            std::snprintf(p->outVirtualPath, p->outVirtualPathSz, "item\\objectcomponents\\weapon\\sword_test_%u%s.m2", p->itemDisplayId,
                          p->keySuffix ? p->keySuffix : "");
        return 1;
    }
    int ApiVPathPopulateGlobalSharedKey(const char*, const char*, uint32_t, const char*, const char*, const uint16_t*, uint32_t, char*, size_t) { return 0; }
    int ApiVPathPopulate(void*, const char*, const uint16_t*, uint32_t, const char*, uint32_t, const char*) { return 0; }
    void ApiVPathEvictCmo(void*) {}
    void ApiVPathRegisterLazyResolver(WxlVPathLazyResolver) {}
    int ApiVPathDecode(const char*, char*, size_t, uint32_t*) { return 0; }
    // the core's real registry, so the service goes through the same code the client uses
    int ApiRegisterService(const char* name, const void* table, size_t size) { return wxl::services::Register(name, table, size) ? 0 : 1; }
    const void* ApiGetService(const char* name, size_t* size) { return wxl::services::Get(name, size); }

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
}

int main(int argc, char** argv)
{
    namespace wx = wxl::scripts::weaponextension;
    const char* path = argc > 1 ? argv[1] : "./libwxl_weapon.so";

    std::printf("1. before the module is loaded\n");
    char out[300] = {};
    Check(!wx::WeaponGetVirtualPath(5001, 0, out, sizeof(out)), "the equip side gets false: no weapon service yet, so the native handling stays");
    Check(!wx::WeaponMirrorIsGeometric(), "and the mirror question has the safe answer");

    std::printf("2. loading the module\n");
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { std::printf("dlopen failed: %s\n", dlerror()); return 1; }
    auto init = reinterpret_cast<WxlModuleInitFn>(dlsym(h, "WxlModuleInit"));
    WxlApi api = MakeApi();
    WxlModuleInfo info = {}; info.structSize = sizeof(info);
    Check(init && init(&api, &info) == 0 && std::strcmp(info.name, "weapon") == 0, "WxlModuleInit succeeds");
    size_t size = 0;
    const void* svc = wxl::services::Get(WXL_SERVICE_WEAPON, &size);
    Check(svc && size == sizeof(WxlWeaponService), "the \"weapon\" service is in the core's registry, with the right size");

    std::printf("3. the equip side asks for a weapon the sidecar knows\n");
    WxlModelLoadArgs args = { nullptr };
    Check(g_modelLoadPre != nullptr, "the module bound OnModelLoadPre");
    if (g_modelLoadPre) g_modelLoadPre(g_user, &args);          // first model load: the module reads WXLWeaponModels.csv
    std::memset(out, 0, sizeof(out));
    const bool ok = wx::WeaponGetVirtualPath(5001, 0, out, sizeof(out));
    std::printf("     WeaponGetVirtualPath(5001, 0) -> %d '%s'\n", ok ? 1 : 0, out);
    Check(ok && std::strstr(out, "sword_test_5001") != nullptr, "returns the virtual path the core minted for the baked model");
    Check(!g_bakedPaths.empty() && g_bakedPaths.back().find("sword_test") != std::string::npos, "baked from the model path in WXLWeaponModels.csv");
    std::memset(out, 0, sizeof(out));
    Check(!wx::WeaponGetVirtualPath(99999, 0, out, sizeof(out)), "an unknown display id is false");
    bool mirrored = true;
    const bool off = wx::WeaponGetOffhandVirtualPath(5001, 0, out, sizeof(out), &mirrored);
    std::printf("     WeaponGetOffhandVirtualPath(5001, 0) -> %d (autoMirrored=%d)\n", off ? 1 : 0, mirrored ? 1 : 0);
    Check(!off || !mirrored, "the off-hand answer and its mirror flag cross the boundary as the module computed them");

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
