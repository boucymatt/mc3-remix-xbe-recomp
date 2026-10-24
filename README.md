# Midnight Club 3 Remix: Xbox recomp

An unofficial native PC port of **Midnight Club 3: DUB Edition** for the original Xbox, with extended rendering and other
enhancements. The game's x86 code is statically recompiled to C++ on your own machine by the
[Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) SDK and runs natively on Windows, drawn by a Direct3D 12
renderer.

> [!IMPORTANT]
> **This repository contains no game files and no code derived from the game.** It is source code and a build script.
> You need your own legally acquired copy of the game. `build.bat` turns your `default.xbe` into C++ on your machine
> (`generated/`, git-ignored) and compiles it with this source. **The executable you build contains the game's code, so
> never share it.** See [NOTICE.md](NOTICE.md).

Status: a working research repository. The game boots, reaches the menus and runs Arcade Cruise and races; the features
below are opt-in and described in their own pages. Expect rough edges.

## Features

* **Render profiles** (`legacy`, `enhanced`, `hybrid`): one Direct3D 12 path with a policy on top. Post processing,
  contact and vehicle shadows, reflections, headlight beams with shadows, motion blur. `legacy` is the console
  reference. See [docs/RENDER_PROFILES.md](docs/RENDER_PROFILES.md).
* **Cruise traffic pool** (opt-in): San Diego's Cruise driven by complete `vp_*` vehicles, up to 100 cars from 30 models,
  each with a random `.carcfg`. See [docs/TRAFFIC_POOL.md](docs/TRAFFIC_POOL.md).
* **Optional 128 MB memory mode** (`--mc3-guest-memory=128`), the memory of a development kit.
* **Debug tools**: a free camera, an AI spectator, a camera that follows each traffic car, a write watchpoint and a
  resource trace.
* **Midnight Club 2 city and career bridge** (experimental, `src/game/mc2_*.cpp`): draws Midnight Club 2's cities with
  Midnight Club 3's engine from files that you supply. It is off unless the environment variable `MC3_MC2_DIR` names
  the folder of your Midnight Club 2 `assets_p`; `MC3_MC2_VIDEO_DIR` names its movies and `MC3_MC2_SAVE_PATH` a save
  file. Not documented further yet.

## Quick start

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

## The supported game build

| | |
| --- | --- |
| Title | Midnight Club 3: DUB Edition, original Xbox |
| Title ID | `54540079` |
| `default.xbe` size | 5,099,520 bytes |
| `default.xbe` SHA-1 | `5bca1cf092edaadf876e8f80129900407a4a07b8` |

The hooks in `config\game.toml` and the recompiled code are tied to this build's addresses. A different dump compiles to
a game that crashes, so `build.bat` and the executable both refuse it.

## Running

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
build.bat, CMakeLists.txt, CMakePresets.json   the auto-compile and the CMake project
config/       game.toml (the recompiler's input and the hooks), supported_xbe.txt
src/          game specific code: src/game/*.cpp hooks and features, src/common/game_info.cpp
launchers/    .cmd files that start the built game
tools/        generators that read your game files at build time, and the repository guard
docs/         per-area documentation; docs/history holds engineering logs
sdk/          the Xbox XBE Recomp SDK (MIT, TGP482), modified: recompiler, runtime, XDK signatures
assets/       YOUR game files (git-ignored, not in the repository)
generated/    the recompiled game, made by build.bat (git-ignored, derived from your game)
out/          build output (git-ignored)
```

## Troubleshooting

* **`build.bat` is not recognized.** Write `.\build.bat`.
* **`default.xbe is not the build this port was made for`.** Your dump differs from the supported build. See the table
  above; there is no way around the check.
* **`missing: clang++`.** Install the Visual Studio component "C++ Clang tools for Windows" (the script offers to), or put
  a clang you already have on `PATH`.
* **The compiler runs out of memory, or the machine freezes.** The recompiled game is compiled in many large files in
  parallel. Set `XBE_BUILD_JOBS` to a lower number and run `build.bat` again; it resumes.
* **The build cannot download dependencies.** CMake fetches four libraries from GitHub on the first configure. See
  `DEPENDENCIES.md`.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md): the rules, how to set up, branch, write a commit message and send a pull request,
and what to do if a game file is committed by mistake.

## Credits and licence

See [NOTICE.md](NOTICE.md). The SDK in `sdk/` is MIT licensed (TGP482). No licence has been chosen yet for the rest.
