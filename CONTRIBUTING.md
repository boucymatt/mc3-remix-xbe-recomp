# Contributing

This is a private working repository for the Midnight Club 3 Xbox recomp. Several people and AI-assisted sessions
(called *forks* below) work on separate features at the same time. This page says how to send your change so that it
can be reviewed, merged, and never leaks anything from the game.

## The two rules that do not bend

1. **No game material, ever.** The repository holds source code only. Never commit:
   * the game's files (`default.xbe`, `assets_x.dat*`, movies, audio, textures, models, saves);
   * `generated/`, which is the game's x86 code translated to C++ and so is derived from the game;
   * captures of the game (screenshots, GPU captures, videos), binaries (`.exe`, `.pdb`, `.dll`, `.obj`) and archives;
   * values read out of the game's files and pasted into source: positions, tables, byte arrays. If the code needs
     one, read it at build time from the user's own files (see `tools/generate_headlight_anchors.py`, which `build.bat`
     runs) or at run time. Plain identifiers are fine: a vehicle name, a function address, a flag.

   `tools/check_repo_clean.py` enforces this and the pre-commit hook runs it on every commit. It is a safety net, not a
   substitute for care.

2. **Nothing changes by default.** A new feature sits behind a `--mc3-*` flag or a `settings.toml` key and is off
   unless asked for. The `legacy` render profile is the regression reference and must keep rendering as before, and the
   game without flags must behave as it did.

Everything written in this repository is in English: code, comments, identifiers, documentation, commit messages.

## One-time setup

```bat
git clone git@github.com:znxee/mc3-remix-xbe-recomp.git
cd mc3-remix-xbe-recomp
tools\install_hooks.bat
```

The repository is private: the owner has to add you as a collaborator first. Then:

1. Put your own copy of the game in `assets\` (see `DEPENDENCIES.md`). It is git-ignored.
2. Install what `DEPENDENCIES.md` lists and run `.\build.bat check`. It tells you what is missing.
3. Run `.\build.bat`. The first build recompiles the whole game (6.5 minutes on a 48-thread machine, much longer on a
   small one); later ones recompile only what changed.

`tools\install_hooks.bat` turns on the pre-commit hook (`git config core.hooksPath .githooks`). Do not skip it.

## Work on a branch, never on `main`

```bat
git fetch origin
git switch -c <area>/<task>-<slug> origin/main
```

`<area>` is one of `render`, `traffic`, `mc2`, `runtime`, `sdk`, `build`, `tools`, `docs`. `<task>` is the maintainers'
task id (`MC3-0015`); ask if you have none. Example: `traffic/MC3-0015-late-model-parts`.

**Several forks on one machine** must not share one working folder: one fork's half-finished edit breaks the other's
build. Give each fork its own folder with a worktree, and point its `assets` at the same game files with a junction:

```bat
git worktree add ..\mc3-wt-<slug> -b <area>/<task>-<slug> origin/main
cd ..\mc3-wt-<slug>
mklink /J assets <full path to the existing assets folder>
tools\install_hooks.bat
```

A worktree has its own `out\` and `generated\`, so its first build is a full one. Set `XBE_BUILD_JOBS` lower if several
builds run at once.

## Make the change

Where things live:

| Area | Files |
| --- | --- |
| Hooks into the game, the config | `src/game/hooks.cpp`, `config/game.toml` |
| Rendering | `sdk/runtime/gpu/`, `src/game/render_car.cpp`, `docs/RENDER_PROFILES.md` |
| Cruise traffic | `src/game/traffic_*.cpp`, `src/game/car_*.cpp`, `docs/TRAFFIC_POOL.md` |
| Midnight Club 2 city and career bridge | `src/game/mc2_*.cpp` |
| Debug tools | `src/game/debug_tools.cpp`, `src/game/write_watch.cpp` |
| Kernel, memory, files, input | `sdk/runtime/` |
| Host performance | `sdk/runtime/gpu/render_thread.cpp`, `sdk/runtime/gpu/d3d12.cpp`, `docs/PERFORMANCE.md` |
| Build and tools | `build.bat`, `CMakeLists.txt`, `tools/` |

How the game is changed:

* `X86_FUNC(sub_XXXXXXXX)` in `src/game/*.cpp` replaces the recompiled function at that guest address. Call
  `__imp__sub_XXXXXXXX` to reach the original. `[[hook]]` entries in `config/game.toml` call a function before or after
  one instruction.
* A new feature gets its own file in `src/game/`. The build takes every `src/game/*.cpp`, so there is nothing to
  register.
* **Two hooks on one address do not link.** Search `src` for `X86_FUNC(sub_XXXXXXXX)` before adding one. If the address is
  taken, hook what it jumps to, or call into your code from the existing hook.
* `sdk/` is a vendored, modified copy of the Xbox XBE Recomp SDK. Change it only when the game cannot be changed from
  `src/`, keep the edit small, and say so in the commit message so it can be offered upstream.
* **Render state has two rules** (`docs/PERFORMANCE.md`): every write to `State::simple` or `State::vsc` calls
  `MarkSimple`/`MarkVsc`, and the backend sets viewport, scissor, blend factor, stencil reference, topology and pipeline
  only through its cached setters (`SetViewport` and the others), never with `cl_->...` directly. Breaking either draws
  with stale state. After touching either, run once with `MC3_STATE_VERIFY=1`.
* Match the code around yours: naming, comment density, idiom. Comments say why, not what.
* No absolute paths, machine names, user names or e-mail addresses in anything you commit.

## Before you commit

1. `.\build.bat` succeeds on your working tree.
2. Smoke test: start the game, reach the main menu, start **Arcade > Cruise** in San Diego, drive for at least a minute,
   pause and choose **Go to Menus**. The log (`logs\` beside the exe) must have no `exception` and no `KeBugCheck`. Without
   `MC3_MC2_DIR` the Arcade menus are the retail ones; with it, they gain the Midnight Club 2 cities, and any script that
   presses keys blindly has to know which layout it is driving.
3. Run it once with no flags and check the default behaviour is untouched.
4. If you added a flag, document it with its default (off) in the page of its area.
5. `python tools\test_repo_guard.py` and `python tools\check_repo_clean.py` pass.

## Commit messages

```text
<area>: <imperative summary, up to about 70 characters>

Why the change exists and what was wrong before: the symptom, and the cause if you found it.
What it does now, in behaviour, and the flag that turns it on.
How you checked it: the command line, for how long, what you looked at.
What is not covered or still open.

Task: MC3-0015
Co-Authored-By: Name <address>      (when someone or something else helped)
```

* `<area>` is the same word as in the branch name.
* One logical change per commit. A fix and the refactor it needed are two commits.
* Say what you measured, not what you expect. "Ran 100 cars for 90 s, four seeds, clean" beats "should be fixed".
* Do not commit `WIP`, do not merge `main` into your branch (rebase, below), do not commit build output.

Example:

```text
traffic: load the car part files for models that first load during the race

A late model came without bumpers, kit and wheels: its LODs asked the mesh loader
[0x507668], which during the race is the world's and loads no car part.

With --mc3-traffic-cruise-late-models the geometry setup (0x109D40) now runs with the
part loader [0x5319E4] in its place and gives the other back after it.

Checked with 100 cars from 30 models, 90 s, Go to Menus, four seeds: every late model
shows its parts. Not checked: render profiles other than enhanced.

Task: MC3-0015
```

## Send it

```bat
git push -u origin <your branch>
gh pr create --base main --fill
```

Or open the pull request on GitHub. The template asks for what the pull request changes, what you ran, and a checklist
that includes the no-game-files rule. The maintainer reviews and merges it. Do not push to `main`, force-push to a
branch that is not yours, or merge your own pull request.

## Keep up with `main`

```bat
git fetch origin
git rebase origin/main
```

Two files are shared by everyone, so they conflict often:

* `config/game.toml`: keep every `[[hook]]` from both sides. Never drop another fork's hook to make a conflict go away.
* `src/game/traffic_vehicle_bridge.cpp` is one large file. Keep changes to it small and focused, and say in the pull
  request which part you touched, so two forks working in it can see each other.

## If a game file was committed

Do not push, and do not just delete the file in a new commit: it would stay in the history of the repository.

* **Not pushed yet:** `git reset --soft origin/main`, unstage the file (`git restore --staged <file>`), delete it, and
  commit again. Check with `python tools/check_repo_clean.py --history`.
* **Already pushed:** tell the maintainer at once. The history has to be rewritten (`git filter-repo`) and force-pushed,
  and everyone must re-clone afterwards. GitHub may keep cached views of the old commits until its support purges them,
  which is why this is worth avoiding rather than fixing.

## Questions and problems

Open an issue in this repository. For a crash, include the last lines of the log and the command line, never a
screenshot or a capture of the game.
