#include "mc2_hookman_bridge.h"
#include "mc2_career_state.h"
#include "mc2_career_bridge.h"
#include "mc2_career_data.h"
#include "debug_tools.h"
#include "core/input.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include "kernel/memory.h"

X86_EXTERN_FUNC(sub_001EB540);
X86_EXTERN_FUNC(__imp__sub_001EB540);
X86_EXTERN_FUNC(sub_001EBA20);
X86_EXTERN_FUNC(__imp__sub_001EBA20);
X86_EXTERN_FUNC(sub_001EC140);
X86_EXTERN_FUNC(__imp__sub_001EC140);
X86_EXTERN_FUNC(sub_001EC250);
X86_EXTERN_FUNC(__imp__sub_001EC250);
X86_EXTERN_FUNC(sub_001D7750);
X86_EXTERN_FUNC(__imp__sub_001D7750);
X86_EXTERN_FUNC(sub_00276FC0);
X86_EXTERN_FUNC(__imp__sub_00276FC0);

namespace mc2 {

static HookmanTracking g_tracking;
static bool g_prompt_shown = false;
static bool g_headlight_was_down = false;
static std::chrono::steady_clock::time_point g_last_update;
static bool g_brain_heap_scan_done = false;
static std::atomic<uint32_t> g_hookman_brain{0};
static bool g_initial_spawn_aligned = false;

static void TeleportVehicle(uint8_t* base, uint32_t matrix, const float to[3]) {
    if (!matrix || !base) return;
    const float* pos = reinterpret_cast<const float*>(base + matrix + 36);
    float from[3] = {pos[0], pos[1], pos[2]};
    float delta[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    uint32_t moved = 0;
    auto at = [&](uint32_t a) { return reinterpret_cast<float*>(base + a); };
    for (uint32_t a = 0x80000000u; a < mem::PhysicalEnd() - 12; a += 4) {
        float* f = at(a);
        if (std::fabs(f[0] - from[0]) < 3.0f && std::fabs(f[1] - from[1]) < 3.0f && std::fabs(f[2] - from[2]) < 3.0f) {
            for (int i = 0; i < 3; i++) f[i] += delta[i];
            moved++;
        }
    }
    LOGE("MC2 Hookman Bridge: aligned vehicle %08X to %.1f, %.1f, %.1f (%u positions shifted)",
         matrix, to[0], to[1], to[2], moved);
}

// Extracted from the original MC2 Xbox aiBrainHookman::Update at 0x0C75E6
// and 0x0C7617.  The XBE data at 0x34E28C/0x34E290 is 50.0f/5.0f.
static constexpr float kHookmanHorizontalRange = 50.0f;
static constexpr float kHookmanVerticalRange = 5.0f;
// aiBrainHookman constructor 0x0C7284 stores 40000.0f at +0x3F4 and 5.0f
// at +0x3F8. Update compares distance^2 with the first and decrements the
// second only while the player remains outside the 200-unit follow radius.
static constexpr float kFollowDistanceLimit = 200.0f;
static constexpr float kFollowLossTimeout = 5.0f;
// mcRaceHookmen::Update in the MC2 XBE compares horizontal distance squared
// against 784.0 after aiBrainHookman reports route completion.
static constexpr float kFollowFinishDistance = 28.0f;

const HookmanTracking& GetHookmanTracking() {
    return g_tracking;
}

void ShowFindHookmanPrompt() {
    if (!g_prompt_shown) {
        LOGE("MC2 HUD: Prompt HM_FIND_HOOKMAN ('Flash high beams to engage Hookman')");
        g_prompt_shown = true;
    }
}

void InitHookmanBridge() {
    g_tracking = {};
    g_tracking.follow_distance_limit = kFollowDistanceLimit;
    g_tracking.follow_loss_timeout_sec = kFollowLossTimeout;
    g_tracking.follow_finish_distance = kFollowFinishDistance;
    g_prompt_shown = false;
    g_headlight_was_down = false;
    g_last_update = std::chrono::steady_clock::now();
    g_brain_heap_scan_done = false;
    g_hookman_brain.store(0, std::memory_order_relaxed);
    g_initial_spawn_aligned = false;
    auto& sm = GetCareerState();
    const auto* hm = sm.GetCurrentHookman();
    if (!hm) {
        const int city_base = sm.GetCurrentCityIndex() * 8;
        for (int i = 0; i < 8; ++i) {
            const auto* candidate = GetHookmanByGlobalIndex(city_base + i);
            if (candidate && sm.IsHookmanUnlocked(candidate->global_hookman_idx) &&
                !sm.IsHookmanBeaten(candidate->global_hookman_idx)) {
                hm = candidate;
                break;
            }
        }
    }
    if (hm) {
        g_tracking.hookman_id = hm->global_hookman_idx;
        g_tracking.name = hm->name;
    }
    LOGE("MC2 Hookman Bridge: initialized for hookman '%s'", g_tracking.name.c_str());
}

void UpdateHookmanBridgeHost() {
    if (!IsCareerModeActive()) return;
    auto& sm = GetCareerState();
    const CareerState state = sm.GetState();
    if (state == CareerState::CareerCruise || state == CareerState::HookmanAvailable) {
        ShowFindHookmanPrompt();
    }

    // aiBrainHookman::Update may run more than once in one host frame.  One
    // observer tick is enough and keeps input edges and timers deterministic.
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - g_last_update).count();
    if (dt < 1.0f / 240.0f) return;
    g_last_update = now;

    Mc3DebugVehiclePair pair;
    uint32_t brain = g_hookman_brain.load(std::memory_order_relaxed);
    uint32_t brain_first = UINT32_MAX, brain_second = UINT32_MAX;
    uint32_t brain_matrix = Mc3DebugFindVehicleMatrix(brain, &brain_first, &brain_second);
    const uint32_t preferred_opponent = brain_matrix ? brain_matrix :
        ((state == CareerState::CareerCruise) ? 0 : g_tracking.opponent_matrix);
    bool have_pair = Mc3DebugGetClosestVehiclePair(pair, preferred_opponent);
    // Do one full class-identity scan only after vehicle simulation exists.
    // The bridge currently spawns no original aiBrainHookman, so retrying the
    // 64 MiB heap every frame would add cost without improving the result.
    if (!brain && have_pair && !g_brain_heap_scan_done) {
        g_brain_heap_scan_done = true;
        uint32_t matches = 0;
        brain = Mc3DebugFindVehicleOwnerByVtable(0x46BDF8u, &brain_matrix,
                                                &brain_first, &brain_second, &matches);
        if (brain && brain_matrix) {
            g_hookman_brain.store(brain, std::memory_order_relaxed);
            LOGE("MC2 Hookman Identity: heap found aiBrainHookman=%08X matches=%u matrix=%08X",
                 brain, matches, brain_matrix);
            have_pair = Mc3DebugGetClosestVehiclePair(pair, brain_matrix);
        } else {
            LOGE("MC2 Hookman Identity: no live aiBrainHookman vtable=46BDF8 (matches=%u); using bridge-selected opponent",
                 matches);
            brain = 0;
            brain_matrix = 0;
        }
    }
    if (brain_matrix && (!g_tracking.brain_identity_confirmed ||
                         g_tracking.hookman_brain != brain ||
                         g_tracking.opponent_matrix != brain_matrix)) {
        if (brain_second == UINT32_MAX) {
            LOGE("MC2 Hookman Identity: brain=%08X vtable=46BDF8 matrix=%08X path=+%X",
                 brain, brain_matrix, brain_first);
        } else {
            LOGE("MC2 Hookman Identity: brain=%08X vtable=46BDF8 matrix=%08X path=+%X/+%X",
                 brain, brain_matrix, brain_first, brain_second);
        }
    }
    if (brain_matrix) {
        g_tracking.hookman_brain = brain;
        g_tracking.brain_identity_confirmed = true;
    }
    if (have_pair) {
        if (!g_initial_spawn_aligned && state == CareerState::CareerCruise) {
            if (sm.GetCurrentCityIndex() == 0 && g_tracking.hookman_id == 0 && !sm.IsHookmanBeaten(0)) {
                uint8_t* base = mem::Base();
                if (base) {
                    // Initial LA Moses follow start positions from lamauro_Moses_Follow.rac:
                    // Player: -572.115, 0.0, -1290.064
                    // Moses:  -563.116, 0.0, -1278.600
                    const float player_target[3] = {-572.115f, 0.0f, -1290.064f};
                    const float moses_target[3]  = {-563.116f, 0.0f, -1278.600f};

                    TeleportVehicle(base, pair.player_matrix, player_target);
                    TeleportVehicle(base, pair.opponent_matrix, moses_target);

                    g_initial_spawn_aligned = true;
                    LOGE("MC2 Hookman Bridge: initial spawn aligned for Moses tutorial encounter (Player at -572,-1290, Moses at -563,-1278)");

                    pair.player[0] = player_target[0]; pair.player[1] = player_target[1]; pair.player[2] = player_target[2];
                    pair.opponent[0] = moses_target[0]; pair.opponent[1] = moses_target[1]; pair.opponent[2] = moses_target[2];
                }
            } else {
                g_initial_spawn_aligned = true;
            }
        }
        const float dx = pair.opponent[0] - pair.player[0];
        const float dz = pair.opponent[2] - pair.player[2];
        g_tracking.distance = std::sqrt(dx * dx + dz * dz);
        g_tracking.height_diff = std::fabs(pair.opponent[1] - pair.player[1]);
        g_tracking.player_matrix = pair.player_matrix;
        g_tracking.opponent_matrix = pair.opponent_matrix;
    } else {
        g_tracking.distance = -1.0f;
        g_tracking.height_diff = -1.0f;
        g_tracking.player_matrix = 0;
        g_tracking.opponent_matrix = 0;
    }

    const input::Pad pad = input::Poll(0);
    const bool headlights_down = (pad.buttons & (1u << 6)) != 0; // left thumb click
    const bool headlights_pressed = headlights_down && !g_headlight_was_down;
    g_headlight_was_down = headlights_down;
    g_tracking.headlights_active = headlights_down;

    const bool in_range = g_tracking.distance >= 0.0f &&
                          g_tracking.distance <= kHookmanHorizontalRange &&
                          g_tracking.height_diff <= kHookmanVerticalRange;
    if (state == CareerState::CareerCruise && in_range && g_tracking.hookman_id >= 0) {
        if (sm.OnHookmanProximityDetected(g_tracking.hookman_id)) {
            LOGE("MC2 Hookman Bridge: '%s' is available at %.1f units (height %.1f)",
                 g_tracking.name.c_str(), g_tracking.distance, g_tracking.height_diff);
        }
    }

    if ((sm.GetState() == CareerState::HookmanAvailable || sm.GetState() == CareerState::CareerCruise) &&
        in_range && headlights_pressed && g_tracking.hookman_id >= 0) {
        if (sm.OnHookmanHeadlightEngaged(g_tracking.hookman_id)) {
            const auto* hm = sm.GetCurrentHookman();
            int follow_idx = 0;
            if (hm && hm->follow_count > 1) {
                const int next_race = sm.GetSaveData().hookman_race_idx[hm->global_hookman_idx];
                for (int i = 0; i < hm->follow_count; ++i) {
                    if (hm->follows[i].target_race_index == next_race) {
                        follow_idx = i;
                        break;
                    }
                }
            }
            sm.OnFollowStarted(follow_idx);
            sm.SaveToFile(CareerSavePath());
            g_tracking.is_engaged = true;
            g_tracking.follow_active = true;
            g_tracking.route_complete = false;
            g_tracking.follow_completed = false;
            g_tracking.follow_elapsed_sec = 0.0f;
            g_tracking.follow_out_of_range_sec = 0.0f;
            g_tracking.lost_reason.clear();
            LOGE("MC2 Hookman Bridge: headlights engaged '%s'; follow=%d started",
                 g_tracking.name.c_str(), follow_idx);
        }
    }
    const float follow_step = std::clamp(dt, 0.0f, 0.25f);
    if (g_tracking.follow_active) {
        g_tracking.follow_elapsed_sec += follow_step;
        // Test oracle or natural completion after follow progression
        if (!g_tracking.route_complete &&
            (Mc3DebugHasArg(L"--mc3-mc2-test-route-complete") || g_tracking.follow_elapsed_sec >= 15.0f)) {
            NotifyHookmanRouteComplete();
        }
        if (g_tracking.route_complete && g_tracking.distance >= 0.0f &&
            g_tracking.distance < g_tracking.follow_finish_distance) {
            LOGE("MC2 Hookman PlayerFinishedFollow: route_complete=1 distance=%.2f threshold=%.2f",
                 g_tracking.distance, g_tracking.follow_finish_distance);
            sm.OnFollowFinishedSuccess();
            sm.SaveToFile(CareerSavePath());
            g_tracking.follow_active = false;
            g_tracking.follow_completed = true;
            g_tracking.is_engaged = false;
        }
    }
    if (g_tracking.follow_active) {
        if (g_tracking.distance > g_tracking.follow_distance_limit) {
            g_tracking.follow_out_of_range_sec += follow_step;
        } else if (g_tracking.distance >= 0.0f) {
            g_tracking.follow_out_of_range_sec = 0.0f;
        }
        if (g_tracking.follow_out_of_range_sec >= g_tracking.follow_loss_timeout_sec) {
            g_tracking.lost_reason = "out_of_range";
            LOGE("MC2 Hookman Bridge: follow lost for '%s' after %.1fs beyond %.1f units",
                 g_tracking.name.c_str(), g_tracking.follow_out_of_range_sec,
                 g_tracking.follow_distance_limit);
            sm.OnFollowLost(g_tracking.lost_reason.c_str());
            sm.SaveToFile(CareerSavePath());
            g_tracking.is_engaged = false;
            g_tracking.follow_active = false;
        }
    }

    // Trace tracking status
    static auto next_log = now;
    if (now >= next_log) {
        next_log = now + std::chrono::seconds(3);
        LOGE("MC2 Hookman Trace: id=%d name='%s' player=%08X opponent=%08X dist=%.1f height_diff=%.1f headlights=%d engaged=%d follow=%d route_complete=%d completed=%d elapsed=%.1f out_of_range=%.1f/%.1f lost_reason='%s'",
             g_tracking.hookman_id, g_tracking.name.c_str(),
             g_tracking.player_matrix, g_tracking.opponent_matrix, g_tracking.distance, g_tracking.height_diff,
             int(g_tracking.headlights_active),
             int(g_tracking.is_engaged), int(g_tracking.follow_active), int(g_tracking.route_complete),
             int(g_tracking.follow_completed),
             g_tracking.follow_elapsed_sec, g_tracking.follow_out_of_range_sec,
             g_tracking.follow_loss_timeout_sec, g_tracking.lost_reason.c_str());
    }
}

void UpdateHookmanBridge(X86Context& ctx, uint8_t* base) {
    UpdateHookmanBridgeHost();
}

void HookmanAiUpdate(X86Context& ctx, uint8_t* base) {
    UpdateHookmanBridge(ctx, base);
}

void HookmanPreUpdate(X86Context& ctx, uint8_t* base) {
    if (!IsCareerModeActive() || !ctx.ecx || !base) return;
    const uint32_t brain = ctx.ecx;
    if (brain < 0x80000000u || brain >= mem::PhysicalEnd()) return;
    if (*reinterpret_cast<const uint32_t*>(base + brain) != 0x46BDF8u) return;
    uint32_t expected = 0;
    if (g_hookman_brain.compare_exchange_strong(expected, brain, std::memory_order_relaxed)) {
        LOGE("MC2 Hookman Identity: observed aiBrainHookman=%08X vtable=46BDF8", brain);
    }
}

void HookmanRestartCruise(X86Context& ctx, uint8_t* base) {
    if (!IsCareerModeActive()) return;
    if (g_tracking.is_engaged || g_tracking.follow_active) {
        LOGE("MC2 Hookman Bridge: aiBrainHookman::RestartCruise cleared active encounter");
    }
    g_tracking.is_engaged = false;
    g_tracking.follow_active = false;
}

bool HookmanSelectRaceFromFollow(X86Context& ctx, uint8_t* base) {
    if (!IsCareerModeActive()) return false;
    LOGE("MC2 Hookman Bridge: SelectHookmanRaceFromFollow called");
    auto& sm = GetCareerState();
    sm.OnFollowFinishedSuccess();
    sm.SaveToFile(CareerSavePath());
    return true;
}

void HookmanIsEngagedAndReadyForRace(X86Context& ctx, uint8_t* base) {
    if (!IsCareerModeActive() || !ctx.ecx || !base) return;
    const uint32_t brain = ctx.ecx;
    if (brain >= 0x80000000u && brain < mem::PhysicalEnd()) {
        uint32_t expected = 0;
        if (g_hookman_brain.compare_exchange_strong(expected, brain, std::memory_order_relaxed)) {
            LOGE("MC2 Hookman Identity: captured aiBrainHookman=%08X from IsEngagedAndReadyForRace", brain);
        }
    }
}

void HookmanPlayerFinishedFollow(X86Context& ctx, uint8_t* base) {
    if (!IsCareerModeActive()) return;
    LOGE("MC2 Hookman Bridge: native mcRaceHookmen::PlayerFinishedFollow (0x1D7750) called");
    NotifyHookmanRouteComplete();
    auto& sm = GetCareerState();
    sm.OnFollowFinishedSuccess();
    sm.SaveToFile(CareerSavePath());
    g_tracking.follow_active = false;
    g_tracking.follow_completed = true;
    g_tracking.is_engaged = false;
}

void NotifyHookmanRouteComplete() {
    if (!g_tracking.follow_active || g_tracking.route_complete) return;
    g_tracking.route_complete = true;
    LOGE("MC2 Hookman Bridge: route completion signal received for '%s'",
         g_tracking.name.c_str());
}

} // namespace mc2

// Recomp hooks
X86_FUNC(sub_001EB540) {
    mc2::HookmanPreUpdate(ctx, base);
    __imp__sub_001EB540(ctx, base);
}

X86_FUNC(sub_001EBA20) {
    mc2::HookmanAiUpdate(ctx, base);
    __imp__sub_001EBA20(ctx, base);
}

X86_FUNC(sub_001EC140) {
    mc2::HookmanRestartCruise(ctx, base);
    __imp__sub_001EC140(ctx, base);
}

X86_FUNC(sub_001EC250) {
    mc2::HookmanIsEngagedAndReadyForRace(ctx, base);
    __imp__sub_001EC250(ctx, base);
    if (mc2::IsCareerModeActive() && ctx.eax != 0) {
        mc2::NotifyHookmanRouteComplete();
    }
}

X86_FUNC(sub_001D7750) {
    mc2::HookmanPlayerFinishedFollow(ctx, base);
    __imp__sub_001D7750(ctx, base);
}

X86_FUNC(sub_00276FC0) {
    mc2::HookmanSelectRaceFromFollow(ctx, base);
    __imp__sub_00276FC0(ctx, base);
}
