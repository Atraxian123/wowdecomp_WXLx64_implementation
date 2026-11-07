// End-to-end test of PhysicsOverride.csv through the C ABI, without the client (companion of host_test.cpp).
//
//   ./run_physics_override_tests.sh        (builds the module and this host with g++, runs it; exit code 0 = ok)
//
// The host plays the core: a wearer with several held-item instances that share ONE model file (the plume of
// make_test_data.py), the OnHandItemAttached event the equip extension fires, baked "<stem>_<displayId>.m2"
// paths, and a PhysicsOverride.csv in the working folder. The module is loaded the way the client's loader does.
// It checks that one display with GravityScale=-1 lifts its plume while a second display on the very same
// model hangs normally, for a plain model (event route) and a baked one (path route), that a creature row and a
// creature path never reach an item, that removing the weapon, a stale binding and a saved CSV all behave.
#include "wxl_api.h"

#include <dlfcn.h>

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace
{
    int g_failed = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) ++g_failed;
    }

    struct FakeInstance
    {
        std::string path;
        void*       parent = nullptr;       // the wearer, for a held weapon
        int         attachLink = -1;
        uint32_t    bones = 5;              // 0 root, 1 anchor, 2..4 plume segments
        uint32_t    clockMs = 0;
        float       own[16];
        std::vector<float> palette;
        const void* modelId = nullptr;
    };
    std::vector<FakeInstance*> g_all;

    std::map<std::string, std::map<std::string, std::string>> g_ini;
    std::vector<std::string> g_log;
    struct Sub { std::string event; WxlEventHandler fn; void* user; };
    std::vector<Sub> g_subs;
    std::set<std::string> g_virtual;          // paths the fake core says were minted by VirtualPath
    std::set<void*> g_swimming;               // instances the fake core says belong to a swimming unit
    std::map<void*, float> g_depth;           // ... and, where set, how deep (bodies below the surface); else 100 when swimming

    void ApiLog(const char* cat, const char* msg) { g_log.push_back(std::string(cat) + ": " + msg); }
    int  ApiThrottle(const char*, uint32_t, uint32_t) { return 1; }
    int  ApiSubscribe(const char* name, WxlEventHandler fn, void* user)
    {
        if (std::strcmp(name, WXL_EVENT_BUILD_BONE_PALETTE) != 0 && std::strcmp(name, WXL_EVENT_HAND_ITEM_ATTACHED) != 0 &&
            std::strcmp(name, WXL_EVENT_CREATURE_MODEL_BUILT) != 0) return 2;
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
        return 1;
    }
    void ApiFree(WxlBuffer* b) { if (b && b->data) { std::free(b->data); b->data = nullptr; b->size = 0; } }
    int  ApiIsVirtual(const char* name)
    {
        return name && g_virtual.count(name) ? 1 : 0;
    }
    const char* ApiWorkingDirectory() { return ""; }   // the module then looks in the current folder
    int ApiInstanceSwimming(void* inst) { return g_swimming.count(inst) ? 1 : 0; }
    float ApiInstanceWaterDepth(void* inst)
    {
        if (!g_swimming.count(inst)) return -1.0f;
        auto it = g_depth.find(inst);
        return it != g_depth.end() ? it->second : 100.0f;
    }

    // "<dir>\<stem>_<digits>.m2" -> id
    int ApiDecode(const char* path, char* out, size_t cap, uint32_t* id)
    {
        std::string p = path;
        const size_t dot = p.rfind(".m2");
        if (dot == std::string::npos) return 0;
        size_t b = dot;
        while (b > 0 && p[b - 1] >= '0' && p[b - 1] <= '9') --b;
        if (b == dot || b == 0 || p[b - 1] != '_') return 0;
        *id = uint32_t(std::atoi(p.substr(b, dot - b).c_str()));
        std::snprintf(out, cap, "%s", (p.substr(0, b - 1) + ".m2").c_str());
        return 1;
    }

    int ApiReadInstance(void* inst, WxlInstanceView* v)
    {
        auto* i = static_cast<FakeInstance*>(inst);
        if (!v || v->structSize < sizeof(*v)) return WXL_READ_NO_MODEL;
        std::snprintf(v->path, sizeof(v->path), "%s", i->path.c_str());
        v->model = i->modelId;
        v->boneCount = i->bones;
        v->clockMs = i->clockMs;
        const float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        std::memcpy(v->sceneMatrix, identity, sizeof(identity));
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
            if (parents) parents[k] = k == 0 ? -1 : int16_t(k - 1);
            if (pivots)
            {
                pivots[k * 3 + 0] = 0; pivots[k * 3 + 1] = 0;
                pivots[k * 3 + 2] = k >= 2 ? -0.25f * float(k - 2) : 0.0f;
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
    int ApiModelInfo(void* inst, WxlModelInfo* out)
    {
        for (FakeInstance* f : g_all)
            if (f == inst)
            {
                out->shared = const_cast<void*>(f->modelId);
                out->parent = f->parent;
                out->boneCount = f->bones;
                out->parsed = 1;
                return WXL_READ_OK;
            }
        return WXL_READ_NO_MODEL;
    }
    std::string Stem(const std::string& path)
    {
        size_t b = path.find_last_of("\\/");
        b = b == std::string::npos ? 0 : b + 1;
        const size_t e = path.find_last_of('.');
        std::string s = path.substr(b, e == std::string::npos || e < b ? std::string::npos : e - b);
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
    void* ApiFindAttachedChild(void* parent, uint32_t attachId, const char* name)
    {
        for (FakeInstance* f : g_all)
            if (f->parent == parent && f->attachLink == int(attachId) && Stem(f->path) == Stem(name)) return f;
        return nullptr;
    }
    int ApiItemDisplayInfo(uint32_t id, WxlItemDisplayInfo* out)
    {
        if (id < 1000 || id > 1999) return 0;
        std::snprintf(out->modelName[0], sizeof(out->modelName[0]), "plume_test.mdx");
        return 1;
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
        a.workingDirectory = ApiWorkingDirectory; a.vpathDecodeGlobalVirtualKey = ApiDecode;
        a.modelInfo = ApiModelInfo; a.findAttachedChild = ApiFindAttachedChild; a.itemDisplayInfo = ApiItemDisplayInfo;
        a.instanceSwimming = ApiInstanceSwimming;
        a.instanceWaterDepth = ApiInstanceWaterDepth;
        return a;
    }

    FakeInstance* NewInstance(const std::string& path, const void* modelId, void* parent = nullptr, int link = -1)
    {
        auto* i = new FakeInstance;
        i->path = path; i->modelId = modelId; i->parent = parent; i->attachLink = link;
        i->palette.assign(size_t(i->bones) * 16, 0.0f);
        g_all.push_back(i);
        return i;
    }
    void SetOwn(FakeInstance& i, double x, double y, double z)
    {
        const float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, float(x),float(y),float(z),1 };
        std::memcpy(i.own, m, sizeof(m));
    }
    void ResetPalette(FakeInstance& i)
    {
        i.palette.assign(size_t(i.bones) * 16, 0.0f);
        for (uint32_t k = 0; k < i.bones; ++k) std::memcpy(&i.palette[size_t(k) * 16], i.own, sizeof(i.own));
    }
    void FireBones(FakeInstance& i)
    {
        WxlBuildBonePaletteArgs args = { &i };
        for (const Sub& s : g_subs)
            if (s.event == WXL_EVENT_BUILD_BONE_PALETTE) s.fn(s.user, &args);
    }
    void FireHand(void* wearer, uint32_t slot, int link, uint32_t displayId, bool removed)
    {
        WxlHandItemAttachedArgs a = {};
        a.model = wearer; a.invSlot = slot; a.attachLink = removed ? -1 : link; a.displayId = removed ? 0 : displayId;
        a.removed = removed ? 1 : 0;
        for (const Sub& s : g_subs)
            if (s.event == WXL_EVENT_HAND_ITEM_ATTACHED) s.fn(s.user, &a);
    }

    void FireCreature(void* model, uint32_t displayId, bool mount, const char* fileName)
    {
        WxlCreatureModelBuiltArgs a = {};
        a.model = model; a.displayId = displayId; a.mount = mount ? 1 : 0; a.fileName = fileName;
        for (const Sub& s : g_subs)
            if (s.event == WXL_EVENT_CREATURE_MODEL_BUILT) s.fn(s.user, &a);
    }

    // world z of the last plume segment's pivot, relative to the instance origin: about -0.5 when it hangs,
    // positive when it lifts
    double TipHeight(const FakeInstance& i)
    {
        const float* m = &i.palette[4 * 16];
        const double pz = -0.5;
        return pz * m[10] + m[14] - i.own[14];
    }

    // a character that jumps and lands; the weapon follows. Returns the tip height at the end.
    double Play(FakeInstance& i, double seconds)
    {
        const double dt = 1.0 / 60.0;
        const double jump = 0.8;
        for (double t = 0; t <= seconds + 1e-9; t += dt)
        {
            const double tt = std::min(t, jump);
            double z = 7.9555 * tt - 0.5 * 19.29 * tt * tt;
            if (z < 0) z = 0;
            SetOwn(i, -8913.2 + 7 * tt, 543.7, 122.4 + z);
            ResetPalette(i);
            i.clockMs += uint32_t(dt * 1000.0 + 0.5);
            FireBones(i);
        }
        return TipHeight(i);
    }

    void WriteCsv(const std::string& text)
    {
        std::ofstream f("PhysicsOverride.csv", std::ios::binary | std::ios::trunc);
        f << text;
    }

    // The file needs the whole set of 14 parameters in every row. Row() writes one with only GravityScale (and
    // optionally Buoyancy) given; the other cells say Default (or are empty when `fill` is empty), i.e. "use the
    // ini's value".
    const char* const kHeader = "Kind,DisplayId,GravityScale,LinearDampingScale,AngularDampingScale,InertiaScale,MassScale,"
                                "FrictionScale,RestitutionScale,SwingLimitScale,TwistLimitScale,JointStiffnessScale,"
                                "JointDampingScale,MotorTorqueScale,FollowScale,Buoyancy,Note";
    std::string Row(const char* kind, int id, const char* gravity, const char* note = "", const char* fill = "Default",
                    const char* buoyancy = nullptr)
    {
        std::string r = std::string(kind) + "," + std::to_string(id) + "," + gravity;
        for (int i = 0; i < 12; ++i) r += std::string(",") + fill;
        r += std::string(",") + (buoyancy ? buoyancy : fill);   // the last parameter: Buoyancy
        return r + "," + note + "\n";
    }
    bool LogHas(const char* part)
    {
        for (const std::string& l : g_log) if (l.find(part) != std::string::npos) return true;
        return false;
    }
}

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "./libwxl_physics.so";
    std::printf("module: %s\n", path);

    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { std::printf("dlopen failed: %s\n", dlerror()); return 1; }
    auto init = reinterpret_cast<WxlModuleInitFn>(dlsym(h, "WxlModuleInit"));
    if (!init) { std::printf("no WxlModuleInit\n"); return 1; }

    // ---- the files ----
    WriteCsv(
        std::string("# per-display physics\n") + kHeader + "\n" +
        Row("Item", 1001, "-1", "upside down", "Default", "1") +       // a plain model, found through the attach event; Buoyancy 1: hangs in the water
        Row("Creature", 1002, "-1", "must never reach an item", "NA") +
        Row("Item", 1003, "-1", "a baked variant", "") +                // found through its virtual path; empty cells too
        Row("Item", 1005, "-1") +
        Row("Creature", 2001, "-1", "a plain creature model - found through the creature event") +
        Row("Creature", 2002, "-1", "a baked creature variant") +
        Row("Item", 1004, "nonsense", "bad cell: ignored - nothing else set so the row is skipped"));
    g_ini["Physics"]["Enabled"] = "1";
    g_ini["Physics"]["WatchIni"] = "1";
    // underwater, the gravity multiplier is the Buoyancy (it replaces GravityScale there): -1 by default, so loose parts
    // rise; the row of 1001 says Buoyancy 1 instead. BlendSeconds is short so a test second is plenty.
    g_ini["Physics"]["Buoyancy"] = "-1";
    g_ini["Physics.Underwater"]["BlendSeconds"] = "0.05";

    WxlApi api = MakeApi();
    WxlModuleInfo info = {}; info.structSize = sizeof(info);

    std::printf("1. loading\n");
    Check(init(&api, &info) == 0, "initializes against the fake core");
    int boneSubs = 0, handSubs = 0, creatureSubs = 0;
    for (const Sub& s : g_subs)
    {
        boneSubs += s.event == WXL_EVENT_BUILD_BONE_PALETTE;
        handSubs += s.event == WXL_EVENT_HAND_ITEM_ATTACHED;
        creatureSubs += s.event == WXL_EVENT_CREATURE_MODEL_BUILT;
    }
    Check(boneSubs == 1 && handSubs == 1 && creatureSubs == 1, "subscribes to OnBuildBonePalette, OnHandItemAttached and OnCreatureModelBuilt once each");
    Check(LogHas("3 creature row(s)") && LogHas("PhysicsOverride.csv"), "reads the csv and counts its rows");
    Check(LogHas("GravityScale='nonsense' is not a number"), "reports the bad cell with its line");

    const char* kPlain = "item\\objectcomponents\\head\\plume_test.m2";
    const void* kModel = reinterpret_cast<const void*>(0x1000);   // ONE loaded model file shared by every plain weapon
    FakeInstance* wearer = NewInstance("character\\human\\male\\humanmale.m2", reinterpret_cast<const void*>(0x9000));
    FakeInstance* wearer2 = NewInstance("character\\human\\male\\humanmale.m2", reinterpret_cast<const void*>(0x9000));

    std::printf("2. two displays, one model file\n");
    FakeInstance* a = NewInstance(kPlain, kModel, wearer, 1);    // display 1001, right hand
    FakeInstance* b = NewInstance(kPlain, kModel, wearer2, 1);   // display 1002, right hand, another wearer
    FireHand(wearer, 15, 1, 1001, false);
    FireHand(wearer2, 15, 1, 1002, false);
    const double tipA = Play(*a, 6.0);
    const double tipB = Play(*b, 6.0);
    std::printf("     tip height: display 1001 -> %.3f   display 1002 -> %.3f\n", tipA, tipB);
    Check(tipB < -0.2, "display 1002 (no item row) hangs: its own creature row did not reach it");
    // the plume's joints are limited to 45 degrees each, so it cannot stand fully upright: -0.5 hanging, about -0.15 lifted
    Check(tipA > tipB + 0.25, "display 1001 (GravityScale=-1) lifts on the very same model file");
    Check(LogHas("item display 1001 on") && LogHas("GravityScale=-1"), "the log names the display and what it changed");
    Check(!LogHas("item display 1002 on"), "...and says nothing about display 1002");

    std::printf("3. a baked variant (virtual path) and the creature-path guard\n");
    const char* kBaked = "item\\objectcomponents\\head\\plume_test_1003.m2";
    g_virtual.insert(kBaked);
    FakeInstance* c = NewInstance(kBaked, reinterpret_cast<const void*>(0x2000));
    const double tipC = Play(*c, 6.0);
    Check(tipC > tipB + 0.25, "display 1003 reached through its baked path lifts");
    const char* kCreature = "creature\\foo\\plume_test_1003.m2";   // same shape, but not under the item folder
    g_virtual.insert(kCreature);
    FakeInstance* d = NewInstance(kCreature, reinterpret_cast<const void*>(0x3000));
    const double tipD = Play(*d, 6.0);
    const bool creatureFound = LogHas("MATCH 'creature");
    if (!creatureFound) std::printf("     (no .phy2 for the creature path in this folder: the guard cannot be seen here)\n");
    Check(!creatureFound || tipD < -0.2, "a baked CREATURE path with the same id gets no item row");

    std::printf("4. the weapon is removed, or its binding goes stale\n");
    FireHand(wearer, 15, 1, 0, true);
    const double tipA2 = Play(*a, 6.0);
    Check(tipA2 < -0.2, "after the removal event the instance is back to the global multipliers");
    FireHand(wearer, 15, 1, 1001, false);                       // equipped again
    Check(Play(*a, 6.0) > tipB + 0.25, "...and equipping it again brings the row back");
    a->parent = wearer2;                                        // the address is recycled by something else
    FireHand(wearer2, 16, 2, 1005, false);                      // any change makes the module look again
    Check(Play(*a, 6.0) < -0.2, "a binding whose weapon is no longer the wearer's child is dropped");

    std::printf("4b. creatures: two displays on one model file, a baked variant, a mount\n");
    const char* kCreaturePlain = "creature\\foo\\plume_test.m2";
    const void* kCreatureModel = reinterpret_cast<const void*>(0x4000);          // ONE creature model file, shared
    FakeInstance* e1 = NewInstance(kCreaturePlain, kCreatureModel);
    FakeInstance* e2 = NewInstance(kCreaturePlain, kCreatureModel);
    FireCreature(e1, 2001, false, kCreaturePlain);
    FireCreature(e2, 2003, false, kCreaturePlain);
    const double tipE1 = Play(*e1, 6.0);
    const double tipE2 = Play(*e2, 6.0);
    std::printf("     tip height: creature display 2001 -> %.3f   2003 -> %.3f\n", tipE1, tipE2);
    Check(tipE2 < -0.2, "creature display 2003 (no row) hangs");
    Check(tipE1 > tipE2 + 0.25, "creature display 2001 (row) lifts on the very same model file");
    Check(LogHas("creature display 2001 on"), "the log says creature display 2001");

    const char* kBakedCreature = "creature\\foo\\plume_test_2002.m2";
    g_virtual.insert(kBakedCreature);
    FakeInstance* e3 = NewInstance(kBakedCreature, reinterpret_cast<const void*>(0x5000));
    Check(Play(*e3, 6.0) > tipE2 + 0.25, "a baked creature variant (display 2002) is found through its path");

    FakeInstance* e4 = NewInstance(kCreaturePlain, kCreatureModel);
    FireCreature(e4, 1001, false, kCreaturePlain);                              // 1001 has an ITEM row only
    Check(Play(*e4, 6.0) < -0.2, "an item row never reaches a creature display with the same id");
    FakeInstance* e5 = NewInstance(kCreaturePlain, kCreatureModel);
    FireCreature(e5, 1002, false, kCreaturePlain);                              // 1002 has a CREATURE row (and no item row)
    Check(Play(*e5, 6.0) > tipE2 + 0.25, "creature display 1002 takes its creature row, while item display 1002 (section 2) did not");

    FakeInstance* mount = NewInstance(kCreaturePlain, kCreatureModel);
    FireCreature(mount, 2001, true, kCreaturePlain);
    Check(Play(*mount, 6.0) > tipE2 + 0.25, "a mount's model (mount = 1) takes the row of its creature display");

    // the model address is reused by something else: another file, another loaded model
    e1->path = kPlain;
    e1->modelId = reinterpret_cast<const void*>(0x6000);
    Check(Play(*e1, 6.0) < -0.2, "a creature binding is dropped when the instance now shows another file");

    std::printf("4c. underwater: Buoyancy replaces the gravity multiplier\n");
    a->parent = wearer;
    FireHand(wearer, 15, 1, 1001, false);                       // display 1001: its row says GravityScale -1
    const double dry1001 = Play(*a, 6.0);
    const double dry1002 = Play(*b, 6.0);                       // display 1002: no item row, the ini's gravity 1
    Check(dry1001 > dry1002 + 0.25, "dry: 1001 (row -1) lifts, 1002 (ini 1) hangs");
    g_swimming.insert(a);
    g_swimming.insert(b);
    const double wet1001 = Play(*a, 6.0);
    const double wet1002 = Play(*b, 6.0);
    std::printf("     swimming: 1001 -> %.3f   1002 -> %.3f\n", wet1001, wet1002);
    Check(wet1001 < -0.2, "swimming: the row's Buoyancy 1 replaces its GravityScale -1, so 1001 now hangs");
    Check(wet1002 > wet1001 + 0.25, "swimming: the ini's Buoyancy -1 replaces the gravity 1, so 1002 now lifts (a row is not needed for it)");
    Check(LogHas("enters the water"), "the log says a model entered the water");
    g_swimming.clear();
    Check(Play(*a, 6.0) > wet1001 + 0.25, "out of the water again: 1001 is back to its row");
    Check(Play(*b, 6.0) < -0.2, "...and 1002 hangs again");

    std::printf("4d. swimming at the water level: the depth scales the effect\n");
    g_swimming.insert(b);
    g_depth[b] = 0.75f;                                        // a swimmer's feet: the water level
    const double surf1002 = Play(*b, 6.0);
    g_depth[b] = 1.4f;                                         // diving
    const double dive1002 = Play(*b, 6.0);
    std::printf("     1002 at the water level -> %.3f   diving -> %.3f\n", surf1002, dive1002);
    Check(surf1002 < -0.2, "at the water level (0.75 bodies) nothing floats up: 1002 hangs");
    Check(dive1002 > surf1002 + 0.25, "diving (1.4 bodies): 1002 lifts with its Buoyancy");
    g_depth.clear();
    g_swimming.clear();

    std::printf("5. the csv is saved while the game runs (WatchIni=1)\n");
    a->parent = wearer;
    FireHand(wearer, 15, 1, 1001, false);
    Check(Play(*a, 6.0) > tipB + 0.25, "before: 1001 inverted");
    WriteCsv(std::string(kHeader) + "\n" + Row("Item", 1002, "-1"));      // swap: 1002 inverted, 1001 normal
    std::this_thread::sleep_for(std::chrono::milliseconds(1300)); // the module looks once a second
    const double tipA3 = Play(*a, 6.0);
    const double tipB3 = Play(*b, 6.0);
    std::printf("     after the save: 1001 -> %.3f   1002 -> %.3f\n", tipA3, tipB3);
    Check(tipA3 < -0.2, "after the save 1001 hangs");
    Check(tipB3 > tipA3 + 0.25, "...and 1002 lifts");
    Check(LogHas("csv saved"), "the reload is logged");

    std::printf("6. a file without the whole set, then no file at all\n");
    WriteCsv("Kind,DisplayId,GravityScale\nItem,1002,-1\n");              // the header lacks 13 of the 14 parameters
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    const double tipB4 = Play(*b, 6.0);   // the module looks at the file while a frame runs
    Check(LogHas("must list the whole set") && LogHas("missing: LinearDampingScale"), "a header without the whole set is refused, and the log names what is missing");
    Check(tipB4 < -0.2, "...and nothing of that file applies (1002 hangs)");
    std::remove("PhysicsOverride.csv");
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    Play(*b, 6.0);
    Check(LogHas("no PhysicsOverride.csv") && Play(*b, 6.0) < -0.2, "deleting the file switches the overrides off");

    for (FakeInstance* f : g_all) delete f;
    g_all.clear();
    if (std::getenv("DUMP_LOG")) for (const std::string& l : g_log) std::printf("LOG %s\n", l.c_str());
    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
