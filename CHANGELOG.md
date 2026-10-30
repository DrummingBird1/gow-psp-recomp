# Changelog

## v1.4.0 (2026-10-30)

- **Fix: Chains of Olympus could crash in the Depths of Tartarus** (issue #24, `[trap] break at 088096AC`). The game
  ran out of heap: the port started it with a 256 KB boot stack instead of the 1 KB its EBOOT asks for, and both games
  size their heap by the free memory left, so they had 255 KB less than on a PSP or in PPSSPP. The boot thread now
  takes the EBOOT's own values, and both games get the full heap. Existing saves load as before.
- **Fix: slow motion in Ghost of Sparta that the Windows key ended for a few seconds** (issue #33). The whole game
  runs on one thread, and on a CPU with performance and efficient cores (Intel 12th gen and later, Core Ultra, Ryzen
  AI 300) Windows could put it on the efficient ones, where each frame took up to twice as long. The game now asks
  Windows not to throttle it and keeps that thread on the fastest cores; the log says so
  (`hybrid CPU: the game thread keeps to ...`). `RT_NO_FAST_CORES=1` leaves it to Windows.
- **Fix: V-Sync with a frame rate above the display's refresh** (issue #35), e.g. 120 FPS on a 60 Hz display. The
  game's time kept falling behind the display, and on a PC a little too slow for every refresh V-Sync settled at
  30 FPS. With V-Sync on, the frame rate is now held to the display's refresh, and the log says so.
- **Spin keys for the "rotate the stick" prompts** (issue #34): hold O or U (or a gamepad button set on the Controls
  page) and the stick goes round, clockwise or counter-clockwise. The keyboard's I/J/K/L could not complete them.
- **Turbo** for the "press repeatedly" prompts (issue #26): hold C (gamepad: right stick click) with a button and it
  presses 10 times a second. Prompts that need a given button at the right moment still need you.
- **Show the buttons pressed** (issue #31): F2 in the game, or Options > *Show the buttons pressed*. A small overlay
  shows the buttons and the stick as the game reads them.
- **Fix: HDR on Auto turned on for displays without HDR** (issue #28), which looked washed out. Auto now follows the
  display's HDR switch in Windows.
- **Fix: no Direct3D 12 or Vulkan device closed the game** (issue #27). The software renderer now takes over (the
  PSP's resolution, slower), and the launcher offers to keep it.
- **Fix: a first Build that failed said nothing useful** (issue #29). The launcher shows the step and the error, and
  the build shows its downloads, stops a stalled one after a timeout and uses the system proxy.
- **A new look for the launcher** ("forged bronze"): a warm brown-black instead of flat grey, game cards in gold
  frames with corner fittings, a forged red Play button, a top bar with the title and buttons for Options and
  Controls (the default keys moved from the footer to the Controls button and page), the folder and Build in one bar,
  and the options grouped in framed sections with one joined preset control. Right to left, the title, the icons and
  the Options panel change sides too. *Reduce motion* still turns every animation off.
- **The launcher in Arabic, German, Italian, Brazilian Portuguese and Russian** (Options > Advanced > Launcher
  language), as well as English, Spanish, French and Hebrew. Arabic, like Hebrew, lays the window out right to left.
  Fix: in Spanish the Options panel was wider than the window, and its right edge was cut off.
- **Bug reports include frame timings**: every Play writes `launcher_<game>_perf.csv` (each frame's times, about
  60 bytes a frame) and *Report a bug* adds its last 5 minutes. The bug-report form now asks for the zip.

## v1.3.1 (2026-10-26)

The first release with the v1.3.0 changes below. v1.3.0 was tagged but never published: found while releasing it,
its launcher would have started a game built by v1.2 with an option that build doesn't know (`--rumble`, on by
default), and the game would have closed at once.

- **After an update, the launcher asks for one Build before Play** when the game was built from older code: the
  game's state says *Needs building* and Play says why (a shortcut or Steam shows it in a message). Each build
  records which runtime it was made from (`build/runtime_<game>.txt`).
- **Pillow 13.0.0** in the launcher (was 12.3.0), for the 41 security advisories that GitHub's Dependabot listed
  against 12.3.0. The launcher only opens PNG files (art from your disc, dumped textures), so the risk was low.
- Fix: closing the "game closed" dialog without a button could act as if one had been chosen.

## v1.3.0 (2026-10-26, not released)

- **Fix: custom controls that seemed not to work** (issue #3). A key put on a PSP button stayed on the button it had
  by default, so WASD for moving still blocked (W = R) and attacked (A, S). Now a key or gamepad button goes to one
  button only: the Controls page moves it and says so, and settings saved by older versions are fixed when the game
  starts. The game log names any key that is still on two buttons.
- **Mouse buttons can be bound** (left, right, middle, 4, 5) on the Controls page (issue #3).
- **The gamepad's right stick evades** (L + R + the stick, as in the PS2 games); the Controls page turns it off.
- **Language** (Discussion #19): English, French or Spanish. Ghost of Sparta's US disc has all three; Chains of
  Olympus's has English only.
- **4:3 aspect ratio** (Discussion #20): the 3D view is cropped at the sides and the HUD scaled to fit, for CRTs and
  4:3 handhelds. **CRT filter:** scanlines and a shadow mask.
- **Frame rate counter:** F3 in the game, or *Show the frame rate* in Options (issue #3).
- **Graphics card** choice for PCs with two (issue #3): the fast one (default) or the power-saving one.
- **Music and effects volume**, separately: the channels that play the game's ATRAC music follow the music volume.
- **Skip the movies:** every cutscene video ends as it starts (any movie could already be skipped by hand).
- **Light preset** (x2) for integrated GPUs, and the starting preset on PCs without a dedicated one (issue #3: x3/x4
  stuttered). Balanced (x4) took 21 ms of GPU time per frame on an Intel UHD 770, more than 60 FPS allows; x2 takes 14.
- **Infinite health** (a cheat, off by default).
- **Import PPSSPP saves** (Options > Advanced): copies the saves of both games from a PPSSPP memory stick. Encrypted
  ones are named, with how to save them unencrypted in PPSSPP.
- **Gamepad rumble when Kratos is hit**, stronger for a bigger hit (on by default; the PSP has none).
- **Circle confirms, Cross cancels** as an option (the Japanese layout).
- **Separate graphics settings for each game**, as an option: e.g. Ultra for one game and Light for the other.
- **AI-upscaled textures:** *Upscale dumped textures (AI)* (Options > Advanced) turns the dumped textures into a
  texture pack with Real-ESRGAN, downloaded once (about 45 MB) after asking. It all runs on your PC.
- **Discord:** *Show what I play on Discord* (off by default) shows the game and the time played in your status.
- **The launcher in Spanish, French and Hebrew** (Options > Advanced > Launcher language): the window's labels,
  buttons and options; longer help texts stay in English.
- **HUD at the screen edges** at 21:9 and 32:9 (experimental, off by default): the HUD keeps its shape at the sides
  of the screen instead of in the middle. Text drawn across the middle can come apart; please report what you see.
- **Fix: a full graphics card closed the game without a word** (found by the v1.3.0 level tours with eight games on
  one GPU). The game now stops with "out of GPU memory" in its log, and the launcher offers a lighter preset.
- **Fix: a failed launcher self-update left its dialog on "Downloading"**; it now says what went wrong.
- **Fix: a damaged `launcher.json`** (for example a hand-edited `settings_version`) could stop the launcher at start;
  such values are reset to defaults like the others.
- For contributors: `RT_TEST_KEYS` simulates held keys to test bindings without a focused window; new options
  `--no-stick-evade`, `--language`, `--crt`, `--show-fps`, `--gpu-power`, `--music-volume`, `--sfx-volume`,
  `--skip-movies`, `--rumble`, `--confirm`, `--hud-edges`; `tools/texture_upscale.py`, `tools/regression_check.py`.

## v1.2.1 (2026-10-23)

- **Fix: crash at launch or a black screen with MSAA on Vulkan** (issue #3), at every MSAA level, with or without
  anisotropic filtering; Chains of Olympus was affected. Direct3D 12 (the *Auto* choice) was not.
- **A lost graphics device stops the game with a clear message** in the log and the launcher's dialog, instead of a
  crash.
- **Fix: choppy music at 120 FPS and above** (issue #3) when the PC can't hold the chosen rate: the time skips that
  keep the game at full speed now leave the music thread room to queue its next block.
- **Fix: the comma key can be bound** (issue #3): a control bound to it got no key at all. Settings saved by v1.2.0
  are fixed when the game starts.
- **Bug reports carry no personal paths:** the logs in the *Report a bug* ZIP show your folders as `<game folder>`,
  `<user folder>` and so on, and your Windows user name as `<user>`. Before, the game log named the disc image by
  its full path.
- **Skipped movies are in the input recording,** so the recording a bug report attaches replays past a skipped
  intro.
- **Movies skip on the keys and buttons bound to Start or Cross** (and on Enter and Space, as before). The click
  that brings the window to the front no longer skips a movie.
- **Self-update:** downloads only over https, and the message for a bad download says what the check proves (a
  damaged download, not a forged release: the launcher is not signed).
- **A damaged disc image can't make the launcher's disc reader read past its buffers.**
- For contributors: CI builds of pull requests upload their artifacts again, and `AGENTS.md` describes the project
  after the launch.

## v1.2.0 (2026-10-18)

- **Fix: crash with anisotropic filtering** on some GPUs (issue #3). Filtering is only applied where it can work, the
  sampler settings are clamped, and a GPU that refuses them gets plain filtering instead of a crash. The log names the
  graphics adapter and driver.
- **Crashes are always logged** with the module and offset, and the launcher says why the game closed and offers a
  fix (turn off anisotropic filtering, try the other graphics API, open the log, report a bug).
- **High and Ultra presets use 4x anisotropic filtering** (was 16x): it looks nearly the same at these
  resolutions and is lighter on weaker GPUs; 8x and 16x stay available in the settings.
- **Graphics API option** in the launcher: Auto, Direct3D 12 or Vulkan.
- **Fix: choppy sound at high frame rates** (issue #3). Skipped time is capped so sound that is playing stays whole;
  the starting preset keeps 60 FPS, and the FPS setting says what a higher rate costs. Remaining dropouts are logged.
- **Texture packs:** the launcher shows whether a pack matches the game and warns about PPSSPP packs, which use a
  different format (issue #4); a *Dump textures* option writes the game's textures as a starting point.
- **Controls page:** rebind keyboard keys and gamepad buttons in the launcher (`--controls FILE` for the game).
  The mouse pointer hides in fullscreen after 2 seconds.
- **Settings check:** out-of-range or broken settings are reset to defaults with a note, instead of failing.
- **Report a bug / Open logs folder:** collects the logs, input recording and PC details (no file paths) into a ZIP
  and opens a prefilled GitHub issue. Play records the input to `GoW_PSP_data\build` for bug reports.
- **Add to Steam / Create desktop shortcut** per game, with the game's icon (issue #5).
- **Launcher self-update:** checks GitHub at start (opt-out in Advanced, or skip a version), shows what's new,
  downloads the ZIP, checks it against `SHA256SUMS.txt` and restarts. `GoW_PSP_data` and settings stay as they are.
- **No console window:** the launcher is now a windowed program, so no black console window opens next to it
  (from Explorer, a shortcut or Steam). Command-line options (`--version`, `--update`, `--build coo`) print in the
  terminal they're started from, or to `GoW_PSP_data\build\launcher_cli.log`.
- **Build progress** shows the real step count and the elapsed time.

## v1.1.5 (2026-10-17)

The number jumps from 1.1.0 to 1.1.5 on purpose: this release started as the v1.1.1 Git fix, which was never
published, and grew to include the launcher, quit and movie-skip changes below. Versions 1.1.1 to 1.1.4 do not exist.

- **Fix: building failed on PCs without Git** (`could not find git for clone of libchdr-populate`). libchdr and
  SDL3 now download as pinned archives (SHA-256) instead of Git clones, and the launcher hides Git from CMake so it
  can't creep back. Nothing besides the launcher's own download is needed: no Git, Python or Visual Studio.
- `tools/check_no_git.py` (also in CI) fails if a Git fetch returns.
- **Fix: launcher background** could leave part of the window grey at some sizes and display scalings. It now always
  covers the window (maximized, any size, 100–150 % scaling), and drawing it takes less CPU (maximized: about 23 % of
  a core instead of 40–80 %).
- **Fix: GoS intro movie stuttered** on weaker PCs with the launcher maximized: the animated launcher competed with
  the game. The launcher now minimizes itself and stops its animation while a game runs (about 0.1 % of a core
  instead of about 60 %) and comes back when the game exits.
- **Quit options:** Alt+F4 now works (it did nothing before) and the close button quits at once. Esc asks
  "Quit the game?" with the game paused (Enter / Cross = yes, Esc / Circle = no) instead of quitting at once.
  Gamepad: hold Back + Start for 2 seconds.
- **Movies can be skipped** with Enter, Space, a mouse click, or gamepad Start or Cross (the original games don't
  allow it). The game goes on as if the movie had ended; recorded `--input` runs never skip.
- README: a Controls section.

## v1.1.0 (2026-10-17)

- **No Visual Studio needed:** the launcher downloads a portable toolchain once (llvm-mingw, CMake, Ninja; 246 MB,
  pinned SHA-256, after asking) and builds both games with it; no administrator rights, nothing installed in Windows.
  Visual Studio stays supported and is the fallback. Same software-renderer output (34/34, same WAV); results in
  `docs/phase15-portable-toolchain.md`.
- CI downloads the portable toolchain and builds `gow_disc` with it.

## v1.0.1 (2026-10-17)

Still a preview: every level loads and runs and saves work, but neither game has had a full playthrough yet.

- **Download is now a ZIP:** `GoW_PSP_Launcher-v1.0.1.zip` holds one `GoW_PSP` folder with `GoWLauncher.exe` and a
  short `README.txt`. The launcher keeps everything it creates in `GoW_PSP_data` next to itself; folders set up by
  v1.0.0 keep their layout, so builds and saves are kept.
- **Fullscreen by default** (F11 / Alt+Enter or the Fullscreen option switch to a window); existing settings switch to
  fullscreen once.
- **No console window** when the game starts. Its log is `GoW_PSP_data\build\launcher_coo.log` / `launcher_gos.log`;
  attach it to bug reports.
- **Faster first build:** about 3 minutes for the first game and under 2 for the second (was about 4 each):
  dependencies are downloaded once and shared, libraries compile in parallel.
- Docs: SmartScreen / antivirus note with SHA-256, one research setup (EBOOT via `gow_disc`), Discussions enabled,
  original social preview image. CI uploads the release ZIP.
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
