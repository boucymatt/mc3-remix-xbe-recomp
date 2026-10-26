# Dependencies

What you need before running `build.bat`. Run `.\build.bat check` at any time: it looks for everything in sections 1 to 3
and tells you what is missing, without building anything.
For native Linux, use the [Linux requirements](#linux-native-build) below and `./build.sh check`.

## At a glance

| | What | Why | Section |
| --- | --- | --- | --- |
| 1 | **Your own copy of the game**: Midnight Club 3: DUB Edition for the original Xbox, extracted to `assets\` | The port is built from, and runs on, your game files. None are provided. | [1](#1-the-game) |
| 2 | **Windows 10 or 11, 64-bit** | The interactive game uses Direct3D 12 and XAudio2. Linux uses SDL3 and OpenGL. | [2](#2-windows-and-hardware) |
| 3 | **Visual Studio 2022** with the C++ workload, the Clang tools and the CMake tools | The compiler (clang, lld), CMake, Ninja, the Windows SDK. | [3](#3-build-tools) |
| 4 | **Git** | CMake downloads four libraries on Windows, three on native Linux. | [3](#3-build-tools) |
| 5 | **Python 3.9 or newer** | Reads two small things out of your game files. Standard library only. | [3](#3-build-tools) |
| 6 | **An internet connection** during the first build | Dependencies come from GitHub. | [4](#4-downloaded-by-cmake) |
| 7 | **Disk space and memory** | The recompiled game is large. | [6](#6-build-resources) |

## 1. The game

You need **Midnight Club 3: DUB Edition** for the **original Xbox**, from a copy you own. Extract its files into the
`assets\` folder of the repository (it is git-ignored; nothing in it is ever committed). The build uses:

| File | Used for |
| --- | --- |
| `assets\default.xbe` | The game's executable. **Must be the supported build**: size 5,099,520 bytes, SHA-1 `5bca1cf092edaadf876e8f80129900407a4a07b8` (`config\supported_xbe.txt`). It is recompiled into C++. |
| `assets\assets_x.dat_2` | Read by `tools\generate_headlight_anchors.py` for the positions of each car's headlights. Required. |
| `assets\assets_x.dat` | Read by `tools\generate_blur_override.py` for the optional motion blur tune sidecar. Optional. |
| the rest of the extracted folder | Read by the game at run time. |

This project does not tell you how to get the game's files, and does not include, link to or accept them. Extract them
from your own disc with any tool that writes the files of an Xbox disc to a folder.

Not needed: an Xbox, an emulator, the Xbox Development Kit (XDK). The SDK's `xdk\5849\signatures.toml` stands in for it.

## 2. Windows and hardware

* **Windows 10 or 11, 64-bit.** Tested on Windows 11 Pro.
* **A GPU that supports Direct3D 12**, and an audio device (XAudio2).
* **PowerShell 5.1 or newer**, which Windows already has: `build.bat` uses it for the SHA-1 and to find Visual Studio.

## 3. Build tools

`build.bat` looks for these and, in an interactive run, offers to install the missing Visual Studio components.

### Visual Studio 2022

Community edition is enough. In the Visual Studio Installer choose the workload **Desktop development with C++**, and
under *Individual components* make sure these are ticked:

| Component | Identifier | Gives you |
| --- | --- | --- |
| MSVC v143 x64/x86 build tools | `Microsoft.VisualStudio.Component.VC.Tools.x86.x64` | the C++ runtime and headers |
| Windows 10 or 11 SDK | (part of the workload) | Direct3D 12, DXGI, XAudio2 import libraries |
| **C++ Clang tools for Windows** | `Microsoft.VisualStudio.Component.VC.Llvm.Clang` and `...VC.Llvm.ClangToolset` | `clang++` and `lld`, which compile the project |
| **C++ CMake tools for Windows** | `Microsoft.VisualStudio.Component.VC.CMake.Project` | CMake (3.25 or newer is required) and Ninja |

The project is built with **clang** (`clang++ -fuse-ld=lld`), not with MSVC's `cl`. If you already have a standalone LLVM,
put its `bin` folder on `PATH` before running `build.bat`; the script uses the `clang++` it finds. Tested with LLVM 23.1.3.

### Git

[Git for Windows](https://git-scm.com/download/win). CMake uses it to download the libraries in section 4, and you need it to
clone and contribute.

### Python

[Python 3.9 or newer](https://www.python.org/downloads/), with "Add python.exe to PATH" ticked (the `py -3` launcher also
works). Only the standard library is used. Developed on 3.12.

## Linux native build

* **x86-64-v2 Linux**, Bash, Clang with C++20 standard library headers, CMake 3.25+, Ninja, Git, Python 3.9+, pkg-config.
* **SDL3 >= 3.2**, **libepoxy**, **shaderc**, and **Vulkan 1.3 headers/loader**, including development headers and pkg-config files.
* An **OpenGL 4.5** capable graphics driver, or **Vulkan 1.3** for the optional native Vulkan backend. The runtime uses the system SDL audio device (PipeWire/PulseAudio/ALSA).
* Your supported `assets/default.xbe`, `assets/assets_x.dat_2`, and the rest of the extracted game for playing.
* Internet access during the first configure for pinned Zydis, toml++ and SPIRV-Cross dependencies.

On Arch Linux the development packages are included with the libraries:

```sh
sudo pacman -S --needed clang cmake ninja git python pkgconf sdl3 libepoxy shaderc vulkan-headers vulkan-icd-loader
./build.sh check
./build.sh
./run.sh
```

The default `linux-release` preset produces `out/build/linux-release/MC3RemixXBE`. `check` verifies prerequisites
without downloading, generating or compiling. Set `XBE_BUILD_JOBS` to a positive integer to override the default
CPU count capped at 8 jobs. Generated code and executable contents are derived from your game and stay local.

The optional `./build.sh headless` target uses the `linux-headless` preset and needs only Clang, CMake, Ninja, Git and
Python plus the owned game files. It fetches Zydis and toml++ only, and provides no window/audio/live input.

## 4. Downloaded by CMake

You install nothing here. CMake fetches these with git the first time it configures the Windows build (into
`out\build\release\_deps`). Native Linux fetches Zydis, toml++ and SPIRV-Cross into `out/build/linux-release/_deps`, and links system SDL3, libepoxy, shaderc and Vulkan. The headless preset fetches only Zydis and toml++.
The first build needs internet access to `github.com`:

| Library | Pinned to | Used for |
| --- | --- | --- |
| [Zydis](https://github.com/zyantific/zydis) | `v4.1.1` | decoding x86 in the recompiler |
| [toml++](https://github.com/marzer/tomlplusplus) | `v3.4.0` | reading the configs |
| [Dear ImGui](https://github.com/ocornut/imgui) | `v1.91.0` | the settings overlay |
| [SDL](https://github.com/libsdl-org/SDL) | `release-3.4.16` | the window, input and controllers |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) (Linux) | `5d127b917f080c6f052553c47170ec0ba702e54f` | translating console shaders to OpenGL GLSL |

The pins are in `sdk\cmake\XbeRecomp.cmake`. Their licences are in `sdk\THIRD_PARTY.txt`.

## 5. To contribute

Also needed if you send changes (see `CONTRIBUTING.md`):

* the **GitHub CLI** (`gh`), to open pull requests from the command line; optional, the web page works too;
* running `tools\install_hooks.bat` once per clone, which turns on the pre-commit check.

## 6. Build resources

The first build recompiles the whole game: it turns `default.xbe` into 133 C++ files of about 1.5 to 2 MB each and
compiles them. The following measurements are from Windows on a clean clone, with 48 parallel compile jobs:

| | Measured |
| --- | --- |
| Time | **6.5 minutes** for the whole first build (493 steps). The SDK's own estimate for an ordinary machine is 30 to 90 minutes; fewer cores take proportionally longer. |
| Extra memory at the peak | about **7.5 GB** with 48 jobs, roughly 150 MB per job. Lower `XBE_BUILD_JOBS` if the machine slows to a crawl or the compiler is killed. |
| Disk | about **0.5 GB**: `generated\` 195 MB, `out\` 268 MB (195 MB of that is the downloaded libraries). Your game files are extra. |
| Later builds | Incremental: only what changed is recompiled. Changing `config\game.toml` or the game's `default.xbe` regenerates the C++ but rewrites only the files whose content changed. |

## 7. Tested configuration

| | |
| --- | --- |
| Windows | 11 Pro (10.0.26200), 64-bit |
| Visual Studio | 2022 Community, its bundled CMake 3.28.0 and Ninja |
| clang | LLVM 23.1.3, a standalone release on `PATH` |
| Python | 3.12.6 |
| Machine | 48 threads, 48 GB RAM, a Direct3D 12 GPU |

**Verified on a fresh clone of this repository** (nothing but the clone and the game's files in `assets\`):
`.\build.bat check`, the full first build, an incremental build (0.9 minutes after touching three source files), the
game booting and playing the intro, and a run from the menus through Arcade > Cruise, a minute of driving, pause and
Go to Menus, with the default settings and with the 100-car setup of `launchers\play_traffic_vp_random.cmd` (100 cars
built, none stuck at the origin, all removed at exit, no `exception` or `KeBugCheck` in the log). The test harness drives
the menu layout that the Midnight Club 2 bridge adds, so that run had `MC3_MC2_DIR` set.

**Not tested:** the compiler that `build.bat` installs through the Visual Studio Installer ("C++ Clang tools for Windows"),
because the test machine uses a standalone LLVM and the installation needs an administrator prompt. If your clang is a
different version and the build fails to compile, that is the first thing to look at; please report the version.
