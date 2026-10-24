#pragma once
#include <cstdint>
#include <cstddef>
#include <span>
#include <string_view>

namespace mc2 {

enum class CityId : int {
    LosAngeles = 0,
    Paris = 1,
    Tokyo = 2,
    Count = 3
};

enum class MovieType : int {
    PreCity,
    PreRace,
    PostRace,
    AltPostRace
};

enum class ConditionType : int {
    RaceBeaten,         // R
    HookmanBeaten,      // H
    HookmanBeatenIter,  // HTCIO
    CityAvailable,      // C
    InRace,             // IR
    VehicleAvailable    // V
};

enum class RewardType : int {
    Vehicle,            // V
    Hookman,            // H
    City,               // C
    Nitro,              // NITRO
    SlipstreamTurbo,    // SST / TEMP_SST
    Burnout,            // BO / TEMP_BO
    TwoWheelDrive,      // 2WD / TEMP_2WD
    InAirControl,       // IAC / TEMP_IAC
};

struct RaceDef {
    const char* name;
    const char* type;
    const char* tod;
    const char* weather;
    float ambient_density;
    float ped_density;
    const char* forced_vehicle; // nullptr if none
    bool is_supported;          // supported by current MC3 race converter
    int city_idx;
    int hookman_idx;
    int race_index_in_hookman;
};

struct FollowDef {
    const char* follow_race;
    const char* target_race;
    int target_race_index;
};

struct HookmanDef {
    const char* name;
    int city_idx;
    int global_hookman_idx;
    const char* cruise_route;
    int race_count;
    const RaceDef* races;
    int follow_count;
    const FollowDef* follows;
};

struct MovieDef {
    MovieType type;
    const char* target_race;    // nullptr for PreCity
    const char* pss_file;
    const char* bik_file;       // relative path to video folder
};

struct UnlockCondition {
    ConditionType type;
    const char* item_id;
};

struct UnlockRule {
    RewardType reward_type;
    const char* reward_id;
    bool is_temporary;
    int condition_count;
    const UnlockCondition* conditions;
};

struct VehicleRewardMapping {
    const char* mc2_veh;
    const char* mc3_veh;
    int tier;
    const char* veh_class;
    const char* display_name;
};

struct Mc3CarDef {
    const char16_t* label;
    const char* name;
    uint32_t record_va;
};

extern const Mc3CarDef kMc3Cars[10];

struct HookmanVehicleDef {
    const char* hookman_name;
    const char* mc2_car;
    const char* mc3_car_name;
    uint32_t mc3_record_va;
};

// Career Data API
int GetTotalHookmenCount();
int GetHookmenCount(CityId city);
const HookmanDef* GetHookman(CityId city, int index);
const HookmanDef* GetHookmanByGlobalIndex(int global_idx);
const HookmanDef* FindHookman(std::string_view name);

const HookmanVehicleDef* GetHookmanVehicleDef(std::string_view hookman_name);
uint32_t GetHookmanVehicleRecord(std::string_view hookman_name);
const char* GetHookmanVehicleName(std::string_view hookman_name);

int GetTotalRacesCount();
const RaceDef* FindRace(std::string_view name);
const FollowDef* FindFollow(std::string_view follow_race_name);

int GetTotalMoviesCount();
const MovieDef* FindMovie(MovieType type, std::string_view target_race);
const MovieDef* GetPreCityMovie(CityId city);

std::span<const UnlockRule> GetUnlockRules();
const VehicleRewardMapping* FindVehicleReward(std::string_view mc2_veh);

} // namespace mc2
