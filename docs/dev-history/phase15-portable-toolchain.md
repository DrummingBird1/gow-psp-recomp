# Phase 15: building without Visual Studio (portable llvm-mingw toolchain, 2026-10-17)

Players no longer need Visual Studio or winget. On the first Build the launcher downloads, after asking, a portable
toolchain into `GoW_PSP_data\build\toolchain` (`tools/toolchain.py`, pinned URLs and SHA-256):

| Archive | Size | License |
|---|---|---|
| llvm-mingw 20261006, ucrt x86_64 (Clang, lld, libc++, mingw-w64) | 191 MB | Apache-2.0 with LLVM Exceptions; mingw-w64: ZPL 2.1 / MIT / public domain |
| CMake 4.4.4 windows-x86_64 | 54 MB | BSD-3-Clause |
| Ninja 1.13.2 | 0.3 MB | Apache-2.0 |

Total download 246 MB; about 550 MB unpacked (the ARM/x86 targets and the bundled Python are skipped). llvm-mingw was
chosen over clang-cl with a downloaded Windows SDK because the SDK may not be redistributed; mingw-w64 brings its
own redistributable Windows headers and import libraries (D3D12, DXGI, Vulkan loading through SDL3 all build).

Build folders: `build/mingw_tools`, `build/mingw_full`, `build/mingw_gos_full` (Ninja, single-config); the Visual
Studio folders `build/cmake_*` stay as before. `GOW_TOOLCHAIN=portable|msvc|auto` chooses (the exe defaults to
portable; from source, auto uses Visual Studio when installed). If the portable path cannot be set up or its build
fails, the launcher falls back to Visual Studio when it is installed.

Portability changes: only `CMakeLists.txt` (MinGW links dbghelp/winmm, links the C/C++ runtimes statically so the game
exes need only Windows DLLs, and writes a CodeView PDB for `--profile`). No feature was disabled.

## Results

| Check | Result |
|---|---|
| Software renderer, llvm-mingw build of CoO (`build/rec_v9`, 6800 frames) | 34/34 frames identical, WAV SHA-256 `1c1ad676...5869` (same as MSVC) |
| GoS level tour (att_010_stern, vol_010_fissure, spa_010_landing, ice_010_bridge), D3D12 and Vulkan | 4/4 ok each, no unimplemented HLE, game 59.98 fps |
| CoO level tour (mar_02_village, ef_01_fields, poh_01_entrance, mar_04_helios_approach), D3D12 and Vulkan | 4/4 ok each, no unimplemented HLE, game 59.98 fps |
| Same tours with the MSVC build | all 16 ok |
| `tools/check_portable_toolchain.py` (download + gow_disc), also in CI | ok |

Host FPS of the headless GPU renderer in the tours (mean over the 4 levels; higher is better; one run each, so a
few percent is noise):

| | llvm-mingw | MSVC |
|---|---|---|
| GoS D3D12 | 147 | 98 |
| GoS Vulkan | 104 | 88 |
| CoO D3D12 | 106 | 89 |
| CoO Vulkan | 97 | 109 |

The software-renderer check took 119 s with llvm-mingw and 124 s with MSVC.

## Fresh test

`GoW_PSP_Launcher-v1.1.0.zip` extracted into a new folder, PATH reduced to `C:\Windows\System32;C:\Windows` (no
Python, CMake, Git or Visual Studio reachable; `INCLUDE`/`LIB` removed), `GOW_SIMULATE_MISSING=all` (Visual Studio
and CMake reported missing), consent answered yes:

| Step | Result |
|---|---|
| `--check-tools` before | portable: not downloaded (246 MB); cmake, vs: MISSING |
| `--build coo` (download 246 MB + unpack, gow_disc, decrypt, recompile, configure, build) | ok, 246 s |
| `--build gos` | ok, 158 s |
| Both games, headless GPU run (D3D12) of 1800 frames from the fresh folder, same stripped PATH | ok, no unimplemented HLE |

With MSVC the same builds take 169 s and 99 s; the portable path is slower mainly in CMake's configure (SDL3's
feature checks run Clang and lld for each one, about 85 s per game). Compiling is faster with Clang (about 25 s
for all of gow_rt).

Not covered: a different PC or Windows user, and a real network failure during the download (a wrong SHA-256 or an
error stops with a message and leaves no half-unpacked toolchain marked complete).