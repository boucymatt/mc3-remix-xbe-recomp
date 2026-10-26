# Midnight Club 3 Remix: Xbox recomp

An unofficial native PC port of **Midnight Club 3: DUB Edition** for the original Xbox, with extended rendering and other
enhancements. The game's x86 code is statically recompiled to C++ on your own machine by the
[Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) SDK and runs natively on Windows, drawn by a Direct3D 12
renderer. Linux uses a native SDL3 runtime with OpenGL and Vulkan backends.

> [!IMPORTANT]
> **This repository contains no game files and no code derived from the game.** It is source code and a build script.
> You need your own legally acquired copy of the game. `build.bat` or `build.sh` turns your `default.xbe` into C++ on your machine
> (`generated/`, git-ignored) and compiles it with this source. **The executable you build contains the game's code, so
> never share it.** See [NOTICE.md](NOTICE.md).

Status: a working research repository. Windows runs Arcade Cruise and races. Linux has been checked through the intro,
menus and San Diego Arcade Cruise with the native renderer. The features below are opt-in and described in their own
pages. Expect rough edges.

## Features

* **Render profiles** (`legacy`, `enhanced`, `hybrid`): one Direct3D 12 path with a policy on top. Post processing,
  contact and vehicle shadows, reflections, headlight beams with shadows, motion blur. `legacy` is the console
  reference. See [docs/RENDER_PROFILES.md](docs/RENDER_PROFILES.md).
* **Cruise traffic pool** (opt-in): San Diego's Cruise driven by complete `vp_*` vehicles, up to 100 cars from 30 models,
  each with a random `.carcfg`. See [docs/TRAFFIC_POOL.md](docs/TRAFFIC_POOL.md).
* **Host performance work**: the title and render threads trimmed so that the 100-car scene runs at about twice
  the frame rate it did. The rules that keep it so are in [docs/PERFORMANCE.md](docs/PERFORMANCE.md).
* **Optional 128 MB memory mode** (`--mc3-guest-memory=128`), the memory of a development kit.
* **Debug tools**: a free camera, an AI spectator, a camera that follows each traffic car, a write watchpoint and a
  resource trace.
* **Midnight Club 2 city and career bridge** (experimental, `src/game/mc2_*.cpp`): draws Midnight Club 2's cities with
  Midnight Club 3's engine from files that you supply. It is off unless the environment variable `MC3_MC2_DIR` names
  the folder of your Midnight Club 2 `assets_p`; `MC3_MC2_VIDEO_DIR` names its movies and `MC3_MC2_SAVE_PATH` a save
  file. Not documented further yet.

## Windows quick start

1. **Clone** this repository.
2. **Put your game files in `assets\`**: the extracted game folder, with `default.xbe` and the game's other files and
   folders beside it. `build.bat` checks `default.xbe` against the supported build (see below).
3. **Install what [DEPENDENCIES.md](DEPENDENCIES.md) lists**, then check you have it:
   ```bat
   .\build.bat check
   ```
4. **Build.** The first build recompiles the whole game: 6.5 minutes on a 48-thread machine, much longer on a small one:
   ```bat
   .\build.bat
   ```
5. **Play.** `build.bat` offers to start the game; later, use a file in `launchers\`, or run
   `out\build\release\MC3RemixXBE.exe`.

In PowerShell write `.\build.bat`; the leading `.\` is required there.

### What `build.bat` does

| Step | What |
| --- | --- |
| 1. Game files | Finds `assets\default.xbe` and checks its SHA-1 against `config\supported_xbe.txt`. Any other build is refused. |
| 2. Python | Finds Python 3.9 or newer and writes `src\game\headlight_anchors.h` from your `assets\assets_x.dat_2`. |
| 3. Toolchain | Finds Visual Studio 2022, clang, CMake, Ninja and git. Offers to install the missing Visual Studio components. |
| 4. Symbols | Normally nothing: `config\game.toml` already carries the XDK symbols. |
| 5. Game code | Recompiles `default.xbe` into `generated\` and compiles everything with clang. Writes the optional motion blur sidecar `assets\mc3_motion_blur_override.bin`. |

`CI=1` makes it run without prompts. `XBE_BUILD_JOBS=N` sets the parallel compile jobs (the CPU count by default); lower it
if the compiler runs out of memory.

## Linux build and play

The native Linux runtime uses **SDL3 with OpenGL 4.5 or native Vulkan 1.3** for a window, accelerated rendering, keyboard/gamepad input,
rumble and stereo audio. It reuses the game's Xbox shader translation, texture decoding and DirectSound/XACT mixer,
including signed blending for car lights and the game's brightness ramp.
Put your supported game files in `assets/`, then:

```sh
./build.sh check   # Verify tools and owned game files.
./build.sh         # Compile the native runtime.
./run.sh           # Launch with assets/.
```

Install Clang, CMake 3.25+, Ninja, Git, Python 3.9+, pkg-config and the SDL3 (3.2+), libepoxy and shaderc development
packages. See [DEPENDENCIES.md](DEPENDENCIES.md#linux-native-build). The first configure fetches pinned Zydis,
toml++ and SPIRV-Cross. `XBE_BUILD_JOBS=4 ./build.sh` reduces parallel compiler memory use (default: at most 8 jobs).

The executable is `out/build/linux-release/MC3RemixXBE`. You can also run it directly with your extracted game folder:

```sh
./out/build/linux-release/MC3RemixXBE /path/to/game
MC3_GAME_DIR=/path/to/game ./run.sh
```

Keyboard controls: **Enter/Z** = A, **Escape/X** = B, **Space** = Start/pause, **Backspace** = Back,
**arrows/WASD** = direction/steering, **right Shift** = throttle, **left Shift** = brake,
**C/V** = X/Y, **Q/E** = black/white. SDL-compatible controllers are hot-pluggable. **F11** toggles fullscreen;
**F3** saves a frame under `out/build/linux-release/captures/`. Settings and saves live beside the executable.
Closing the window exits the runtime.

`./settings.sh --camera-orbit on` enables right-stick orbit around the player's car in the chase camera.
Move the right stick left/right to circle the car and up/down to raise/lower the view. The view stays at the
chosen angle when released; click the right stick (**R3**) to return behind the car. A radial deadzone filters
stick drift, and the game retains camera smoothing and collision handling. Menus and steering retain their
normal controls. Use `--camera-orbit off` to restore the original right-stick behavior. The feature works with
both Linux renderers and is off in fresh settings until enabled.

### Linux graphics settings

Select the native Vulkan 1.3 backend with `./settings.sh --renderer vulkan`, or return to the
OpenGL 4.5 default with `./settings.sh --renderer opengl`. Vulkan submits console draws directly through
Vulkan and uses SPIR-V shaders; it does not require Zink, Wine or DXVK. This new backend is experimental
and currently uses conservative GPU waits. It is not yet a guaranteed performance improvement over OpenGL.

Use `settings.sh` before launching; changes apply after restarting the game. For example:

```sh
./settings.sh --resolution 1920x1080 --fps 60 --vsync off --motion-blur off --light-streaks off
./run.sh
```

`--resolution` accepts `1280x720`, `1920x1080`, `2560x1440`, or `3840x2160` and sets both the window and internal
render height. `--render-height 480|720|1080|1440|2160` overrides the internal height independently. Render targets,
depth, viewport/scissor rectangles, copies and guest readbacks scale together; this is higher-resolution rendering,
not just a larger window. The console's anamorphic image is still displayed at its intended aspect ratio.

`--fps` accepts `30`, `60`, `90`, `120`, `144`, `165`, `240`, or `0` (uncapped). Use `--vsync off` to avoid a refresh-rate
limit. These are caps, not guaranteed frame rates; higher resolutions increase GPU cost. The existing game-time and
movie-time hooks keep their clocks independent of presentation rate.

`--motion-blur on|off` controls the original speed blur; `--light-streaks on|off` controls lamp/headlight glare and
stretched light trails, including the lamppost star-pattern effect and translucent lamp beams. It retains road illumination,
smoke, and the HUD. The build generates `mc3_light_glare.bin` beside the executable to identify those textures from your
own game; keep that file beside the executable.
`--road-shine off` gives the roads a dry, matte appearance by disabling the game's road-shininess tuning across
all city, time-of-day, and weather variants. Road textures, lighting, and car materials retain their original settings.
Use `./settings.sh --road-shine on` to restore the original look. The build generates `assets/mc3_road_override.bin`
from your own files; the original game archive is never modified.
Use `./settings.sh --show` to view settings. The file is `out/build/linux-release/settings.toml`; the editor preserves
unrelated settings. Defaults remain effects on, 30 FPS, and native internal resolution.

The Linux renderer currently targets the original console rendering path. The Windows-only enhanced D3D12 effects,
experimental MC2 city renderer, ImGui settings overlay, shared-memory automation UI and hardware write watchpoints are
not ported. Linux currently starts in a window; use F11 for fullscreen. Windows render-profile and `render_percent`
settings do not enable their D3D12 features on Linux; use the Linux settings above.
The optional headless research target remains available through `./build.sh headless`; it writes
`out/build/linux-headless/MC3RemixXBE`. `./build.sh check-headless` checks its smaller dependency set.

For repeatable local checks, `MC3_RUN_SECONDS=N` ends a run after N seconds, `MC3_CAPTURE_EVERY=N` captures every Nth
frame, and `MC3_TEST_HIDDEN=1` keeps the test window hidden. `MC3_INPUT_SCRIPT=/path/to/script` selects deterministic
input replay (`frame buttons duration`, buttons joined with `+`, e.g. `60 START 6`); physical input is replaced during
replay. `MC3_INPUT_COMMAND=/path/to/command` optionally consumes one `buttons duration` command from that file.
`MC3_PERF=1` logs CPU draw time and renderer waits. For scripted comparisons,
`MC3_BENCHMARK_FROM_FRAME=N` removes the configured FPS cap after N presented frames while leaving the
saved setting unchanged. Keep the same save, input replay, resolution and effects for each backend, and
compare only gameplay after shader warmup. `MC3_VK_VALIDATION=1` enables the installed Khronos Vulkan
validation layer; do not use validation runs to measure performance.
These diagnostics are off by default. Captures and generated game code stay local and must not be committed.

Native Vulkan validation: intro, menus and San Diego gameplay were rendered at 1440p with frame captures
inspected. A 190-second route with Khronos validation and synchronization validation reported no errors.
This is initial coverage, not a full-game compatibility claim; OpenGL remains the default.
A local 1440p stationary San Diego replay on the RX 9060 XT measured median rates of 165 FPS before the
OpenGL state/constant-upload optimizations, 205 FPS afterward, and 187 FPS with native Vulkan. These are
single-run gameplay samples after warmup with VSync, blur, light streaks and road shine disabled, not whole-game
averages. Vulkan still waits for each submission; overlapping CPU recording and GPU execution is a remaining
optimization opportunity. The OpenGL path avoids redundant framebuffer, sampler and constant-buffer updates.

Linux OpenGL validation: Arch Linux, AMD Radeon RX 9060 XT, Mesa 26.2.5. The native build, intro, menus, San Diego Arcade
Cruise, a minute of scripted driving, pause and return to menus were checked at approximately 30 FPS, with displayed-frame captures
inspected. The SDL stereo output device opened successfully; audible output and physical controller rumble were not
independently checked. Other GPUs, race modes, long career sessions and networking still need coverage. The headless
build, career-state checks and 15 repository guard tests also pass.

The Linux graphics settings were additionally checked with 1080p internal target height and a 60 FPS cap in gameplay:
the clock reported approximately 1.000 game second per real second. Glare-off texture matches and blur suppression
were verified, and effects-on boot/menu rendering was checked. A 2160p render-target startup check passed; sustained
4K gameplay and every higher FPS cap have not been benchmarked. Three settings-editor tests cover preservation of
unrelated keys, toggling, and indented TOML keys.

## The supported game build

| | |
| --- | --- |
| Title | Midnight Club 3: DUB Edition, original Xbox |
| Title ID | `54540079` |
| `default.xbe` size | 5,099,520 bytes |
| `default.xbe` SHA-1 | `5bca1cf092edaadf876e8f80129900407a4a07b8` |

The hooks in `config\game.toml` and the recompiled code are tied to this build's addresses. A different dump compiles to
a game that crashes, so both build scripts and the executables check the game image.

## Running on Windows

Launchers in `launchers\` start the built game:

| Launcher | What |
| --- | --- |
| `play_render_legacy.cmd`, `play_render_enhanced.cmd`, `play_render_hybrid.cmd` | A render profile. Extra arguments pass through. |
| `play_traffic_vp_random.cmd` | Enhanced profile, Arcade > Cruise with 100 random `vp_*` cars from 30 models. Optional seed as the first argument. |
| `play_traffic_vp_follow.cmd` | The same, with a camera that goes round the cars for as long as the race lasts. |

Settings go in `settings.toml` beside the exe; the command line wins over it. The common flags:

| Flag | |
| --- | --- |
| `--mc3-render-profile=legacy\|enhanced\|hybrid` | Render profile; `legacy` unless set. |
| `--mc3-render-feature=...`, `--mc3-render-disable=...` | Add or remove single render features. |
| `--mc3-gpu-index=N` | Which Direct3D 12 adapter, in high-performance order; the boot log lists them. |
| `--mc3-guest-memory=128` | 128 MB guest memory instead of 64. |
| `--mc3-traffic-*`, `--mc3-car-cache=N` | The Cruise traffic pool. See `docs/TRAFFIC_POOL.md`. |
| `--mc3-freecam`, `--mc3-ai-follow` | Free camera (F7), AI spectator (F8, F9 for the next car). |

The full lists are in the docs of each area. Everything not listed there is a diagnostic.

## Repository layout

```text
build.bat, build.sh, run.sh, CMakeLists.txt, CMakePresets.json   build scripts and the CMake project
config/       game.toml (the recompiler's input and the hooks), supported_xbe.txt
src/          game specific code: src/game/*.cpp hooks and features, src/common/game_info.cpp
launchers/    .cmd files that start the built game
tools/        generators that read your game files at build time, and the repository guard
docs/         per-area documentation; docs/history holds engineering logs
sdk/          the Xbox XBE Recomp SDK (MIT, TGP482), modified: recompiler, runtime, XDK signatures
assets/       YOUR game files (git-ignored, not in the repository)
generated/    the recompiled game, made by the build scripts (git-ignored, derived from your game)
out/          build output (git-ignored)
```

## Troubleshooting

* **`build.bat` is not recognized.** Write `.\build.bat`.
* **`default.xbe is not the build this port was made for`.** Your dump differs from the supported build. See the table
  above; there is no way around the check.
* **`missing: clang++`.** Install the Visual Studio component "C++ Clang tools for Windows" (the script offers to), or put
  a clang you already have on `PATH`.
* **The compiler runs out of memory, or the machine freezes.** The recompiled game is compiled in many large files in
  parallel. Set `XBE_BUILD_JOBS` to a lower number and run your build script again; it resumes.
* **The build cannot download dependencies.** CMake fetches four libraries on Windows or three on native Linux from GitHub on the first configure. See
  `DEPENDENCIES.md`.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md): the rules, how to set up, branch, write a commit message and send a pull request,
and what to do if a game file is committed by mistake.

## Credits and licence

See [NOTICE.md](NOTICE.md). The SDK in `sdk/` is MIT licensed (TGP482). No licence has been chosen yet for the rest.
