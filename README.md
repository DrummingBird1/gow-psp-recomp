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

You only download **one file**, the launcher. No Python, no manual setup.

1. Download [`GoW_PSP_Launcher-v1.1.0.zip`](https://github.com/DrummingBird1/gow-psp-recomp/releases/latest/download/GoW_PSP_Launcher-v1.1.0.zip)
   from the [latest release](https://github.com/DrummingBird1/gow-psp-recomp/releases/latest) and extract it anywhere: you get
   one `GoW_PSP` folder with `GoWLauncher.exe` and a short `README.txt`.
   Everything the launcher creates (sources, builds, saves, settings) goes into `GoW_PSP_data` next to the exe, so
   even a launcher started straight from Downloads adds only that one folder.
2. Run `GoWLauncher.exe` and choose the folder with **your own disc images** (`.chd` or `.iso`). It detects each game by its disc ID,
   not by the file name.
3. Press **Build**. The first time, the launcher asks to download its portable build tools (**about 246 MB**, once;
   about 550 MB unpacked): [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (Clang compiler), CMake and Ninja,
   from their official releases, checked against pinned SHA-256 hashes. They stay in `GoW_PSP_data`; **no Visual
   Studio, no administrator rights, nothing installed in Windows.**
4. Wait for the one-time build: about 4 minutes for the first game (including the download) and under 3 for the
   second, on a 20-thread CPU.
5. Pick a graphics preset and press **Play**.

> **Windows warning?** `GoWLauncher.exe` is unsigned and built with PyInstaller, so Windows SmartScreen may show
> "Windows protected your PC" (click **More info**, then **Run anyway**), and some antivirus programs flag
> PyInstaller programs as a false positive. Check that your download is the real one (v1.1.0 SHA-256):
> - `GoW_PSP_Launcher-v1.1.0.zip`: `9438f4eb60cfcad6c341cb6b864f9f70b49f2a1d7fd44166b826e861fc92a3cc`
> - `GoW_PSP\GoWLauncher.exe`: `ac9b96bcca910359af1bf8c4a9ef5f47fa8e710d730706e299945138aed12c2b`
>
> (PowerShell: `Get-FileHash <file>`). Each release lists its hashes in the release notes.

The game starts in fullscreen (F11 or Alt+Enter switches to a window; the launcher's Fullscreen option sets the
default) without a console window. What it prints goes to `GoW_PSP_data\build\launcher_coo.log` (or
`launcher_gos.log`); attach that file to bug reports.

Why build on your PC? The game code is generated from your own disc, so it can't be distributed; the launcher
compiles it locally, once.

| | |
|---|---|
| ![Options](docs/images/launcher-options.png) | ![Advanced options](docs/images/launcher-advanced.png) |

Keys: arrows = D-pad, Z = Cross, X = Circle, A = Square, S = Triangle, Q/W = L/R, Space/Enter = Start, V = Select,
I/J/K/L = analog stick, F11 fullscreen, F12 screenshot, Esc quit. Gamepads use the PSP layout.

## Build from source

Install Python 3.15. For the compiler, either let the launcher download its portable toolchain (llvm-mingw, CMake,
Ninja; `GOW_TOOLCHAIN=portable`, or `python tools/check_portable_toolchain.py`), or install CMake and Visual Studio
2022 or later (or its Build Tools) with the C++ workload; run from source, the launcher uses Visual Studio when it
finds it (`GOW_TOOLCHAIN=auto`, the default; `msvc` forces it). Both produce identical software-renderer output.
Then:

```bash
py -3 -m venv .venv
.venv/Scripts/python -m pip install -r tools/requirements-dev.txt
.venv/Scripts/python tools/launcher.pyw            # the launcher UI
.venv/Scripts/python tools/launcher.pyw --build coo # or build from the command line (coo / gos)
.venv/Scripts/python tools/build_launcher.py        # GoWLauncher.exe (PyInstaller)
```

Details: [`recomp/README.md`](recomp/README.md). The research setup (Ghidra, PPSSPP references) is under
[Research and development](#research-and-development).

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

### Setup on a new machine

1. Tools: Ghidra 12.1.3 + ghidra-allegrex v21.4, JDK 21, Python 3, CMake, Visual Studio with C++.
2. Decrypted EBOOT: the launcher does this for you (into `build/eboot/`). By hand, build `gow_disc`
   (`cmake -S . -B build/cmake_tools -DGOW_TOOLS_ONLY=ON` then `cmake --build build/cmake_tools --config Release`) and run
   `build/cmake_tools/Release/gow_disc.exe --disc <your disc>.chd --decrypt-eboot re/dumps/UCUS98653_EBOOT.BIN`
   (`UCUS98737_EBOOT.BIN` for Ghost of Sparta). A PPSSPP dump (`DumpFileTypes = 3`) gives the same bytes and is no
   longer needed.
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

Questions, ideas and help: [Discussions](https://github.com/DrummingBird1/gow-psp-recomp/discussions). Bugs:
[Issues](https://github.com/DrummingBird1/gow-psp-recomp/issues). Contributions are welcome: see [`CONTRIBUTING.md`](CONTRIBUTING.md). **Never attach or commit game files.** Releases
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
