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

VERSION = "1.0.1"
# Frozen (GoWLauncher.exe, tools/build_launcher.py): the exe carries the sources the build needs and unpacks them
# on the first Build. Everything it writes (sources, build/, saves, settings) goes into one folder next to the exe,
# GoW_PSP_data/, so a Downloads or Desktop folder only gains that one folder.
FROZEN = getattr(sys, "frozen", False)
DATA_DIR = "GoW_PSP_data"


def data_root():
    exe_dir = Path(sys.executable).resolve().parent
    if (exe_dir / "build" / "launcher.json").exists():
        return exe_dir  # set up by v1.0.0, which kept its files next to the exe: keep the builds and saves there
    return exe_dir / DATA_DIR


ROOT = data_root() if FROZEN else Path(__file__).resolve().parent.parent
CONFIG = Path(os.environ.get("GOW_LAUNCHER_CONFIG") or ROOT / "build" / "launcher.json")
GAMES = {
    "coo": {"id": "UCUS98653", "title": "God of War: Chains of Olympus", "build": "build/cmake_full", "ms0": "build/ms0"},
    "gos": {"id": "UCUS98737", "title": "God of War: Ghost of Sparta", "build": "build/cmake_gos_full", "ms0": "build/ms0_gos"},
}
DEFAULTS = {
    "game": "coo", "disc": {}, "renderer": "gpu", "res_scale": 2, "fx_scale": 0, "msaa": 1, "aniso": 1,
    "fxaa": False, "sharpen": 0.0, "hdr": False, "fps": 60, "vsync": False, "aspect": "native", "fullscreen": True,
    "window_scale": 2, "texture_pack": "", "mute": False, "game_folder": "", "preset": "", "geometry": "",
    "settings_version": 2,
}
KEYS = ("Keys: arrows = D-pad, Z = cross, X = circle, A = square, S = triangle, Q/W = L/R, Space/Enter = Start, "
        "V = Select, I/J/K/L = stick, F11 = fullscreen, F12 = screenshot, Esc = quit. Gamepads: PSP layout.")


def load_config():
    cfg = dict(DEFAULTS)
    try:
        saved = json.loads(CONFIG.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        saved = {}
    if saved and saved.get("settings_version", 1) < 2:
        # v1.0.0 saved every setting, so its "fullscreen": false was the old default, not a choice: switch to the
        # new default (fullscreen) once; the Fullscreen option and F11 still switch back to a window.
        saved["fullscreen"], saved["settings_version"] = True, 2
    cfg.update(saved)
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
    icon = art_dir(game) / "ICON0.PNG"  # the game's own icon, off the player's disc (extract_art); else the exe's
    if icon.exists():
        cmd += ["--icon", str(icon)]
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


def is_configure(cmd):
    return len(cmd) > 2 and cmd[1] == "-S" and "-B" in cmd


def clean_cmake_dir(build_dir, out=print):
    """Removes CMake's cache and probe results from a build folder (the sources and fetched dependencies stay).
    A configure that was interrupted, or ran twice at once in the same folder, can leave a cache that fails
    every later configure (e.g. "No CMAKE_C_COMPILER could be found" although the compiler is there)."""
    d = Path(build_dir)
    out(f"configure failed with an existing CMake cache in {d}: removing CMakeCache.txt and CMakeFiles, retrying once")
    (d / "CMakeCache.txt").unlink(missing_ok=True)
    shutil.rmtree(d / "CMakeFiles", ignore_errors=True)


def run_command(cmd, out=print):
    """Runs a command, sending its output lines to out; returns (exit code, lines)."""
    out("> " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
    p = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                         creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    lines = []
    for line in p.stdout:
        lines.append(line.rstrip())
        out(line.rstrip())
    return p.wait(), lines


def ensure_disc_tool(out=print, ask=toolchain.ask_console):
    """Builds gow_disc once (it reads the disc images before any game is built). Returns True when it exists."""
    if DISC_TOOL.exists():
        return True
    tools = toolchain.ensure(ask, out)
    if not tools:
        return False
    unpack_sources(out)
    cmake = tools["cmake"]
    for cmd in ([cmake, "-S", str(ROOT), "-B", str(TOOLS_BUILD), "-DGOW_TOOLS_ONLY=ON"],
                [cmake, "--build", str(TOOLS_BUILD), "--config", "Release", "--target", "gow_disc", "--parallel"]):
        had_cache = is_configure(cmd) and (TOOLS_BUILD / "CMakeCache.txt").exists()
        code = run_command(cmd, out)[0]
        if code and had_cache:
            clean_cmake_dir(TOOLS_BUILD, out)
            code = run_command(cmd, out)[0]
        if code:
            out("failed to build gow_disc")
            return False
    return DISC_TOOL.exists()


def scan_folder(folder, out=print):
    """{game: disc image path} for the .chd/.iso files in folder (and its direct subfolders), by disc ID."""
    ids = {g["id"]: k for k, g in GAMES.items()}
    found = {}
    base = Path(folder)
    cands = [p for pat in ("*.chd", "*.iso", "*/*.chd", "*/*.iso") for p in sorted(base.glob(pat), key=lambda q: str(q).lower())]
    for f in cands:
        try:
            r = subprocess.run([str(DISC_TOOL), "--disc", str(f), "--id"], capture_output=True, text=True, timeout=120,
                               creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        except (OSError, subprocess.TimeoutExpired):
            continue
        disc_id = r.stdout.strip().splitlines()[0] if r.returncode == 0 and r.stdout.strip() else ""
        out(f"{f.name}: {disc_id or 'not a PSP disc'}")
        if disc_id in ids and ids[disc_id] not in found:
            found[ids[disc_id]] = str(f)
    return found


def art_dir(game):
    return ROOT / "build" / "launcher" / game


def extract_art(game, disc):
    """Copies PSP_GAME/PIC1.PNG and ICON0.PNG off the player's disc into build/launcher/<game>/ (never committed)."""
    d = art_dir(game)
    d.mkdir(parents=True, exist_ok=True)
    for name in ("PIC1.PNG", "ICON0.PNG"):
        if not (d / name).exists():
            subprocess.run([str(DISC_TOOL), "--disc", disc, "--extract", f"disc0:/PSP_GAME/{name}", str(d / name)],
                           capture_output=True, timeout=120, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    return {n: d / n for n in ("PIC1.PNG", "ICON0.PNG") if (d / n).exists()}


MENU_FRAMES = {"coo": 1200, "gos": 1500}  # the main menu, idle, after a boot with an empty ms0
MENU_LOOP = 48  # frames rendered for the background loop, every 4th


def menu_dir(game):
    return art_dir(game) / "menu"


def menu_frames(game):
    return sorted(menu_dir(game).glob("*.png"))


def render_menu(cfg, game, out=print):
    """Renders the game's own main menu headless at 4x into build/launcher/<game>/menu/ (never committed): the
    launcher's live background. Needs the built game and its disc image."""
    if not exe_path(game).exists():
        out("render_menu: build the game first")
        return False
    disc = check_disc(cfg, game)
    d = menu_dir(game)
    ms0 = art_dir(game) / "menu_ms0"
    shutil.rmtree(ms0, ignore_errors=True)
    ms0.mkdir(parents=True)
    d.mkdir(parents=True, exist_ok=True)
    start = MENU_FRAMES[game]
    at = [start + 4 * i for i in range(MENU_LOOP)]
    cmd = [str(exe_path(game)), "--image", str(gen_dir(game) / "image.bin"), "--boot", "--disc", disc, "--ms0", str(ms0),
           "--renderer", "gpu", "--res-scale", "4", "--mute", "--frames-dir", str(d), "--frames-at", ",".join(map(str, at)),
           "--max-frames", str(at[-1]), "--cpu-ms", "900000", "--wall-ms", "900000"]
    code, _ = run_command(cmd, lambda line: None)
    shutil.rmtree(ms0, ignore_errors=True)
    ok = code == 0 and len(menu_frames(game)) > 0
    out(f"render_menu {game}: {'ok' if ok else 'failed'} ({len(menu_frames(game))} frames)")
    return ok


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
        build_dir = Path(cmd[cmd.index("-B") + 1]) if is_configure(cmd) else None
        had_cache = build_dir is not None and (build_dir / "CMakeCache.txt").exists()
        code, lines = run_command(cmd, out)
        if code and had_cache:
            clean_cmake_dir(build_dir, out)
            code, lines = run_command(cmd, out)
        if code:
            out(f"failed (exit {code})")
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
    # gow_rt is a console program (so developers see its log in a terminal); started from here it gets no console
    # window, and its output goes to game_log(game) instead, for bug reports.
    log = open(game_log(game), "w", encoding="utf-8", errors="replace")
    return subprocess.Popen(play_command(cfg, game), cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))


def game_log(game):
    """What the game printed during the last Play (build/launcher_<game>.log)."""
    return ROOT / "build" / f"launcher_{game}.log"


def gui():
    import launcher_qt
    launcher_qt.run(sys.modules[__name__])


def main(argv):
    if argv and argv[0] == "--recomp":
        return run_recomp(argv[1:])
    if argv == ["--check-tools"]:
        for tool, path in toolchain.detect().items():
            print(f"{tool:7} {path or 'MISSING'}")
        return 0 if all(toolchain.detect().values()) else 1
    if len(argv) == 2 and argv[0] == "--render-menu" and argv[1] in GAMES:
        return 0 if render_menu(load_config(), argv[1]) else 1
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
