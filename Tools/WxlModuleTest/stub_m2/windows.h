// Just enough of <windows.h> to compile the Model2 headers on Linux for the modern-M2 host test. Never used by the real build.
#pragma once
#include <cstdint>
#include <cstddef>
typedef void* HANDLE; typedef unsigned long DWORD; typedef int BOOL; typedef void* HWND; typedef void* HINSTANCE;
typedef void* HMODULE; typedef long LONG; typedef unsigned int UINT; typedef const char* LPCSTR; typedef char* LPSTR;
typedef void* LPVOID; typedef unsigned short WORD; typedef unsigned char BYTE; typedef long long LONGLONG;
typedef uintptr_t UINT_PTR; typedef intptr_t LONG_PTR; typedef uintptr_t ULONG_PTR; typedef void* HDC; typedef void* HGLRC;
typedef long HRESULT; typedef unsigned long ULONG; typedef void* HKEY; typedef void* HICON; typedef void* HMONITOR;
struct CRITICAL_SECTION { void* p[6]; };
struct RECT { long left, top, right, bottom; }; struct POINT { long x, y; };
union LARGE_INTEGER { struct { DWORD LowPart; LONG HighPart; }; LONGLONG QuadPart; };
#define WINAPI
#define CALLBACK
#define APIENTRY
#define INFINITE 0xFFFFFFFF
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
