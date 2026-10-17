#include "Engine/Source/WXL/Log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace wxl
{
    void Log(const char* format, ...)
    {
        static std::mutex s_mutex;
        std::lock_guard<std::mutex> lock(s_mutex);

        FILE* f = std::fopen("WXL.log", "ab");
        if (!f) return;

        va_list args;
        va_start(args, format);
        std::vfprintf(f, format, args);
        va_end(args);
        std::fputc('\n', f);
        std::fclose(f);
    }
}
