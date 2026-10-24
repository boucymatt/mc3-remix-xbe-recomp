## What this changes

<!-- The symptom or goal, and what the code does now. Name the flag that turns it on. -->

## Task

<!-- MC3-00xx, or "none" -->

## What I ran

<!-- The command line, for how long, what you looked at. Say what you measured, not what you expect.
     Paste the relevant log lines, never a screenshot or capture of the game. -->

## Not covered

<!-- What you did not test or know is still open. -->

## Checklist

- [ ] **No game material.** No game files, no `generated/`, no captures of the game, no binaries, no values read out of the
      game's files. `python tools/check_repo_clean.py` passes.
- [ ] **Nothing changes by default.** The new behaviour is behind a flag or setting, off unless asked for. The `legacy`
      render profile and the game without flags behave as before.
- [ ] `.\build.bat` succeeds and the smoke test in `CONTRIBUTING.md` was run (menu, Arcade > Cruise, a minute, Go to Menus,
      no `exception` or `KeBugCheck` in the log).
- [ ] New flags are documented in the page of their area, with their default.
- [ ] Edits under `sdk/` are small, needed, and named in the commit message.
- [ ] Rebased on `main`; in `config/game.toml` every hook from both sides is kept.
- [ ] Everything written is in English.
