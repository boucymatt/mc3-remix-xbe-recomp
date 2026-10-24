#include "mc2_career_data.h"
#include <cstring>
#include <algorithm>
#include <string>

namespace mc2 {

static constexpr VehicleRewardMapping kVehicleRewards[] = {
    {"vp_civica", "vp_eclipse_04", 1, "tuner", "1998 Dodge Neon / 2004 Eclipse"},
    {"vp_jetta", "vp_jetta_03", 1, "sedan", "VW Jetta / 1995 Golf"},
    {"vp_integraa", "vp_golfr32_04", 1, "tuner", "1995 VW Golf R32"},
    {"vp_civicb", "vp_lancer_04", 2, "tuner", "2004 Mitsubishi Lancer Evo"},
    {"vp_s2000", "vp_elise_04", 2, "sport", "Lotus Elise / 350Z"},
    {"vp_gto", "vp_gto_70", 2, "muscle", "1970 Pontiac GTO / El Camino"},
    {"vp_rx7", "vp_supra_98", 2, "tuner", "1998 Toyota Supra"},
    {"vp_viper", "vp_viper_03", 3, "exotic", "2003 Dodge Viper / Corvette"},
    {"vp_cbr929a", "vp_ninja_03", 2, "motorcycle", "2004 Ducati 999R"},
    {"vp_puma", "vp_golfr32_04", 2, "tuner", "VW Golf R32"},
    {"vp_cosworth", "vp_lancer_04", 2, "tuner", "2004 Mitsubishi Lancer Evo"},
    {"vp_m5", "vp_is300_04", 2, "sedan", "Lexus IS300 / 350Z"},
    {"vp_elise", "vp_elise_04", 2, "sport", "Lotus Elise"},
    {"vp_golfa", "vp_golfr32_04", 2, "tuner", "VW Golf R32"},
    {"vp_clio", "vp_eclipse_04", 2, "tuner", "2004 Mitsubishi Eclipse"},
    {"vp_911", "vp_corvette_03", 3, "exotic", "2003 Corvette Z06"},
    {"vp_ducati", "vp_ninja_03", 3, "motorcycle", "2004 Ducati 999R"},
    {"vp_vanquish", "vp_corvette_03", 3, "exotic", "Corvette Z06 / Viper"},
    {"vp_pariscop", "vp_impalass_cop_s_96", 2, "cop", "Police Impala"},
    {"vp_mr2", "vp_elise_04", 3, "sport", "Lotus Elise / 350Z"},
    {"vp_nissanz", "vp_350z_04", 3, "sport", "2004 Nissan 350Z"},
    {"vp_supraa", "vp_supra_98", 3, "tuner", "1998 Toyota Supra"},
    {"vp_suprab", "vp_supra_98", 3, "tuner", "1998 Toyota Supra Spec B"},
    {"vp_lexus", "vp_is300_04", 3, "sedan", "Lexus IS300"},
    {"vp_yamaha", "vp_ninja_03", 3, "motorcycle", "2004 Ducati 999R"},
    {"vp_lancer", "vp_lancer_04", 3, "tuner", "2004 Mitsubishi Lancer Evo"},
    {"vp_skylinea", "vp_skyline_02", 4, "tuner", "2002 Nissan Skyline GT-R"},
    {"vp_tokyocop", "vp_d_tokyo_cop_01", 3, "cop", "Tokyo Police"},
    {"vp_cop_l", "vp_impalass_cop_s_96", 2, "cop", "LA Police Impala"},
    {"vp_saleen", "vp_saleens7_04", 4, "exotic", "2004 Saleen S7"},
    {"vp_slf450x", "vp_saleens7_04", 4, "special", "2004 Saleen S7 (Special)"},
};

static constexpr RaceDef k_losangeles_Moses_races[] = {
    {"lamauro_Moses_Checkpoint_One", "cp_ordered", "midnight", "clear", 1.0f, 10.0f, nullptr, true, 0, 0, 0},
    {"lamauro_Moses_Checkpoint_Two", "cp_ordered", "dusk", "clear", 1.0f, 10.0f, nullptr, true, 0, 0, 1},
};
static constexpr FollowDef k_losangeles_Moses_follows[] = {
    {"lamauro_Moses_Follow", "lamauro_Moses_Checkpoint_One", 0},
};
static constexpr HookmanDef k_losangeles_Moses_def = {
    "Moses", 0, 0, nullptr,
    int(sizeof(k_losangeles_Moses_races)/sizeof(k_losangeles_Moses_races[0])), k_losangeles_Moses_races,
    int(sizeof(k_losangeles_Moses_follows)/sizeof(k_losangeles_Moses_follows[0])), k_losangeles_Moses_follows
};

static constexpr RaceDef k_losangeles_Steven_races[] = {
    {"lamauro_Steven_Checkpoint_One", "cp_ordered_time_global", "midnight", "clear", 1.0f, 5.0f, nullptr, true, 0, 1, 0},
    {"lachrisbeta_Steven_Checkpoint_Two", "cp_unordered", "midnight", "clear", 1.0f, 15.0f, nullptr, true, 0, 1, 1},
};
static constexpr FollowDef k_losangeles_Steven_follows[] = {
    {"lamaurobeta_Steven_Follow", "lamauro_Steven_Checkpoint_One", 0},
    {"lamaurobeta_Steven_Follow_Two", "lachrisbeta_Steven_Checkpoint_Two", 1},
};
static constexpr HookmanDef k_losangeles_Steven_def = {
    "Steven", 0, 1, "lamauro_Steven_PatrolRoute",
    int(sizeof(k_losangeles_Steven_races)/sizeof(k_losangeles_Steven_races[0])), k_losangeles_Steven_races,
    int(sizeof(k_losangeles_Steven_follows)/sizeof(k_losangeles_Steven_follows[0])), k_losangeles_Steven_follows
};

static constexpr RaceDef k_losangeles_Maria_races[] = {
    {"lamauro_Maria_Checkpoint_One", "cp_ordered", "midnight", "clear", 1.0f, 10.0f, nullptr, true, 0, 2, 0},
    {"lamauro_Maria_Checkpoint_Two", "cp_ordered_time_local", "midnight", "clear", 1.0f, 5.0f, nullptr, true, 0, 2, 1},
};
static constexpr FollowDef k_losangeles_Maria_follows[] = {
    {"lamauro_Maria_Follow", "lamauro_Maria_Checkpoint_One", 0},
    {"lamauro_Maria_Follow_Two", "lamauro_Maria_Checkpoint_Two", 1},
};
static constexpr HookmanDef k_losangeles_Maria_def = {
    "Maria", 0, 2, "lamauro_Maria_PatrolRoute",
    int(sizeof(k_losangeles_Maria_races)/sizeof(k_losangeles_Maria_races[0])), k_losangeles_Maria_races,
    int(sizeof(k_losangeles_Maria_follows)/sizeof(k_losangeles_Maria_follows[0])), k_losangeles_Maria_follows
};

static constexpr RaceDef k_losangeles_Angel_races[] = {
    {"lamauro_Angel_Checkpoint_One", "cp_unordered", "midnight", "clear", 1.0f, 10.0f, nullptr, true, 0, 3, 0},
    {"lamauro_Angel_SideRace", "cp_ordered", "midnight", "clear", 1.0f, 10.0f, nullptr, true, 0, 3, 1},
    {"lamauro_Angel_Checkpoint_Two", "cp_ordered", "dawn", "cloudy", 1.0f, 0.0f, nullptr, true, 0, 3, 2},
};
static constexpr FollowDef k_losangeles_Angel_follows[] = {
    {"lamauro_Angel_Follow", "lamauro_Angel_Checkpoint_One", 0},
    {"lamauro_Angel_Follow_Two", "lamauro_Angel_SideRace", 1},
    {"lamauro_Angel_Follow_Three", "lamauro_Angel_Checkpoint_Two", 2},
};
static constexpr HookmanDef k_losangeles_Angel_def = {
    "Angel", 0, 3, "lamauro_Angel_PatrolRoute",
    int(sizeof(k_losangeles_Angel_races)/sizeof(k_losangeles_Angel_races[0])), k_losangeles_Angel_races,
    int(sizeof(k_losangeles_Angel_follows)/sizeof(k_losangeles_Angel_follows[0])), k_losangeles_Angel_follows
};

static constexpr RaceDef k_losangeles_Gina_races[] = {
    {"laChris_Gina_Checkpoint_One", "cp_ordered", "midnight", "clear", 1.0f, 10.0f, nullptr, true, 0, 4, 0},
    {"laChris_Gina_SideRace", "cp_ordered", "dusk", "clear", 1.0f, 5.0f, nullptr, true, 0, 4, 1},
    {"laChris_Gina_Bike_Tutorial", "cp_ordered_time_global", "dusk", "clear", 2.0f, 10.0f, "vp_cbr929a", true, 0, 4, 2},
    {"laChris_Gina_Checkpoint_Two", "cp_ordered", "dawn", "clear", 1.0f, 15.0f, nullptr, true, 0, 4, 3},
};
static constexpr FollowDef k_losangeles_Gina_follows[] = {
    {"laChris_Gina_Follow", "laChris_Gina_Checkpoint_One", 0},
    {"laChris_Gina_Follow_Two", "laChris_Gina_SideRace", 1},
    {"laChris_Gina_Follow_Three", "laChris_Gina_Checkpoint_Two", 3},
    {"laChris_Gina_Follow_Three", "laChris_Gina_Bike_Tutorial", 2},
};
static constexpr HookmanDef k_losangeles_Gina_def = {
    "Gina", 0, 4, "laChris_Gina_PatrolRoute",
    int(sizeof(k_losangeles_Gina_races)/sizeof(k_losangeles_Gina_races[0])), k_losangeles_Gina_races,
    int(sizeof(k_losangeles_Gina_follows)/sizeof(k_losangeles_Gina_follows[0])), k_losangeles_Gina_follows
};

static constexpr RaceDef k_losangeles_Hector_races[] = {
    {"lawing_Hector_LoseTheCops", "lose_the_cops", "midnight", "rainy", 1.0f, 10.0f, nullptr, false, 0, 5, 0},
    {"lachrisbeta_Hector_Checkpoint_Two", "cp_unordered", "midnight", "rainy", 1.0f, 10.0f, nullptr, true, 0, 5, 1},
    {"lachrisbeta_Hector_Checkpoint_Three", "cp_ordered", "dusk", "clear", 1.5f, 15.0f, nullptr, true, 0, 5, 2},
};
static constexpr FollowDef k_losangeles_Hector_follows[] = {
    {"lachrisbeta_Hector_Follow", "lawing_Hector_LoseTheCops", 0},
    {"laChris_Hector_Follow_Two", "lachrisbeta_Hector_Checkpoint_Two", 1},
    {"laChris_Hector_Follow_Three", "lachrisbeta_Hector_Checkpoint_Three", 2},
};
static constexpr HookmanDef k_losangeles_Hector_def = {
    "Hector", 0, 5, "laChris_Hector_PatrolRoute",
    int(sizeof(k_losangeles_Hector_races)/sizeof(k_losangeles_Hector_races[0])), k_losangeles_Hector_races,
    int(sizeof(k_losangeles_Hector_follows)/sizeof(k_losangeles_Hector_follows[0])), k_losangeles_Hector_follows
};

static constexpr RaceDef k_losangeles_Dice_races[] = {
    {"lamaurobeta_Dice_Checkpoint_One", "cp_ordered", "midnight", "clear", 1.5f, 10.0f, nullptr, true, 0, 6, 0},
    {"lamaurobeta_Dice_Checkpoint_Two", "cp_ordered", "dawn", "cloudy", 1.25f, 0.0f, nullptr, true, 0, 6, 1},
};
static constexpr FollowDef k_losangeles_Dice_follows[] = {
};
static constexpr HookmanDef k_losangeles_Dice_def = {
    "Dice", 0, 6, nullptr,
    int(sizeof(k_losangeles_Dice_races)/sizeof(k_losangeles_Dice_races[0])), k_losangeles_Dice_races,
    int(sizeof(k_losangeles_Dice_follows)/sizeof(k_losangeles_Dice_follows[0])), nullptr
};

static constexpr RaceDef k_losangeles_WorldChampla_races[] = {
    {"lachrisbeta_WorldChampLA_Checkpoint_One", "cp_unordered", "midnight", "rainy", 1.5f, 10.0f, nullptr, true, 0, 7, 0},
    {"laWC_WorldChampLA_Checkpoint_Two", "cp_ordered", "midnight", "clear", 2.0f, 10.0f, nullptr, true, 0, 7, 1},
};
static constexpr FollowDef k_losangeles_WorldChampla_follows[] = {
};
static constexpr HookmanDef k_losangeles_WorldChampla_def = {
    "WorldChampla", 0, 7, nullptr,
    int(sizeof(k_losangeles_WorldChampla_races)/sizeof(k_losangeles_WorldChampla_races[0])), k_losangeles_WorldChampla_races,
    int(sizeof(k_losangeles_WorldChampla_follows)/sizeof(k_losangeles_WorldChampla_follows[0])), nullptr
};

static constexpr RaceDef k_paris_Blog_races[] = {
    {"parischris_Blog_Checkpoint_One", "cp_ordered", "dusk", "clear", 1.0f, 5.0f, nullptr, true, 1, 0, 0},
    {"parischris_Blog_Checkpoint_Two", "cp_ordered", "dusk", "clear", 1.2f, 0.0f, nullptr, true, 1, 0, 1},
};
static constexpr FollowDef k_paris_Blog_follows[] = {
    {"parischris_Blog_Follow", "parischris_Blog_Checkpoint_One", 0},
    {"parischris_Blog_Follow_Two", "parischris_Blog_Checkpoint_Two", 1},
};
static constexpr HookmanDef k_paris_Blog_def = {
    "Blog", 1, 8, "parischris_Blog_PatrolRoute",
    int(sizeof(k_paris_Blog_races)/sizeof(k_paris_Blog_races[0])), k_paris_Blog_races,
    int(sizeof(k_paris_Blog_follows)/sizeof(k_paris_Blog_follows[0])), k_paris_Blog_follows
};

static constexpr RaceDef k_paris_Julie_races[] = {
    {"pariswing_Julie_Checkpoint_One", "cp_unordered", "midnight", "clear", 1.0f, 15.0f, nullptr, true, 1, 1, 0},
    {"pariswing_Julie_Checkpoint_Two", "cp_ordered", "dawn", "clear", 0.25f, 0.0f, nullptr, true, 1, 1, 1},
};
static constexpr FollowDef k_paris_Julie_follows[] = {
    {"pariswing_Julie_Follow", "pariswing_Julie_Checkpoint_One", 0},
    {"pariswing_Julie_Follow_Two", "pariswing_Julie_Checkpoint_Two", 1},
};
static constexpr HookmanDef k_paris_Julie_def = {
    "Julie", 1, 9, "pariswing_Julie_PatrolRoute",
    int(sizeof(k_paris_Julie_races)/sizeof(k_paris_Julie_races[0])), k_paris_Julie_races,
    int(sizeof(k_paris_Julie_follows)/sizeof(k_paris_Julie_follows[0])), k_paris_Julie_follows
};

static constexpr RaceDef k_paris_Primo_races[] = {
    {"parischris_Primo_Checkpoint_One", "cp_unordered_time_global", "dusk", "clear", 0.5f, 20.0f, nullptr, false, 1, 2, 0},
    {"paris_Primo_Checkpoint_Two", "cp_ordered", "midnight", "clear", 1.75f, 15.0f, nullptr, true, 1, 2, 1},
};
static constexpr FollowDef k_paris_Primo_follows[] = {
    {"paris_Primo_Follow", "parischris_Primo_Checkpoint_One", 0},
    {"paris_Primo_Follow_Two", "paris_Primo_Checkpoint_Two", 1},
};
static constexpr HookmanDef k_paris_Primo_def = {
    "Primo", 1, 10, "paris_Primo_PatrolRoute",
    int(sizeof(k_paris_Primo_races)/sizeof(k_paris_Primo_races[0])), k_paris_Primo_races,
    int(sizeof(k_paris_Primo_follows)/sizeof(k_paris_Primo_follows[0])), k_paris_Primo_follows
};

static constexpr RaceDef k_paris_Stephane_races[] = {
    {"parischris_Stephane_Checkpoint_One", "cp_unordered", "midnight", "clear", 1.25f, 5.0f, nullptr, true, 1, 3, 0},
    {"parischris_Stephane_Checkpoint_Two", "cp_ordered", "midnight", "rainy", 1.25f, 0.0f, nullptr, true, 1, 3, 1},
};
static constexpr FollowDef k_paris_Stephane_follows[] = {
    {"parischris_Stephane_Follow", "parischris_Stephane_Checkpoint_One", 0},
    {"parischris_Stephane_Follow_Two", "parischris_Stephane_Checkpoint_Two", 1},
};
static constexpr HookmanDef k_paris_Stephane_def = {
    "Stephane", 1, 11, "parischris_Stephane_PatrolRoute",
    int(sizeof(k_paris_Stephane_races)/sizeof(k_paris_Stephane_races[0])), k_paris_Stephane_races,
    int(sizeof(k_paris_Stephane_follows)/sizeof(k_paris_Stephane_follows[0])), k_paris_Stephane_follows
};

static constexpr RaceDef k_paris_Ian_races[] = {
    {"paris_Ian_Checkpoint_One", "cp_ordered", "dusk", "clear", 1.0f, 20.0f, nullptr, true, 1, 4, 0},
    {"paris_Ian_Checkpoint_Two", "cp_ordered", "dusk", "clear", 1.25f, 15.0f, nullptr, true, 1, 4, 1},
    {"paris_Ian_SideRace", "cp_ordered", "midnight", "clear", 1.5f, 20.0f, nullptr, true, 1, 4, 2},
};
static constexpr FollowDef k_paris_Ian_follows[] = {
    {"paris_Ian_Follow", "paris_Ian_Checkpoint_One", 0},
    {"paris_Ian_Follow_Two", "paris_Ian_Checkpoint_Two", 1},
    {"paris_Ian_Follow_Three", "paris_Ian_SideRace", 2},
};
static constexpr HookmanDef k_paris_Ian_def = {
    "Ian", 1, 12, "paris_Ian_PatrolRoute",
    int(sizeof(k_paris_Ian_races)/sizeof(k_paris_Ian_races[0])), k_paris_Ian_races,
    int(sizeof(k_paris_Ian_follows)/sizeof(k_paris_Ian_follows[0])), k_paris_Ian_follows
};

static constexpr RaceDef k_paris_Farid_races[] = {
    {"pariswing_Farid_Checkpoint_One", "cp_ordered", "midnight", "clear", 1.5f, 15.0f, nullptr, true, 1, 5, 0},
    {"pariswing_Farid_LoseTheCops", "lose_the_cops", "midnight", "cloudy", 1.5f, 15.0f, nullptr, false, 1, 5, 1},
    {"pariswing_Farid_SideRace", "cp_ordered", "dawn", "clear", 1.0f, 5.0f, nullptr, true, 1, 5, 2},
};
static constexpr FollowDef k_paris_Farid_follows[] = {
    {"pariswing_Farid_Follow", "pariswing_Farid_Checkpoint_One", 0},
    {"pariswing_Farid_Follow_Two", "pariswing_Farid_LoseTheCops", 1},
    {"pariswing_Farid_Follow_Three", "pariswing_Farid_SideRace", 2},
};
static constexpr HookmanDef k_paris_Farid_def = {
    "Farid", 1, 13, "pariswing_Farid_PatrolRoute",
    int(sizeof(k_paris_Farid_races)/sizeof(k_paris_Farid_races[0])), k_paris_Farid_races,
    int(sizeof(k_paris_Farid_follows)/sizeof(k_paris_Farid_follows[0])), k_paris_Farid_follows
};

static constexpr RaceDef k_paris_Parfait_races[] = {
    {"paris_Parfait_Checkpoint_One", "cp_ordered", "dusk", "clear", 1.5f, 20.0f, nullptr, true, 1, 6, 0},
    {"paris_Parfait_Checkpoint_Two", "cp_ordered", "midnight", "cloudy", 1.5f, 20.0f, nullptr, true, 1, 6, 1},
};
static constexpr FollowDef k_paris_Parfait_follows[] = {
};
static constexpr HookmanDef k_paris_Parfait_def = {
    "Parfait", 1, 14, nullptr,
    int(sizeof(k_paris_Parfait_races)/sizeof(k_paris_Parfait_races[0])), k_paris_Parfait_races,
    int(sizeof(k_paris_Parfait_follows)/sizeof(k_paris_Parfait_follows[0])), nullptr
};

static constexpr RaceDef k_paris_WorldChampParis_races[] = {
    {"paris_WorldChampParis_Checkpoint_One", "cp_unordered", "midnight", "clear", 4.0f, 0.0f, nullptr, true, 1, 7, 0},
    {"paris_WorldChampParis_Checkpoint_Two", "cp_ordered", "dawn", "clear", 1.0f, 5.0f, nullptr, true, 1, 7, 1},
};
static constexpr FollowDef k_paris_WorldChampParis_follows[] = {
};
static constexpr HookmanDef k_paris_WorldChampParis_def = {
    "WorldChampParis", 1, 15, nullptr,
    int(sizeof(k_paris_WorldChampParis_races)/sizeof(k_paris_WorldChampParis_races[0])), k_paris_WorldChampParis_races,
    int(sizeof(k_paris_WorldChampParis_follows)/sizeof(k_paris_WorldChampParis_follows[0])), nullptr
};

static constexpr RaceDef k_tokyo_Ricky_races[] = {
    {"tokyomauro_Ricky_Checkpoint_One", "cp_ordered", "midnight", "clear", 1.0f, 2.0f, nullptr, true, 2, 0, 0},
    {"tokyomauro_Ricky_Checkpoint_Two", "cp_ordered_time_global", "dawn", "clear", 1.0f, 10.0f, nullptr, true, 2, 0, 1},
};
static constexpr FollowDef k_tokyo_Ricky_follows[] = {
    {"tokyomauro_Ricky_Follow", "tokyomauro_Ricky_Checkpoint_One", 0},
    {"tokyomauro_Ricky_Follow_Two", "tokyomauro_Ricky_Checkpoint_Two", 1},
};
static constexpr HookmanDef k_tokyo_Ricky_def = {
    "Ricky", 2, 16, "tokyomauro_Ricky_PatrolRoute",
    int(sizeof(k_tokyo_Ricky_races)/sizeof(k_tokyo_Ricky_races[0])), k_tokyo_Ricky_races,
    int(sizeof(k_tokyo_Ricky_follows)/sizeof(k_tokyo_Ricky_follows[0])), k_tokyo_Ricky_follows
};

static constexpr RaceDef k_tokyo_Shing_races[] = {
    {"tokyochris_Shing_Checkpoint_One", "cp_ordered_time_local", "dusk", "clear", 2.5f, 20.0f, nullptr, true, 2, 1, 0},
    {"tokyochris_Shing_SideRace", "cp_ordered", "midnight", "clear", 1.5f, 10.0f, nullptr, true, 2, 1, 1},
    {"tokyochris_Shing_Checkpoint_Two", "cp_ordered", "midnight", "clear", 1.0f, 10.0f, nullptr, true, 2, 1, 2},
};
static constexpr FollowDef k_tokyo_Shing_follows[] = {
    {"tokyochris_Shing_Follow", "tokyochris_Shing_Checkpoint_One", 0},
    {"tokyochris_Shing_Follow_Two", "tokyochris_Shing_SideRace", 1},
    {"tokyochris_Shing_Follow_Three", "tokyochris_Shing_Checkpoint_Two", 2},
};
static constexpr HookmanDef k_tokyo_Shing_def = {
    "Shing", 2, 17, "tokyochris_Shing_PatrolRoute",
    int(sizeof(k_tokyo_Shing_races)/sizeof(k_tokyo_Shing_races[0])), k_tokyo_Shing_races,
    int(sizeof(k_tokyo_Shing_follows)/sizeof(k_tokyo_Shing_follows[0])), k_tokyo_Shing_follows
};

static constexpr RaceDef k_tokyo_Haley_races[] = {
    {"tokyo_Haley_Checkpoint_One", "cp_unordered", "midnight", "clear", 0.5f, 10.0f, nullptr, true, 2, 2, 0},
    {"tokyo_Haley_Checkpoint_Two", "cp_ordered", "dawn", "cloudy", 1.0f, 10.0f, nullptr, true, 2, 2, 1},
};
static constexpr FollowDef k_tokyo_Haley_follows[] = {
    {"tokyo_Haley_Follow", "tokyo_Haley_Checkpoint_One", 0},
    {"tokyo_Haley_Follow_Two", "tokyo_Haley_Checkpoint_Two", 1},
};
static constexpr HookmanDef k_tokyo_Haley_def = {
    "Haley", 2, 18, "tokyo_Haley_PatrolRoute",
    int(sizeof(k_tokyo_Haley_races)/sizeof(k_tokyo_Haley_races[0])), k_tokyo_Haley_races,
    int(sizeof(k_tokyo_Haley_follows)/sizeof(k_tokyo_Haley_follows[0])), k_tokyo_Haley_follows
};

static constexpr RaceDef k_tokyo_Nikko_races[] = {
    {"tokyomarc_Nikko_Checkpoint_One", "cp_ordered", "midnight", "rainy", 1.0f, 10.0f, nullptr, true, 2, 3, 0},
    {"tokyomarc_Nikko_Checkpoint_Two", "cp_ordered_time_local", "dusk", "clear", 1.0f, 10.0f, nullptr, true, 2, 3, 1},
};
static constexpr FollowDef k_tokyo_Nikko_follows[] = {
    {"tokyomarc_Nikko_Follow", "tokyomarc_Nikko_Checkpoint_One", 0},
    {"tokyomarc_Nikko_Follow_Two", "tokyomarc_Nikko_Checkpoint_Two", 1},
};
static constexpr HookmanDef k_tokyo_Nikko_def = {
    "Nikko", 2, 19, "tokyomarc_Nikko_PatrolRoute",
    int(sizeof(k_tokyo_Nikko_races)/sizeof(k_tokyo_Nikko_races[0])), k_tokyo_Nikko_races,
    int(sizeof(k_tokyo_Nikko_follows)/sizeof(k_tokyo_Nikko_follows[0])), k_tokyo_Nikko_follows
};

static constexpr RaceDef k_tokyo_Zen_races[] = {
    {"tokyowing_Zen_checkpoint_one", "cp_ordered_time_global", "midnight", "clear", 1.0f, 10.0f, nullptr, true, 2, 4, 0},
    {"tokyowing_Zen_checkpoint_two", "cp_ordered", "midnight", "clear", 1.0f, 20.0f, nullptr, true, 2, 4, 1},
};
static constexpr FollowDef k_tokyo_Zen_follows[] = {
    {"tokyowing_Zen_follow", "tokyowing_Zen_checkpoint_one", 0},
    {"tokyowing_Zen_Follow_Two", "tokyowing_Zen_checkpoint_two", 1},
};
static constexpr HookmanDef k_tokyo_Zen_def = {
    "Zen", 2, 20, "tokyowing_Zen_patrol",
    int(sizeof(k_tokyo_Zen_races)/sizeof(k_tokyo_Zen_races[0])), k_tokyo_Zen_races,
    int(sizeof(k_tokyo_Zen_follows)/sizeof(k_tokyo_Zen_follows[0])), k_tokyo_Zen_follows
};

static constexpr RaceDef k_tokyo_Kenichi_races[] = {
    {"tokyo_Kenichi_Checkpoint_One", "cp_ordered", "midnight", "clear", 1.0f, 10.0f, nullptr, true, 2, 5, 0},
    {"tokyo_Kenichi_Checkpoint_Two", "destroy", "midnight", "clear", 1.0f, 10.0f, "vp_tokyocop", false, 2, 5, 1},
    {"tokyo_Kenichi_Checkpoint_Three", "cp_ordered", "midnight", "rainy", 4.0f, 20.0f, nullptr, true, 2, 5, 2},
};
static constexpr FollowDef k_tokyo_Kenichi_follows[] = {
    {"tokyo_Kenichi_Follow", "tokyo_Kenichi_Checkpoint_One", 0},
    {"tokyo_Kenichi_Follow_Two", "tokyo_Kenichi_Checkpoint_Two", 1},
    {"tokyo_Kenichi_Follow_Three", "tokyo_Kenichi_Checkpoint_Three", 2},
};
static constexpr HookmanDef k_tokyo_Kenichi_def = {
    "Kenichi", 2, 21, "tokyo_Kenichi_PatrolRoute",
    int(sizeof(k_tokyo_Kenichi_races)/sizeof(k_tokyo_Kenichi_races[0])), k_tokyo_Kenichi_races,
    int(sizeof(k_tokyo_Kenichi_follows)/sizeof(k_tokyo_Kenichi_follows[0])), k_tokyo_Kenichi_follows
};

static constexpr RaceDef k_tokyo_Makoto_races[] = {
    {"tokyochris_Makoto_Checkpoint_One", "cp_ordered_time_global", "midnight", "clear", 1.0f, 10.0f, nullptr, true, 2, 6, 0},
    {"tokyochris_Makoto_Checkpoint_Two", "cp_ordered", "dawn", "rainy", 2.0f, 25.0f, nullptr, true, 2, 6, 1},
};
static constexpr FollowDef k_tokyo_Makoto_follows[] = {
};
static constexpr HookmanDef k_tokyo_Makoto_def = {
    "Makoto", 2, 22, nullptr,
    int(sizeof(k_tokyo_Makoto_races)/sizeof(k_tokyo_Makoto_races[0])), k_tokyo_Makoto_races,
    int(sizeof(k_tokyo_Makoto_follows)/sizeof(k_tokyo_Makoto_follows[0])), nullptr
};

static constexpr RaceDef k_tokyo_WorldChampTokyo_races[] = {
    {"tokyomarc_WorldChampTokyo_Checkpoint_One", "cp_ordered", "midnight", "clear", 1.0f, 10.0f, nullptr, true, 2, 7, 0},
    {"tokyomarc_WorldChampTokyo_Checkpoint_Two", "cp_ordered_time_global", "dusk", "clear", 2.5f, 20.0f, nullptr, true, 2, 7, 1},
};
static constexpr FollowDef k_tokyo_WorldChampTokyo_follows[] = {
};
static constexpr HookmanDef k_tokyo_WorldChampTokyo_def = {
    "WorldChampTokyo", 2, 23, nullptr,
    int(sizeof(k_tokyo_WorldChampTokyo_races)/sizeof(k_tokyo_WorldChampTokyo_races[0])), k_tokyo_WorldChampTokyo_races,
    int(sizeof(k_tokyo_WorldChampTokyo_follows)/sizeof(k_tokyo_WorldChampTokyo_follows[0])), nullptr
};

static const HookmanDef* const kLaHookmen[] = {
    &k_losangeles_Moses_def,
    &k_losangeles_Steven_def,
    &k_losangeles_Maria_def,
    &k_losangeles_Angel_def,
    &k_losangeles_Gina_def,
    &k_losangeles_Hector_def,
    &k_losangeles_Dice_def,
    &k_losangeles_WorldChampla_def,
};

static const HookmanDef* const kParisHookmen[] = {
    &k_paris_Blog_def,
    &k_paris_Julie_def,
    &k_paris_Primo_def,
    &k_paris_Stephane_def,
    &k_paris_Ian_def,
    &k_paris_Farid_def,
    &k_paris_Parfait_def,
    &k_paris_WorldChampParis_def,
};

static const HookmanDef* const kTokyoHookmen[] = {
    &k_tokyo_Ricky_def,
    &k_tokyo_Shing_def,
    &k_tokyo_Haley_def,
    &k_tokyo_Nikko_def,
    &k_tokyo_Zen_def,
    &k_tokyo_Kenichi_def,
    &k_tokyo_Makoto_def,
    &k_tokyo_WorldChampTokyo_def,
};

static const HookmanDef* const kAllHookmen[24] = {
    &k_losangeles_Moses_def,
    &k_losangeles_Steven_def,
    &k_losangeles_Maria_def,
    &k_losangeles_Angel_def,
    &k_losangeles_Gina_def,
    &k_losangeles_Hector_def,
    &k_losangeles_Dice_def,
    &k_losangeles_WorldChampla_def,
    &k_paris_Blog_def,
    &k_paris_Julie_def,
    &k_paris_Primo_def,
    &k_paris_Stephane_def,
    &k_paris_Ian_def,
    &k_paris_Farid_def,
    &k_paris_Parfait_def,
    &k_paris_WorldChampParis_def,
    &k_tokyo_Ricky_def,
    &k_tokyo_Shing_def,
    &k_tokyo_Haley_def,
    &k_tokyo_Nikko_def,
    &k_tokyo_Zen_def,
    &k_tokyo_Kenichi_def,
    &k_tokyo_Makoto_def,
    &k_tokyo_WorldChampTokyo_def,
};

static constexpr MovieDef kAllMovies[] = {
    {MovieType::PreCity, nullptr, "la/moses_01.pss", "la\\moses_01.bik"},
    {MovieType::PreRace, "lamauro_Moses_Checkpoint_One", "la/moses_02.pss", "la\\moses_02.bik"},
    {MovieType::PreRace, "lamauro_Moses_Checkpoint_Two", "la/moses_03.pss", "la\\moses_03.bik"},
    {MovieType::PostRace, "lamauro_Moses_Checkpoint_Two", "la/moses_04.pss", "la\\moses_04.bik"},
    {MovieType::PreRace, "lamauro_Steven_Checkpoint_One", "la/steve_02.pss", "la\\steve_02.bik"},
    {MovieType::PreRace, "lachrisbeta_Steven_Checkpoint_Two", "la/steve_01.pss", "la\\steve_01.bik"},
    {MovieType::PostRace, "lachrisbeta_Steven_Checkpoint_Two", "la/steve_03.pss", "la\\steve_03.bik"},
    {MovieType::PreRace, "lawing_Hector_LoseTheCops", "la/Hectr_01.pss", "la\\Hectr_01.bik"},
    {MovieType::PreRace, "lachrisbeta_Hector_Checkpoint_Two", "la/Hectr_02.pss", "la\\Hectr_02.bik"},
    {MovieType::PreRace, "lachrisbeta_Hector_Checkpoint_Three", "la/Hectr_03.pss", "la\\Hectr_03.bik"},
    {MovieType::PostRace, "lachrisbeta_Hector_Checkpoint_Three", "la/Hectr_04.pss", "la\\Hectr_04.bik"},
    {MovieType::AltPostRace, "lachrisbeta_Hector_Checkpoint_Three", "la/Hectr_4b.pss", "la\\Hectr_4b.bik"},
    {MovieType::PreRace, "lamauro_Maria_Checkpoint_One", "la/pica_01.pss", "la\\pica_01.bik"},
    {MovieType::PreRace, "lamauro_Maria_Checkpoint_Two", "la/pica_02.pss", "la\\pica_02.bik"},
    {MovieType::PostRace, "lamauro_Maria_Checkpoint_Two", "la/pica_03.pss", "la\\pica_03.bik"},
    {MovieType::PreRace, "lamauro_Angel_Checkpoint_One", "la/angel_01.pss", "la\\angel_01.bik"},
    {MovieType::PreRace, "lamauro_Angel_SideRace", "la/angel_02.pss", "la\\angel_02.bik"},
    {MovieType::PreRace, "lamauro_Angel_Checkpoint_Two", "la/angel_03.pss", "la\\angel_03.bik"},
    {MovieType::PostRace, "lamauro_Angel_Checkpoint_Two", "la/angel_04.pss", "la\\angel_04.bik"},
    {MovieType::AltPostRace, "lamauro_Angel_Checkpoint_Two", "la/angel_4b.pss", "la\\angel_4b.bik"},
    {MovieType::PreRace, "laChris_Gina_Checkpoint_One", "la/Gina_01.pss", "la\\Gina_01.bik"},
    {MovieType::PreRace, "laChris_Gina_SideRace", "la/Gina_02.pss", "la\\Gina_02.bik"},
    {MovieType::PreRace, "laChris_Gina_Bike_Tutorial", "la/ginabike.pss", "la\\ginabike.bik"},
    {MovieType::PreRace, "laChris_Gina_Checkpoint_Two", "la/Gina_03.pss", "la\\Gina_03.bik"},
    {MovieType::PostRace, "laChris_Gina_Checkpoint_Two", "la/Gina_04.pss", "la\\Gina_04.bik"},
    {MovieType::AltPostRace, "laChris_Gina_Checkpoint_Two", "la/Gina_4b.pss", "la\\Gina_4b.bik"},
    {MovieType::PreRace, "lamaurobeta_Dice_Checkpoint_Two", "la/Dice_02.pss", "la\\Dice_02.bik"},
    {MovieType::PostRace, "lamaurobeta_Dice_Checkpoint_Two", "la/Dice_03.pss", "la\\Dice_03.bik"},
    {MovieType::PreRace, "lachrisbeta_WorldChampLA_Checkpoint_One", "world/world_03.pss", "world\\world_03.bik"},
    {MovieType::PostRace, "laWC_WorldChampLA_Checkpoint_Two", "world/world_04.pss", "world\\world_04.bik"},
    {MovieType::PreRace, "parischris_Blog_Checkpoint_One", "Paris/blog_01.pss", "Paris\\blog_01.bik"},
    {MovieType::PreRace, "parischris_Blog_Checkpoint_Two", "Paris/blog_02.pss", "Paris\\blog_02.bik"},
    {MovieType::PostRace, "parischris_Blog_Checkpoint_Two", "Paris/blog_03.pss", "Paris\\blog_03.bik"},
    {MovieType::PreRace, "parischris_Stephane_Checkpoint_One", "Paris/steph_01.pss", "Paris\\steph_01.bik"},
    {MovieType::PreRace, "parischris_Stephane_Checkpoint_Two", "Paris/steph_02.pss", "Paris\\steph_02.bik"},
    {MovieType::PostRace, "parischris_Stephane_Checkpoint_Two", "Paris/steph_03.pss", "Paris\\steph_03.bik"},
    {MovieType::PreRace, "parischris_Primo_Checkpoint_One", "Paris/Primo_01.pss", "Paris\\Primo_01.bik"},
    {MovieType::PreRace, "paris_Primo_Checkpoint_Two", "Paris/Primo_02.pss", "Paris\\Primo_02.bik"},
    {MovieType::PostRace, "paris_Primo_Checkpoint_Two", "Paris/Primo_03.pss", "Paris\\Primo_03.bik"},
    {MovieType::PreRace, "paris_Ian_Checkpoint_One", "Paris/Ian_01.pss", "Paris\\Ian_01.bik"},
    {MovieType::PreRace, "paris_Ian_Checkpoint_Two", "Paris/Ian_02.pss", "Paris\\Ian_02.bik"},
    {MovieType::PreRace, "paris_Ian_SideRace", "Paris/Ian_03.pss", "Paris\\Ian_03.bik"},
    {MovieType::PostRace, "paris_Ian_SideRace", "Paris/Ian_04.pss", "Paris\\Ian_04.bik"},
    {MovieType::AltPostRace, "paris_Ian_SideRace", "Paris/Ian_4b.pss", "Paris\\Ian_4b.bik"},
    {MovieType::PreRace, "pariswing_Farid_Checkpoint_One", "Paris/Farid_01.pss", "Paris\\Farid_01.bik"},
    {MovieType::PreRace, "pariswing_Farid_LoseTheCops", "Paris/Farid_02.pss", "Paris\\Farid_02.bik"},
    {MovieType::PreRace, "pariswing_Farid_SideRace", "Paris/Farid_03.pss", "Paris\\Farid_03.bik"},
    {MovieType::PostRace, "pariswing_Farid_SideRace", "Paris/Farid_04.pss", "Paris\\Farid_04.bik"},
    {MovieType::AltPostRace, "pariswing_Farid_SideRace", "Paris/Farid_4b.pss", "Paris\\Farid_4b.bik"},
    {MovieType::PreRace, "pariswing_Julie_Checkpoint_One", "Paris/Twins_01.pss", "Paris\\Twins_01.bik"},
    {MovieType::PreRace, "pariswing_Julie_Checkpoint_Two", "Paris/Twins_02.pss", "Paris\\Twins_02.bik"},
    {MovieType::PostRace, "pariswing_Julie_Checkpoint_Two", "Paris/Twins_03.pss", "Paris\\Twins_03.bik"},
    {MovieType::PreRace, "paris_Parfait_Checkpoint_Two", "Paris/Parft_02.pss", "Paris\\Parft_02.bik"},
    {MovieType::PostRace, "paris_Parfait_Checkpoint_Two", "Paris/Parft_03.pss", "Paris\\Parft_03.bik"},
    {MovieType::PreRace, "paris_WorldChampParis_Checkpoint_One", "world/world_02.pss", "world\\world_02.bik"},
    {MovieType::PreRace, "tokyomauro_Ricky_Checkpoint_One", "Tokyo/Ricky_01.pss", "Tokyo\\Ricky_01.bik"},
    {MovieType::PreRace, "tokyomauro_Ricky_Checkpoint_Two", "Tokyo/Ricky_02.pss", "Tokyo\\Ricky_02.bik"},
    {MovieType::PostRace, "tokyomauro_Ricky_Checkpoint_Two", "Tokyo/Ricky_03.pss", "Tokyo\\Ricky_03.bik"},
    {MovieType::PreRace, "tokyowing_Zen_checkpoint_one", "Tokyo/Zen_01.pss", "Tokyo\\Zen_01.bik"},
    {MovieType::PreRace, "tokyowing_Zen_checkpoint_two", "Tokyo/Zen_02.pss", "Tokyo\\Zen_02.bik"},
    {MovieType::PostRace, "tokyowing_Zen_checkpoint_two", "Tokyo/Zen_03.pss", "Tokyo\\Zen_03.bik"},
    {MovieType::AltPostRace, "tokyowing_Zen_checkpoint_two", "Tokyo/Zen_3b.pss", "Tokyo\\Zen_3b.bik"},
    {MovieType::PreRace, "tokyo_Haley_Checkpoint_One", "Tokyo/Haley_01.pss", "Tokyo\\Haley_01.bik"},
    {MovieType::PreRace, "tokyo_Haley_Checkpoint_Two", "Tokyo/Haley_02.pss", "Tokyo\\Haley_02.bik"},
    {MovieType::PostRace, "tokyo_Haley_Checkpoint_Two", "Tokyo/Haley_03.pss", "Tokyo\\Haley_03.bik"},
    {MovieType::PreRace, "tokyochris_Shing_Checkpoint_One", "Tokyo/Shing_01.pss", "Tokyo\\Shing_01.bik"},
    {MovieType::PreRace, "tokyochris_Shing_SideRace", "Tokyo/Shing_02.pss", "Tokyo\\Shing_02.bik"},
    {MovieType::PreRace, "tokyochris_Shing_Checkpoint_Two", "Tokyo/Shing_03.pss", "Tokyo\\Shing_03.bik"},
    {MovieType::PostRace, "tokyochris_Shing_Checkpoint_Two", "Tokyo/Shing_04.pss", "Tokyo\\Shing_04.bik"},
    {MovieType::PreRace, "tokyo_Kenichi_Checkpoint_One", "Tokyo/Ken_01.pss", "Tokyo\\Ken_01.bik"},
    {MovieType::PreRace, "tokyo_Kenichi_Checkpoint_Two", "Tokyo/Ken_02.pss", "Tokyo\\Ken_02.bik"},
    {MovieType::PreRace, "tokyo_Kenichi_Checkpoint_Three", "Tokyo/Ken_03.pss", "Tokyo\\Ken_03.bik"},
    {MovieType::PostRace, "tokyo_Kenichi_Checkpoint_Three", "Tokyo/Ken_04.pss", "Tokyo\\Ken_04.bik"},
    {MovieType::AltPostRace, "tokyo_Kenichi_Checkpoint_Three", "Tokyo/Ken_4b.pss", "Tokyo\\Ken_4b.bik"},
    {MovieType::PreRace, "tokyomarc_Nikko_Checkpoint_One", "Tokyo/Nikko_01.pss", "Tokyo\\Nikko_01.bik"},
    {MovieType::PreRace, "tokyomarc_Nikko_Checkpoint_Two", "Tokyo/Nikko_02.pss", "Tokyo\\Nikko_02.bik"},
    {MovieType::PostRace, "tokyomarc_Nikko_Checkpoint_Two", "Tokyo/Nikko_03.pss", "Tokyo\\Nikko_03.bik"},
    {MovieType::PreRace, "tokyochris_Makoto_Checkpoint_Two", "Tokyo/Mako_02.pss", "Tokyo\\Mako_02.bik"},
    {MovieType::PostRace, "tokyochris_Makoto_Checkpoint_Two", "Tokyo/Mako_03.pss", "Tokyo\\Mako_03.bik"},
};

static constexpr UnlockCondition kRule_0_conds[] = {
    {ConditionType::HookmanBeatenIter, "Moses"},
};
static constexpr UnlockCondition kRule_1_conds[] = {
    {ConditionType::HookmanBeatenIter, "Moses"},
};
static constexpr UnlockCondition kRule_2_conds[] = {
    {ConditionType::HookmanBeatenIter, "Steven"},
};
static constexpr UnlockCondition kRule_3_conds[] = {
    {ConditionType::HookmanBeatenIter, "Maria"},
};
static constexpr UnlockCondition kRule_4_conds[] = {
    {ConditionType::HookmanBeatenIter, "Maria"},
};
static constexpr UnlockCondition kRule_5_conds[] = {
    {ConditionType::HookmanBeatenIter, "Angel"},
    {ConditionType::HookmanBeatenIter, "Gina"},
    {ConditionType::HookmanBeatenIter, "Hector"},
};
static constexpr UnlockCondition kRule_6_conds[] = {
    {ConditionType::HookmanBeaten, "Dice"},
};
static constexpr UnlockCondition kRule_7_conds[] = {
    {ConditionType::HookmanBeaten, "Parfait"},
};
static constexpr UnlockCondition kRule_8_conds[] = {
    {ConditionType::HookmanBeatenIter, "Dice"},
};
static constexpr UnlockCondition kRule_9_conds[] = {
    {ConditionType::HookmanBeatenIter, "Julie"},
};
static constexpr UnlockCondition kRule_10_conds[] = {
    {ConditionType::HookmanBeatenIter, "Blog"},
};
static constexpr UnlockCondition kRule_11_conds[] = {
    {ConditionType::HookmanBeatenIter, "Blog"},
};
static constexpr UnlockCondition kRule_12_conds[] = {
    {ConditionType::HookmanBeatenIter, "Primo"},
};
static constexpr UnlockCondition kRule_13_conds[] = {
    {ConditionType::HookmanBeatenIter, "Primo"},
    {ConditionType::HookmanBeatenIter, "Stephane"},
};
static constexpr UnlockCondition kRule_14_conds[] = {
    {ConditionType::HookmanBeatenIter, "Farid"},
    {ConditionType::HookmanBeatenIter, "Ian"},
};
static constexpr UnlockCondition kRule_15_conds[] = {
    {ConditionType::HookmanBeatenIter, "Parfait"},
};
static constexpr UnlockCondition kRule_16_conds[] = {
    {ConditionType::HookmanBeatenIter, "Parfait"},
};
static constexpr UnlockCondition kRule_17_conds[] = {
    {ConditionType::HookmanBeatenIter, "Ricky"},
    {ConditionType::HookmanBeatenIter, "Haley"},
};
static constexpr UnlockCondition kRule_18_conds[] = {
    {ConditionType::HookmanBeatenIter, "Shing"},
};
static constexpr UnlockCondition kRule_19_conds[] = {
    {ConditionType::HookmanBeatenIter, "Ricky"},
};
static constexpr UnlockCondition kRule_20_conds[] = {
    {ConditionType::HookmanBeatenIter, "Haley"},
    {ConditionType::HookmanBeatenIter, "Nikko"},
};
static constexpr UnlockCondition kRule_21_conds[] = {
    {ConditionType::HookmanBeatenIter, "Kenichi"},
    {ConditionType::HookmanBeatenIter, "Zen"},
};
static constexpr UnlockCondition kRule_22_conds[] = {
    {ConditionType::HookmanBeatenIter, "Makoto"},
};
static constexpr UnlockCondition kRule_23_conds[] = {
    {ConditionType::HookmanBeatenIter, "WorldChampTokyo"},
};
static constexpr UnlockCondition kRule_24_conds[] = {
    {ConditionType::HookmanBeatenIter, "WorldChampParis"},
};
static constexpr UnlockCondition kRule_25_conds[] = {
    {ConditionType::HookmanBeaten, "Moses"},
};
static constexpr UnlockCondition kRule_26_conds[] = {
    {ConditionType::HookmanBeaten, "Steven"},
};
static constexpr UnlockCondition kRule_27_conds[] = {
    {ConditionType::HookmanBeaten, "Maria"},
};
static constexpr UnlockCondition kRule_28_conds[] = {
    {ConditionType::HookmanBeaten, "Gina"},
};
static constexpr UnlockCondition kRule_29_conds[] = {
    {ConditionType::HookmanBeaten, "Angel"},
};
static constexpr UnlockCondition kRule_30_conds[] = {
    {ConditionType::RaceBeaten, "lamauro_Angel_SideRace"},
};
static constexpr UnlockCondition kRule_31_conds[] = {
    {ConditionType::HookmanBeaten, "Hector"},
};
static constexpr UnlockCondition kRule_32_conds[] = {
    {ConditionType::HookmanBeaten, "Dice"},
};
static constexpr UnlockCondition kRule_33_conds[] = {
    {ConditionType::RaceBeaten, "lachris_Gina_SideRace"},
};
static constexpr UnlockCondition kRule_34_conds[] = {
    {ConditionType::HookmanBeaten, "Julie"},
};
static constexpr UnlockCondition kRule_35_conds[] = {
    {ConditionType::HookmanBeaten, "Primo"},
};
static constexpr UnlockCondition kRule_36_conds[] = {
    {ConditionType::HookmanBeaten, "Stephane"},
};
static constexpr UnlockCondition kRule_37_conds[] = {
    {ConditionType::RaceBeaten, "paris_Ian_Checkpoint_Two"},
};
static constexpr UnlockCondition kRule_38_conds[] = {
    {ConditionType::HookmanBeaten, "Blog"},
};
static constexpr UnlockCondition kRule_39_conds[] = {
    {ConditionType::RaceBeaten, "pariswing_Farid_LoseTheCops"},
};
static constexpr UnlockCondition kRule_40_conds[] = {
    {ConditionType::HookmanBeaten, "Parfait"},
};
static constexpr UnlockCondition kRule_41_conds[] = {
    {ConditionType::RaceBeaten, "paris_Ian_SideRace"},
};
static constexpr UnlockCondition kRule_42_conds[] = {
    {ConditionType::HookmanBeaten, "Shing"},
};
static constexpr UnlockCondition kRule_43_conds[] = {
    {ConditionType::HookmanBeaten, "Haley"},
};
static constexpr UnlockCondition kRule_44_conds[] = {
    {ConditionType::HookmanBeaten, "Kenichi"},
};
static constexpr UnlockCondition kRule_45_conds[] = {
    {ConditionType::HookmanBeaten, "Ricky"},
};
static constexpr UnlockCondition kRule_46_conds[] = {
    {ConditionType::RaceBeaten, "tokyochris_Shing_SideRace"},
};
static constexpr UnlockCondition kRule_47_conds[] = {
    {ConditionType::HookmanBeaten, "Zen"},
};
static constexpr UnlockCondition kRule_48_conds[] = {
    {ConditionType::HookmanBeaten, "Nikko"},
};
static constexpr UnlockCondition kRule_49_conds[] = {
    {ConditionType::HookmanBeaten, "Makoto"},
};
static constexpr UnlockCondition kRule_50_conds[] = {
    {ConditionType::HookmanBeaten, "WorldChampLA"},
};
static constexpr UnlockCondition kRule_51_conds[] = {
    {ConditionType::RaceBeaten, "pariswing_Farid_SideRace"},
};
static constexpr UnlockCondition kRule_52_conds[] = {
    {ConditionType::RaceBeaten, "paris_Arcade_Circuit_Six"},
    {ConditionType::RaceBeaten, "paris_Arcade_Circuit_Five"},
    {ConditionType::RaceBeaten, "paris_Arcade_Circuit_Four"},
    {ConditionType::RaceBeaten, "paris_Arcade_Circuit_Three"},
    {ConditionType::RaceBeaten, "paris_Arcade_Circuit_Two"},
    {ConditionType::RaceBeaten, "paris_Arcade_Circuit_One"},
};
static constexpr UnlockCondition kRule_53_conds[] = {
    {ConditionType::RaceBeaten, "lachris_Arcade_Circuit_Five"},
    {ConditionType::RaceBeaten, "lachris_Arcade_Circuit_Four"},
    {ConditionType::RaceBeaten, "lachris_Arcade_Circuit_Three"},
    {ConditionType::RaceBeaten, "lachris_Arcade_Circuit_Two"},
    {ConditionType::RaceBeaten, "lamauro_Arcade_Circuit_One"},
};
static constexpr UnlockCondition kRule_54_conds[] = {
    {ConditionType::RaceBeaten, "tokyotroy_Arcade_Circuit_Seven"},
    {ConditionType::RaceBeaten, "tokyotroy_Arcade_Circuit_Six"},
    {ConditionType::RaceBeaten, "tokyoarcade_Arcade_Circuit_Five"},
    {ConditionType::RaceBeaten, "tokyoarcade_Arcade_Circuit_Four"},
    {ConditionType::RaceBeaten, "tokyowing_Arcade_Circuit_Three"},
    {ConditionType::RaceBeaten, "tokyoarcade_Arcade_Circuit_Two"},
    {ConditionType::RaceBeaten, "tokyoarcade_Arcade_Circuit_One"},
};
static constexpr UnlockCondition kRule_55_conds[] = {
    {ConditionType::VehicleAvailable, "vp_pariscop"},
    {ConditionType::VehicleAvailable, "vp_cop_l"},
    {ConditionType::VehicleAvailable, "vp_tokyocop"},
    {ConditionType::HookmanBeaten, "WorldChampLA"},
};
static constexpr UnlockCondition kRule_56_conds[] = {
    {ConditionType::RaceBeaten, "lachrisbeta_Hector_Checkpoint_Two"},
};
static constexpr UnlockCondition kRule_57_conds[] = {
    {ConditionType::RaceBeaten, "lachrisbeta_Hector_Checkpoint_Two"},
};
static constexpr UnlockCondition kRule_58_conds[] = {
    {ConditionType::RaceBeaten, "lamauro_Angel_Checkpoint_One"},
};
static constexpr UnlockCondition kRule_59_conds[] = {
    {ConditionType::RaceBeaten, "lamauro_Angel_Checkpoint_One"},
};
static constexpr UnlockCondition kRule_60_conds[] = {
    {ConditionType::RaceBeaten, "lamauro_Maria_Follow"},
};
static constexpr UnlockCondition kRule_61_conds[] = {
    {ConditionType::RaceBeaten, "lamauro_Maria_Follow"},
};
static constexpr UnlockCondition kRule_62_conds[] = {
    {ConditionType::RaceBeaten, "lamaurobeta_Dice_Checkpoint_Two"},
};
static constexpr UnlockCondition kRule_63_conds[] = {
    {ConditionType::RaceBeaten, "lamaurobeta_Dice_Checkpoint_Two"},
};
static constexpr UnlockCondition kRule_64_conds[] = {
    {ConditionType::HookmanBeaten, "Moses"},
};
static constexpr UnlockRule kAllUnlockRules[] = {
    {RewardType::Hookman, "Maria", false, int(sizeof(kRule_0_conds)/sizeof(kRule_0_conds[0])), kRule_0_conds},
    {RewardType::Hookman, "Steven", false, int(sizeof(kRule_1_conds)/sizeof(kRule_1_conds[0])), kRule_1_conds},
    {RewardType::Hookman, "Angel", false, int(sizeof(kRule_2_conds)/sizeof(kRule_2_conds[0])), kRule_2_conds},
    {RewardType::Hookman, "Gina", false, int(sizeof(kRule_3_conds)/sizeof(kRule_3_conds[0])), kRule_3_conds},
    {RewardType::Hookman, "Hector", false, int(sizeof(kRule_4_conds)/sizeof(kRule_4_conds[0])), kRule_4_conds},
    {RewardType::Hookman, "Dice", false, int(sizeof(kRule_5_conds)/sizeof(kRule_5_conds[0])), kRule_5_conds},
    {RewardType::City, "Paris", false, int(sizeof(kRule_6_conds)/sizeof(kRule_6_conds[0])), kRule_6_conds},
    {RewardType::City, "Tokyo", false, int(sizeof(kRule_7_conds)/sizeof(kRule_7_conds[0])), kRule_7_conds},
    {RewardType::Hookman, "Blog", false, int(sizeof(kRule_8_conds)/sizeof(kRule_8_conds[0])), kRule_8_conds},
    {RewardType::Hookman, "Stephane", false, int(sizeof(kRule_9_conds)/sizeof(kRule_9_conds[0])), kRule_9_conds},
    {RewardType::Hookman, "Primo", false, int(sizeof(kRule_10_conds)/sizeof(kRule_10_conds[0])), kRule_10_conds},
    {RewardType::Hookman, "Julie", false, int(sizeof(kRule_11_conds)/sizeof(kRule_11_conds[0])), kRule_11_conds},
    {RewardType::Hookman, "Ian", false, int(sizeof(kRule_12_conds)/sizeof(kRule_12_conds[0])), kRule_12_conds},
    {RewardType::Hookman, "Farid", false, int(sizeof(kRule_13_conds)/sizeof(kRule_13_conds[0])), kRule_13_conds},
    {RewardType::Hookman, "Parfait", false, int(sizeof(kRule_14_conds)/sizeof(kRule_14_conds[0])), kRule_14_conds},
    {RewardType::Hookman, "Shing", false, int(sizeof(kRule_15_conds)/sizeof(kRule_15_conds[0])), kRule_15_conds},
    {RewardType::Hookman, "Ricky", false, int(sizeof(kRule_16_conds)/sizeof(kRule_16_conds[0])), kRule_16_conds},
    {RewardType::Hookman, "Zen", false, int(sizeof(kRule_17_conds)/sizeof(kRule_17_conds[0])), kRule_17_conds},
    {RewardType::Hookman, "Haley", false, int(sizeof(kRule_18_conds)/sizeof(kRule_18_conds[0])), kRule_18_conds},
    {RewardType::Hookman, "Nikko", false, int(sizeof(kRule_19_conds)/sizeof(kRule_19_conds[0])), kRule_19_conds},
    {RewardType::Hookman, "Kenichi", false, int(sizeof(kRule_20_conds)/sizeof(kRule_20_conds[0])), kRule_20_conds},
    {RewardType::Hookman, "Makoto", false, int(sizeof(kRule_21_conds)/sizeof(kRule_21_conds[0])), kRule_21_conds},
    {RewardType::Hookman, "WorldChampTokyo", false, int(sizeof(kRule_22_conds)/sizeof(kRule_22_conds[0])), kRule_22_conds},
    {RewardType::Hookman, "WorldChampParis", false, int(sizeof(kRule_23_conds)/sizeof(kRule_23_conds[0])), kRule_23_conds},
    {RewardType::Hookman, "WorldChampLA", false, int(sizeof(kRule_24_conds)/sizeof(kRule_24_conds[0])), kRule_24_conds},
    {RewardType::Vehicle, "vp_civica", false, int(sizeof(kRule_25_conds)/sizeof(kRule_25_conds[0])), kRule_25_conds},
    {RewardType::Vehicle, "vp_jetta", false, int(sizeof(kRule_26_conds)/sizeof(kRule_26_conds[0])), kRule_26_conds},
    {RewardType::Vehicle, "vp_integraa", false, int(sizeof(kRule_27_conds)/sizeof(kRule_27_conds[0])), kRule_27_conds},
    {RewardType::Vehicle, "vp_civicb", false, int(sizeof(kRule_28_conds)/sizeof(kRule_28_conds[0])), kRule_28_conds},
    {RewardType::Vehicle, "vp_s2000", false, int(sizeof(kRule_29_conds)/sizeof(kRule_29_conds[0])), kRule_29_conds},
    {RewardType::Vehicle, "vp_gto", false, int(sizeof(kRule_30_conds)/sizeof(kRule_30_conds[0])), kRule_30_conds},
    {RewardType::Vehicle, "vp_rx7", false, int(sizeof(kRule_31_conds)/sizeof(kRule_31_conds[0])), kRule_31_conds},
    {RewardType::Vehicle, "vp_viper", false, int(sizeof(kRule_32_conds)/sizeof(kRule_32_conds[0])), kRule_32_conds},
    {RewardType::Vehicle, "vp_cbr929a", false, int(sizeof(kRule_33_conds)/sizeof(kRule_33_conds[0])), kRule_33_conds},
    {RewardType::Vehicle, "vp_cosworth", false, int(sizeof(kRule_34_conds)/sizeof(kRule_34_conds[0])), kRule_34_conds},
    {RewardType::Vehicle, "vp_m5", false, int(sizeof(kRule_35_conds)/sizeof(kRule_35_conds[0])), kRule_35_conds},
    {RewardType::Vehicle, "vp_elise", false, int(sizeof(kRule_36_conds)/sizeof(kRule_36_conds[0])), kRule_36_conds},
    {RewardType::Vehicle, "vp_golfa", false, int(sizeof(kRule_37_conds)/sizeof(kRule_37_conds[0])), kRule_37_conds},
    {RewardType::Vehicle, "vp_puma", false, int(sizeof(kRule_38_conds)/sizeof(kRule_38_conds[0])), kRule_38_conds},
    {RewardType::Vehicle, "vp_clio", false, int(sizeof(kRule_39_conds)/sizeof(kRule_39_conds[0])), kRule_39_conds},
    {RewardType::Vehicle, "vp_911", false, int(sizeof(kRule_40_conds)/sizeof(kRule_40_conds[0])), kRule_40_conds},
    {RewardType::Vehicle, "vp_ducati", false, int(sizeof(kRule_41_conds)/sizeof(kRule_41_conds[0])), kRule_41_conds},
    {RewardType::Vehicle, "vp_nissanz", false, int(sizeof(kRule_42_conds)/sizeof(kRule_42_conds[0])), kRule_42_conds},
    {RewardType::Vehicle, "vp_supraa", false, int(sizeof(kRule_43_conds)/sizeof(kRule_43_conds[0])), kRule_43_conds},
    {RewardType::Vehicle, "vp_suprab", false, int(sizeof(kRule_44_conds)/sizeof(kRule_44_conds[0])), kRule_44_conds},
    {RewardType::Vehicle, "vp_mr2", false, int(sizeof(kRule_45_conds)/sizeof(kRule_45_conds[0])), kRule_45_conds},
    {RewardType::Vehicle, "vp_lexus", false, int(sizeof(kRule_46_conds)/sizeof(kRule_46_conds[0])), kRule_46_conds},
    {RewardType::Vehicle, "vp_yamaha", false, int(sizeof(kRule_47_conds)/sizeof(kRule_47_conds[0])), kRule_47_conds},
    {RewardType::Vehicle, "vp_lancer", false, int(sizeof(kRule_48_conds)/sizeof(kRule_48_conds[0])), kRule_48_conds},
    {RewardType::Vehicle, "vp_skylinea", false, int(sizeof(kRule_49_conds)/sizeof(kRule_49_conds[0])), kRule_49_conds},
    {RewardType::Vehicle, "vp_saleen", false, int(sizeof(kRule_50_conds)/sizeof(kRule_50_conds[0])), kRule_50_conds},
    {RewardType::Vehicle, "vp_vanquish", false, int(sizeof(kRule_51_conds)/sizeof(kRule_51_conds[0])), kRule_51_conds},
    {RewardType::Vehicle, "vp_pariscop", false, int(sizeof(kRule_52_conds)/sizeof(kRule_52_conds[0])), kRule_52_conds},
    {RewardType::Vehicle, "vp_cop_l", false, int(sizeof(kRule_53_conds)/sizeof(kRule_53_conds[0])), kRule_53_conds},
    {RewardType::Vehicle, "vp_tokyocop", false, int(sizeof(kRule_54_conds)/sizeof(kRule_54_conds[0])), kRule_54_conds},
    {RewardType::Vehicle, "vp_slf450x", false, int(sizeof(kRule_55_conds)/sizeof(kRule_55_conds[0])), kRule_55_conds},
    {RewardType::SlipstreamTurbo, "0", true, int(sizeof(kRule_56_conds)/sizeof(kRule_56_conds[0])), kRule_56_conds},
    {RewardType::SlipstreamTurbo, "0", false, int(sizeof(kRule_57_conds)/sizeof(kRule_57_conds[0])), kRule_57_conds},
    {RewardType::Burnout, "0", true, int(sizeof(kRule_58_conds)/sizeof(kRule_58_conds[0])), kRule_58_conds},
    {RewardType::Burnout, "0", false, int(sizeof(kRule_59_conds)/sizeof(kRule_59_conds[0])), kRule_59_conds},
    {RewardType::InAirControl, "0", true, int(sizeof(kRule_60_conds)/sizeof(kRule_60_conds[0])), kRule_60_conds},
    {RewardType::InAirControl, "0", false, int(sizeof(kRule_61_conds)/sizeof(kRule_61_conds[0])), kRule_61_conds},
    {RewardType::TwoWheelDrive, "0", true, int(sizeof(kRule_62_conds)/sizeof(kRule_62_conds[0])), kRule_62_conds},
    {RewardType::TwoWheelDrive, "0", false, int(sizeof(kRule_63_conds)/sizeof(kRule_63_conds[0])), kRule_63_conds},
    {RewardType::Nitro, "0", false, int(sizeof(kRule_64_conds)/sizeof(kRule_64_conds[0])), kRule_64_conds},
};


int GetTotalHookmenCount() { return 24; }

int GetHookmenCount(CityId city) {
    return 8;
}

const HookmanDef* GetHookman(CityId city, int index) {
    if (index < 0 || index >= 8) return nullptr;
    switch (city) {
        case CityId::LosAngeles: return kLaHookmen[index];
        case CityId::Paris: return kParisHookmen[index];
        case CityId::Tokyo: return kTokyoHookmen[index];
        default: return nullptr;
    }
}

const HookmanDef* GetHookmanByGlobalIndex(int global_idx) {
    if (global_idx < 0 || global_idx >= 24) return nullptr;
    return kAllHookmen[global_idx];
}

static bool StrEqualsCaseInsensitive(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    }
    return true;
}

const HookmanDef* FindHookman(std::string_view name) {
    for (const auto* hm : kAllHookmen) {
        if (StrEqualsCaseInsensitive(hm->name, name)) return hm;
    }
    return nullptr;
}

int GetTotalRacesCount() { return 56; }

const RaceDef* FindRace(std::string_view name) {
    for (const auto* hm : kAllHookmen) {
        for (int i = 0; i < hm->race_count; ++i) {
            if (StrEqualsCaseInsensitive(hm->races[i].name, name)) {
                return &hm->races[i];
            }
        }
    }
    return nullptr;
}

const FollowDef* FindFollow(std::string_view follow_race_name) {
    for (const auto* hm : kAllHookmen) {
        for (int i = 0; i < hm->follow_count; ++i) {
            if (StrEqualsCaseInsensitive(hm->follows[i].follow_race, follow_race_name)) {
                return &hm->follows[i];
            }
        }
    }
    return nullptr;
}

int GetTotalMoviesCount() { return int(sizeof(kAllMovies)/sizeof(kAllMovies[0])); }

const MovieDef* FindMovie(MovieType type, std::string_view target_race) {
    for (const auto& mv : kAllMovies) {
        if (mv.type == type) {
            if (mv.target_race == nullptr && target_race.empty()) return &mv;
            if (mv.target_race != nullptr && StrEqualsCaseInsensitive(mv.target_race, target_race)) {
                return &mv;
            }
        }
    }
    return nullptr;
}

const MovieDef* GetPreCityMovie(CityId city) {
    if (city == CityId::LosAngeles) {
        for (const auto& mv : kAllMovies) {
            if (mv.type == MovieType::PreCity) return &mv;
        }
    }
    return nullptr;
}

std::span<const UnlockRule> GetUnlockRules() {
    return kAllUnlockRules;
}

const Mc3CarDef kMc3Cars[10] = {
    {u"1998 DODGE NEON (MC3)",         "Dodge Neon SRT-4",         0x541700u},
    {u"1995 VOLKSWAGEN GOLF (MC3)",     "VW Golf R32",              0x541700u + 0x108u * 1},
    {u"2004 MITSUBISHI ECLIPSE (MC3)",  "Mitsubishi Eclipse",       0x541700u + 0x108u * 2},
    {u"2004 MITSUBISHI LANCER (MC3)",   "Mitsubishi Lancer Evo",    0x541700u + 0x108u * 3},
    {u"2004 NISSAN 350Z (MC3)",         "Nissan 350Z",              0x541700u + 0x108u * 4},
    {u"1998 TOYOTA SUPRA (MC3)",        "Toyota Supra",             0x541700u + 0x108u * 5},
    {u"2002 NISSAN SKYLINE (MC3)",      "Nissan Skyline GT-R",      0x541700u + 0x108u * 6},
    {u"2003 CHEVROLET CORVETTE (MC3)",  "Corvette Z06",             0x541700u + 0x108u * 7},
    {u"2004 DUCATI 999R (MC3)",         "Ducati 999R",              0x541700u + 0x108u * 8},
    {u"1970 CHEVROLET EL CAMINO (MC3)", "Chevy El Camino",          0x541700u + 0x108u * 9},
};

static constexpr HookmanVehicleDef kHookmanVehicles[] = {
    // Los Angeles (City 0)
    {"Moses",       "vp_civica",  "Dodge Neon SRT-4",         0x541700u},
    {"Steven",      "vp_gto",     "Chevy El Camino",          0x541700u + 0x108u * 9},
    {"Maria",       "vp_integraa","Mitsubishi Eclipse",       0x541700u + 0x108u * 2},
    {"Angel",       "vp_civicb",  "Mitsubishi Lancer Evo",    0x541700u + 0x108u * 3},
    {"Gina",        "vp_cbr929a", "Ducati 999R",              0x541700u + 0x108u * 8},
    {"Hector",      "vp_nissanz", "Nissan 350Z",              0x541700u + 0x108u * 4},
    {"Dice",        "vp_viper",   "Corvette Z06",             0x541700u + 0x108u * 7},
    {"WorldChampLA","vp_saleen",  "Corvette Z06",             0x541700u + 0x108u * 7},
    {"Savo",        "vp_saleen",  "Corvette Z06",             0x541700u + 0x108u * 7},

    // Paris (City 1)
    {"Blog",        "vp_puma",    "VW Golf R32",              0x541700u + 0x108u * 1},
    {"Julie",       "vp_clio",    "Mitsubishi Eclipse",       0x541700u + 0x108u * 2},
    {"Primo",       "vp_elise",   "Nissan 350Z",              0x541700u + 0x108u * 4},
    {"Stephane",    "vp_cosworth","Mitsubishi Lancer Evo",    0x541700u + 0x108u * 3},
    {"Ian",         "vp_m5",      "Toyota Supra",             0x541700u + 0x108u * 5},
    {"Farid",       "vp_911",     "Corvette Z06",             0x541700u + 0x108u * 7},
    {"Parfait",     "vp_vanquish","Corvette Z06",             0x541700u + 0x108u * 7},

    // Tokyo (City 2)
    {"Ricky",       "vp_mr2",     "Nissan 350Z",              0x541700u + 0x108u * 4},
    {"Shing",       "vp_supraa",  "Toyota Supra",             0x541700u + 0x108u * 5},
    {"Haley",       "vp_yamaha",  "Ducati 999R",              0x541700u + 0x108u * 8},
    {"Nikko",       "vp_lancer",  "Mitsubishi Lancer Evo",    0x541700u + 0x108u * 3},
    {"Zen",         "vp_suprab",  "Toyota Supra",             0x541700u + 0x108u * 5},
    {"Kenichi",     "vp_skylinea","Nissan Skyline GT-R",      0x541700u + 0x108u * 6},
    {"Makoto",      "vp_skylinea","Nissan Skyline GT-R",      0x541700u + 0x108u * 6},
};

const HookmanVehicleDef* GetHookmanVehicleDef(std::string_view hookman_name) {
    for (const auto& hv : kHookmanVehicles) {
        if (StrEqualsCaseInsensitive(hv.hookman_name, hookman_name)) return &hv;
    }
    // Default fallback
    return &kHookmanVehicles[0];
}

uint32_t GetHookmanVehicleRecord(std::string_view hookman_name) {
    const auto* def = GetHookmanVehicleDef(hookman_name);
    return def ? def->mc3_record_va : 0x541700u;
}

const char* GetHookmanVehicleName(std::string_view hookman_name) {
    const auto* def = GetHookmanVehicleDef(hookman_name);
    return def ? def->mc3_car_name : "Dodge Neon SRT-4";
}

const VehicleRewardMapping* FindVehicleReward(std::string_view mc2_veh) {
    for (const auto& vr : kVehicleRewards) {
        if (StrEqualsCaseInsensitive(vr.mc2_veh, mc2_veh)) return &vr;
    }
    return nullptr;
}

} // namespace mc2
