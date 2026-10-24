#include "mc2_career_test.h"
#include "mc2_career_state.h"
#include "core/log.h"
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <cstddef>

namespace mc2 {

#define MC2_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            LOGE("[FAIL] Assertion failed: %s (%s:%d)", msg, __FILE__, __LINE__); \
            return 1; \
        } \
    } while (0)

static void BeatHookmanDirect(CareerStateMachine& sm, const char* hm_name);

static uint32_t TestChecksumBytes(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t sum = 0x811C9DC5u;
    for (size_t i = 12; i < size; ++i) {
        sum ^= bytes[i];
        sum *= 0x01000193u;
    }
    return sum;
}

static int TestInitialState() {
    CareerStateMachine sm;
    sm.StartNewCampaign();

    MC2_ASSERT(sm.GetCurrentCity() == CityId::LosAngeles, "Initial city must be Los Angeles");
    MC2_ASSERT(sm.GetState() == CareerState::PreCityMovie, "Initial state must be PreCityMovie");
    MC2_ASSERT(std::string(sm.GetPendingMovie()).find("moses_01.bik") != std::string::npos, "Pre-city movie must be moses_01.bik");

    // Moses is unlocked, others are locked
    MC2_ASSERT(sm.IsHookmanUnlocked(0), "Moses must be unlocked");
    for (int i = 1; i < 24; ++i) {
        MC2_ASSERT(!sm.IsHookmanUnlocked(i), "All hookmen other than Moses must be locked initially");
    }
    MC2_ASSERT(!sm.IsCampaignComplete(), "Campaign must not be complete initially");
    MC2_ASSERT(!sm.IsAbilityUnlocked(RewardType::Nitro), "Nitro must be locked initially");
    MC2_ASSERT(sm.IsCityUnlocked(CityId::LosAngeles), "LA must be unlocked initially");
    MC2_ASSERT(!sm.IsCityUnlocked(CityId::Paris), "Paris must be locked initially");
    MC2_ASSERT(!sm.IsCityUnlocked(CityId::Tokyo), "Tokyo must be locked initially");

    LOGI("[PASS] TestInitialState passed");
    return 0;
}

static int TestLossAndRetryNoProgression() {
    CareerStateMachine sm;
    sm.StartNewCampaign();
    sm.OnPreCityMovieDone();

    MC2_ASSERT(sm.GetState() == CareerState::CareerCruise, "Must enter cruise after intro movie");

    // Proximity to Moses -> HookmanAvailable
    bool prox = sm.OnHookmanProximityDetected(0);
    MC2_ASSERT(prox, "Must detect proximity to Moses");
    MC2_ASSERT(sm.GetState() == CareerState::HookmanAvailable, "State must be HookmanAvailable");

    // Headlight engage
    bool engage = sm.OnHookmanHeadlightEngaged(0);
    MC2_ASSERT(engage, "Headlight engage must succeed");
    MC2_ASSERT(sm.GetState() == CareerState::HookmanEngaged, "State must be HookmanEngaged");

    // Follow started -> finished
    sm.OnFollowStarted(0);
    MC2_ASSERT(sm.GetState() == CareerState::FollowActive, "State must be FollowActive");

    sm.OnFollowFinishedSuccess();
    // Moses race 1 selected
    MC2_ASSERT(sm.GetState() == CareerState::PreRaceMovie || sm.GetState() == CareerState::RaceSelected, "Race 1 must be selected");
    if (sm.GetState() == CareerState::PreRaceMovie) sm.OnPreRaceMovieDone();

    sm.OnRaceStarted();
    MC2_ASSERT(sm.GetState() == CareerState::RaceActive, "State must be RaceActive");

    // Loss simulation
    sm.OnRaceResult(1); // 1 = Defeat
    MC2_ASSERT(sm.GetState() == CareerState::RaceLostOrCancelled, "State must be RaceLostOrCancelled");
    MC2_ASSERT(!sm.IsRaceBeaten("lamauro_Moses_Checkpoint_One"), "Race 1 must NOT be marked beaten after defeat");
    MC2_ASSERT(sm.GetSaveData().hookman_race_idx[0] == 0, "Moses race index must NOT advance after defeat");
    MC2_ASSERT(!sm.IsHookmanBeaten("Moses"), "Moses must not be beaten");
    MC2_ASSERT(!sm.IsHookmanUnlocked("Steven"), "Steven must remain locked after defeat");
    MC2_ASSERT(!sm.IsHookmanUnlocked("Maria"), "Maria must remain locked after defeat");

    // Retry simulation
    sm.OnRaceSelected(0, 0);
    sm.OnRaceStarted();
    sm.OnRaceResult(2); // 2 = Cancelled / Retry
    MC2_ASSERT(sm.GetState() == CareerState::RaceLostOrCancelled, "State must be RaceLostOrCancelled after cancel");
    MC2_ASSERT(sm.GetSaveData().hookman_race_idx[0] == 0, "Moses race index must remain 0 after cancel");
    MC2_ASSERT(!sm.IsHookmanUnlocked("Steven"), "Steven must remain locked");

    LOGI("[PASS] TestLossAndRetryNoProgression passed");
    return 0;
}

static int TestMosesProgressionToMariaSteven() {
    CareerStateMachine sm;
    sm.StartNewCampaign();
    sm.OnPreCityMovieDone();
    sm.OnRaceSelected(0, 0); // Moses Race 1
    if (sm.GetState() == CareerState::PreRaceMovie) sm.OnPreRaceMovieDone();
    sm.OnRaceStarted();

    // Win Race 1
    sm.OnRaceResult(0);
    if (sm.GetState() == CareerState::PostRaceMovie) sm.OnPostRaceMovieDone();
    sm.OnRewardDismissed();

    MC2_ASSERT(sm.IsRaceBeaten("lamauro_Moses_Checkpoint_One"), "Race 1 must be beaten");
    MC2_ASSERT(sm.GetSaveData().hookman_race_idx[0] == 1, "Moses race index must advance to 1");
    MC2_ASSERT(!sm.IsHookmanBeaten("Moses"), "Moses must NOT be beaten yet (needs race 2)");
    MC2_ASSERT(!sm.IsHookmanUnlocked("Steven"), "Steven must remain locked before Moses is beaten");
    MC2_ASSERT(!sm.IsHookmanUnlocked("Maria"), "Maria must remain locked before Moses is beaten");

    // Race 2
    sm.OnRaceSelected(0, 1); // Moses Race 2
    if (sm.GetState() == CareerState::PreRaceMovie) sm.OnPreRaceMovieDone();
    sm.OnRaceStarted();

    // Win Race 2
    sm.OnRaceResult(0);
    if (sm.GetState() == CareerState::PostRaceMovie) sm.OnPostRaceMovieDone();
    sm.OnRewardDismissed();

    MC2_ASSERT(sm.IsRaceBeaten("lamauro_Moses_Checkpoint_Two"), "Race 2 must be beaten");
    MC2_ASSERT(sm.GetSaveData().hookman_race_idx[0] == 2, "Moses race index must be 2");
    MC2_ASSERT(sm.IsHookmanBeaten("Moses"), "Moses must now be beaten!");
    MC2_ASSERT(sm.IsAbilityUnlocked(RewardType::Nitro), "Nitro must be unlocked after beating Moses!");
    MC2_ASSERT(sm.IsVehicleUnlocked("vp_civica"), "Civic reward must unlock after beating Moses");
    MC2_ASSERT(sm.IsHookmanUnlocked("Steven"), "Steven must be unlocked after beating Moses!");
    MC2_ASSERT(sm.IsHookmanUnlocked("Maria"), "Maria must be unlocked after beating Moses!");
    MC2_ASSERT(!sm.IsHookmanUnlocked("Angel"), "Angel must remain locked (needs Steven)");
    MC2_ASSERT(!sm.IsHookmanUnlocked("Dice"), "Dice must remain locked");

    LOGI("[PASS] TestMosesProgressionToMariaSteven passed");
    return 0;
}

static int TestFollowRewardPersists() {
    CareerStateMachine sm;
    sm.StartNewCampaign();
    sm.OnPreCityMovieDone();
    BeatHookmanDirect(sm, "Moses");

    const auto* maria = FindHookman("Maria");
    MC2_ASSERT(maria && maria->follow_count > 0, "Maria follow definition must exist");
    MC2_ASSERT(!sm.IsAbilityUnlocked(RewardType::InAirControl), "IAC must be locked before Maria follow");
    MC2_ASSERT(sm.OnHookmanProximityDetected(maria->global_hookman_idx), "Maria must be available after Moses");
    MC2_ASSERT(sm.OnHookmanHeadlightEngaged(maria->global_hookman_idx), "Maria headlight engagement must succeed");
    sm.OnFollowStarted(0);
    sm.OnFollowFinishedSuccess();

    MC2_ASSERT(sm.IsRaceBeaten("lamauro_Maria_Follow"), "Maria follow must be recorded as completed");
    MC2_ASSERT(sm.IsAbilityUnlocked(RewardType::InAirControl), "IAC must unlock immediately after Maria follow");

    const std::string path = "mc2_career_follow_test_save.dat";
    MC2_ASSERT(sm.SaveToFile(path), "Failed to save follow progress");
    CareerStateMachine loaded;
    MC2_ASSERT(loaded.ResumeFromSave(path), "Failed to reload follow progress");
    MC2_ASSERT(loaded.GetSaveData().version == 2, "Follow progress requires save format v2");
    MC2_ASSERT(loaded.IsRaceBeaten("lamauro_Maria_Follow"), "Maria follow must survive reload");
    MC2_ASSERT(loaded.IsAbilityUnlocked(RewardType::InAirControl), "IAC reward must survive reload");

    std::filesystem::remove(path);
    LOGI("[PASS] TestFollowRewardPersists passed");
    return 0;
}

static void BeatHookmanDirect(CareerStateMachine& sm, const char* hm_name) {
    const auto* hm = FindHookman(hm_name);
    assert(hm);
    for (int r = 0; r < hm->race_count; ++r) {
        sm.OnRaceSelected(hm->global_hookman_idx, r);
        if (sm.GetState() == CareerState::PreRaceMovie) sm.OnPreRaceMovieDone();
        sm.OnRaceStarted();
        sm.OnRaceResult(0); // Win
        if (sm.GetState() == CareerState::PostRaceMovie) sm.OnPostRaceMovieDone();
        sm.OnRewardDismissed();
    }
}

static int TestFullCampaignGraphSimulation() {
    CareerStateMachine sm;
    sm.StartNewCampaign();
    sm.OnPreCityMovieDone();

    // 1. Beat Moses -> Maria and Steven
    BeatHookmanDirect(sm, "Moses");
    MC2_ASSERT(sm.IsHookmanUnlocked("Steven"), "Steven unlocked");
    MC2_ASSERT(sm.IsHookmanUnlocked("Maria"), "Maria unlocked");

    // 2. Beat Steven -> Angel
    BeatHookmanDirect(sm, "Steven");
    MC2_ASSERT(sm.IsHookmanUnlocked("Angel"), "Angel unlocked");
    MC2_ASSERT(!sm.IsHookmanUnlocked("Dice"), "Dice still locked");

    // 3. Beat Maria -> Gina and Hector
    BeatHookmanDirect(sm, "Maria");
    MC2_ASSERT(sm.IsHookmanUnlocked("Gina"), "Gina unlocked");
    MC2_ASSERT(sm.IsHookmanUnlocked("Hector"), "Hector unlocked");
    MC2_ASSERT(!sm.IsHookmanUnlocked("Dice"), "Dice still locked");

    // 4. Beat Angel, Gina, Hector -> Dice
    BeatHookmanDirect(sm, "Angel");
    BeatHookmanDirect(sm, "Gina");
    MC2_ASSERT(!sm.IsHookmanUnlocked("Dice"), "Dice still locked (needs Hector)");
    BeatHookmanDirect(sm, "Hector");
    MC2_ASSERT(sm.IsHookmanUnlocked("Dice"), "Dice must now be unlocked!");

    // 5. Beat Dice -> Paris and Blog
    BeatHookmanDirect(sm, "Dice");
    MC2_ASSERT(sm.IsCityUnlocked(CityId::Paris), "Paris must be unlocked after Dice!");
    MC2_ASSERT(sm.IsHookmanUnlocked("Blog"), "Blog must be unlocked after Dice!");

    // Transition to Paris
    sm.OnCityTransitionRequested(CityId::Paris);
    MC2_ASSERT(sm.GetCurrentCity() == CityId::Paris, "Current city must be Paris");

    // 6. Beat Blog -> Julie and Primo
    BeatHookmanDirect(sm, "Blog");
    MC2_ASSERT(sm.IsHookmanUnlocked("Julie"), "Julie unlocked");
    MC2_ASSERT(sm.IsHookmanUnlocked("Primo"), "Primo unlocked");

    // 7. Beat Julie -> Stephane
    BeatHookmanDirect(sm, "Julie");
    MC2_ASSERT(sm.IsHookmanUnlocked("Stephane"), "Stephane unlocked");

    // 8. Beat Primo -> Ian
    BeatHookmanDirect(sm, "Primo");
    MC2_ASSERT(sm.IsHookmanUnlocked("Ian"), "Ian unlocked");
    MC2_ASSERT(!sm.IsHookmanUnlocked("Farid"), "Farid still locked (needs Primo + Stephane)");

    // 9. Beat Stephane -> Farid unlocked
    BeatHookmanDirect(sm, "Stephane");
    MC2_ASSERT(sm.IsHookmanUnlocked("Farid"), "Farid must now be unlocked!");
    MC2_ASSERT(!sm.IsHookmanUnlocked("Parfait"), "Parfait still locked (needs Farid + Ian)");

    // 10. Beat Farid + Ian -> Parfait
    BeatHookmanDirect(sm, "Farid");
    BeatHookmanDirect(sm, "Ian");
    MC2_ASSERT(sm.IsHookmanUnlocked("Parfait"), "Parfait must now be unlocked!");

    // 11. Beat Parfait -> Tokyo, Ricky and Shing
    BeatHookmanDirect(sm, "Parfait");
    MC2_ASSERT(sm.IsCityUnlocked(CityId::Tokyo), "Tokyo must be unlocked after Parfait!");
    MC2_ASSERT(sm.IsHookmanUnlocked("Ricky"), "Ricky unlocked");
    MC2_ASSERT(sm.IsHookmanUnlocked("Shing"), "Shing unlocked");

    // Transition to Tokyo
    sm.OnCityTransitionRequested(CityId::Tokyo);
    MC2_ASSERT(sm.GetCurrentCity() == CityId::Tokyo, "Current city must be Tokyo");

    // 12. Beat Shing -> Haley
    BeatHookmanDirect(sm, "Shing");
    MC2_ASSERT(sm.IsHookmanUnlocked("Haley"), "Haley unlocked");
    MC2_ASSERT(!sm.IsHookmanUnlocked("Zen"), "Zen still locked (needs Ricky + Haley)");

    // 13. Beat Ricky -> Nikko
    BeatHookmanDirect(sm, "Ricky");
    MC2_ASSERT(sm.IsHookmanUnlocked("Nikko"), "Nikko unlocked");
    MC2_ASSERT(!sm.IsHookmanUnlocked("Zen"), "Zen still locked (Haley not beaten)");

    // 14. Beat Haley -> Zen
    BeatHookmanDirect(sm, "Haley");
    MC2_ASSERT(sm.IsHookmanUnlocked("Zen"), "Zen must now be unlocked!");
    MC2_ASSERT(!sm.IsHookmanUnlocked("Kenichi"), "Kenichi still locked (needs Haley + Nikko)");

    // 15. Beat Nikko -> Kenichi
    BeatHookmanDirect(sm, "Nikko");
    MC2_ASSERT(sm.IsHookmanUnlocked("Kenichi"), "Kenichi must now be unlocked!");
    MC2_ASSERT(!sm.IsHookmanUnlocked("Makoto"), "Makoto still locked (needs Kenichi + Zen)");

    // 16. Beat Zen + Kenichi -> Makoto
    BeatHookmanDirect(sm, "Zen");
    BeatHookmanDirect(sm, "Kenichi");
    MC2_ASSERT(sm.IsHookmanUnlocked("Makoto"), "Makoto must now be unlocked!");

    // 17. Beat Makoto -> WorldChampTokyo -> WorldChampParis -> WorldChampLA
    BeatHookmanDirect(sm, "Makoto");
    MC2_ASSERT(sm.IsHookmanUnlocked("WorldChampTokyo"), "WorldChampTokyo unlocked");

    BeatHookmanDirect(sm, "WorldChampTokyo");
    MC2_ASSERT(sm.IsHookmanUnlocked("WorldChampParis"), "WorldChampParis unlocked");

    BeatHookmanDirect(sm, "WorldChampParis");
    MC2_ASSERT(sm.IsHookmanUnlocked("WorldChampLA"), "WorldChampLA unlocked");
    MC2_ASSERT(!sm.IsCampaignComplete(), "Campaign must not be complete before WorldChampLA");

    BeatHookmanDirect(sm, "WorldChampLA");
    MC2_ASSERT(sm.IsCampaignComplete(), "Campaign must now be complete after WorldChampLA!");

    LOGI("[PASS] TestFullCampaignGraphSimulation passed");
    return 0;
}

static int TestSaveReloadRoundTrip() {
    CareerStateMachine sm1;
    sm1.StartNewCampaign();
    sm1.OnPreCityMovieDone();
    BeatHookmanDirect(sm1, "Moses");

    std::string test_save_path = "mc2_career_test_save.dat";
    bool saved = sm1.SaveToFile(test_save_path);
    MC2_ASSERT(saved, "Failed to save career state to file");

    CareerStateMachine sm2;
    bool loaded = sm2.ResumeFromSave(test_save_path);
    MC2_ASSERT(loaded, "Failed to load career state from file");

    MC2_ASSERT(sm2.GetSaveData().save_rev == sm1.GetSaveData().save_rev, "Save revision mismatch");
    MC2_ASSERT(sm2.GetSaveData().races_beaten_mask == sm1.GetSaveData().races_beaten_mask, "Races beaten mask mismatch");
    MC2_ASSERT(sm2.GetSaveData().hookmen_beaten_mask == sm1.GetSaveData().hookmen_beaten_mask, "Hookmen beaten mask mismatch");
    MC2_ASSERT(sm2.GetSaveData().unlocked_abilities == sm1.GetSaveData().unlocked_abilities, "Unlocked abilities mismatch");
    MC2_ASSERT(sm2.GetSaveData().unlocked_vehicles_mask == sm1.GetSaveData().unlocked_vehicles_mask, "Unlocked vehicles mismatch");
    MC2_ASSERT(sm2.GetSaveData().version == 2, "Save format must be v2");
    MC2_ASSERT(sm2.IsHookmanBeaten("Moses"), "Moses must remain beaten after reload");
    MC2_ASSERT(sm2.IsHookmanUnlocked("Steven"), "Steven must remain unlocked after reload");
    MC2_ASSERT(sm2.IsHookmanUnlocked("Maria"), "Maria must remain unlocked after reload");

    std::filesystem::remove(test_save_path);
    LOGI("[PASS] TestSaveReloadRoundTrip passed");
    return 0;
}

static int TestV1SaveMigration() {
    CareerStateMachine source;
    source.StartNewCampaign();
    source.OnPreCityMovieDone();
    BeatHookmanDirect(source, "Moses");

    CareerSaveData old = source.GetSaveData();
    old.version = 1;
    constexpr size_t old_size = offsetof(CareerSaveData, follows_beaten_mask);
    old.checksum = TestChecksumBytes(&old, old_size);

    const std::string path = "mc2_career_v1_migration_test.dat";
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(&old), old_size);
        MC2_ASSERT(file.good(), "Failed to create v1 migration fixture");
    }

    CareerStateMachine migrated;
    MC2_ASSERT(migrated.ResumeFromSave(path), "Failed to migrate v1 career save");
    MC2_ASSERT(migrated.GetSaveData().version == 2, "Migrated save must use v2");
    MC2_ASSERT(migrated.IsHookmanBeaten("Moses"), "Migration must preserve Hookman progress");
    MC2_ASSERT(migrated.IsVehicleUnlocked("vp_civica"), "Migration must rebuild vehicle rewards");
    MC2_ASSERT(std::filesystem::file_size(path) == sizeof(CareerSaveData), "Migrated save must be rewritten as v2");

    std::filesystem::remove(path);
    LOGI("[PASS] TestV1SaveMigration passed");
    return 0;
}

int RunCareerStateMachineTests() {
    LOGE("=== RUNNING MC2 CAREER STATE MACHINE DETERMINISTIC TESTS (GATE 2) ===");
    if (TestInitialState() != 0) return 1;
    if (TestLossAndRetryNoProgression() != 0) return 2;
    if (TestMosesProgressionToMariaSteven() != 0) return 3;
    if (TestFollowRewardPersists() != 0) return 4;
    if (TestFullCampaignGraphSimulation() != 0) return 5;
    if (TestSaveReloadRoundTrip() != 0) return 6;
    if (TestV1SaveMigration() != 0) return 7;
    LOGE("=== ALL GATE 2 TESTS PASSED SUCCESSFULLY ===");
    return 0;
}

} // namespace mc2
