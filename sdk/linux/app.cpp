// SDL owns the window and event queue on the main thread; rendering uses its own GL context.
#include "platform.h"
#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include <windows.h>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace linuxapp {
namespace {
SDL_Window* window = nullptr;
SDL_GLContext context = nullptr;
int VirtualKey(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + s - SDL_SCANCODE_A;
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return 0x70 + s - SDL_SCANCODE_F1;
    switch (s) {
        case SDL_SCANCODE_RETURN: return VK_RETURN;
        case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;
        case SDL_SCANCODE_SPACE: return VK_SPACE;
        case SDL_SCANCODE_BACKSPACE: return VK_BACK;
        case SDL_SCANCODE_UP: return VK_UP;
        case SDL_SCANCODE_DOWN: return VK_DOWN;
        case SDL_SCANCODE_LEFT: return VK_LEFT;
        case SDL_SCANCODE_RIGHT: return VK_RIGHT;
        case SDL_SCANCODE_LSHIFT: return VK_LSHIFT;
        case SDL_SCANCODE_RSHIFT: return VK_RSHIFT;
        case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return VK_CONTROL;
        case SDL_SCANCODE_PAGEUP: return VK_PRIOR;
        case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
        default: return 0;
    }
}
}
SDL_Window* Window() { return window; }
SDL_GLContext Context() { return context; }
bool Init() {
    const auto& s = app::Get();
    const bool vulkan = s.linux_renderer == "vulkan";
    if (!vulkan && s.linux_renderer != "opengl") {
        LOGE("Unknown linux_renderer '%s'; choose opengl or vulkan", s.linux_renderer.c_str());
        return false;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        LOGE("SDL initialization: %s", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    const bool hidden = std::getenv("MC3_TEST_HIDDEN") != nullptr;
    window = SDL_CreateWindow("Midnight Club 3 — Linux", s.width ? s.width : 1280, s.height ? s.height : 720,
                              (vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL) | SDL_WINDOW_RESIZABLE | (hidden ? SDL_WINDOW_HIDDEN : 0));
    if (!window || (!vulkan && !(context = SDL_GL_CreateContext(window)))) {
        LOGE("Linux graphics window creation: %s", SDL_GetError());
        return false;
    }
    if (!vulkan) SDL_GL_MakeCurrent(window, nullptr);
    input::Init();
    LOGR("Linux window ready. Enter/Z=A, Escape/X=B, Space=Start, arrows/WASD=steer, right Shift=throttle, left Shift=brake; F11=fullscreen.");
    return true;
}
void Run() {
    const auto began = std::chrono::steady_clock::now();
    const char* limit = std::getenv("MC3_RUN_SECONDS");
    const double seconds = limit ? std::strtod(limit, nullptr) : 0;
    while (!stop.load()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) stop = true;
            else if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED)
                input::OnDevice(e.gdevice.which, e.type == SDL_EVENT_GAMEPAD_ADDED);
            else if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
                const bool down = e.type == SDL_EVENT_KEY_DOWN;
                input::OnKey(VirtualKey(e.key.scancode), down);
                if (e.key.scancode == SDL_SCANCODE_LSHIFT || e.key.scancode == SDL_SCANCODE_RSHIFT)
                    input::OnKey(VK_SHIFT, (e.key.mod & SDL_KMOD_SHIFT) != 0);
                if (down && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F11)
                    SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
                if (down && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F3) gpu::g_capture = true;
            } else if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                for (unsigned k = 0; k < 256; k++) input::OnKey(k, false);
            }
        }
        if (seconds > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count() >= seconds) stop = true;
        SDL_Delay(2);
    }
    LOGR("Linux session ended: %llu frames presented", static_cast<unsigned long long>(frames.load()));
    logx::Flush();
    // The kernel owns detached guest/audio/render threads. As on Windows ExitProcess,
    // terminate together instead of destroying their resources underneath them.
    std::fflush(nullptr);
    std::_Exit(exit_code.load());
}
}

namespace app {
std::filesystem::path ExeDir() {
    static const auto dir = [] {
        std::error_code ec;
        auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        return ec ? std::filesystem::current_path() : exe.parent_path();
    }();
    return dir;
}
int Instance() { const char* e = std::getenv("INSTANCE"); return e ? std::atoi(e) : 0; }
void* Window() { return linuxapp::Window(); }
void ApplyDisplay() {}
void LogSystem() { LOGR("Linux SDL runtime, renderer=%s", Get().linux_renderer.c_str()); LogSettings(); }
bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}
std::recursive_mutex& UiLock() { static std::recursive_mutex m; return m; }
void Pace() {
    static auto next = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto cap = Get().fps_cap;
    // Keep scripted menu navigation reproducible, then measure uncapped gameplay.
    static const uint64_t bench_frame = [] {
        const char* value = std::getenv("MC3_BENCHMARK_FROM_FRAME");
        return value ? std::strtoull(value, nullptr, 10) : 0ull;
    }();
    if (bench_frame && linuxapp::frames.load() >= bench_frame) cap = 0;
    if (!cap) { next = now; return; }
    auto step = std::chrono::nanoseconds(1000000000 / cap);
    next = std::max(next + step, now);
    std::this_thread::sleep_until(next);
}
}
