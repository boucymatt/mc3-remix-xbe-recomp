// The existing DirectSound/XACT mixer feeds SDL's device conversion stream.
#include "audio/audio.h"
#include "core/log.h"
#include <SDL3/SDL.h>
#include <thread>
#include <vector>

namespace audio {
void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::thread([render = std::move(render)] {
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kRate};
        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream || !SDL_ResumeAudioStreamDevice(stream)) {
            LOGE("SDL audio unavailable: %s; continuing mixer without output", SDL_GetError());
            if (stream) SDL_DestroyAudioStream(stream);
            stream = nullptr;
        } else LOGR("SDL audio: stereo float, %u Hz", kRate);
        constexpr uint32_t frames = kRate / 100;
        std::vector<float> samples(frames * 2);
        for (;;) {
            if (stream && SDL_GetAudioStreamQueued(stream) >= int(samples.size() * sizeof(float) * 3)) {
                SDL_Delay(2);
                continue;
            }
            render(samples.data(), frames);
            if (stream) {
                if (!SDL_PutAudioStreamData(stream, samples.data(), int(samples.size() * sizeof(float)))) {
                    LOGE("SDL audio submission: %s", SDL_GetError());
                    SDL_DestroyAudioStream(stream);
                    stream = nullptr;
                }
            } else SDL_Delay(10);
        }
    }).detach();
}
}
