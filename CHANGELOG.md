# Changelog

## Unreleased

## v1.0.0 (2026-10-17)

**Preview:** every level of both games loads and runs and saves work, but neither game has had a full playthrough
yet. Play from your disc image without Python or a manual EBOOT dump.

- **EBOOT decryption built in:** `gow_disc` reads the `.chd` / `.iso`, identifies the game and decrypts the EBOOT
  (PPSSPP PrxDecrypter + libkirk, fetched at build time with a pinned SHA-256); no PPSSPP dump needed.
- **Toolchain check:** the launcher finds CMake, Visual Studio C++ tools and Python, and offers to install missing ones
  with winget only after asking.
- **One-file launcher:** `GoWLauncher.exe` (PyInstaller) runs the recompiler in-process, so players need no Python.
- **New launcher UI** (PySide6/Qt, LGPL-3.0, linked dynamically): choose the game folder, detect each disc, build with a
  progress bar, graphics presets in a side panel, the game's own main menu as a live background after the first build,
  animations with a Reduce motion option. Third-party notices ship with the launcher.
- **Icons:** an original icon (`tools/make_icon.py`) for the launcher, its taskbar entry and the game executables; the
  game window and game cards use the game's own icon from your disc (`--icon`).
- **CI:** a GitHub Actions workflow builds `GoWLauncher.exe` on Windows (no game data) and uploads it as an artifact.

Bug pass before release:

- Fixed: a failed CMake configure (stale cache) now cleans the build folder and retries once.
- Checked, no change needed: HUD after Load Game (not reproducible); movies ending on a black frame (same as PPSSPP);
  Ghost of Sparta geometry clipping (same on PSP / PPSSPP); no music right after a level warp (music starts from level
  scripts, a warp artifact).
- Known issue: warping into Ghost of Sparta's `scb_010_climb` at spawn 1 crashes on a sound trigger the warp never creates
  (likely a warp artifact; see `docs/phase12-gos-level-tour.md`); a normal playthrough is still to be checked.

## v0.1.0 (2026-10-17)

First version. Both games run natively on Windows from your own disc.

- **Recompiler:** every function of both EBOOTs translated into C (`recomp/`), with differential tests against Unicorn.
- **Runtime:** threads and kernel objects, file system on CHD/ISO, VFPU, GE display lists, PGD decryption (Ghost of Sparta).
- **Renderers:** a multithreaded software renderer and a hardware renderer on SDL3 GPU (Direct3D 12 / Vulkan) with
  render-to-texture, stencil and framebuffer management.
- **Graphics options:** internal resolution up to 4K (`--res-scale 1-8`), effects resolution, MSAA, anisotropic filtering,
  LOD bias, FXAA, sharpening, HDR10/scRGB with inverse tone mapping, frame rates up to 1000 Hz (`--fps`; both games step by
  elapsed time), V-Sync, 21:9 (`--aspect`), texture dump and replacement packs.
- **Sound and movies:** sceAudio, SAS, ATRAC3+, PSMF/H.264 movies.
- **Saves, input recording and replay;** keyboard and gamepads.
- **Launcher** (`tools/launcher.pyw`): build from your own EBOOT dump, graphics options, play.
- **Tested:** every level of both games through an automated tour (`tools/level_tour.py`).

Known limits: key mapping is fixed (see `README.md`); Windows only so far (Linux / Steam Deck planned); the decrypted
EBOOT has to be dumped with PPSSPP.
