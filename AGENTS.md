# Instructions for AI coding agents

You are working on the user's Windows PC in `E:\AI\GoW_PSP\repo`, a static recompilation of the two PSP God of War
games into native PC ports. The user speaks Hebrew: report to them in Hebrew, briefly, and ask before anything risky.
Read `README.md`, `docs/phase13-graphics.md` and `docs/phase14-plan.md` first.

## Rules (never break these)

1. **Never commit, upload or paste game content:** `*.chd`, `*.iso`, anything in `build/`, `re/dumps/`, `re/ghidra/`,
   `media/`, generated C, `image.bin`, EBOOT/PRX files, screenshots of the game. Do not send them to any online service,
   including your own chat. Before every commit run `git status` and check that only source/docs files are staged.
2. Never rewrite git history, force-push, or delete branches/tags that exist on GitHub.
3. Do not kill processes you did not start. Run long builds and test runs in the background.
4. One task per commit. Commit messages: a short title line, a body that says what and why, and the user's approval.
5. If a check below fails, stop and tell the user; do not "fix" a check by changing what it compares against.

The order is: back up as a **private** repository (step 1), finish phase 14 (step 2), and only then launch publicly
with v1.0.0 (step 3). Nothing is public before step 3.

## Step 1: back up to a private GitHub repository (no release)

The work is committed on branch `phase2/threads-and-fs`, tag `v0.1.0` is local, `gh` is logged in as DrummingBird1.
Run these from `E:\AI\GoW_PSP\repo`, one by one, and stop if one fails:

```bash
git status --short
git ls-files | grep -iE '\.(chd|iso|cso|pbp|prx)$|^build/|^media/|^re/dumps/|^re/ghidra/' ; echo "must print nothing above"
gh repo create gow-psp-recomp --private --description "Native PC ports of the two PSP God of War games by static recompilation (bring your own disc): 4K, MSAA, HDR, 120+ FPS, 21:9" --source . --remote origin
git push origin HEAD:main
git push origin v0.1.0
```

`git status --short` may list `docs/` or `runtime/` changes of other sessions; that is fine (they are not pushed).
Push your phase 14 commits to `main` as you go (`git push origin HEAD:main`), so the backup stays current.

## Step 2: phase 14, the one-download launcher

Follow `docs/phase14-plan.md` (goal, tasks, acceptance checks). Do the tasks in this order, each in its own commit,
and tell the user after each one:

1. **Decrypt the EBOOT from the disc image** (task 1). Compare the result with `re/dumps/UCUS98653_EBOOT.BIN` and
   `re/dumps/UCUS98737_EBOOT.BIN` (PPSSPP dumps): they must be byte-identical. Wire it into `tools/launcher.pyw` so the
   EBOOT field is no longer needed.
2. **Toolchain without manual installs** (task 3). Start with the simple, robust way: the launcher checks for Python,
   CMake and Visual Studio Build Tools and, with the user's consent, installs the missing ones with `winget`
   (`Python.Python.3.12`, `Kitware.CMake`, `Microsoft.VisualStudio.2022.BuildTools` with the C++ workload). Only after
   that works end to end, and only if the user asks, try bundling a compiler (Zig/clang) instead.
3. **One-file launcher** (task 2): freeze `tools/launcher.pyw` + `recomp/` with PyInstaller into `GoWLauncher.exe`. It
   must produce the same `build/gen/<ID>_full/*.c` files, byte for byte, as running `recomp/recomp.py` directly.
4. **Launcher UX** (task 4): pick the game folder, detect each game's disc image (UCUS98653 / UCUS98737), Build with
   progress, Play. Follow "Launcher design" below; show the user a screenshot before moving on.
5. **Release workflow** (task 5): a GitHub Actions workflow that builds `GoWLauncher.exe` (no game data in CI); update
   `CHANGELOG.md` (a v1.0.0 entry) and the README quick start (download the launcher, point it at the game folder).

Phase 14 is done only when, on a fresh Windows user profile with only `GoWLauncher.exe` and the disc image, both games
build and reach gameplay, and the software-renderer check below still passes. Show the user, and wait for their go.

## Launcher design (task 4)

Today's `tools/launcher.pyw` works but is a plain grey form with raw numbers. Keep its logic (`play_command`,
`build_steps`, `build/launcher.json`) and redo the window like this:

- **Look:** dark theme (ttk `clam` styled): background `#121212`, panels `#1c1c1c`, text `#f0efec`, secondary text
  `#a8a69e`, accent red `#b3261e` (Play button, selection), gold `#c9a227` (headings). Segoe UI 10 pt, headings 12 pt,
  title 20 pt bold. Make the process DPI-aware on Windows (`ctypes.windll.shcore.SetProcessDpiAwareness(1)`) so the text
  is sharp at 150 % scaling. **No game logos, box art or the game's font** (trademarks and copyright): a text title,
  "God of War PSP: native PC ports (unofficial)".
- **Top:** the two games as two large cards side by side (title + status line: "Ready to play", "Needs building",
  "No disc image found"). Below them one field, **Game folder**: the launcher finds the `.chd`/`.iso` of each game in it
  (by the disc's ID, not the file name) and fills both cards.
- **Presets** (one row of buttons; the chosen one highlighted; changing any option below switches to "Custom"):

  | Preset | Resolution | MSAA | Anisotropic | Other |
  |---|---|---|---|---|
  | Original | x1 (480x272) | off | off | the PSP's look |
  | Balanced | x4 (1920x1088) | off | 8x | |
  | High | x6 (2880x1632) | 4x | 16x | |
  | Ultra | x8 (3840x2176) | 4x | 16x | |

- **Main options**, with words, not raw numbers: Resolution ("x1 480x272 (PSP)", "x2 960x544", "x3 1440x816",
  "x4 1920x1088 (1080p)", "x6 2880x1632", "x8 3840x2176 (4K)"); Anti-aliasing ("Off", "MSAA 2x/4x/8x", "FXAA");
  Frame rate ("60 (original)", "120", "144", "165", "240", "Unlimited (VRR)" = `--fps 1000`); Aspect ratio ("Original",
  "21:9", "32:9"); HDR ("Auto", "Off"); Fullscreen; Window size.
- **Advanced** (collapsed by default): Renderer (Hardware / Software (accurate, slow)), Effects resolution
  ("Match resolution" = 0, "Original glow" = 1), Anisotropic filtering, Sharpening (slider 0-1), V-Sync, Texture pack
  folder, Mute.
- **Bottom:** a big red **Play** button (disabled with a reason in its tooltip until the game is ready), a **Build**
  button that turns into a progress bar with the current step ("Decrypting", "Recompiling", "Compiling 37/113",
  "Linking"), and a "Show log" link that opens the full log in its own window. A footer line with the keys and links to
  the GitHub page and the license.
- Keep window size and position in `build/launcher.json`. Every control reachable with the keyboard.

**Make it feel like God of War, legally.** The user's goal: the launcher should look as close to the games as possible
and be as simple as possible. Nothing from the games may be in the repository or the download, but the player's own disc
is on their PC, so the launcher can take the art from there at run time:

- **Game cards from the player's disc:** after the game folder is chosen, extract each game's `PSP_GAME/PIC1.PNG`
  (480x272 background art) and `PSP_GAME/ICON0.PNG` (144x80 icon) with `gow_rt --disc <image> --extract
  disc0:/PSP_GAME/PIC1.PNG <out>` into `build/launcher/<game>/` (never committed) and use them as the card backgrounds
  and icons. After the first build, also render the game's own main menu headless at high resolution
  (`--renderer gpu --res-scale 4 --frames-at 1500 --max-frames 1500`, GoS; find the menu frame for CoO) and use it,
  darkened and blurred, as the window background behind the selected game. Before the disc is chosen, show the plain
  dark theme.
- **Typography:** bundle the free "Cinzel" font (SIL Open Font License, Roman inscription capitals, close to the games'
  titles; include its OFL text next to it) for the title, headings and buttons; Segoe UI for small text. On Windows load
  it privately with `ctypes.windll.gdi32.AddFontResourceExW(path, 0x10, 0)` before creating the Tk fonts.
- **Ornament, drawn by code:** a thin gold Greek-key (meander) border along the top and bottom of the window and around
  the selected card, a dark stone/marble texture generated with noise (Pillow), a soft red glow behind the Play button.
  Generated at run time or as small PNGs made by a script in the repository (original work, not taken from the games).
- **Simple first run:** a three-step flow instead of a form: 1. "Choose the folder with your games", 2. "Preparing
  <game> (one time, about 10 minutes)" with the progress bar, 3. the game card with a big **Play**. The options sit
  behind a gear button, and the starting preset is picked for the PC: a discrete GPU → High, otherwise Balanced; HDR on
  when the display has it; frame rate = the display's refresh rate when it is 120 Hz or more.
- **Toolkit:** if Tkinter cannot reach this look (images behind controls, rounded cards, hover effects), switching to a
  toolkit that can (e.g. `customtkinter`, MIT, or PySide6, LGPL) is fine; keep the frozen exe small and say what it adds.
- **Optional, only if cheap:** key mapping. The runtime has the keys fixed in `runtime/rt_host.c` (`live_input`); it would
  need a `--keymap file` option there first.

## Step 3: launch (only after the user approves the finished phase 14)

```bash
git ls-files | grep -iE '\.(chd|iso|cso|pbp|prx)$|^build/|^media/|^re/dumps/|^re/ghidra/' ; echo "must print nothing above"
git tag -a v1.0.0 -m "v1.0.0: one-download launcher (preview)"
git push origin HEAD:main
git push origin v1.0.0
gh repo edit DrummingBird1/gow-psp-recomp --visibility public --accept-visibility-change-consequences
gh repo edit DrummingBird1/gow-psp-recomp --add-topic static-recompilation,psp,reverse-engineering,recompiler,god-of-war,pc-port
gh release create v1.0.0 --title "v1.0.0 (preview)" --notes "<preview note + the v1.0.0 section of CHANGELOG.md>" GoWLauncher.exe
```

Send the user the repository and release links.

## How to build and check (the commands that already work)

```bash
.venv/Scripts/python.exe tools/launcher.pyw --build coo
.venv/Scripts/python.exe tools/launcher.pyw --build gos
.venv/Scripts/python.exe tools/launcher.pyw --print-command coo
```

Software renderer must stay pixel-identical (after any change to `runtime/` or the toolchain), about 6 minutes:

```bash
mkdir -p build/check/ms0
build/cmake_full/Release/gow_rt_UCUS98653_full.exe --image build/gen/UCUS98653_full/image.bin --boot --disc "God of War - Chains of Olympus (USA).chd" --ms0 build/check/ms0 --input build/rec_v9/input.txt --frames-dir build/check --frames-every 200 --max-frames 6800 --cpu-ms 1200000 --wall-ms 1200000 --wav build/check/mix.wav
.venv/Scripts/python.exe -I tools/check_frames.py build/rec_v9 build/check
sha256sum build/check/mix.wav
```

Expected: 34 frames `identical`, and the SHA-256 `1c1ad6762270d1dead70c3faa0997f9fb4cff264c111420b3221a621cdff5869`.
Use a fresh, empty `ms0` folder every time (a save in it changes the boot).

Speed (after a toolchain change): the same command with `--renderer gpu --perf-csv build/check/perf.csv` and no
`--frames-every`, three runs, and compare the average of `host_frame_ms` over frames 4700-5400 with the old build.

## Pitfalls already met

- Git Bash heredocs drop backslashes: write files that contain `\` with your file tool, not `cat <<EOF`.
- Paths with spaces (the disc images) need quotes; `gh api` needs endpoints without a leading `/` in Git Bash.
- The machine often runs other heavy programs: measure speed as the best of three runs, never from one run.
- `build/` holds the user's own game data and earlier results (`build/rec_v9` is the reference): never delete it.
