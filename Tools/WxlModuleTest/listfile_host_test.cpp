// End-to-end test of the listfile module (wxl_listfile) through the C ABI, without the client.
//
//   R=<repo root>; M=$R/Engine/Source/WXLModules/Listfile
//   g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -I $R/Engine/Source/WXL/Sdk -I $M $M/*.cpp -o libwxl_listfile.so
//   g++ -std=c++17 -O2 -pthread -I $R/Engine/Source/WXL/Sdk listfile_host_test.cpp -ldl -o listfile_host_test
//   ./listfile_host_test ./libwxl_listfile.so <listfile snippet.csv> <scenario>
//   or: run_listfile_tests.sh, which runs every scenario (exit code 0 = every check passed)
//
// The module keeps process-wide state (like it does in the client), so each scenario is its own process:
//   eager        defaults: listfile at <exe folder>\WXL\listfile.csv, table built at load; lookups, not-found names, reload
//   lazy         [Listfile] Eager=0: nothing read at load, each lookup reads the file, answers cached
//   formats      header row, comma delimiter, BOM, duplicates, unsorted ids, junk lines, no final newline
//   noexists     [Listfile] CheckExists=0
//   customname   [Listfile] NotFoundName=...
//   missingfile  there is no listfile at all
//   scale        a 3,000,000-line listfile: build time, memory, random lookups, a full runtime scan
#include "wxl_api.h"
#include "wxl_listfile_api.h"

#include <dlfcn.h>
#include <limits.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
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

    // ---- the fake client ------------------------------------------------------------------------
    std::mutex                                  g_lock;
    std::map<std::string, std::map<std::string, std::string>> g_ini;
    std::vector<std::string>                    g_log;
    std::set<std::string>                       g_clientFiles;   // what the "archives" hold, normalized
    struct Provider { WxlFileProviderFn fn; void* user; };
    std::vector<Provider>                       g_providers;
    std::map<std::string, const void*>          g_services;
    std::atomic<int>                            g_reserveCalls{ 0 };

    std::string Norm(const char* name)
    {
        std::string s(name);
        for (char& c : s) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    void ApiLog(const char* cat, const char* msg)
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_log.push_back(std::string(cat) + ": " + msg);
    }
    int ApiThrottle(const char*, uint32_t, uint32_t) { return 1; }
    int ApiSubscribe(const char*, WxlEventHandler, void*) { return 2; }

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

    void* SinkReserve(void* ctx, size_t size)
    {
        ++g_reserveCalls;
        auto* v = static_cast<std::vector<uint8_t>*>(ctx);
        v->resize(size);
        return v->data();
    }
    bool OpenThroughProviders(const char* name, std::vector<uint8_t>& out)
    {
        std::vector<Provider> providers;
        { std::lock_guard<std::mutex> l(g_lock); providers = g_providers; }
        const WxlServeSink sink = { &out, &SinkReserve };
        for (const Provider& p : providers)
        {
            out.clear();
            if (p.fn(p.user, name, &sink) && !out.empty()) return true;
            out.clear();
        }
        return false;
    }
    int ApiReadFile(const char*, WxlBuffer* out) { if (out) { out->data = nullptr; out->size = 0; } return 0; }
    void ApiFreeBuffer(WxlBuffer*) {}
    int ApiIsVirtualKey(const char*) { return 0; }
    int ApiReadInstance(void*, WxlInstanceView*) { return WXL_READ_NO_MODEL; }
    int ApiReadBones(void*, uint32_t, float*, int16_t*, float*) { return WXL_READ_NO_MODEL; }
    int ApiWritePalette(void*, const float*, uint32_t) { return WXL_READ_NO_MODEL; }

    int ApiRegisterProvider(WxlFileProviderFn fn, void* user)
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_providers.push_back({ fn, user });
        return 0;
    }
    int ApiRegisterQuery(WxlVirtualKeyFn, void*) { return 0; }
    int ApiRegisterService(const char* name, const void* table, size_t)
    {
        std::lock_guard<std::mutex> l(g_lock);
        return g_services.emplace(name, table).second ? 0 : 2;
    }
    const void* ApiGetService(const char* name, size_t* size)
    {
        if (size) *size = 0;
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_services.find(name);
        return it == g_services.end() ? nullptr : it->second;
    }
    // Like the client: modules' virtual files count as there, then the archives.
    int ApiFileExists(const char* name)
    {
        if (!name) return 0;
        std::vector<uint8_t> tmp;
        if (OpenThroughProviders(name, tmp)) return 1;
        std::lock_guard<std::mutex> l(g_lock);
        return g_clientFiles.count(Norm(name)) ? 1 : 0;
    }

    WxlApi MakeApi()
    {
        WxlApi api = {};
        api.abiVersion = WXL_ABI_VERSION;
        api.structSize = sizeof(WxlApi);
        api.log = &ApiLog;
        api.logThrottle = &ApiThrottle;
        api.subscribe = &ApiSubscribe;
        api.iniGetString = &ApiIniString;
        api.iniGetInt = &ApiIniInt;
        api.iniGetBool = &ApiIniBool;
        api.iniStamp = &ApiIniStamp;
        api.readFile = &ApiReadFile;
        api.freeBuffer = &ApiFreeBuffer;
        api.isVirtualKey = &ApiIsVirtualKey;
        api.readInstance = &ApiReadInstance;
        api.readBones = &ApiReadBones;
        api.writePalette = &ApiWritePalette;
        api.registerFileProvider = &ApiRegisterProvider;
        api.registerVirtualKeyQuery = &ApiRegisterQuery;
        api.registerService = &ApiRegisterService;
        api.getService = &ApiGetService;
        api.fileExists = &ApiFileExists;
        return api;
    }

    size_t LogCount() { std::lock_guard<std::mutex> l(g_lock); return g_log.size(); }
    bool LogHas(const std::string& needle)
    {
        std::lock_guard<std::mutex> l(g_lock);
        for (const std::string& line : g_log) if (line.find(needle) != std::string::npos) return true;
        return false;
    }
    void DumpLog()
    {
        std::printf("  --- log ---\n");
        for (const std::string& line : g_log) std::printf("  | %s\n", line.c_str());
    }

    // ---- helpers ------------------------------------------------------------------------------
    std::string ReadAll(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
    void WriteAll(const std::string& path, const std::string& text)
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << text;
    }
    std::string ExeFolder()
    {
        char buf[PATH_MAX] = {};
        const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        std::string p(buf, n > 0 ? static_cast<size_t>(n) : 0);
        return p.substr(0, p.find_last_of('/') + 1);
    }
    double Seconds(std::chrono::steady_clock::time_point t0)
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    long CurrentRssMb()
    {
        FILE* f = std::fopen("/proc/self/status", "r");
        char line[256];
        long kb = 0;
        while (f && std::fgets(line, sizeof(line), f)) if (std::sscanf(line, "VmRSS: %ld kB", &kb) == 1) break;
        if (f) std::fclose(f);
        return kb / 1024;
    }
    long PeakRssMb()
    {
        rusage ru = {};
        getrusage(RUSAGE_SELF, &ru);
        return ru.ru_maxrss / 1024;   // KB on Linux
    }

    struct Loaded
    {
        void* dll = nullptr;
        WxlModuleInitFn init = nullptr;
        WxlModuleShutdownFn shutdown = nullptr;
        WxlApi api = {};
        const WxlListfileServices* lf = nullptr;
        int rc = -1;
    };
    Loaded Load(const char* dllPath)
    {
        Loaded m;
        m.dll = dlopen(dllPath, RTLD_NOW | RTLD_LOCAL);
        if (!m.dll) { std::printf("dlopen failed: %s\n", dlerror()); std::exit(2); }
        m.init     = reinterpret_cast<WxlModuleInitFn>(dlsym(m.dll, "WxlModuleInit"));
        m.shutdown = reinterpret_cast<WxlModuleShutdownFn>(dlsym(m.dll, "WxlModuleShutdown"));
        return m;
    }
    void Init(Loaded& m)
    {
        m.api = MakeApi();
        WxlModuleInfo info = {}; info.structSize = sizeof(info);
        m.rc = m.init(&m.api, &info);
        m.lf = WxlListfileGet(&m.api);
    }

    // The standard client contents for the scenarios.
    void StockClient()
    {
        g_clientFiles.insert(Norm("Interface\\Cinematics\\logo_800.avi"));
        g_clientFiles.insert(Norm("Sound\\Music\\CityMusic\\Darnassus\\Darnassus Intro.mp3"));
    }

    // ---- shared checks ---------------------------------------------------------------------
    void CheckSnippetLookups(const WxlListfileServices* lf)
    {
        char out[260];
        Check(lf->lookupPath(1, out, sizeof(out)) == 1 && std::strcmp(out, "interface\\cinematics\\logo_800.avi") == 0,
              "lookupPath(1): 'interface\\cinematics\\logo_800.avi' (forward slashes became backslashes)");
        Check(lf->lookupPath(2, out, sizeof(out)) == 1 && std::strcmp(out, "interface\\cinematics\\wow_intro_800.avi") == 0, "lookupPath(2)");
        Check(lf->lookupPath(21, out, sizeof(out)) == 1 && std::strcmp(out, "interface\\cinematics\\logo_1024.avi") == 0, "lookupPath(21)");
        Check(lf->lookupPath(53183, out, sizeof(out)) == 1 &&
              std::strcmp(out, "sound\\music\\citymusic\\darnassus\\darnassus intro.mp3") == 0, "lookupPath(53183): a path with a space");
        Check(lf->lookupPath(53187, out, sizeof(out)) == 1 &&
              std::strcmp(out, "sound\\music\\citymusic\\darnassus\\druid grove.mp3") == 0, "lookupPath(53187): the last line (CRLF stripped)");
        Check(lf->lookupPath(3, out, sizeof(out)) == 0, "lookupPath(3): an id that is not in the file is 'not found'");
        Check(lf->lookupPath(0, out, sizeof(out)) == 0, "lookupPath(0): not found");
        Check(lf->lookupPath(4294967295u, out, sizeof(out)) == 0, "lookupPath(0xFFFFFFFF): not found");
        char tiny[8];
        Check(lf->lookupPath(1, tiny, sizeof(tiny)) == -1, "a buffer that is too small is an error, not a cut-off path");
        Check(lf->lookupPath(1, nullptr, 10) == -1 && lf->lookupPath(1, out, 0) == -1, "null / zero-size buffers are errors");
    }

    void CheckLoggedOnce(const WxlListfileServices* lf)
    {
        char out[260];
        lf->lookupPath(777777, out, sizeof(out));
        const size_t after1 = LogCount();
        Check(LogHas("file id 777777 is not in the listfile"), "a missing id is logged");
        lf->lookupPath(777777, out, sizeof(out));
        lf->lookupPath(777777, out, sizeof(out));
        Check(LogCount() == after1, "  ...once, not on every repeated lookup");
    }

    // A file that is not found: the answer is NOTFOUND and `out` holds a name the client cannot open, so the client
    // falls back to its own missing texture (green) / missing model (cube).
    void CheckNotFound(const WxlListfileServices* lf, const char* base)
    {
        char out[260];
        const std::string tex = std::string(base) + ".blp", mdl = std::string(base) + ".m2";
        std::printf(" not found\n");
        Check(lf->resolve(1, WXL_LISTFILE_ANY, out, sizeof(out)) == WXL_LISTFILE_FOUND && std::strcmp(out, "interface\\cinematics\\logo_800.avi") == 0,
              "resolve: an id whose file is in the client gives its own path");

        Check(lf->resolve(4000001, WXL_LISTFILE_TEXTURE, out, sizeof(out)) == WXL_LISTFILE_NOTFOUND && tex == out,
              "resolve: an id not in the listfile, wanted as a texture -> NOTFOUND and the name '<base>.blp'");
        Check(ApiFileExists(out) == 0, "  ...which is not a file the client can open (so its own green texture takes over)");
        Check(LogHas(("file id 4000001 is not in the listfile (texture): answering '" + tex + "'").c_str()), "  ...and it is logged");

        Check(lf->resolve(4000002, WXL_LISTFILE_MODEL, out, sizeof(out)) == WXL_LISTFILE_NOTFOUND && mdl == out,
              "resolve: wanted as a model -> NOTFOUND and '<base>.m2'");
        Check(ApiFileExists(out) == 0, "  ...also not a file the client can open (its own cube takes over)");
        Check(LogHas(("file id 4000002 is not in the listfile (model): answering '" + mdl + "'").c_str()), "  ...and it is logged");

        Check(lf->resolve(4000003, WXL_LISTFILE_ANY, out, sizeof(out)) == WXL_LISTFILE_NOTFOUND && std::strcmp(out, base) == 0,
              "resolve: an unknown id of unknown type -> NOTFOUND and the bare name");
        Check(LogHas(("file id 4000003 is not in the listfile (any type): answering '" + std::string(base) + "'").c_str()), "  ...and it is logged");

        // by path, the kind from the extension
        Check(lf->resolvePath("Interface/Nothing/Here.blp", WXL_LISTFILE_ANY, out, sizeof(out)) == WXL_LISTFILE_NOTFOUND && tex == out,
              "resolvePath: a missing .blp -> '<base>.blp'");
        Check(lf->resolvePath("creature/nothing/here.m2", WXL_LISTFILE_ANY, out, sizeof(out)) == WXL_LISTFILE_NOTFOUND && mdl == out,
              "resolvePath: a missing .m2 -> '<base>.m2'");
        Check(lf->resolvePath("creature/nothing/here.mdx", WXL_LISTFILE_ANY, out, sizeof(out)) == WXL_LISTFILE_NOTFOUND && mdl == out,
              "resolvePath: a missing .mdx is a model too");
        Check(lf->resolvePath("sound/music/nothing.mp3", WXL_LISTFILE_ANY, out, sizeof(out)) == WXL_LISTFILE_NOTFOUND && std::strcmp(out, base) == 0,
              "resolvePath: a missing .mp3 -> the bare name");
        Check(LogHas("'sound\\music\\nothing.mp3' is not in the client (any type)"), "  ...and it is logged (any extension is logged)");
        Check(lf->resolvePath("interface/cinematics/logo_800.avi", WXL_LISTFILE_ANY, out, sizeof(out)) == WXL_LISTFILE_FOUND && std::strcmp(out, "interface\\cinematics\\logo_800.avi") == 0,
              "resolvePath: a path that is in the client comes back as itself, backslashes");
        Check(lf->resolvePath(nullptr, 0, out, sizeof(out)) == WXL_LISTFILE_ERROR && lf->resolvePath("", 0, out, sizeof(out)) == WXL_LISTFILE_ERROR, "resolvePath: null / empty path is an error");
        char tiny[6];
        Check(lf->resolve(4000001, WXL_LISTFILE_TEXTURE, tiny, sizeof(tiny)) == WXL_LISTFILE_ERROR, "resolve: a buffer too small for the not-found name is an error");

        // the id IS in the listfile but the client lacks the file (id 2 = wow_intro_800.avi)
        Check(lf->resolve(2, WXL_LISTFILE_ANY, out, sizeof(out)) == WXL_LISTFILE_NOTFOUND && std::strcmp(out, base) == 0, "resolve: in the listfile, not in the client -> NOTFOUND");
        Check(LogHas(("file id 2 -> 'interface\\cinematics\\wow_intro_800.avi' is not in the client (any type): answering '" + std::string(base) + "'").c_str()),
              "  ...logged with both the id and the path, in one line");
        Check(lf->resolve(53184, WXL_LISTFILE_TEXTURE, out, sizeof(out)) == WXL_LISTFILE_NOTFOUND && tex == out, "resolve: the caller's kind decides the extension even for a known id");
        // the not-found log is once per file
        const size_t n = LogCount();
        lf->resolve(4000001, WXL_LISTFILE_TEXTURE, out, sizeof(out));
        lf->resolve(2, WXL_LISTFILE_ANY, out, sizeof(out));
        lf->resolvePath("sound/music/nothing.mp3", WXL_LISTFILE_ANY, out, sizeof(out));
        Check(LogCount() == n, "a repeated not-found is not logged again");
    }

    // ---- scenarios -------------------------------------------------------------------------
    void RunThreads(const WxlListfileServices* lf)
    {
        std::printf(" threads\n");
        std::atomic<int> bad{ 0 };
        auto worker = [&](int seed) {
            char out[260];
            for (int i = 0; i < 400; ++i)
            {
                const uint32_t ids[] = { 1, 2, 21, 22, 53183, 53187 };
                const uint32_t id = ids[(i + seed) % 6];
                if (lf->lookupPath(id, out, sizeof(out)) != 1) ++bad;
                if (lf->lookupPath(900000 + static_cast<uint32_t>(i % 5), out, sizeof(out)) != 0) ++bad;
                if (lf->resolve(900000 + static_cast<uint32_t>(i % 5), WXL_LISTFILE_TEXTURE, out, sizeof(out)) != WXL_LISTFILE_NOTFOUND || std::strcmp(out, "WXL\\NotFound.blp") != 0) ++bad;
            }
        };
        std::vector<std::thread> t;
        for (int i = 0; i < 8; ++i) t.emplace_back(worker, i);
        for (auto& x : t) x.join();
        Check(bad == 0, "8 threads x 400 lookups/resolves: every answer correct, no crash");
    }

    int Eager(const char* dll, const std::string& snippet)
    {
        // default ini: the listfile is <client folder>\WXL\listfile.csv. Here the "client folder" is this program's.
        const std::string folder = ExeFolder() + "WXL";
        mkdir(folder.c_str(), 0755);
        const std::string file = folder + "/listfile.csv";
        WriteAll(file, snippet);
        StockClient();

        Loaded m = Load(dll);
        Init(m);
        std::printf("eager (defaults)\n");
        Check(m.rc == 0, "WxlModuleInit accepts the client");
        Check(m.lf != nullptr && WXL_LISTFILE_HAS(m.lf, reload), "publishes " WXL_SERVICE_LISTFILE " with every member");
        if (m.rc != 0 || !m.lf) { DumpLog(); return 1; }
        Check(m.lf->isEager() == 1, "eager by default");
        Check(m.lf->entryCount() == 9, "the table was built at load: 9 entries (the snippet has 9 lines, no header)");
        Check(LogHas("eager table built") && LogHas("header none"), "  ...and it is logged: no header row was found");
        Check(g_providers.empty(), "no file provider registered: the module serves no files of its own");
        CheckSnippetLookups(m.lf);
        CheckLoggedOnce(m.lf);
        CheckNotFound(m.lf, "WXL\\NotFound");
        RunThreads(m.lf);

        // reload picks up an edited file
        std::printf(" reload\n");
        char out[260];
        Check(m.lf->lookupPath(60000, out, sizeof(out)) == 0, "id 60000 is not in the table yet");
        WriteAll(file, snippet + "60000;world/test/new_thing.m2\r\n");
        Check(m.lf->lookupPath(60000, out, sizeof(out)) == 0, "  ...and an edit to the file is not seen until reload (the table is in memory)");
        Check(m.lf->reload() == 1 && m.lf->entryCount() == 10, "reload(): rebuilt, 10 entries");
        Check(m.lf->lookupPath(60000, out, sizeof(out)) == 1 && std::strcmp(out, "world\\test\\new_thing.m2") == 0, "  ...and the new id is found");

        m.shutdown();
        std::remove(file.c_str());
        rmdir(folder.c_str());
        return g_failed;
    }

    int Lazy(const char* dll, const std::string& snippet)
    {
        const std::string file = "/tmp/wxl_listfile_lazy.csv";
        WriteAll(file, snippet);
        g_ini["Listfile"]["Eager"] = "0";
        g_ini["Listfile"]["Path"] = file;
        StockClient();

        Loaded m = Load(dll);
        Init(m);
        std::printf("lazy ([Listfile] Eager=0)\n");
        Check(m.rc == 0 && m.lf, "module loads");
        if (m.rc != 0 || !m.lf) { DumpLog(); return 1; }
        Check(m.lf->isEager() == 0, "not eager");
        Check(m.lf->entryCount() == 0, "nothing was read at load: no table");
        Check(!LogHas("eager table built"), "  ...and nothing was built");
        CheckSnippetLookups(m.lf);
        CheckLoggedOnce(m.lf);
        CheckNotFound(m.lf, "WXL\\NotFound");
        RunThreads(m.lf);

        std::printf(" cache and reload\n");
        char out[260];
        // answers (hits and misses) are cached: with the file gone they still come back
        std::remove(file.c_str());
        Check(m.lf->lookupPath(53183, out, sizeof(out)) == 1, "a cached hit survives the file going away");
        Check(m.lf->lookupPath(3, out, sizeof(out)) == 0, "a cached miss too");
        Check(m.lf->lookupPath(53186, out, sizeof(out)) == 0 && LogHas("cannot open"), "an id never asked for, file gone: not found, and the log says it cannot open the file");
        WriteAll(file, snippet + "60000;world/test/new_thing.m2\r\n");
        Check(m.lf->lookupPath(53186, out, sizeof(out)) == 1, "the file is back: an uncached id is read from it (a failed open is not cached)");
        Check(m.lf->lookupPath(60000, out, sizeof(out)) == 1 && std::strcmp(out, "world\\test\\new_thing.m2") == 0, "a line added to the file is found at run time");
        Check(m.lf->reload() == 1, "reload() clears the cache");
        std::remove(file.c_str());
        Check(m.lf->lookupPath(53183, out, sizeof(out)) == 0, "  ...so with the file gone nothing is answered any more");
        m.shutdown();
        return g_failed;
    }

    int Formats(const char* dll)
    {
        const std::string file = "/tmp/wxl_listfile_formats.csv";
        // BOM, header, comma delimiter, a duplicate (first wins), unsorted ids, a junk line, blank lines, spaces after the
        // delimiter, a 32-bit id, and no newline at the end.
        std::string text = "\xEF\xBB\xBF" "fileDataID,filePath\r\n"
                           "300,world/c.m2\r\n"
                           "\r\n"
                           "100, world/a.m2\r\n"
                           "this line is junk\r\n"
                           "200,world/b with space.blp\n"
                           "100,world/DUPLICATE.m2\r\n"
                           "4294967295,world/last.m2\r\n"
                           "4294967296,world/too_big.m2\r\n"
                           "7,world/seven.m2";
        WriteAll(file, text);
        g_ini["Listfile"]["Path"] = file;
        Loaded m = Load(dll);
        Init(m);
        std::printf("formats\n");
        Check(m.rc == 0 && m.lf, "module loads");
        if (!m.lf) { DumpLog(); return 1; }
        Check(m.lf->entryCount() == 5, "5 distinct ids: 7, 100, 200, 300, 4294967295");
        Check(LogHas("header skipped") && LogHas("sorted on load") && LogHas("1 duplicate id(s), 2 bad line(s)"),
              "the log counts the header, the sort, 1 duplicate, 2 bad lines (junk and the id over 32 bits)");
        char out[260];
        Check(m.lf->lookupPath(100, out, sizeof(out)) == 1 && std::strcmp(out, "world\\a.m2") == 0, "a duplicate id: the first entry wins; spaces after the comma are dropped");
        Check(m.lf->lookupPath(200, out, sizeof(out)) == 1 && std::strcmp(out, "world\\b with space.blp") == 0, "LF-only line ending, spaces inside the path kept");
        Check(m.lf->lookupPath(300, out, sizeof(out)) == 1 && std::strcmp(out, "world\\c.m2") == 0, "an id that came before a smaller one in the file");
        Check(m.lf->lookupPath(7, out, sizeof(out)) == 1 && std::strcmp(out, "world\\seven.m2") == 0, "the last line has no newline and is still read");
        Check(m.lf->lookupPath(4294967295u, out, sizeof(out)) == 1, "the largest 32-bit id");
        Check(m.lf->lookupPath(4294967296ull & 0xFFFFFFFFu, out, sizeof(out)) == 0, "an id over 32 bits was rejected, not wrapped to 0");
        Check(m.lf->lookupPath(1, out, sizeof(out)) == 0, "the header text is not an entry");
        m.shutdown();
        std::remove(file.c_str());
        return g_failed;
    }

    int NoExists(const char* dll, const std::string& snippet)
    {
        const std::string file = "/tmp/wxl_listfile_noexists.csv";
        WriteAll(file, snippet + "70000;world/not/in/client.blp\r\n");
        g_ini["Listfile"]["Path"] = file;
        g_ini["Listfile"]["CheckExists"] = "0";
        Loaded m = Load(dll);
        Init(m);
        std::printf("existence check off ([Listfile] CheckExists=0)\n");
        Check(m.rc == 0 && m.lf, "module loads");
        if (!m.lf) { DumpLog(); return 1; }
        char out[260];
        Check(m.lf->resolve(70000, WXL_LISTFILE_TEXTURE, out, sizeof(out)) == WXL_LISTFILE_FOUND && std::strcmp(out, "world\\not\\in\\client.blp") == 0,
              "a path the listfile names is returned as is, whether or not the client has it");
        Check(m.lf->resolve(70001, WXL_LISTFILE_TEXTURE, out, sizeof(out)) == WXL_LISTFILE_NOTFOUND && std::strcmp(out, "WXL\\NotFound.blp") == 0, "an id that is not in the listfile still gets the not-found name");
        m.shutdown();
        std::remove(file.c_str());
        return g_failed;
    }

    int CustomName(const char* dll, const std::string& snippet)
    {
        const std::string file = "/tmp/wxl_listfile_customname.csv";
        WriteAll(file, snippet);
        g_ini["Listfile"]["Path"] = file;
        g_ini["Listfile"]["NotFoundName"] = "Nope/Missing";
        StockClient();
        Loaded m = Load(dll);
        Init(m);
        std::printf("custom not-found name ([Listfile] NotFoundName=Nope/Missing)\n");
        Check(m.rc == 0 && m.lf, "module loads");
        if (!m.lf) { DumpLog(); return 1; }
        CheckNotFound(m.lf, "Nope\\Missing");   // forward slashes in the ini are turned into backslashes
        m.shutdown();
        std::remove(file.c_str());
        return g_failed;
    }

    int MissingFile(const char* dll)
    {
        g_ini["Listfile"]["Path"] = "/tmp/wxl_listfile_does_not_exist.csv";
        StockClient();
        Loaded m = Load(dll);
        Init(m);
        std::printf("no listfile on disk\n");
        Check(m.rc == 0 && m.lf, "the module still loads");
        if (!m.lf) { DumpLog(); return 1; }
        Check(LogHas("cannot open '/tmp/wxl_listfile_does_not_exist.csv'"), "the log names the file it could not open");
        char out[260];
        Check(m.lf->entryCount() == 0 && m.lf->lookupPath(1, out, sizeof(out)) == 0, "every lookup is 'not found'");
        Check(m.lf->resolve(1, WXL_LISTFILE_TEXTURE, out, sizeof(out)) == WXL_LISTFILE_NOTFOUND && std::strcmp(out, "WXL\\NotFound.blp") == 0, "  ...and a texture still gets the not-found name");
        m.shutdown();
        return g_failed;
    }

    int Refusals(const char* dll)
    {
        std::printf("refusals\n");
        Loaded m = Load(dll);
        WxlModuleInfo info = {}; info.structSize = sizeof(info);
        WxlApi oldCore = MakeApi();
        oldCore.structSize = static_cast<uint32_t>(offsetof(WxlApi, fileExists));   // a core from before fileExists
        Check(m.init(&oldCore, &info) != 0, "a core without fileExists: refused");
        Check(g_services.empty(), "  ...and nothing was registered");
        WxlApi otherAbi = MakeApi();
        otherAbi.abiVersion = WXL_ABI_VERSION + 1;
        Check(m.init(&otherAbi, &info) != 0, "another ABI version: refused");
        Check(g_services.empty(), "  ...and nothing was registered");
        return g_failed;
    }

    int Scale(const char* dll)
    {
        const std::string file = "/tmp/wxl_listfile_scale.csv";
        const uint32_t N = 3000000;
        std::vector<uint32_t> ids(N);
        for (uint32_t i = 0; i < N; ++i) ids[i] = 1 + i * 3;          // gaps, like a real listfile
        std::mt19937 rng(42);
        std::shuffle(ids.begin(), ids.end(), rng);                    // unsorted: the worst case for the build
        {
            std::ofstream f(file, std::ios::binary);
            for (uint32_t id : ids) f << id << ";world/generic/some/long/directory/name/model_" << id << ".m2\r\n";
        }
        struct stat st = {};
        stat(file.c_str(), &st);
        g_ini["Listfile"]["Path"] = file;
        StockClient();
        Loaded m = Load(dll);
        const long rssBefore = PeakRssMb();
        const auto t0 = std::chrono::steady_clock::now();
        Init(m);
        const double buildSec = Seconds(t0);
        std::printf("scale: %u lines, %.0f MB, shuffled\n", N, st.st_size / 1048576.0);
        std::printf("  build %.2f s, peak memory %ld MB, memory held after the build %ld MB (was %ld MB; the test itself holds ~30 MB of ids)\n",
                    buildSec, PeakRssMb(), CurrentRssMb(), rssBefore);
        Check(m.rc == 0 && m.lf && m.lf->entryCount() == N, "3,000,000 entries");
        if (!m.lf) { DumpLog(); return 1; }
        char out[260];
        bool allOk = true;
        const auto t1 = std::chrono::steady_clock::now();
        for (int i = 0; i < 200000; ++i)
        {
            const uint32_t id = ids[static_cast<size_t>(rng()) % N];
            char want[200];
            std::snprintf(want, sizeof(want), "world\\generic\\some\\long\\directory\\name\\model_%u.m2", id);
            if (m.lf->lookupPath(id, out, sizeof(out)) != 1 || std::strcmp(out, want) != 0) { allOk = false; break; }
            if (m.lf->lookupPath(id + 1, out, sizeof(out)) != 0) { allOk = false; break; }   // ids have gaps
        }
        const double lookupSec = Seconds(t1);
        Check(allOk, "200,000 random lookups (hit + miss each): every answer right");
        std::printf("  %.0f ns per lookup\n", lookupSec / 400000 * 1e9);
        m.shutdown();
        std::remove(file.c_str());
        return g_failed;
    }

    // the runtime-read mode on the same big file: one full scan, then cached
    int ScaleLazy(const char* dll)
    {
        const std::string file = "/tmp/wxl_listfile_scale2.csv";
        const uint32_t N = 3000000;
        {
            std::ofstream f(file, std::ios::binary);
            for (uint32_t i = 0; i < N; ++i) f << (1 + i * 3) << ";world/generic/some/long/directory/name/model_" << (1 + i * 3) << ".m2\r\n";
        }
        g_ini["Listfile"]["Path"] = file;
        g_ini["Listfile"]["Eager"] = "0";
        Loaded m = Load(dll);
        const auto t0 = std::chrono::steady_clock::now();
        Init(m);
        std::printf("scale, runtime read: %u lines\n", N);
        std::printf("  load took %.4f s (it reads nothing)\n", Seconds(t0));
        char out[260];
        const auto t1 = std::chrono::steady_clock::now();
        const int r = m.lf->lookupPath(1 + (N - 1) * 3, out, sizeof(out));     // the very last line: a full scan
        const double scan = Seconds(t1);
        std::printf("  first lookup of the LAST id (a full scan): %.3f s\n", scan);
        Check(r == 1, "found at the end of the file");
        const auto t2 = std::chrono::steady_clock::now();
        for (int i = 0; i < 100000; ++i) m.lf->lookupPath(1 + (N - 1) * 3, out, sizeof(out));
        std::printf("  the same lookup again: %.0f ns (cached)\n", Seconds(t2) / 100000 * 1e9);
        m.shutdown();
        std::remove(file.c_str());
        return g_failed;
    }
}

int main(int argc, char** argv)
{
    if (argc < 4) { std::printf("usage: %s <libwxl_listfile.so> <snippet.csv> <scenario>\n", argv[0]); return 2; }
    char dll[PATH_MAX];
    if (!realpath(argv[1], dll)) { std::perror(argv[1]); return 2; }
    const std::string snippet = ReadAll(argv[2]);
    const std::string scenario = argv[3];
    if (snippet.empty()) { std::printf("cannot read %s\n", argv[2]); return 2; }

    int failed;
    if      (scenario == "eager")       failed = Eager(dll, snippet);
    else if (scenario == "lazy")        failed = Lazy(dll, snippet);
    else if (scenario == "formats")     failed = Formats(dll);
    else if (scenario == "noexists")    failed = NoExists(dll, snippet);
    else if (scenario == "customname")  failed = CustomName(dll, snippet);
    else if (scenario == "missingfile") failed = MissingFile(dll);
    else if (scenario == "refusals")    failed = Refusals(dll);
    else if (scenario == "scale")       failed = Scale(dll);
    else if (scenario == "scalelazy")   failed = ScaleLazy(dll);
    else { std::printf("unknown scenario '%s'\n", scenario.c_str()); return 2; }

    if (failed) DumpLog();
    std::printf("%s (%d failed)\n", failed ? "FAILED" : "passed", failed);
    return failed ? 1 : 0;
}
