# Allegrex static recompiler (Phase 1 prototype)

Translates the decrypted EBOOT of God of War: Chains of Olympus / Ghost of Sparta into C,
which is compiled together with a minimal runtime into a native Windows executable.

> Generated output (`build/gen/**`, `image.bin`) and the dumps contain Sony's code.
> Personal use only; do not distribute.

## Pipeline

```
re/dumps/<ID>_EBOOT.BIN ──► pspelf.py (load + PSP relocations, base 0x08804000)
Ghidra project ──► ExportRecompInput.java ──► <ID>.recomp.json (functions, ranges, jump tables)
                     │
                     ▼
recomp.py ── allegrex.py (decode) ── emitc.py (C emit) ──► build/gen/<ID>/{code_*.c, stubs.c, table.c, ...}
                                                              │
runtime/ (psp_rt.h, rt_core.c, rt_hle.c, rt_main.c) ──────────┴─► CMake/MSVC ─► gow_rt_<ID>.exe
```

| File | Role |
|---|---|
| `pspelf.py` | ELF/PRX parsing, `SHT_PSPREL` relocations (PPSSPP semantics), module info, imports |
| `verify_reloc.py` | Checks the relocator against Ghidra's loaded image word by word |
| `code_pointers.py` | Finds code addresses built by relocated `lui/addiu` pairs that Ghidra missed (seeds for `SeedFunctionsFromRelocations.java`) |
| `allegrex.py` | Instruction decoder (encodings from PPSSPP `MIPSTables.cpp`) |
| `emitc.py` | Per-function C generation: delay slots, branch-likely, jump tables, tail calls, HLE calls |
| `recomp.py` | Driver: translates every function in memory (coverage), writes the selected subset |
| `difftest.py` | Differential test vs Unicorn (MIPS32 24Kf) on random inputs |
| `vfpu_check.py` | Checks VFPU operand decoding against Ghidra's disassembly (`re/scripts/ListVfpu.java` output) |

## Translation model

* Function bodies come from Ghidra, then are completed by the recompiler's own control-flow walk from each entry (code Ghidra left out, e.g. a fall-through, is added; other entries end the walk).
* One C function per guest function: `void f_XXXXXXXX(RtCtx *c)`; CPU state lives in `RtCtx`.
* Guest memory is a 512MB arena indexed by `addr & 0x1FFFFFFF` (cached/uncached/kernel mirrors map to the same bytes).
* Branch conditions and `jr` targets are captured before the delay slot executes; branch-likely runs the delay slot only when taken.
* `jal` to a recompiled function is a direct C call; `jr $ra` returns; jumps/branches out of the function are tail calls.
* Jump tables recovered by Ghidra become `switch` statements; other `jr`/`jalr` go through `rt_call` (address → function lookup).
* Calls to import stubs go to generated `f_<stub>` functions that call `rt_hle(nid, name)`.
* Instruction semantics (div by zero, float→int conversion, FCR31, lwl/lwr, Allegrex extensions) follow PPSSPP's interpreter.
* VFPU instructions become calls to the runtime's VFPU interpreter (`rt_vfpu(c, word, pc)`, semantics from
  PPSSPP incl. prefixes); `bvf/bvt` are emitted as C branches. `vfpu_check.py` checks the operand decoding
  against Ghidra; `gow_rt --vfpu-selftest` checks the semantics (see `docs/phase3-vfpu.md`).

## Rebuild

```bash
python -I recomp/recomp.py --elf re/dumps/UCUS98653_EBOOT.BIN --ghidra re/out/recomp/UCUS98653_EBOOT.BIN.recomp.json --nids re/out/psp_nids_ppsspp.tsv --out build/gen/UCUS98653
```

```bash
cmake -S . -B build/cmake -G "Visual Studio 18 2026" -A x64 -DGAME=UCUS98653
```

```bash
cmake --build build/cmake --config Release
```

```bash
.venv/Scripts/python.exe -I recomp/difftest.py build/gen/UCUS98653 build/cmake/Release/gow_rt_UCUS98653.exe --cases 8
```

Full build (every translatable function) for the boot run:

```bash
python -I recomp/recomp.py --elf re/dumps/UCUS98653_EBOOT.BIN --ghidra re/out/recomp/UCUS98653_EBOOT.BIN.recomp.json --nids re/out/psp_nids_ppsspp.tsv --out build/gen/UCUS98653_full --difftest 0 --boot 0 --all
```

```bash
cmake -S . -B build/cmake_full -G "Visual Studio 18 2026" -A x64 -DGAME=UCUS98653_full
```

```bash
cmake --build build/cmake_full --config Release
```

```bash
build/cmake_full/Release/gow_rt_UCUS98653_full.exe --image build/gen/UCUS98653_full/image.bin --boot --disc "God of War - Chains of Olympus (USA).chd" --ms0 build/ms0
```

The boot runs under the thread scheduler (`runtime/rt_thread.c`) and reads the disc
through libchdr (`runtime/rt_io.c`); see `docs/phase2-threads-fs.md`.

Display lists are executed by `runtime/rt_ge.c` and drawn by the software renderer
(`runtime/rt_gesoft.c`). Frame output (`runtime/rt_frames.c`) for documentation and performance tracking:

| Option | Effect |
|---|---|
| `--frames-dir DIR --frames-every N` / `--frames-at N,N,...` | Writes displayed frames as `DIR/frame_NNNNNN.png` |
| `--record out.mp4 [--ffmpeg exe]` | Pipes every virtual vblank (59.94 Hz) to ffmpeg; the video plays at game speed |
| `--record-host out.mp4` | Same on the host clock: shows how smoothly this machine runs it |
| `--perf-csv frames.csv` | Host time per displayed frame; a `[perf]` summary (fps, frame-time percentiles) is printed at exit |
| `--max-frames N` | Stops after displayed frame N (runs of equal length for comparisons) |
| `--fast-forward N` | Draws nothing before displayed frame N; the game runs exactly the same, about twice as fast (long replays) |
| `--profile` | Sampling profiler of the main thread; top functions printed at exit (needs the Release PDB) |

Playing in a window (`runtime/rt_host.c`, SDL3):

```bash
build/cmake_full/Release/gow_rt_UCUS98653_full.exe --image build/gen/UCUS98653_full/image.bin --boot --disc "God of War - Chains of Olympus (USA).chd" --ms0 build/ms0 --window
```

| Option / key | Effect |
|---|---|
| `--window [--scale N] [--fullscreen] [--nearest]` | Shows the game and runs it in real time (the game slows down when the host cannot keep up) |
| Keyboard | Arrows = D-pad, Z = ✕, X = ○, A = □, S = △, Q/W = L/R, Space or Enter = Start, V = Select, I/J/K/L = analog stick |
| Gamepad | PSP layout (south button = ✕), left stick = analog stick, both shoulders and triggers = L/R |
| F11 or Alt+Enter, F12, Esc | Fullscreen, screenshot to `--shots DIR`, quit |
| `--input "1000:cross:6,t27.5:start:0.3"` | Scripted presses by displayed frame (or `t` + virtual seconds), headless or windowed; buttons `cross circle square triangle up down left right l r start select aup adown aleft aright` and `lx=N`/`ly=N` (stick, 0-255), joined with `+`. Also takes a file of entries (`#` comments), e.g. a recording |
| `--record-input file` | Writes every change of what the game reads from the controller, by displayed frame, in the `--input` format; `--input file` replays it read for read, headless too (see `docs/phase9-saves-replay.md`) |
| `--save-slot NAME` | The save slot that the game's save/load lists use (`DATA00`..`DATA14` in Chains of Olympus); otherwise saves go to the first empty slot (then the oldest) and loads take the newest |

Without `--window` the run stays headless and deterministic (virtual time as fast as the host allows).
`tools\play.cmd` starts Chains of Olympus in a window (extra arguments are passed on, e.g. `--fullscreen`). Saves go to
`build\ms0\PSP\SAVEDATA`, and every session is recorded in `build\sessions\<date_time>\`; `tools\replay.cmd` replays one
headless, from the saves it started with to the frame where it stopped or crashed.

Sound (`runtime/rt_audio.c`, `rt_sas.c`, `rt_atrac.c`; see `docs/phase7-audio.md`): sceAudio channels are mixed on
the virtual clock; sceSasCore (sound effects) and sceAtrac3plus (music) are implemented in the runtime.

| Option | Effect |
|---|---|
| (with `--window`) | Plays through the default audio device (SDL3), ~50 ms latency |
| `--mute` | No sound in the window |
| `--wav out.wav` | Writes the whole mix in game time, from virtual time 0 (headless or windowed; deterministic headless) |
| `--disc game.chd --extract disc0:/PSP_GAME/USRDIR/music/MAINMENU.AT3 out.at3` | Copies a file off the disc (to check decoders against ffmpeg); also takes `disc0:/sce_lbn0x..._size0x...` names, and lists a directory with each file's sectors |

The ATRAC3/ATRAC3+ decoder (PPSSPP's `ext/at3_standalone`, FFmpeg's decoder, LGPL-2.1+) is downloaded by CMake at
configure time from PPSSPP tag v1.20.4 and checked against SHA-256 hashes; it is not part of this repository.

Movies (`runtime/rt_mpeg.c`, `rt_h264dec.cpp`; see `docs/phase8-movies.md`): sceMpeg demuxes the PSMF stream the game
feeds through its ring buffer; H.264 pictures are decoded by OpenH264 and ATRAC3+ sound by the decoder above.

| Option | Effect |
|---|---|
| `--movie-test movie.pmf out [audio channel]` | Demuxes and decodes a movie file without the game: `out.yuv` (I420, to compare with `ffmpeg -f rawvideo -pix_fmt yuv420p`), `out.wav`, and a PNG every 300 pictures |

OpenH264's decoder (Cisco, BSD-2-Clause) is downloaded by CMake at configure time, file by file from tag v2.6.0, and
checked against the SHA-256 hashes in `cmake/openh264-v2.6.0.sha256`; it is not part of this repository.

The renderer draws on all logical processors (`RT_GE_THREADS=N` overrides); `RT_GE_TRACE_LIST=N`
prints every primitive of display list N with the pixels it shaded.

```bash
build/cmake_full/Release/gow_rt_UCUS98653_full.exe --image build/gen/UCUS98653_full/image.bin --boot --disc "God of War - Chains of Olympus (USA).chd" --ms0 build/ms0 --cpu-ms 60000 --frames-dir build/frames --frames-every 120 --record build/frames/run.mp4 --ffmpeg C:/Tools/ffmpeg/bin/ffmpeg.exe --perf-csv build/frames/perf.csv
```

Documentation tools (`tools/`): `render_terminal.py` turns a command's real output into a terminal-style PNG;
`run_logged.sh` records several commands with their output for it; `check_frames.py` compares the frames of
two runs (renderer changes must keep them identical) and summarizes frame times; `perf_report.py` draws a
frame-time chart and a side-by-side host-clock video with a live fps overlay; `audio_report.py` prints sound levels
of a `--wav`, matches it against a reference decode (dB) and draws a labelled spectrogram. Results go to `docs/progress.md`.
`capture_window.ps1` captures one window by process id (for when the port has a window of its own).

When Ghidra functions change, re-export with `ExportRecompInput.java` and re-run
`code_pointers.py` to check for code pointers that still lack a function.
