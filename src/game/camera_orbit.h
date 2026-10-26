#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mc3 {
// Host-authored orbit controls. Rates are radians per second, independent of FPS.
struct CameraOrbit {
    float yaw = 0, pitch = 0;
    void Reset() { yaw = pitch = 0; }
    void Update(int16_t x, int16_t y, bool reset, float dt) {
        if (reset) {
            Reset();
            return;
        }
        const float fx = x / 32768.f, fy = y / 32768.f;
        const float length = std::sqrt(fx * fx + fy * fy);
        constexpr float deadzone = .20f;
        if (length <= deadzone || !std::isfinite(dt))
            return;
        const float scale = (std::min(length, 1.f) - deadzone) / ((1.f - deadzone) * length);
        dt = std::clamp(dt, 0.f, .05f);
        yaw = std::remainder(yaw + fx * scale * 2.4f * dt, 6.28318530718f);
        pitch = std::clamp(pitch + fy * scale * 1.5f * dt, -.18f, .95f);
    }
    // Matrix34: three basis rows followed by eye position. Leaves invalid poses untouched.
    bool Transform(float *desired, const float *pivot) const {
        if (std::fabs(yaw) + std::fabs(pitch) < .00001f)
            return false;
        for (int i = 0; i < 12; ++i)
            if (!std::isfinite(desired[i]))
                return false;
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(pivot[i]))
                return false;
        const float dx = desired[9] - pivot[0], dy = desired[10] - pivot[1], dz = desired[11] - pivot[2];
        const float radius = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (radius < 1.f || radius > 100.f)
            return false;
        // Pitch around the original camera right vector, then yaw around world up.
        float axis[3] = {desired[0], desired[1], desired[2]};
        const float length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        if (length < .5f || length > 1.5f)
            return false;
        for (auto &x : axis)
            x /= length;
        const float elevation = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
        const float tilt = std::clamp(elevation + pitch, .05f, 1.20f) - elevation;
        // This camera's right-handed pose needs a negative rotation to raise its eye.
        // Keep the eye above the target plane; the guest still handles world collisions.
        const float cp = std::cos(tilt), sp = -std::sin(tilt), cy = std::cos(yaw), sy = std::sin(yaw);
        auto rotate = [&](float *v) {
            const float dot = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
            const float x = v[0] * cp + (axis[1] * v[2] - axis[2] * v[1]) * sp + axis[0] * dot * (1 - cp);
            const float y = v[1] * cp + (axis[2] * v[0] - axis[0] * v[2]) * sp + axis[1] * dot * (1 - cp);
            const float z = v[2] * cp + (axis[0] * v[1] - axis[1] * v[0]) * sp + axis[2] * dot * (1 - cp);
            v[0] = x * cy + z * sy;
            v[1] = y;
            v[2] = -x * sy + z * cy;
        };
        for (int i = 0; i < 3; ++i)
            rotate(desired + i * 3);
        float offset[3] = {dx, dy, dz};
        rotate(offset);
        for (int i = 0; i < 3; ++i)
            desired[9 + i] = pivot[i] + offset[i];
        return true;
    }
};
} // namespace mc3
namespace input {
struct Pad;
}
void Mc3OrbitPad(int port, input::Pad &pad);
