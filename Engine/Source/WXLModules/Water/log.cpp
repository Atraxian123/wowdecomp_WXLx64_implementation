#include "log.h"

#include <windows.h>

#include <cstdio>

namespace
{
constexpr long kMaxLogBytes = 4 * 1024 * 1024;

FILE* g_file = nullptr;
int g_level = static_cast<int>(LogLevel::Info);
CRITICAL_SECTION g_lock;
bool g_lockReady = false;
}

void LogOpen(const char* path)
{
    if (!g_lockReady)
    {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }
    if (g_file)
        return;
    g_file = std::fopen(path, "w");
}

void LogSetLevel(int level)
{
    g_level = level;
}

bool LogEnabled(LogLevel level)
{
    return g_file && static_cast<int>(level) <= g_level;
}

void LogWrite(LogLevel level, const char* fmt, ...)
{
    if (!LogEnabled(level))
        return;
    EnterCriticalSection(&g_lock);
    if (std::ftell(g_file) < kMaxLogBytes)
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        std::fprintf(g_file, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list args;
        va_start(args, fmt);
        std::vfprintf(g_file, fmt, args);
        va_end(args);
        std::fputc('\n', g_file);
        std::fflush(g_file);
    }
    LeaveCriticalSection(&g_lock);
}
