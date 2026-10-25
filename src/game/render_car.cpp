// The cars for the renderer's contact shadow (MC3-0008, gpu/render_profile.h kContactShadow). Each time the title sets
// its view, every car's Matrix34 goes into the device state's host values, so it reaches the render thread in order
// with that frame's draws; the renderer picks the player's as the car its main camera follows. Here the views say
// nothing about that: every car renders its own reflection map from a view of its own, and a race also sets one
// about 860 m away. Nothing here writes guest memory.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "gpu/render_profile.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"
#include "headlight_anchors.h"

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t kCarVtable = 0x46C750;  // its Matrix34 follows 16 bytes in (debug_tools.cpp)
constexpr uint32_t kStateObject = 0x557ED0;
// the game's state: 7 the frontend, 4 a movie; races are 2 (MC2 career runs, 18/10)
constexpr uint32_t kFrontend = 7, kMovie = 4;
std::vector<uint32_t> g_cars;
Clock::time_point g_next_scan;

bool Guest(uint32_t va) { return va >= 0x80000000u && va < mem::PhysicalEnd(); }

uint32_t GuestU32(uint32_t va) {
    if (!Guest(va) || !Guest(va + 3) || !mem::IsCommitted(va) || !mem::IsCommitted(va + 3)) return 0;
    return *reinterpret_cast<const uint32_t*>(mem::Base() + va);
}

// the anchor found for a car, kept while its owner, descriptor and name pointer stay the same: Mc3RenderCar runs on
// every view the title sets, many times a frame, and reading the name byte by byte each time showed in the profile
struct KnownAnchor {
    uint32_t owner = 0, descriptor = 0, name_va = 0;
    const mc3::headlights::Anchor* anchor = nullptr;
};
std::unordered_map<uint32_t, KnownAnchor> g_anchors;  // by Matrix34 address

const mc3::headlights::Anchor* CarHeadlightAnchor(uint32_t matrix) {
    // vehCar+0x40 owns the mcCar. Its +0x50 model descriptor stores a pointer
    // to the vp_* resource name at +0x04. The reverse +0x1C link prevents
    // accidentally matching an unrelated or recycled allocation.
    const uint32_t instance = matrix - 16;
    const uint32_t owner = GuestU32(instance + 0x40);
    if (!Guest(owner) || GuestU32(owner + 0x1C) != instance) return nullptr;
    const uint32_t descriptor = GuestU32(owner + 0x50);
    const uint32_t name_va = GuestU32(descriptor + 0x04);
    if (!Guest(name_va)) return nullptr;
    KnownAnchor& known = g_anchors[matrix];
    if (known.owner == owner && known.descriptor == descriptor && known.name_va == name_va) return known.anchor;
    known = {owner, descriptor, name_va, nullptr};
    const uint8_t* base = mem::Base();
    size_t size = 0;
    for (; size < 63; ++size) {
        const uint32_t at = name_va + uint32_t(size);
        if (!Guest(at) || !mem::IsCommitted(at)) return nullptr;
        const unsigned char c = base[at];
        if (!c) break;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return nullptr;
    }
    if (size < 4 || size == 63 || base[name_va] != 'v' || base[name_va + 1] != 'p' || base[name_va + 2] != '_')
        return nullptr;
    return known.anchor = mc3::headlights::Find(std::string_view(reinterpret_cast<const char*>(base + name_va), size));
}

const float* CarMatrix(uint32_t matrix) {
    uint8_t* base = mem::Base();
    if (!base || !Guest(matrix) || !mem::IsCommitted(matrix - 16) || !mem::IsCommitted(matrix + 44)) return nullptr;
    if (*reinterpret_cast<uint32_t*>(base + matrix - 16) != kCarVtable) return nullptr;
    const float* m = reinterpret_cast<const float*>(base + matrix);
    for (int i = 0; i < 12; i++)
        if (!std::isfinite(m[i]) || std::fabs(m[i]) > 100000) return nullptr;
    return m;
}

uint32_t GameState() {
    if (!mem::IsCommitted(kStateObject)) return 0;
    uint8_t* base = mem::Base();
    uint32_t obj = *reinterpret_cast<uint32_t*>(base + kStateObject);
    return Guest(obj) && mem::IsCommitted(obj + 4) ? *reinterpret_cast<uint32_t*>(base + obj + 4) : 0;
}

// every car's Matrix34 in guest memory: a whole pass over it, done again only when the list goes stale
void ScanCars() {
    auto t0 = Clock::now();
    uint8_t* base = mem::Base();
    g_cars.clear();
    g_anchors.clear();
    for (uint32_t matrix = 0x80000010u; matrix < mem::PhysicalEnd() - 48; matrix += 4)
        if (*reinterpret_cast<uint32_t*>(base + matrix - 16) == kCarVtable && CarMatrix(matrix)) g_cars.push_back(matrix);
    LOGR("MC3RENDER contact-shadow: %zu cars found (scan %.1f ms)", g_cars.size(),
         std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    if (g_cars.size() > gpu::State::kHostCars)
        LOGE("MC3RENDER contact-shadow: only the first %zu cars reach the renderer, the player's may be missing",
             gpu::State::kHostCars);
}

}  // namespace

void Mc3RenderCar(uint32_t type, const float* view) {
    if (type != 0 || !view || (!gpu::render::On(gpu::render::kContactShadow) && !gpu::render::On(gpu::render::kVehicleShadow) &&
                                 !gpu::render::On(gpu::render::kSunShadow) &&
                                 !gpu::render::On(gpu::render::kLightShadow) &&
                                 !gpu::render::On(gpu::render::kMotionBlur))) return;
    auto& host = d3d::State().host;
    // each car checked once a call: the matrices of the first kHostCars valid ones are kept for the copy below
    static std::vector<const float*> valid;
    valid.clear();
    bool stale = g_cars.empty();
    for (uint32_t c : g_cars) {
        const float* m = CarMatrix(c);
        if (!m) stale = true;
        else if (valid.size() < gpu::State::kHostCars) valid.push_back(m);
    }
    auto now = Clock::now();
    if (stale && now >= g_next_scan) {
        g_next_scan = now + std::chrono::seconds(2);
        uint32_t state = GameState();
        static uint32_t last_state = ~0u;
        if (state != last_state) LOGR("MC3RENDER contact-shadow: game state %u", state);
        last_state = state;
        if (state != 0 && state != kFrontend && state != kMovie) ScanCars();
        else g_cars.clear();
        // the list changed: check every car again
        valid.clear();
        for (uint32_t c : g_cars)
            if (const float* m = CarMatrix(c); m && valid.size() < gpu::State::kHostCars) valid.push_back(m);
    }
    const uint8_t* base = mem::Base();
    size_t n = 0;
    for (const float* m : valid) {
        const uint32_t c = uint32_t(reinterpret_cast<const uint8_t*>(m) - base);
        const size_t at = 4 + n * gpu::State::kHostCarStride;
        for (int i = 0; i < 12; i++) host[at + size_t(i)] = m[i];
        const auto* anchor = CarHeadlightAnchor(c);
        host[at + 12] = anchor ? anchor->x : 0.0f;
        host[at + 13] = anchor ? anchor->y : 0.0f;
        host[at + 14] = anchor ? anchor->z : 0.0f;
        host[at + 15] = anchor ? 1.0f : 0.0f;
        n++;
    }
    host[0] = float(n);
}
