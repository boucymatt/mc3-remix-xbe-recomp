#pragma once
#include <string>
#include <string_view>
#include "cpu/x86_context.h"

namespace mc2 {

struct HookmanTracking {
    int hookman_id = -1;
    std::string name;
    float distance = 0.0f;
    float height_diff = 0.0f;
    bool headlights_active = false;
    bool is_engaged = false;
    bool follow_active = false;
    bool route_complete = false;
    bool follow_completed = false;
    float follow_elapsed_sec = 0.0f;
    float follow_finish_distance = 28.0f;
    float follow_distance_limit = 200.0f;
    float follow_out_of_range_sec = 0.0f;
    float follow_loss_timeout_sec = 5.0f;
    uint32_t player_matrix = 0;
    uint32_t opponent_matrix = 0;
    uint32_t hookman_brain = 0;
    bool brain_identity_confirmed = false;
    std::string lost_reason;
};

// Initialize Hookman tracking for the current city
void InitHookmanBridge();

// Update Hookman proximity, headlights, and follow mechanics
void UpdateHookmanBridge(X86Context& ctx, uint8_t* base);
void UpdateHookmanBridgeHost();

// Hooks for aiBrainHookman and Follow selection
void HookmanAiUpdate(X86Context& ctx, uint8_t* base);
void HookmanPreUpdate(X86Context& ctx, uint8_t* base);
void HookmanRestartCruise(X86Context& ctx, uint8_t* base);
bool HookmanSelectRaceFromFollow(X86Context& ctx, uint8_t* base);
void NotifyHookmanRouteComplete();

// Prompt HUD helper
void ShowFindHookmanPrompt();

const HookmanTracking& GetHookmanTracking();

} // namespace mc2
