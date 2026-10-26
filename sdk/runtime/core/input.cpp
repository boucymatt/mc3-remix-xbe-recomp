// controllers through SDL: Xbox, PlayStation and Switch pads read into the console controller's report
#include "core/input.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iterator>
#include <mutex>

#include <SDL3/SDL.h>
#if XBE_LINUX_WINDOW
#include <windows.h>
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "app/app.h"
#include "core/log.h"

namespace input {

#if XBE_LINUX_WINDOW
void MergeScript(Pad&);
#endif

namespace {

// a controller takes the lowest free port when it is plugged in and keeps it until it is unplugged
std::mutex g_lock;
bool g_sdl = false;
SDL_Gamepad* g_pads[4] = {};

void AddPad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads)
        if (pad && SDL_GetGamepadID(pad) == id) return;
    SDL_Gamepad** slot = std::find(std::begin(g_pads), std::end(g_pads), nullptr);
    if (slot == std::end(g_pads)) return;
    *slot = SDL_OpenGamepad(id);
    const char* name = *slot ? SDL_GetGamepadName(*slot) : SDL_GetError();
    LOGI("input: controller %d %s: %s", int(slot - g_pads) + 1, *slot ? "plugged in" : "failed to open", name ? name : "");
}

void RemovePad(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) {
        if (!g_pads[i] || SDL_GetGamepadID(g_pads[i]) != id) continue;
        SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = nullptr;
        LOGI("input: controller %d unplugged", i + 1);
    }
}

// once a millisecond at most, however many ports and threads read
void UpdatePads() {
#if !XBE_LINUX_WINDOW
    static uint64_t last = 0;
    uint64_t now = SDL_GetTicks();
    if (!g_sdl || now == last) return;
    last = now;
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(e.gdevice.which);
    }
#endif
}

}  // namespace

void Init() {
    // WGI reads the triggers apart and HIDAPI takes PlayStation and Switch pads. Reads come from the guest's threads,
    // so SDL's own thread takes the device notices.
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOGE("input: SDL failed to start, no controllers: %s", SDL_GetError());
        return;
    }
    int v = SDL_GetVersion();
    LOGI("input: SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    std::lock_guard lock(g_lock);
    g_sdl = true;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
}

#if defined(_WIN32) || XBE_LINUX_WINDOW
static std::atomic<bool> g_key_state[256] = {};

void OnKey(uint32_t vk, bool down) {
    if (vk < 256) g_key_state[vk].store(down, std::memory_order_relaxed);
}

// The title can ask for the same controller report hundreds of times in one
// frame. GetAsyncKeyState crosses into win32u on every key, so doing that for
// every report consumed a substantial part of the title thread with dense
// traffic. Keep direct keyboard polling (needed when SDL owns only the gamepad
// subsystem), but take each key at most once per millisecond. Poll is already
// serialized by g_lock.
bool KeyDown(int vk) {
#ifdef _WIN32
    static uint64_t sampled_at = ~0ull;
    static bool known[256] = {};
    static bool direct[256] = {};
    const uint64_t now = SDL_GetTicks();
    if (now != sampled_at) {
        sampled_at = now;
        std::fill(std::begin(known), std::end(known), false);
    }
    const bool valid = vk >= 0 && vk < 256;
    if (valid && !known[vk]) {
        direct[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        known[vk] = true;
    }
    return (valid && direct[vk]) ||
           (valid && g_key_state[vk].load(std::memory_order_relaxed));
#else
    return vk >= 0 && vk < 256 && g_key_state[vk].load(std::memory_order_relaxed);
#endif
}

void MergeKeyboard(Pad& p) {
    p.connected = true;
    p.buttons |= uint16_t(KeyDown(VK_UP)) << 0;
    p.buttons |= uint16_t(KeyDown(VK_DOWN)) << 1;
    p.buttons |= uint16_t(KeyDown(VK_LEFT)) << 2;
    p.buttons |= uint16_t(KeyDown(VK_RIGHT)) << 3;
    p.buttons |= uint16_t(KeyDown(VK_SPACE)) << 4;
    p.buttons |= uint16_t(KeyDown(VK_BACK)) << 5;
    p.buttons |= uint16_t(KeyDown('F')) << 6;
    if (KeyDown(VK_RETURN) || KeyDown('Z')) p.analog[0] = 0xFF;
    if (KeyDown(VK_ESCAPE) || KeyDown('X')) p.analog[1] = 0xFF;
    if (KeyDown('C')) p.analog[2] = 0xFF;
    if (KeyDown('V')) p.analog[3] = 0xFF;
    if (KeyDown('Q')) p.analog[4] = 0xFF;
    if (KeyDown('E')) p.analog[5] = 0xFF;
    if (KeyDown(VK_LSHIFT)) p.analog[6] = 0xFF;
    if (KeyDown(VK_RSHIFT)) p.analog[7] = 0xFF;
    if (KeyDown('D') || KeyDown(VK_RIGHT)) p.lx = 32767;
    else if (KeyDown('A') || KeyDown(VK_LEFT)) p.lx = -32768;
    if (KeyDown('W') || KeyDown(VK_UP)) p.ly = 32767;
    else if (KeyDown('S') || KeyDown(VK_DOWN)) p.ly = -32768;
}
#else
void OnKey(uint32_t, bool) {}
bool KeyDown(int) { return false; }
#endif

void OnDevice(uint32_t id, bool added) {
    std::lock_guard lock(g_lock);
    if (added) AddPad(id); else RemovePad(id);
}

Pad Poll(int port) {
    Pad p;
    {
        std::lock_guard lock(g_lock);
        UpdatePads();
        if (SDL_Gamepad* pad = port >= 0 && port < 4 ? g_pads[port] : nullptr) {
            p.connected = true;
            constexpr SDL_GamepadButton kButtons[] = {
                SDL_GAMEPAD_BUTTON_DPAD_UP,   SDL_GAMEPAD_BUTTON_DPAD_DOWN,  SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,
                SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
            for (int i = 0; i < 8; i++) p.buttons |= uint16_t(SDL_GetGamepadButton(pad, kButtons[i]) << i);
            // A B X Y black white
            constexpr SDL_GamepadButton kAnalog[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                                     SDL_GAMEPAD_BUTTON_WEST,  SDL_GAMEPAD_BUTTON_NORTH,
                                                     SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
            for (int i = 0; i < 6; i++) p.analog[i] = SDL_GetGamepadButton(pad, kAnalog[i]) ? 0xFF : 0;
            auto axis = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(pad, a)); };
            // 0 to 32767 into 0 to 255
            p.analog[6] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0) >> 7);
            p.analog[7] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0) >> 7);
            // SDL's sticks have y down
            // not plain negation: -32768 has no positive
            p.lx = int16_t(axis(SDL_GAMEPAD_AXIS_LEFTX));
            p.ly = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_LEFTY));
            p.rx = int16_t(axis(SDL_GAMEPAD_AXIS_RIGHTX));
            p.ry = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_RIGHTY));
        } else if (port == 0) {
            // Keyboard fallback for development systems without a controller.
#if defined(_WIN32) || XBE_LINUX_WINDOW
            // Applied once below so a physical pad and the host keyboard use
            // the same merge path and a keyboard-only report is not read twice.
            p.connected = true;
#else
            const bool* key = SDL_GetKeyboardState(nullptr);
            p.connected = true;
            p.buttons |= uint16_t(key[SDL_SCANCODE_UP]) << 0;
            p.buttons |= uint16_t(key[SDL_SCANCODE_DOWN]) << 1;
            p.buttons |= uint16_t(key[SDL_SCANCODE_LEFT]) << 2;
            p.buttons |= uint16_t(key[SDL_SCANCODE_RIGHT]) << 3;
            p.buttons |= uint16_t(key[SDL_SCANCODE_SPACE]) << 4;
            p.buttons |= uint16_t(key[SDL_SCANCODE_BACKSPACE]) << 5;
            p.buttons |= uint16_t(key[SDL_SCANCODE_F]) << 6;
            p.analog[0] = (key[SDL_SCANCODE_RETURN] || key[SDL_SCANCODE_Z]) ? 0xFF : 0;
            p.analog[1] = (key[SDL_SCANCODE_ESCAPE] || key[SDL_SCANCODE_X]) ? 0xFF : 0;
            p.analog[2] = key[SDL_SCANCODE_C] ? 0xFF : 0;
            p.analog[3] = key[SDL_SCANCODE_V] ? 0xFF : 0;
            p.analog[4] = key[SDL_SCANCODE_Q] ? 0xFF : 0;
            p.analog[5] = key[SDL_SCANCODE_E] ? 0xFF : 0;
            p.analog[6] = key[SDL_SCANCODE_LSHIFT] ? 0xFF : 0;
            p.analog[7] = key[SDL_SCANCODE_RSHIFT] ? 0xFF : 0;
            p.lx = key[SDL_SCANCODE_D] ? 32767 : key[SDL_SCANCODE_A] ? -32768 : 0;
            p.ly = key[SDL_SCANCODE_W] ? 32767 : key[SDL_SCANCODE_S] ? -32768 : 0;
#endif
        }
#if defined(_WIN32) || XBE_LINUX_WINDOW
        // Keep the host keyboard usable when SDL has claimed port 0 for a physical or virtual pad.
        // MC3's title screen consumes events produced from XInputGetState, so writing its translated
        // button words later in the frame is insufficient for Start.
        if (port == 0) {
            MergeKeyboard(p);

            const uint32_t report = p.buttons | (uint32_t(p.analog[0]) << 16) |
                                    (uint32_t(p.analog[1]) << 24);
            static std::atomic<uint32_t> last_merged_report{~0u};
            const uint32_t old_report = last_merged_report.exchange(report);
            if (old_report != report)
                LOGE("input: merged report buttons=%04X A=%u B=%u", p.buttons, p.analog[0], p.analog[1]);
        }
#endif
    }
#if XBE_LINUX_WINDOW
    if (port == 0) {
        // Replays must not depend on a connected wheel/pad or desktop typing.
        if (std::getenv("MC3_INPUT_SCRIPT")) { p = {}; p.connected = true; }
        MergeScript(p);
    }
#endif
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

// the motors run until set again
void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    std::lock_guard lock(g_lock);
    if (port >= 0 && port < 4 && g_pads[port]) SDL_RumbleGamepad(g_pads[port], left, right, 0);
}

}  // namespace input
