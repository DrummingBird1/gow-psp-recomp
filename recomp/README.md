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

## Translation model

* Function bodies come from Ghidra, then are completed by the recompiler's own control-flow walk from each entry (code Ghidra left out, e.g. a fall-through, is added; other entries end the walk).
* One C function per guest function: `void f_XXXXXXXX(RtCtx *c)`; CPU state lives in `RtCtx`.
* Guest memory is a 512MB arena indexed by `addr & 0x1FFFFFFF` (cached/uncached/kernel mirrors map to the same bytes).
* Branch conditions and `jr` targets are captured before the delay slot executes; branch-likely runs the delay slot only when taken.
* `jal` to a recompiled function is a direct C call; `jr $ra` returns; jumps/branches out of the function are tail calls.
* Jump tables recovered by Ghidra become `switch` statements; other `jr`/`jalr` go through `rt_call` (address → function lookup).
* Calls to import stubs go to generated `f_<stub>` functions that call `rt_hle(nid, name)`.
* Instruction semantics (div by zero, float→int conversion, FCR31, lwl/lwr, Allegrex extensions) follow PPSSPP's interpreter.
* VFPU is not translated yet (81 / 109 functions); calling one ends the run with "function not in this build".

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

When Ghidra functions change, re-export with `ExportRecompInput.java` and re-run
`code_pointers.py` to check for code pointers that still lack a function.
