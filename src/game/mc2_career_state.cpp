#include "mc2_career_state.h"
#include "core/log.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <cctype>
#include <cstddef>

namespace mc2 {

#pragma pack(push, 1)
struct CareerSaveDataV1 {
    uint32_t magic;
    uint32_t version;
    uint32_t checksum;
    uint32_t save_rev;
    int32_t current_city;
    int32_t career_iteration;
    int32_t selected_mc3_car;
    uint64_t races_beaten_mask;
    uint32_t hookmen_beaten_mask;
    uint8_t hookman_race_idx[24];
    int32_t current_hookman_idx;
    int32_t current_race_idx;
    int32_t current_follow_idx;
    uint32_t unlocked_abilities;
    uint32_t unlocked_vehicles_mask;
    uint32_t unlocked_cities_mask;
    uint8_t campaign_complete;
    uint8_t career_state;
    char pending_movie[64];
    char pending_audio[64];
};
#pragma pack(pop)

static uint32_t ComputeChecksumBytes(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t sum = 0x811C9DC5u;
    for (size_t i = 12; i < size; ++i) {
        sum ^= bytes[i];
        sum *= 0x01000193u;
    }
    return sum;
}

static_assert(offsetof(CareerSaveData, follows_beaten_mask) == sizeof(CareerSaveDataV1),
              "Career save v2 must append fields after the complete v1 layout");

static bool StrEqualsCi(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    }
    return true;
}

const char* ToString(CareerState state) {
    switch (state) {
        case CareerState::NewCampaign:          return "new_campaign";
        case CareerState::PreCityMovie:         return "pre_city_movie";
        case CareerState::CareerCruise:         return "career_cruise";
        case CareerState::HookmanAvailable:     return "hookman_available";
        case CareerState::HookmanEngaged:       return "hookman_engaged";
        case CareerState::FollowActive:         return "follow_active";
        case CareerState::FollowFailed:         return "follow_failed";
        case CareerState::RaceSelected:         return "race_selected";
        case CareerState::PreRaceMovie:         return "pre_race_movie";
        case CareerState::RaceActive:           return "race_active";
        case CareerState::RaceLostOrCancelled:  return "race_lost_or_cancelled";
        case CareerState::RaceWon:              return "race_won";
        case CareerState::PostRaceMovie:        return "post_race_movie";
        case CareerState::RewardPending:        return "reward_pending";
        case CareerState::CityTransition:       return "city_transition";
        case CareerState::CampaignComplete:     return "campaign_complete";
        default:                                return "unknown";
    }
}

CareerStateMachine::CareerStateMachine() {
    StartNewCampaign();
}

uint32_t CareerStateMachine::ComputeChecksum(const CareerSaveData& data) {
    return ComputeChecksumBytes(&data, sizeof(data));
}

int CareerStateMachine::GetGlobalRaceIndex(const RaceDef* race) const {
    if (!race) return -1;
    int idx = 0;
    for (int h = 0; h < GetTotalHookmenCount(); ++h) {
        const auto* hm = GetHookmanByGlobalIndex(h);
        if (!hm) continue;
        for (int r = 0; r < hm->race_count; ++r) {
            if (&hm->races[r] == race || StrEqualsCi(hm->races[r].name, race->name)) {
                return idx;
            }
            idx++;
        }
    }
    return -1;
}

int CareerStateMachine::GetGlobalFollowIndex(const FollowDef* follow) const {
    if (!follow) return -1;
    int idx = 0;
    for (int h = 0; h < GetTotalHookmenCount(); ++h) {
        const auto* hm = GetHookmanByGlobalIndex(h);
        if (!hm) continue;
        for (int f = 0; f < hm->follow_count; ++f) {
            if (&hm->follows[f] == follow || StrEqualsCi(hm->follows[f].follow_race, follow->follow_race)) {
                return idx;
            }
            idx++;
        }
    }
    return -1;
}

const HookmanDef* CareerStateMachine::GetCurrentHookman() const {
    if (save_.current_hookman_idx < 0 || save_.current_hookman_idx >= GetTotalHookmenCount()) {
        return nullptr;
    }
    return GetHookmanByGlobalIndex(save_.current_hookman_idx);
}

const RaceDef* CareerStateMachine::GetCurrentRace() const {
    const auto* hm = GetCurrentHookman();
    if (!hm || save_.current_race_idx < 0 || save_.current_race_idx >= hm->race_count) {
        return nullptr;
    }
    return &hm->races[save_.current_race_idx];
}

const FollowDef* CareerStateMachine::GetCurrentFollow() const {
    const auto* hm = GetCurrentHookman();
    if (!hm || save_.current_follow_idx < 0 || save_.current_follow_idx >= hm->follow_count) {
        return nullptr;
    }
    return &hm->follows[save_.current_follow_idx];
}

void CareerStateMachine::SetAudioContext(const char* context) {
    if (!context) {
        save_.pending_audio[0] = '\0';
        return;
    }
    const auto* hm = GetCurrentHookman();
    const char* hm_name = hm ? hm->name : "moses";
    snprintf(save_.pending_audio, sizeof(save_.pending_audio), "%s\\%s", hm_name, context);
}

void CareerStateMachine::LogTransition(CareerState old_state, CareerState new_state, const char* reason, int result_code) {
    const char* race_key = "none";
    const auto* r = GetCurrentRace();
    if (r) race_key = r->name;

    const char* movie_key = save_.pending_movie[0] ? save_.pending_movie : "none";
    const char* audio_key = save_.pending_audio[0] ? save_.pending_audio : "none";

    LOGE("MC2CAREER state=%s->%s city=%d hookman=%d race=%s result=%d reason=%s movie=%s audio=%s save_rev=%u",
         ToString(old_state), ToString(new_state),
         save_.current_city, save_.current_hookman_idx,
         race_key, result_code, reason ? reason : "none",
         movie_key, audio_key, save_.save_rev);
}

void CareerStateMachine::TransitionTo(CareerState next_state, const char* reason, int result_code) {
    CareerState old_state = current_state_;
    current_state_ = next_state;
    save_.career_state = static_cast<uint8_t>(next_state);
    save_.save_rev++;
    save_.checksum = ComputeChecksum(save_);

    LogTransition(old_state, next_state, reason, result_code);

    if (on_state_change_) {
        on_state_change_(old_state, next_state, *this);
    }
}

void CareerStateMachine::StartNewCampaign() {
    save_ = CareerSaveData{};
    current_state_ = CareerState::NewCampaign;
    save_.current_city = 0; // LA
    save_.unlocked_cities_mask = 1; // LA unlocked
    save_.selected_mc3_car = 0; // Default starter vehicle
    EvaluateAllUnlocks();

    const auto* pre_movie = GetPreCityMovie(CityId::LosAngeles);
    if (pre_movie) {
        strncpy(save_.pending_movie, pre_movie->bik_file, sizeof(save_.pending_movie) - 1);
        SetAudioContext("pre\\pre_find_hook");
        TransitionTo(CareerState::PreCityMovie, "new_campaign_intro");
    } else {
        TransitionTo(CareerState::CareerCruise, "new_campaign_direct");
    }
}

bool CareerStateMachine::ResumeFromSave(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    uint32_t header[3]{};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!file || header[0] != 0x4D433243 || (header[1] != 1 && header[1] != 2)) {
        LOGE("MC2 Career: invalid save header or version in %s", path.c_str());
        return false;
    }
    file.clear();
    file.seekg(0);

    CareerSaveData loaded{};
    bool migrated = false;
    if (header[1] == 1) {
        CareerSaveDataV1 old{};
        file.read(reinterpret_cast<char*>(&old), sizeof(old));
        if (!file || old.checksum != ComputeChecksumBytes(&old, sizeof(old))) {
            LOGE("MC2 Career: v1 save checksum mismatch in %s", path.c_str());
            return false;
        }
        std::memcpy(&loaded, &old, sizeof(old));
        loaded.version = 2;
        loaded.follows_beaten_mask = 0;
        loaded.checksum = ComputeChecksum(loaded);
        migrated = true;
        LOGE("MC2 Career: migrated v1 save to v2 (follow results start empty)");
    } else {
        file.read(reinterpret_cast<char*>(&loaded), sizeof(loaded));
        if (!file || loaded.checksum != ComputeChecksum(loaded)) {
            LOGE("MC2 Career: v2 save checksum mismatch in %s", path.c_str());
            return false;
        }
    }

    if (loaded.current_city < 0 || loaded.current_city >= static_cast<int>(CityId::Count) ||
        loaded.current_hookman_idx < -1 || loaded.current_hookman_idx >= GetTotalHookmenCount() ||
        loaded.current_race_idx < -1 || loaded.current_follow_idx < -1 ||
        loaded.career_state > static_cast<uint8_t>(CareerState::CampaignComplete)) {
        LOGE("MC2 Career: invalid field range in %s", path.c_str());
        return false;
    }
    if (loaded.current_hookman_idx >= 0) {
        const auto* hm = GetHookmanByGlobalIndex(loaded.current_hookman_idx);
        if (!hm || loaded.current_race_idx >= hm->race_count || loaded.current_follow_idx >= hm->follow_count) {
            LOGE("MC2 Career: invalid active Hookman indices in %s", path.c_str());
            return false;
        }
    } else if (loaded.current_race_idx != -1 || loaded.current_follow_idx != -1) {
        LOGE("MC2 Career: orphan active race/follow indices in %s", path.c_str());
        return false;
    }

    save_ = loaded;
    current_state_ = static_cast<CareerState>(save_.career_state);
    EvaluateAllUnlocks();
    LOGE("MC2 Career: resumed from save revision %u, city=%d, state=%s",
         save_.save_rev, save_.current_city, ToString(current_state_));
    if (migrated && !SaveToFile(path)) {
        LOGE("MC2 Career: could not persist migrated v2 save to %s", path.c_str());
    }
    return true;
}

bool CareerStateMachine::SaveToFile(const std::string& path) {
    save_.checksum = ComputeChecksum(save_);
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(&save_), sizeof(save_));
    return file.good();
}

bool CareerStateMachine::IsHookmanBeaten(int global_hookman_idx) const {
    if (global_hookman_idx < 0 || global_hookman_idx >= 24) return false;
    return (save_.hookmen_beaten_mask & (1u << global_hookman_idx)) != 0;
}

bool CareerStateMachine::IsHookmanBeaten(std::string_view name) const {
    const auto* hm = FindHookman(name);
    if (!hm) return false;
    return IsHookmanBeaten(hm->global_hookman_idx);
}

bool CareerStateMachine::IsRaceBeaten(std::string_view race_name) const {
    const auto* r = FindRace(race_name);
    if (r) {
        int g_idx = GetGlobalRaceIndex(r);
        if (g_idx >= 0 && g_idx < 64) {
            return (save_.races_beaten_mask & (1ULL << g_idx)) != 0;
        }
    }
    const auto* follow = FindFollow(race_name);
    if (follow) {
        int f_idx = GetGlobalFollowIndex(follow);
        if (f_idx >= 0 && f_idx < 32) {
            return (save_.follows_beaten_mask & (1u << f_idx)) != 0;
        }
    }
    return false;
}

bool CareerStateMachine::IsCityUnlocked(CityId city) const {
    int c_idx = static_cast<int>(city);
    return (save_.unlocked_cities_mask & (1u << c_idx)) != 0;
}

bool CareerStateMachine::IsAbilityUnlocked(RewardType ability) const {
    switch (ability) {
        case RewardType::Nitro:           return (save_.unlocked_abilities & (1u << 0)) != 0;
        case RewardType::SlipstreamTurbo: return (save_.unlocked_abilities & (1u << 1)) != 0;
        case RewardType::Burnout:         return (save_.unlocked_abilities & (1u << 2)) != 0;
        case RewardType::TwoWheelDrive:   return (save_.unlocked_abilities & (1u << 3)) != 0;
        case RewardType::InAirControl:    return (save_.unlocked_abilities & (1u << 4)) != 0;
        default:                          return false;
    }
}

bool CareerStateMachine::IsVehicleUnlocked(std::string_view mc2_veh) const {
    int idx = 0;
    for (const auto& vr : GetUnlockRules()) {
        if (vr.reward_type == RewardType::Vehicle && StrEqualsCi(vr.reward_id, mc2_veh)) {
            return idx < 32 && (save_.unlocked_vehicles_mask & (1u << idx)) != 0;
        }
        if (vr.reward_type == RewardType::Vehicle) idx++;
    }
    return false;
}

bool CareerStateMachine::IsConditionMet(const UnlockCondition& condition) const {
    switch (condition.type) {
        case ConditionType::RaceBeaten:
            return IsRaceBeaten(condition.item_id);
        case ConditionType::HookmanBeaten:
        case ConditionType::HookmanBeatenIter:
            // The imported MC2 table targets iteration zero. A later prestige loop can
            // split this into per-iteration masks without changing the rule evaluator.
            return IsHookmanBeaten(condition.item_id);
        case ConditionType::CityAvailable: {
            CityId city = CityId::LosAngeles;
            if (StrEqualsCi(condition.item_id, "paris")) city = CityId::Paris;
            else if (StrEqualsCi(condition.item_id, "tokyo")) city = CityId::Tokyo;
            return IsCityUnlocked(city);
        }
        case ConditionType::InRace: {
            const auto* race = GetCurrentRace();
            return race && StrEqualsCi(race->name, condition.item_id);
        }
        case ConditionType::VehicleAvailable:
            return IsVehicleUnlocked(condition.item_id);
        default:
            return false;
    }
}

bool CareerStateMachine::IsRuleMet(const UnlockRule& rule) const {
    for (int i = 0; i < rule.condition_count; ++i) {
        if (!IsConditionMet(rule.conditions[i])) return false;
    }
    return true;
}

bool CareerStateMachine::IsHookmanUnlocked(int global_hookman_idx) const {
    const auto* hm = GetHookmanByGlobalIndex(global_hookman_idx);
    if (!hm) return false;

    // Moses is always unlocked at the start of LA
    if (StrEqualsCi(hm->name, "Moses")) return true;

    // Evaluate rules for this Hookman (OR between lines, AND within line)
    bool has_rule = false;
    for (const auto& rule : GetUnlockRules()) {
        if (rule.reward_type == RewardType::Hookman && StrEqualsCi(rule.reward_id, hm->name)) {
            has_rule = true;
            if (IsRuleMet(rule)) return true;
        }
    }

    // If no rules declared, item is unlocked by default
    return !has_rule;
}

bool CareerStateMachine::IsHookmanUnlocked(std::string_view name) const {
    const auto* hm = FindHookman(name);
    if (!hm) return false;
    return IsHookmanUnlocked(hm->global_hookman_idx);
}

void CareerStateMachine::EvaluateAllUnlocks() {
    // Some rewards depend on earlier rewards in the same CSV (notably the final
    // SLF450X vehicle). Iterate to a fixed point so table order is not semantic.
    for (int pass = 0; pass < 4; ++pass) {
        const uint32_t old_cities = save_.unlocked_cities_mask;
        const uint32_t old_vehicles = save_.unlocked_vehicles_mask;
        const uint32_t old_abilities = save_.unlocked_abilities;
        int vehicle_idx = 0;

        for (const auto& rule : GetUnlockRules()) {
            const int this_vehicle_idx = rule.reward_type == RewardType::Vehicle ? vehicle_idx++ : -1;
            if (!IsRuleMet(rule)) continue;

            switch (rule.reward_type) {
                case RewardType::City:
                    if (StrEqualsCi(rule.reward_id, "losangeles")) save_.unlocked_cities_mask |= 1u << 0;
                    else if (StrEqualsCi(rule.reward_id, "paris")) save_.unlocked_cities_mask |= 1u << 1;
                    else if (StrEqualsCi(rule.reward_id, "tokyo")) save_.unlocked_cities_mask |= 1u << 2;
                    break;
                case RewardType::Vehicle:
                    if (this_vehicle_idx >= 0 && this_vehicle_idx < 32) {
                        save_.unlocked_vehicles_mask |= 1u << this_vehicle_idx;
                    }
                    break;
                case RewardType::Nitro:           save_.unlocked_abilities |= 1u << 0; break;
                case RewardType::SlipstreamTurbo: save_.unlocked_abilities |= 1u << 1; break;
                case RewardType::Burnout:         save_.unlocked_abilities |= 1u << 2; break;
                case RewardType::TwoWheelDrive:   save_.unlocked_abilities |= 1u << 3; break;
                case RewardType::InAirControl:    save_.unlocked_abilities |= 1u << 4; break;
                case RewardType::Hookman:         break; // queried directly through the same rule table
            }
        }

        if (old_cities == save_.unlocked_cities_mask &&
            old_vehicles == save_.unlocked_vehicles_mask &&
            old_abilities == save_.unlocked_abilities) {
            break;
        }
    }

    // Check Campaign Complete (WorldChampLA beaten)
    if (IsHookmanBeaten("WorldChampLA")) {
        save_.campaign_complete = 1;
    }
}

void CareerStateMachine::OnPreCityMovieDone() {
    save_.pending_movie[0] = '\0';
    TransitionTo(CareerState::CareerCruise, "pre_city_movie_complete");
}

void CareerStateMachine::OnCruiseEntered() {
    save_.current_hookman_idx = -1;
    save_.current_race_idx = -1;
    save_.current_follow_idx = -1;
    save_.pending_movie[0] = '\0';
    SetAudioContext("career_music");
    TransitionTo(CareerState::CareerCruise, "cruise_active");
}

void CareerStateMachine::SelectStage(int stage_idx) {
    if (stage_idx <= 0) {
        OnCruiseEntered();
        return;
    }
    CityId city = GetCurrentCity();
    int race_counter = 1;
    for (int h = 0; h < 8; ++h) {
        const auto* hm = GetHookman(city, h);
        if (!hm) continue;
        for (int ri = 0; ri < hm->race_count; ++ri) {
            if (hm->races[ri].is_supported) {
                if (race_counter == stage_idx) {
                    OnRaceSelected(hm->global_hookman_idx, ri);
                    return;
                }
                race_counter++;
            }
        }
    }
}

bool CareerStateMachine::OnHookmanProximityDetected(int global_hookman_idx) {
    if (!IsHookmanUnlocked(global_hookman_idx) || IsHookmanBeaten(global_hookman_idx)) {
        return false;
    }
    save_.current_hookman_idx = global_hookman_idx;
    SetAudioContext("pre\\pre_find_hook");
    TransitionTo(CareerState::HookmanAvailable, "hookman_nearby");
    return true;
}

bool CareerStateMachine::OnHookmanHeadlightEngaged(int global_hookman_idx) {
    if (save_.current_hookman_idx != global_hookman_idx) {
        if (!OnHookmanProximityDetected(global_hookman_idx)) return false;
    }
    SetAudioContext("follow\\dur_start_follow");
    TransitionTo(CareerState::HookmanEngaged, "headlights_flashed");
    return true;
}

void CareerStateMachine::OnFollowStarted(int follow_idx) {
    save_.current_follow_idx = follow_idx;
    SetAudioContext("follow\\dur_follow");
    TransitionTo(CareerState::FollowActive, "follow_begun");
}

void CareerStateMachine::OnFollowFinishedSuccess() {
    const auto* hm = GetCurrentHookman();
    if (!hm) return;

    if (save_.current_follow_idx >= 0 && save_.current_follow_idx < hm->follow_count) {
        const auto* follow = &hm->follows[save_.current_follow_idx];
        int follow_idx = GetGlobalFollowIndex(follow);
        if (follow_idx >= 0 && follow_idx < 32) {
            save_.follows_beaten_mask |= 1u << follow_idx;
        }
        EvaluateAllUnlocks();
        int target_idx = hm->follows[save_.current_follow_idx].target_race_index;
        if (target_idx >= 0 && target_idx < hm->race_count) {
            OnRaceSelected(hm->global_hookman_idx, target_idx);
            return;
        }
    }
    // Default to current race for hookman
    int next_r = save_.hookman_race_idx[hm->global_hookman_idx];
    OnRaceSelected(hm->global_hookman_idx, next_r);
}

void CareerStateMachine::OnFollowLost(const char* reason) {
    TransitionTo(CareerState::FollowFailed, reason ? reason : "follow_lost");
    // Return to cruise
    OnCruiseEntered();
}

void CareerStateMachine::OnRaceSelected(int global_hookman_idx, int race_idx_in_hookman) {
    save_.current_hookman_idx = global_hookman_idx;
    save_.current_race_idx = race_idx_in_hookman;

    const auto* race = GetCurrentRace();
    if (!race) return;

    // Check pre-race movie
    const auto* pre_m = FindMovie(MovieType::PreRace, race->name);
    if (pre_m) {
        strncpy(save_.pending_movie, pre_m->bik_file, sizeof(save_.pending_movie) - 1);
        SetAudioContext("pre");
        TransitionTo(CareerState::PreRaceMovie, "pre_race_movie_pending");
    } else {
        SetAudioContext("dur");
        TransitionTo(CareerState::RaceSelected, "race_ready");
    }
}

void CareerStateMachine::OnPreRaceMovieDone() {
    save_.pending_movie[0] = '\0';
    SetAudioContext("dur");
    TransitionTo(CareerState::RaceSelected, "pre_race_movie_complete");
}

void CareerStateMachine::OnRaceStarted() {
    TransitionTo(CareerState::RaceActive, "countdown_finished");
}

void CareerStateMachine::OnRaceResult(int result_code) {
    if (result_code != 0) {
        // Defeat / Cancelled / Retry
        // Race is NOT marked as beaten, progression does not advance!
        SetAudioContext("end");
        TransitionTo(CareerState::RaceLostOrCancelled, "race_failed_or_aborted", result_code);
        return;
    }

    // Result 0: WIN!
    const auto* r = GetCurrentRace();
    const auto* hm = GetCurrentHookman();
    if (!r || !hm) return;

    int g_race_idx = GetGlobalRaceIndex(r);
    if (g_race_idx >= 0 && g_race_idx < 64) {
        save_.races_beaten_mask |= (1ULL << g_race_idx);
    }

    // Advance this Hookman's race index
    int next_idx = save_.hookman_race_idx[hm->global_hookman_idx] + 1;
    save_.hookman_race_idx[hm->global_hookman_idx] = static_cast<uint8_t>(next_idx);

    if (next_idx >= hm->race_count) {
        save_.hookmen_beaten_mask |= (1u << hm->global_hookman_idx);
    }

    EvaluateAllUnlocks();
    SetAudioContext("end_win");

    // Check post-race movie
    // If other unlocked hookmen remain in current city, alt_post_race is preferred
    bool has_remaining_hookmen = false;
    int city_base = save_.current_city * 8;
    for (int i = 0; i < 8; ++i) {
        int check_idx = city_base + i;
        if (check_idx != hm->global_hookman_idx && IsHookmanUnlocked(check_idx) && !IsHookmanBeaten(check_idx)) {
            has_remaining_hookmen = true;
            break;
        }
    }

    const MovieDef* post_m = nullptr;
    if (has_remaining_hookmen) {
        post_m = FindMovie(MovieType::AltPostRace, r->name);
    }
    if (!post_m) {
        post_m = FindMovie(MovieType::PostRace, r->name);
    }

    if (post_m) {
        strncpy(save_.pending_movie, post_m->bik_file, sizeof(save_.pending_movie) - 1);
        TransitionTo(CareerState::PostRaceMovie, "post_race_movie_pending", 0);
    } else {
        TransitionTo(CareerState::RaceWon, "race_won_no_movie", 0);
    }
}

void CareerStateMachine::OnPostRaceMovieDone() {
    save_.pending_movie[0] = '\0';
    TransitionTo(CareerState::RewardPending, "movie_finished");
}

void CareerStateMachine::OnRewardDismissed() {
    if (save_.campaign_complete) {
        TransitionTo(CareerState::CampaignComplete, "worldchamp_beaten");
        return;
    }

    // Check if current city is cleared
    int city_base = save_.current_city * 8;
    bool city_cleared = true;
    for (int i = 0; i < 8; ++i) {
        if (!IsHookmanBeaten(city_base + i)) {
            city_cleared = false;
            break;
        }
    }

    if (city_cleared && save_.current_city < 2) {
        CityId next_city = static_cast<CityId>(save_.current_city + 1);
        if (IsCityUnlocked(next_city)) {
            OnCityTransitionRequested(next_city);
            return;
        }
    }

    OnCruiseEntered();
}

void CareerStateMachine::OnCityTransitionRequested(CityId target_city) {
    save_.current_city = static_cast<int>(target_city);
    save_.current_hookman_idx = -1;
    save_.current_race_idx = -1;
    save_.current_follow_idx = -1;

    TransitionTo(CareerState::CityTransition, "city_transition");

    // Return to cruise in new city
    OnCruiseEntered();
}

CareerStateMachine& GetCareerState() {
    static CareerStateMachine s_instance;
    return s_instance;
}

} // namespace mc2
