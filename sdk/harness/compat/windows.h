#pragma once
// harness only: the Win32 names portable runtime and game files touch
#include <cstdint>
#include <cwchar>
#include <strings.h>

typedef long HRESULT;
typedef void* HANDLE;
typedef void* HMODULE;
typedef void* HWND;
typedef void* HCURSOR;
typedef const wchar_t* PCWSTR;
typedef unsigned int UINT;
typedef int BOOL;
#define WINAPI
#ifndef TRUE
#define TRUE 1
#endif

inline HMODULE GetModuleHandleW(const wchar_t*) { return nullptr; }
inline void* GetProcAddress(HMODULE, const char*) { return nullptr; }
inline HANDLE GetCurrentThread() { return nullptr; }

// window messages and keys a game's GameInfo::message names; no window sends them here
enum : uint32_t {
    WM_ACTIVATE = 0x0006, WM_SETCURSOR = 0x0020, WM_INPUT = 0x00FF, WM_KEYDOWN = 0x0100, WM_SYSKEYDOWN = 0x0104,
    WM_LBUTTONUP = 0x0202,
};
enum { WA_INACTIVE = 0, HTCLIENT = 1 };
enum {
    VK_BACK = 0x08, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_ESCAPE = 0x1B, VK_SPACE = 0x20,
    VK_LSHIFT = 0xA0, VK_RSHIFT = 0xA1,
    VK_PRIOR = 0x21, VK_NEXT = 0x22, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28,
    VK_F3 = 0x72, VK_F4 = 0x73, VK_F5 = 0x74, VK_F6 = 0x75, VK_F7 = 0x76, VK_F8 = 0x77,
    VK_F9 = 0x78, VK_F10 = 0x79, VK_OEM_3 = 0xC0,
};
inline int _wcsicmp(const wchar_t* a, const wchar_t* b) { return ::wcscasecmp(a, b); }
inline int _wtoi(const wchar_t* value) { return int(std::wcstol(value, nullptr, 10)); }
inline int _stricmp(const char* a, const char* b) { return ::strcasecmp(a, b); }
#define LOWORD(v) ((uint16_t)((uintptr_t)(v) & 0xFFFF))
#define MAKEINTRESOURCEW(i) ((const wchar_t*)(uintptr_t)(uint16_t)(i))
inline HCURSOR LoadCursorW(void*, const wchar_t*) { return nullptr; }
inline HCURSOR SetCursor(HCURSOR) { return nullptr; }
#if XBE_LINUX_WINDOW
namespace input { bool KeyDown(int); }
inline short GetAsyncKeyState(int key) { return input::KeyDown(key) ? short(0x8000) : 0; }
#else
inline short GetAsyncKeyState(int) { return 0; }
#endif
