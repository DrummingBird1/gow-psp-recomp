# GoW_PSP

Reverse engineering and static recompilation of the two PSP God of War games
(Chains of Olympus, UCUS98653 · Ghost of Sparta, UCUS98737) into native Windows executables.

> This repository holds tools, scripts, runtime code and documentation only.
> The game files, decrypted dumps, Ghidra project, generated C and memory images contain
> Sony's code; they stay local and are excluded by `.gitignore`. Bring your own copies.

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
| `tools/` | Documentation helpers: terminal-output screenshots, logged command runs |

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
