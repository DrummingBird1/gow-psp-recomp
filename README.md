# GoW_PSP

Reverse engineering and static recompilation of the two PSP God of War games
(Chains of Olympus, UCUS98653 · Ghost of Sparta, UCUS98737) into native Windows executables.

> This repository holds tools, scripts, runtime code and documentation only.
> The game files, decrypted dumps, Ghidra project, generated C and memory images contain
> Sony's code; they stay local and are excluded by `.gitignore`. Bring your own copies.
> Not affiliated with or endorsed by Sony Interactive Entertainment. God of War is a trademark of its owner.

Both games boot, play through every level, save and load, with sound, movies and a hardware renderer:
up to 4K internal resolution, MSAA, anisotropic filtering, HDR10, 120+ FPS and 21:9 (see `docs/`).

## Quick start (play from your own disc)

You need your own UMD copy of the game, as a disc image (`.chd` or `.iso`).

1. Download `GoWLauncher.exe` (from a release, or the `launcher` workflow artifact on GitHub Actions) and put it in an
   empty folder; it keeps its files next to itself. It finds CMake and Visual Studio (2022 or later, C++ tools) and
   offers to install any missing build tool with winget, after asking you. No Python needed.
2. Run it, point it at the folder with your disc images: it detects which game each one is. Press **Build** (decrypts
   the EBOOT from the disc, recompiles it into C and builds it; about 10 minutes the first time), choose the graphics
   options, press **Play**.

From source instead: install Python 3 and the packages in `tools/requirements-dev.txt`, then run
`python tools/launcher.pyw`.

The launcher keeps your paths in `build/launcher.json`. Keys: arrows = D-pad, Z = ✕, X = ○, A = □, S = △, Q/W = L/R,
Space/Enter = Start, V = Select, I/J/K/L = analog stick, F11 fullscreen, F12 screenshot; gamepads use the PSP layout.

## Layout

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

## Contributing and versions

Contributions are welcome: see [`CONTRIBUTING.md`](CONTRIBUTING.md). Releases follow semantic versioning and are listed in
[`CHANGELOG.md`](CHANGELOG.md).

## License

GPL-3.0-or-later (see [`LICENSE`](LICENSE)). The runtime contains C ports of parts of PPSSPP (GPL-2.0-or-later). CMake
downloads further third-party sources at configure time (SDL3, libchdr, OpenH264, PPSSPP's `at3_standalone` and
`libkirk`); they keep their own licenses and are not part of this repository.
