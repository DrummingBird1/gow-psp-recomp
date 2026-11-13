# Development

How to build the ports from source, how the repository is laid out and how to set up the research tools. Players
don't need any of this: the launcher in the [README](../README.md) does it all. Contributions: see
[`CONTRIBUTING.md`](../CONTRIBUTING.md).

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

If the launcher can't download a tool (no internet, a blocked download), install CMake, Ninja and a Clang or Visual
Studio C++ compiler yourself and put them on PATH.

The recompile, build and test commands are in [`recomp/README.md`](../recomp/README.md). The software renderer must
stay pixel-identical after any change to `runtime/`, `recomp/` or the build: `tools/regression_check.py` checks it
against a recorded run (see `CONTRIBUTING.md`).

## How it works

This is a **static recompilation**, not an emulator. The game's MIPS (Allegrex) code is decrypted from your disc and
every function is translated ahead of time into C (`recomp/`), which is compiled with the C runtime (`runtime/`) into a
normal Windows program. The runtime replaces the PSP's operating system and hardware: threads and kernel objects,
the file system (reading straight from your `.chd` / `.iso`), the VFPU, the GE graphics chip (a software renderer and
a hardware renderer on SDL3 GPU / Direct3D 12 / Vulkan), sound, ATRAC3+, movies and saves. Because the GE output is
redrawn on the PC's GPU, it can render at any resolution and frame rate.

How each part was built, phase by phase, is in the [development history](README.md#development-history) (mostly in Hebrew, the
original notes).

## Layout

| Path | Contents |
|---|---|
| `docs/` | Player guides, this page, the roadmap and the [development history](README.md#development-history) |
| `re/scripts/` | Ghidra scripts (analysis export, metrics, import naming, function seeding) and `build_nid_db.mjs` |
| `re/out/` | Analysis results: metrics, function lists, imports, NID table (no game code) |
| `recomp/` | Recompiler (Python): loader/relocator, decoder, C emitter, driver, differential tests |
| `runtime/` | C runtime the generated code links against |
| `CMakeLists.txt` | Builds `gow_rt_<GAME>` from `build/gen/<GAME>` |
| `cmake/` | Pinned file lists (SHA-256) of third-party sources CMake downloads, e.g. OpenH264's decoder |
| `tools/` | `launcher.pyw` (build and play), play/replay scripts, level tour, frame checks, documentation helpers |

Local only (ignored): `*.chd`, `re/dumps/`, `re/ghidra/`, `build/`, `.venv/`, `re/ppsspp-*`, `media/` (screenshots
and recordings of the game, which never go into the repository).

## Setup for research

1. Tools: Ghidra 12.1.3 + ghidra-allegrex v21.4, JDK 21 and Python 3.15 for the analysis. For compiling, the portable
   toolchain is enough (llvm-mingw, CMake, Ninja: `python tools/check_portable_toolchain.py` downloads it, or
   `GOW_TOOLCHAIN=portable`); Visual Studio 2022+ with the C++ workload and CMake also works. Git is only needed to
   clone this repository: the build downloads its third-party sources as pinned archives.
2. Decrypted EBOOT: the launcher does this for you (into `build/eboot/`). By hand, build `gow_disc`
   (`cmake -S . -B build/cmake_tools -DGOW_TOOLS_ONLY=ON` then `cmake --build build/cmake_tools --config Release`) and run
   `build/cmake_tools/Release/gow_disc.exe --disc <your disc>.chd --decrypt-eboot re/dumps/UCUS98653_EBOOT.BIN`
   (`UCUS98737_EBOOT.BIN` for Ghost of Sparta). A PPSSPP dump (`DumpFileTypes = 3`) gives the same bytes and is no
   longer needed.
3. Python environment for the tests: `py -3 -m venv .venv` then `.venv/Scripts/python -m pip install unicorn`.
4. PPSSPP reference sources used by `build_nid_db.mjs` (optional, re-fetchable, tag v1.20.4):
   - `Core/HLE/*.cpp` (and, for the movies, `Core/HW/MediaEngine.*` and `Core/HW/MpegDemux.*`) into `re/ppsspp-hle/`;
   - `Core/MIPS/MIPSInt.cpp` and `MIPSTables.cpp` into `re/ppsspp-mips/`;
   - the GE references (`GPU/Software/*`, `GPU/GPUState.*`, `GPU/ge_constants.h`, `GPU/Common/VertexDecoderCommon.*`,
     `GPU/Common/TextureDecoder.*`, `GPU/GPUCommon*.cpp`) into `re/ppsspp-gpu/`;
   - the audio references (`Core/HW/SasAudio.*`, `Core/HW/SasReverb.*`, `Core/HW/Atrac3Standalone.*`,
     `Core/HLE/AtracCtx2.h`, `Core/Util/AtracTrack.*`, `ext/at3_standalone/`) into `re/ppsspp-audio/`.
5. For progress screenshots and recordings: `.venv/Scripts/python.exe -m pip install pillow numpy`, and ffmpeg.

## Releases

Releases are numbered `vMAJOR.MINOR.PATCH`, roughly following semantic versioning, and are listed in
[`CHANGELOG.md`](../CHANGELOG.md). Each has `GoW_PSP_Launcher-vX.Y.Z.zip` and `SHA256SUMS.txt`, built by the
`launcher` workflow from the tag, with a build provenance from GitHub.
