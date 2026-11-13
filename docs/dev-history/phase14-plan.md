# Phase 14 plan: one-download launcher

A self-contained brief for whoever takes this on (a contributor, or another AI agent with access to the repository and a
Windows machine with the user's own game files).

## Goal

A player downloads only the launcher, points it at the folder with their own disc image (`.chd`/`.iso`), and plays
God of War: Chains of Olympus (UCUS98653) or Ghost of Sparta (UCUS98737). No PPSSPP, Python, CMake or Visual Studio on
the player's PC.

## Hard constraints

- **No game content is ever distributed or committed:** no EBOOT/PRX dumps, no generated C (`build/gen`), no
  `image.bin`, no screenshots. The game's code must be decrypted, recompiled and compiled on the player's PC.
- **License:** the project is GPL-3.0 (the runtime ports parts of PPSSPP, GPL-2.0-or-later). Binaries in releases must
  come with the source (this repository at the tag).
- **Determinism checks must keep passing** (see Verification).

## Where things are today (v0.1.0)

- `tools/launcher.pyw`: Tkinter launcher. Needs a decrypted `EBOOT.BIN` (dumped with PPSSPP), runs
  `recomp/recomp.py` (Python) and CMake + Visual Studio, then starts `build/cmake_full` / `build/cmake_gos_full`.
- `recomp/recomp.py`: MIPS → C. Inputs: the decrypted EBOOT, `re/out/recomp/<ID>_EBOOT.BIN.recomp.json` and
  `re/out/psp_nids_ppsspp.tsv` (both committed, no game code). Output: `build/gen/<ID>_full/` (~113 C files + `image.bin`).
- `CMakeLists.txt`: builds `gow_rt_<ID>_full.exe` from the generated C plus `runtime/` (C/C++), downloading SDL3,
  libchdr, OpenH264, PPSSPP's `at3_standalone` and `libkirk` with pinned SHA-256.
- `runtime/rt_io.c`: reads CHD/ISO; `--extract disc0:/PSP_GAME/SYSDIR/EBOOT.BIN out` copies a file off the disc
  (still encrypted). PGD decryption already uses libkirk.

## Tasks

1. **Decrypt the EBOOT ourselves.** Port PPSSPP's PRX decryption (`Core/ELF/PrxDecrypter.cpp`, GPL) on top of the
   libkirk the build already downloads. Accept: the decrypted output is byte-identical to a PPSSPP dump for both games.
2. **No Python on the player's PC.** Freeze the launcher and the recompiler into one executable (e.g. PyInstaller), or
   port the recompiler. Accept: same generated C, byte for byte, as `recomp.py` today.
3. **No Visual Studio on the player's PC.** Ship the runtime prebuilt (it holds no game code) and compile only the
   generated C on the player's PC with a pinned, auto-downloaded toolchain (e.g. Zig/clang, SHA-256 checked), or with
   the same compiler that built the runtime. Accept: the game runs as fast as the MSVC build (Verification) and the
   software renderer stays pixel-identical.
4. **Launcher UX:** choose the game folder, detect which game each image is, build with a progress bar, then Play with the
   existing graphics options (`docs/phase13-graphics.md`). Optional: key mapping (keys are fixed in
   `runtime/rt_host.c`, `live_input`).
5. **Release:** a GitHub Actions workflow that builds the launcher and the prebuilt runtime (no game data in CI),
   a `v0.2.0` release, `CHANGELOG.md`, README quick start updated.

## Verification (needs the user's own game files, on their machine)

- **Software renderer identical:** `--renderer soft`, the `build/rec_v9` input, empty `ms0`, frames every 200 up to 6800
  → `tools/check_frames.py build/rec_v9 <out>`: 34/34 identical, same `--wav` SHA-256 (`1c1ad676…`).
- **Performance:** `--renderer gpu --perf-csv`, CoO frames 4700-5400 of that input, best of 3 runs (the machine is often
  busy; GPU cost via `RT_GPU_SYNC=1` and the `gpu_ms` column).
- **Every level still loads:** `tools/level_tour.py --game coo|gos` on a handful of levels: no traps, no missing HLE.
- **Clean machine:** a fresh Windows user profile with only the downloaded launcher and the disc image reaches gameplay
  in both games. Document timings and sizes in `docs/phase14-launcher.md`.

## Also open (separate from phase 14)

- Ghost of Sparta: `scb_010_climb` from spawn point 1 stops on a call to address 0 after a checkpoint restart
  (`docs/phase12-gos-level-tour.md`, bug 2). A normal playthrough does not start there.
- Linux / Steam Deck (phase 15): SDL3 GPU already has a Vulkan backend; the runtime and build need porting.
