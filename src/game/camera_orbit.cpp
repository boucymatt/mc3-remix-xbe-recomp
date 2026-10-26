// Rotate the game's desired chase-camera pose before its smoothing/collision pass.
// The guest camera is then used for both scene culling and rendering on every backend.
#include "camera_orbit.h"
#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "debug_tools.h"
#include "kernel/memory.h"
#include <atomic>
#include <chrono>

namespace {
using Clock = std::chrono::steady_clock;
std::atomic<uint64_t> stick{0};
std::atomic<int64_t> sampled_at{0};
int64_t Milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
bool Guest(uint32_t address, uint32_t bytes) {
    return address >= 0x80000000u && uint64_t(address) + bytes <= mem::PhysicalEnd() && mem::IsCommitted(address) &&
           mem::IsCommitted(address + bytes - 1);
}
bool Playing() {
    if (!app::Get().camera_orbit || !mem::Base() || !mem::IsCommitted(0x557ED0))
        return false;
    const auto state = *reinterpret_cast<const uint32_t *>(mem::Base() + 0x557ED0);
    return Guest(state, 10) && *reinterpret_cast<const uint32_t *>(mem::Base() + state + 4) == 2 &&
           mem::Base()[state + 9] == 0;
}
void Apply(uint32_t camera, uint8_t *base) {
    static mc3::CameraOrbit orbit;
    static uint32_t player_before = 0;
    static auto updated = Clock::now();
    const auto now = Clock::now();
    if (!Playing() || !Guest(camera, 0x270)) {
        orbit.Reset();
        player_before = 0;
        updated = now;
        return;
    }
    const uint32_t vtable = *reinterpret_cast<const uint32_t *>(base + camera);
    // The two chase camera classes; exclude cockpit, cinematic and reflection views.
    if (vtable != 0x476BEC && vtable != 0x476AFC)
        return;
    const uint32_t player = Mc3DebugPlayerMatrix();
    if (!player || *reinterpret_cast<const uint32_t *>(base + camera + 0x6C) != player)
        return;
    if (player != player_before) {
        orbit.Reset();
        player_before = player;
        updated = now;
    }
    const float dt = std::chrono::duration<float>(now - updated).count();
    updated = now;
    const uint64_t value = stick.load(std::memory_order_acquire);
    const bool fresh = Milliseconds() - sampled_at.load() < 250 && (value >> 33);
    if (!fresh)
        orbit.Reset();
    else
        orbit.Update(int16_t(value & 0xffff), int16_t((value >> 16) & 0xffff), (value >> 32) & 1, dt);
    if (std::fabs(orbit.yaw) + std::fabs(orbit.pitch) < .00001f)
        return;
    float *desired = reinterpret_cast<float *>(base + camera + 0xE8);
    const float *pivot = reinterpret_cast<const float *>(base + camera + 0xDC);
    if (!orbit.Transform(desired, pivot))
        return;
    if (std::getenv("MC3_ORBIT_TRACE")) {
        static auto reported = Clock::time_point{};
        if (now - reported > std::chrono::milliseconds(500)) {
            LOGR("MC3 orbit: yaw=%.3f pitch=%.3f camera=%08X", orbit.yaw, orbit.pitch, camera);
            reported = now;
        }
    }
}
} // namespace
void Mc3OrbitPad(int port, input::Pad &pad) {
    if (port != 0)
        return;
    const bool active = Playing();
    const uint64_t value = active ? uint64_t(uint16_t(pad.rx)) | (uint64_t(uint16_t(pad.ry)) << 16) |
                                        (uint64_t((pad.buttons >> 7) & 1) << 32) | (uint64_t(pad.connected) << 33)
                                  : 0;
    stick.store(value, std::memory_order_release);
    sampled_at = Milliseconds();
    if (active) {
        pad.rx = pad.ry = 0;
        pad.buttons &= ~uint16_t(1u << 7);
    }
}
// 0x357960 calls this after constructing its desired Matrix34 (+0xE8),
// before 0x362EF0 smooths it and 0x3563A0 resolves collisions.
X86_EXTERN_FUNC(__imp__sub_00356210);
X86_FUNC(sub_00356210) {
    const uint32_t camera = ctx.ecx;
    __imp__sub_00356210(ctx, base);
    Apply(camera, base);
}
