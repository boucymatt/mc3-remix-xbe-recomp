// The renderer's profile: command line over the settings file over legacy, read once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "gpu/render_profile.h"
#include "gpu/shaders.h"

#ifdef _WIN32
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#endif

namespace gpu::render {

namespace {
std::atomic<bool> g_motion_blur_tune_ready{false};
}

bool MotionBlurTuneReady() { return g_motion_blur_tune_ready.load(std::memory_order_acquire); }
void MarkMotionBlurTuneReady() { g_motion_blur_tune_ready.store(true, std::memory_order_release); }

namespace {

struct Name {
    Feature f;
    const char* name;
    const char* alias;  // the spelling with underscores, as a toml key would have it
};
const Name kNames[] = {
    {kPost, "post", "post"},
    {kMaterials, "materials", "materials"},
    {kContactShadow, "contact-shadow", "contact_shadow"},
    {kVehicleShadow, "vehicle-shadow", "vehicle_shadow"},
    {kSunShadow, "sun-shadow", "sun_shadow"},
    {kReflections, "reflections", "reflections"},
    {kLightShadow, "light-shadow", "light_shadow"},
    {kMotionBlur, "motion-blur", "motion_blur"},
};

#ifdef _WIN32
std::string Utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? size_t(n - 1) : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
#else
std::vector<std::string> LinuxArgs() {
    std::ifstream f("/proc/self/cmdline", std::ios::binary);
    std::vector<std::string> out;
    for (std::string arg; std::getline(f, arg, '\0');) out.push_back(std::move(arg));
    return out;
}
#endif

std::string Lower(std::string s) {
    for (char& c : s) c = char(tolower(uint8_t(c)));
    return s;
}

bool ParseProfile(const std::string& v, Profile& p) {
    std::string s = Lower(v);
    if (s == "legacy") p = Profile::Legacy;
    else if (s == "enhanced") p = Profile::Enhanced;
    else if (s == "hybrid") p = Profile::Hybrid;
    else return false;
    return true;
}

// a comma list of feature names; "all" and "none" too; unknown names go to errors
uint32_t ParseFeatures(const std::string& list, std::string& errors) {
    uint32_t mask = 0;
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        std::string item = Lower(list.substr(i, j - i));
        while (!item.empty() && item.front() == ' ') item.erase(item.begin());
        while (!item.empty() && item.back() == ' ') item.pop_back();
        if (item == "all") {
            mask |= kAllFeatures;
        } else if (!item.empty() && item != "none") {
            bool known = false;
            for (const Name& n : kNames) {
                if (item == n.name || item == n.alias) {
                    mask |= n.f;
                    known = true;
                }
            }
            if (!known) errors += "unknown feature '" + item + "'; ";
        }
        i = j + 1;
    }
    return mask;
}

// --name=value from the command line, the last one winning; every one for lists
std::vector<std::string> Args() {
#ifdef _WIN32
    std::vector<std::string> out;
    int n = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!argv) return out;
    for (int i = 1; i < n; i++) out.push_back(Utf8(argv[i]));
    LocalFree(argv);
#else
    auto out = LinuxArgs();
    if (!out.empty()) out.erase(out.begin());
#endif
    return out;
}

bool Value(const std::string& arg, const char* key, std::string& v) {
    size_t k = std::strlen(key);
    if (arg.compare(0, k, key) != 0 || arg.size() <= k || arg[k] != '=') return false;
    v = arg.substr(k + 1);
    return true;
}

bool ParseInt(const std::string& v, int& out) {
    char* end = nullptr;
    long n = strtol(v.c_str(), &end, 10);
    if (v.empty() || *end) return false;
    out = int(n);
    return true;
}

// key=value pairs split by commas; unknown keys and bad numbers go to errors
void ParsePost(const std::string& list, Post& p, std::string& errors) {
    size_t i = 0;
    while (i < list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        std::string item = Lower(list.substr(i, j - i));
        i = j + 1;
        size_t eq = item.find_first_of("=:");
        if (item.empty()) continue;
        if (eq == std::string::npos) {
            errors += "post setting '" + item + "' has no value; ";
            continue;
        }
        std::string k = item.substr(0, eq), v = item.substr(eq + 1);
        char* end = nullptr;
        float f = strtof(v.c_str(), &end);
        if (v.empty() || *end) {
            errors += "post setting " + k + "=" + v + " is not a number; ";
            continue;
        }
        if (k == "fxaa") p.fxaa = f != 0;
        else if (k == "split") p.split = f != 0;
        else if (k == "exposure") p.exposure = std::max(f, 0.0f);
        else if (k == "gamma") p.gamma = std::max(f, 0.05f);
        else if (k == "contrast") p.contrast = std::max(f, 0.0f);
        else if (k == "saturation") p.saturation = std::max(f, 0.0f);
        else if (k == "shoulder") p.shoulder = std::min(std::max(f, 0.0f), 1.0f);
        else if (k == "threshold") p.threshold = std::min(std::max(f, 0.0f), 1.0f);
        else errors += "unknown post setting '" + k + "'; ";
    }
}

void ParseBlur(const std::string& list, Blur& b, std::string& errors) {
    size_t i = 0;
    while (i < list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        std::string item = Lower(list.substr(i, j - i));
        i = j + 1;
        if (item.empty()) continue;
        size_t eq = item.find_first_of("=:");
        std::string k = item.substr(0, eq), v = eq == std::string::npos ? "" : item.substr(eq + 1);
        char* end = nullptr;
        float f = strtof(v.c_str(), &end);
        if (v.empty() || *end || !std::isfinite(f)) {
            errors += "blur setting '" + item + "' is not key=number; ";
            continue;
        }
        auto clamp = [&](float lo, float hi) { return std::min(std::max(f, lo), hi); };
        if (k == "strength") b.strength = clamp(0, 2);
        else if (k == "max_pixels") b.max_pixels = clamp(0, 64);
        else if (k == "samples") b.samples = int(clamp(2, 16));
        else if (k == "split") b.split = f != 0;
        else errors += "unknown blur setting '" + k + "'; ";
    }
}

void ParseShadow(const std::string& list, Shadow& sh, std::string& errors) {
    size_t i = 0;
    while (i < list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        std::string item = Lower(list.substr(i, j - i));
        i = j + 1;
        if (item.empty()) continue;
        size_t eq = item.find_first_of("=:");
        std::string k = item.substr(0, eq), v = eq == std::string::npos ? "" : item.substr(eq + 1);
        char* end = nullptr;
        float f = strtof(v.c_str(), &end);
        if (v.empty() || *end) {
            errors += "shadow setting '" + item + "' is not key=number; ";
            continue;
        }
        auto clamp = [&](float lo, float hi) { return std::min(std::max(f, lo), hi); };
        if (k == "strength") sh.strength = clamp(0, 1);
        else if (k == "width") sh.width = clamp(0.1f, 10);
        else if (k == "length") sh.length = clamp(0.1f, 20);
        else if (k == "ground") sh.ground = clamp(-5, 5);
        else if (k == "top") sh.top = clamp(0, 5);
        else if (k == "fade") sh.fade = clamp(0.05f, 10);
        else if (k == "soft") sh.soft = clamp(0.01f, 1);
        else if (k == "near") sh.near_m = clamp(0, 10000);
        else if (k == "far") sh.far_m = clamp(0.1f, 10000);
        else if (k == "debug") sh.debug = int(f);
        else errors += "unknown shadow setting '" + k + "'; ";
    }
    if (sh.far_m <= sh.near_m) sh.far_m = sh.near_m + 0.1f;
}

void ParseVShadow(const std::string& list, VShadow& sh, std::string& errors) {
    size_t i = 0;
    while (i < list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        std::string item = Lower(list.substr(i, j - i));
        i = j + 1;
        if (item.empty()) continue;
        size_t eq = item.find_first_of("=:");
        std::string k = item.substr(0, eq), v = eq == std::string::npos ? "" : item.substr(eq + 1);
        char* end = nullptr;
        float f = strtof(v.c_str(), &end);
        if (v.empty() || *end) {
            errors += "vshadow setting '" + item + "' is not key=number; ";
            continue;
        }
        auto clamp = [&](float lo, float hi) { return std::min(std::max(f, lo), hi); };
        if (k == "strength") sh.strength = clamp(0, 1);
        else if (k == "size") sh.size = f >= 2048 ? 2048 : f >= 1024 ? 1024 : f >= 512 ? 512 : 256;
        else if (k == "elev") sh.elev = clamp(5, 90);
        else if (k == "game") sh.game = f != 0;
        else if (k == "lumref") sh.lumref = clamp(0.05f, 4);
        else if (k == "floor") sh.floor = clamp(0, 1);
        else if (k == "azim") sh.azim = f;
        else if (k == "radius") sh.radius = clamp(1, 20);
        else if (k == "bias") sh.bias = clamp(0, 1);
        else if (k == "slope") sh.slope = clamp(0, 20);
        else if (k == "pcf") sh.pcf = int(clamp(0, 3));
        else if (k == "top") sh.top = clamp(0, 5);
        else if (k == "near") sh.near_m = clamp(0, 10000);
        else if (k == "far") sh.far_m = clamp(0.1f, 10000);
        else if (k == "boxw") sh.boxw = clamp(0.1f, 100);
        else if (k == "boxl") sh.boxl = clamp(0.1f, 100);
        else if (k == "boxt") sh.boxt = clamp(0.1f, 100);
        else if (k == "debug") sh.debug = int(f);
        else errors += "unknown vshadow setting '" + k + "'; ";
    }
    if (sh.far_m <= sh.near_m) sh.far_m = sh.near_m + 0.1f;
}

void ParseSun(const std::string& list, Sun& sh, std::string& errors) {
    size_t i = 0;
    while (i < list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        std::string item = Lower(list.substr(i, j - i));
        i = j + 1;
        if (item.empty()) continue;
        size_t eq = item.find_first_of("=:");
        std::string k = item.substr(0, eq), v = eq == std::string::npos ? "" : item.substr(eq + 1);
        char* end = nullptr;
        float f = strtof(v.c_str(), &end);
        if (v.empty() || *end) {
            errors += "sun setting '" + item + "' is not key=number; ";
            continue;
        }
        auto clamp = [&](float lo, float hi) { return std::min(std::max(f, lo), hi); };
        if (k == "strength") sh.strength = clamp(0, 1);
        else if (k == "size") sh.size = f >= 4096 ? 4096 : f >= 2048 ? 2048 : f >= 1024 ? 1024 : 512;
        else if (k == "range") sh.range = clamp(10, 300);
        else if (k == "cascades") sh.cascades = f >= 2 ? 2 : 1;
        else if (k == "far") sh.far_m = clamp(20, 1000);
        else if (k == "bias") sh.bias = clamp(0, 2);
        else if (k == "slope") sh.slope = clamp(0, 20);
        else if (k == "pcf") sh.pcf = int(clamp(0, 3));
        else if (k == "game") sh.game = f != 0;
        else if (k == "lumref") sh.lumref = clamp(0.05f, 4);
        else if (k == "floor") sh.floor = clamp(0, 1);
        else if (k == "elev") sh.elev = clamp(5, 90);
        else if (k == "azim") sh.azim = f;
        else if (k == "debug") sh.debug = int(f);
        else errors += "unknown sun setting '" + k + "'; ";
    }
}

void ParseRefl(const std::string& list, Refl& r, std::string& errors) {
    size_t i = 0;
    while (i < list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        std::string item = Lower(list.substr(i, j - i));
        i = j + 1;
        if (item.empty()) continue;
        size_t eq = item.find_first_of("=:");
        std::string k = item.substr(0, eq), v = eq == std::string::npos ? "" : item.substr(eq + 1);
        char* end = nullptr;
        float f = strtof(v.c_str(), &end);
        if (v.empty() || *end) {
            errors += "refl setting '" + item + "' is not key=number; ";
            continue;
        }
        auto clamp = [&](float lo, float hi) { return std::min(std::max(f, lo), hi); };
        if (k == "scale") r.scale = clamp(1, 4);
        else if (k == "strength") r.strength = clamp(0, 4);
        else if (k == "fresnel") r.fresnel = clamp(0.1f, 4);
        else if (k == "boost") r.boost = clamp(0, 4);
        else if (k == "debug") r.debug = int(f);
        else errors += "unknown refl setting '" + k + "'; ";
    }
}

void ParseLights(const std::string& list, Lights& r, std::string& errors) {
    size_t i = 0;
    while (i < list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        std::string item = Lower(list.substr(i, j - i));
        i = j + 1;
        if (item.empty()) continue;
        size_t eq = item.find_first_of("=:");
        std::string k = item.substr(0, eq), v = eq == std::string::npos ? "" : item.substr(eq + 1);
        char* end = nullptr;
        float f = strtof(v.c_str(), &end);
        if (v.empty() || *end) {
            errors += "lights setting '" + item + "' is not key=number; ";
            continue;
        }
        auto clamp = [&](float lo, float hi) { return std::min(std::max(f, lo), hi); };
        if (k == "strength") r.strength = clamp(0, 1);
        else if (k == "size") r.size = f >= 2048 ? 2048 : f >= 1024 ? 1024 : 512;
        else if (k == "fov") r.fov = clamp(20, 150);
        else if (k == "near") r.near_m = clamp(0.05f, 5);
        else if (k == "far") r.far_m = clamp(5, 200);
        else if (k == "fwd") r.fwd = clamp(-5, 5);
        else if (k == "height") r.height = clamp(-2, 5);
        else if (k == "pitch") r.pitch = clamp(-45, 45);
        else if (k == "bias") r.bias = clamp(0, 2);
        else if (k == "slope") r.slope = clamp(0, 0.5f);
        else if (k == "pcf") r.pcf = int(clamp(0, 3));
        else if (k == "debug") r.debug = int(f);
        else if (k == "lamps") r.lamps = int(clamp(0, 4));
        else if (k == "lamp_range") r.lamp_range = clamp(5, 200);
        else if (k == "lamp_radius") r.lamp_radius = clamp(2, 60);
        else if (k == "lamp_strength") r.lamp_strength = clamp(0, 1);
        else if (k == "lamp_fov") r.lamp_fov = clamp(30, 160);
        else if (k == "lamp_size") r.lamp_size = f >= 1024 ? 1024 : f >= 512 ? 512 : 256;
        else if (k == "lamp_far") r.lamp_far = clamp(5, 100);
        else if (k == "lamp_min_h") r.lamp_min_h = clamp(-5, 50);
        else if (k == "lamp_max_h") r.lamp_max_h = clamp(0, 100);
        else if (k == "lamp_drop") r.lamp_drop = clamp(0, 5);
        else if (k == "lamp_hits") r.lamp_hits = int(clamp(1, 600));
        else if (k == "lamp_bias") r.lamp_bias = clamp(0, 2);
        else if (k == "lamp_slope") r.lamp_slope = clamp(0, 0.5f);
        else if (k == "cars") r.cars = int(clamp(0, 4));
        else if (k == "beam_strength") r.beam_strength = clamp(0, 10);
        else if (k == "beam_range") r.beam_range = clamp(5, 100);
        else if (k == "beam_fov") r.beam_fov = clamp(20, 150);
        else if (k == "beam_inner") r.beam_inner = clamp(0, 1);
        else if (k == "beam_pitch") r.beam_pitch = clamp(-45, 20);
        else if (k == "beam_r") r.beam_r = clamp(0, 2);
        else if (k == "beam_g") r.beam_g = clamp(0, 2);
        else if (k == "beam_b") r.beam_b = clamp(0, 2);
        else if (k == "beam_size") r.beam_size = f >= 1024 ? 1024 : f >= 512 ? 512 : 256;
        else if (k == "beam_bias") r.beam_bias = clamp(0, 2);
        else if (k == "beam_slope") r.beam_slope = clamp(0, 0.5f);
        else if (k == "beam_debug") r.beam_debug = int(f);
        else if (k == "beam_cone") r.beam_cone = f != 0;
        else if (k == "beam_fwd") r.beam_fwd = clamp(-5, 10);
        else if (k == "beam_height") r.beam_height = clamp(0, 3);
        else errors += "unknown lights setting '" + k + "'; ";
    }
}

Config Parse() {
    Config c;
    const app::Settings& st = app::Get();
    // the settings file
    if (!st.render_profile.empty()) {
        if (ParseProfile(st.render_profile, c.profile)) c.profile_from = "settings";
        else c.errors += "render_profile '" + st.render_profile + "' in the settings file is not legacy/enhanced/hybrid; ";
    }
    c.requested = ParseFeatures(st.render_features, c.errors);
    ParsePost(st.render_post, c.post, c.errors);
    ParseBlur(st.render_blur, c.blur, c.errors);
    ParseShadow(st.render_shadow, c.shadow, c.errors);
    ParseVShadow(st.render_vshadow, c.vshadow, c.errors);
    ParseSun(st.render_sun, c.sun, c.errors);
    ParseRefl(st.render_refl, c.refl, c.errors);
    ParseLights(st.render_lights, c.lights, c.errors);
    if (st.gpu_index >= 0) {
        c.gpu_index = st.gpu_index;
        c.gpu_from = "settings";
    }
    // the command line wins
    bool features_on_line = false;
    for (const std::string& a : Args()) {
        std::string v;
        if (Value(a, "--mc3-render-profile", v)) {
            if (ParseProfile(v, c.profile)) c.profile_from = "command line";
            else c.errors += "--mc3-render-profile=" + v + " is not legacy/enhanced/hybrid; ";
        } else if (Value(a, "--mc3-render-feature", v) || Value(a, "--mc3-render-features", v)) {
            // the line's features replace the settings file's
            if (!features_on_line) c.requested = 0;
            features_on_line = true;
            c.requested |= ParseFeatures(v, c.errors);
        } else if (Value(a, "--mc3-render-disable", v)) {
            c.disabled |= ParseFeatures(v, c.errors);
        } else if (Value(a, "--mc3-render-capture-ab", v)) {
            c.capture_ab = v;
        } else if (Value(a, "--mc3-gpu-index", v)) {
            int n = -1;
            if (ParseInt(v, n) && n >= 0) {
                c.gpu_index = n;
                c.gpu_from = "command line";
            } else {
                c.errors += "--mc3-gpu-index=" + v + " is not an adapter index; ";
            }
        } else if (Value(a, "--mc3-lights", v)) {
            ParseLights(v, c.lights, c.errors);
        } else if (Value(a, "--mc3-refl", v)) {
            ParseRefl(v, c.refl, c.errors);
        } else if (Value(a, "--mc3-sun", v)) {
            ParseSun(v, c.sun, c.errors);
        } else if (Value(a, "--mc3-vshadow", v)) {
            ParseVShadow(v, c.vshadow, c.errors);
        } else if (Value(a, "--mc3-shadow", v)) {
            ParseShadow(v, c.shadow, c.errors);
        } else if (Value(a, "--mc3-post", v)) {
            ParsePost(v, c.post, c.errors);
        } else if (Value(a, "--mc3-blur", v)) {
            ParseBlur(v, c.blur, c.errors);
        } else if (a == "--mc3-render-timing") {
            c.timing = true;
        }
    }
    if (!c.capture_ab.empty()) c.timing = true;
    uint32_t allowed = 0;
    switch (c.profile) {
        case Profile::Legacy: allowed = kMotionBlur; break;
        case Profile::Enhanced: allowed = kApproved; break;
        case Profile::Hybrid: allowed = c.requested | kHybridDefault; break;
    }
    c.ignored = c.requested & ~allowed;
    allowed &= ~c.disabled;
    c.pending = allowed & ~kImplemented;
    c.effective = allowed & kImplemented;
    std::string key = std::string(ProfileName(c.profile)) + ";" + FeatureNames(c.effective) + ";gpu=" +
                      std::to_string(c.gpu_index);
    if (c.effective & kPost) key += ";" + PostNames(c.post);
    if (c.effective & kMotionBlur) key += ";" + BlurNames(c.blur);
    if (c.effective & kContactShadow) key += ";" + ShadowNames(c.shadow);
    if (c.effective & kVehicleShadow) key += ";" + VShadowNames(c.vshadow);
    if (c.effective & kSunShadow) key += ";" + SunNames(c.sun);
    if (c.effective & kReflections) key += ";" + ReflNames(c.refl);
    if (c.effective & kLightShadow) key += ";" + LightsNames(c.lights);
    c.hash = Hash(key.data(), key.size());
    return c;
}

std::mutex g_lock;
Adapter g_adapter;
Output g_output;

std::string Hex(uint64_t v, int digits) {
    char b[24];
    snprintf(b, sizeof(b), "%0*llX", digits, (unsigned long long)v);
    return b;
}

// the running executable's SHA-256, for the evidence a gate asks for
std::string ExeSha256() {
#ifdef _WIN32
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::ifstream f(path, std::ios::binary);
    if (!f) return "?";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "?";
    std::string out = "?";
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buf(1 << 20);
        while (f) {
            f.read(buf.data(), std::streamsize(buf.size()));
            if (f.gcount() > 0) BCryptHashData(h, reinterpret_cast<PUCHAR>(buf.data()), ULONG(f.gcount()), 0);
        }
        uint8_t d[32];
        if (BCryptFinishHash(h, d, sizeof(d), 0) == 0) {
            out.clear();
            for (uint8_t b : d) out += Hex(b, 2);
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
#else
    // The headless harness has no D3D12 evidence gate or BCrypt dependency.
    return "unavailable (headless)";
#endif
}

}  // namespace

const Config& Get() {
    static const Config c = Parse();
    return c;
}

const char* ProfileName(Profile p) {
    switch (p) {
        case Profile::Legacy: return "legacy";
        case Profile::Enhanced: return "enhanced";
        case Profile::Hybrid: return "hybrid";
    }
    return "?";
}

std::string FeatureNames(uint32_t mask) {
    std::string s;
    for (const Name& n : kNames) {
        if (!(mask & n.f)) continue;
        if (!s.empty()) s += ",";
        s += n.name;
    }
    return s.empty() ? "none" : s;
}

std::string PostNames(const Post& p) {
    char b[200];
    snprintf(b, sizeof(b),
             "fxaa=%d split=%d exposure=%.3f gamma=%.3f contrast=%.3f saturation=%.3f shoulder=%.3f threshold=%.4f",
             p.fxaa, p.split, p.exposure, p.gamma, p.contrast, p.saturation, p.shoulder, p.threshold);
    return b;
}

std::string BlurNames(const Blur& b) {
    char out[120];
    snprintf(out, sizeof(out), "strength=%.3f max_pixels=%.1f samples=%d split=%d", b.strength, b.max_pixels,
             b.samples, b.split);
    return out;
}

std::string ShadowNames(const Shadow& s) {
    char b[240];
    snprintf(b, sizeof(b),
             "strength=%.2f width=%.2f length=%.2f ground=%.2f top=%.2f fade=%.2f soft=%.2f near=%.1f far=%.1f debug=%d",
             s.strength, s.width, s.length, s.ground, s.top, s.fade, s.soft, s.near_m, s.far_m, s.debug);
    return b;
}

std::string VShadowNames(const VShadow& s) {
    char b[260];
    snprintf(b, sizeof(b),
             "strength=%.2f size=%d game=%d lumref=%.2f floor=%.2f elev=%.1f azim=%.1f radius=%.2f bias=%.3f slope=%.2f pcf=%d "
             "top=%.2f near=%.1f far=%.1f debug=%d",
             s.strength, s.size, s.game, s.lumref, s.floor, s.elev, s.azim, s.radius, s.bias, s.slope, s.pcf, s.top, s.near_m,
             s.far_m, s.debug);
    return b;
}

std::string SunNames(const Sun& s) {
    char b[260];
    snprintf(b, sizeof(b),
             "strength=%.2f size=%d cascades=%d range=%.0f far=%.0f bias=%.3f slope=%.2f pcf=%d game=%d lumref=%.2f "
             "floor=%.2f elev=%.1f azim=%.1f debug=%d",
             s.strength, s.size, s.cascades, s.range, s.far_m, s.bias, s.slope, s.pcf, s.game, s.lumref, s.floor, s.elev,
             s.azim, s.debug);
    return b;
}

std::string ReflNames(const Refl& r) {
    char b[160];
    snprintf(b, sizeof(b), "scale=%.2f strength=%.2f fresnel=%.2f boost=%.2f debug=%d", r.scale, r.strength, r.fresnel,
             r.boost, r.debug);
    return b;
}

std::string LightsNames(const Lights& r) {
    char b[900];
    snprintf(b, sizeof(b),
             "strength=%.2f size=%d fov=%.0f near=%.2f far=%.0f fwd=%.2f height=%.2f pitch=%.1f bias=%.3f slope=%.3f pcf=%d "
             "debug=%d lamps=%d lamp_range=%.0f lamp_radius=%.1f lamp_strength=%.2f lamp_fov=%.0f lamp_size=%d lamp_far=%.0f "
             "lamp_min_h=%.1f lamp_max_h=%.1f lamp_drop=%.2f lamp_hits=%d lamp_bias=%.3f lamp_slope=%.3f cars=%d "
             "beam_strength=%.2f beam_range=%.0f beam_fov=%.0f beam_inner=%.2f beam_pitch=%.1f beam_rgb=%.2f,%.2f,%.2f beam_size=%d "
             "beam_debug=%d beam_cone=%d beam_fwd=%.2f beam_height=%.2f",
             r.strength, r.size, r.fov, r.near_m, r.far_m, r.fwd, r.height, r.pitch, r.bias, r.slope, r.pcf, r.debug,
             r.lamps, r.lamp_range, r.lamp_radius, r.lamp_strength, r.lamp_fov, r.lamp_size, r.lamp_far, r.lamp_min_h,
             r.lamp_max_h, r.lamp_drop, r.lamp_hits, r.lamp_bias, r.lamp_slope, r.cars, r.beam_strength, r.beam_range,
             r.beam_fov, r.beam_inner, r.beam_pitch, r.beam_r, r.beam_g, r.beam_b, r.beam_size, r.beam_debug, r.beam_cone, r.beam_fwd,
             r.beam_height);
    return b;
}

std::string CacheIdentity(const Adapter& a) {
    // no luid: it changes across boots; the device ids and the driver do not until the hardware or driver does
    char b[160];
    snprintf(b, sizeof(b), "mc3render-cache-v1 vendor=%04X device=%04X subsys=%08X rev=%02X driver=%s profile=%016llX",
             a.vendor, a.device, a.subsys, a.revision, a.driver.c_str(), (unsigned long long)Get().hash);
    return b;
}

void LogConfig() {
    const Config& c = Get();
    LOGR("MC3RENDER profile=%s features=%s (profile from %s; restart to change, the profile is fixed at boot)",
         ProfileName(c.profile), FeatureNames(c.effective).c_str(), c.profile_from);
    if (c.requested || c.disabled)
        LOGR("MC3RENDER requested=%s disabled=%s ignored=%s pending=%s", FeatureNames(c.requested).c_str(),
             FeatureNames(c.disabled).c_str(), FeatureNames(c.ignored).c_str(), FeatureNames(c.pending).c_str());
    if (c.ignored && c.profile != Profile::Hybrid)
        LOGR("MC3RENDER %s takes %s; pick features one by one with --mc3-render-profile=hybrid",
             ProfileName(c.profile), c.profile == Profile::Legacy ? "only its shared baseline" : "only its approved package");
    if (c.pending) LOGR("MC3RENDER not implemented yet, left off: %s", FeatureNames(c.pending).c_str());
    if (c.effective & kPost) LOGR("MC3RENDER post %s", PostNames(c.post).c_str());
    if (c.effective & kMotionBlur) LOGR("MC3RENDER motion-blur %s", BlurNames(c.blur).c_str());
    if (c.effective & kContactShadow) LOGR("MC3RENDER contact-shadow %s", ShadowNames(c.shadow).c_str());
    if (c.effective & kVehicleShadow) LOGR("MC3RENDER vehicle-shadow %s", VShadowNames(c.vshadow).c_str());
    if (c.effective & kSunShadow) LOGR("MC3RENDER sun-shadow %s", SunNames(c.sun).c_str());
    if (c.effective & kReflections) LOGR("MC3RENDER reflections %s", ReflNames(c.refl).c_str());
    if (c.effective & kLightShadow) LOGR("MC3RENDER light-shadow %s", LightsNames(c.lights).c_str());
    if (!c.errors.empty()) LOGE("MC3RENDER settings ignored: %s", c.errors.c_str());
}

void LogAdapter(const Adapter& a) {
    {
        std::lock_guard lock(g_lock);
        g_adapter = a;
    }
    LOGR("MC3RENDER adapter_index=%d name=\"%s\" luid=%s vram_mb=%llu driver=%s vendor=%04X device=%04X (index from %s)",
         a.index, a.name.c_str(), Hex(a.luid, 16).c_str(), (unsigned long long)a.vram_mb, a.driver.c_str(), a.vendor,
         a.device, Get().gpu_from);
}

void LogOutput(const Output& o) {
    {
        std::lock_guard lock(g_lock);
        g_output = o;
    }
    const Config& c = Get();
    LOGR("MC3RENDER guest=%ux%u output=%ux%u scale=%.3f vsync=%u tearing=%u fps_cap=%u", o.guest_w, o.guest_h, o.out_w,
         o.out_h, o.scale, o.vsync, o.tearing, o.fps_cap);
    LOGR("MC3RENDER pso_cache=memory shader_cache=memory config_hash=%s timing=%u capture_ab=%s",
         Hex(c.hash, 16).c_str(), c.timing, c.capture_ab.empty() ? "-" : c.capture_ab.c_str());
}

void WriteEffectiveConfig() {
    const Config& c = Get();
    Adapter a;
    Output o;
    {
        std::lock_guard lock(g_lock);
        a = g_adapter;
        o = g_output;
    }
    std::string name = "render_effective_config";
    if (app::Instance() > 1) name += "-" + std::to_string(app::Instance());
    std::filesystem::path p = app::ExeDir() / "logs" / (name + ".txt");
    std::ofstream f(p);
    if (!f) {
        LOGE("MC3RENDER could not write %s", p.string().c_str());
        return;
    }
#ifdef _WIN32
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char when[32];
    snprintf(when, sizeof(when), "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    std::string executable = Utf8(exe);
    std::string command_line = Utf8(GetCommandLineW());
#else
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    std::string executable = ec ? "?" : exe.string();
    const std::time_t now = std::time(nullptr);
    std::tm t{};
    char when[32] = "?";
    if (localtime_r(&now, &t)) std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &t);
    std::ostringstream line;
    for (const auto& arg : LinuxArgs()) {
        if (line.tellp() > 0) line << ' ';
        line << std::quoted(arg);
    }
    std::string command_line = line.str();
#endif
    f << "# what took effect this launch; command line over settings.toml over legacy\n";
    f << "written=" << when << "\n";
    f << "exe=" << executable << "\n";
    f << "exe_sha256=" << ExeSha256() << "\n";
    f << "command_line=" << command_line << "\n";
    f << "profile=" << ProfileName(c.profile) << "\n";
    f << "profile_from=" << c.profile_from << "\n";
    f << "features_effective=" << FeatureNames(c.effective) << "\n";
    f << "features_requested=" << FeatureNames(c.requested) << "\n";
    f << "features_disabled=" << FeatureNames(c.disabled) << "\n";
    f << "features_ignored=" << FeatureNames(c.ignored) << "\n";
    f << "features_pending=" << FeatureNames(c.pending) << "\n";
    f << "features_implemented=" << FeatureNames(kImplemented) << "\n";
    f << "features_approved_for_enhanced=" << FeatureNames(kApproved) << "\n";
    f << "gpu_index_requested=" << c.gpu_index << "\n";
    f << "gpu_index_from=" << c.gpu_from << "\n";
    f << "adapter_index=" << a.index << "\n";
    f << "adapter_name=" << a.name << "\n";
    f << "adapter_luid=" << Hex(a.luid, 16) << "\n";
    f << "adapter_vram_mb=" << a.vram_mb << "\n";
    f << "adapter_vendor=" << Hex(a.vendor, 4) << "\n";
    f << "adapter_device=" << Hex(a.device, 4) << "\n";
    f << "adapter_driver=" << a.driver << "\n";
    f << "guest=" << o.guest_w << "x" << o.guest_h << "\n";
    f << "output=" << o.out_w << "x" << o.out_h << "\n";
    f << "render_scale=" << o.scale << "\n";
    f << "vsync=" << o.vsync << "\n";
    f << "tearing=" << o.tearing << "\n";
    f << "fps_cap=" << o.fps_cap << "\n";
    f << "pso_cache=memory\n";
    f << "shader_cache=memory\n";
    f << "cache_identity=" << CacheIdentity(a) << "\n";
    f << "config_hash=" << Hex(c.hash, 16) << "\n";
    f << "post=" << ((c.effective & kPost) ? PostNames(c.post) : std::string("off")) << "\n";
    f << "motion_blur=" << ((c.effective & kMotionBlur) ? BlurNames(c.blur) : std::string("off")) << "\n";
    f << "contact_shadow=" << ((c.effective & kContactShadow) ? ShadowNames(c.shadow) : std::string("off")) << "\n";
    f << "vehicle_shadow=" << ((c.effective & kVehicleShadow) ? VShadowNames(c.vshadow) : std::string("off")) << "\n";
    f << "sun_shadow=" << ((c.effective & kSunShadow) ? SunNames(c.sun) : std::string("off")) << "\n";
    f << "reflections=" << ((c.effective & kReflections) ? ReflNames(c.refl) : std::string("off")) << "\n";
    f << "light_shadow=" << ((c.effective & kLightShadow) ? LightsNames(c.lights) : std::string("off")) << "\n";
    f << "timing=" << c.timing << "\n";
    f << "capture_ab=" << c.capture_ab << "\n";
    f << "errors=" << c.errors << "\n";
    LOGR("MC3RENDER effective config written to %s", p.string().c_str());
}

}  // namespace gpu::render
