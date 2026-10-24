#include "mc2_career_bridge.h"
#include "mc2_career_state.h"
#include "mc2_career_data.h"
#include "mc2_career_av.h"
#include "mc2_hookman_bridge.h"
#include "cpu/x86_ops.h"
#include "core/log.h"
#include "debug_tools.h"
#include "kernel/memory.h"
#include <cwchar>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <initializer_list>

X86_EXTERN_FUNC(sub_0026B8E0); // mcRaceConfig::InitFromCityRace
X86_EXTERN_FUNC(sub_00272010); // mcRaceConfigCar::SetVehicleFromRecord
X86_EXTERN_FUNC(sub_002D27C0); // mcGame::SetState
X86_EXTERN_FUNC(sub_0015AC90); // mcMoviePlayer::Stop

namespace mc2 {

static inline uint32_t GuestCall(X86Context& ctx, uint8_t* base, void (*fn)(X86Context&, uint8_t*), uint32_t ecx, std::initializer_list<uint32_t> args) {
    uint32_t eax = ctx.eax, ecx0 = ctx.ecx, edx = ctx.edx, esp = ctx.esp;
    uint32_t cf = ctx.cf, zf = ctx.zf, sf = ctx.sf, of = ctx.of, pf = ctx.pf;
    for (auto it = std::rbegin(args); it != std::rend(args); ++it) X86_PUSH32(*it);
    X86_PUSH32(0);  // return address: ret only pops it
    ctx.ecx = ecx;
    fn(ctx, base);
    uint32_t r = ctx.eax;
    ctx.eax = eax, ctx.ecx = ecx0, ctx.edx = edx, ctx.esp = esp;
    ctx.cf = cf, ctx.zf = zf, ctx.sf = sf, ctx.of = of, ctx.pf = pf;
    return r;
}

static bool g_bridge_career_mode = false;
static bool g_debug_stage_override = false;
static int g_debug_stage_value = -1;
static bool g_waiting_for_city_movie = false;
static std::u16string g_stage_label_buf;
static std::string g_career_save_path = "mc2_career_progress.dat";

static constexpr uint32_t kCityRecords[3] = {13, 10, 11}; // LA, Paris, Tokyo
static constexpr const char* kCityNames[3] = {"losangeles", "paris", "tokyo_mc2"};


bool IsCareerModeActive() {
    return g_bridge_career_mode;
}

void SetCareerModeActive(bool active) {
    g_bridge_career_mode = active;
}

int GetCurrentCareerCity() {
    return GetCareerState().GetCurrentCityIndex();
}

int GetCurrentCareerCar() {
    return GetCareerState().GetSelectedCar();
}

const char* CareerSavePath() {
    return g_career_save_path.c_str();
}

bool TryStartDeferredCityMovie(X86Context& ctx, uint8_t* base, bool city_first_draw_ready) {
    if (!g_bridge_career_mode || !g_waiting_for_city_movie || !city_first_draw_ready) return false;
    auto& sm = GetCareerState();
    if (sm.GetState() != CareerState::PreCityMovie || IsMoviePlaying()) return false;

    const char* movie = sm.GetPendingMovie();
    if (movie && movie[0] && PlayCareerMovie(ctx, base, movie)) {
        g_waiting_for_city_movie = false;
        uint32_t game = X86_MEM32(0x557ED0);
        if (game) X86_MEM32(game + 4) = 4;
        GuestCall(ctx, base, sub_002D27C0, 0, {4, 0});
        sm.SaveToFile(CareerSavePath());
        LOGE("MC2 Career Bridge: deferred pre-city movie '%s' started after the first city draw", movie);
        return true;
    }

    g_waiting_for_city_movie = false;
    LOGE("MC2 Career Bridge: deferred pre-city movie could not start; continuing career without it");
    sm.OnPreCityMovieDone();
    InitHookmanBridge();
    sm.SaveToFile(CareerSavePath());
    return false;
}

void CareerBridgeInit() {
    InitCareerAv();
    if (const char* save_path = std::getenv("MC3_MC2_SAVE_PATH"); save_path && save_path[0]) {
        g_career_save_path = save_path;
        LOGE("MC2 Career Bridge: save path='%s' (MC3_MC2_SAVE_PATH)", CareerSavePath());
    } else {
        g_career_save_path = "mc2_career_progress.dat";
        LOGE("MC2 Career Bridge: save path='%s' (manual/default)", CareerSavePath());
    }
    auto& career = GetCareerState();
    if (Mc3DebugHasArg(L"--mc3-mc2-new-career")) {
        career.StartNewCampaign();
        LOGE("MC2 Career Bridge: forced a new career with --mc3-mc2-new-career");
    } else if (!career.ResumeFromSave(CareerSavePath())) {
        career.StartNewCampaign();
        LOGE("MC2 Career Bridge: no valid save found; started a new career");
    }
    if (const wchar_t* stage = Mc3DebugArg(L"--mc3-mc2-stage")) {
        g_debug_stage_override = true;
        g_debug_stage_value = _wtoi(stage);
        LOGE("MC2 Career Bridge: debug stage override active = %d", g_debug_stage_value);
    }
}

static int FindCityRaceIndex(CityId city, const char* race_name) {
    if (!race_name) return 0;
    // Map of known race names in installed .loc files
    // In LA:
    // 0: Cruise
    // 1: Moses 1, 2: Moses 2, 3: Steven 1, 4: Steven 2, 5: Maria 1, 6: Maria 2, ...
    const auto* r = FindRace(race_name);
    if (!r) return 0;

    int city_idx = static_cast<int>(city);
    int race_counter = 1;
    for (int h = 0; h < 8; ++h) {
        const auto* hm = GetHookman(city, h);
        if (!hm) continue;
        for (int ri = 0; ri < hm->race_count; ++ri) {
            if (&hm->races[ri] == r || strcmp(hm->races[ri].name, race_name) == 0) {
                return race_counter;
            }
            if (hm->races[ri].is_supported) {
                race_counter++;
            }
        }
    }
    return 1;
}

const char16_t* GetCurrentStageLabel() {
    auto& sm = GetCareerState();
    const auto* hm = sm.GetCurrentHookman();
    const auto* r = sm.GetCurrentRace();

    std::string s;
    if (sm.GetState() == CareerState::CareerCruise || !hm) {
        s = "CRUISE: " + std::string(kCityNames[sm.GetCurrentCityIndex()]) + " ROAM";
    } else if (r) {
        s = std::string(hm->name) + " #" + std::to_string(sm.GetCurrentRaceIndex() + 1) + ": " + std::string(r->name);
    } else {
        s = std::string(hm->name) + " (AVAILABLE)";
    }

    g_stage_label_buf.assign(s.begin(), s.end());
    return g_stage_label_buf.c_str();
}

bool HandleSetupNextCareerEvent(X86Context& ctx, uint8_t* base) {
    if (!g_bridge_career_mode) return false;

    auto& sm = GetCareerState();
    uint32_t career_esi = ctx.ecx;

    int city_idx = sm.GetCurrentCityIndex();
    uint32_t rec = kCityRecords[city_idx];

    // Determine target stage and cruise status:
    int stage_idx = 0;
    bool is_cruise = true;
    const char* rname = "cruise";

    if (g_debug_stage_override) {
        stage_idx = g_debug_stage_value;
        is_cruise = (stage_idx <= 0);
        if (!is_cruise) {
            sm.SelectStage(stage_idx);
            const auto* race = sm.GetCurrentRace();
            if (race) rname = race->name;
        }
    } else {
        is_cruise = (sm.GetState() == CareerState::CareerCruise ||
                     sm.GetState() == CareerState::PreCityMovie ||
                     sm.GetState() == CareerState::HookmanAvailable ||
                     sm.GetState() == CareerState::FollowActive);
        if (!is_cruise) {
            const auto* race = sm.GetCurrentRace();
            if (race) {
                stage_idx = FindCityRaceIndex(sm.GetCurrentCity(), race->name);
                rname = race->name;
            } else {
                is_cruise = true;
            }
        }
    }

    const char* pending_movie = nullptr;
    if (!g_debug_stage_override) {
        if (sm.GetState() == CareerState::PreCityMovie || sm.GetState() == CareerState::PreRaceMovie) {
            pending_movie = sm.GetPendingMovie();
        }
    }

    uint32_t rc = X86_MEM32(0x53F2E4);
    if (rc) {
        int car_idx = sm.GetSelectedCar();
        if (car_idx < 0 || car_idx >= int(std::size(kMc3Cars))) car_idx = 0;

        // Check if forced vehicle exists for this race
        const auto* race = sm.GetCurrentRace();
        if (race && race->forced_vehicle) {
            const auto* vmap = FindVehicleReward(race->forced_vehicle);
            if (vmap) {
                // Find matching MC3 car
                for (size_t c = 0; c < std::size(kMc3Cars); ++c) {
                    if (strcmp(kMc3Cars[c].name, vmap->mc3_veh) == 0) {
                        car_idx = int(c);
                        break;
                    }
                }
            }
        }

        uint32_t race_arg = is_cruise ? 0xFFFFFFFFu : uint32_t(stage_idx);
        GuestCall(ctx, base, sub_0026B8E0, rc, {rec, race_arg});

        // 1. Configure Player vehicle (Slot 0) AFTER InitFromCityRace
        if (X86_MEM32(rc + 0x120)) {
            GuestCall(ctx, base, sub_00272010, X86_MEM32(rc + 0x120), {kMc3Cars[car_idx].record_va});
            X86_MEM8(0x53F2FC) = 1;
            X86_MEM8(0x50F131) = 0;
        }

        // 2. Configure Hookman opponent vehicle (Slot 1) AFTER InitFromCityRace
        const auto* hm = sm.GetCurrentHookman();
        const char* hm_name = hm ? hm->name : (city_idx == 0 ? "Moses" : city_idx == 1 ? "Blog" : "Ricky");
        uint32_t hm_veh_record = GetHookmanVehicleRecord(hm_name);
        const char* hm_veh_name = GetHookmanVehicleName(hm_name);

        uint32_t slot1 = X86_MEM32(rc + 0x120 + 0x2C);
        if (slot1) {
            GuestCall(ctx, base, sub_00272010, slot1, {hm_veh_record});
            LOGE("MC2 Career Bridge: Opponent Slot 1 (Hookman '%s') configured with vehicle '%s' (0x%08X)",
                 hm_name, hm_veh_name, hm_veh_record);
        }

        X86_MEM32(rc + 0x0C) = 4;
        X86_MEM32(rc + 0x10) = 4;
        X86_MEM32(rc + 0x3D4) = 0xFFFFFFFFu;
        X86_MEM32(rc + 0x40) = 8;
        X86_MEM8(rc + 0x10F) = 1;

        const char* actual_rname = reinterpret_cast<const char*>(base + rc + 0x68);
        LOGE("MC2 Career: launching city=%s (record %u), stage=%d, vehicle=%s, race='%s'",
             kCityNames[city_idx], rec, stage_idx, kMc3Cars[car_idx].name, actual_rname);
        LOGE("MC2 Career Bridge: mc2_race='%s'", rname);
    }

    X86_MEM32(career_esi + 0x2C) = is_cruise ? 3u : 4u;
    X86_MEM32(career_esi + 0x30) = rec;

    // Check if video playback should be started
    if (pending_movie && pending_movie[0]) {
        if (sm.GetState() == CareerState::PreCityMovie) {
            g_waiting_for_city_movie = true;
            sm.SaveToFile(CareerSavePath());
            LOGE("MC2 Career Bridge: pre-city movie '%s' deferred until the first city draw", pending_movie);
            return true;
        }
        if (rc) {
            char* movie_buf = reinterpret_cast<char*>(base + rc + 0xE8);
            std::strncpy(movie_buf, pending_movie, 31);
            movie_buf[31] = '\0';
        }
        bool movie_ok = PlayCareerMovie(ctx, base, pending_movie);
        if (movie_ok) {
            uint32_t game = X86_MEM32(0x557ED0);
            if (game) {
                X86_MEM32(game + 4) = 4;
            }
            GuestCall(ctx, base, sub_002D27C0, 0, {4, 0});
            LOGE("MC2 Career Bridge: started movie '%s' and transitioned mcGame to state 4; career transition is waiting for playback completion", pending_movie);
            sm.SaveToFile(CareerSavePath());
            return true;
        }
        LOGE("MC2 Career Bridge: movie '%s' could not start; continuing without it", pending_movie);
    }

    if (sm.GetState() == CareerState::PreCityMovie) {
        sm.OnPreCityMovieDone();
    } else if (sm.GetState() == CareerState::PreRaceMovie) {
        sm.OnPreRaceMovieDone();
    }

    if (!is_cruise) {
        ResolvedAudioSample sample;
        ResolveAudioPlaylist(rname, "pre", sample);
        sm.OnRaceStarted();
    } else {
        InitHookmanBridge();
    }

    sm.SaveToFile(CareerSavePath());
    return true;
}

bool HandleReportRaceResult(X86Context& ctx, uint8_t* base) {
    if (!g_bridge_career_mode) return false;

    uint32_t result_code = X86_MEM32(ctx.esp + 4);
    LOGE("MC2 Career Bridge: ReportRaceResult received code=%u", result_code);

    auto& sm = GetCareerState();
    const auto* race = sm.GetCurrentRace();
    std::string race_name = race ? race->name : "";

    sm.OnRaceResult(int(result_code));

    if (result_code == 0) {
        ResolvedAudioSample sample;
        ResolveAudioPlaylist(race_name.c_str(), "end_win", sample);
        if (const char* movie_key = sm.GetPendingMovie()) {
            if (movie_key[0] && PlayCareerMovie(ctx, base, movie_key)) {
                uint32_t game = X86_MEM32(0x557ED0);
                if (game) X86_MEM32(game + 4) = 4;
                GuestCall(ctx, base, sub_002D27C0, 0, {4, 0});
                LOGE("MC2 Career Bridge: post-race movie '%s' started; reward transition is waiting for playback completion", movie_key);
            }
        }
    } else {
        ResolvedAudioSample sample;
        ResolveAudioPlaylist(race_name.c_str(), "end", sample);
    }

    // Save progression
    sm.SaveToFile(CareerSavePath());
    return true;
}

bool HandleSelectHookmanRaceFromFollow(X86Context& ctx, uint8_t* base) {
    if (!g_bridge_career_mode) return false;

    auto& sm = GetCareerState();
    sm.OnFollowFinishedSuccess();
    sm.SaveToFile(CareerSavePath());
    LOGE("MC2 Career Bridge: Follow completed, selected next race");
    return true;
}

void HandleCareerMovieTick(X86Context& ctx, uint8_t* base,
                           uint32_t game_state, uint32_t movie_player, uint16_t movie_flags) {
    if (!g_bridge_career_mode || !IsMoviePlaying()) return;

    // mcMoviePlayer::Play sets +0x20 while Bink is active.  Its state-4 update
    // clears that byte and sets +0x21 when playback ends or the user skips it.
    bool guest_playing = movie_player != 0 && (movie_flags & 0x0001u) != 0;
    if (guest_playing && game_state != 4) {
        // On a skip, mcGame changes state before mcMoviePlayer::Update gets its
        // next state-4 tick.  Finish the same cleanup explicitly so +0x20 does
        // not remain stuck and block the following career movie.
        GuestCall(ctx, base, sub_0015AC90, movie_player, {});
        movie_flags = uint16_t(X86_MEM8(movie_player + 0x20)) |
                      (uint16_t(X86_MEM8(movie_player + 0x21)) << 8);
        guest_playing = (movie_flags & 0x0001u) != 0;
        LOGE("MC2 Career Bridge: stopped skipped movie after mcGame left state 4; flags=%04X",
             unsigned(movie_flags));
    }
    if (guest_playing) return;

    auto& sm = GetCareerState();
    const CareerState completed = sm.GetState();
    OnMovieFinished();

    switch (completed) {
        case CareerState::PreCityMovie:
            sm.OnPreCityMovieDone();
            InitHookmanBridge();
            break;
        case CareerState::PreRaceMovie:
            sm.OnPreRaceMovieDone();
            sm.OnRaceStarted();
            break;
        case CareerState::PostRaceMovie:
            sm.OnPostRaceMovieDone();
            break;
        default:
            LOGE("MC2 Career Bridge: movie ended in unexpected career state=%s game_state=%u",
                 ToString(completed), game_state);
            break;
    }
    sm.SaveToFile(CareerSavePath());
    LOGE("MC2 Career Bridge: playback completion applied for career state=%s game_state=%u flags=%04X",
         ToString(completed), game_state, unsigned(movie_flags));
}

} // namespace mc2
