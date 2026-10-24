#pragma once
#include <string>
#include <string_view>
#include <vector>
#include "cpu/x86_context.h"
#include "mc2_career_data.h"

namespace mc2 {

struct ResolvedAudioSample {
    std::string playlist;
    std::string bank;
    std::string sample;
    bool is_playable = false;
};

// Initialize AV subsystem with paths to assets and videos
void InitCareerAv();

// Resolve movie key (e.g. "la/moses_01.pss") to actual BIK host path
std::string ResolveMoviePath(std::string_view pss_or_bik_key);

// Play career movie (returns true if movie was found and started)
bool PlayCareerMovie(X86Context& ctx, uint8_t* base, const char* movie_key);

// Resolve audio playlist by context (pre_find_hook, follow, pre, dur, end, end_win, career_music)
bool ResolveAudioPlaylist(const char* race_name, const char* context, ResolvedAudioSample& out_sample);

// Check if currently playing movie is active
bool IsMoviePlaying();
void OnMovieFinished();

} // namespace mc2
