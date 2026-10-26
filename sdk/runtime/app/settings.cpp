// the settings file beside the exe
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <SDL3/SDL.h>
#endif

#include <algorithm>
#include <atomic>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/log.h"

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

std::filesystem::path File() { return ExeDir() / kGame.settings; }

}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.motion_blur = t["motion_blur"].value_or(s.motion_blur);
    s.light_streaks = t["light_streaks"].value_or(s.light_streaks);
    s.camera_orbit = t["camera_orbit"].value_or(s.camera_orbit);
    s.road_shine = t["road_shine"].value_or(s.road_shine);
    s.linux_renderer = t["linux_renderer"].value_or(s.linux_renderer);
    s.render_height = std::clamp(t["render_height"].value_or(s.render_height), 480u, 2160u);
    s.fps_cap = std::min(s.fps_cap, 1000u);
    s.width = std::min(s.width, 7680u); s.height = std::min(s.height, 4320u);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    s.render_profile = t["render_profile"].value_or(s.render_profile);
    s.render_features = t["render_features"].value_or(s.render_features);
    s.render_post = t["render_post"].value_or(s.render_post);
    s.render_blur = t["render_blur"].value_or(s.render_blur);
    s.render_shadow = t["render_shadow"].value_or(s.render_shadow);
    s.render_vshadow = t["render_vshadow"].value_or(s.render_vshadow);
    s.render_sun = t["render_sun"].value_or(s.render_sun);
    s.render_refl = t["render_refl"].value_or(s.render_refl);
    s.render_lights = t["render_lights"].value_or(s.render_lights);
    s.gpu_index = int32_t(t["gpu_index"].value_or(int64_t(s.gpu_index)));
    s.guest_memory_mb = t["guest_memory_mb"].value_or(s.guest_memory_mb);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"motion_blur", s.motion_blur},
                  {"light_streaks", s.light_streaks},
                  {"road_shine", s.road_shine},
                  {"camera_orbit", s.camera_orbit},
                  {"linux_renderer", s.linux_renderer},
                  {"render_height", int64_t(s.render_height)},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"join_ip", s.join_ip},
                  {"render_profile", s.render_profile.empty() ? std::string("legacy") : s.render_profile},
                  {"render_features", s.render_features},
                  {"render_post", s.render_post},
                  {"render_blur", s.render_blur},
                  {"render_shadow", s.render_shadow},
                  {"render_vshadow", s.render_vshadow},
                  {"render_sun", s.render_sun},
                  {"render_refl", s.render_refl},
                  {"render_lights", s.render_lights},
                  {"gpu_index", int64_t(s.gpu_index)}};
    if (s.guest_memory_mb != 64) t.insert("guest_memory_mb", int64_t(s.guest_memory_mb));  // absent is retail
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

void Save() {
    std::ofstream f(File());
    f << Table() << "\n";
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

Mode Desktop() {
#ifdef _WIN32
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
        return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
#else
    const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    return mode ? Mode{uint32_t(mode->w), uint32_t(mode->h)} : Mode{1280, 720};
#endif
}

Mode Output() {
    const Settings& s = Get();
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
}

bool Widescreen() {
    // asked per frame and per HUD quad: the desktop mode lookup only runs when the resolution setting changes
    static std::atomic<uint64_t> key{~0ull};
    static std::atomic<bool> wide{true};
    const Settings& s = Get();
    uint64_t k = uint64_t(s.width) << 32 | s.height;
    if (key.exchange(k) != k) {
        Mode m = Output();
        wide = m.width * 2 >= m.height * 3;  // 3:2 or wider
    }
    return wide;
}

float RenderScale(uint32_t guest_height) {
    float h = Output().height * (Get().render_percent / 100.0f);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

}  // namespace app
