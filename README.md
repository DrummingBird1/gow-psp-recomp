# God of War PSP: native PC ports (unofficial)

Play **God of War: Chains of Olympus** and **God of War: Ghost of Sparta** natively on Windows, recompiled from your own
PSP disc, with 4K, MSAA, HDR and high frame rates.

> **Unofficial fan project.** Not affiliated with or endorsed by Sony Interactive Entertainment or Santa Monica Studio.
> God of War is a trademark of its owner. **No game content is included or downloadable here:** you need your own,
> legally dumped disc image (`.chd` or `.iso`) of the US release (UCUS98653 / UCUS98737). Everything built from it stays
> on your PC.

![The launcher](docs/images/launcher-main.png)

## Status: preview (v1.0.0)

- Every level of both games loads and runs (automated tours: [phase 11](docs/phase11-level-tour.md),
  [phase 12](docs/phase12-gos-level-tour.md)).
- Saving and loading work; sound, music and movies play.
- **No full playthrough of either game yet**, so expect bugs late in a game. Known issues are listed in the
  [phase write-ups](docs/), the [launcher acceptance test](docs/phase14-launcher.md) and the [changelog](CHANGELOG.md).
- Windows only for now (Linux / Steam Deck planned).

Bug reports are very welcome: please attach a recorded input file (`--record-input`), **never game files or screenshots of the game**.

## Features

- Native executables: every function of the game recompiled to C, no emulator at run time.
- Internal resolution up to 4K (and beyond), MSAA, anisotropic filtering, FXAA, sharpening.
- HDR10 / scRGB output.
- 60, 120, 144 FPS and more (both games step by elapsed time), V-Sync.
- 21:9 and other aspect ratios.
- Keyboard and gamepads (PSP layout), saves, texture dump and replacement packs.
- A one-download launcher: finds your disc images, builds each game, graphics presets, Play.

![Launcher tour](docs/images/launcher-tour.gif)

## Quick start

1. Download `GoWLauncher.exe` from the [latest release](https://github.com/DrummingBird1/gow-psp-recomp/releases/latest)
   and put it in an empty folder (it keeps its files next to itself). No Python needed.
2. You need CMake and Visual Studio 2022 or later with the C++ tools. The launcher checks for them and offers to install
   missing ones with winget, only after asking you.
3. Run it and choose the folder with your disc images; it detects each game by its disc ID, not the file name.
4. Press **Build** (decrypts the EBOOT from your disc, recompiles it and compiles it; about 10 minutes the first time),
   pick a graphics preset, press **Play**.

| | |
|---|---|
| ![Options](docs/images/launcher-options.png) | ![Advanced options](docs/images/launcher-advanced.png) |

Keys: arrows = D-pad, Z = Cross, X = Circle, A = Square, S = Triangle, Q/W = L/R, Space/Enter = Start, V = Select,
I/J/K/L = analog stick, F11 fullscreen, F12 screenshot, Esc quit. Gamepads use the PSP layout.

## Build from source

Install Python 3, CMake and Visual Studio (C++), then:

```bash
py -3 -m venv .venv
.venv/Scripts/python -m pip install -r tools/requirements-dev.txt
.venv/Scripts/python tools/launcher.pyw            # the launcher UI
.venv/Scripts/python tools/launcher.pyw --build coo # or build from the command line (coo / gos)
.venv/Scripts/python tools/build_launcher.py        # GoWLauncher.exe (PyInstaller)
```

Details: [`recomp/README.md`](recomp/README.md). The research setup (Ghidra, PPSSPP references) is below.

## How it works

This is a **static recompilation**, not an emulator. The game's MIPS (Allegrex) code is decrypted from your disc and
every function is translated ahead of time into C (`recomp/`), which is compiled with the C runtime (`runtime/`) into a
normal Windows program. The runtime replaces the PSP's operating system and hardware: threads and kernel objects,
the file system (reading straight from your `.chd` / `.iso`), the VFPU, the GE graphics chip (a software renderer and
a hardware renderer on SDL3 GPU / Direct3D 12 / Vulkan), sound, ATRAC3+, movies and saves. Because the GE output is
redrawn on the PC's GPU, it can render at any resolution and frame rate.

![Welcome screen](docs/images/launcher-welcome.png)

## Research and development

### Layout

| Path | Contents |
|---|---|
| `docs/` | Feasibility report and phase write-ups |
| `re/scripts/` | Ghidra scripts (analysis export, metrics, import naming, function seeding) and `build_nid_db.mjs` |
| `re/out/` | Analysis results: metrics, function lists, imports, NID table (no game code) |
| `recomp/` | Recompiler (Python): loader/relocator, decoder, C emitter, driver, differential tests |
| `runtime/` | C runtime the generated code links against |
| `CMakeLists.txt` | Builds `gow_rt_<GAME>` from `build/gen/<GAME>` |
| `cmake/` | Pinned file lists (SHA-256) of third-party sources CMake downloads, e.g. OpenH264's decoder |
| `tools/` | `launcher.pyw` (build and play), play/replay scripts, level tour, frame checks, documentation helpers |

Local only (ignored): `*.chd`, `re/dumps/`, `re/ghidra/`, `build/`, `.venv/`, `re/ppsspp-*`, `media/` (screenshots and recordings; see [`docs/progress.md`](docs/progress.md)).

## Setup on a new machine

1. Tools: Ghidra 12.1.3 + ghidra-allegrex v21.4, JDK 21, PPSSPP (to dump the decrypted EBOOT), Python 3, CMake, Visual Studio with C++.
2. Dump `EBOOT.BIN` with PPSSPP (`DumpFileTypes = 3`) into `re/dumps/`.
3. Python environment for the tests: `py -3 -m venv .venv` then `.venv/Scripts/python -m pip install unicorn`.
4. PPSSPP reference sources used by `build_nid_db.mjs` (optional, re-fetchable):
   `Core/HLE/*.cpp` (and, for the movies, `Core/HW/MediaEngine.*` and `Core/HW/MpegDemux.*`) into `re/ppsspp-hle/`, `Core/MIPS/MIPSInt.cpp` and `MIPSTables.cpp` into `re/ppsspp-mips/`,
   and the GE references (`GPU/Software/*`, `GPU/GPUState.*`, `GPU/ge_constants.h`, `GPU/Common/VertexDecoderCommon.*`,
   `GPU/Common/TextureDecoder.*`, `GPU/GPUCommon*.cpp`) into `re/ppsspp-gpu/`, and the audio references
   (`Core/HW/SasAudio.*`, `Core/HW/SasReverb.*`, `Core/HW/Atrac3Standalone.*`, `Core/HLE/AtracCtx2.h`,
   `Core/Util/AtracTrack.*`, `ext/at3_standalone/`) into `re/ppsspp-audio/` (tag v1.20.4).
5. For progress screenshots and recordings: `.venv/Scripts/python.exe -m pip install pillow numpy`, and ffmpeg.

See [`recomp/README.md`](recomp/README.md) for the recompile/build/test commands and
[`docs/`](docs/) for results.

### Setup on a new machine

1. Tools: Ghidra 12.1.3 + ghidra-allegrex v21.4, JDK 21, PPSSPP (to dump the decrypted EBOOT), Python 3, CMake, Visual Studio with C++.
2. Dump `EBOOT.BIN` with PPSSPP (`DumpFileTypes = 3`) into `re/dumps/`.
3. Python environment for the tests: `py -3 -m venv .venv` then `.venv/Scripts/python -m pip install unicorn`.
4. PPSSPP reference sources used by `build_nid_db.mjs` (optional, re-fetchable):
   `Core/HLE/*.cpp` (and, for the movies, `Core/HW/MediaEngine.*` and `Core/HW/MpegDemux.*`) into `re/ppsspp-hle/`, `Core/MIPS/MIPSInt.cpp` and `MIPSTables.cpp` into `re/ppsspp-mips/`,
   and the GE references (`GPU/Software/*`, `GPU/GPUState.*`, `GPU/ge_constants.h`, `GPU/Common/VertexDecoderCommon.*`,
   `GPU/Common/TextureDecoder.*`, `GPU/GPUCommon*.cpp`) into `re/ppsspp-gpu/`, and the audio references
   (`Core/HW/SasAudio.*`, `Core/HW/SasReverb.*`, `Core/HW/Atrac3Standalone.*`, `Core/HLE/AtracCtx2.h`,
   `Core/Util/AtracTrack.*`, `ext/at3_standalone/`) into `re/ppsspp-audio/` (tag v1.20.4).
5. For progress screenshots and recordings: `.venv/Scripts/python.exe -m pip install pillow numpy`, and ffmpeg.

See [`recomp/README.md`](recomp/README.md) for the recompile/build/test commands and
[`docs/`](docs/) for results.

## Contributing

Contributions are welcome: see [`CONTRIBUTING.md`](CONTRIBUTING.md). **Never attach or commit game files.** Releases
follow semantic versioning and are listed in [`CHANGELOG.md`](CHANGELOG.md).

## Credits

- [PPSSPP](https://www.ppsspp.org/): the reference for the PSP's hardware and HLE; parts of it are ported in the runtime
  (GPL-2.0-or-later), and its `at3_standalone` decoder and PrxDecrypter are used.
- libkirk (PSP crypto), libchdr, SDL3, OpenH264.
- [Cinzel](https://fonts.google.com/specimen/Cinzel) font (SIL Open Font License).
- [Qt for Python / PySide6](https://doc.qt.io/qtforpython/) (LGPL-3.0, linked dynamically) for the launcher.
- The icon is original art (`tools/make_icon.py`).

## License

GPL-3.0-or-later (see [`LICENSE`](LICENSE)). CMake downloads the third-party sources at configure time (SDL3, libchdr,
OpenH264, PPSSPP's `at3_standalone` and `libkirk`) with pinned SHA-256s; they keep their own licenses and are not part
of this repository. The launcher ships its third-party notices (`tools/launcher_assets/THIRD_PARTY_NOTICES.txt`).
