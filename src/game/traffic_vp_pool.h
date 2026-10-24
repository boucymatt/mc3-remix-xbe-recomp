#pragma once
// Originally generated from an inventory of the game's vp_* vehicles by a script that is not in this repository yet.
// Keep the entries sorted by vehicle name and re-run the Cruise probe sweep before setting validated = true.
// The vp_* cars the enhanced Cruise traffic pool may build (bikes left out). profile is the opponent profile
// name (the vehicle without vp_; mcCarConfig keeps 12 characters); native_defaults keeps the type's own profile
// (police); police cars are skipped by a random draw; validated = passed the strict Cruise probe sweep.
#include <cstddef>

struct TrafficVpModel {
    const char* vehicle;
    const char* profile;
    const char* vehicle_class;
    bool native_defaults;
    bool police;
    bool validated;
    unsigned presets;  // numbered .carcfg presets (vp_x_1..vp_x_N) beside the plain vp_x.carcfg
};

inline constexpr TrafficVpModel kTrafficVpPool[] = {
    {"vp_350z_04", "350z_04", "CULTURE", false, false, true, 10},
    {"vp_belair_57", "belair_57", "MUSCLE", false, false, true, 20},
    {"vp_cad16_04", "cad16_04", "LUXURY", false, false, true, 10},
    {"vp_charger_69", "charger_69", "MUSCLE", false, false, true, 10},
    {"vp_charger_99", "charger_99", "CULTURE", false, false, true, 10},
    {"vp_chrysler300c_05", "chrysler300c_05", "LUXURY", false, false, true, 20},
    {"vp_cien_02", "cien_02", "PERFORMANCE", false, false, true, 10},
    {"vp_cl500_04", "cl500_04", "LUXURY", false, false, true, 10},
    {"vp_cl55_amg_04", "cl55_amg_04", "LUXURY", false, false, true, 10},
    {"vp_clk_gtr_03", "clk_gtr_03", "PERFORMANCE", false, false, true, 10},
    {"vp_corvette_68", "corvette_68", "MUSCLE", false, false, true, 10},
    {"vp_corvettez06_03", "corvettez06_03", "CULTURE", false, false, true, 10},
    {"vp_ctsv_04", "ctsv_04", "LUXURY", false, false, true, 10},
    {"vp_d_charger_06", "d_charger_06", "CULTURE", false, false, true, 10},
    {"vp_d_chevelle_70", "d_chevelle_70", "MUSCLE", false, false, true, 10},
    {"vp_d_cls55_05", "d_cls55_05", "LUXURY", false, false, true, 10},
    {"vp_d_cobalt_05", "d_cobalt_05", "CULTURE", false, false, true, 10},
    {"vp_d_corvette_63", "d_corvette_63", "MUSCLE", false, false, true, 10},
    {"vp_d_deville_66", "d_deville_66", "MUSCLE", false, false, true, 10},
    {"vp_d_diablo_98", "d_diablo_98", "PERFORMANCE", false, false, true, 10},
    {"vp_d_eldorado_70", "d_eldorado_70", "MUSCLE", false, false, true, 10},
    {"vp_d_f355_98", "d_f355_98", "PERFORMANCE", false, false, true, 10},
    {"vp_d_fx45_05", "d_fx45_05", "SUV", false, false, true, 10},
    {"vp_d_g35_05", "d_g35_05", "LUXURY", false, false, true, 10},
    {"vp_d_gt750_04", "d_gt750_04", "SUV", false, false, true, 10},
    {"vp_d_h3t_05", "d_h3t_05", "SUV", false, false, true, 10},
    {"vp_d_magnum_05", "d_magnum_05", "LUXURY", false, false, true, 10},
    {"vp_d_scion_tc_05", "d_scion_tc_05", "CULTURE", false, false, true, 10},
    {"vp_d_sportconcept_05", "d_sportconcept_05", "CULTURE", false, false, true, 10},
    {"vp_d_ssr_05", "d_ssr_05", "SUV", false, false, true, 10},
    {"vp_d_superhemi_01", "d_superhemi_01", "LUXURY", false, false, true, 10},
    {"vp_d_tokyo_cop_01", "d_tokyo_cop_01", "COPCAR", true, true, true, 0},
    {"vp_d_xlr_04", "d_xlr_04", "LUXURY", false, false, true, 10},
    {"vp_d_yukon_05", "d_yukon_05", "SUV", false, false, true, 10},
    {"vp_d_zonda_04", "d_zonda_04", "PERFORMANCE", false, false, true, 10},
    {"vp_eclipse_04", "eclipse_04", "CULTURE", false, false, true, 10},
    {"vp_elcamino_70", "elcamino_70", "MUSCLE", false, false, true, 20},
    {"vp_elise_04", "elise_04", "CULTURE", false, false, true, 10},
    {"vp_escalade_03", "escalade_03", "SUV", false, false, true, 10},
    {"vp_esprit_04", "esprit_04", "CULTURE", false, false, true, 10},
    {"vp_ext_03", "ext_03", "SUV", false, false, true, 20},
    {"vp_fleetline_49", "fleetline_49", "MUSCLE", false, false, true, 10},
    {"vp_g500_03", "g500_03", "SUV", false, false, true, 10},
    {"vp_g55_amg_03", "g55_amg_03", "SUV", false, false, true, 10},
    {"vp_gallardo_04", "gallardo_04", "PERFORMANCE", false, false, true, 10},
    {"vp_gemballa_04", "gemballa_04", "PERFORMANCE", false, false, true, 10},
    {"vp_golfr32_04", "golfr32_04", "CULTURE", false, false, true, 10},
    {"vp_gs430_04", "gs430_04", "LUXURY", false, false, true, 10},
    {"vp_gto_68", "gto_68", "MUSCLE", false, false, true, 10},
    {"vp_gto_70", "gto_70", "MUSCLE", false, false, true, 10},
    {"vp_h2_03", "h2_03", "SUV", false, false, true, 10},
    {"vp_hummer_02", "hummer_02", "SUV", false, false, true, 10},
    {"vp_impala_64", "impala_64", "MUSCLE", false, false, true, 20},
    {"vp_impalass_96", "impalass_96", "MUSCLE", false, false, true, 10},
    {"vp_impalass_cop_a_96", "impalass_cop_a_96", "COPCAR", true, true, true, 0},
    {"vp_impalass_cop_d_96", "impalass_cop_d_96", "COPCAR", true, true, true, 0},
    {"vp_impalass_cop_s_96", "impalass_cop_s_96", "COPCAR", true, true, true, 0},
    {"vp_is300_04", "is300_04", "CULTURE", false, false, true, 10},
    {"vp_jetta_03", "jetta_03", "CULTURE", false, false, true, 10},
    {"vp_lancer_04", "lancer_04", "CULTURE", false, false, true, 10},
    {"vp_mclaren_f1_95", "mclaren_f1_95", "PERFORMANCE", false, false, true, 10},
    {"vp_me412_04", "me412_04", "PERFORMANCE", false, false, true, 10},
    {"vp_montecarlo_78", "montecarlo_78", "MUSCLE", false, false, true, 20},
    {"vp_murcielago_04", "murcielago_04", "PERFORMANCE", false, false, true, 10},
    {"vp_phaeton_04", "phaeton_04", "LUXURY", false, false, true, 10},
    {"vp_ram_04", "ram_04", "SUV", false, false, true, 10},
    {"vp_saleen_sr_00", "saleen_sr_00", "CULTURE", false, false, true, 10},
    {"vp_saleens7_04", "saleens7_04", "PERFORMANCE", false, false, true, 10},
    {"vp_sc430_04", "sc430_04", "LUXURY", false, false, true, 10},
    {"vp_silverado_ss_04", "silverado_ss_04", "SUV", false, false, true, 10},
    {"vp_skyline_02", "skyline_02", "CULTURE", false, false, true, 10},
    {"vp_sl500_04", "sl500_04", "LUXURY", false, false, true, 10},
    {"vp_sl55_amg_04", "sl55_amg_04", "LUXURY", false, false, true, 10},
    {"vp_slr_mclaren_04", "slr_mclaren_04", "PERFORMANCE", false, false, true, 10},
    {"vp_srt4_04", "srt4_04", "CULTURE", false, false, true, 10},
    {"vp_supra_98", "supra_98", "CULTURE", false, false, true, 10},
    {"vp_viper_03", "viper_03", "CULTURE", false, false, true, 10},
    {"vp_z28_69", "z28_69", "MUSCLE", false, false, true, 10},
    {"vp_z28_81", "z28_81", "MUSCLE", false, false, true, 10},
};
inline constexpr std::size_t kTrafficVpPoolSize = 79;
