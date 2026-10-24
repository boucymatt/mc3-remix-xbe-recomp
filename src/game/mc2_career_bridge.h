#pragma once
#include <cstdint>
#include "cpu/x86_context.h"

namespace mc2 {

// Returns true if MC2 career mode is active
bool IsCareerModeActive();
void SetCareerModeActive(bool active);

// Hooks called from mc2_city.cpp or recomp hooks
bool HandleSetupNextCareerEvent(X86Context& ctx, uint8_t* base);
bool HandleReportRaceResult(X86Context& ctx, uint8_t* base);
bool HandleSelectHookmanRaceFromFollow(X86Context& ctx, uint8_t* base);
// Starts a deferred pre-city movie only after the city renderer has produced
// its first valid frame, avoiding overlap with the original loading pipeline.
bool TryStartDeferredCityMovie(X86Context& ctx, uint8_t* base, bool city_first_draw_ready);

// Called from the host's per-frame state probe.  Career movies must finish (or
// be skipped by the player) before their logical career transition is applied.
void HandleCareerMovieTick(X86Context& ctx, uint8_t* base,
                           uint32_t game_state, uint32_t movie_player, uint16_t movie_flags);

// UI helpers
const char16_t* GetCurrentStageLabel();
int GetCurrentCareerCity();
int GetCurrentCareerCar();
int GetSelectedMenuStage(int city_idx);

// All career persistence goes through this path.  The unattended runner sets
// MC3_MC2_SAVE_PATH so its synthetic progression can never modify a player's
// manual save next to the executable.
const char* CareerSavePath();

// Boot args & initialization
void CareerBridgeInit();

} // namespace mc2
