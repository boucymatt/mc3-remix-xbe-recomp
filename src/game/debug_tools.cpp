// Host-side debugging aids for the MC3 recomp.  These are equivalents of the
// PS2 freecam/AI-spectator/boot-argument mods, but they deliberately stay out of
// guest simulation state.  Only the host renderer's view matrix is replaced.

#include "debug_tools.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwchar>
#ifndef _WIN32
#include <fstream>
#endif
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#ifdef _WIN32
#include <shellapi.h>
#endif

#include "core/log.h"
#include "kernel/memory.h"

namespace mc2 { void UpdateHookmanBridgeHost(); }
uint32_t Mc3TrafficBuiltCar(uint32_t index, uint32_t* name);  // traffic_vehicle_bridge.cpp
uint32_t Mc3TrafficBuiltCount();

namespace {
using Clock = std::chrono::steady_clock;

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Length(Vec3 v) { return std::sqrt(Dot(v, v)); }
Vec3 Normal(Vec3 v, Vec3 fallback = {0, 0, 1}) {
    float n = Length(v);
    return n > 0.0001f && std::isfinite(n) ? v * (1.0f / n) : fallback;
}

std::vector<std::wstring> g_args;
bool g_initialized = false;
bool g_freecam = false;
bool g_ai_follow = false;
bool g_traffic_follow = false;
float g_traffic_seconds = 6.0f;
uint32_t g_traffic_index = 18;
Clock::time_point g_traffic_since;
bool g_camera_valid = false;
float g_camera_speed = 30.0f;
float g_follow_seconds = 8.0f;
Vec3 g_eye, g_right{1, 0, 0}, g_up{0, 1, 0}, g_forward{0, 0, 1};
float g_yaw = 0, g_pitch = 0, g_roll = 0;
Clock::time_point g_last_update;
Clock::time_point g_last_scan;
Clock::time_point g_last_switch;
std::vector<uint32_t> g_cars;  // Matrix34 address; vtable is address - 16, position is +36.
size_t g_follow_index = 0;
uint32_t g_reported_car = 0;
uint32_t g_player_car = 0;

// Mc3DebugTransform runs on every view transform the title sets, many times a frame, and GetAsyncKeyState crosses into
// win32u on each call: with dense traffic that was a fifth of the title thread. A key is read at most every 8 ms, still
// several times a frame and far shorter than a key press.
bool Down(int key) {
    static std::array<Clock::time_point, 256> read_at{};
    static std::array<bool, 256> down{};
    const size_t k = size_t(key) & 255;
    const auto now = Clock::now();
    if (now - read_at[k] >= std::chrono::milliseconds(8)) {
        read_at[k] = now;
        down[k] = (GetAsyncKeyState(key) & 0x8000) != 0;
    }
    return down[k];
}
bool Pressed(int key) {
    static std::array<bool, 256> was{};
    bool now = Down(key);
    bool edge = now && !was[key & 255];
    was[key & 255] = now;
    return edge;
}

void ReadArgs() {
    static bool read = false;
    if (read) return;
    read = true;
#ifdef _WIN32
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;
    for (int i = 1; i < argc; ++i) g_args.emplace_back(argv[i]);
    LocalFree(argv);
#else
    // Linux exposes the original arguments with NUL separators, so spaces in
    // an argument remain intact. Debug switches and their values are ASCII.
    std::ifstream command_line("/proc/self/cmdline", std::ios::binary);
    std::string arg;
    std::getline(command_line, arg, '\0');  // executable name
    while (std::getline(command_line, arg, '\0'))
        g_args.emplace_back(arg.begin(), arg.end());
#endif
}

float FloatArg(const wchar_t* key, float fallback) {
    const wchar_t* value = Mc3DebugArg(key);
    if (!value || !*value) return fallback;
    wchar_t* end = nullptr;
    float result = std::wcstof(value, &end);
    return end != value && std::isfinite(result) ? result : fallback;
}

void FromView(const float* m) {
    // Direct3D row-vector view matrix: camera axes occupy its columns.
    g_right = Normal({m[0], m[4], m[8]}, {1, 0, 0});
    g_up = Normal({m[1], m[5], m[9]}, {0, 1, 0});
    g_forward = Normal({m[2], m[6], m[10]}, {0, 0, 1});
    g_eye = g_right * -m[12] + g_up * -m[13] + g_forward * -m[14];
    g_yaw = std::atan2(g_forward.x, g_forward.z);
    g_pitch = std::asin(std::clamp(g_forward.y, -1.0f, 1.0f));
    g_roll = 0;
    g_camera_valid = true;
}

void ToView(float* m, Vec3 eye, Vec3 forward, Vec3 up_hint) {
    forward = Normal(forward);
    Vec3 right = Normal(Cross(up_hint, forward), {1, 0, 0});
    Vec3 up = Normal(Cross(forward, right), {0, 1, 0});
    m[0] = right.x;   m[1] = up.x;   m[2] = forward.x;   m[3] = 0;
    m[4] = right.y;   m[5] = up.y;   m[6] = forward.y;   m[7] = 0;
    m[8] = right.z;   m[9] = up.z;   m[10] = forward.z;  m[11] = 0;
    m[12] = -Dot(right, eye);
    m[13] = -Dot(up, eye);
    m[14] = -Dot(forward, eye);
    m[15] = 1;
}

bool ValidCar(uint32_t matrix) {
    if (!matrix || !mem::Base() || !mem::IsCommitted(matrix - 16) || !mem::IsCommitted(matrix + 44)) return false;
    uint8_t* base = mem::Base();
    if (*reinterpret_cast<uint32_t*>(base + matrix - 16) != 0x46C750u) return false;
    const float* p = reinterpret_cast<const float*>(base + matrix + 36);
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) &&
           std::fabs(p[0]) < 100000 && std::fabs(p[1]) < 100000 && std::fabs(p[2]) < 100000;
}

uint32_t VehicleMatrixFromObjectGraph(uint32_t root, uint32_t* first_offset,
                                      uint32_t* second_offset) {
    if (first_offset) *first_offset = UINT32_MAX;
    if (second_offset) *second_offset = UINT32_MAX;
    uint8_t* base = mem::Base();
    if (!base || root < 0x80000000u || root >= mem::PhysicalEnd() ||
        !mem::IsCommitted(root + 0x200)) return 0;

    auto matrix_from_instance = [](uint32_t instance) {
        const uint32_t matrix = instance ? instance + 0x10 : 0;
        return ValidCar(matrix) ? matrix : 0u;
    };
    if (const uint32_t self = matrix_from_instance(root)) return self;
    for (uint32_t off = 0; off < 0x200; off += 4) {
        const uint32_t child = *reinterpret_cast<const uint32_t*>(base + root + off);
        if (const uint32_t direct = matrix_from_instance(child)) {
            if (first_offset) *first_offset = off;
            return direct;
        }
        if (child < 0x80000000u || child >= mem::PhysicalEnd() ||
            !mem::IsCommitted(child + 0x200)) continue;
        for (uint32_t child_off = 0; child_off < 0x200; child_off += 4) {
            const uint32_t instance = *reinterpret_cast<const uint32_t*>(base + child + child_off);
            if (const uint32_t nested = matrix_from_instance(instance)) {
                if (first_offset) *first_offset = off;
                if (second_offset) *second_offset = child_off;
                return nested;
            }
        }
    }
    return 0;
}

void ScanCars() {
    uint32_t previous_player = g_player_car;
    g_cars.clear();
    uint8_t* base = mem::Base();
    if (!base) return;
    for (uint32_t matrix = 0x80000010u; matrix < mem::PhysicalEnd() - 48; matrix += 4) {
        if (*reinterpret_cast<uint32_t*>(base + matrix - 16) == 0x46C750u && ValidCar(matrix))
            g_cars.push_back(matrix);
    }
    g_player_car = std::find(g_cars.begin(), g_cars.end(), previous_player) != g_cars.end() ? previous_player : 0;
    if (!g_player_car) {
        float nearest = 1e30f;
        for (uint32_t matrix : g_cars) {
            const float* p = reinterpret_cast<const float*>(base + matrix + 36);
            Vec3 pos{p[0], p[1], p[2]};
            float d = Dot(pos - g_eye, pos - g_eye);
            if (d < nearest) nearest = d, g_player_car = matrix;
        }
    }
    g_follow_index = 0;
    g_last_scan = Clock::now();
    LOGE("MC3 debug: AI spectator found %zu car matrices; player=%08X", g_cars.size(), g_player_car);
}

uint32_t PlayerMatrixFromGame() {
    // REA maps retail PS2 mcPlayer::GetPlayer(0) to Xbox 0x2BF570.  That
    // function reads mcPlayerManager at 0x558814 and slot zero at +8.  Object
    // offsets differ from PS2, so resolve the short pointer chain by matching
    // the known Matrix34 instance base instead of importing PS2 offsets.
    uint8_t* base = mem::Base();
    if (!base || !mem::IsCommitted(0x558814u)) return 0;
    const uint32_t manager = *reinterpret_cast<const uint32_t*>(base + 0x558814u);
    if (!manager || !mem::IsCommitted(manager + 8)) return 0;
    const uint32_t player = *reinterpret_cast<const uint32_t*>(base + manager + 8);
    if (!player || !mem::IsCommitted(player + 0x200)) return 0;

    return VehicleMatrixFromObjectGraph(player, nullptr, nullptr);
}

void RefreshCars(Clock::time_point now) {
    const float since_scan = std::chrono::duration<float>(now - g_last_scan).count();
    if ((g_cars.empty() && since_scan >= 2.0f) || (!g_cars.empty() && since_scan >= 10.0f)) ScanCars();
    g_cars.erase(std::remove_if(g_cars.begin(), g_cars.end(), [](uint32_t a) { return !ValidCar(a); }), g_cars.end());
    if (std::find(g_cars.begin(), g_cars.end(), g_player_car) == g_cars.end()) g_player_car = 0;
}

void FreeCamera(float* matrix, float dt) {
    float turn = 1.7f * dt;
    if (Down(VK_LEFT)) g_yaw -= turn;
    if (Down(VK_RIGHT)) g_yaw += turn;
    if (Down(VK_UP)) g_pitch += turn;
    if (Down(VK_DOWN)) g_pitch -= turn;
    if (Down('Q')) g_roll -= turn;
    if (Down('E')) g_roll += turn;
    g_pitch = std::clamp(g_pitch, -1.54f, 1.54f);

    float cp = std::cos(g_pitch), sp = std::sin(g_pitch);
    Vec3 forward{std::sin(g_yaw) * cp, sp, std::cos(g_yaw) * cp};
    Vec3 level_right = Normal(Cross({0, 1, 0}, forward), {1, 0, 0});
    Vec3 level_up = Normal(Cross(forward, level_right), {0, 1, 0});
    Vec3 up = Normal(level_up * std::cos(g_roll) + level_right * std::sin(g_roll), level_up);
    Vec3 right = Normal(Cross(up, forward), level_right);
    float speed = g_camera_speed * (Down(VK_SHIFT) ? 4.0f : 1.0f) * (Down(VK_CONTROL) ? 0.2f : 1.0f);
    Vec3 move{};
    if (Down('W')) move = move + forward;
    if (Down('S')) move = move - forward;
    if (Down('D')) move = move + right;
    if (Down('A')) move = move - right;
    if (Down('R')) move = move + up;
    if (Down('F')) move = move - up;
    if (Length(move) > 0.001f) g_eye = g_eye + Normal(move) * (speed * dt);
    g_forward = forward; g_up = up; g_right = right;
    ToView(matrix, g_eye, g_forward, g_up);
}

bool FollowCar(float* matrix, Clock::time_point now) {
    // Before a race there are no car instances. Poll at a low rate instead of
    // scanning the 64 MiB physical window once for every view transform.
    RefreshCars(now);
    if (g_cars.size() < 2) return false;
    if (std::chrono::duration<float>(now - g_last_switch).count() >= g_follow_seconds) {
        g_follow_index = (g_follow_index + 1) % g_cars.size();
        g_last_switch = now;
    }
    if (Pressed(VK_F9)) {
        g_follow_index = (g_follow_index + 1) % g_cars.size();
        g_last_switch = now;
    }

    // The car nearest the title camera is normally the player. Skip it while
    // there are opponents, matching the PS2 mod's "skip player instance" rule.
    uint8_t* base = mem::Base();
    size_t player = g_cars.size();
    for (size_t i = 0; i < g_cars.size(); ++i) {
        if (g_cars[i] == g_player_car) {
            player = i;
            break;
        }
    }
    size_t selected = g_follow_index % g_cars.size();
    if (selected == player) selected = (selected + 1) % g_cars.size();
    // A newly allocated opponent can briefly retain an identity matrix at the
    // world origin. Skip it until the race initializes its transform.
    for (size_t attempts = 0; attempts < g_cars.size(); ++attempts) {
        const float* candidate = reinterpret_cast<const float*>(base + g_cars[selected] + 36);
        if (selected != player && std::fabs(candidate[0]) + std::fabs(candidate[1]) + std::fabs(candidate[2]) > 0.01f) break;
        selected = (selected + 1) % g_cars.size();
    }
    const float* car = reinterpret_cast<const float*>(base + g_cars[selected]);
    Vec3 up = Normal({car[3], car[4], car[5]}, {0, 1, 0});
    Vec3 forward = Normal({car[6], car[7], car[8]}, g_forward);
    Vec3 target{car[9], car[10], car[11]};
    if (g_reported_car != g_cars[selected]) {
        g_reported_car = g_cars[selected];
        LOGE("MC3 debug: following AI slot=%zu matrix=%08X pos=%.1f,%.1f,%.1f player_slot=%zu",
             selected, g_reported_car, target.x, target.y, target.z, player);
    }
    Vec3 eye = target - forward * 14.0f + up * 5.0f;
    ToView(matrix, eye, target + up * 1.5f - eye, up);
    return true;
}
// The Cruise pool's own cars in build order (traffic_vehicle_bridge.cpp), from --mc3-traffic-follow-from (18 by
// default: the first models past San Diego's spawn points, loaded during the race), each for --mc3-traffic-follow
// seconds; F9 goes to the next one. The game's own cameras are turned onto the car, so the city, culling and level of
// detail follow it as they follow the player: each player camera (classes 0x476DE8, 0x476BEC, 0x476AFC, 0x46CC00)
// keeps its target's Matrix34 pointer at +0x6C. Their collision test (CollideType, +0x15C) ignores only the player's
// own car and pulled the camera into the followed one, so it is off meanwhile. Found once a race by the pointer to
// the player's Matrix34, all given back when the race ends or the car goes.
struct CameraSlot {
    uint32_t camera = 0;
    uint32_t collide = 0;  // its own CollideType
};
std::vector<CameraSlot> g_camera_slots;
uint32_t g_camera_player = 0;

void FindCameraSlots(uint8_t* base, uint32_t player) {
    g_camera_slots.clear();
    g_camera_player = player;
    for (uint32_t page = 0x80000000u; page < mem::PhysicalEnd(); page += 0x1000) {
        if (!mem::IsCommitted(page) || !mem::IsCommitted(page - 0x1000)) continue;
        for (uint32_t va = page; va < page + 0x1000; va += 4) {
            if (*reinterpret_cast<const uint32_t*>(base + va) != player) continue;
            const uint32_t camera = va - 0x6C, vt = *reinterpret_cast<const uint32_t*>(base + camera);
            if ((vt == 0x476DE8u || vt == 0x476BECu || vt == 0x476AFCu || vt == 0x46CC00u) &&
                mem::IsCommitted(camera + 0x15C))
                g_camera_slots.push_back({camera, *reinterpret_cast<const uint32_t*>(base + camera + 0x15C)});
        }
    }
    LOGE("MC3 debug: traffic camera found %zu player cameras (player matrix %08X)", g_camera_slots.size(), player);
}

void AimCameras(uint8_t* base, uint32_t target) {
    const bool player = target == g_camera_player;
    for (const CameraSlot& slot : g_camera_slots) {
        if (!mem::IsCommitted(slot.camera + 0x15C)) continue;
        *reinterpret_cast<uint32_t*>(base + slot.camera + 0x6C) = target;
        *reinterpret_cast<uint32_t*>(base + slot.camera + 0x15C) = player ? slot.collide : 0;
    }
}

void ReleaseCameras(uint8_t* base) {
    if (!g_camera_slots.empty() && g_camera_player) AimCameras(base, g_camera_player);
    g_camera_slots.clear();
    g_camera_player = 0;
}

void FollowTraffic(Clock::time_point now) {
    uint8_t* base = mem::Base();
    const uint32_t state_object = base && mem::IsCommitted(0x557ED0) ? *reinterpret_cast<const uint32_t*>(base + 0x557ED0) : 0;
    const bool racing = state_object && mem::IsCommitted(state_object + 4) &&
                        *reinterpret_cast<const uint32_t*>(base + state_object + 4) == 2;
    const uint32_t count = racing ? Mc3TrafficBuiltCount() : 0;
    if (!count) {
        ReleaseCameras(base);
        return;
    }
    static bool started = false;
    if (!started) started = true, g_traffic_since = now;  // the hold starts with the race, not with the boot
    const float held = std::chrono::duration<float>(now - g_traffic_since).count();
    if (held >= g_traffic_seconds || Pressed(VK_F9)) {
        ++g_traffic_index;
        g_traffic_since = now;
    }
    // a car not built yet is waited for (the player meanwhile); one gone is skipped; past the last, wrap
    uint32_t car = 0, name = 0;
    static uint32_t last_count = 0;
    static Clock::time_point grown;
    if (count != last_count) last_count = count, grown = now;
    if (g_traffic_index >= count && std::chrono::duration<float>(now - grown).count() >= g_traffic_seconds * 4)
        g_traffic_index = 0;
    for (uint32_t tries = 0; tries < count && !car && g_traffic_index < count; ++tries) {
        car = Mc3TrafficBuiltCar(g_traffic_index, &name);
        if (!car) g_traffic_index = (g_traffic_index + 1) % count;
    }
    if (g_camera_slots.empty()) {
        if (const uint32_t player = PlayerMatrixFromGame()) FindCameraSlots(base, player);
    }
    if (!car || !mem::IsCommitted(car + 0x40)) {
        if (g_camera_player) AimCameras(base, g_camera_player);
        return;
    }
    static uint32_t reported = UINT32_MAX;
    if (reported != g_traffic_index) {
        reported = g_traffic_index;
        const float* m = reinterpret_cast<const float*>(base + car + 0x10);
        const char* text = name && mem::IsCommitted(name) ? reinterpret_cast<const char*>(base + name) : "?";
        LOGE("MC3 debug: following traffic car #%u '%.40s' vehCar=%08X pos=%.1f,%.1f,%.1f", g_traffic_index, text, car,
             m[9], m[10], m[11]);
    }
    AimCameras(base, car + 0x10);
}
}  // namespace

uint32_t Mc3DebugFindVehicleMatrix(uint32_t root, uint32_t* first_offset,
                                   uint32_t* second_offset) {
    return VehicleMatrixFromObjectGraph(root, first_offset, second_offset);
}

uint32_t Mc3DebugFindVehicleOwnerByVtable(uint32_t vtable, uint32_t* vehicle_matrix,
                                         uint32_t* first_offset, uint32_t* second_offset,
                                         uint32_t* object_count) {
    if (vehicle_matrix) *vehicle_matrix = 0;
    if (first_offset) *first_offset = UINT32_MAX;
    if (second_offset) *second_offset = UINT32_MAX;
    if (object_count) *object_count = 0;
    uint8_t* base = mem::Base();
    if (!base || !vtable) return 0;

    // The Xbox physical-memory window is a contiguous 64 MiB host mapping in
    // this runtime.  Scan it only on the bridge's slow retry cadence.  Matching
    // the class vtable first means a generic nearby vehicle can never be
    // mistaken for an aiBrainHookman instance.
    uint32_t first_object = 0;
    uint32_t matches = 0;
    for (uint32_t object = 0x80000000u; object < mem::PhysicalEnd() - 0x204u; object += 4) {
        if (*reinterpret_cast<const uint32_t*>(base + object) != vtable) continue;
        ++matches;
        if (!first_object) first_object = object;
        uint32_t first = UINT32_MAX, second = UINT32_MAX;
        const uint32_t matrix = VehicleMatrixFromObjectGraph(object, &first, &second);
        if (!matrix) continue;
        if (vehicle_matrix) *vehicle_matrix = matrix;
        if (first_offset) *first_offset = first;
        if (second_offset) *second_offset = second;
        if (object_count) *object_count = matches;
        return object;
    }
    if (object_count) *object_count = matches;
    return first_object;
}

bool Mc3DebugGetClosestVehiclePair(Mc3DebugVehiclePair& pair, uint32_t preferred_opponent) {
    pair = {};
    Mc3DebugInit();
    const auto now = Clock::now();
    RefreshCars(now);
    if (g_cars.size() < 2) return false;

    uint8_t* base = mem::Base();
    if (!base) return false;
    const uint32_t exact_player = PlayerMatrixFromGame();
    if (exact_player) {
        static uint32_t reported_exact_player = 0;
        g_player_car = exact_player;
        if (reported_exact_player != exact_player) {
            reported_exact_player = exact_player;
            LOGE("MC3 debug: exact player Matrix34 from mcPlayer = %08X", exact_player);
        }
    }
    if (!g_player_car) {
        float nearest_camera_sq = 1e30f;
        for (uint32_t matrix : g_cars) {
            const float* p = reinterpret_cast<const float*>(base + matrix + 36);
            const Vec3 pos{p[0], p[1], p[2]};
            const float d2 = Dot(pos - g_eye, pos - g_eye);
            if (d2 < nearest_camera_sq) {
                nearest_camera_sq = d2;
                g_player_car = matrix;
            }
        }
    }
    if (!ValidCar(g_player_car)) return false;

    const float* player = reinterpret_cast<const float*>(base + g_player_car + 36);
    const Vec3 player_pos{player[0], player[1], player[2]};
    uint32_t closest = ValidCar(preferred_opponent) && preferred_opponent != g_player_car
        ? preferred_opponent : 0;
    float closest_horizontal_sq = 1e30f;
    if (closest) {
        const float* p = reinterpret_cast<const float*>(base + closest + 36);
        const float dx = p[0] - player_pos.x;
        const float dz = p[2] - player_pos.z;
        closest_horizontal_sq = dx * dx + dz * dz;
    }
    for (uint32_t matrix : g_cars) {
        if (matrix == g_player_car) continue;
        if (preferred_opponent && closest == preferred_opponent) break;
        const float* p = reinterpret_cast<const float*>(base + matrix + 36);
        const float dx = p[0] - player_pos.x;
        const float dz = p[2] - player_pos.z;
        const float d2 = dx * dx + dz * dz;
        if (d2 < closest_horizontal_sq) {
            closest_horizontal_sq = d2;
            closest = matrix;
        }
    }
    if (!closest) return false;

    const float* opponent = reinterpret_cast<const float*>(base + closest + 36);
    pair.player_matrix = g_player_car;
    pair.opponent_matrix = closest;
    pair.player[0] = player[0]; pair.player[1] = player[1]; pair.player[2] = player[2];
    pair.opponent[0] = opponent[0]; pair.opponent[1] = opponent[1]; pair.opponent[2] = opponent[2];
    return true;
}

bool Mc3DebugHasArg(const wchar_t* key) {
    ReadArgs();
    size_t n = std::wcslen(key);
    for (const auto& arg : g_args)
        if (arg == key || (arg.size() > n && arg.compare(0, n, key) == 0 && arg[n] == L'=')) return true;
    return false;
}

const wchar_t* Mc3DebugArg(const wchar_t* key) {
    ReadArgs();
    size_t n = std::wcslen(key);
    for (const auto& arg : g_args)
        if (arg.size() > n && arg.compare(0, n, key) == 0 && arg[n] == L'=') return arg.c_str() + n + 1;
    return nullptr;
}

void Mc3DebugInit() {
    if (g_initialized) return;
    g_initialized = true;
    ReadArgs();
    g_freecam = Mc3DebugHasArg(L"--mc3-freecam");
    g_ai_follow = Mc3DebugHasArg(L"--mc3-ai-follow");
    g_camera_speed = std::clamp(FloatArg(L"--mc3-camera-speed", 30.0f), 1.0f, 500.0f);
    g_follow_seconds = std::clamp(FloatArg(L"--mc3-ai-follow", 8.0f), 1.0f, 120.0f);
    g_traffic_follow = Mc3DebugHasArg(L"--mc3-traffic-follow");
    g_traffic_seconds = std::clamp(FloatArg(L"--mc3-traffic-follow", 6.0f), 1.0f, 120.0f);
    g_traffic_index = uint32_t(std::clamp(FloatArg(L"--mc3-traffic-follow-from", 18.0f), 0.0f, 511.0f));
    g_traffic_since = Clock::now();
    g_last_update = g_last_scan = g_last_switch = Clock::now();
    LOGE("MC3 debug tools: freecam=%u ai_follow=%u follow_seconds=%.1f speed=%.1f (F7/F8 toggle, F9 next; traffic_follow=%u)",
         unsigned(g_freecam), unsigned(g_ai_follow), g_follow_seconds, g_camera_speed, unsigned(g_traffic_follow));
}

void Mc3RenderCar(uint32_t type, const float* view);  // render_car.cpp

void Mc3DebugTransform(uint32_t type, float* matrix) {
    Mc3RenderCar(type, matrix);  // the title's own view, before the cameras below replace it
    if (type != 0 || !matrix) return;
    Mc3DebugInit();
    auto now = Clock::now();
    float dt = std::clamp(std::chrono::duration<float>(now - g_last_update).count(), 0.0f, 0.05f);
    g_last_update = now;
    if (!g_camera_valid) FromView(matrix);
    if (Pressed(VK_F7)) {
        g_freecam = !g_freecam;
        if (g_freecam) FromView(matrix);
        LOGE("MC3 debug: freecam %s", g_freecam ? "ON" : "OFF");
    }
    if (Pressed(VK_F8)) {
        g_ai_follow = !g_ai_follow;
        if (g_ai_follow) ScanCars();
        LOGE("MC3 debug: AI spectator %s", g_ai_follow ? "ON" : "OFF");
    }
    if (g_freecam) FreeCamera(matrix, dt);
    else if (g_traffic_follow) {
        FromView(matrix);
        FollowTraffic(now);
    }
    else if (g_ai_follow) {
        // Keep the untouched title camera as the reference used to identify
        // the player's car before replacing the host view.
        FromView(matrix);
        FollowCar(matrix, now);
    }
    else FromView(matrix);
    // This view callback is reached in cruise even when MC3 never dispatches
    // the small aiBrainHookman::Update wrapper.  The bridge internally gates
    // itself to MC2 Career and throttles duplicate views from the same frame.
    mc2::UpdateHookmanBridgeHost();
}

uint32_t Mc3DebugPlayerMatrix() { return PlayerMatrixFromGame(); }
