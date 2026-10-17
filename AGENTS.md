# Instructions for AI coding agents

You are working on the user's Windows PC in `E:\Projects\GoW_PSP`, a static recompilation of the two PSP God of War
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

## Step 1: publish v0.1.0 (already prepared, just run it)

The work is committed on branch `phase2/threads-and-fs`, tag `v0.1.0` is local, `gh` is logged in as DrummingBird1.
Run these from `E:\Projects\GoW_PSP`, one by one, and stop if one fails:

```bash
git status --short
git ls-files | grep -iE '\.(chd|iso|cso|pbp|prx)$|^build/|^media/|^re/dumps/|^re/ghidra/' ; echo "must print nothing above"
gh repo create gow-psp-recomp --public --description "Native PC ports of the two PSP God of War games by static recompilation (bring your own disc): 4K, MSAA, HDR, 120+ FPS, 21:9" --source . --remote origin
git push origin HEAD:main
git push origin v0.1.0
gh release create v0.1.0 --title "v0.1.0" --notes-file build/release_notes_v0.1.0.md
gh repo edit DrummingBird1/gow-psp-recomp --add-topic static-recompilation,psp,reverse-engineering,recompiler,god-of-war,pc-port
```

`git status --short` may list `docs/` or `runtime/` changes of other sessions; that is fine (they are not pushed).
If the tag is not on the pushed commit (`git describe --tags HEAD` is not `v0.1.0`), push only `main` and ask the user.
Send the user the repository link when done.

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
   progress, Play.
5. **Release v0.2.0** (task 5): a GitHub Actions workflow that builds `GoWLauncher.exe` (no game data in CI), update
   `CHANGELOG.md` and `README.md`, tag `v0.2.0`, `gh release create v0.2.0` with the exe attached.

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
