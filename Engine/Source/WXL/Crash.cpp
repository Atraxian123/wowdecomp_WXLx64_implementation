#include "Engine/Source/WXL/Crash.h"

#ifdef _WIN32

#include "Engine/Source/WXL/Log.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace wxl
{
    namespace
    {
        std::mutex g_symMutex;
        bool       g_symReady = false;

        const char* BaseName(const char* path)
        {
            const char* slash = std::strrchr(path, '\\');
            return slash ? slash + 1 : path;
        }

        /// "module+0xRVA" always, plus "function+0xOFF (file:line)" when DbgHelp finds the PDB.
        void Describe(void* address, char* out, size_t outSize)
        {
            char moduleName[MAX_PATH] = "?";
            HMODULE module = nullptr;
            uintptr_t rva = 0;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCSTR>(address), &module) && module)
            {
                GetModuleFileNameA(module, moduleName, sizeof(moduleName));
                rva = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module);
            }

            int n = std::snprintf(out, outSize, "%s+0x%llX", BaseName(moduleName), static_cast<unsigned long long>(rva));

            std::lock_guard<std::mutex> lock(g_symMutex);
            if (!g_symReady)
            {
                SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
                g_symReady = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
            }
            if (!g_symReady || n <= 0 || static_cast<size_t>(n) >= outSize) return;

            alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
            SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen   = 255;
            DWORD64 displacement = 0;
            if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(address), &displacement, sym))
            {
                n += std::snprintf(out + n, outSize - n, "  %s+0x%llX", sym->Name, static_cast<unsigned long long>(displacement));
                IMAGEHLP_LINE64 line = {};
                line.SizeOfStruct = sizeof(line);
                DWORD lineDisp = 0;
                if (n > 0 && static_cast<size_t>(n) < outSize &&
                    SymGetLineFromAddr64(GetCurrentProcess(), reinterpret_cast<DWORD64>(address), &lineDisp, &line))
                    std::snprintf(out + n, outSize - n, "  (%s:%lu)", BaseName(line.FileName), line.LineNumber);
            }
        }

        const char* CodeName(DWORD code)
        {
            switch (code)
            {
                case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
                case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
                case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
                case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
                case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
                case 0xC0000374:                      return "HEAP_CORRUPTION";
                case 0xC0000409:                      return "STACK_BUFFER_OVERRUN/FAST_FAIL";
                case 0xC0000096:                      return "PRIVILEGED_INSTRUCTION";
                default:                              return "EXCEPTION";
            }
        }

        bool IsInteresting(DWORD code)
        {
            switch (code)
            {
                case EXCEPTION_ACCESS_VIOLATION:
                case EXCEPTION_STACK_OVERFLOW:
                case EXCEPTION_ILLEGAL_INSTRUCTION:
                case EXCEPTION_INT_DIVIDE_BY_ZERO:
                case EXCEPTION_IN_PAGE_ERROR:
                case 0xC0000374:
                case 0xC0000409:
                case 0xC0000096:
                    return true;
                default:
                    return false;
            }
        }

        LONG CALLBACK Handler(PEXCEPTION_POINTERS info)
        {
            const EXCEPTION_RECORD* rec = info->ExceptionRecord;
            if (!IsInteresting(rec->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;

            // Stack overflow leaves almost no stack: log the bare minimum, then let it propagate.
            const bool stackOverflow = rec->ExceptionCode == EXCEPTION_STACK_OVERFLOW;

            static std::mutex s_mutex;
            static std::unordered_map<void*, unsigned> s_seen;
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                unsigned& n = s_seen[rec->ExceptionAddress];
                if (++n > 4) return EXCEPTION_CONTINUE_SEARCH; // same spot again and again: already on record
            }

            if (stackOverflow)
            {
                wxl::LogCat("CRASH", "STACK_OVERFLOW at %p", rec->ExceptionAddress);
                return EXCEPTION_CONTINUE_SEARCH;
            }

            char where[512];
            Describe(rec->ExceptionAddress, where, sizeof(where));
            wxl::LogCat("CRASH", "%s (0x%08lX) at %s  [first-chance: a __try guard may handle this]",
                        CodeName(rec->ExceptionCode), rec->ExceptionCode, where);

            if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
            {
                const ULONG_PTR kind = rec->ExceptionInformation[0];
                wxl::LogCat("CRASH", "  %s address 0x%llX",
                            kind == 0 ? "read of" : kind == 1 ? "write to" : kind == 8 ? "execute of" : "access to",
                            static_cast<unsigned long long>(rec->ExceptionInformation[1]));
            }

            void* frames[32] = {};
            const USHORT count = CaptureStackBackTrace(1, 32, frames, nullptr);
            for (USHORT i = 0; i < count; ++i)
            {
                char line[512];
                Describe(frames[i], line, sizeof(line));
                wxl::LogCat("CRASH", "  #%u %s", static_cast<unsigned>(i), line);
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    void InstallCrashLogger()
    {
        static bool s_done = false;
        if (s_done) return;
        s_done = true;
        AddVectoredExceptionHandler(1, &Handler);
        wxl::LogCat("CRASH", "crash logger installed");
    }
}

#else

namespace wxl
{
    void InstallCrashLogger() {}
}

#endif
