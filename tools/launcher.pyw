"""Launcher for the native PC ports of God of War: Chains of Olympus and Ghost of Sparta.

Picks the game, builds it from your own copy (the decrypted EBOOT.BIN, dumped with PPSSPP), sets the
graphics options of the hardware renderer (docs/phase13-graphics.md) and starts it. Settings are kept
in build/launcher.json (your paths stay on your machine; build/ is not in git).

    python tools/launcher.pyw                     the window
    python tools/launcher.pyw --build coo|gos     build without the window (recompile + CMake)
    python tools/launcher.pyw --play coo|gos      start with the saved settings
    python tools/launcher.pyw --print-command coo|gos

Building needs Python 3, CMake and Visual Studio with C++ (see README.md)."""
import hashlib
import json
import os
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONFIG = ROOT / "build" / "launcher.json"
GAMES = {
    "coo": {"id": "UCUS98653", "title": "God of War: Chains of Olympus", "build": "build/cmake_full", "ms0": "build/ms0"},
    "gos": {"id": "UCUS98737", "title": "God of War: Ghost of Sparta", "build": "build/cmake_gos_full", "ms0": "build/ms0_gos"},
}
DEFAULTS = {
    "game": "coo", "disc": {}, "eboot": {}, "renderer": "gpu", "res_scale": 2, "fx_scale": 0, "msaa": 1, "aniso": 1,
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


def build_steps(cfg, game):
    """Recompile the EBOOT into C (skipped when this EBOOT was already recompiled), configure, build."""
    g, eboot = GAMES[game], cfg["eboot"].get(game, "")
    if not eboot or not Path(eboot).is_file():
        raise ValueError("Choose the decrypted EBOOT.BIN of this game first (dump it with PPSSPP, see README.md).")
    digest = hashlib.sha256(Path(eboot).read_bytes()).hexdigest()
    stamp = gen_dir(game) / "eboot.sha256"
    steps = []
    if not (gen_dir(game) / "image.bin").exists() or not stamp.exists() or stamp.read_text().strip() != digest:
        steps.append([sys.executable, "-I", str(ROOT / "recomp" / "recomp.py"), "--elf", eboot,
                      "--ghidra", str(ROOT / "re/out/recomp" / f"{g['id']}_EBOOT.BIN.recomp.json"),
                      "--nids", str(ROOT / "re/out/psp_nids_ppsspp.tsv"), "--out", str(gen_dir(game)),
                      "--difftest", "0", "--boot", "0", "--all"])
    steps.append(["cmake", "-S", str(ROOT), "-B", str(ROOT / g["build"]), f"-DGAME={g['id']}_full"])
    steps.append(["cmake", "--build", str(ROOT / g["build"]), "--config", "Release", "--parallel"])
    return steps, stamp, digest


def run_build(cfg, game, out=print):
    steps, stamp, digest = build_steps(cfg, game)
    for cmd in steps:
        out("> " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
        p = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        for line in p.stdout:
            out(line.rstrip())
        if p.wait():
            out(f"failed (exit {p.returncode})")
            return False
        if "recomp.py" in cmd[2]:
            stamp.write_text(digest)
    out(f"built {exe_path(game)}")
    return True


def start_game(cfg, game):
    if not exe_path(game).exists():
        raise ValueError("Not built yet: choose the EBOOT and press Build.")
    if not cfg["disc"].get(game) or not Path(cfg["disc"][game]).is_file():
        raise ValueError("Choose the disc image (.chd or .iso) of this game.")
    (ROOT / GAMES[game]["ms0"]).mkdir(parents=True, exist_ok=True)
    (ROOT / "build" / "screenshots").mkdir(parents=True, exist_ok=True)
    log = open(ROOT / "build" / f"launcher_{game}.log", "w", encoding="utf-8", errors="replace")
    return subprocess.Popen(play_command(cfg, game), cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)


def gui():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    cfg = load_config()
    root = tk.Tk()
    root.title("God of War PSP native ports: launcher")
    v = {k: (tk.BooleanVar if isinstance(DEFAULTS[k], bool) else tk.StringVar)(value=cfg[k]) for k in
         ("game", "renderer", "res_scale", "fx_scale", "msaa", "aniso", "fxaa", "sharpen", "hdr", "fps", "vsync", "aspect",
          "fullscreen", "window_scale", "texture_pack", "mute")}
    disc, eboot, status = tk.StringVar(), tk.StringVar(), tk.StringVar()

    def collect():
        for k, var in v.items():
            val = var.get()
            cfg[k] = val if isinstance(DEFAULTS[k], (bool, str)) else (float(val) if isinstance(DEFAULTS[k], float) else int(float(val)))
        cfg["disc"][cfg["game"]] = disc.get()
        cfg["eboot"][cfg["game"]] = eboot.get()
        save_config(cfg)
        return cfg

    def on_game(*_):
        g = v["game"].get()
        disc.set(cfg["disc"].get(g, ""))
        eboot.set(cfg["eboot"].get(g, ""))
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
        cfg["eboot"][prev["game"]] = eboot.get()
        prev["game"] = v["game"].get()

    def path_row(label, var, types):
        nonlocal row
        ttk.Label(frm, text=label).grid(row=row, column=0, sticky="w")
        ttk.Entry(frm, textvariable=var, width=60).grid(row=row, column=1, sticky="we")
        ttk.Button(frm, text="...", width=3, command=lambda: var.set(filedialog.askopenfilename(filetypes=types) or var.get())).grid(
            row=row, column=2)
        row += 1

    path_row("Disc image", disc, [("PSP disc", "*.chd *.iso"), ("All", "*.*")])
    path_row("Decrypted EBOOT.BIN", eboot, [("EBOOT", "*.BIN"), ("All", "*.*")])
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
        threading.Thread(target=lambda: (run_build(c, g, out), root.after(0, on_game)), daemon=True).start()

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
