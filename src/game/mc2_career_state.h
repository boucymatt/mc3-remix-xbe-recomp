#pragma once
#include "mc2_career_data.h"
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <functional>

namespace mc2 {

enum class CareerState : int {
    NewCampaign = 0,
    PreCityMovie,
    CareerCruise,
    HookmanAvailable,
    HookmanEngaged,
    FollowActive,
    FollowFailed,
    RaceSelected,
    PreRaceMovie,
    RaceActive,
    RaceLostOrCancelled,
    RaceWon,
    PostRaceMovie,
    RewardPending,
    CityTransition,
    CampaignComplete,
};

const char* ToString(CareerState state);

#pragma pack(push, 1)
struct CareerSaveData {
    uint32_t magic = 0x4D433243; // 'MC2C'
    uint32_t version = 2;
    uint32_t checksum = 0;
    uint32_t save_rev = 0;

    int32_t current_city = 0; // 0=LA, 1=Paris, 2=Tokyo
    int32_t career_iteration = 0;
    int32_t selected_mc3_car = 0;

    uint64_t races_beaten_mask = 0;   // 56 races (bits 0..55)
    uint32_t hookmen_beaten_mask = 0; // 24 hookmen (bits 0..23)
    uint8_t hookman_race_idx[24] = {0}; // current race index for each hookman

    int32_t current_hookman_idx = -1; // -1 if cruise
    int32_t current_race_idx = -1;    // index in hookman, or -1
    int32_t current_follow_idx = -1;  // index in hookman follows, or -1

    uint32_t unlocked_abilities = 0; // bit 0: Nitro, 1: SST, 2: BO, 3: 2WD, 4: IAC
    uint32_t unlocked_vehicles_mask = 0;
    uint32_t unlocked_cities_mask = 1; // bit 0: LA, bit 1: Paris, bit 2: Tokyo

    uint8_t campaign_complete = 0;
    uint8_t career_state = 0; // CareerState enum
    char pending_movie[64] = {0};
    char pending_audio[64] = {0};
    uint32_t follows_beaten_mask = 0; // all campaign follow events (version 2)
};
#pragma pack(pop)

class CareerStateMachine {
public:
    CareerStateMachine();

    // Lifecycle / Progression
    void StartNewCampaign();
    bool ResumeFromSave(const std::string& path);
    bool SaveToFile(const std::string& path);

    // Pure Transitions
    void TransitionTo(CareerState next_state, const char* reason, int result_code = -1);

    // Queries
    CareerState GetState() const { return current_state_; }
    CityId GetCurrentCity() const { return static_cast<CityId>(save_.current_city); }
    int GetCurrentCityIndex() const { return save_.current_city; }
    int GetCurrentHookmanIndex() const { return save_.current_hookman_idx; }
    int GetCurrentRaceIndex() const { return save_.current_race_idx; }
    const HookmanDef* GetCurrentHookman() const;
    const RaceDef* GetCurrentRace() const;
    const FollowDef* GetCurrentFollow() const;
    const char* GetPendingMovie() const { return save_.pending_movie; }
    const char* GetPendingAudio() const { return save_.pending_audio; }
    uint32_t GetSaveRev() const { return save_.save_rev; }
    void SetCurrentCity(CityId city) { save_.current_city = static_cast<int32_t>(city); }
    int GetSelectedCar() const { return save_.selected_mc3_car; }
    void SetSelectedCar(int car_idx) { save_.selected_mc3_car = car_idx; }
    bool IsCampaignComplete() const { return save_.campaign_complete != 0; }
    void SelectStage(int stage_idx);

    // Evaluator
    bool IsHookmanUnlocked(int global_hookman_idx) const;
    bool IsHookmanUnlocked(std::string_view name) const;
    bool IsCityUnlocked(CityId city) const;
    bool IsRaceBeaten(std::string_view race_name) const;
    bool IsHookmanBeaten(int global_hookman_idx) const;
    bool IsHookmanBeaten(std::string_view name) const;
    bool IsAbilityUnlocked(RewardType ability) const;
    bool IsVehicleUnlocked(std::string_view mc2_veh) const;
    void EvaluateAllUnlocks();

    // Event Actions
    void OnPreCityMovieDone();
    void OnCruiseEntered();
    bool OnHookmanProximityDetected(int global_hookman_idx);
    bool OnHookmanHeadlightEngaged(int global_hookman_idx);
    void OnFollowStarted(int follow_idx);
    void OnFollowFinishedSuccess();
    void OnFollowLost(const char* reason = "out_of_range");
    void OnRaceSelected(int global_hookman_idx, int race_idx_in_hookman);
    void OnPreRaceMovieDone();
    void OnRaceStarted();
    void OnRaceResult(int result_code); // 0=Win, 1=Lost, 2=Cancelled, 3=Retry
    void OnPostRaceMovieDone();
    void OnRewardDismissed();
    void OnCityTransitionRequested(CityId target_city);

    // Logging helper
    void LogTransition(CareerState old_state, CareerState new_state, const char* reason, int result_code);

    // Hook for callbacks
    using StateChangeCallback = std::function<void(CareerState, CareerState, const CareerStateMachine&)>;
    void SetStateChangeCallback(StateChangeCallback cb) { on_state_change_ = cb; }

    const CareerSaveData& GetSaveData() const { return save_; }
    void SetSaveData(const CareerSaveData& data) { save_ = data; current_state_ = static_cast<CareerState>(data.career_state); }

private:
    uint32_t ComputeChecksum(const CareerSaveData& data);
    int GetGlobalRaceIndex(const RaceDef* race) const;
    int GetGlobalFollowIndex(const FollowDef* follow) const;
    bool IsConditionMet(const UnlockCondition& condition) const;
    bool IsRuleMet(const UnlockRule& rule) const;
    void SetAudioContext(const char* context);

    CareerState current_state_ = CareerState::NewCampaign;
    CareerSaveData save_;
    StateChangeCallback on_state_change_;
};

// Global instance accessor
CareerStateMachine& GetCareerState();

} // namespace mc2
