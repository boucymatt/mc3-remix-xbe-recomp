#include "mc2_career_av.h"
#include "mc2_career_data.h"
#include "mc2_career_state.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/memory.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

// Guest functions for movie playback
X86_EXTERN_FUNC(sub_0015AA20); // mcMoviePlayer::Instance
X86_EXTERN_FUNC(sub_0015AA30); // mcMoviePlayer::Play(const char* path)
X86_EXTERN_FUNC(sub_0015B2D0); // mcMoviePlayer::Create

namespace mc2 {

// The folders of the user's own Midnight Club 2 files, set from the environment (MC3_MC2_DIR names MC2's assets_p,
// MC3_MC2_VIDEO_DIR its video folder). Empty until then, and every lookup below finds nothing while it is.
static std::string g_mc2_assets_dir;
static std::string g_mc2_video_dir;
static bool g_movie_playing = false;
static std::string g_current_movie_key;

static inline uint32_t GuestCall(X86Context& ctx, uint8_t* base, void (*fn)(X86Context&, uint8_t*), uint32_t ecx, std::initializer_list<uint32_t> args) {
    uint32_t eax = ctx.eax, ecx0 = ctx.ecx, edx = ctx.edx, esp = ctx.esp;
    uint32_t cf = ctx.cf, zf = ctx.zf, sf = ctx.sf, of = ctx.of, pf = ctx.pf;
    for (auto it = std::rbegin(args); it != std::rend(args); ++it) X86_PUSH32(*it);
    X86_PUSH32(0);
    ctx.ecx = ecx;
    fn(ctx, base);
    uint32_t r = ctx.eax;
    ctx.eax = eax, ctx.ecx = ecx0, ctx.edx = edx, ctx.esp = esp;
    ctx.cf = cf, ctx.zf = zf, ctx.sf = sf, ctx.of = of, ctx.pf = pf;
    return r;
}

void InitCareerAv() {
    if (const char* env_dir = std::getenv("MC3_MC2_DIR")) {
        g_mc2_assets_dir = env_dir;
    }
    if (const char* env_vdir = std::getenv("MC3_MC2_VIDEO_DIR")) {
        g_mc2_video_dir = env_vdir;
    }
    LOGE("MC2 Career AV: initialized. assets_dir='%s', video_dir='%s'",
         g_mc2_assets_dir.c_str(), g_mc2_video_dir.c_str());
}

std::string ResolveMoviePath(std::string_view pss_or_bik_key) {
    if (pss_or_bik_key.empty() || g_mc2_video_dir.empty()) return {};

    std::string rel(pss_or_bik_key);
    // Convert slashes to local separator
    for (char& c : rel) {
        if (c == '/') c = '\\';
    }

    // Change .pss to .bik
    size_t dot = rel.rfind('.');
    if (dot != std::string::npos) {
        rel.replace(dot, rel.size() - dot, ".bik");
    } else {
        rel += ".bik";
    }

    // Check directly inside video_dir
    fs::path p1 = fs::path(g_mc2_video_dir) / rel;
    std::error_code ec;
    if (fs::exists(p1, ec)) {
        return p1.string();
    }

    // Special check for intro
    if (rel.find("intro") != std::string::npos) {
        fs::path p_intro = fs::path(g_mc2_video_dir) / "mc2intro.bik";
        if (fs::exists(p_intro, ec)) {
            return p_intro.string();
        }
    }

    // Case-insensitive fallback lookup in video dir
    std::string rel_lower = rel;
    for (char& c : rel_lower) c = char(std::tolower(uint8_t(c)));

    for (const auto& entry : fs::recursive_directory_iterator(g_mc2_video_dir, ec)) {
        if (entry.is_regular_file()) {
            std::string fn = entry.path().filename().string();
            std::string fn_lower = fn;
            for (char& c : fn_lower) c = char(std::tolower(uint8_t(c)));
            if (rel_lower.ends_with(fn_lower)) {
                return entry.path().string();
            }
        }
    }

    return {};
}

bool PlayCareerMovie(X86Context& ctx, uint8_t* base, const char* movie_key) {
    if (!movie_key || !movie_key[0]) return false;

    std::string resolved = ResolveMoviePath(movie_key);
    if (resolved.empty()) {
        LOGE("MC2 Career AV: movie key='%s' not found on disk (open failure)", movie_key);
        return false;
    }

    LOGE("MC2 Career AV: playing movie key='%s' resolved_path='%s'", movie_key, resolved.c_str());

    uint32_t player = X86_MEM32(0x50F19C);
    if (!player) {
        GuestCall(ctx, base, sub_0015B2D0, 0, {});
        player = X86_MEM32(0x50F19C);
    }
    if (!player) {
        LOGE("MC2 Career AV: movie player not active (0x50F19C == 0), movie key='%s' deferred/skipped", movie_key);
        return false;
    }

    // Allocate temporary string buffer on guest heap
    static uint32_t movie_buf_va = 0;
    if (!movie_buf_va) {
        movie_buf_va = mem::AllocSystem(512, 16);
    }

    // Pass the relative path expected by mcMoviePlayer::Play, e.g. "la/moses_01.bik"
    std::string rel_path(movie_key);
    for (char& c : rel_path) {
        if (c == '\\') c = '/';
    }
    size_t len = std::min(rel_path.size(), size_t(500));
    std::memcpy(base + movie_buf_va, rel_path.c_str(), len);
    base[movie_buf_va + len] = '\0';

    // Call sub_0015AA30(path_va)
    uint32_t ok = GuestCall(ctx, base, sub_0015AA30, player, {movie_buf_va});
    g_movie_playing = (ok != 0);
    g_current_movie_key = movie_key;

    LOGE("MC2 Career AV: mcMoviePlayer::Play('%s') returned %u (player=%08X)", rel_path.c_str(), ok, player);
    return g_movie_playing;
}

bool IsMoviePlaying() {
    return g_movie_playing;
}

void OnMovieFinished() {
    if (g_movie_playing) {
        LOGE("MC2 Career AV: movie finished key='%s'", g_current_movie_key.c_str());
        g_movie_playing = false;
        g_current_movie_key.clear();
    }
}

bool ResolveAudioPlaylist(const char* race_name, const char* context, ResolvedAudioSample& out_sample) {
    out_sample = {};
    if (!race_name || !context || g_mc2_assets_dir.empty()) return false;

    std::string lower_race(race_name);
    for (char& c : lower_race) c = char(std::tolower(uint8_t(c)));

    std::string lower_ctx(context);
    for (char& c : lower_ctx) c = char(std::tolower(uint8_t(c)));

    // Form candidate filename: e.g. lamauro_moses_checkpoint_one_pre.play
    std::string candidate = lower_race + "_" + lower_ctx + ".play";
    fs::path play_path = fs::path(g_mc2_assets_dir) / "tune" / "audio" / "playlist" / candidate;

    std::error_code ec;
    if (!fs::exists(play_path, ec)) {
        // Try without city prefix or with Hookman subrace name
        for (const auto& entry : fs::directory_iterator(fs::path(g_mc2_assets_dir) / "tune" / "audio" / "playlist", ec)) {
            std::string fname = entry.path().filename().string();
            std::string fname_lower = fname;
            for (char& c : fname_lower) c = char(std::tolower(uint8_t(c)));
            if (fname_lower.find(lower_ctx) != std::string::npos && fname_lower.ends_with(".play")) {
                // Check if race parts match
                play_path = entry.path();
                candidate = fname;
                break;
            }
        }
    }

    if (!fs::exists(play_path, ec)) {
        LOGE("MC2 Career AV: audio playlist '%s' not found on disk", candidate.c_str());
        return false;
    }

    out_sample.playlist = candidate;

    // Parse .play file
    std::ifstream file(play_path);
    if (!file.is_open()) {
        LOGE("MC2 Career AV: failed to open playlist file '%s'", play_path.string().c_str());
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Trim whitespace
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start != std::string::npos) line = line.substr(start);

        if (line.empty() || line.starts_with("#") || line.starts_with("num_songs")) continue;

        // format is: bank/sample
        size_t slash = line.find('/');
        if (slash != std::string::npos) {
            out_sample.bank = line.substr(0, slash);
            out_sample.sample = line.substr(slash + 1);
            out_sample.is_playable = false; // sample bank not mounted in MC3 runtime (open failure)
            LOGE("MC2 Career AV: audio context=%s playlist=%s bank=%s sample=%s (bank not mounted - falha aberta)",
                 context, out_sample.playlist.c_str(), out_sample.bank.c_str(), out_sample.sample.c_str());
            return true;
        }
    }

    LOGE("MC2 Career AV: playlist '%s' had no valid song entries", candidate.c_str());
    return false;
}

} // namespace mc2
