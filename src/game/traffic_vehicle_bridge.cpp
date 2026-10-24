// Enhanced traffic research bridge (MC3-0015).
//
// Ambient traffic supplies the lane target; an independently initialized
// opponent supplies the complete vehicle and physics. Legacy is untouched.

#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "gpu/render_profile.h"
#include "game/debug_tools.h"
#include "game/traffic_vp_pool.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

void Mc3EnhancedTrafficCruiseLoadCheckpoint(X86Context& ctx, uint8_t* base);
X86_EXTERN_FUNC(__imp__sub_002B9670);  // ambient traffic source SetDensity(float)

X86_EXTERN_FUNC(sub_0020DBD0);  // native allocator mutex enter
X86_EXTERN_FUNC(sub_0020DBF0);  // native allocator mutex leave
X86_EXTERN_FUNC(sub_001EC2D0);  // aiOpponent::aiOpponent
X86_EXTERN_FUNC(sub_001ED2F0);  // aiOpponent::Init
X86_EXTERN_FUNC(sub_002C8860);  // mcRacer start pose setter
X86_EXTERN_FUNC(sub_002C8690);  // mcRacer reset: entity Reset and physics activation
X86_EXTERN_FUNC(sub_002BF2A0);  // mcRacer::RemoveFromLevel -> mcCar::RemoveFromLevel 0xFFB40
X86_EXTERN_FUNC(sub_001EC430);  // aiOpponent destructor (does not free owner)
X86_EXTERN_FUNC(sub_001F1710);  // aiTarget::Reset
X86_EXTERN_FUNC(sub_001ED9C0);  // aiOpponent::UpdateInput(aiTarget const&, float)
X86_EXTERN_FUNC(sub_00078940);  // aiAmbientVehicle::Update(float)
X86_EXTERN_FUNC(sub_00079AB0);  // aiAmbientVehicle::Deactivate
X86_EXTERN_FUNC(sub_003308C0);  // vehManager::Instance
X86_EXTERN_FUNC(sub_0001D9B0);  // mcGame::ShouldRunPauseUpdateLoopOnly
X86_EXTERN_FUNC(sub_0020D6F0);  // asset manager virtual Open
X86_EXTERN_FUNC(sub_001A2630);  // release asset stream
X86_EXTERN_FUNC(sub_002105F0);  // load typed resource after Open
X86_EXTERN_FUNC(sub_00105CA0);  // car selection's asynchronous resource preload
X86_EXTERN_FUNC(sub_0026A6A0);  // vehicle-name registry index
X86_EXTERN_FUNC(sub_0020DB50);  // release typed resource buffer
X86_EXTERN_FUNC(sub_00271D20);  // mcCarConfig constructor
X86_EXTERN_FUNC(sub_00270AC0);  // mcCarConfig vehicle type setter
X86_EXTERN_FUNC(sub_00270A90);  // mcCarConfig profile setter
X86_EXTERN_FUNC(sub_001CA210);  // aiOpponentStart::Clear
X86_EXTERN_FUNC(sub_002733B0);  // mcCarConfig::Load(name): tune/vehicle/customdata/<name>.carcfg

namespace {

constexpr uint32_t kGuestLo = 0x80000000u;
constexpr uint32_t kCarVtable = 0x0046C750u;
constexpr uint32_t kTrafficCapacity = 6;
// the independent Cruise pool: one complete car per moving ambient lane. San Diego's Cruise runs 18 at its own 2 %
// density; --mc3-traffic-density-scale raises the lane count (about 100 at x5.5), so the pool has room for 128
constexpr uint32_t kCruiseCapacity = 128;
constexpr uint32_t kOpponentSize = 0x43c;

struct AmbientBridge {
    uint32_t ambient = 0;
    uint32_t owner = 0;
    uint32_t entity = 0;
    uint32_t car = 0;
    uint64_t last_seen = 0;
    uint64_t last_route_update = 0;
    uint64_t last_moved = 0;
    std::array<float, 3> previous_position{};
    float observed_motion = 0;
    bool observed_position = false;
};

struct VehicleTemplate {
    uint32_t name = 0;
    uint32_t config = 0;
    uint32_t opponent = 0;
    uint32_t slot = 0;
    uint32_t start = 0;
    uint32_t init_arg2 = 0;
    uint32_t init_arg3 = 0;
};

struct InitCapture {
    uint32_t opponent = 0;
    uint32_t arg0 = 0;
    uint32_t start = 0;
    uint32_t arg2 = 0;
    uint32_t arg3 = 0;
};

// Render traversal includes many parked cars before live lane traffic.
// A 32-entry observation cache could fill entirely with stationary scenery.
std::array<AmbientBridge, 512> g_ambient{};
std::array<uint32_t, 512> g_detached_ambients{};
std::array<uint32_t, 512> g_cruise_respawn_candidates{};
std::array<VehicleTemplate, kCruiseCapacity> g_templates{};  // a Cruise pool takes one per car
// extra owner slots past the race's own opponents: kTrafficCapacity for the race bridge, kCruiseCapacity for the pool
uint32_t g_extra_capacity = kTrafficCapacity;
// distinct models in the Cruise pool: live-lane builds wait until the state-7 builds have loaded all of them
uint32_t g_cruise_distinct_models = 1;
std::array<InitCapture, 16> g_init_captures{};
uint32_t g_template_count = 0;
uint32_t g_built_count = 0;
// each built car in build order, {vehCar, template name}, for the debug camera (Mc3TrafficBuiltCar)
std::array<std::array<uint32_t, 2>, 512> g_built_cars{};
bool g_manager_ready = false;
bool g_constructing = false;
bool g_monitor_physics = false;
uint32_t g_manager = 0;
uint32_t g_regular_count = 0;
uint64_t g_tick = 0;
uint32_t g_bounded_index_writes = 0;
uint32_t g_cruise_pilot_array = 0;
uint32_t g_cruise_pilot_config = 0;
uint32_t g_cruise_pilot_start = 0;
uint32_t g_cruise_pilot_name = 0;
bool g_cruise_pilot_active = false;
bool g_cruise_pilot_attempted = false;
bool g_cruise_pilot_preloaded = false;
bool g_cruise_load_window_seen = false;
bool g_cruise_city_ambient_reset = false;
bool g_cruise_city_load_marked = false;
unsigned g_cruise_preload_polls = 0;
uint64_t g_cruise_pool_full_tick = 0;
bool g_cruise_recycle_test_fired = false;

struct TrafficTimingStats {
    uint64_t frames = 0;
    uint64_t active_cars = 0;
    uint64_t total_ns = 0;
    uint64_t observe_ns = 0;
    uint64_t collider_ns = 0;
    uint64_t route_ns = 0;
    uint64_t target_ns = 0;
    uint64_t input_ns = 0;
};

TrafficTimingStats g_traffic_timing{};

bool Guest(uint32_t va, uint32_t bytes = 4);
uint32_t U32(uint8_t* base, uint32_t va);
const char* GuestString(uint8_t* base, uint32_t va);

// Native car-resource creation at 0x105B36. Read only: compare the exact
// virtual paths used by ordinary vehicles before attempting Cruise preload.
void LogTrafficCarResourcePath(X86Context& ctx, uint8_t* base) {
    static const bool enabled = Mc3DebugHasArg(L"--mc3-traffic-resource-probe");
    static uint32_t samples = 0;
    if (!enabled || !base || samples >= 24) return;
    LOGE("MC3TRAFFIC resource request path='%s' path_va=%08X type=%08X extra=%08X constructing=%u",
         GuestString(base, ctx.edx), ctx.edx, U32(base, ctx.esp + 4),
         U32(base, ctx.esp + 8), unsigned(g_constructing));
    ++samples;
}


bool Guest(uint32_t va, uint32_t bytes) {
    const bool physical = va >= kGuestLo && va <= mem::PhysicalEnd() - bytes;
    const bool system = va >= mem::kSystemBase && va <= mem::kSystemBase + mem::kSystemSize - bytes;
    const bool stack = va >= mem::kStackBase && va <= mem::kStackBase + mem::kStackSize - bytes;
    return (physical || system || stack) && mem::IsCommitted(va) &&
           mem::IsCommitted(va + bytes - 1);
}

uint32_t U32(uint8_t* base, uint32_t va) {
    return Guest(va) ? *reinterpret_cast<const uint32_t*>(base + va) : 0;
}

// A vehCar render instance has the known 0x46C750 vtable and its Matrix34 at
// +0x10.  Return both pointer-chain offsets so the bridge can later retain the
// actual physical entity instead of copying only its visible matrix.
uint32_t FindCar(uint8_t* base, uint32_t root, uint32_t span,
                 uint32_t& first, uint32_t& second) {
    first = second = UINT32_MAX;
    if (!Guest(root, span)) return 0;
    if (U32(base, root) == kCarVtable) return root;
    // mcCar owns its actual rmcInstance at +0x1C; prefer it over caches.
    const uint32_t direct = U32(base, root + 0x1c);
    if (U32(base, direct) == kCarVtable) {
        first = 0x1c;
        return direct;
    }
    for (uint32_t off = 0; off + 4 <= span; off += 4) {
        const uint32_t child = U32(base, root + off);
        if (!Guest(child, 4)) continue;
        if (U32(base, child) == kCarVtable) {
            first = off;
            return child;
        }
        if (!Guest(child, 0x200)) continue;
        for (uint32_t child_off = 0; child_off < 0x200; child_off += 4) {
            const uint32_t instance = U32(base, child + child_off);
            if (Guest(instance, 4) && U32(base, instance) == kCarVtable) {
                first = off;
                second = child_off;
                return instance;
            }
        }
    }
    return 0;
}

const char* GuestString(uint8_t* base, uint32_t va) {
    if (!Guest(va, 2)) return "?";
    const char* s = reinterpret_cast<const char*>(base + va);
    size_t n = 0;
    for (; n != 96 && Guest(va + uint32_t(n), 1) && s[n]; ++n) {
        const unsigned char c = static_cast<unsigned char>(s[n]);
        if (c < 0x20 || c >= 0x7f) return "?";
    }
    return n && n != 96 ? s : "?";
}

bool Enabled() {
    // The independent vp_* traffic experiment is paused. Keep the game's
    // original ambient traffic in every render profile unless explicitly
    // re-enabled for a controlled research run.
    static const bool requested = Mc3DebugHasArg(L"--mc3-traffic-vp-bridge");
    return requested && gpu::render::Get().profile == gpu::render::Profile::Enhanced;
}

uint32_t CruisePilotCount() {
    static const uint32_t count = [] {
        const wchar_t* value = Mc3DebugArg(L"--mc3-traffic-cruise-count");
        if (!value || !*value) return 1u;
        wchar_t* end = nullptr;
        const unsigned long parsed = std::wcstoul(value, &end, 10);
        if (end == value || *end) return 1u;
        return std::clamp(uint32_t(parsed), 1u, kCruiseCapacity);
    }();
    return count;
}

bool WalkTrafficHeap(uint8_t* base, const char* phase, bool quiet = false) {
    // Native 0x1B435F advances its payload pointer by aligned length + 0x20;
    // 0x1B42DD then subtracts 0x10 to get the next header. Header stride is
    // aligned length + 0x10.
    // Never repair headers or suppress the original heap checker.
    constexpr uint32_t heap = 0x4c9be0;
    const uint32_t begin = *reinterpret_cast<const uint32_t*>(base + heap + 8);
    const uint32_t size = *reinterpret_cast<const uint32_t*>(base + heap + 0x10);
    uint32_t at = begin, previous = 0, blocks = 0;
    uint64_t by_flag[2] = {};  // payload bytes of blocks with header bit 0 clear / set
    uint32_t largest[2] = {};
    while (blocks < 200000 && at >= begin && at < uint64_t(begin) + size) {
        if (!Guest(at, 16)) break;
        const uint32_t flags = U32(base, at);
        const uint32_t length = U32(base, at + 4);
        by_flag[flags & 1] += length;
        largest[flags & 1] = std::max(largest[flags & 1], length);
        const uint64_t next = uint64_t(at) + ((uint64_t(length) + 15) & ~15ull) + 16;
        if ((flags & ~7u) != previous || next <= at || next > uint64_t(begin) + size + 16) {
            LOGE("MC3TRAFFIC heap BAD phase=%s reason=%s heap=%08X begin=%08X size=%08X "
                 "at=%08X prev=%08X flags=%08X length=%08X blocks=%u",
                 phase, (flags & ~7u) != previous ? "backlink" : "length",
                 heap, begin, size, at, previous, flags, length, blocks);
            return false;
        }
        previous = at;
        at = uint32_t(next);
        ++blocks;
    }
    // +0x0C is the capacity (0x1B3DF0, 47 MB retail) and +0x10 the part used so far: the heap grows into the rest
    const uint32_t capacity = *reinterpret_cast<const uint32_t*>(base + heap + 0x0C);
    if (!quiet)
        LOGE("MC3TRAFFIC heap phase=%s begin=%08X size=%08X blocks=%u end=%08X bit0clear=%.2fMB(max %.2f) "
             "bit0set=%.2fMB(max %.2f) cap=%08X untouched=%.2fMB",
             phase, begin, size, blocks, at, by_flag[0] / 1048576.0, largest[0] / 1048576.0,
             by_flag[1] / 1048576.0, largest[1] / 1048576.0, capacity,
             capacity > size ? (capacity - size) / 1048576.0 : 0.0);
    return true;
}

// Read-only invariant for phLevel's active non-collider records. Native
// 0x2AC1B0 resolves type 2 through +0x24 into the 12-byte table at +0x50.
// Catch the first stale mapping before a later banger update dereferences it.
void CheckTrafficPhysics(uint8_t* base, const char* phase) {
    const uint32_t level = *reinterpret_cast<const uint32_t*>(base + 0x5580f0);
    if (!Guest(level, 0x100)) return;
    const uint32_t count = *reinterpret_cast<const uint16_t*>(base + level + 0xe);
    const uint32_t capacity = *reinterpret_cast<const uint16_t*>(base + level + 0x44);
    const uint32_t types = U32(base, level + 0x20);
    const uint32_t indices = U32(base, level + 0x24);
    const uint32_t nodes = U32(base, level + 0x50);
    if (!count || !Guest(types, count) || !Guest(indices, count * 2) ||
        !Guest(nodes, capacity * 12)) return;
    static bool reported = false;
    uint32_t active = 0;
    for (uint32_t id = 0; id < count; ++id) {
        if (base[types + id] != 2) continue;
        ++active;
        const uint32_t index = *reinterpret_cast<const uint16_t*>(base + indices + id * 2);
        const uint32_t row = nodes + index * 12;
        const uint32_t instance = U32(base, row + 8);
        if (index < capacity && mem::IsCommitted(instance) && mem::IsCommitted(instance + 0x3f) &&
            *reinterpret_cast<const uint16_t*>(base + instance + 8) == id) continue;
        if (!reported) {
            LOGE("MC3TRAFFIC physics BAD phase=%s level=%08X id=%u index=%u capacity=%u "
                 "row=%08X instance=%08X actual_id=%u types=%08X indices=%08X nodes=%08X",
                 phase, level, id, index, capacity, row, instance,
                 (mem::IsCommitted(instance) && mem::IsCommitted(instance + 0x3f)) ? unsigned(*reinterpret_cast<const uint16_t*>(base + instance + 8)) : 65535,
                 types, indices, nodes);
            reported = true;
        }
        return;
    }
    if (std::strcmp(phase, "tick") != 0)
        LOGE("MC3TRAFFIC physics phase=%s level=%08X active=%u capacity=%u", phase, level, active, capacity);
}

void LogTrafficTypes(uint8_t* base, const char* phase) {
    const uint32_t manager = *reinterpret_cast<const uint32_t*>(base + 0x50a08c);
    if (!Guest(manager, 0x100)) return;
    for (uint32_t slot = 0; slot < 16; ++slot) {
        const uint32_t row = manager + slot * 16;
        const uint32_t type = U32(base, row + 4);
        if (type)
            LOGE("MC3TRAFFIC type phase=%s slot=%u type=%08X refs=%u", phase, slot,
                 type, unsigned(base[row + 9]));
    }
}

uint32_t GuestCall(X86Context& ctx, uint8_t* base,
                   void (*fn)(X86Context&, uint8_t*), uint32_t ecx,
                   std::initializer_list<uint32_t> args) {
    const X86Context saved = ctx;
    for (auto it = std::rbegin(args); it != std::rend(args); ++it) X86_PUSH32(*it);
    X86_PUSH32(0);
    ctx.ecx = ecx;
    fn(ctx, base);
    const uint32_t result = ctx.eax;
    ctx = saved;
    return result;
}



bool CheckTrafficHeap(X86Context& ctx, uint8_t* base, const char* phase, bool quiet = false) {
    // Match operator new/delete's 0x5348EC gate and 0x20DBD0/0x20DBF0
    // guard. A walk racing a streaming-thread split/coalesce can land in a
    // payload and report a false broken header. Do not mutate the heap.
    const bool guarded = base[0x5348ec] != 0;
    static bool logged = false;
    if (!logged) {
        LOGE("MC3TRAFFIC heap snapshot native_allocator_guard=%u", unsigned(guarded));
        logged = true;
    }
    if (guarded) GuestCall(ctx, base, sub_0020DBD0, 0, {});
    const bool valid = WalkTrafficHeap(base, phase, quiet);
    if (guarded) GuestCall(ctx, base, sub_0020DBF0, 0, {});
    return valid;
}

// aiAmbientVehicle::Deactivate (0x79AB0) removes just its phInst through
// phLevel's vtable+0x18. Keep the rail object alive, but remove its duplicate
// collider before the replacement takes its first physical step.
bool RemoveAmbientCollider(X86Context& ctx, uint8_t* base, uint32_t ambient) {
    if (*reinterpret_cast<const uint16_t*>(base + ambient + 0x2c) == 0xffff)
        return true;
    const uint32_t level = *reinterpret_cast<const uint32_t*>(base + 0x5580f0);
    if (!Guest(level)) return false;
    const uint32_t vt = U32(base, level);
    if (!mem::IsCommitted(vt + 0x18)) return false;
    const uint32_t fn = *reinterpret_cast<const uint32_t*>(base + vt + 0x18);
    const X86Context saved = ctx;
    X86_PUSH32(ambient + 0x24);
    X86_PUSH32(0);
    ctx.ecx = level;
    x86_icall(ctx, base, fn);
    ctx = saved;
    const bool removed = *reinterpret_cast<const uint16_t*>(base + ambient + 0x2c) == 0xffff;
    if (removed && std::find(g_detached_ambients.begin(), g_detached_ambients.end(), ambient) == g_detached_ambients.end()) {
        const auto free_slot = std::find(g_detached_ambients.begin(), g_detached_ambients.end(), 0);
        if (free_slot != g_detached_ambients.end()) *free_slot = ambient;
    }
    return removed;
}

AmbientBridge* ObserveAmbient(uint32_t ambient, uint8_t* base) {
    AmbientBridge* free_slot = nullptr;
    for (auto& item : g_ambient) {
        if (item.ambient == ambient) {
            item.last_seen = g_tick;
            return &item;
        }
        if (!item.ambient && !free_slot) free_slot = &item;
    }
    if (g_manager_ready && (!Guest(U32(base, ambient + 0xfc)) ||
                            U32(base, ambient + 0xc4) == 1)) return nullptr;
    if (!free_slot) {
        // Reuse only an unbridged observation that has been absent for several
        // seconds.  Constructed cars remain owned until the city is torn down.
        for (auto& item : g_ambient) {
            if (!item.entity && g_tick - item.last_seen > 3) {
                free_slot = &item;
                break;
            }
        }
    }
    if (!free_slot) return nullptr;
    *free_slot = {};
    free_slot->ambient = ambient;
    free_slot->last_seen = g_tick;
    return free_slot;
}

bool BuildCompleteVehicle(AmbientBridge& item, const VehicleTemplate& source,
                          X86Context& ctx, uint8_t* base) {
    CheckTrafficHeap(ctx, base, "before-build");
    const uint32_t vehicle_manager = GuestCall(ctx, base, sub_003308C0, 0, {});
    if (!Guest(vehicle_manager, 0x80) || U32(base, vehicle_manager + 0x7c) < 128) {
        LOGE("MC3TRAFFIC bridge disabled: vehicle manager lacks enhanced component capacity; restart with enhanced selected");
        g_manager_ready = false;
        return false;
    }
    const uint32_t array = U32(base, g_manager + 8);
    uint32_t slot = g_regular_count;
    for (; slot < g_regular_count + g_extra_capacity; ++slot) {
        const uint32_t candidate = array + slot * kOpponentSize;
        if (std::none_of(g_ambient.begin(), g_ambient.end(),
                         [candidate](const auto& other) { return other.owner == candidate; }))
            break;
    }
    if (slot == g_regular_count + g_extra_capacity) return false;
    const uint32_t owner = array + slot * kOpponentSize;
    if (!array || !Guest(owner, kOpponentSize)) return false;
    std::memset(base + owner, 0, 0x43c);

    if (!Guest(source.start, 0x88)) return false;
    const uint32_t start = mem::AllocSystem(0x88, 16);
    if (!start) return false;
    std::memcpy(base + start, base + source.start, 0x88);
    // +0x24 is the ambient phInst, whose Matrix34 is another 0x10 bytes in.
    // World translation is therefore ambient+0x58, not ambient+0x48.
    const auto* matrix = reinterpret_cast<const float*>(base + item.ambient + 0x34);
    auto* pose = reinterpret_cast<float*>(base + start + 0x38);
    pose[0] = matrix[9]; pose[1] = matrix[10]; pose[2] = matrix[11];
    pose[3] = std::atan2(matrix[6], matrix[8]);
    GuestCall(ctx, base, sub_001EC2D0, owner, {});
    LogTrafficTypes(base, "before-init");
    g_constructing = true;
    const uint32_t initialized = GuestCall(
        ctx, base, sub_001ED2F0, owner,
        {slot, start, source.init_arg2, source.init_arg3});
    g_constructing = false;
    CheckTrafficHeap(ctx, base, "built");
    CheckTrafficPhysics(base, "built");
    LogTrafficTypes(base, "after-init");
    mem::FreeSystem(start);
    const uint32_t entity = U32(base, owner + 0x20);
    if (!Guest(entity, 0x100)) {
        LOGE("MC3TRAFFIC full-vp init failed ambient=%08X template='%s' result=%u entity=%08X",
             item.ambient, GuestString(base, source.name), initialized, entity);
        return false;
    }

    GuestCall(ctx, base, sub_002C8690, owner + 0xc, {0});
    CheckTrafficHeap(ctx, base, "after-reset");
    uint32_t first = 0, second = 0;
    const uint32_t car = FindCar(base, entity, 0x400, first, second);
    if (!car) {
        LOGE("MC3TRAFFIC full-vp no vehCar ambient=%08X template='%s' entity=%08X",
             item.ambient, GuestString(base, source.name), entity);
        return false;
    }

    item.owner = owner;
    item.entity = entity;
    item.car = car;
    // During state-7 Cruise setup the rail object exists but still carries
    // its waiting state/sentinel lane. Let the traffic director activate it
    // normally; the state-2 bridge will detach its collider once the lane is
    // live. Race-created templates already have a usable route here.
    const bool route_ready = U32(base, item.ambient + 0xc4) != 1 &&
                             Guest(U32(base, item.ambient + 0xfc));
    const bool removed = route_ready && RemoveAmbientCollider(ctx, base, item.ambient);
    const auto* position = reinterpret_cast<const float*>(base + car + 0x34);
    LOGE("MC3TRAFFIC full-vp built ambient=%08X template='%s' owner=%08X entity=%08X "
         "vehCar=%08X chain=%X/%X pos=%.2f,%.2f,%.2f",
         item.ambient, GuestString(base, source.name), owner, entity, car, first, second,
         position[0], position[1], position[2]);
    if (g_built_count < g_built_cars.size()) g_built_cars[g_built_count] = {car, source.name};
    LOGE("MC3TRAFFIC rail collider removed=%u ambient=%08X state=%u lane=%08X",
         unsigned(removed), item.ambient, U32(base, item.ambient + 0xc4),
         U32(base, item.ambient + 0xfc));
    return true;
}

void DestroyCompleteVehicle(AmbientBridge& item, X86Context& ctx, uint8_t* base) {
    // Ordinary racers are removed from phLevel and the update/culling lists
    // before deletion. Extra owners do not participate in that racer loop.
    // mcCar's destructor alone frees components but does not unregister the
    // live vehCar phInst. Use the same native removal as normal mcCar owners.
    if (Guest(item.entity, 0x64) && base[item.entity + 0x60] == 0 &&
        Guest(item.car, 0x40) &&
        *reinterpret_cast<const uint16_t*>(base + item.car + 8) != 0xffff) {
        const uint32_t id = *reinterpret_cast<const uint16_t*>(base + item.car + 8);
        GuestCall(ctx, base, sub_002BF2A0, item.owner + 0xc, {});
        LOGE("MC3TRAFFIC removed full-vp owner=%08X entity=%08X ph_id=%u now=%u",
             item.owner, item.entity, id,
             unsigned(*reinterpret_cast<const uint16_t*>(base + item.car + 8)));
    }
    GuestCall(ctx, base, sub_001EC430, item.owner, {});
}

void LogAmbient(const char* source, X86Context& ctx, uint8_t* base,
                uint32_t arg_bias) {
    static uint32_t samples = 0;
    if (samples >= 48) return;

    const uint32_t ambient = ctx.ecx;
    const uint32_t type = U32(base, ctx.esp + arg_bias);
    const uint32_t pool = U32(base, ctx.esp + arg_bias + 4);
    const uint32_t index = U32(base, ctx.esp + arg_bias + 8);
    if (!Guest(ambient, 0x21c)) {
        LOGE("MC3TRAFFIC ambient %s rejected object=%08X esp=%08X type=%08X pool=%08X index=%u",
             source, ambient, ctx.esp, type, pool, index);
        ++samples;
        return;
    }

    const uint32_t model_array = U32(base, type + 0x674);
    uint32_t first = 0, second = 0;
    const uint32_t car = FindCar(base, ambient, 0x21c, first, second);
    LOGE("MC3TRAFFIC map ambient source=%s index=%u object=%08X type=%08X type_kind=%u "
         "model_array=%08X vehCar=%08X chain=%X/%X pool=%08X",
         source, index, ambient, type, U32(base, type + 0x294), model_array,
         car, first, second, pool);
    ++samples;
}

}  // namespace

void Mc3EnhancedTrafficResourcePathProbe(X86Context& ctx, uint8_t* base) {
    LogTrafficCarResourcePath(ctx, base);
    // Opt-in, one-shot diagnostic: ask the *same* native file manager used by
    // 0x2126C0 to open two opponent resources during ordinary Arcade Cruise.
    // This distinguishes stream lookup from later xbck type/data parsing.
    static const bool raw_open = Mc3DebugHasArg(L"--mc3-traffic-raw-open-probe");
    static bool sampled = false;
    if (!raw_open || sampled || !base || g_constructing || !Guest(ctx.edx, 128)) return;
    char* path = reinterpret_cast<char*>(base + ctx.edx);
    if (!std::strstr(path, "/vp_jetta_03/vp_jetta_03") ||
        std::strstr(path, "_o")) return;
    sampled = true;
    std::array<char, 128> original{};
    std::memcpy(original.data(), path, original.size());
    const uint32_t manager = *reinterpret_cast<const uint32_t*>(base + 0x5348b0);
    const uint32_t context = *reinterpret_cast<const uint32_t*>(base + 0x4b2dd0);
    for (const char* candidate : {
             "$/resources/vehicle/vp_jetta_03/vp_jetta_03_o",
             "$/resources/vehicle/vp_d_scion_tc_05/vp_d_scion_tc_05_o"}) {
        std::memset(path, 0, 128);
        std::memcpy(path, candidate, std::strlen(candidate) + 1);
        const uint32_t stream = GuestCall(ctx, base, sub_0020D6F0, manager,
                                          {ctx.edx, context, 0, 1});
        LOGE("MC3TRAFFIC raw open candidate='%s' stream=%08X manager=%08X context=%08X",
             candidate, stream, manager, context);
        if (stream) GuestCall(ctx, base, sub_001A2630, stream, {});
    }
    // The previous synthetic aiOpponent failed after Open. Probe the typed
    // loader separately while retaining the ordinary player path for return.
    constexpr const char* scion =
        "$/resources/vehicle/vp_d_scion_tc_05/vp_d_scion_tc_05_o";
    std::memset(path, 0, 128);
    std::memcpy(path, scion, std::strlen(scion) + 1);
    const uint32_t out_a = ctx.edx + 0x70, out_b = ctx.edx + 0x74;
    *reinterpret_cast<uint32_t*>(base + out_a) = 0;
    *reinterpret_cast<uint32_t*>(base + out_b) = 0;
    const uint32_t resource = GuestCall(ctx, base, sub_002105F0, 0,
                                        {ctx.edx, 0x38d030, out_a, out_b});
    LOGE("MC3TRAFFIC typed load candidate='%s' resource=%08X header_a=%08X header_b=%08X",
         scion, resource, U32(base, out_a), U32(base, out_b));
    if (resource) GuestCall(ctx, base, sub_0020DB50, 0, {resource});
    std::memcpy(path, original.data(), original.size());
}

// 0x2126E8 follows the asset manager's virtual Open call in 0x2126C0.
// Observe its raw result before resource type validation or parsing can
// obscure whether the requested vehicle path was found at all.
void Mc3EnhancedTrafficResourceOpenProbe(X86Context& ctx, uint8_t* base) {
    static const bool enabled = Mc3DebugHasArg(L"--mc3-traffic-resource-probe");
    static uint32_t samples = 0;
    if (!enabled || !base || samples >= 24 || U32(base, ctx.ebp + 12) != 0x38d030) return;
    const uint32_t path_va = U32(base, ctx.ebp + 8);
    const char* path = GuestString(base, path_va);
    if (!std::strstr(path, "/vehicle/")) return;
    // These two globals and the vtable live in the XBE's low static image;
    // U32 intentionally accepts only heap/system/stack guest addresses.
    const uint32_t manager = *reinterpret_cast<const uint32_t*>(base + 0x5348b0);
    const uint32_t vtable = manager ? *reinterpret_cast<const uint32_t*>(base + manager) : 0;
    const uint32_t open_fn = vtable ? *reinterpret_cast<const uint32_t*>(base + vtable + 0x24) : 0;
    const uint32_t context = *reinterpret_cast<const uint32_t*>(base + 0x4b2dd0);
    LOGE("MC3TRAFFIC resource open path='%s' result=%08X type=%08X manager=%08X "
         "vtable=%08X open=%08X context=%08X caller=%08X constructing=%u",
         path, ctx.eax, U32(base, ctx.ebp + 12), manager, vtable,
         open_fn, context,
         U32(base, ctx.ebp + 4), unsigned(g_constructing));
    ++samples;
}

void Mc3EnhancedTrafficResourceHeaderProbe(X86Context& ctx, uint8_t* base) {
    static const bool enabled = Mc3DebugHasArg(L"--mc3-traffic-resource-probe");
    static uint32_t samples = 0;
    if (!enabled || !base || samples >= 24 || U32(base, ctx.ebp + 12) != 0x38d030) return;
    const char* path = GuestString(base, U32(base, ctx.ebp + 8));
    if (!std::strstr(path, "/vehicle/")) return;
    LOGE("MC3TRAFFIC resource header path='%s' read=%08X words=%08X/%08X/%08X/%08X expected=%08X",
         path, ctx.eax, U32(base, ctx.esp + 8), U32(base, ctx.esp + 12),
         U32(base, ctx.esp + 16), U32(base, ctx.esp + 20), U32(base, ctx.ebp + 12));
    ++samples;
}

// Immediately before rmcShaderFactoryStandard's Invalid type assertion.
// The native value is in EAX; record the pointed shader and relocation base
// without suppressing the original failure.
void Mc3EnhancedTrafficShaderTypeProbe(X86Context& ctx, uint8_t* base) {
    static const bool enabled = Mc3DebugHasArg(L"--mc3-traffic-resource-probe");
    if (!enabled || !base) return;
    LOGE("MC3TRAFFIC shader invalid type=%u shader=%08X word=%08X reloc=%08X "
         "index=%u table=%08X resource_base=%08X constructing=%u",
         ctx.eax, ctx.ecx, U32(base, ctx.ecx + 4), ctx.ebx, ctx.esi,
         ctx.edi, U32(base, ctx.ebx + 4), unsigned(g_constructing));
}

// Read the shader table entry before ResourcePageIn applies its relocation.
// A bad entry here identifies a resource/fixup problem rather than a shader
// implementation gap.
void Mc3EnhancedTrafficShaderEntryProbe(X86Context& ctx, uint8_t* base) {
    static const bool enabled = Mc3DebugHasArg(L"--mc3-traffic-resource-probe");
    static unsigned samples = 0;
    if (!enabled || !base || samples >= 96 || !Guest(ctx.edi + ctx.esi * 4)) return;
    const uint32_t raw = U32(base, ctx.edi + ctx.esi * 4);
    const uint32_t resource_base = U32(base, ctx.ebx + 4);
    LOGE("MC3TRAFFIC shader entry raw=%08X base=%08X computed=%08X index=%u table=%08X constructing=%u",
         raw, resource_base, raw + resource_base, ctx.esi, ctx.edi,
         unsigned(g_constructing));
    ++samples;
}

void Mc3EnhancedTrafficCarPageInProbe(X86Context& ctx, uint8_t* base) {
    static const bool enabled = Mc3DebugHasArg(L"--mc3-traffic-resource-probe");
    static unsigned samples = 0;
    if (!enabled || !base || samples++ >= 24) return;
    const uint32_t body = U32(base, ctx.esp);
    LOGE("MC3TRAFFIC car page-in body=%08X delta=%08X len=%08X first=%08X/%08X/%08X/%08X constructing=%u",
         body, U32(base, ctx.esp + 4), U32(base, ctx.esp + 8),
         U32(base, body), U32(base, body + 4), U32(base, body + 8),
         U32(base, body + 12), unsigned(g_constructing));
}

// Entry to the game's 16-slot asynchronous car-resource cache. Capture the
// six native flags as well as the cache owner so Race and Cruise can be
// compared without inferring them from the selected suffix.
void Mc3EnhancedTrafficPreloadEntryProbe(X86Context& ctx, uint8_t* base) {
    static const bool enabled = Mc3DebugHasArg(L"--mc3-traffic-resource-probe");
    static unsigned samples = 0;
    if (!enabled || !base || samples >= 64 || !Guest(ctx.esp + 4, 24)) return;
    const uint32_t name_va = U32(base, ctx.esp + 4);
    const char* name = GuestString(base, name_va);
    if (!std::strstr(name, "vp_")) return;
    LOGE("MC3TRAFFIC preload entry cache=%08X name='%s' args=%08X/%08X/%08X/%08X/%08X "
         "tick=%llu manager=%08X constructing=%u",
         ctx.ecx, name, U32(base, ctx.esp + 8), U32(base, ctx.esp + 12),
         U32(base, ctx.esp + 16), U32(base, ctx.esp + 20), U32(base, ctx.esp + 24),
         static_cast<unsigned long long>(g_tick),
         *reinterpret_cast<const uint32_t*>(base + 0x534074), unsigned(g_constructing));
    ++samples;
}

// Entry to the synchronous 16-slot car-resource cache used when an actual
// vehCar is constructed. Police, opponents and player cars all reach this
// route; the frontend-only preloader at 0x105CA0 does not replace it.
void Mc3EnhancedTrafficResourceAcquireProbe(X86Context& ctx, uint8_t* base) {
    static const bool enabled = Mc3DebugHasArg(L"--mc3-traffic-resource-probe");
    static const bool cruise_pilot = Mc3DebugHasArg(L"--mc3-traffic-cruise-pilot");
    static unsigned samples = 0;
    if (!base || !Guest(ctx.esp + 4, 24)) return;
    const uint32_t name_va = U32(base, ctx.esp + 4);
    const char* name = GuestString(base, name_va);
    if (!std::strstr(name, "vp_")) return;
    // In Cruise the empty aiOpponentManager already exists by the time the
    // city player car is acquired.  Frontend preview cars also use this cache
    // while state 7 is reported, so the game state alone is insufficient.
    // Requiring a live zero-count/zero-array manager identifies the actual
    // city-load window without depending on a compiler-sensitive stack flag.
    const uint32_t state_object = *reinterpret_cast<const uint32_t*>(base + 0x557ed0);
    const uint32_t state = Guest(state_object, 8) ? U32(base, state_object + 4) : UINT32_MAX;
    const uint32_t opponent_manager = *reinterpret_cast<const uint32_t*>(base + 0x534074);
    if (cruise_pilot && !g_cruise_pilot_active && state == 7 &&
        Guest(opponent_manager, 12)) {
        g_cruise_load_window_seen = true;
        // This cache entry is the one checkpoint confirmed on every decode.
        // Native races finish opponent construction before loading the player;
        // do the same here, before the current base-resource acquisition begins.
        if (!g_constructing)
            Mc3EnhancedTrafficCruiseLoadCheckpoint(ctx, base);
    }
    if (!enabled || samples >= 96) return;
    LOGE("MC3TRAFFIC acquire entry cache=%08X name='%s' args=%08X/%08X/%08X/%08X/%08X "
         "tick=%llu opponent_manager=%08X count=%u array=%08X state=%u constructing=%u",
         ctx.ecx, name, U32(base, ctx.esp + 8), U32(base, ctx.esp + 12),
         U32(base, ctx.esp + 16), U32(base, ctx.esp + 20), U32(base, ctx.esp + 24),
         static_cast<unsigned long long>(g_tick), opponent_manager,
         U32(base, opponent_manager), U32(base, opponent_manager + 8), state,
         unsigned(g_constructing));
    ++samples;
}

void Mc3EnhancedTrafficPreloadResultProbe(X86Context& ctx, uint8_t* base) {
    static const bool enabled = Mc3DebugHasArg(L"--mc3-traffic-resource-probe");
    static unsigned samples = 0;
    if (!enabled || !base || samples >= 64) return;
    const uint32_t entry = ctx.esi;
    const uint32_t cache = ctx.edi;
    const uint32_t delta = entry >= cache ? entry - cache : UINT32_MAX;
    const int slot = delta < 0x100 && !(delta & 15) ? int(delta / 16) : -1;
    LOGE("MC3TRAFFIC preload registration result=%08X cache=%08X entry=%08X slot=%d "
         "key=%08X ptr=%08X flags=%02X/%02X/%02X handle=%08X constructing=%u",
         ctx.eax, cache, entry, slot, U32(base, entry), U32(base, entry + 4),
         Guest(entry + 8, 3) ? base[entry + 8] : 0,
         Guest(entry + 8, 3) ? base[entry + 9] : 0,
         Guest(entry + 8, 3) ? base[entry + 10] : 0,
         U32(base, entry + 12), unsigned(g_constructing));
    ++samples;
}

void Mc3EnhancedTrafficAsyncTypeProbe(X86Context& ctx, uint8_t* base) {
    static const bool enabled = Mc3DebugHasArg(L"--mc3-traffic-resource-probe");
    static const bool pilot = Mc3DebugHasArg(L"--mc3-traffic-cruise-pilot");
    static unsigned samples = 0;
    if (!base) return;
    const char* path = Guest(ctx.esp + 8, 32)
        ? reinterpret_cast<const char*>(base + ctx.esp + 8) : "?";
    if (pilot && Enabled() && !g_cruise_city_load_marked &&
        std::strstr(path, "loadscreen_arcade_")) {
        // The frontend has its own moving ambient traffic and reaches the same
        // spawn hooks. The arcade loadscreen is the stable boundary before the
        // actual city respawns, so discard every frontend observation here.
        g_ambient = {};
        g_cruise_respawn_candidates = {};
        g_cruise_city_ambient_reset = false;
        g_cruise_city_load_marked = true;
        LOGE("MC3TRAFFIC cruise city load boundary path='%s'", path);
    }
    if (!enabled || samples++ >= 64) return;
    LOGE("MC3TRAFFIC async type lookup result=%08X handle=%08X buffer=%08X table_count=%08X "
         "table=%08X path='%s' manager=%08X constructing=%u",
         ctx.eax, ctx.esi, ctx.edi,
         *reinterpret_cast<const uint32_t*>(base + 0x53492c),
         *reinterpret_cast<const uint32_t*>(base + 0x534928),
         path,
         *reinterpret_cast<const uint32_t*>(base + 0x534074), unsigned(g_constructing));
}

// vehAutoMgr allocates each entity/component registry from this constructor
// argument. Its AddEntry (0x35D500) has no capacity check; the original 10
// entries overflow when the extra cars contribute their component arrays.
// Resize at the original allocation/lifetime boundary, before any car exists.
void Mc3EnhancedTrafficVehicleManagerInit(X86Context& ctx, uint8_t* base) {
    if (!Enabled() || !base || !Guest(ctx.esp + 4)) return;
    const uint32_t original = U32(base, ctx.esp + 4);
    // one entry per car: room for the whole Cruise pool beside the race, the player and the police
    if (original < 512) {
        *reinterpret_cast<uint32_t*>(base + ctx.esp + 4) = 512;
        LOGE("MC3TRAFFIC vehAutoMgr component capacity %u -> 512", original);
    }
}

// --mc3-traffic-density-scale=X multiplies the traffic density each ambient traffic source gets. 0x2B9670 stores it at
// source+0x20 and 0x2B93C0 turns density x the source's road lengths into its target car count at +0x28, so the count
// scales with X. The race's RaceDensity is a percentage (0x26D480: 0..100 -> x0.01): San Diego's Cruise sets 2 % and
// runs 18 moving cars. Off unless asked for, in any profile
X86_FUNC(sub_002B9670) {
    static const float scale = [] {
        const wchar_t* value = Mc3DebugArg(L"--mc3-traffic-density-scale");
        const float parsed = value && *value ? float(std::wcstod(value, nullptr)) : 1.0f;
        return std::isfinite(parsed) && parsed > 0.0f ? parsed : 1.0f;
    }();
    if (scale != 1.0f) {
        float density;
        std::memcpy(&density, base + ctx.esp + 4, sizeof(density));
        const float scaled = density * scale;
        std::memcpy(base + ctx.esp + 4, &scaled, sizeof(scaled));
        static uint32_t logged = 0;
        if (logged++ < 8) LOGE("MC3TRAFFIC density source=%08X %.4f x %.2f = %.4f", ctx.ecx, density, scale, scaled);
    }
    __imp__sub_002B9670(ctx, base);
}

// Runs at aiAmbientVehicle::Init entry.  Initial city population reaches this
// routine through 0x6C840, while runtime respawns reach it through 0x6D7D0;
// hooking the shared entry observes both paths.  ECX is the actual 0x21C-byte
// ambient object and the stack still contains (type, pool, index).
void Mc3EnhancedTrafficAmbientSpawn(X86Context& ctx, uint8_t* base) {
    if (!Enabled() || !base) return;
    LogAmbient("entry", ctx, base, 4);
}

// The recompiler can bind direct calls to the weak entry wrapper before a
// hook is regenerated.  Probe both concrete call sites as well: the initial
// population in aiAmbientVehicleInstances::Init and later runtime respawns.
// At these points the three arguments have been pushed but the return address
// has not, hence arg_bias=0.
void Mc3EnhancedTrafficAmbientInitial(X86Context& ctx, uint8_t* base) {
    if (!Enabled() || !base) return;
    LogAmbient("initial", ctx, base, 0);
}

// Immediately after aiAmbientVehicle::Init returns during the initial city
// population.  The entry probes above are useful for arguments, but the lane
// fields are not complete until the native initializer has returned.  Keep a
// reference to the settled rail object so a Cruise vehicle can be constructed
// in the same state-7 load phase as the player and native race opponents.
void Mc3EnhancedTrafficAmbientInitialReady(X86Context& ctx, uint8_t* base) {
    if (!Enabled() || !base || !Guest(ctx.esi, 0x134)) return;
    const uint32_t objects = U32(base, ctx.esi + 0x130);
    const uint32_t ambient = objects + ctx.ebx;
    if (!Guest(ambient, 0x21c)) return;
    ObserveAmbient(ambient, base);
}

void Mc3EnhancedTrafficAmbientRespawn(X86Context& ctx, uint8_t* base) {
    if (!Enabled() || !base) return;
    static const bool pilot = Mc3DebugHasArg(L"--mc3-traffic-cruise-pilot");
    const uint32_t opponent_manager = *reinterpret_cast<const uint32_t*>(base + 0x534074);
    if (pilot && !g_cruise_city_ambient_reset && Guest(opponent_manager, 12)) {
        // This pre-call hook is present even when the recompiler bypasses the
        // post-call decode. Start the city epoch here, then retain the object
        // address; its synchronous Init has completed by the player checkpoint.
        g_ambient = {};
        g_cruise_city_ambient_reset = true;
        LOGE("MC3TRAFFIC cruise city ambient epoch manager=%08X", opponent_manager);
    }
    LogAmbient("respawn", ctx, base, 0);
    if (pilot && Guest(ctx.ecx, 0x21c) &&
        std::find(g_cruise_respawn_candidates.begin(),
                  g_cruise_respawn_candidates.end(), ctx.ecx) ==
            g_cruise_respawn_candidates.end()) {
        const auto free_slot = std::find(g_cruise_respawn_candidates.begin(),
                                         g_cruise_respawn_candidates.end(), 0);
        if (free_slot != g_cruise_respawn_candidates.end()) *free_slot = ctx.ecx;
    }
    if (g_cruise_city_ambient_reset && Guest(ctx.ecx, 0x21c))
        ObserveAmbient(ctx.ecx, base);
}

void Mc3EnhancedTrafficAmbientRespawnReady(X86Context& ctx, uint8_t* base) {
    if (!Enabled() || !base || !Guest(ctx.edi, 0x10)) return;
    static const bool pilot = Mc3DebugHasArg(L"--mc3-traffic-cruise-pilot");
    const uint32_t opponent_manager = *reinterpret_cast<const uint32_t*>(base + 0x534074);
    if (pilot && !g_cruise_city_ambient_reset && Guest(opponent_manager, 12)) {
        // Title/profile cars share the ambient renderer and can have the same
        // main-loop tick as the synchronous city load. Drop those stale
        // observations when the real Cruise manager and population coexist.
        g_ambient = {};
        g_cruise_city_ambient_reset = true;
        LOGE("MC3TRAFFIC cruise city ambient epoch manager=%08X", opponent_manager);
    }
    const uint32_t pool = U32(base, ctx.edi + 0x0c);
    const uint32_t objects = U32(base, pool);
    const uint32_t ambient = objects + ctx.esi;
    if (!Guest(ambient, 0x21c)) return;
    ObserveAmbient(ambient, base);
}

// aiAmbientVehicle::Render is entered through its rmcRenderable subobject,
// located at ambient+0x10.  Unlike Init, this route is also used by ambient
// instances restored directly from the serialized traffic resource.
void Mc3EnhancedTrafficAmbientRender(X86Context& ctx, uint8_t* base) {
    if (!Enabled() || !base || ctx.ecx < 0x10) return;
    static uint32_t samples = 0;

    const uint32_t ambient = ctx.ecx - 0x10;
    if (!Guest(ambient, 0x21c)) return;
    AmbientBridge* bridge = ObserveAmbient(ambient, base);
    if (bridge && bridge->car && Guest(bridge->car + 0x10, 0x30)) {

        // The full vp actor replaces only the stock ambient representation;
        // its rail controller remains alive and continues supplying the route.
        *reinterpret_cast<uint32_t*>(base + ctx.esp + 4) = 0xffffffffu;
    }
    if (samples < 48) {
        const uint32_t type = U32(base, ambient + 0x138);
        const uint32_t pass = U32(base, ctx.esp + 4);
        LOGE("MC3TRAFFIC live ambient pass=%u object=%08X renderable=%08X type=%08X "
             "type_kind=%u matrix=%08X rail=%08X state=%u",
             pass, ambient, ctx.ecx, type, U32(base, type + 0x294), ambient + 0x34,
             U32(base, ambient + 0x108), U32(base, ambient + 0xc4));
        ++samples;
    }
}

// A detached phInst is absent from the physics culling list. Update its lane
// oracle explicitly from the main loop, and suppress a second native update
// during that same tick. The physical replacement still uses ordinary mcCar
// updates; this hook never writes its transform or controls.
bool Mc3EnhancedTrafficAmbientUpdate(X86Context& ctx, uint8_t* base) {
    if (!Enabled() || !base) return false;
    for (auto& item : g_ambient) {
        if (item.ambient != ctx.ecx || !item.owner) continue;
        if (item.last_route_update == g_tick) return true;
        item.last_route_update = g_tick;
        break;
    }
    return false;
}

// The native traffic director retires/reuses ambient slots as the player
// moves. Destroy the complete actor through its original owner destructor
// before that oracle loses its lane pointers. A later spawn may reuse the
// now-free reserved owner slot without changing the race opponent count.
void Mc3EnhancedTrafficAmbientDeactivate(X86Context& ctx, uint8_t* base) {
    if (!Enabled() || !base) return;
    if (!Guest(ctx.ecx, 0x21c) || U32(base, ctx.ecx + 0xc4) == 1 ||
        base[ctx.ecx + 0xad] != 0 || U32(base, ctx.ecx + 0xa8) != 0) return;
    for (auto& item : g_ambient) {
        if (item.ambient != ctx.ecx || !item.owner) continue;
        const uint32_t owner = item.owner;
        DestroyCompleteVehicle(item, ctx, base);
        LOGE("MC3TRAFFIC retired owner=%08X ambient=%08X", owner, item.ambient);
        item = {};
        if (g_built_count) --g_built_count;
        // The independent Cruise pilot stops constructing after reaching its
        // requested count. A native lane retirement must reopen one slot so
        // the following respawn can reuse the released owner.
        if (g_cruise_pilot_active && g_template_count)
            g_manager_ready = true;
        break;
    }
}

// Preserve the exact four arguments used by the game's own aiOpponent::Init.
// The later factory hook has the vehicle/config pointers but its large stack
// frame no longer exposes the manager context passed as argument four.
void Mc3EnhancedTrafficOpponentInitEntry(X86Context& ctx, uint8_t* base) {
    if (!Enabled() || !base || g_constructing) return;
    InitCapture* slot = nullptr;
    for (auto& capture : g_init_captures) {
        if (capture.opponent == ctx.ecx) return;
        if (!capture.opponent && !slot) slot = &capture;
    }
    if (!slot) return;
    *slot = {ctx.ecx, U32(base, ctx.esp + 4), U32(base, ctx.esp + 8),
             U32(base, ctx.esp + 12), U32(base, ctx.esp + 16)};
    LOGE("MC3TRAFFIC init capture owner=%08X slot=%u start=%08X arg2=%08X arg3=%08X",
         slot->opponent, slot->arg0, slot->start, slot->arg2, slot->arg3);
}

// Runs after aiOpponentFactory::Construct returns inside aiOpponent::Init.
// EAX is the complete constructed vehicle, ESI is aiOpponent and EDI remains
// aiOpponentStart.  This captures the exact pointer chain shared by player
// opponents and the future enhanced traffic instances.
void Mc3EnhancedTrafficOpponentBuilt(X86Context& ctx, uint8_t* base) {
    if (!Enabled() || !base || g_constructing) return;
    static uint32_t samples = 0;

    const uint32_t opponent = ctx.esi;
    const uint32_t entity = ctx.eax;
    const uint32_t start = ctx.edi;
    const uint32_t config = U32(base, start + 0x64);
    const uint32_t name_va = config ? config + 0xe3 : 0;
    const InitCapture* init = nullptr;
    for (const auto& capture : g_init_captures)
        if (capture.opponent == opponent) {
            init = &capture;
            break;
        }
    if (Guest(config, 0x100) && g_template_count < g_templates.size()) {
        bool duplicate = false;
        for (uint32_t i = 0; i < g_template_count; ++i)
            duplicate |= g_templates[i].config == config;
        if (!duplicate && init)
            g_templates[g_template_count++] = {name_va, config, opponent, ctx.ebx,
                                                init->start, init->arg2,
                                                init->arg3};
    }
    // Limit diagnostics, never template capture: later city/race loads must
    // still populate their own pool after the first 24 source opponents.
    if (samples >= 24) return;
    uint32_t of1 = 0, of2 = 0, ef1 = 0, ef2 = 0;
    const uint32_t opponent_car = FindCar(base, opponent, 0x43c, of1, of2);
    const uint32_t entity_car = FindCar(base, entity, 0x400, ef1, ef2);
    LOGE("MC3TRAFFIC map opponent slot=%u opponent=%08X start=%08X config=%08X vehicle='%s' "
         "entity=%08X car_from_opponent=%08X chain=%X/%X car_from_entity=%08X chain=%X/%X",
         ctx.ebx, opponent, start, config, GuestString(base, name_va), entity,
         opponent_car, of1, of2, entity_car, ef1, ef2);
    ++samples;
}

// End of aiOpponentManager::Init, after every regular opponent and its
// complete vehicle stack has finished registration.  Reusing that settled
// boundary avoids mutating renderer/physics lists from aiAmbientVehicle::Render.
void Mc3EnhancedTrafficOpponentManagerReady(X86Context& ctx, uint8_t* base) {
    if (!Enabled() || !base || !g_template_count) return;
    g_ambient = {};
    g_manager_ready = true;
    g_monitor_physics = true;
    LOGE("MC3TRAFFIC full-vp manager bridge ready built=%u ambient_seen=%u templates=%u",
         g_built_count,
         unsigned(std::count_if(g_ambient.begin(), g_ambient.end(),
                                [](const auto& item) { return item.ambient != 0; })),
         g_template_count);
}

// Increase only the backing allocation. The manager's race count and the
// vector-destructor header retain the real race opponents, so extra traffic
// slots have unique collision identities without entering race standings.
void Mc3EnhancedTrafficReserveSlots(X86Context& ctx, uint8_t* base) {
    if (!Enabled() || !base || ctx.esi > 32) return;
    g_manager = ctx.edi;
    g_regular_count = ctx.esi;
    ctx.eax += kTrafficCapacity * kOpponentSize;
    g_extra_capacity = kTrafficCapacity;
    g_built_count = g_template_count = 0;
    g_bounded_index_writes = 0;
    g_manager_ready = false;
    g_ambient = {};
    g_templates = {};
    g_init_captures = {};
    g_detached_ambients = {};
    LOGE("MC3TRAFFIC reserved %u extra owner slots manager=%08X race_count=%u",
         kTrafficCapacity, g_manager, g_regular_count);
}

// The index-th car the Cruise pool built and still drives (its vehCar, whose Matrix34 is at +0x10), or 0
uint32_t Mc3TrafficBuiltCar(uint32_t index, uint32_t* name) {
    if (index >= g_built_count || index >= g_built_cars.size()) return 0;
    const uint32_t car = g_built_cars[index][0];
    for (const auto& item : g_ambient)
        if (item.owner && item.car == car) {
            if (name) *name = g_built_cars[index][1];
            return car;
        }
    return 0;
}

uint32_t Mc3TrafficBuiltCount() { return g_built_count; }

// Called before either manager cleanup path frees the original owner array.
// Extra owners were placement-constructed beyond the ordinary vector header.
void Mc3EnhancedTrafficReleaseSlots(X86Context& ctx, uint8_t* base) {
    if (!base || !g_manager || (ctx.ecx != g_manager && ctx.edi != g_manager)) return;
    for (auto& item : g_ambient) {
        if (!item.owner) continue;
        CheckTrafficHeap(ctx, base, "before-destroy");
        CheckTrafficPhysics(base, "before-destroy");
        LogTrafficTypes(base, "before-destroy");
        DestroyCompleteVehicle(item, ctx, base);
        CheckTrafficHeap(ctx, base, "after-destroy");
        CheckTrafficPhysics(base, "after-destroy");
        LogTrafficTypes(base, "after-destroy");
        item = {};
    }
    LOGE("MC3TRAFFIC released %u extra owners manager=%08X", g_built_count,
         g_manager);
    if (g_cruise_pilot_array) {
        // A zero-count Cruise manager does not own or delete this backing
        // array. Retire every complete car first, then clear its pointer.
        *reinterpret_cast<uint32_t*>(base + g_manager + 8) = 0;
        mem::FreeSystem(g_cruise_pilot_array);
        mem::FreeSystem(g_cruise_pilot_config);
        mem::FreeSystem(g_cruise_pilot_start);
        mem::FreeSystem(g_cruise_pilot_name);
        g_cruise_pilot_array = g_cruise_pilot_config =
            g_cruise_pilot_start = g_cruise_pilot_name = 0;
        g_cruise_pilot_active = false;
        g_cruise_pilot_attempted = false;
        g_cruise_pilot_preloaded = false;
        g_cruise_load_window_seen = false;
        g_cruise_city_ambient_reset = false;
        g_cruise_city_load_marked = false;
        g_cruise_respawn_candidates = {};
        g_cruise_preload_polls = 0;
        g_cruise_pool_full_tick = 0;
        g_cruise_recycle_test_fired = false;
    }
    g_manager = 0;
    g_built_count = g_template_count = 0;
    g_manager_ready = false;
}

// Controlled construction pilot for MC3-0015. It follows native
// config/start constructors and uses separately owned manager storage while
// leaving the ordinary Cruise race count at zero. Never runs without its
// explicit command-line flag; the experiment may still expose a native assert.
void TryCruisePilot(X86Context& ctx, uint8_t* base) {
    static const bool enabled = Mc3DebugHasArg(L"--mc3-traffic-cruise-pilot");
    if (!enabled || g_cruise_pilot_attempted || g_manager || !base) return;
    const uint32_t manager = *reinterpret_cast<const uint32_t*>(base + 0x534074);
    if (!Guest(manager, 12)) return;
    const uint32_t count = U32(base, manager);
    const uint32_t existing = U32(base, manager + 8);
    if (count || existing) return;
    g_cruise_pilot_attempted = true;
    LOGE("MC3TRAFFIC cruise pilot manager=%08X count=%u array=%08X",
         manager, count, existing);
    g_cruise_pilot_array = mem::AllocSystem(kCruiseCapacity * kOpponentSize, 16);
    g_cruise_pilot_config = mem::AllocSystem(kCruiseCapacity * 0x108, 16);
    g_cruise_pilot_start = mem::AllocSystem(kCruiseCapacity * 0x88, 16);
    g_cruise_pilot_name = mem::AllocSystem(kCruiseCapacity * 64, 16);
    if (!g_cruise_pilot_array || !g_cruise_pilot_config ||
        !g_cruise_pilot_start || !g_cruise_pilot_name) {
        LOGE("MC3TRAFFIC cruise pilot allocation failed");
        if (g_cruise_pilot_array) mem::FreeSystem(g_cruise_pilot_array);
        if (g_cruise_pilot_config) mem::FreeSystem(g_cruise_pilot_config);
        if (g_cruise_pilot_start) mem::FreeSystem(g_cruise_pilot_start);
        if (g_cruise_pilot_name) mem::FreeSystem(g_cruise_pilot_name);
        g_cruise_pilot_array = g_cruise_pilot_config =
            g_cruise_pilot_start = g_cruise_pilot_name = 0;
        return;
    }
    std::memset(base + g_cruise_pilot_array, 0, kCruiseCapacity * kOpponentSize);
    std::memset(base + g_cruise_pilot_config, 0, kCruiseCapacity * 0x108);
    std::memset(base + g_cruise_pilot_start, 0, kCruiseCapacity * 0x88);
    std::memset(base + g_cruise_pilot_name, 0, kCruiseCapacity * 64);
    g_extra_capacity = kCruiseCapacity;
    const char* vehicle = "vp_g500_03";
    const char* profile = "g500_03";
    bool use_native_type_defaults = false;
    if (const wchar_t* requested = Mc3DebugArg(L"--mc3-traffic-cruise-model")) {
        if (!std::wcscmp(requested, L"vp_jetta_03")) {
            vehicle = "vp_jetta_03";
            profile = "jetta_03";
        } else if (!std::wcscmp(requested, L"vp_d_scion_tc_05")) {
            vehicle = "vp_d_scion_tc_05";
            profile = "d_scion_tc_05";
        } else if (!std::wcscmp(requested, L"vp_impalass_cop_s_96")) {
            vehicle = "vp_impalass_cop_s_96";
            profile = "impalass_cop_s_96";
            // aiCopMgr creates this config with mcCarConfig::SetVehicleType
            // only. Preserve the type's built-in profile instead of forcing
            // the ordinary opponent-profile setter onto the police branch.
            use_native_type_defaults = true;
        } else if (std::wcscmp(requested, L"vp_g500_03")) {
            LOGE("MC3TRAFFIC cruise pilot rejected model; allowed=vp_g500_03,vp_jetta_03,"
                 "vp_d_scion_tc_05,vp_impalass_cop_s_96");
        }
    }
    *reinterpret_cast<uint32_t*>(base + manager + 8) = g_cruise_pilot_array;
    g_manager = manager;
    g_regular_count = 0;
    g_templates = {};
    struct CruiseModel {
        const char* vehicle;
        const char* profile;
        bool native_defaults;
        uint32_t presets = 0;  // numbered .carcfg presets; with a pool, each car loads one at random
    };
    const CruiseModel selected{vehicle, profile, use_native_type_defaults};
    constexpr std::array<CruiseModel, 4> mixed_models{{
        {"vp_impalass_cop_s_96", "impalass_cop_s_96", true},
        {"vp_g500_03", "g500_03", false},
        {"vp_jetta_03", "jetta_03", false},
        {"vp_d_scion_tc_05", "d_scion_tc_05", false},
    }};
    const bool mixed = Mc3DebugHasArg(L"--mc3-traffic-cruise-mixed");
    const uint32_t target_count = CruisePilotCount();
    // --mc3-traffic-cruise-models=vp_a,vp_b,... takes any car of traffic_vp_pool.h, in turn;
    // --mc3-traffic-cruise-random[=seed] draws target_count distinct ordinary (non-police) cars from it. Either one
    // replaces the mixed list; the seed is logged so a run can be repeated
    std::vector<CruiseModel> pool_models;
    const auto pool_model = [](const TrafficVpModel& m) {
        return CruiseModel{m.vehicle, m.profile, m.native_defaults, m.presets};
    };
    if (const wchar_t* list = Mc3DebugArg(L"--mc3-traffic-cruise-models")) {
        std::wstring names(list);
        for (size_t at = 0; at <= names.size();) {
            size_t end = names.find(L',', at);
            if (end == std::wstring::npos) end = names.size();
            const std::wstring want = names.substr(at, end - at);
            at = end + 1;
            if (want.empty()) continue;
            const TrafficVpModel* found = nullptr;
            for (const TrafficVpModel& m : kTrafficVpPool) {
                if (std::equal(want.begin(), want.end(), m.vehicle, m.vehicle + std::strlen(m.vehicle))) found = &m;
            }
            if (found) pool_models.push_back(pool_model(*found));
            else LOGE("MC3TRAFFIC cruise pool rejected '%ls': not a car of traffic_vp_pool.h", want.c_str());
        }
    }
    // the draw's seed: the one given to --mc3-traffic-cruise-random, else the clock (a new draw at every load)
    const wchar_t* given_seed = Mc3DebugArg(L"--mc3-traffic-cruise-random");
    const uint32_t seed = given_seed && *given_seed
        ? uint32_t(std::wcstoul(given_seed, nullptr, 10))
        : uint32_t(std::chrono::steady_clock::now().time_since_epoch().count());
    std::mt19937 appearance(seed ^ 0x9E3779B9u);  // the presets' own stream: the same seed repeats both
    if (pool_models.empty() && Mc3DebugHasArg(L"--mc3-traffic-cruise-random")) {
        // the cars the probe sweep validated (vp_pool_sweep.py); all ordinary cars while none is
        const bool any_validated = std::any_of(std::begin(kTrafficVpPool), std::end(kTrafficVpPool),
                                               [](const TrafficVpModel& m) { return m.validated; });
        std::vector<const TrafficVpModel*> cars;
        for (const TrafficVpModel& m : kTrafficVpPool)
            if (!m.police && (m.validated || !any_validated)) cars.push_back(&m);
        std::shuffle(cars.begin(), cars.end(), std::mt19937(seed));
        // --mc3-traffic-cruise-random-models=K draws only K distinct models and repeats them over the cars: a repeated
        // model shares its loaded car type (refs > 1), so the heap holds many more cars (San Diego's ~41 MB heap overran
        // at the 15th distinct model); each repeat still gets its own random .carcfg
        uint32_t distinct = target_count;
        if (const wchar_t* k = Mc3DebugArg(L"--mc3-traffic-cruise-random-models"); k && *k)
            distinct = std::clamp(uint32_t(std::wcstoul(k, nullptr, 10)), 1u, target_count);
        for (uint32_t i = 0; i < distinct && i < cars.size(); ++i) pool_models.push_back(pool_model(*cars[i]));
        std::string picked;
        for (const CruiseModel& m : pool_models) picked += std::string(picked.empty() ? "" : ",") + m.vehicle;
        LOGE("MC3TRAFFIC cruise pool random seed=%u models=%s", seed, picked.c_str());
    }
    for (uint32_t i = 0; i < target_count; ++i) {
        const CruiseModel& model = !pool_models.empty() ? pool_models[i % pool_models.size()]
                                 : mixed ? mixed_models[i % mixed_models.size()] : selected;
        const uint32_t config = g_cruise_pilot_config + i * 0x108;
        const uint32_t start = g_cruise_pilot_start + i * 0x88;
        const uint32_t name = g_cruise_pilot_name + i * 64;
        std::memcpy(base + name, model.vehicle, std::strlen(model.vehicle) + 1);
        std::memcpy(base + name + 32, model.profile, std::strlen(model.profile) + 1);
        GuestCall(ctx, base, sub_00271D20, config, {});
        GuestCall(ctx, base, sub_00270AC0, config, {name});
        if (!model.native_defaults)
            GuestCall(ctx, base, sub_00270A90, config, {name + 32});
        // the car's look, as a race opponent gets it (aiOpponent setup 0x1CADB0 -> mcCarConfig::Load 0x2733B0): one of its
        // .carcfg, the plain vp_x or a numbered preset vp_x_N (paint, rims, kit, plate), picked at random; police keep
        // their type's own
        if (!pool_models.empty() && !model.native_defaults) {
            const uint32_t pick = uint32_t(appearance() % (model.presets + 1));
            char cfg[48];
            if (pick) std::snprintf(cfg, sizeof(cfg), "%s_%u", model.vehicle, pick);
            else std::snprintf(cfg, sizeof(cfg), "%s", model.vehicle);
            std::memcpy(base + name + 32, cfg, std::strlen(cfg) + 1);
            const bool loaded = (GuestCall(ctx, base, sub_002733B0, config, {name + 32}) & 0xFF) != 0;
            LOGE("MC3TRAFFIC cruise pilot carcfg template=%u '%s' %s", i, cfg, loaded ? "loaded" : "missing");
        }
        GuestCall(ctx, base, sub_001CA210, start, {});
        *reinterpret_cast<uint32_t*>(base + start + 0x64) = config;
        g_templates[i] = {config + 0xe3, config, 0, i, start, 0, 0};
        LOGE("MC3TRAFFIC cruise pilot template=%u config=%08X start=%08X name='%s' profile='%s'",
             i, config, start, GuestString(base, config + 0xe3),
             GuestString(base, config + 0xf9));
    }
    g_template_count = target_count;
    g_cruise_distinct_models = !pool_models.empty() ? uint32_t(pool_models.size())
                             : mixed ? uint32_t(mixed_models.size()) : 1u;
    g_manager_ready = true;
    g_cruise_pilot_active = true;
    g_cruise_pilot_preloaded = false;
    g_cruise_preload_polls = 0;
    g_cruise_pool_full_tick = 0;
    g_cruise_recycle_test_fired = false;
    LOGE("MC3TRAFFIC cruise pilot ready owner_array=%08X config=%08X start=%08X "
         "name='%s' profile='%s' target_count=%u",
         g_cruise_pilot_array, g_cruise_pilot_config, g_cruise_pilot_start,
         GuestString(base, g_cruise_pilot_config + 0xe3),
         GuestString(base, g_cruise_pilot_config + 0xf9), target_count);
}

// End of the concrete player-car construction path (0x2CBAE0), while the city
// loader still reports state 7.  Police and race opponents construct
// their complete vp_* actors in this phase.  Build the opt-in Cruise pilot at
// this settled checkpoint instead of waiting for the next main-loop tick,
// which already sees state 2 and produces invalid late resource fixups.
void Mc3EnhancedTrafficCruiseLoadCheckpoint(X86Context& ctx, uint8_t* base) {
    static const bool pilot = Mc3DebugHasArg(L"--mc3-traffic-cruise-pilot");
    if (!pilot || !Enabled() || !base) return;
    const uint32_t state_object = *reinterpret_cast<const uint32_t*>(base + 0x557ed0);
    const uint32_t state = Guest(state_object, 8) ? U32(base, state_object + 4) : UINT32_MAX;
    if (!g_cruise_city_ambient_reset) {
        g_ambient = {};
        for (const uint32_t ambient : g_cruise_respawn_candidates) {
            if (!ambient) break;
            if (Guest(ambient, 0x21c)) ObserveAmbient(ambient, base);
        }
        g_cruise_city_ambient_reset = true;
        LOGE("MC3TRAFFIC cruise city ambient epoch candidates=%u",
             unsigned(std::count_if(g_cruise_respawn_candidates.begin(),
                                    g_cruise_respawn_candidates.end(),
                                    [](uint32_t value) { return value != 0; })));
    }
    static unsigned samples = 0;
    if (samples < 16) {
        LOGE("MC3TRAFFIC player checkpoint state=%u tick=%llu armed=%u active=%u manager=%08X",
             state, static_cast<unsigned long long>(g_tick),
             unsigned(g_cruise_load_window_seen), unsigned(g_cruise_pilot_active),
             *reinterpret_cast<const uint32_t*>(base + 0x534074));
        ++samples;
    }
    if (!g_cruise_load_window_seen || g_cruise_pilot_active) return;
    LOGE("MC3TRAFFIC cruise load checkpoint state=%u tick=%llu ambients=%u manager=%08X",
         state, static_cast<unsigned long long>(g_tick),
         unsigned(std::count_if(g_ambient.begin(), g_ambient.end(),
                                [](const auto& item) { return item.ambient != 0; })),
         *reinterpret_cast<const uint32_t*>(base + 0x534074));
    if (state != 7) return;
    TryCruisePilot(ctx, base);
    if (!g_cruise_pilot_active || !g_manager_ready ||
        g_built_count >= CruisePilotCount()) return;
    for (auto& item : g_ambient) {
        if (g_built_count >= CruisePilotCount()) break;
        if (item.entity || !Guest(item.ambient, 0x21c) ||
            U32(base, item.ambient + 0xc4) == 1 ||
            !Guest(U32(base, item.ambient + 0xfc))) continue;
        LOGE("MC3TRAFFIC cruise load build state=%u ambient=%08X lane=%08X",
             state, item.ambient, U32(base, item.ambient + 0xfc));
        const uint32_t template_index = g_built_count % g_template_count;
        if (BuildCompleteVehicle(item, g_templates[template_index], ctx, base)) {
            ++g_built_count;
        }
    }
    if (g_built_count >= CruisePilotCount()) {
        g_manager_ready = false;
        return;
    }
    // Cruise initializes its ambient pool before assigning live lanes. The
    // complete vehicle can still be built now (matching race/police resource
    // timing); attachment to the lane is deferred until state 2.
    for (auto& item : g_ambient) {
        if (g_built_count >= CruisePilotCount()) break;
        if (item.entity || !Guest(item.ambient, 0x21c) ||
            g_tick - item.last_seen > 1) continue;
        LOGE("MC3TRAFFIC cruise load build waiting state=%u ambient=%08X route_state=%u lane=%08X",
             state, item.ambient, U32(base, item.ambient + 0xc4),
             U32(base, item.ambient + 0xfc));
        const uint32_t template_index = g_built_count % g_template_count;
        if (BuildCompleteVehicle(item, g_templates[template_index], ctx, base)) {
            ++g_built_count;
        }
    }
    if (g_built_count >= CruisePilotCount()) g_manager_ready = false;
}

// City shutdown (0x29690) destroys ambient/race state before reaching the
// normal opponent-array cleanup. Retire extra traffic while those dependencies
// and the physical registration are still alive. Keep manager cleanup hooks
// as fallback for other owner-array teardown paths.
void Mc3EnhancedTrafficCityExit(X86Context& ctx, uint8_t* base) {
    if (!base || !g_manager) return;
    CheckTrafficPhysics(base, "city-exit");
    const uint32_t saved_ecx = ctx.ecx;
    ctx.ecx = g_manager;
    Mc3EnhancedTrafficReleaseSlots(ctx, base);
    ctx.ecx = saved_ecx;
}

// Called once per main-loop tick, outside the renderer's list traversal.
// Build at most one complete vehicle per tick so the original streaming and
// registration path never receives a burst of large 0x3138-byte car stacks.
void Mc3EnhancedTrafficTick(X86Context& ctx, uint8_t* base) {
    ++g_tick;
    extern void Mc3WriteWatchStart(uint8_t*);  // write_watch.cpp: --mc3-write-watch
    Mc3WriteWatchStart(base);
    if (base && g_monitor_physics) CheckTrafficPhysics(base, "tick");
    // State 2 remains set while the pause menu is open. Match the native
    // simulation gate (gGameState+9 and the network/race checks), not just dt.
    if (Enabled() && base && ctx.eax == 2 &&
        (GuestCall(ctx, base, sub_0001D9B0, 0, {}) & 0xff)) return;
    // Native race opponents acquire their `_o` resources while mcGame still
    // reports state 7. Reproduce that window for the one-car Cruise pilot:
    // the late state-2 construction is precisely where PageIn sees shader
    // word 0xAA as type 42. Ambient instances already exist at this point.
    if (Enabled() && base && ctx.eax == 7 && g_cruise_load_window_seen) {
        TryCruisePilot(ctx, base);
        if (g_cruise_pilot_active && g_manager_ready &&
            g_built_count < CruisePilotCount()) {
            for (auto& item : g_ambient) {
                if (g_built_count >= CruisePilotCount()) break;
                if (item.entity) continue;
                if (!Guest(item.ambient, 0x21c) ||
                    U32(base, item.ambient + 0xc4) == 1 ||
                    !Guest(U32(base, item.ambient + 0xfc))) continue;
                LOGE("MC3TRAFFIC cruise early-build state=%u tick=%llu ambient=%08X lane=%08X",
                     ctx.eax, static_cast<unsigned long long>(g_tick), item.ambient,
                     U32(base, item.ambient + 0xfc));
                const uint32_t template_index = g_built_count % g_template_count;
                if (BuildCompleteVehicle(item, g_templates[template_index], ctx, base)) {
                    ++g_built_count;
                    if (g_built_count >= CruisePilotCount())
                        g_manager_ready = false;
                }
            }
        }
    }
    if (Enabled() && base && ctx.eax == 2) TryCruisePilot(ctx, base);
    if (Enabled() && base && ctx.eax == 2) {
        using TimingClock = std::chrono::steady_clock;
        static const bool timing_enabled =
            Mc3DebugHasArg(L"--mc3-traffic-timing");
        const auto timing_frame_begin = TimingClock::now();
        uint64_t timing_observe_ns = 0;
        uint64_t timing_collider_ns = 0;
        uint64_t timing_route_ns = 0;
        uint64_t timing_target_ns = 0;
        uint64_t timing_input_ns = 0;
        uint32_t timing_active_cars = 0;
        // Cruise creates the complete vp_* car during the state-7 resource
        // window, before the ambient traffic director has chosen which pool
        // slots will become live.  The construction slot is therefore only a
        // spawn pose.  Once state 2 exposes a live lane, bind the actor to the
        // nearest active ambient oracle and leave the waiting slot untouched.
        for (auto& waiting : g_ambient) {
            if (!waiting.owner || !Guest(waiting.ambient, 0x21c) ||
                (U32(base, waiting.ambient + 0xc4) != 1 &&
                 Guest(U32(base, waiting.ambient + 0xfc)))) continue;
            if (!Guest(waiting.car, 0x44)) continue;
            const auto* physical = reinterpret_cast<const float*>(base + waiting.car + 0x34);
            AmbientBridge* nearest = nullptr;
            float nearest_distance = INFINITY;
            for (auto& candidate : g_ambient) {
                if (&candidate == &waiting || candidate.owner ||
                    !Guest(candidate.ambient, 0x21c) ||
                    U32(base, candidate.ambient + 0xc4) == 1 ||
                    !Guest(U32(base, candidate.ambient + 0xfc))) continue;
                const auto* rail = reinterpret_cast<const float*>(base + candidate.ambient + 0x58);
                const float distance = std::hypot(physical[0] - rail[0],
                                                  physical[2] - rail[2]);
                if (std::isfinite(distance) && distance < nearest_distance) {
                    nearest = &candidate;
                    nearest_distance = distance;
                }
            }
            if (!nearest) continue;
            nearest->owner = waiting.owner;
            nearest->entity = waiting.entity;
            nearest->car = waiting.car;
            waiting.owner = waiting.entity = waiting.car = 0;
            // Reuse the opponent's native start-pose and Reset path instead
            // of making the AI drive across the city from the construction
            // slot.  owner+0x7c is the bridge's existing temporary target
            // buffer and is overwritten normally by the controller below.
            const uint32_t pose_va = nearest->owner + 0x7c;
            const auto* matrix = reinterpret_cast<const float*>(
                base + nearest->ambient + 0x34);
            auto* pose = reinterpret_cast<float*>(base + pose_va);
            pose[0] = matrix[9];
            pose[1] = matrix[10];
            pose[2] = matrix[11];
            pose[3] = std::atan2(matrix[6], matrix[8]);
            GuestCall(ctx, base, sub_002C8860, nearest->owner + 0xc,
                      {pose_va, 1});
            GuestCall(ctx, base, sub_002C8690, nearest->owner + 0xc, {0});
            LOGE("MC3TRAFFIC cruise lane bind spawn=%08X oracle=%08X lane=%08X distance=%.2f",
                 waiting.ambient, nearest->ambient,
                 U32(base, nearest->ambient + 0xfc), nearest_distance);
        }
        // More cars than spawn points: the state-7 checkpoint builds one per ControlledAmbient (San Diego's Cruise has
        // 18), which loads every model of the pool. The rest go straight onto free live lanes, one per tick, from
        // types already loaded: a repeated model shares its type (refs > 1), so nothing new is paged in during state
        // 2, which is what broke late construction before (shader type 42). Only once every distinct model is in
        // Opt-in --mc3-traffic-cruise-late-models drops that wait, so a pool with more models than spawn points
        // (needs --mc3-car-cache past 16) pages the rest in during state 2.
        static const bool late_models = Mc3DebugHasArg(L"--mc3-traffic-cruise-late-models");
        if (g_cruise_pilot_active && g_manager_ready && g_built_count < CruisePilotCount() &&
            (late_models || g_built_count >= g_cruise_distinct_models)) {
            for (auto& item : g_ambient) {
                if (item.owner || item.entity || !Guest(item.ambient, 0x21c) ||
                    U32(base, item.ambient + 0xc4) == 1 ||
                    !Guest(U32(base, item.ambient + 0xfc))) continue;
                if (BuildCompleteVehicle(item, g_templates[g_built_count % g_template_count], ctx, base)) {
                    ++g_built_count;
                    LOGE("MC3TRAFFIC cruise live-lane build ambient=%08X lane=%08X built=%u",
                         item.ambient, U32(base, item.ambient + 0xfc), g_built_count);
                    if (g_built_count >= CruisePilotCount()) g_manager_ready = false;
                }
                break;
            }
        }
        // Opt-in lifecycle test: retire one live lane through the game's own
        // Deactivate routine after the pool has settled. The ordinary traffic
        // director must respawn it; the bridge then reuses the freed owner.
        static const bool recycle_test =
            Mc3DebugHasArg(L"--mc3-traffic-cruise-recycle-test");
        if (!g_cruise_pool_full_tick &&
            g_built_count >= CruisePilotCount())
            g_cruise_pool_full_tick = g_tick;
        if (recycle_test && !g_cruise_recycle_test_fired &&
            g_cruise_pool_full_tick &&
            g_tick - g_cruise_pool_full_tick >= 600) {
            for (auto& item : g_ambient) {
                if (!item.owner || !Guest(item.ambient, 0x21c) ||
                    U32(base, item.ambient + 0xc4) == 1 ||
                    !Guest(U32(base, item.ambient + 0xfc))) continue;
                g_cruise_recycle_test_fired = true;
                LOGE("MC3TRAFFIC recycle test native deactivate owner=%08X ambient=%08X lane=%08X",
                     item.owner, item.ambient, U32(base, item.ambient + 0xfc));
                GuestCall(ctx, base, sub_00079AB0, item.ambient, {});
                break;
            }
        }
        const auto timing_observe_begin = TimingClock::now();
        for (auto& item : g_ambient) {
            if (!item.ambient || !Guest(item.ambient, 0x21c)) continue;
            const auto* position = reinterpret_cast<const float*>(base + item.ambient + 0x58);
            if (item.observed_position) {
                const float dx = position[0] - item.previous_position[0];
                const float dz = position[2] - item.previous_position[2];
                const float distance = std::hypot(dx, dz);
                if (std::isfinite(distance) && distance > .001f && distance < 10.0f) {
                    item.observed_motion += distance;
                    item.last_moved = g_tick;
                }
            }
            std::copy_n(position, 3, item.previous_position.begin());
            item.observed_position = true;
        }
        timing_observe_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            TimingClock::now() - timing_observe_begin).count());
        for (auto& item : g_ambient) {
            if (!item.owner || !Guest(item.ambient, 0x21c)) continue;
            if (U32(base, item.ambient + 0xc4) == 1 ||
                !Guest(U32(base, item.ambient + 0xfc))) continue;
            ++timing_active_cars;
            auto timing_section_begin = TimingClock::now();
            RemoveAmbientCollider(ctx, base, item.ambient);
            timing_collider_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                TimingClock::now() - timing_section_begin).count());
            float dt = *reinterpret_cast<const float*>(base + 0x4b2d48);
            const auto* before = reinterpret_cast<const float*>(base + item.ambient + 0x34);
            const auto* physical = reinterpret_cast<const float*>(base + item.car + 0x34);
            const float dx = before[9] - physical[0];
            const float dz = before[11] - physical[2];
            const float gap = std::hypot(dx, dz);
            const float behind = -before[6] * dx - before[8] * dz;
            // A rail car has no acceleration/impact delay. Let the physical
            // car catch up after a collision instead of sending its oracle
            // through several intersections while the car is still blocked.
            // Only route time is slowed; the physical transform stays owned
            // by the original simulation and the lane choice stays native.
            if (behind > 10.0f || gap > 40.0f)
                dt *= std::clamp((30.0f - gap) / 20.0f, 0.0f, 1.0f);
            uint32_t dt_bits = 0;
            std::memcpy(&dt_bits, &dt, 4);
            timing_section_begin = TimingClock::now();
            GuestCall(ctx, base, sub_00078940, item.ambient, {dt_bits});
            timing_route_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                TimingClock::now() - timing_section_begin).count());
            const auto* matrix = reinterpret_cast<const float*>(base + item.ambient + 0x34);
            const float rail_speed = *reinterpret_cast<const float*>(base + item.ambient + 0xe0);
            if (!std::isfinite(rail_speed)) continue;
            const float speed = std::clamp(std::fabs(rail_speed), 0.0f, 25.0f);
            // The ambient controller remains the route oracle. Feed a target
            // and requested speed to the ordinary opponent input controller;
            // the real vehicle sim supplies suspension, wheels and collision.
            const uint32_t target_va = item.owner + 0x7c;
            timing_section_begin = TimingClock::now();
            GuestCall(ctx, base, sub_001F1710, target_va, {});
            auto* target = reinterpret_cast<float*>(base + target_va);
            const float ahead = 5.0f + speed * .5f;
            for (uint32_t axis = 0; axis < 3; ++axis) {
                target[axis] = matrix[9 + axis] - matrix[6 + axis] * ahead;
                // aiInput uses this as a spline tangent, not a velocity.
                target[3 + axis] = -matrix[6 + axis];
            }
            uint32_t speed_bits = 0;
            std::memcpy(&speed_bits, &speed, 4);
            timing_target_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                TimingClock::now() - timing_section_begin).count());
            timing_section_begin = TimingClock::now();
            GuestCall(ctx, base, sub_001ED9C0, item.owner, {target_va, speed_bits});
            timing_input_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                TimingClock::now() - timing_section_begin).count());
        }
        if (timing_enabled) {
            const uint64_t timing_total_ns = uint64_t(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    TimingClock::now() - timing_frame_begin).count());
            ++g_traffic_timing.frames;
            g_traffic_timing.active_cars += timing_active_cars;
            g_traffic_timing.total_ns += timing_total_ns;
            g_traffic_timing.observe_ns += timing_observe_ns;
            g_traffic_timing.collider_ns += timing_collider_ns;
            g_traffic_timing.route_ns += timing_route_ns;
            g_traffic_timing.target_ns += timing_target_ns;
            g_traffic_timing.input_ns += timing_input_ns;
            if (g_traffic_timing.frames >= 120) {
                const double divisor = double(g_traffic_timing.frames) * 1000000.0;
                const uint64_t measured_ns = g_traffic_timing.observe_ns +
                    g_traffic_timing.collider_ns + g_traffic_timing.route_ns +
                    g_traffic_timing.target_ns + g_traffic_timing.input_ns;
                const uint64_t other_ns = g_traffic_timing.total_ns > measured_ns
                    ? g_traffic_timing.total_ns - measured_ns : 0;
                LOGE("MC3TRAFFIC timing frames=%llu cars=%.1f total=%.3fms observe=%.3fms "
                     "collider=%.3fms route=%.3fms target=%.3fms input=%.3fms other=%.3fms",
                     static_cast<unsigned long long>(g_traffic_timing.frames),
                     double(g_traffic_timing.active_cars) / double(g_traffic_timing.frames),
                     double(g_traffic_timing.total_ns) / divisor,
                     double(g_traffic_timing.observe_ns) / divisor,
                     double(g_traffic_timing.collider_ns) / divisor,
                     double(g_traffic_timing.route_ns) / divisor,
                     double(g_traffic_timing.target_ns) / divisor,
                     double(g_traffic_timing.input_ns) / divisor,
                     double(other_ns) / divisor);
                g_traffic_timing = {};
            }
        }
    }
    if (Enabled() && base && ctx.eax == 2 && g_tick % 120 == 0) {
        for (const auto& item : g_ambient) {
            if (!item.car || !Guest(item.car, 0x44)) continue;
            const auto* p = reinterpret_cast<const float*>(base + item.car + 0x34);
            const auto* rail = reinterpret_cast<const float*>(base + item.ambient + 0x58);
            LOGE("MC3TRAFFIC actor owner=%08X entity=%08X vtable=%08X car=%08X "
                 "pos=%.2f,%.2f,%.2f sim=%08X input=%08X collider=%08X rail=%.2f target=%.2f,%.2f,%.2f "
                 "route_state=%u lane=%08X error=%.2f",
                 item.owner, item.entity, U32(base, item.entity), item.car,
                 p[0], p[1], p[2], U32(base, item.entity + 8),
                 U32(base, item.entity + 4), U32(base, item.entity + 0x44),
                 *reinterpret_cast<const float*>(base + item.ambient + 0xe0),
                 rail[0], rail[1], rail[2], U32(base, item.ambient + 0xc4),
                 U32(base, item.ambient + 0xfc), std::hypot(p[0] - rail[0], p[2] - rail[2]));
        }
    }
    if (!Enabled() || !base || ctx.eax != 2 || !g_manager_ready ||
        !g_template_count || g_built_count >= kTrafficCapacity)
        return;
    static const bool preload_probe = Mc3DebugHasArg(L"--mc3-traffic-cruise-preload-probe");
    if (g_cruise_pilot_active && preload_probe && !g_cruise_pilot_preloaded) {
        const uint32_t cache = *reinterpret_cast<const uint32_t*>(base + 0x50a08c);
        if (!Guest(cache, 0x100) || g_cruise_preload_polls >= 180) {
            if (g_cruise_preload_polls++ == 0 || g_cruise_preload_polls == 181)
                LOGE("MC3TRAFFIC cruise preload unavailable cache=%08X polls=%u",
                     cache, g_cruise_preload_polls);
            return;
        }
        if (g_cruise_preload_polls == 0) {
            LOGE("MC3TRAFFIC cruise preload name='%s' registry_index=%d player_registry_index=%d",
                 GuestString(base, g_cruise_pilot_name),
                 int32_t(GuestCall(ctx, base, sub_0026A6A0, 0, {g_cruise_pilot_name})),
                 int32_t(GuestCall(ctx, base, sub_0026A6A0, 0, {g_cruise_pilot_config + 0xe3})));
            LOGE("MC3TRAFFIC cruise preload suffix bytes=%02X/%02X/%02X/%02X alternate=%02X/%02X/%02X/%02X",
                 base[0x455298], base[0x455299], base[0x45529a], base[0x45529b],
                 base[0x45529c], base[0x45529d], base[0x45529e], base[0x45529f]);
            for (unsigned i = 0; i < 16; ++i) {
                const uint32_t e = cache + i * 16;
                LOGE("MC3TRAFFIC cruise cache slot=%u hash=%08X ptr=%08X flags=%02X/%02X/%02X handle=%08X",
                     i, U32(base, e), U32(base, e + 4), base[e + 8], base[e + 9],
                     base[e + 10], U32(base, e + 12));
            }
        }
        const uint32_t result = GuestCall(ctx, base, sub_00105CA0, cache,
            {g_cruise_pilot_name, 0, 0, 0, 0, 1});
        ++g_cruise_preload_polls;
        if (g_cruise_preload_polls == 1 || result || g_cruise_preload_polls % 30 == 0)
            LOGE("MC3TRAFFIC cruise preload cache=%08X result=%08X polls=%u suffix='%s'",
                 cache, result, g_cruise_preload_polls,
                 reinterpret_cast<const char*>(base + 0x455298));
        if (g_cruise_preload_polls == 1 || g_cruise_preload_polls == 30)
            for (unsigned i = 0; i < 3; ++i) {
                const uint32_t e = cache + i * 16;
                LOGE("MC3TRAFFIC cruise cache after slot=%u hash=%08X ptr=%08X flags=%02X/%02X/%02X handle=%08X",
                     i, U32(base, e), U32(base, e + 4), base[e + 8], base[e + 9],
                     base[e + 10], U32(base, e + 12));
            }
        if (Guest(result, 0x80)) g_cruise_pilot_preloaded = true;
        return;
    }
    for (auto& item : g_ambient) {
        if (!item.ambient || item.entity || g_tick - item.last_seen > 2) continue;
        // A collider-state ambient has already left the lane controller.
        // It cannot supply a live route to a new physical traffic actor.
        if (U32(base, item.ambient + 0xc4) == 1 ||
            !Guest(U32(base, item.ambient + 0xfc))) continue;
        if (item.observed_motion < .5f || g_tick - item.last_moved > 3) continue;
        const float speed = *reinterpret_cast<const float*>(base + item.ambient + 0xe0);
        if (!std::isfinite(speed) || speed < 2.0f) continue;
        // Stable address hashing gives each ambient slot a deterministic but
        // varied complete vp_* vehicle without depending on frame rate.
        const uint32_t choice = ((item.ambient >> 2) ^ (item.ambient >> 11) ^
                                 0x9e3779b9u) % g_template_count;
        if (BuildCompleteVehicle(item, g_templates[choice], ctx, base))
            ++g_built_count;
        if (g_cruise_pilot_active) g_manager_ready = false;
        break;
    }
}


// Temporary construction diagnostic: identify the first allocator boundary
// after an overwrite without suppressing any allocation or native check.
void Mc3EnhancedTrafficAllocationProbe(X86Context& ctx, uint8_t* base) {
    if (!g_constructing || !base) return;
    static const bool enabled = Mc3DebugHasArg(L"--mc3-traffic-check-allocations");
    if (!enabled) return;
    static bool reported = false;
    if (reported || CheckTrafficHeap(ctx, base, "allocation", true)) return;
    reported = true;
    for (uint32_t off = 0; off < 0x100; off += 4)
        LOGE("MC3TRAFFIC allocation stack+%X=%08X", off, U32(base, ctx.esp + off));
}

// Opt-in construction checkpoints around the native vehCar setup calls.
// The first failed boundary identifies the writer interval, without changing
// native control flow or object contents. Remove after MC3-0015 is stable.
void Mc3EnhancedTrafficBoundaryProbe(X86Context& ctx, uint8_t* base, uint32_t guest_pc) {
    if (!g_constructing || !base) return;
    static const bool enabled = Mc3DebugHasArg(L"--mc3-traffic-check-allocations");
    static bool reported = false;
    if (!enabled || reported || CheckTrafficHeap(ctx, base, "boundary", true)) return;
    reported = true;
    LOGE("MC3TRAFFIC first failed setup boundary=%08X", guest_pc);
}

#define MC3_TRAFFIC_BOUNDARY(name, address) \
    void Mc3EnhancedTrafficBoundary##name(X86Context& ctx, uint8_t* base) { \
        Mc3EnhancedTrafficBoundaryProbe(ctx, base, address); \
    }
MC3_TRAFFIC_BOUNDARY(00, 0x001FA9C0)
MC3_TRAFFIC_BOUNDARY(01, 0x001FA9D9)
MC3_TRAFFIC_BOUNDARY(02, 0x001FA9F8)
MC3_TRAFFIC_BOUNDARY(03, 0x001FAA0D)
MC3_TRAFFIC_BOUNDARY(04, 0x001FAA15)
MC3_TRAFFIC_BOUNDARY(05, 0x001FAA1D)
MC3_TRAFFIC_BOUNDARY(06, 0x001FAA25)
MC3_TRAFFIC_BOUNDARY(07, 0x001FAA2D)
MC3_TRAFFIC_BOUNDARY(08, 0x001FAA35)
MC3_TRAFFIC_BOUNDARY(09, 0x001FAA3D)
MC3_TRAFFIC_BOUNDARY(10, 0x001FAA45)
MC3_TRAFFIC_BOUNDARY(11, 0x001FAA4D)
MC3_TRAFFIC_BOUNDARY(12, 0x001FAA55)
MC3_TRAFFIC_BOUNDARY(13, 0x001FAA5D)
MC3_TRAFFIC_BOUNDARY(14, 0x001FAA64)
#undef MC3_TRAFFIC_BOUNDARY

// 0x1FA9C0 allocates a 0x1AC-byte object. Its constructor initializes exactly
// four index words at +0x190..+0x19C and floats at +0x1A0..+0x1A8. Native
// 0x121150 appends indices from a global list without checking that four-word
// limit. With extra full vehicles, it can write into the next heap header.
// Skip only the overflowing stores; native scanning and final float writes
// continue. This is scoped to our own extra aiOpponent::Init call.
bool Mc3EnhancedTrafficBoundedSetupIndex(X86Context& ctx, uint8_t* base) {
    if (!Enabled() || !g_constructing || !base) return false;
    const uint32_t object = U32(base, ctx.esp + 0x14);
    if (!Guest(object, 0x1ac) || U32(base, object) != 0x004564f0u) return false;
    if (ctx.esi < object + 0x1a0 || ctx.esi >= object + 0x1000) return false;
    if (g_bounded_index_writes++ < 16)
        LOGE("MC3TRAFFIC bounded native setup index object=%08X target=%08X slot=%u",
             object, ctx.esi, (ctx.esi - object - 0x190) / 4);
    return true;
}

// Native aiAmbientVehicle activation cleanup assumes its phInst is registered
// while +AD is set. The bridge deliberately detached that collider earlier.
// Skip only that redundant RemoveEntry(FFFF), not the rest of native cleanup.
// Otherwise 0x2AD433 writes types[FFFF]=6 into another physical record.
bool Mc3EnhancedTrafficDetachedAmbientRemoval(X86Context& ctx, uint8_t* base) {
    if (!Enabled() || !base || !Guest(ctx.esi, 0x21c) || U32(base, ctx.esp) != 0xffff ||
        std::find(g_detached_ambients.begin(), g_detached_ambients.end(), ctx.esi) == g_detached_ambients.end())
        return false;
    LOGE("MC3TRAFFIC already detached ambient=%08X native_cleanup=000796B0", ctx.esi);
    ctx.esp += 4;  // consume the single argument of the skipped stdcall
    return true;
}

void Mc3EnhancedTrafficPhysicsRemoveProbe(X86Context& ctx, uint8_t* base) {
    if (!base || !g_monitor_physics || U32(base, ctx.esp + 4) != 0xffff) return;
    LOGE("MC3TRAFFIC invalid physics removal caller=%08X level=%08X", U32(base, ctx.esp), ctx.ecx);
}
