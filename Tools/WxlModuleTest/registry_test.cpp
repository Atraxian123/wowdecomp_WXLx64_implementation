// Test of the core's side of the module API that is not engine-bound: WXL/ModuleRegistry.cpp (file providers fanned
// out through the REAL storage layer, virtual-key queries). Log.h is stubbed below; nothing else is.
//
//   R=<repo root>
//   g++ -std=c++17 -O1 -pthread -I $R registry_test.cpp $R/Engine/Source/WXL/ModuleRegistry.cpp $R/Engine/Source/WXL/Storage.cpp -o registry_test
//   ./registry_test            (exit code 0 = every check passed)
#include "Engine/Source/WXL/ModuleRegistry.h"
#include "Engine/Source/WXL/Storage.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---- the log, stubbed: what Storage.cpp and ModuleRegistry.cpp need from Log.h ----
namespace wxl
{
    static std::mutex g_logLock;
    static std::vector<std::string> g_log;
    void LogCat(const char* category, const char* format, ...)
    {
        char line[512];
        va_list ap; va_start(ap, format); std::vsnprintf(line, sizeof(line), format, ap); va_end(ap);
        std::lock_guard<std::mutex> l(g_logLock);
        g_log.push_back(std::string(category) + ": " + line);
    }
    bool LogThrottle(const char*, unsigned, unsigned) { return false; }
}

namespace
{
    int g_failed = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) ++g_failed;
    }

    // providers for the test: each claims one exact name
    struct Spec { const char* name; const char* bytes; bool reserveThenDecline; bool claimWithoutBytes; };
    int Provide(void* user, const char* name, const WxlServeSink* sink)
    {
        const Spec* s = static_cast<const Spec*>(user);
        if (std::strcmp(name, s->name) != 0) return 0;
        if (s->claimWithoutBytes) return 1;                       // says yes, serves nothing
        void* dst = sink->reserve(sink->ctx, std::strlen(s->bytes));
        if (!dst) return 0;
        std::memcpy(dst, s->bytes, std::strlen(s->bytes));
        return s->reserveThenDecline ? 0 : 1;                     // fills the buffer, then says no
    }
    int IsKey(void* user, const char* name) { return std::strcmp(name, static_cast<const char*>(user)) == 0; }
    int DeclineAll(void*, const char*, const WxlServeSink*) { return 0; }
    std::atomic<int> g_decliner{ 0 };
    int CountingDecline(void*, const char*, const WxlServeSink*) { ++g_decliner; return 0; }

    bool Serve(const char* name, std::string& out)
    {
        std::vector<uint8_t> v;
        if (!wxl::storage::TryServe(name, v)) return false;
        out.assign(v.begin(), v.end());
        return true;
    }
}

int main()
{
    using namespace wxl::registry;

    std::printf("file providers\n");
    Check(!wxl::storage::HasProviders(), "nothing is registered at the start");
    Check(RegisterFileProvider(nullptr, nullptr) == 1, "a null provider is refused");
    Check(!wxl::storage::HasProviders(), "  ...and installs nothing");

    static Spec a = { "dir\\a.m2", "AAAA", false, false };
    static Spec b = { "dir\\b.m2", "BBBBBB", false, false };
    static Spec half = { "dir\\half.m2", "partial", true, false };      // fills the buffer, then declines
    static Spec liar = { "dir\\liar.m2", "", false, true };            // says yes without a byte
    Check(RegisterFileProvider(&CountingDecline, nullptr) == 0, "the first provider is accepted");
    Check(wxl::storage::HasProviders(), "  ...and the storage layer now has a provider");
    Check(RegisterFileProvider(&Provide, &a) == 0 && RegisterFileProvider(&Provide, &b) == 0 &&
          RegisterFileProvider(&Provide, &half) == 0 && RegisterFileProvider(&Provide, &liar) == 0, "four more are accepted");
    {
        int installs = 0;
        for (const std::string& l : wxl::g_log) if (l.find("STORAGE: provider registered") != std::string::npos) ++installs;
        Check(installs == 1, "the storage layer was given ONE provider, however many modules register");
    }

    std::string out;
    Check(Serve("dir\\a.m2", out) && out == "AAAA", "a file the 2nd provider owns is served");
    Check(Serve("dir\\b.m2", out) && out == "BBBBBB", "a file the 3rd owns is served, whatever the size of the first");
    const int before = g_decliner;
    Check(Serve("dir\\a.m2", out), "(again)");
    Check(g_decliner > before, "providers are asked in registration order (the declining first one saw the open)");
    Check(!Serve("dir\\nobody.m2", out), "a name nobody owns is not served");
    {
        std::vector<uint8_t> v = { 1, 2, 3 };
        wxl::storage::TryServe("dir\\half.m2", v);
        Check(v.empty(), "a provider that filled the buffer and then said no: the half-filled buffer is cleared, no hit");
        Check(!wxl::storage::TryServe("dir\\half.m2", v), "  ...and it is a miss");
        v = { 9 };
        Check(!wxl::storage::TryServe("dir\\liar.m2", v) && v.empty(), "a provider that says yes with no bytes is a miss, not an empty file");
    }
    Check(!Serve("", out) && !Serve(nullptr, out), "empty and null names are misses");

    std::printf("capacity\n");
    int accepted = 0, refused = 0;
    for (int i = 0; i < 20; ++i) (RegisterFileProvider(&DeclineAll, nullptr) == 0 ? accepted : refused)++;
    Check(accepted == 16 - 5 && refused == 20 - (16 - 5), "the table takes 16 providers in all; the rest are refused (return 2)");
    Check(Serve("dir\\a.m2", out) && out == "AAAA", "  ...and the ones already registered keep working");

    std::printf("virtual keys\n");
    Check(IsVirtualKey("x") == 0, "no query registered: not a virtual key");
    Check(RegisterVirtualKeyQuery(nullptr, nullptr) == 1, "a null query is refused");
    static const char* k1 = "virt\\one.m2"; static const char* k2 = "virt\\two.m2";
    Check(RegisterVirtualKeyQuery(&IsKey, const_cast<char*>(k1)) == 0 && RegisterVirtualKeyQuery(&IsKey, const_cast<char*>(k2)) == 0, "two queries registered");
    Check(IsVirtualKey("virt\\one.m2") == 1 && IsVirtualKey("virt\\two.m2") == 1, "either one claims its name");
    Check(IsVirtualKey("virt\\three.m2") == 0 && IsVirtualKey("") == 0 && IsVirtualKey(nullptr) == 0, "others, empty and null do not");

    std::printf("existence probe (storage)\n");
    Check(!wxl::storage::ExistsNative("x"), "no probe installed: false");
    wxl::storage::SetNativeExists([](const char* n) { return std::strcmp(n, "there") == 0; });
    Check(wxl::storage::ExistsNative("there") && !wxl::storage::ExistsNative("gone") && !wxl::storage::ExistsNative("") && !wxl::storage::ExistsNative(nullptr),
          "the installed probe answers; empty and null are false");

    std::printf("threads: opens while providers register\n");
    {
        std::atomic<int> bad{ 0 };
        std::atomic<bool> stop{ false };
        std::vector<std::thread> readers;
        for (int t = 0; t < 6; ++t)
            readers.emplace_back([&] {
                std::string s;
                while (!stop)
                {
                    if (!Serve("dir\\b.m2", s) || s != "BBBBBB") ++bad;
                    if (Serve("dir\\other.m2", s)) ++bad;
                    if (IsVirtualKey("virt\\one.m2") != 1) ++bad;
                }
            });
        for (int i = 0; i < 2000; ++i) { RegisterFileProvider(&DeclineAll, nullptr); RegisterVirtualKeyQuery(&IsKey, const_cast<char*>(k1)); }
        stop = true;
        for (auto& r : readers) r.join();
        Check(bad == 0, "readers never see a wrong answer while registrations keep arriving (lock-free reads)");
    }

    std::printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "all passed", g_failed);
    return g_failed ? 1 : 0;
}
