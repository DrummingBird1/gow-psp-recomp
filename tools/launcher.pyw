"""Launcher for the native PC ports of God of War: Chains of Olympus and Ghost of Sparta.

Picks the game, builds it from your own disc image (the EBOOT is decrypted from it), sets the
graphics options of the hardware renderer (docs/phase13-graphics.md) and starts it. Settings are kept
in build/launcher.json (your paths stay on your machine; build/ is not in git).

    python tools/launcher.pyw                     the window
    python tools/launcher.pyw --build coo|gos     build without the window (recompile + CMake)
    python tools/launcher.pyw --play coo|gos      start with the saved settings
    python tools/launcher.pyw --print-command coo|gos
    python tools/launcher.pyw --check-tools       show where Python, CMake and Visual Studio were found

Building needs Python 3, CMake and Visual Studio with C++: Build finds them and, if some are missing, offers to
install them with winget (tools/toolchain.py; GOW_SIMULATE_MISSING=all shows the commands without installing). The EBOOT is decrypted from the
disc image into build/eboot/ by gow_disc (built once into build/cmake_tools)."""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain  # noqa: E402

# Frozen (GoWLauncher.exe, tools/build_launcher.py): the exe carries the sources the build needs and unpacks
# them next to itself on the first Build; build/ is made there too.
FROZEN = getattr(sys, "frozen", False)
ROOT = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
CONFIG = ROOT / "build" / "launcher.json"
GAMES = {
    "coo": {"id": "UCUS98653", "title": "God of War: Chains of Olympus", "build": "build/cmake_full", "ms0": "build/ms0"},
    "gos": {"id": "UCUS98737", "title": "God of War: Ghost of Sparta", "build": "build/cmake_gos_full", "ms0": "build/ms0_gos"},
}
DEFAULTS = {
    "game": "coo", "disc": {}, "renderer": "gpu", "res_scale": 2, "fx_scale": 0, "msaa": 1, "aniso": 1,
    "fxaa": False, "sharpen": 0.0, "hdr": False, "fps": 60, "vsync": False, "aspect": "native", "fullscreen": False,
    "window_scale": 2, "texture_pack": "", "mute": False,
}
KEYS = ("Keys: arrows = D-pad, Z = cross, X = circle, A = square, S = triangle, Q/W = L/R, Space/Enter = Start, "
        "V = Select, I/J/K/L = stick, F11 = fullscreen, F12 = screenshot, Esc = quit. Gamepads: PSP layout.")


def load_config():
    cfg = dict(DEFAULTS)
    try:
        cfg.update(json.loads(CONFIG.read_text(encoding="utf-8")))
    except (OSError, ValueError):
        pass
    return cfg


def save_config(cfg):
    CONFIG.parent.mkdir(parents=True, exist_ok=True)
    CONFIG.write_text(json.dumps(cfg, indent=1), encoding="utf-8")


def exe_path(game):
    g = GAMES[game]
    return ROOT / g["build"] / "Release" / f"gow_rt_{g['id']}_full.exe"


def gen_dir(game):
    return ROOT / "build" / "gen" / f"{GAMES[game]['id']}_full"


def unpack_sources(out=print):
    """Frozen only: copies the bundled sources (CMakeLists.txt, runtime/, cmake/, re/out tables) next to the exe
    when they are missing or from another launcher version."""
    if not FROZEN:
        return
    src = Path(sys._MEIPASS) / "src"
    manifest = (src / "manifest.txt").read_text(encoding="utf-8")
    stamp = ROOT / "build" / "launcher_sources.txt"
    if stamp.exists() and stamp.read_text(encoding="utf-8") == manifest:
        return
    out("unpacking the build sources next to the launcher")
    for line in manifest.splitlines():
        rel = line.split("  ", 1)[1]
        dst = ROOT / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src / rel, dst)
    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.write_text(manifest, encoding="utf-8")


def recomp_command(python):
    """recomp/recomp.py, run by Python, or by the frozen launcher itself (--recomp: no Python needed)."""
    if FROZEN:
        return [sys.executable, "--recomp"]
    return [python, "-I", str(ROOT / "recomp" / "recomp.py")]


def run_recomp(args):
    """--recomp: recomp.py's main, in this process (the frozen launcher carries recomp/)."""
    import recomp
    sys.argv = ["recomp.py"] + args
    return recomp.main()


TOOLS_BUILD = ROOT / "build" / "cmake_tools"
DISC_TOOL = TOOLS_BUILD / "Release" / "gow_disc.exe"


def eboot_path(game):
    """Where the EBOOT decrypted from the disc image goes (build/ is not in git)."""
    return ROOT / "build" / "eboot" / f"{GAMES[game]['id']}_EBOOT.BIN"


def check_disc(cfg, game):
    """The disc image of this game; raises ValueError when it is missing (run_build checks its disc ID)."""
    disc = cfg["disc"].get(game, "")
    if not disc or not Path(disc).is_file():
        raise ValueError("Choose the disc image (.chd or .iso) of this game.")
    return disc


def play_command(cfg, game):
    """The gow_rt command line for these settings."""
    g = GAMES[game]
    cmd = [str(exe_path(game)), "--image", str(gen_dir(game) / "image.bin"), "--boot", "--disc", cfg["disc"].get(game, ""),
           "--ms0", str(ROOT / g["ms0"]), "--window", "--scale", str(cfg["window_scale"]), "--shots", str(ROOT / "build" / "screenshots"),
           "--renderer", cfg["renderer"]]
    if cfg["fullscreen"]:
        cmd.append("--fullscreen")
    if cfg["mute"]:
        cmd.append("--mute")
    if cfg["renderer"] == "gpu":
        if cfg["res_scale"] > 1:
            cmd += ["--res-scale", str(cfg["res_scale"])]
        if cfg["fx_scale"]:
            cmd += ["--fx-scale", str(cfg["fx_scale"])]
        if cfg["msaa"] > 1:
            cmd += ["--msaa", str(cfg["msaa"])]
        if cfg["aniso"] > 1:
            cmd += ["--aniso", str(cfg["aniso"])]
        if cfg["fxaa"]:
            cmd.append("--fxaa")
        if cfg["sharpen"] > 0:
            cmd += ["--sharpen", f"{cfg['sharpen']:.2f}"]
        if cfg["hdr"]:
            cmd.append("--hdr")
        if cfg["vsync"]:
            cmd.append("--vsync")
        if cfg["aspect"] != "native":
            cmd += ["--aspect", cfg["aspect"]]
        if cfg["texture_pack"]:
            cmd += ["--texture-pack", cfg["texture_pack"]]
    if int(cfg["fps"]) != 60:
        cmd += ["--fps", str(cfg["fps"])]
    return cmd


def build_steps(cfg, game, tools=None):
    """Build gow_disc (once), decrypt the EBOOT from the disc image, recompile it into C (skipped when this
    EBOOT was already recompiled), configure, build. A step is (command, condition or None)."""
    g, disc = GAMES[game], check_disc(cfg, game)
    tools = tools or {}
    cmake, python = tools.get("cmake") or "cmake", tools.get("python") or sys.executable
    eboot = eboot_path(game)
    stamp = gen_dir(game) / "eboot.sha256"
    steps = []
    if not DISC_TOOL.exists():
        steps.append(([cmake, "-S", str(ROOT), "-B", str(TOOLS_BUILD), "-DGOW_TOOLS_ONLY=ON"], None))
        steps.append(([cmake, "--build", str(TOOLS_BUILD), "--config", "Release", "--target", "gow_disc", "--parallel"], None))
    steps.append(([str(DISC_TOOL), "--disc", disc, "--id", "--decrypt-eboot", str(eboot)], None))

    def needs_recomp():
        digest = hashlib.sha256(eboot.read_bytes()).hexdigest()
        return not (gen_dir(game) / "image.bin").exists() or not stamp.exists() or stamp.read_text().strip() != digest

    steps.append((recomp_command(python) + ["--elf", str(eboot),
                   "--ghidra", str(ROOT / "re/out/recomp" / f"{g['id']}_EBOOT.BIN.recomp.json"),
                   "--nids", str(ROOT / "re/out/psp_nids_ppsspp.tsv"), "--out", str(gen_dir(game)),
                   "--difftest", "0", "--boot", "0", "--all"], needs_recomp))
    steps.append(([cmake, "-S", str(ROOT), "-B", str(ROOT / g["build"]), f"-DGAME={g['id']}_full"], None))
    steps.append(([cmake, "--build", str(ROOT / g["build"]), "--config", "Release", "--parallel"], None))
    return steps, stamp


def run_build(cfg, game, out=print, ask=toolchain.ask_console):
    try:
        check_disc(cfg, game)
    except ValueError as e:
        out(str(e))
        return False
    tools = toolchain.ensure(ask, out)
    if not tools:
        return False
    unpack_sources(out)
    try:
        steps, stamp = build_steps(cfg, game, tools)
    except ValueError as e:
        out(str(e))
        return False
    eboot = eboot_path(game)
    eboot.parent.mkdir(parents=True, exist_ok=True)
    for cmd, cond in steps:
        if cond is not None and not cond():
            out("recompiled C is up to date for this EBOOT, skipping recomp.py")
            continue
        out("> " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
        p = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        lines = []
        for line in p.stdout:
            lines.append(line.rstrip())
            out(line.rstrip())
        if p.wait():
            out(f"failed (exit {p.returncode})")
            return False
        if cmd[0] == str(DISC_TOOL) and (not lines or lines[0].strip() != GAMES[game]["id"]):
            out(f"this disc image is {lines[0].strip() if lines else 'unknown'}, not {GAMES[game]['id']} ({GAMES[game]['title']})")
            return False
        if cond is not None:  # the recompile step
            stamp.write_text(hashlib.sha256(eboot.read_bytes()).hexdigest())
    out(f"built {exe_path(game)}")
    return True


def start_game(cfg, game):
    if not exe_path(game).exists():
        raise ValueError("Not built yet: choose the disc image and press Build.")
    check_disc(cfg, game)
    (ROOT / GAMES[game]["ms0"]).mkdir(parents=True, exist_ok=True)
    (ROOT / "build" / "screenshots").mkdir(parents=True, exist_ok=True)
    log = open(ROOT / "build" / f"launcher_{game}.log", "w", encoding="utf-8", errors="replace")
    return subprocess.Popen(play_command(cfg, game), cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)


def gui():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    if FROZEN and os.name == "nt":  # started by double-click: hide the console window the exe opened
        import ctypes
        k32 = ctypes.windll.kernel32
        if k32.GetConsoleProcessList((ctypes.c_uint * 2)(), 2) == 1:
            ctypes.windll.user32.ShowWindow(k32.GetConsoleWindow(), 0)
    cfg = load_config()
    root = tk.Tk()
    root.title("God of War PSP native ports: launcher")
    v = {k: (tk.BooleanVar if isinstance(DEFAULTS[k], bool) else tk.StringVar)(value=cfg[k]) for k in
         ("game", "renderer", "res_scale", "fx_scale", "msaa", "aniso", "fxaa", "sharpen", "hdr", "fps", "vsync", "aspect",
          "fullscreen", "window_scale", "texture_pack", "mute")}
    disc, status = tk.StringVar(), tk.StringVar()

    def collect():
        for k, var in v.items():
            val = var.get()
            cfg[k] = val if isinstance(DEFAULTS[k], (bool, str)) else (float(val) if isinstance(DEFAULTS[k], float) else int(float(val)))
        cfg["disc"][cfg["game"]] = disc.get()
        save_config(cfg)
        return cfg

    def on_game(*_):
        g = v["game"].get()
        disc.set(cfg["disc"].get(g, ""))
        status.set("built" if exe_path(g).exists() else "not built yet")

    frm = ttk.Frame(root, padding=10)
    frm.grid(sticky="nsew")
    row = 0
    for key, g in GAMES.items():
        ttk.Radiobutton(frm, text=g["title"], variable=v["game"], value=key, command=lambda: (collect_game_switch(), on_game())).grid(
            row=row, column=0, columnspan=3, sticky="w")
        row += 1
    prev = {"game": v["game"].get()}

    def collect_game_switch():
        cfg["disc"][prev["game"]] = disc.get()
        prev["game"] = v["game"].get()

    def path_row(label, var, types):
        nonlocal row
        ttk.Label(frm, text=label).grid(row=row, column=0, sticky="w")
        ttk.Entry(frm, textvariable=var, width=60).grid(row=row, column=1, sticky="we")
        ttk.Button(frm, text="...", width=3, command=lambda: var.set(filedialog.askopenfilename(filetypes=types) or var.get())).grid(
            row=row, column=2)
        row += 1

    path_row("Disc image", disc, [("PSP disc", "*.chd *.iso"), ("All", "*.*")])
    ttk.Label(frm, textvariable=status).grid(row=row, column=1, sticky="w")
    row += 1

    def combo(label, key, values, hint=""):
        nonlocal row
        ttk.Label(frm, text=label).grid(row=row, column=0, sticky="w")
        ttk.Combobox(frm, textvariable=v[key], values=values, width=12).grid(row=row, column=1, sticky="w")
        if hint:
            ttk.Label(frm, text=hint, foreground="gray").grid(row=row, column=1, sticky="e")
        row += 1

    combo("Renderer", "renderer", ["gpu", "soft"], "the options below need gpu")
    combo("Internal resolution", "res_scale", [1, 2, 3, 4, 6, 8], "x PSP (4 = 1920x1088, 8 = 3840x2176)")
    combo("Effects resolution", "fx_scale", [0, 1, 2, 4], "0 = same; 1 = the original glow")
    combo("MSAA", "msaa", [1, 2, 4, 8])
    combo("Anisotropic filtering", "aniso", [1, 2, 4, 8, 16])
    combo("Sharpening (0-1)", "sharpen", [0, 0.3, 0.5, 0.8])
    combo("Frame rate (Hz)", "fps", [60, 120, 144, 165, 240, 1000], "1000 = unlimited (VRR)")
    combo("Aspect ratio", "aspect", ["native", "16:9", "21:9", "32:9"])
    combo("Window size", "window_scale", [1, 2, 3, 4])
    for text, key in (("FXAA", "fxaa"), ("HDR (when the display has it)", "hdr"), ("V-Sync", "vsync"), ("Fullscreen", "fullscreen"),
                      ("Mute", "mute")):
        ttk.Checkbutton(frm, text=text, variable=v[key]).grid(row=row, column=1, sticky="w")
        row += 1
    ttk.Label(frm, text="Texture pack folder").grid(row=row, column=0, sticky="w")
    ttk.Entry(frm, textvariable=v["texture_pack"], width=60).grid(row=row, column=1, sticky="we")
    ttk.Button(frm, text="...", width=3, command=lambda: v["texture_pack"].set(filedialog.askdirectory() or v["texture_pack"].get())).grid(
        row=row, column=2)
    row += 1
    log = tk.Text(frm, height=12, width=100)
    log.grid(row=row + 1, column=0, columnspan=3, pady=(8, 0))

    def out(line):
        root.after(0, lambda: (log.insert("end", line + "\n"), log.see("end")))

    def do_build():
        c, g = collect(), v["game"].get()
        try:
            build_steps(c, g)
        except ValueError as e:
            messagebox.showerror("Build", str(e))
            return

        def ask(text):  # called from the build thread: show the consent dialog on the Tk thread and wait
            answer, done = [False], threading.Event()
            root.after(0, lambda: (answer.__setitem__(0, messagebox.askyesno("Install build tools", text)), done.set()))
            done.wait()
            return answer[0]

        threading.Thread(target=lambda: (run_build(c, g, out, ask), root.after(0, on_game)), daemon=True).start()

    def do_play():
        try:
            start_game(collect(), v["game"].get())
            out("started: " + " ".join(play_command(cfg, v["game"].get())))
        except ValueError as e:
            messagebox.showerror("Play", str(e))

    bar = ttk.Frame(frm)
    bar.grid(row=row, column=0, columnspan=3, pady=(8, 0), sticky="w")
    ttk.Button(bar, text="Build", command=do_build).pack(side="left")
    ttk.Button(bar, text="Play", command=do_play).pack(side="left", padx=6)
    ttk.Label(frm, text=KEYS, wraplength=700, foreground="gray").grid(row=row + 2, column=0, columnspan=3, sticky="w", pady=(6, 0))
    on_game()
    root.protocol("WM_DELETE_WINDOW", lambda: (collect(), root.destroy()))
    root.mainloop()


def main(argv):
    if argv and argv[0] == "--recomp":
        return run_recomp(argv[1:])
    if argv == ["--check-tools"]:
        for tool, path in toolchain.detect().items():
            print(f"{tool:7} {path or 'MISSING'}")
        return 0 if all(toolchain.detect().values()) else 1
    if len(argv) == 2 and argv[0] in ("--build", "--play", "--print-command") and argv[1] in GAMES:
        cfg = load_config()
        if argv[0] == "--build":
            return 0 if run_build(cfg, argv[1]) else 1
        if argv[0] == "--print-command":
            print(subprocess.list2cmdline(play_command(cfg, argv[1])))
            return 0
        return start_game(cfg, argv[1]).wait()
    if argv:
        print(__doc__)
        return 2
    gui()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
