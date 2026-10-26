#include "game/camera_orbit.h"
#include <cassert>
#include <cmath>
#include <limits>
int main() {
    mc3::CameraOrbit a, b;
    for (int i = 0; i < 60; ++i)
        a.Update(32767, 0, false, 1.f / 60);
    for (int i = 0; i < 240; ++i)
        b.Update(32767, 0, false, 1.f / 240);
    assert(std::fabs(a.yaw - b.yaw) < .0001f);
    mc3::CameraOrbit left;
    for (int i = 0; i < 60; ++i)
        left.Update(-32768, 0, false, 1.f / 60);
    assert(std::fabs(left.yaw + a.yaw) < .001f);
    mc3::CameraOrbit diagonal;
    diagonal.Update(32767, 32767, false, .02f);
    assert(diagonal.yaw < 2.4f * .02f);
    mc3::CameraOrbit invalid;
    invalid.Update(32767, 32767, false, std::numeric_limits<float>::quiet_NaN());
    assert(invalid.yaw == 0 && invalid.pitch == 0);
    for (int i = 0; i < 10000; ++i)
        left.Update(-32768, 0, false, .016f);
    assert(std::isfinite(left.yaw) && std::fabs(left.yaw) <= 3.141593f);
    const float yaw = a.yaw;
    a.Update(2000, -2000, false, .02f);
    assert(a.yaw == yaw);
    for (int i = 0; i < 1000; ++i)
        a.Update(0, 32767, false, .016f);
    assert(a.pitch <= .95f);
    for (int i = 0; i < 1000; ++i)
        a.Update(0, -32768, false, .016f);
    assert(a.pitch >= -.18f);
    a.Update(32767, 32767, true, .02f);
    assert(a.yaw == 0 && a.pitch == 0);
    const float pivot[3] = {0, 1, 0};
    float pose[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 6};
    mc3::CameraOrbit raised;
    raised.pitch = .6f;
    assert(raised.Transform(pose, pivot));
    assert(pose[10] > 1); // Up on the stick raises the eye, rather than moving into the ground.
    assert(std::fabs(std::hypot(pose[10] - 1, pose[11]) - 6) < .0001f);
    for (int row = 0; row < 3; ++row) {
        const float *v = pose + row * 3;
        assert(std::fabs(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] - 1) < .0001f);
    }
    float low[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 6};
    raised.pitch = -.18f;
    assert(raised.Transform(low, pivot));
    assert(low[10] >= pivot[1]);
    float stationary[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 6};
    raised.Reset();
    assert(!raised.Transform(stationary, pivot));
    assert(stationary[11] == 6);
}
