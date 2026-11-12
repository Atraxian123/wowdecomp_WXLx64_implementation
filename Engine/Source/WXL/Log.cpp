#include "Engine/Source/WXL/Log.h"
#include "Engine/Source/WXL/Ini.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#else
#include <functional>
#include <thread>
#include <unistd.h>
#endif

namespace wxl
{
    namespace
    {
        constexpr long long kMaxLogBytes = 256ll * 1024 * 1024; // a runaway log stops here instead of filling the disk

        std::mutex& LogMutex()
        {
            static std::mutex m;
            return m;
        }

        unsigned ThreadId()
        {
#ifdef _WIN32
            return static_cast<unsigned>(GetCurrentThreadId());
#else
            return static_cast<unsigned>(std::hash<std::thread::id>()(std::this_thread::get_id()) & 0xFFFFu);
#endif
        }

        struct Timestamp
        {
            int h, m, s, ms;
        };

        Timestamp Now()
        {
            Timestamp t = {};
#ifdef _WIN32
            SYSTEMTIME st;
            GetLocalTime(&st);
            t.h = st.wHour; t.m = st.wMinute; t.s = st.wSecond; t.ms = st.wMilliseconds;
#else
            std::time_t now = std::time(nullptr);
            std::tm* lt = std::localtime(&now);
            if (lt) { t.h = lt->tm_hour; t.m = lt->tm_min; t.s = lt->tm_sec; }
#endif
            return t;
        }

        // [Log] <key>: a missing file, section or key gives `fallback`. Read through wxl::ini, which never logs, so
        // this cannot recurse.
        bool IniLogFlag(const char* key, bool fallback)
        {
            return wxl::ini::GetBool("Log", key, fallback);
        }

        // [Log] Enabled: off unless set.
        bool IniEnabled()
        {
            return IniLogFlag("Enabled", false);
        }

        // The modules that have their own [Log] switch, and the categories each one writes under.
        enum class Area { None, ModernM2, Equip, Physics, Creature, Weapon };

        Area AreaOf(const char* category)
        {
            if (!category) return Area::None;
            if (!std::strcmp(category, "M2")) return Area::ModernM2;
            if (!std::strcmp(category, "EQUIP") || !std::strcmp(category, "SIDECAR") || !std::strcmp(category, "VPATH"))
                return Area::Equip;
            if (!std::strcmp(category, "PHYS")) return Area::Physics;
            if (!std::strcmp(category, "CREATURE")) return Area::Creature;
            if (!std::strcmp(category, "WEAPON")) return Area::Weapon;
            return Area::None;
        }

        // The per-module switches, every one on unless the ini says 0. Re-read when the ini changes (a save, or a
        // console override), so a module's log can be silenced or enabled without a restart.
        bool AreaEnabled(Area area)
        {
            static std::atomic<uint32_t> seen{0xFFFFFFFFu};
            static std::atomic<uint32_t> bits{0x1Fu};
            const uint32_t gen = wxl::ini::Generation();
            if (seen.load(std::memory_order_acquire) != gen)
            {
                static std::mutex m;
                std::lock_guard<std::mutex> lock(m);
                if (seen.load(std::memory_order_relaxed) != gen)
                {
                    uint32_t b = 0;
                    if (IniLogFlag("ModernM2", true)) b |= 1u << 0;
                    if (IniLogFlag("Equip", true))    b |= 1u << 1;
                    if (IniLogFlag("Physics", true))  b |= 1u << 2;
                    if (IniLogFlag("Creature", true)) b |= 1u << 3;
                    if (IniLogFlag("Weapon", true))   b |= 1u << 4;
                    bits.store(b, std::memory_order_relaxed);
                    seen.store(gen, std::memory_order_release);
                }
            }
            const uint32_t b = bits.load(std::memory_order_relaxed);
            switch (area)
            {
                case Area::ModernM2: return (b & (1u << 0)) != 0;
                case Area::Equip:    return (b & (1u << 1)) != 0;
                case Area::Physics:  return (b & (1u << 2)) != 0;
                case Area::Creature: return (b & (1u << 3)) != 0;
                case Area::Weapon:   return (b & (1u << 4)) != 0;
                default:             return true;
            }
        }

        bool DecideEnabled()
        {
            // the environment wins for a single run: WXL_LOG=1 forces on, WXL_LOG=0 (or n/f) forces off
            const char* env = std::getenv("WXL_LOG");
            if (env && *env)
            {
                if (*env == '0' || *env == 'n' || *env == 'N' || *env == 'f' || *env == 'F') return false;
                return true;
            }
            return IniEnabled();
        }

        struct Sink
        {
            FILE*     file    = nullptr;
            long long written = 0;
            bool      capped  = false;
        };

        Sink& GetSink()
        {
            static Sink s;
            return s;
        }

        void WriteLine(const char* category, const char* format, va_list args)
        {
            std::lock_guard<std::mutex> lock(LogMutex());
            Sink& sink = GetSink();
            if (sink.capped) return;

            if (!sink.file)
            {
                sink.file = std::fopen("WXL.log", "wb"); // truncate: one session per file
                if (!sink.file) { sink.capped = true; return; }
                std::fprintf(sink.file, "==== WXL log opened ([Log] Enabled=1 in WXLSetup.ini, or env WXL_LOG=1; ModernM2, Equip, Physics, Creature and Weapon = 0 there silence one module) ====\n");
            }

            const Timestamp t = Now();
            char body[2048];
            std::vsnprintf(body, sizeof(body), format, args);

            const int n = std::fprintf(sink.file, "[%02d:%02d:%02d.%03d] [T%u] [%s] %s\n",
                                       t.h, t.m, t.s, t.ms, ThreadId(), category, body);
            std::fflush(sink.file); // durable across a crash: the last line written is the last thing that happened
            if (n > 0) sink.written += n;

            if (sink.written > kMaxLogBytes)
            {
                std::fprintf(sink.file, "==== WXL log reached %lld MB, further lines dropped ====\n", kMaxLogBytes >> 20);
                std::fflush(sink.file);
                sink.capped = true;
            }
        }
    }

    bool LogEnabled()
    {
        // [Log] Enabled is looked at again whenever the ini changes (a save, or a console override); WXL_LOG still wins.
        static std::atomic<uint32_t> seen{0xFFFFFFFFu};
        static std::atomic<bool> enabled{false};
        const uint32_t gen = wxl::ini::Generation();
        if (seen.load(std::memory_order_acquire) != gen)
        {
            static std::mutex m;
            std::lock_guard<std::mutex> lock(m);
            if (seen.load(std::memory_order_relaxed) != gen)
            {
                enabled.store(DecideEnabled(), std::memory_order_relaxed);
                seen.store(gen, std::memory_order_release);
            }
        }
        return enabled.load(std::memory_order_relaxed);
    }

    bool LogCategoryEnabled(const char* category)
    {
        return LogEnabled() && AreaEnabled(AreaOf(category));
    }

    void LogCatV(const char* category, const char* format, va_list args)
    {
        if (!LogCategoryEnabled(category)) return;
        WriteLine(category ? category : "WXL", format, args);
    }

    void LogCat(const char* category, const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        LogCatV(category, format, args);
        va_end(args);
    }

    void Log(const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        LogCatV("WXL", format, args);
        va_end(args);
    }

    bool LogThrottle(const char* key, unsigned first, unsigned every)
    {
        static std::mutex s_mutex;
        static std::unordered_map<std::string, unsigned> s_counts;
        std::lock_guard<std::mutex> lock(s_mutex);
        const unsigned n = ++s_counts[key];
        if (n <= first) return true;
        return every != 0 && (n % every) == 0;
    }

    const char* WorkingDirectory()
    {
        static const std::string dir = []() {
            char buf[1024] = {};
#ifdef _WIN32
            if (!GetCurrentDirectoryA(sizeof(buf), buf)) return std::string("(unknown)");
#else
            if (!getcwd(buf, sizeof(buf))) return std::string("(unknown)");
#endif
            return std::string(buf);
        }();
        return dir.c_str();
    }
}
