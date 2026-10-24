#pragma once
#include <mutex>

#include "gpu/state.h"

namespace d3d {

gpu::State& State();
std::recursive_mutex& Lock();
// the game's own drawing at this point of the title's: gpu::Extension::HostDraw(id) on the render thread
void HostDraw(uint32_t id);

}  // namespace d3d
