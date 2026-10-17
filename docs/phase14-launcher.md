# Phase 14: one-file launcher acceptance (2026-10-17)

Near-fresh test of `GoWLauncher.exe` (48.5 MB, built by `tools/build_launcher.py` at commit 63a70f6) on the
development PC. Not a separate Windows user profile (see "Not covered").

## Setup

- The exe alone in a new empty folder outside the repo (`E:\GoW_PSP_test\`), plus a `build/launcher.json` that only
  names the two `.chd` images (no EBOOT paths, no reuse of the repo's `build/`).
- Environment: `PYTHON*` and `VIRTUAL_ENV` removed, every PATH entry with Python, `.venv`, the py launcher or
  `WindowsApps` removed (neither `python` nor `py` resolvable). CMake and Visual Studio found by the launcher.
- Headless CLI build: `GoWLauncher.exe --build coo`, then `--build gos`.

## Results

| Check | Result |
|---|---|
| First run: unpack bundled sources, build `gow_disc`, decrypt EBOOT from the CHD | pass |
| Full recomp in-process (no Python installed on PATH) | pass: 114 / 143 generated files, all byte-identical to the repo's `build/gen` |
| CMake configure + full compile, Chains of Olympus | pass, 3 min 57 s (whole `--build coo`), `gow_rt_UCUS98653_full.exe` 14.2 MB |
| CMake configure + full compile, Ghost of Sparta | pass, 4 min 9 s (whole `--build gos`), `gow_rt_UCUS98737_full.exe` 17.9 MB |
| Software renderer, exe-built CoO, `build/rec_v9` replay to frame 6800 | pass: 34/34 frames identical, WAV SHA-256 `1c1ad676...cdff5869` (157 s) |
| GoS reaches gameplay, exe-built binary, `level_tour.py --modes gpu` on att_010_stern, vol_010_fissure, spa_010_landing, ice_010_bridge | pass: 4/4 ok, each level loaded and current, next level streamed, no unimplemented HLE, game 59.98 fps |

Test folder after both builds: 606 MB.

Notes:
- The very first attempt failed at CMake configure of the game ("No CMAKE_C_COMPILER could be found", although the
  compiler-id program had been built). It followed an earlier aborted run in the same folder; after emptying the
  folder and starting again from scratch, configure and both builds passed. Not reproduced since; if a player hits it,
  deleting `build/cmake_full` (or `build/cmake_gos_full`) and pressing Build again is the workaround.
- `Start-Process -Wait` in PowerShell waits for descendants (MSBuild node processes outlive the build), so time the
  launcher process itself.

## Not covered

- A real winget install of missing tools (only the consent flow and `winget show` were tested; tools were present).
- A separate Windows user profile (fresh `%LOCALAPPDATA%`, no Visual Studio user state).
- The GUI build path (same `run_build` code as the CLI) and a visual check of the rendered menu background.
- The GitHub Actions build: the run was not started by GitHub (account billing / spending limit).