#pragma once
#include <SDL3/SDL.h>
#include <atomic>

namespace linuxapp {
bool Init();
void Run();
SDL_Window* Window();
SDL_GLContext Context();
inline std::atomic<uint64_t> frames{0};
inline std::atomic<int> exit_code{0};
inline std::atomic<bool> stop{false};
}
