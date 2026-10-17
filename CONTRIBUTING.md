# Contributing

Thanks for helping! Issues and pull requests are welcome: bugs in a level, rendering differences, missing HLE
functions, performance, the launcher, Linux support.

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
- Say in the pull request what you ran and what you saw (frame counts, PSNR, FPS).

## Versions

Releases use semantic versioning (`vMAJOR.MINOR.PATCH`): a new feature bumps MINOR, a fix PATCH. Add a line to
`CHANGELOG.md` under "Unreleased"; a release moves those lines under a new version and gets a git tag and a GitHub release.
