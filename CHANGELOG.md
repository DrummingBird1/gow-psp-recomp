# Changelog

## Unreleased

## v0.2.0 (unreleased draft)

Play from your disc image without Python or a manual EBOOT dump.

- **EBOOT decryption built in:** `gow_disc` reads the `.chd` / `.iso`, identifies the game and decrypts the EBOOT
  (PPSSPP PrxDecrypter + libkirk, fetched at build time with a pinned SHA-256); no PPSSPP dump needed.
- **Toolchain check:** the launcher finds CMake, Visual Studio C++ tools and Python, and offers to install missing ones
  with winget only after asking.
- **One-file launcher:** `GoWLauncher.exe` (PyInstaller) runs the recompiler in-process, so players need no Python.
- **New launcher UI** (PySide6/Qt, LGPL-3.0, linked dynamically): choose the game folder, detect each disc, build with a
  progress bar, graphics presets in a side panel, the game's own main menu as a live background after the first build,
  animations with a Reduce motion option. Third-party notices ship with the launcher.
- **CI:** a GitHub Actions workflow builds `GoWLauncher.exe` on Windows (no game data) and uploads it as an artifact.

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
