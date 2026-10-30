# Contributing

Thanks for helping! Issues and pull requests are welcome: bugs in a level, rendering differences, missing HLE
functions, performance, the launcher, Linux support.

Questions and ideas go to [Discussions](https://github.com/DrummingBird1/gow-psp-recomp/discussions); Issues are for bugs.

## Ground rules

> **Never attach game files** to issues, pull requests or discussions: no disc images, EBOOTs, saves from the game
> folder, generated C, memory dumps, or screenshots and videos of the game. To show a bug, attach the recorded input
> (`--record-input file.txt`) and say which level and what happens.

- **Never commit game content.** No disc images, EBOOT/PRX dumps, generated C (`build/gen`), memory images, Ghidra
  projects, extracted files, screenshots or recordings of the game. `.gitignore` covers the usual places; check
  `git status` before you commit. Pull requests that contain game data are closed.
- Third-party code keeps its license. PPSSPP-derived code (GPL-2.0-or-later) is fine; say where it comes from in a comment.
  New dependencies are downloaded by CMake with a pinned version and SHA-256, like the existing ones.
- Match the surrounding code: C11 for the runtime, comments that say why, no dead code or debug leftovers.

## Building and testing

Follow the quick start in `README.md` (or `recomp/README.md` for the details). Before a pull request:

- **Renderer or GE changes:** the software renderer must stay pixel-identical. Run a reference session
  (`--frames-dir ... --frames-every 200`) before and after and compare with `tools/check_frames.py`. For the GPU
  renderer, compare `--renderer gpu` frames the same way at `--res-scale 1`.
- **Game logic, HLE, timing:** headless runs are deterministic. A recorded session (`--record-input`, `tools/replay.cmd`)
  must give the same frames and the same `--wav` before and after, unless the change is meant to alter timing.
- **Many levels at once:** `tools/level_tour.py --game coo|gos` warps into every level and reports crashes, missing HLE
  functions and missing files.
- **Launcher changes:** `python tools/check_launcher.py` (also run by CI) opens the window in every language without
  showing it and fails on a text that doesn't fit, a translation table that lacks a text, or a broken bug-report zip.
- Say in the pull request what you ran and what you saw (frame counts, PSNR, FPS).

## Translating the launcher

The launcher's window is translated in `tools/launcher_i18n.py`, one table per language: the English text as the
key, your translation as the value. Tooltips, dialogs and the log stay in English.

- **To fix or finish a language,** edit its table. A text missing from a table shows in English.
- **To add a language,** copy a whole table (the Hebrew one has every text), give it the language's code, translate
  the values, and add `("Native name", "code")` to `LANGS` and the code to `ui_language` in `ALLOWED` in
  `tools/launcher.pyw`. A right-to-left language also goes into `RTL`.
- **Keep it short.** The Options panel is narrow: the five preset names share one row, and a long label or button
  pushes the panel past the window's edge. Use the words players know from other games, even when shorter than the
  dictionary's. `tools/check_launcher.py` says which text doesn't fit.
- Keep the names as they are (Kratos, Steam, Discord, PPSSPP, V-Sync, HUD, CRT, VRR, F1-F12) and the `…` of a
  button that opens a dialog.
- Open a pull request with the label `translation`, and say whether you are a native speaker. A native speaker's
  review of an existing language is welcome too.

## Versions

Releases are numbered `vMAJOR.MINOR.PATCH`, roughly following semantic versioning: usually a new feature bumps MINOR
and a fix PATCH, but the maintainer may pick a bigger step for a release that bundles several changes (as v1.1.5 did). Add a line to
`CHANGELOG.md` under "Unreleased"; a release moves those lines under a new version and gets a git tag and a GitHub release.
