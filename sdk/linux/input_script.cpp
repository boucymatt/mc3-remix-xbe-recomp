// Optional frame-based input replay for reproducible native renderer checks.
// MC3_INPUT_SCRIPT: frame buttons duration; buttons joined with +, as in the headless harness.
#include "core/input.h"
#include "core/log.h"
#include "platform.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <mutex>

namespace input {
void MergeScript(Pad& pad) {
    struct Press { uint64_t at, length; std::string buttons; };
    static const auto script = [] {
        std::vector<Press> presses;
        const char* path = std::getenv("MC3_INPUT_SCRIPT");
        if (!path) return presses;
        std::ifstream file(path);
        if (!file) { LOGE("Cannot read MC3_INPUT_SCRIPT: %s", path); return presses; }
        for (std::string line; std::getline(file, line);) {
            std::istringstream row(line); Press p{}; p.length = 8;
            if (row >> p.at >> p.buttons) { row >> p.length; presses.push_back(p); }
        }
        LOGR("Linux input replay: %s (%zu presses)", path, presses.size());
        return presses;
    }();
    const uint64_t frame = linuxapp::frames.load();
    static std::mutex lock;
    std::lock_guard guard(lock);
    static Press live{0, 0, ""};
    static uint64_t checked = ~0ull;
    if (const char* command = std::getenv("MC3_INPUT_COMMAND"); command && checked != frame) {
        checked = frame;
        std::ifstream file(command);
        std::string buttons; uint64_t length = 6;
        if (file >> buttons) {
            file >> length;
            live = {frame, length, buttons};
            file.close(); std::remove(command);
            LOGR("Linux replay command at frame %llu: %s (%llu frames)", (unsigned long long)frame, buttons.c_str(), (unsigned long long)length);
        }
    }
    auto apply = [&](const Press& p) {
        if (frame < p.at || frame - p.at >= p.length) return;
        std::istringstream buttons(p.buttons);
        for (std::string b; std::getline(buttons, b, '+');) {
            if (b == "START") pad.buttons |= 16;
            else if (b == "BACK") pad.buttons |= 32;
            else if (b == "UP") pad.buttons |= 1;
            else if (b == "DOWN") pad.buttons |= 2;
            else if (b == "LEFT") pad.buttons |= 4;
            else if (b == "RIGHT") pad.buttons |= 8;
            else if (b == "A") pad.analog[0] = 255;
            else if (b == "B") pad.analog[1] = 255;
            else if (b == "X") pad.analog[2] = 255;
            else if (b == "Y") pad.analog[3] = 255;
            else if (b == "LT") pad.analog[6] = 255;
            else if (b == "RT") pad.analog[7] = 255;
            else if (b == "RSL") pad.rx = -32768;
            else if (b == "RSR") pad.rx = 32767;
            else if (b == "RSU") pad.ry = 32767;
            else if (b == "RSD") pad.ry = -32768;
            else if (b == "R3") pad.buttons |= 128;
            else if (b == "LS") pad.ly = 32767;
            else if (b == "LSD") pad.ly = -32768;
            else if (b == "LSL") pad.lx = -32768;
            else if (b == "LSR") pad.lx = 32767;
        }
    };
    for (const auto& p : script) apply(p);
    apply(live);
}
}
