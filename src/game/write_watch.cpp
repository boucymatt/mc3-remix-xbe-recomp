// Opt-in diagnostic: --mc3-write-watch=0xVA puts an x64 data breakpoint (DR0, 4-byte write) on one guest address in
// every thread and logs the recompiled function that writes it, by its PDB name (sub_XXXXXXXX = the guest address).
// It observes only: the write goes through. Found the heap block an overflowing native array writes past.

#include "core/log.h"
#include "game/debug_tools.h"
#include "kernel/kernel.h"

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cwchar>

#pragma comment(lib, "dbghelp.lib")

namespace {

uintptr_t g_watch_host = 0;
uint32_t g_watch_va = 0;
volatile LONG g_watch_hits = 0;

LONG CALLBACK WatchHandler(PEXCEPTION_POINTERS ep) {
    CONTEXT* c = ep->ContextRecord;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(c->Dr6 & 1u) || c->Dr0 != g_watch_host)
        return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 &= ~1ull;
    const LONG hit = InterlockedIncrement(&g_watch_hits);
    // allocator churn writes the same spot thousands of times from the same code: log each writer once
    static DWORD64 seen[256];
    static volatile LONG seen_count = 0;
    bool fresh = true;
    for (LONG i = 0; i < seen_count && fresh; ++i) fresh = seen[i] != c->Rip;
    if (fresh && seen_count < 256) seen[InterlockedIncrement(&seen_count) - 1] = c->Rip;
    if (fresh) {
        char buffer[sizeof(SYMBOL_INFO) + 256] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        const bool named = SymFromAddr(GetCurrentProcess(), c->Rip, &displacement, symbol);
        LOGE("MC3WATCH write va=%08X value=%08X hit=%ld rip=%llX %s+0x%llX tid=%lu", g_watch_va,
             *reinterpret_cast<volatile uint32_t*>(g_watch_host), hit, static_cast<unsigned long long>(c->Rip),
             named ? symbol->Name : "?", static_cast<unsigned long long>(displacement), GetCurrentThreadId());
        // a CRT copy or the allocator names nothing useful: the guest return addresses say who called it
        if (kernel::Thread* t = kernel::Current(); t && t->ctx) kernel::LogGuestStack(*t->ctx, 512);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD WINAPI ArmThreads(LPVOID) {
    for (unsigned pass = 0; pass < 3600; ++pass) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 item{sizeof(item)};
        if (snapshot != INVALID_HANDLE_VALUE) {
            if (Thread32First(snapshot, &item)) do {
                if (item.th32OwnerProcessID != GetCurrentProcessId() || item.th32ThreadID == GetCurrentThreadId())
                    continue;
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE,
                                           item.th32ThreadID);
                if (!thread) continue;
                CONTEXT c{};
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (SuspendThread(thread) != DWORD(-1)) {
                    if (GetThreadContext(thread, &c) && c.Dr0 != g_watch_host) {
                        c.Dr0 = g_watch_host;
                        c.Dr7 = (c.Dr7 & ~(15ull << 16)) | 1ull | (13ull << 16);  // L0, write, 4 bytes
                        SetThreadContext(thread, &c);
                    }
                    ResumeThread(thread);
                }
                CloseHandle(thread);
            } while (Thread32Next(snapshot, &item));
            CloseHandle(snapshot);
        }
        Sleep(500);
    }
    return 0;
}

}  // namespace

// once, with the guest memory base
void Mc3WriteWatchStart(uint8_t* base) {
    static bool started = false;
    if (started || !base) return;
    started = true;
    const wchar_t* spec = Mc3DebugArg(L"--mc3-write-watch");
    if (!spec || !*spec) return;
    const unsigned long long va = std::wcstoull(spec, nullptr, 0);
    if (va < 0x10000 || (va & 3)) return;
    g_watch_va = uint32_t(va);
    g_watch_host = reinterpret_cast<uintptr_t>(base) + uintptr_t(va);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    AddVectoredExceptionHandler(1, WatchHandler);
    if (HANDLE thread = CreateThread(nullptr, 0, ArmThreads, nullptr, 0, nullptr)) CloseHandle(thread);
    LOGE("MC3WATCH armed va=%08X host=%llX", g_watch_va, static_cast<unsigned long long>(g_watch_host));
}
