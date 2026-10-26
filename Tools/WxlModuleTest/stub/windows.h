// Just enough of <windows.h> to build the Windows-only parts of a module (a directory scan) on Linux for tests.
// Never used by the real build.
#pragma once
#include <cstdint>
typedef void* HANDLE; typedef unsigned long DWORD; typedef int BOOL; typedef const char* LPCSTR;
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define MAX_PATH 260
struct WIN32_FIND_DATAA { DWORD dwFileAttributes; char cFileName[260]; };
inline HANDLE FindFirstFileA(const char*, WIN32_FIND_DATAA*) { return INVALID_HANDLE_VALUE; }
inline BOOL FindNextFileA(HANDLE, WIN32_FIND_DATAA*) { return 0; }
inline BOOL FindClose(HANDLE) { return 1; }
#define FILE_ATTRIBUTE_DIRECTORY 0x10
