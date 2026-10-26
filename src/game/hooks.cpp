#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "kernel/memory.h"
#include "app/app.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "core/input.h"
#include "debug_tools.h"
#include "camera_orbit.h"
#include "mc2_career_test.h"
#include "mc2_career_bridge.h"

void Mc2ApplyDebugBootArgs();
bool Mc2CityFirstDrawReady();

namespace {
constexpr uint32_t kMcpxBase = 0xFE800000;
constexpr uint32_t kMcpxSize = 0x00800000;
std::chrono::steady_clock::time_point g_counter_start;
uint32_t g_active_flash_self = 0;

uint32_t ApuGlobalSampleCounter() {
    // NV_PAPU_XGSCNT is a 10 MHz free-running counter in the existing
    // xboxrecomp APU model. The guest observes the low 32 bits.
    using namespace std::chrono;
    const auto elapsed = steady_clock::now() - g_counter_start;
    return static_cast<uint32_t>(duration_cast<nanoseconds>(elapsed).count() / 100);
}
}  // namespace

void InitLiveUiState();

void Mc3Init() {
    // Keep F3 diagnostics reproducible when the executable is launched directly.
    // It only chooses the destination; no capture work occurs until F3 is pressed.
#ifdef _WIN32
    _wputenv_s(L"XBE_CAPTURE_DIR", L"gpu_captures");
#else
    setenv("XBE_CAPTURE_DIR", "gpu_captures", 1);
#endif
    Mc3DebugInit();
    if (Mc3DebugHasArg(L"--mc2-test-state")) {
        int rc = mc2::RunCareerStateMachineTests();
        std::exit(rc);
    }
}

void Mc3Loaded() {
    InitLiveUiState();
    Mc2ApplyDebugBootArgs();
    mc2::CareerBridgeInit();
    if (!mem::Commit(kMcpxBase, kMcpxSize)) {
        LOGE("MC3: could not map MCPX MMIO aperture");
        return;
    }
    g_counter_start = std::chrono::steady_clock::now();
    LOGI("MC3: mapped MCPX aperture; XGSCNT supplied by two instruction hooks");
}

bool Mc3ReadApuCounterEax(X86Context& ctx, uint8_t*) {
    ctx.eax = ApuGlobalSampleCounter();
    return true;
}

bool Mc3ReadApuCounterEbx(X86Context& ctx, uint8_t*) {
    ctx.ebx = ApuGlobalSampleCounter();
    return true;
}

#pragma pack(push, 1)
struct Mc3LiveUiState {
    uint32_t magic;          // 0x4D433353 ('MC3S')
    uint32_t version;        // 2
    uint32_t frame_counter;  // increments every frame
    uint32_t state;          // 4 = FMV, 7 = Frontend, 5 = Ingame
    uint32_t movie_ptr;      // != 0 if movie playing
    uint32_t is_fmv;         // 1 if FMV active, 0 otherwise
    uint32_t menu_active;    // 1 if menu widget active
    uint32_t cursor_ready;   // 1 if interactive and ready for selection
    int32_t  cursor_index;   // current selected row index (widget + 0x25C)
    uint32_t widget_state;   // 0 = idle, !=0 = animating/busy (widget + 0x300)
    uint32_t widget_flags;   // widget display flags (widget + 0x44)
    uint32_t active_widget;  // guest address of active widget
    uint32_t manager_ptr;    // guest address of frontend manager
    uint32_t active_menu_vt; // vtable of active menu
    uint32_t pad_buttons;    // current buttons pressed
    uint32_t total_items;    // final valid row index in active menu (widget + 0x258)
    // Host automation input. The controller writes a report and an absolute
    // frame deadline; both the early title/FMV path and XInput see it.
    uint32_t automation_buttons;
    uint32_t automation_a;
    uint32_t automation_b;
    uint32_t automation_rt;
    int32_t automation_lx;
    int32_t automation_ly;
    uint32_t automation_until_frame;
};
#pragma pack(pop)

static Mc3LiveUiState* g_live_ui_state = nullptr;

void InitLiveUiState() {
    if (g_live_ui_state) return;
#ifdef _WIN32
    HANDLE hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Mc3LiveUiState), "Local\\MC3_GAME_STATE");
    if (!hMap) {
        LOGE("MC3: CreateFileMappingA Local\\MC3_GAME_STATE failed err=%lu", GetLastError());
        return;
    }
    g_live_ui_state = static_cast<Mc3LiveUiState*>(MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, sizeof(Mc3LiveUiState)));
    if (!g_live_ui_state) {
        LOGE("MC3: MapViewOfFile failed err=%lu", GetLastError());
        return;
    }
#else
    // Keep in-process state tracking and frame counts in the Linux harness.
    // The external automation interface uses a Windows named file mapping.
    static Mc3LiveUiState state{};
    g_live_ui_state = &state;
    LOGI("MC3: live UI state is process-local; Windows shared-memory automation is unavailable");
#endif
    std::memset(g_live_ui_state, 0, sizeof(Mc3LiveUiState));
    g_live_ui_state->magic = 0x4D433353;
    g_live_ui_state->version = 2;
#ifdef _WIN32
    LOGE("MC3: Initialized live UI state shared memory Local\\MC3_GAME_STATE at %p", g_live_ui_state);
#endif
}

static void UpdateLiveUiState(uint32_t state, uint8_t* base, uint32_t current_pad) {
    InitLiveUiState();

    if (!base) base = mem::Base();
    if (!base) return;

    // Guard against uncommitted memory during early initialization
    if (!mem::IsCommitted(0x557ED0) || !mem::IsCommitted(0x50F148) || !mem::IsCommitted(0x50F19C)) {
        return;
    }

    auto readable = [](uint32_t address, uint32_t bytes) {
#ifdef _WIN32
        // Windows retains its existing SEH guard without per-frame page checks.
        (void)address;
        (void)bytes;
        return true;
#else
        const uint64_t end = uint64_t(address) + bytes;
        if (!bytes || end > (uint64_t{1} << 32)) return false;
        for (uint64_t page = address & ~(uint64_t(mem::kPage) - 1); page < end; page += mem::kPage)
            if (!mem::IsCommitted(uint32_t(page))) return false;
        return true;
#endif
    };

#ifdef _WIN32
    __try
#endif
    {
        if (state == ~0u) {
            const uint32_t state_obj = X86_MEM32(0x557ED0);
            state = (state_obj >= 0x80000000u && state_obj < mem::PhysicalEnd() && readable(state_obj, 8))
                ? X86_MEM32(state_obj + 4) : 0;
        }

        uint32_t movie = X86_MEM32(0x50F19C);
        uint16_t movie_flags = movie && readable(movie, 0x22)
            ? uint16_t(X86_MEM8(movie + 0x20)) | (uint16_t(X86_MEM8(movie + 0x21)) << 8)
            : 0;
        bool is_fmv = (state == 4) || (movie != 0 && (movie_flags & 1u) != 0);

        int32_t cursor = -1;
        uint32_t count = 0;
        uint32_t w_state = ~0u;
        uint32_t w_flags = 0;
        uint32_t active_widget_addr = 0;
        uint32_t active_menu_vt = 0;
        uint32_t dbg_widget[2] = {};
        uint32_t dbg_state[2] = {~0u, ~0u};
        uint32_t dbg_flags[2] = {};
        int32_t dbg_cursor[2] = {-1, -1};
        uint32_t dbg_items[2] = {};
        bool ready = false;
        bool menu_active = false;

        uint32_t manager = X86_MEM32(0x50F148);

        if (manager >= 0x80000000u && manager < mem::PhysicalEnd() && readable(manager, 16)) {
            uint32_t holder = X86_MEM32(manager + 12);
            if (holder >= 0x80000000u && holder < mem::PhysicalEnd() && readable(holder, 0x100)) {
                uint32_t active_menu = X86_MEM32(holder + 12);
                if (active_menu >= 0x80000000u && active_menu < mem::PhysicalEnd() && readable(active_menu, 4)) {
                    active_menu_vt = X86_MEM32(active_menu);
                }

                // Both slots can remain visible during frontend transitions. The
                // first visible one is often a three-row auxiliary panel while
                // the main menu lives in the other slot, so retain the visible
                // candidate with the largest valid row range.
                uint32_t widget_slot = 0;
                for (uint32_t off : {0xFCu, 0xF8u}) {
                    uint32_t widget = X86_MEM32(holder + off);
                    dbg_widget[widget_slot] = widget;
                    if (widget >= 0x80000000u && widget < mem::PhysicalEnd() && readable(widget, 0x304)) {
                        uint32_t s = X86_MEM32(widget + 0x300);
                        uint8_t f = X86_MEM8(widget + 0x44);
                        int32_t c = static_cast<int32_t>(X86_MEM32(widget + 0x25C)); // Cursor selected row index
                        uint32_t items = X86_MEM32(widget + 0x258);                  // Final valid row index
                        dbg_state[widget_slot] = s;
                        dbg_flags[widget_slot] = f;
                        dbg_cursor[widget_slot] = c;
                        dbg_items[widget_slot] = items;
                        if ((f & 1) && (!menu_active || items > count)) {
                            menu_active = true;
                            active_widget_addr = widget;
                            w_state = s;
                            w_flags = f;
                            cursor = c;
                            count = items;
                            // Ready when: not FMV, displayed (flags & 1), and transition state is 0 (idle)
                            if (!is_fmv && s == 0) {
                                ready = true;
                            }
                        }
                    }
                    widget_slot++;
                }
            }
        }

        if (g_live_ui_state) {
            g_live_ui_state->frame_counter++;
            g_live_ui_state->state = state;
            g_live_ui_state->pad_buttons = current_pad;
            g_live_ui_state->movie_ptr = movie;
            g_live_ui_state->is_fmv = is_fmv ? 1 : 0;
            g_live_ui_state->menu_active = menu_active ? 1 : 0;
            g_live_ui_state->cursor_ready = ready ? 1 : 0;
            g_live_ui_state->cursor_index = cursor;
            g_live_ui_state->total_items = count;
            g_live_ui_state->widget_state = w_state;
            g_live_ui_state->widget_flags = w_flags;
            g_live_ui_state->active_widget = active_widget_addr;
            g_live_ui_state->active_menu_vt = active_menu_vt;
            g_live_ui_state->manager_ptr = manager;
        }

    static int32_t last_log_cursor = -999;
    static uint32_t last_log_ready = ~0u;
    static uint32_t last_log_fmv = ~0u;
    static uint32_t last_log_state = ~0u;

        if (cursor != last_log_cursor || ready != last_log_ready || (is_fmv ? 1 : 0) != last_log_fmv || state != last_log_state) {
            LOGE("MC3_UI_STATE: state=%u fmv=%u cursor=%d last_row=%u ready=%u w_state=%08X w_flags=%02X menu_vt=%08X "
                 "FC=%08X/%d/%u/%08X/%02X F8=%08X/%d/%u/%08X/%02X",
                 state, is_fmv ? 1 : 0, cursor, count, ready ? 1 : 0, w_state, w_flags, active_menu_vt,
                 dbg_widget[0], dbg_cursor[0], dbg_items[0], dbg_state[0], dbg_flags[0],
                 dbg_widget[1], dbg_cursor[1], dbg_items[1], dbg_state[1], dbg_flags[1]);
            last_log_cursor = cursor;
            last_log_ready = ready;
            last_log_fmv = is_fmv ? 1 : 0;
            last_log_state = state;
        }
    }
#ifdef _WIN32
    __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
}

void Mc3Pad(int port, input::Pad& pad) {
    if (port == 0) {
        if (g_live_ui_state && g_live_ui_state->frame_counter <= g_live_ui_state->automation_until_frame) {
            pad.buttons |= uint16_t(g_live_ui_state->automation_buttons);
            pad.analog[0] = std::max<uint8_t>(pad.analog[0], uint8_t(g_live_ui_state->automation_a));
            pad.analog[1] = std::max<uint8_t>(pad.analog[1], uint8_t(g_live_ui_state->automation_b));
            pad.analog[7] = std::max<uint8_t>(pad.analog[7], uint8_t(g_live_ui_state->automation_rt));
            if (g_live_ui_state->automation_lx) pad.lx = int16_t(g_live_ui_state->automation_lx);
            if (g_live_ui_state->automation_ly) pad.ly = int16_t(g_live_ui_state->automation_ly);
        }
        UpdateLiveUiState(~0u, mem::Base(), pad.buttons);
        Mc3OrbitPad(port, pad);
    }
}

// The early state machine reads the game's already-translated controller words
// before this title starts calling the XInput HLE. Feed only A and Start into
// those words at the original state dispatch; later states can still use XInput.
void Mc3MainLoopPadProbe(X86Context& ctx, uint8_t* base) {
    static uint32_t previous = 0;
    static uint32_t last_state = ~0u;
    static uint32_t last_movie = ~0u;
    static uint32_t calls = 0;

    const bool career_movie_started =
        mc2::TryStartDeferredCityMovie(ctx, base, Mc2CityFirstDrawReady());
    uint32_t movie = X86_MEM32(0x50F19C);
    uint16_t movie_flags = movie
        ? uint16_t(X86_MEM8(movie + 0x20)) | (uint16_t(X86_MEM8(movie + 0x21)) << 8)
        : 0;
    // GuestCall preserves the probe's registers, so ctx.eax still contains the
    // previous state on the exact tick SetState(4) starts a deferred movie.
    // Let the next main-loop tick observe state 4 before testing for completion.
    if (!career_movie_started)
        mc2::HandleCareerMovieTick(ctx, base, ctx.eax, movie, movie_flags);
    if (calls++ < 3 || ctx.eax != last_state || movie != last_movie) {
        LOGE("MC3: state=%u movie=%08X movie_flags=%04X", ctx.eax, movie, unsigned(movie_flags));
        last_state = ctx.eax;
        last_movie = movie;
    }

    auto down = [](int key) {
#if XBE_LINUX_WINDOW
        if (std::getenv("MC3_INPUT_SCRIPT")) return false;
#endif
        return (GetAsyncKeyState(key) & 0x8000) != 0;
    };
    uint32_t current = 0;
    if (down(VK_SPACE)) current |= 0x800;                // Start
    if (down(VK_RETURN) || down('Z')) current |= 0x40;  // A
#if XBE_LINUX_WINDOW
    // The pre-game state machine predates XInput polling. Include SDL pads and
    // replay input here too, otherwise controllers cannot dismiss the intro.
    const auto host_pad = input::Poll(0);
    if (host_pad.buttons & (1u << 4)) current |= 0x800;
    if (host_pad.analog[0]) current |= 0x40;
#endif
    if (g_live_ui_state && g_live_ui_state->frame_counter <= g_live_ui_state->automation_until_frame) {
        if (g_live_ui_state->automation_buttons & (1u << 4)) current |= 0x800;
        if (g_live_ui_state->automation_a) current |= 0x40;
    }

    // Maintain live UI state in shared memory & logs on every tick
    UpdateLiveUiState(ctx.eax, base, current);
    extern void Mc3EnhancedTrafficTick(X86Context&, uint8_t*);
    Mc3EnhancedTrafficTick(ctx, base);

    // These raw pad buffers only bootstrap the pre-game screens. During
    // gameplay the normal pad poll owns both current and previous buttons;
    // rewriting them here can consume Start before the pause handler sees it.
    if (ctx.eax == 5 || ctx.eax == 2) {
        return;
    }

    constexpr uint32_t pads[][2] = {
        {0x4AFDB4, 0x4AFDC0}, {0x4AFDEC, 0x4AFDF8},
        {0x4AFE24, 0x4AFE30}, {0x4AFE5C, 0x4AFE68},
    };
    for (auto [now, old] : pads) {
        X86_MEM32(now) = current;
        X86_MEM32(old) = previous;
    }
    if (current != previous)
        LOGE("MC3: early-state pad current=%04X previous=%04X", current, previous);
    previous = current;
}

void Mc3TitleUpdateProbe(X86Context& ctx, uint8_t* base) {
    static uint32_t calls = 0;
    static uint8_t last_flags = 0xFF;
    const uint32_t self = ctx.ecx;
    const uint8_t flags = X86_MEM8(self + 0x44);
    if (calls++ < 3 || flags != last_flags) {
        const uint32_t game = X86_MEM32(0x50F15C);
        const uint32_t flash = game >= 0x80000000u && game < mem::PhysicalEnd() ? X86_MEM32(game + 0xFAC) : 0;
        const uint32_t root = flash >= 0x80000000u && flash < mem::PhysicalEnd() ? X86_MEM32(flash + 8) : 0;
        const uint32_t flash_flags = flash >= 0x80000000u && flash < mem::PhysicalEnd()
                                         ? uint32_t(X86_MEM8(flash + 12)) | uint32_t(X86_MEM8(flash + 13)) << 8
                                         : 0;
        LOGE("MC3 title: update self=%08X vtable=%08X flags=%02X timers=%g/%g ready=%u game=%08X flash=%08X root=%08X flash_flags=%04X manager=%08X",
             self, X86_MEM32(self), flags, X86_MEMF32(self + 0x1EC), X86_MEMF32(self + 0x1F0),
             uint32_t(X86_MEM8(self + 0x258)), game, flash, root, flash_flags, X86_MEM32(0x50F148));
        last_flags = flags;
    }
}

void Mc3TitleEventProbe(X86Context& ctx, uint8_t* base) {
    const uint32_t event = X86_MEM32(ctx.esp + 8);
    LOGE("MC3 title: event self=%08X event=%08X arg0=%08X arg2=%08X flags=%02X",
         ctx.ecx, event, X86_MEM32(ctx.esp + 4), X86_MEM32(ctx.esp + 12), X86_MEM8(ctx.ecx + 0x44));
}

void Mc3FlashDrawProbe(X86Context& ctx, uint8_t* base) {
    static uint32_t last_self = 0;
    static uint32_t state7_calls = 0;
    const uint32_t self = ctx.ecx;
    g_active_flash_self = self;
    const bool valid_self = self >= 0x80000000u && self < mem::PhysicalEnd();
    const uint32_t root = valid_self ? X86_MEM32(self + 8) : 0;
    const uint32_t state_obj = X86_MEM32(0x557ED0);
    const uint32_t state = state_obj >= 0x80000000u && state_obj < mem::PhysicalEnd()
                               ? X86_MEM32(state_obj + 4)
                               : ~0u;
    const bool changed = self != last_self;
    if (changed || (state == 7 && state7_calls++ < 24))
        LOGE("MC3 flash: Draw state=%u self=%08X root=%08X arg=%u flags=%02X/%02X",
             state,
             self, root, X86_MEM32(ctx.esp + 4),
             valid_self ? X86_MEM8(self + 12) : 0,
             valid_self ? X86_MEM8(self + 13) : 0);
    last_self = self;
}

void Mc3MenuDrawProbe(X86Context& ctx, uint8_t* base) {
    static uint32_t calls = 0;
    static uint32_t last_self = 0;
    const uint32_t state_obj = X86_MEM32(0x557ED0);
    const uint32_t state = state_obj >= 0x80000000u && state_obj < mem::PhysicalEnd()
                               ? X86_MEM32(state_obj + 4)
                               : ~0u;
    if (ctx.ecx != last_self || calls++ < 24)
        LOGE("MC3 frontend: menu Draw state=%u self=%08X vtable=%08X",
             state, ctx.ecx, X86_MEM32(ctx.ecx));
    last_self = ctx.ecx;
}

void Mc3FlashNodeDrawProbe(X86Context& ctx, uint8_t* base) {
    static uint32_t shown = 0;
    static uint32_t last_flash = 0;
    const uint32_t state_obj = X86_MEM32(0x557ED0);
    const uint32_t state = state_obj >= 0x80000000u && state_obj < mem::PhysicalEnd()
                               ? X86_MEM32(state_obj + 4)
                               : ~0u;
    const uint32_t game = X86_MEM32(0x50F15C);
    const uint32_t frontend_flash = game >= 0x80000000u && game < mem::PhysicalEnd()
                                        ? X86_MEM32(game + 0xFAC)
                                        : 0;
    if (g_active_flash_self != last_flash) {
        shown = 0;
        last_flash = g_active_flash_self;
    }
    if (state != 7 || g_active_flash_self != frontend_flash || shown++ >= 160) return;
    const uint32_t node = ctx.esi;
    const uint32_t drawable = ctx.ecx;
    const uint32_t vtable = ctx.edx;
    const uint32_t target = vtable >= 0x10000u && vtable < 0x80000000u
                                ? X86_MEM32(vtable + 4)
                                : 0;
    LOGE("MC3 flash node: node=%08X flags=%02X kind=%02X drawable=%08X vtable=%08X target=%08X arg0=%08X arg1=%08X",
         node,
         node >= 0x80000000u && node < mem::PhysicalEnd() ? X86_MEM8(node + 0x6C) : 0,
         node >= 0x80000000u && node < mem::PhysicalEnd() ? X86_MEM8(node + 0x43) : 0,
         drawable, vtable, target, X86_MEM32(ctx.esp + 4), X86_MEM32(ctx.esp + 8));
}

void Mc3MenuVirtualDrawProbe(X86Context& ctx, uint8_t* base) {
    static uint32_t shown = 0;
    if (shown++ >= 32) return;
    const uint32_t self = ctx.ecx;
    const uint32_t vtable = ctx.edx;
    const uint32_t target = vtable >= 0x10000u && vtable < 0x80000000u
                                ? X86_MEM32(vtable + 0x20)
                                : 0;
    LOGE("MC3 frontend virtual: self=%08X flags=%08X vtable=%08X target=%08X",
         self,
         self >= 0x80000000u && self < mem::PhysicalEnd() ? X86_MEM32(self + 0x44) : 0,
         vtable, target);
}

// The game heap (the 0x124 byte object at 0x4C9BE0 on this build) is a fixed number of megabytes: 0x12440 pushes 0x2F
// (47) as the second argument here, and 0x2698C0 -> 0x367310 rounds MB << 20 to pages and takes it with XPhysicalAlloc
// (0x18EC6) before handing it to the heap (0x1B3DF0). Nothing asks the kernel how much memory there is, so a 128 MB
// guest (--mc3-guest-memory=128) only reaches the game when this grows by what the retail 64 MB lacks.
X86_EXTERN_FUNC(__imp__sub_002698C0);
X86_FUNC(sub_002698C0) {
    const uint32_t extra = (mem::PhysicalSize() - mem::kPhysicalSize) >> 20;
    if (extra) {
        const uint32_t mb = X86_MEM32(ctx.esp + 8);
        X86_MEM32(ctx.esp + 8) = mb + extra;
        LOGR("MC3MEMORY guest %u MB: game heap %u -> %u MB", mem::PhysicalSize() >> 20, mb, mb + extra);
    }
    __imp__sub_002698C0(ctx, base);
}

// The Xbox Bink timer waits against an audio/timing callback whose reconstructed
// clock advances far too slowly on this host, so BinkWait is answered here from
// the host clock instead: a movie frame is due once its period (FrameRateDiv /
// FrameRate, BINK +0x18 / +0x14) has passed since the previous one was due. The
// schedule accumulates, so a 29.97 movie keeps 29.97 at any presentation rate;
// it resynchronises after a stall. Without this (the earlier "never wait") a
// movie advanced one frame per presented frame and ran fast above 30 fps.
X86_FUNC(sub_003ACDF0) {
    using clock = std::chrono::steady_clock;
    struct Movie { uint32_t frame = ~0u; clock::time_point due; };
    static std::mutex lock;
    static std::unordered_map<uint32_t, Movie> movies;
    const uint32_t bink = X86_MEM32(ctx.esp + 4);
    uint32_t wait = 0;
    if (bink) {
        const uint32_t rate = X86_MEM32(bink + 0x14), div = X86_MEM32(bink + 0x18);
        if (rate && div && rate / div < 240) {
            const auto period = std::chrono::duration_cast<clock::duration>(
                std::chrono::duration<double>(double(div) / double(rate)));
            const uint32_t frame = X86_MEM32(bink + 0x0C);
            const auto now = clock::now();
            std::lock_guard<std::mutex> g(lock);
            Movie& m = movies[bink];
            if (m.frame != frame) {
                // the frame on screen changed: it stays one period after the previous one was due
                m.due = (m.frame == ~0u || now - m.due > std::chrono::milliseconds(250)) ? now + period : m.due + period;
                m.frame = frame;
            }
            wait = now < m.due ? 1u : 0u;
        }
    }
    ctx.eax = wait;
    ctx.esp += 8;  // return address and BINK handle
}

// -----------------------------------------------------------------------------
// Delta time, so the frame rate can be unlocked (settings.toml fps_cap = 0 or any
// value) without changing the game's speed.
//
// The Xbox build runs FIXED STEP: every preset is mode 0 with rate 60 and one
// sample, so mcGame::ApplyFrameRateConfig calls datTimeManager::FixedFrame(60, 1)
// (0x20BB00) and every update advances exactly 1/60 whatever the time it took -
// the game was made for 60 updates a second (front end included). At fps_cap 30
// it ran at half speed, unlocked it ran faster. Mc3DtVariableStep turns the
// fixed mode off right after (what RealTime(0), 0x20BAB0, does), so the timer
// measures real time; Mc3DtNoRounding keeps that measurement unrounded.
//
// What the game advanced by a constant per update is then scaled to the time the
// update really took, against the 60 Hz tick it was tuned for: c -> c * dt * 60.
// dt is the scaled game dt (0x4B2D48: the simulation's, 0 while paused), the raw
// measured dt (0x4B2D70, real time), or for the loading-screen thread the real
// time since the same site last ran.
// -----------------------------------------------------------------------------
namespace {
constexpr float kMaxDt = 0.1f;  // the game's own upper bound (0x4B2D5C)
constexpr float kTick = 60.0f;  // the update rate the game was tuned for

float Clamp(float dt) { return dt > 0.0f ? (dt < kMaxDt ? dt : kMaxDt) : 0.0f; }
float GameDt(uint8_t* base) { return Clamp(X86_MEMF32(0x4B2D48)); }
float RawDt(uint8_t* base) { return Clamp(X86_MEMF32(0x4B2D70)); }

// real seconds since this site last asked; one tick on the first call
float SiteDt(int site) {
    using clock = std::chrono::steady_clock;
    static std::mutex lock;
    static clock::time_point last[8];
    static bool seen[8];
    std::lock_guard<std::mutex> g(lock);
    const auto now = clock::now();
    float dt = 1.0f / kTick;
    if (seen[site]) dt = Clamp(std::chrono::duration<float>(now - last[site]).count());
    last[site] = now;
    seen[site] = true;
    return dt;
}

uint32_t Bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
void Push(X86Context& ctx, uint8_t* base, uint32_t value) {
    ctx.esp -= 4;
    X86_MEM32(ctx.esp) = value;
}
void LoadScalar(X86Xmm& x, float f) {  // movss xmm, m32 clears the upper lanes
    x.f[0] = f;
    x.u[1] = x.u[2] = x.u[3] = 0;
}
constexpr float kFlashA = 0.0333333f;  // 0x3D08882F
constexpr float kC033 = 0.033f;        // 0x3D072B02
constexpr float kC1_30 = 1.0f / 30.0f;
float g_flash_c = kC033;  // one value for both calls of the same update

// once every 5 s, from the sky hook (every frame): frames per second and game
// seconds (0x53475C) per real second - 1.000 is normal speed
void SpeedLog(uint8_t* base) {
    using clock = std::chrono::steady_clock;
    static clock::time_point t0 = clock::now();
    static float g0 = X86_MEMF32(0x53475C);
    static uint32_t frames = 0;
    ++frames;
    const auto now = clock::now();
    const float real = std::chrono::duration<float>(now - t0).count();
    if (real >= 5.0f) {
        const float game = X86_MEMF32(0x53475C) - g0;
        LOGE("MC3 dt: %.1f fps, game %.3f s per real s, dt %.4f", frames / real, game / real, X86_MEMF32(0x4B2D48));
        t0 = now;
        g0 = X86_MEMF32(0x53475C);
        frames = 0;
    }
}
}  // namespace

// 0x208D0, right after ApplyFrameRateConfig's FixedFrame call: variable step.
void Mc3DtVariableStep(X86Context&, uint8_t* base) {
    X86_MEM32(0x534770) = 0;   // fixed mode off
    X86_MEMF32(0x4B2D58) = 0;  // and no fixed step, as RealTime(0) leaves it
}

// 0x20893 ApplyFrameRateConfig: `mov byte [0x534776], bl` turns on the timer's
// rounding of the measured dt UP to 1/60, 1/30, 0.05 or 1/15. Skipped: above 60
// fps every update would count as 1/60.
bool Mc3DtNoRounding(X86Context&, uint8_t*) { return true; }

// mcFlash::Update with a constant dt in the loading screens (0x1AD27, 0x1BA4E,
// 0x1B7B7/0x1B7C7 twice in one update, 0xC7A33). Their thread presents its own
// frames, so the time between its calls follows the frame rate.
bool Mc3DtFlashA(X86Context& ctx, uint8_t* base) { Push(ctx, base, Bits(kFlashA * SiteDt(0) * kTick)); return true; }
bool Mc3DtFlashB(X86Context& ctx, uint8_t* base) { Push(ctx, base, Bits(kFlashA * SiteDt(1) * kTick)); return true; }
bool Mc3DtFlashC1(X86Context& ctx, uint8_t* base) {
    g_flash_c = kC033 * SiteDt(2) * kTick;
    Push(ctx, base, Bits(g_flash_c));
    return true;
}
bool Mc3DtFlashC2(X86Context& ctx, uint8_t* base) { Push(ctx, base, Bits(g_flash_c)); return true; }
bool Mc3DtFlashD(X86Context& ctx, uint8_t* base) { Push(ctx, base, Bits(kC033 * SiteDt(3) * kTick)); return true; }

// 0x9B279: the sky update took 1/60 every update - exactly the game dt now.
bool Mc3DtSky(X86Context& ctx, uint8_t* base) {
    SpeedLog(base);
    Push(ctx, base, Bits(GameDt(base)));
    return true;
}
// 0x2CDACA / 0x2CDAE6: the replay director camera took and accumulated 1/30 per
// update; real time, since it also runs while the replay is paused.
bool Mc3DtReplayArg(X86Context& ctx, uint8_t* base) { Push(ctx, base, Bits(kC1_30 * RawDt(base) * kTick)); return true; }
bool Mc3DtReplayTimer(X86Context& ctx, uint8_t* base) { ctx.xmm[0].f[0] += kC1_30 * RawDt(base) * kTick; return true; }
// 0x2C2686: the camera update at 0x2C1BD0 took 1/30 per update.
bool Mc3DtCamera(X86Context& ctx, uint8_t* base) { Push(ctx, base, Bits(kC1_30 * GameDt(base) * kTick)); return true; }

// 0x125CB0, 0x125EBF mcCarGyro::Update: tune * input * 1/30 as an angular
// impulse every update. 0x29FBA6: a force turned into an impulse with 1/30 (the
// ApplyForceScaled path of the wheels). Both per update, so scaled.
bool Mc3DtGyro(X86Context& ctx, uint8_t* base) { ctx.xmm[0].f[0] *= kC1_30 * GameDt(base) * kTick; return true; }
bool Mc3DtImpulse(X86Context& ctx, uint8_t* base) { LoadScalar(ctx.xmm[0], kC1_30 * GameDt(base) * kTick); return true; }
// 0x1F55D mcGame::Draw: a fade timed as updates since start * 1/30.
bool Mc3DtFade(X86Context& ctx, uint8_t* base) { ctx.xmm[0].f[0] *= kC1_30 * GameDt(base) * kTick; return true; }
// 0x1D21F: the music timer took 0.033 off per update.
bool Mc3DtMusic(X86Context& ctx, uint8_t* base) { ctx.xmm[0].f[0] -= kC033 * RawDt(base) * kTick; return true; }
// 0x15A799: the front end camera transition added 0.033 per update.
bool Mc3DtFrontEndCamera(X86Context& ctx, uint8_t* base) { ctx.xmm[1].f[0] += kC033 * RawDt(base) * kTick; return true; }
// 0x22B18D: creature animations (pedestrians) advance `rate` animation frames
// per update.
bool Mc3DtCreatureAnim(X86Context& ctx, uint8_t* base) {
    ctx.xmm[0].f[0] += X86_MEMF32(ctx.esi + 0x10) * GameDt(base) * kTick;
    return true;
}
// 0x15AE77/0x15AE87, 0x15AED4/0x15AEE4: the movie subtitles are updated with dt
// forced to 1/29.97 per update (and 29.97 as its inverse).
bool Mc3DtSubtitleDt(X86Context& ctx, uint8_t* base) {
    LoadScalar(ctx.xmm[0], RawDt(base) * kTick / 29.97f);
    return true;
}
bool Mc3DtSubtitleRate(X86Context& ctx, uint8_t* base) {
    const float dt = RawDt(base) * kTick / 29.97f;
    LoadScalar(ctx.xmm[0], dt > 0.0f ? 1.0f / dt : 29.97f);
    return true;
}
