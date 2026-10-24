# Notices

## The game

*Midnight Club 3: DUB Edition* is a game by Rockstar San Diego, published by Rockstar Games. *Midnight Club*, *Rockstar
Games* and related names are trademarks of Take-Two Interactive Software, Inc. This project is unofficial and is not
affiliated with, endorsed by or sponsored by any of them.

This repository contains **no part of the game**: no executable, no data, no media, and no code translated from the
game's executable. It contains source code that is compiled together with the recompiled game on the machine of someone
who owns the game. `build.bat` makes that recompiled code, `generated/`, from the user's own `default.xbe`; it is
derived from the game, stays on that machine and is git-ignored.

**The executable you build contains the game's code translated to C++. Do not distribute it, and do not upload it
anywhere.** For the same reason this repository has no release script and no release downloads.

## The SDK in `sdk/`

`sdk/` is a copy of the **Xbox XBE Recomp** SDK, version 0.1.0 (`sdk/SDK_VERSION`), by TGP482, licensed under the MIT
licence (`sdk/LICENSE`, Copyright (c) 2026 TGP482). Upstream: <https://github.com/TGP482/XboxXBE-Recomp>. The licence
requires its notice to stay with every copy, so it does.

**This copy is modified.** It is not a clean upstream snapshot and is not the latest upstream. The changes this project
made include the renderer profiles and features under `sdk/runtime/gpu/`, the optional 128 MB guest memory mode in
`sdk/runtime/kernel/`, and smaller changes elsewhere in `sdk/runtime/`; this list is not a complete diff. Anything that
is not game specific is a candidate to offer upstream.

`sdk/THIRD_PARTY.txt` carries the notices for software that ends up in the built executable (toml++, Dear ImGui and SDL).
CMake downloads those, and Zydis, when it configures the build; they are not stored in this repository. See
`DEPENDENCIES.md`.

`sdk/xdk/5849/signatures.toml` holds lengths and hashes of Xbox library functions with their addresses masked, as the SDK
documents; it does not contain library code.

## Credits

* [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) by TGP482: the recompiler and runtime this is built on.
* [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), whose overridable function layout and instruction hooks the SDK
  follows.
* [Zydis](https://github.com/zyantific/zydis), [toml++](https://github.com/marzer/tomlplusplus),
  [Dear ImGui](https://github.com/ocornut/imgui) and [SDL](https://github.com/libsdl-org/SDL).
* [xemu](https://github.com/xemu-project/xemu) and [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded), used by
  the SDK as references for Xbox behaviour. No emulator code is used.

## LARecomp

`docs/RENDER_PROFILES.md` refers to **LARecomp**, a separate project for Midnight Club: Los Angeles, as a reference for
rendering behaviour that project documents (the motion blur streak pass, for one). This project's rule is not to copy code
from it.

## This project's own code

No licence has been chosen yet for the code under `src/`, `tools/`, `config/`, `docs/` and the scripts, so by default all
rights are reserved by its authors. Decide on one before the repository is made public. The SDK keeps its own MIT licence
either way.
