#include "app/app.h"

void Mc3Loaded();
void Mc3Init();
void Mc3DebugTransform(uint32_t type, float* matrix);
namespace input { struct Pad; }
void Mc3Pad(int port, input::Pad& pad);

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"MC3 Remix XboxXBERecomp pilot", // window and splash
    .short_title = L"MC3 Remix",          // message boxes
    .name = L"MC3RemixXBE",               // window classes, crash dumps, handover event
    .log = "mc3_xbe",                     // log file prefix
    .settings = "settings.toml",          // settings file beside the exe
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "MC3RemixXBE"}}, // each recompiled xbe and its exe
    .options = {},                        // game settings kept in the settings file
    .load = nullptr,                      // reads more from the settings file's toml::table
    .save = nullptr,                      // writes more into it
    .init = Mc3Init,                      // enable the MC3 frame-capture directory before the renderer starts
    .loaded = Mc3Loaded,                  // map the MC3 MCPX aperture after loading the image
    .window = nullptr,                    // the game window is up
    .message = nullptr,                   // window messages first; true takes one
    .ui = nullptr,                        // ImGui drawing each frame
    .pad = Mc3Pad,                        // changes a polled pad
    .rumble = nullptr,                    // changes a rumble before it is set
    .transform = Mc3DebugTransform,       // host free camera and AI race spectator
};
