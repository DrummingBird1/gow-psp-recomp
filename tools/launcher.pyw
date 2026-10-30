"""Launcher for the native PC ports of God of War: Chains of Olympus and Ghost of Sparta.

Picks the game, builds it from your own disc image (the EBOOT is decrypted from it), sets the
graphics options of the hardware renderer (docs/phase13-graphics.md) and starts it. Settings are kept
in build/launcher.json (your paths stay on your machine; build/ is not in git).

    python tools/launcher.pyw                     the window
    python tools/launcher.pyw --build coo|gos     build without the window (recompile + CMake)
    python tools/launcher.pyw --play coo|gos      start with the saved settings
    python tools/launcher.pyw --print-command coo|gos
    python tools/launcher.pyw --check-tools       show which build tools were found

Building needs a C/C++ toolchain and CMake. By default the launcher downloads a portable one once (llvm-mingw, CMake,
Ninja; pinned SHA-256, after asking) into build/toolchain and builds with it into build/mingw_*; Visual Studio's C++
tools (build/cmake_*) stay supported and are the fallback (tools/toolchain.py: GOW_TOOLCHAIN, GOW_SIMULATE_MISSING).
The EBOOT is decrypted from the disc image into build/eboot/ by gow_disc (built once)."""
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

VERSION = "1.4.0"
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
    "window_scale": 2, "texture_pack": "", "texture_dump": False, "gpu_api": "", "gpu_power": "high", "show_fps": False, "show_input": False, "crt": False,
    "language": "en", "ui_language": "en", "music_volume": 100, "sfx_volume": 100, "skip_movies": False, "god": False, "rumble": True, "confirm_circle": False, "discord": False, "hud_edges": False, "check_updates": True, "skip_version": "", "mute": False, "game_folder": "", "preset": "", "geometry": "",
    "settings_version": 2,
}
KEYS = ("Keys: arrows = D-pad, Z = cross, X = circle, A = square, S = triangle, Q/W = L/R, Space/Enter = Start, "
        "V = Select, I/J/K/L = stick, O/U = spin the stick, C = turbo, F11 = fullscreen, F12 = screenshot, F3 = frame rate, Esc = quit. Gamepads: PSP layout. "
        "Change them in Options > Controls.")


# --- controls ---------------------------------------------------------------------------------------------------
# PSP button -> (keys, gamepad buttons), in SDL names; the runtime's defaults (runtime/rt_host.c, --controls).
CONTROLS = {
    "up": (["Up"], ["dpup"]), "down": (["Down"], ["dpdown"]), "left": (["Left"], ["dpleft"]), "right": (["Right"], ["dpright"]),
    "cross": (["Z"], ["a"]), "circle": (["X"], ["b"]), "square": (["A"], ["x"]), "triangle": (["S"], ["y"]),
    "l": (["Q"], ["leftshoulder", "lefttrigger"]), "r": (["W"], ["rightshoulder", "righttrigger"]),
    "start": (["Space", "Return"], ["start"]), "select": (["V"], ["back"]),
    "stick_up": (["I"], []), "stick_down": (["K"], []), "stick_left": (["J"], []), "stick_right": (["L"], []),
    "stick_spin_cw": (["O"], []), "stick_spin_ccw": (["U"], []), "turbo": (["C"], ["rightstick"]),
}
PAD_BUTTONS = ["a", "b", "x", "y", "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger", "back", "start",
               "dpup", "dpdown", "dpleft", "dpright", "leftstick", "rightstick"]
RESERVED_KEYS = {"Escape", "F11", "F12", "F3", "F2", "F4"}  # quit box, fullscreen, screenshot, frame rate, buttons, Alt+F4


def controls_of(cfg):
    """{button: (keys, pads)}: the defaults with the player's changes."""
    out = {b: (list(k), list(p)) for b, (k, p) in CONTROLS.items()}
    mine = {b: v for b, v in (cfg.get("controls") or {}).items() if b in out and isinstance(v, dict)}
    for b, v in mine.items():
        out[b] = ([str(x) for x in v.get("keys", [])][:4], [x for x in v.get("pad", []) if x in PAD_BUTTONS][:4])
    # a key or gamepad button the player put on a button leaves the defaults of the others: WASD for moving no
    # longer also blocks (W = R) and attacks (A, S), which made custom controls look broken (issue #3)
    taken_k = {k for b in mine for k in out[b][0]}
    taken_p = {p for b in mine for p in out[b][1]}
    for b in out:
        if b not in mine:
            out[b] = ([k for k in out[b][0] if k not in taken_k], [p for p in out[b][1] if p not in taken_p])
    return out


def controls_problems(controls):
    """Warnings about a set of bindings: a key or gamepad button on two PSP buttons, a reserved key, a button with
    nothing on it."""
    out, seen_k, seen_p = [], {}, {}
    for b, (keys, pads) in controls.items():
        if not keys and not pads:
            out.append(f"{b}: nothing assigned")
        elif not keys:
            out.append(f"{b}: no key (gamepad only)")
        for k in keys:
            if k in RESERVED_KEYS:
                out.append(f"{k} is reserved (Esc quit box, F11 fullscreen, F12 screenshot, F3 frame rate, Alt+F4)")
            elif k in seen_k:
                out.append(f"key {k} is on both {seen_k[k]} and {b}")
            seen_k.setdefault(k, b)
        for p in pads:
            if p in seen_p and not (b.startswith("stick") or seen_p[p].startswith("stick")):
                out.append(f"gamepad {p} is on both {seen_p[p]} and {b}")
            seen_p.setdefault(p, b)
    return out


def write_controls(cfg):
    """build/controls.ini for --controls when the player changed any binding, else None (the runtime's defaults)."""
    if not cfg.get("controls"):
        return None
    f = ROOT / "build" / "controls.ini"
    f.parent.mkdir(parents=True, exist_ok=True)
    lines = ["# written by the launcher (Options > Controls); see runtime/rt_host.c, --controls"]
    for b, (keys, pads) in controls_of(cfg).items():
        keys = ["Comma" if k == "," else k for k in keys]  # the file is comma-separated (settings from v1.2.0 say ",")
        lines.append(f"{b} = " + ", ".join(keys + [f"pad:{p}" for p in pads]))
    f.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return f


# --- settings ---------------------------------------------------------------------------------------------------
ALLOWED = {"renderer": ("gpu", "soft"), "res_scale": (1, 2, 3, 4, 6, 8), "fx_scale": (0, 1), "msaa": (1, 2, 4, 8),
           "aniso": (1, 2, 4, 8, 16), "fps": (60, 120, 144, 165, 240, 1000), "aspect": ("native", "21:9", "32:9", "4:3"),
           "window_scale": (1, 2, 3, 4), "gpu_api": ("", "direct3d12", "vulkan"), "gpu_power": ("high", "low"), "language": ("en", "fr", "es"), "ui_language": ("en", "es", "fr", "he"),
           "music_volume": tuple(range(101)), "sfx_volume": tuple(range(101))}
BOOLS = ("per_game", "fxaa", "hdr", "vsync", "fullscreen", "mute", "texture_dump", "reduce_motion", "check_updates", "stick_evade", "show_fps", "show_input", "crt", "skip_movies", "god", "rumble", "confirm_circle", "discord", "hud_edges")


# --- per-game graphics ------------------------------------------------------------------------------------------
# With "Separate graphics settings for each game" on, these keys are kept per game (cfg["game_settings"][game]).
GAME_KEYS = ("res_scale", "msaa", "fxaa", "aniso", "fps", "aspect", "hdr", "renderer", "fx_scale", "sharpen", "vsync",
             "crt", "show_fps", "hud_edges", "preset")


def settings_for(cfg, game):
    """The settings a game plays with: the shared ones, with that game's own graphics when they are kept apart."""
    own = (cfg.get("game_settings") or {}).get(game) if cfg.get("per_game") else None
    if not isinstance(own, dict):
        return cfg
    out = dict(cfg)
    for k in GAME_KEYS:
        if k in own:
            out[k] = own[k]
    validate_config(out)
    return out


def validate_config(cfg):
    """Puts any setting outside what the options offer (a hand-edited or damaged launcher.json) back to its default;
    returns what was changed."""
    fixed = []
    for k, allowed in ALLOWED.items():
        v = cfg.get(k, DEFAULTS.get(k))
        if isinstance(allowed[0], int):
            try:
                v = int(v)
            except (TypeError, ValueError):
                v = None
        if v not in allowed:
            fixed.append(f"{k}={cfg.get(k)!r} -> {DEFAULTS.get(k, allowed[0])!r}")
            v = DEFAULTS.get(k, allowed[0])
        cfg[k] = v
    for k in BOOLS:
        if k in cfg and not isinstance(cfg[k], bool):
            fixed.append(f"{k}={cfg[k]!r} -> {bool(cfg[k])}")
            cfg[k] = bool(cfg[k])
    try:
        s = float(cfg.get("sharpen", 0.0))
    except (TypeError, ValueError):
        s = -1
    if not 0.0 <= s <= 1.0:
        fixed.append(f"sharpen={cfg.get('sharpen')!r} -> 0.0")
        s = 0.0
    cfg["sharpen"] = s
    if not isinstance(cfg.get("disc"), dict):
        fixed.append("disc -> {}")
        cfg["disc"] = {}
    if not isinstance(cfg.get("game"), str) or cfg.get("game") not in GAMES:
        fixed.append(f"game={cfg.get('game')!r} -> 'coo'")
        cfg["game"] = "coo"
    if cfg.get("game_settings") is not None and not isinstance(cfg.get("game_settings"), dict):
        fixed.append("game_settings -> {}")
        cfg["game_settings"] = {}
    if cfg.get("controls") is not None and not isinstance(cfg.get("controls"), dict):
        fixed.append("controls -> defaults")
        cfg["controls"] = {}
    return fixed


def load_config():
    cfg = dict(DEFAULTS)
    try:
        saved = json.loads(CONFIG.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        saved = {}
    if not isinstance(saved, dict):  # a hand-edited or damaged file: start from the defaults instead of failing
        saved = {}
    version = saved.get("settings_version", 1)
    if not isinstance(version, (int, float)) or isinstance(version, bool):
        saved["settings_version"] = version = 2  # damaged, not v1.0.0's missing key: don't touch fullscreen
    if saved and version < 2:
        # v1.0.0 saved every setting, so its "fullscreen": false was the old default, not a choice: switch to the
        # new default (fullscreen) once; the Fullscreen option and F11 still switch back to a window.
        saved["fullscreen"], saved["settings_version"] = True, 2
    cfg.update(saved)
    cfg["_fixed"] = validate_config(cfg)
    return cfg


def save_config(cfg):
    CONFIG.parent.mkdir(parents=True, exist_ok=True)
    CONFIG.write_text(json.dumps({k: v for k, v in cfg.items() if not k.startswith("_")}, indent=1), encoding="utf-8")


# Build folders per toolchain: the portable one (Ninja, single-config) and Visual Studio (multi-config, Release/).
FLAVOURS = {
    "portable": {"tools": "build/mingw_tools", "coo": "build/mingw_full", "gos": "build/mingw_gos_full", "sub": ""},
    "msvc": {"tools": "build/cmake_tools", "coo": "build/cmake_full", "gos": "build/cmake_gos_full", "sub": "Release"},
}
TOOLCHAIN_DIR = ROOT / "build" / "toolchain"


def built_file(kind, key, name):
    f = FLAVOURS[kind]
    return ROOT / f[key] / f["sub"] / name


def newest(paths):
    """The most recently built of these files, or the first when none exists yet."""
    have = [p for p in paths if p.exists()]
    return max(have, key=lambda p: p.stat().st_mtime) if have else paths[0]


def runtime_stamp():
    """What a game build is built from: the launcher's bundled sources (frozen) or the runtime sources (from git).
    A game built from other sources may not know the options this launcher passes (v1.3.0 added --rumble, on by
    default: a v1.2 build stopped at once with "unknown argument"), so it must be built again first."""
    h = hashlib.sha256()
    if FROZEN:
        h.update((Path(sys._MEIPASS) / "src" / "manifest.txt").read_bytes())
    else:
        for p in sorted((ROOT / "runtime").rglob("*")) + [ROOT / "CMakeLists.txt"]:
            if p.is_file():
                h.update(p.relative_to(ROOT).as_posix().encode() + b"\0" + p.read_bytes())
    return h.hexdigest()


def stamp_file(game):
    return ROOT / "build" / f"runtime_{game}.txt"


def build_is_current(game):
    """True when this game's build was made from this launcher's runtime (False: missing or older)."""
    try:
        return stamp_file(game).read_text(encoding="utf-8").strip() == runtime_stamp()
    except OSError:
        return False


def exe_path(game):
    return newest([built_file(k, game, f"gow_rt_{GAMES[game]['id']}_full.exe") for k in FLAVOURS])


def disc_tool():
    return newest([built_file(k, "tools", "gow_disc.exe") for k in FLAVOURS])


def configure_args(tools):
    """Extra CMake configure arguments for this toolchain. Git is never used (sources are pinned archives) and is
    hidden from CMake, so a Git that happens to be installed can't mask a regression (tools/check_no_git.py)."""
    no_git = ["-DCMAKE_DISABLE_FIND_PACKAGE_Git=ON"]
    if tools.get("kind") != "portable":
        return no_git
    return no_git + ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_MAKE_PROGRAM={tools['ninja']}",
            f"-DCMAKE_C_COMPILER={tools['cc']}", f"-DCMAKE_CXX_COMPILER={tools['cxx']}"]


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
    cfg = settings_for(cfg, game)
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
        if cfg.get("gpu_api") in ("direct3d12", "vulkan"):
            cmd += ["--gpu-driver", cfg["gpu_api"]]
        if cfg.get("gpu_power") == "low":
            cmd += ["--gpu-power", "low"]
        if cfg.get("show_fps"):
            cmd.append("--show-fps")
        if cfg.get("show_input"):
            cmd.append("--show-input")
        if cfg.get("crt"):
            cmd.append("--crt")
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
            if cfg.get("hud_edges") and cfg["aspect"] in ("21:9", "32:9"):
                cmd.append("--hud-edges")
        if cfg["texture_pack"]:
            cmd += ["--texture-pack", cfg["texture_pack"]]
        if cfg.get("texture_dump"):
            cmd += ["--texture-dump", str(texture_dump_dir(game))]
    if int(cfg["fps"]) != 60:
        cmd += ["--fps", str(cfg["fps"])]
    if cfg.get("controls"):
        cmd += ["--controls", str(ROOT / "build" / "controls.ini")]
    if not cfg.get("stick_evade", True):
        cmd.append("--no-stick-evade")
    if cfg.get("language", "en") != "en":
        cmd += ["--language", cfg["language"]]
    for k, opt in (("music_volume", "--music-volume"), ("sfx_volume", "--sfx-volume")):
        if int(cfg.get(k, 100)) != 100:
            cmd += [opt, str(int(cfg[k]))]
    if cfg.get("skip_movies"):
        cmd.append("--skip-movies")
    if cfg.get("god"):
        cmd.append("--god")
    if cfg.get("rumble", True):
        cmd.append("--rumble")
    if cfg.get("confirm_circle"):
        cmd += ["--confirm", "circle"]
    return cmd


def sfo_values(data):
    """{key: value} of a PARAM.SFO (strings without their padding, integers, binary as bytes); {} when it isn't one."""
    import struct
    if len(data) < 20 or data[:4] != b"\0PSF":
        return {}
    keys_off, data_off, n = struct.unpack_from("<III", data, 8)
    out = {}
    for i in range(n):
        if 20 + i * 16 + 16 > len(data):
            break
        k_off, fmt, length, _max, d_off = struct.unpack_from("<HHIII", data, 20 + i * 16)
        end = data.find(b"\0", keys_off + k_off)
        key = data[keys_off + k_off:end].decode("ascii", "replace")
        raw = data[data_off + d_off:data_off + d_off + length]
        out[key] = (struct.unpack("<I", raw[:4])[0] if fmt == 0x404 and len(raw) >= 4 else
                    raw.rstrip(b"\0").decode("utf-8", "replace") if fmt == 0x204 else raw)
    return out


def ppsspp_saves(folder):
    """Save folders of the two games in a PPSSPP memory stick (the memstick folder, its PSP or SAVEDATA folder):
    [(game, path, encrypted)]. PPSSPP encrypts saves unless EncryptSave is off in its ppsspp.ini; the port reads plain ones."""
    d = Path(folder)
    for sub in ("PSP/SAVEDATA", "SAVEDATA", ""):
        if (d / sub).is_dir() and any((d / sub).glob("UCUS987*")):
            d = d / sub
            break
    out = []
    for game, g in GAMES.items():
        for p in sorted(d.glob(g["id"] + "*")):
            sfo = p / "PARAM.SFO"
            if not sfo.is_file():
                continue
            params = sfo_values(sfo.read_bytes()).get("SAVEDATA_PARAMS", b"")
            out.append((game, p, isinstance(params, bytes) and any(params)))
    return out


def import_ppsspp_saves(folder):
    """Copies the plain PPSSPP saves of both games into this port's memory sticks (build/ms0, build/ms0_gos), next to
    the saves already there (a folder that exists already is kept, not overwritten). Returns report lines."""
    found = ppsspp_saves(folder)
    if not found:
        return ["No saves of Chains of Olympus (UCUS98653) or Ghost of Sparta (UCUS98737) in this folder. Choose "
                "PPSSPP's memstick folder (it has PSP\\SAVEDATA in it)."]
    lines = []
    for game, src, encrypted in found:
        dst = ROOT / GAMES[game]["ms0"] / "PSP" / "SAVEDATA" / src.name
        if encrypted:
            lines.append(f"{src.name}: encrypted, not copied. Turn off PPSSPP's save encryption (with PPSSPP closed, "
                         "EncryptSave = False in memstick\\PSP\\SYSTEM\\ppsspp.ini), load this save in PPSSPP and save "
                         "again, then import it.")
        elif dst.exists():
            lines.append(f"{src.name}: already here, kept as it is.")
        else:
            shutil.copytree(src, dst)
            lines.append(f"{src.name}: imported ({GAMES[game]['title']}).")
    return lines


def ai_pack_dir(game):
    """Where "Upscale dumped textures (AI)" writes its texture pack for this game."""
    return ROOT / "build" / "texture_pack_ai" / game


def upscale_textures(game, ask, out=print):
    """Real-ESRGAN over this game's dumped textures into ai_pack_dir(game) (texture_upscale.py; the upscaler is
    downloaded once, after ask() says yes). Returns the pack folder, or None when there was nothing to upscale."""
    import texture_upscale as T
    tools = ROOT / "build" / "tools"
    if not T.find_exe(tools):
        if ask(T.consent_text()):
            if not toolchain.download_archive(T.REALESRGAN, tools, out):
                out("The download failed: the textures are scaled with Lanczos only.")
        else:
            out("Without the download the textures are scaled with Lanczos only.")
    ai, lz = T.upscale(texture_dump_dir(game), ai_pack_dir(game), tools, out=out)
    return ai_pack_dir(game) if ai + lz else None


def texture_dump_dir(game):
    """Where "Dump textures" writes each new texture of this game as <16 hex digits>.png (the pack format)."""
    return ROOT / "build" / "texture_dump" / game


def texture_pack_status(folder):
    """(ok, text) about a texture pack folder: this port loads <16 hex digits>.png files, named as "Dump textures"
    (--texture-dump) writes them; PPSSPP packs (textures.ini, PPSSPP's own hashes) don't match them."""
    import re as _re
    if not folder:
        return True, ""
    d = Path(folder)
    if not d.is_dir():
        return False, "This folder doesn't exist."
    pngs = list(d.glob("*.png"))  # the game looks in the folder itself, not in subfolders
    ours = [p for p in pngs if _re.fullmatch(r"[0-9a-fA-F]{16}\.png", p.name)]
    if ours:
        return True, f"{len(ours)} replacement textures found."
    if (d / "textures.ini").exists():
        return False, ("This is a PPSSPP texture pack (textures.ini). It can't work here: PPSSPP names textures by its "
                       "own hashes. Packs for this port are made with \"Dump textures\".")
    if pngs:
        return False, f"{len(pngs)} .png files, but none named as \"Dump textures\" writes them (16 hex digits).png."
    if any(d.rglob("*.png")):
        return False, "The .png files are in subfolders: choose the folder that holds them."
    return False, "No .png files in this folder."


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


def run_command(cmd, out=print, env=None):
    """Runs a command, sending its output lines to out; returns (exit code, lines)."""
    out("> " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
    try:
        p = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0), env=env)
    except OSError as e:  # blocked or removed by an antivirus program, damaged: a failed step, not a dead thread (#29)
        out(toolchain.ERROR + f"could not start {Path(cmd[0]).name}: {toolchain.explain(e)}")
        return 1, []
    lines = []
    for line in p.stdout:
        lines.append(line.rstrip())
        out(line.rstrip())
    return p.wait(), lines


def ensure_disc_tool(out=print, ask=toolchain.ask_console):
    """Builds gow_disc once (it reads the disc images before any game is built). Returns True when it exists."""
    if disc_tool().exists():
        return True
    tools = toolchain.ensure(ask, out, TOOLCHAIN_DIR)
    if not tools:
        return False
    unpack_sources(out)
    tools_build = ROOT / FLAVOURS[tools["kind"]]["tools"]
    for cmd in disc_tool_steps(tools):
        had_cache = is_configure(cmd) and (tools_build / "CMakeCache.txt").exists()
        code, lines = run_command(cmd, out, tools.get("env"))
        if code and had_cache:
            clean_cmake_dir(tools_build, out)
            code, lines = run_command(cmd, out, tools.get("env"))
        if code:
            out("failed to build gow_disc")
            why = first_error(lines)
            if why:
                out(toolchain.ERROR + f"building the disc reader (gow_disc) failed: {why}")
            return False
    return disc_tool().exists()


def first_error(lines):
    """The lines of a tool's output that say best what went wrong (a download, CMake, the compiler, Ninja), for the
    error box; '' when there are none."""
    for test in (lambda s: "error: downloading" in s, lambda s: s.startswith("CMake Error"),
                 lambda s: ": error:" in s or ": fatal error" in s, lambda s: s.startswith(("FAILED:", "ninja: error"))):
        for i, line in enumerate(lines):
            if test(line):
                more = [m.strip() for m in lines[i + 1:i + 4] if m.strip() and not m.lstrip().startswith(("Call Stack", "log:"))]
                return " ".join([line.strip()] + more[:2])[:400]
    return ""


def disc_tool_steps(tools):
    cmake, tools_build = tools["cmake"], ROOT / FLAVOURS[tools["kind"]]["tools"]
    return [[cmake, "-S", str(ROOT), "-B", str(tools_build), "-DGOW_TOOLS_ONLY=ON"] + configure_args(tools),
            [cmake, "--build", str(tools_build), "--config", "Release", "--target", "gow_disc", "--parallel"]]


def scan_folder(folder, out=print):
    """{game: disc image path} for the .chd/.iso files in folder (and its direct subfolders), by disc ID."""
    ids = {g["id"]: k for k, g in GAMES.items()}
    found = {}
    base = Path(folder)
    cands = [p for pat in ("*.chd", "*.iso", "*/*.chd", "*/*.iso") for p in sorted(base.glob(pat), key=lambda q: str(q).lower())]
    for f in cands:
        try:
            r = subprocess.run([str(disc_tool()), "--disc", str(f), "--id"], capture_output=True, text=True, timeout=120,
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
            subprocess.run([str(disc_tool()), "--disc", disc, "--extract", f"disc0:/PSP_GAME/{name}", str(d / name)],
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
    dt = disc_tool() if disc_tool().exists() else built_file(tools.get("kind", "msvc"), "tools", "gow_disc.exe")
    if not dt.exists():
        steps += [(cmd, None) for cmd in disc_tool_steps(tools)]
    steps.append(([str(dt), "--disc", disc, "--id", "--decrypt-eboot", str(eboot)], None))

    def needs_recomp():
        digest = hashlib.sha256(eboot.read_bytes()).hexdigest()
        return not (gen_dir(game) / "image.bin").exists() or not stamp.exists() or stamp.read_text().strip() != digest

    steps.append((recomp_command(python) + ["--elf", str(eboot),
                   "--ghidra", str(ROOT / "re/out/recomp" / f"{g['id']}_EBOOT.BIN.recomp.json"),
                   "--nids", str(ROOT / "re/out/psp_nids_ppsspp.tsv"), "--out", str(gen_dir(game)),
                   "--difftest", "0", "--boot", "0", "--all"], needs_recomp))
    build = ROOT / FLAVOURS[tools.get("kind", "msvc")][game]
    steps.append(([cmake, "-S", str(ROOT), "-B", str(build), f"-DGAME={g['id']}_full"] + configure_args(tools), None))
    steps.append(([cmake, "--build", str(build), "--config", "Release", "--parallel"], None))
    return steps, stamp


def run_build(cfg, game, out=print, ask=toolchain.ask_console):
    try:
        check_disc(cfg, game)
    except ValueError as e:
        out(str(e))
        return False
    tools = toolchain.ensure(ask, out, TOOLCHAIN_DIR)
    if not tools:
        return False
    unpack_sources(out)
    if build_with(cfg, game, tools, out):
        return True
    msvc = toolchain.detect()
    if tools.get("kind") == "portable" and all(msvc.values()) and toolchain.mode() != "portable":
        out("The portable build failed; trying again with Visual Studio's C++ tools.")
        return build_with(cfg, game, dict(msvc, kind="msvc"), out)
    return False


def build_with(cfg, game, tools, out=print):
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
        code, lines = run_command(cmd, out, tools.get("env"))
        if code and had_cache:
            clean_cmake_dir(build_dir, out)
            code, lines = run_command(cmd, out, tools.get("env"))
        if code:
            out(f"failed (exit {code})")
            why = first_error(lines)
            if why:
                out(toolchain.ERROR + why)
            return False
        if cmd[0].endswith("gow_disc.exe") and (not lines or lines[0].strip() != GAMES[game]["id"]):
            out(toolchain.ERROR + f"this disc image is {lines[0].strip() if lines else 'unknown'}, not {GAMES[game]['id']} "
                f"({GAMES[game]['title']})")
            return False
        if cond is not None:  # the recompile step
            stamp.write_text(hashlib.sha256(eboot.read_bytes()).hexdigest())
    stamp_file(game).write_text(runtime_stamp(), encoding="utf-8")
    out(f"built {exe_path(game)}")
    return True


def start_game(cfg, game):
    if not exe_path(game).exists():
        raise ValueError("Not built yet: choose the disc image and press Build.")
    if not build_is_current(game):
        raise ValueError("This launcher brings a newer version of the game's code: press Build in the launcher once "
                         "(a few minutes) before playing.")
    check_disc(cfg, game)
    (ROOT / GAMES[game]["ms0"]).mkdir(parents=True, exist_ok=True)
    (ROOT / "build" / "screenshots").mkdir(parents=True, exist_ok=True)
    # gow_rt is a console program (so developers see its log in a terminal); started from here it gets no console
    # window, and its output goes to game_log(game) instead, for bug reports.
    write_controls(cfg)
    log = open(game_log(game), "w", encoding="utf-8", errors="replace")
    # every Play also records the button presses (a few KB) and each frame's timings, so a bug report can include
    # them without a command line
    cmd = play_command(cfg, game) + ["--record-input", str(game_input_log(game)), "--perf-csv", str(game_perf_log(game))]
    proc = subprocess.Popen(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if cfg.get("discord"):  # "Playing <title>" on Discord while the game runs (off by default)
        import discord_presence
        discord_presence.show_while_running(proc, GAMES[game]["title"])
    return proc


def game_log(game):
    """What the game printed during the last Play (build/launcher_<game>.log)."""
    return ROOT / "build" / f"launcher_{game}.log"


def ran_without_gpu(game):
    """True when the last Play fell back to the software renderer: no Direct3D 12 or Vulkan device (runtime's
    software_instead, issue #27)."""
    try:
        return "the hardware renderer could not start" in game_log(game).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False


def crash_summary(game):
    """The lines of the last Play's log that say why it stopped early ([crash], GPU or SDL errors), at most 6."""
    try:
        lines = game_log(game).read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []
    keys = ("[crash]", "SDL_CreateGPUDevice", "refused", "SDL_Init", "SDL_CreateWindow", "failed", "cannot")
    return [l for l in lines if any(k in l for k in keys)][-6:]


def game_input_log(game):
    """The button presses of the last Play (build/launcher_<game>_input.txt, the --input format)."""
    return ROOT / "build" / f"launcher_{game}_input.txt"


def game_perf_log(game):
    """Each frame's timings in the last Play (build/launcher_<game>_perf.csv, --perf-csv): about 60 bytes a frame."""
    return ROOT / "build" / f"launcher_{game}_perf.csv"


PERF_ROWS = 18000  # the bug report keeps the last 5 minutes of frame timings


# --- bug reports ---------------------------------------------------------------------------------------------
REPO = "https://github.com/DrummingBird1/gow-psp-recomp"


def logs_dir():
    """The folder with the game logs and recorded input (build/), opened by "Open logs folder"."""
    return ROOT / "build"


def _ps(command):
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-Command", command], capture_output=True, text=True, timeout=30,
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        return r.stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return ""


def pc_info():
    """A few lines about this PC for a bug report: Windows, CPU, GPU and driver, memory, displays. Nothing personal."""
    import platform
    lines = [f"launcher: {VERSION}{' (exe)' if FROZEN else ' (source)'}", f"os: {platform.platform()}",
             f"python: {platform.python_version()}"]
    if os.name == "nt":
        lines.append("cpu: " + _ps("(Get-CimInstance Win32_Processor).Name -join '; '"))
        lines.append("gpu: " + _ps("(Get-CimInstance Win32_VideoController | ForEach-Object { $_.Name + ' (driver ' + "
                                   "$_.DriverVersion + ')' }) -join '; '"))
        lines.append("ram GB: " + _ps("[math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)"))
        lines.append("displays: " + _ps("(Get-CimInstance Win32_VideoController | Where-Object CurrentHorizontalResolution | "
                                        "ForEach-Object { '' + $_.CurrentHorizontalResolution + 'x' + $_.CurrentVerticalResolution + "
                                        "' @' + $_.CurrentRefreshRate + ' Hz' }) -join '; '"))
    return "\n".join(lines)


def report_settings(cfg):
    """The settings without paths: disc images by file name only, the rest of the folder layout left out."""
    import re as _re

    def strip(v):
        if isinstance(v, dict):
            return {k: strip(x) for k, x in v.items()}
        if isinstance(v, list):
            return [strip(x) for x in v]
        if isinstance(v, str) and _re.match(r"^([A-Za-z]:[\\/]|\\\\|/)", v):
            return Path(v).name  # an absolute path: only the file name
        return v
    out = strip({k: v for k, v in cfg.items() if not k.startswith("_")})
    for k in ("game_folder", "texture_pack"):
        if cfg.get(k):
            out[k] = "(set)"
    out.pop("geometry", None)
    return out


def redact_paths(text, cfg=None):
    """Text for a bug report without personal paths: the player's own folders become placeholders (<game folder>,
    <data folder>, <user folder>...), any other absolute path keeps only its file name, and the Windows user name
    becomes <user>. The logs name the disc image, the input recording and so on by full path."""
    import re as _re
    cfg = cfg or {}
    known = [(cfg.get("game_folder"), "<game folder>"), (cfg.get("texture_pack"), "<texture pack>"), (str(ROOT), "<data folder>"),
             (str(Path(sys.executable).resolve().parent) if FROZEN else "", "<launcher folder>"),
             (os.environ.get("USERPROFILE") or str(Path.home()), "<user folder>")]
    known += [(str(Path(p).parent), "<disc folder>") for p in (cfg.get("disc") or {}).values() if p]
    for p, name in sorted(((str(Path(p)), n) for p, n in known if p), key=lambda x: -len(x[0])):
        for v in {p, p.replace("\\", "/")}:
            text = _re.sub(_re.escape(v), name, text, flags=_re.I)
    text = _re.sub(r"(?i)(?<![A-Za-z])([A-Za-z]:[\\/]Users[\\/])[^\\/\r\n\"'<>|*?:]+", r"\1<user>", text)
    text = _re.sub(r"(?<![A-Za-z])[A-Za-z]:[\\/](?:[^\\/\r\n\"'|*?:]+[\\/])+", "<...>/", text)  # other folders
    user = os.environ.get("USERNAME", "")
    if len(user) >= 3:
        text = _re.sub(r"(?i)(?<![A-Za-z0-9])" + _re.escape(user) + r"(?![A-Za-z0-9])", "<user>", text)
    return text


def make_bug_report(cfg, game, launcher_log=()):
    """Zips what a bug report needs into build/bug_reports/: the last Play's log and recorded input, the settings
    (no paths), PC info and the launcher's log, all with paths and the user name taken out (redact_paths). Never
    game files: no disc image, EBOOT, generated C, saves or screenshots. Returns the zip's path."""
    import time
    import zipfile
    d = ROOT / "build" / "bug_reports"
    d.mkdir(parents=True, exist_ok=True)
    z = d / f"gow_bug_{game}_{time.strftime('%Y%m%d_%H%M%S')}.zip"
    with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as f:
        for p in (game_log(game), game_input_log(game)):
            if p.exists():
                f.writestr(p.name, redact_paths(p.read_text(encoding="utf-8", errors="replace"), cfg))
        perf = game_perf_log(game)
        if perf.exists():  # numbers only (frame, times, primitive counts): the header and the last PERF_ROWS rows
            rows = perf.read_text(encoding="utf-8", errors="replace").splitlines()
            f.writestr(perf.name, "\n".join(rows[:1] + rows[1:][-PERF_ROWS:]) + "\n")
        f.writestr("settings.json", json.dumps(report_settings(cfg), indent=1))
        f.writestr("pc_info.txt", pc_info() + "\n")
        if launcher_log:
            f.writestr("launcher_log.txt", redact_paths("\n".join(launcher_log) + "\n", cfg))
        f.writestr("README.txt", "God of War PSP native port: bug report. The game's log, recorded button presses and frame "
                   "timings of the last Play, the launcher settings, PC info. Paths keep only file names (your folders show as "
                   "<game folder>, <user folder>...), and the Windows user name shows as <user>. No game files.\n")
    return z


def new_issue_url(game, info=""):
    from urllib.parse import urlencode
    q = {"template": "bug_report.yml", "game": {"coo": "Chains of Olympus", "gos": "Ghost of Sparta"}.get(game, "Launcher only"),
         "version": f"v{VERSION}", "settings": info[:1500]}
    return f"{REPO}/issues/new?{urlencode(q)}"


# --- shortcuts: desktop and Steam ------------------------------------------------------------------------------
def shortcut_target(game):
    """(program, arguments, start folder) that start this game directly, without the launcher window: the launcher
    exe (or pythonw + launcher.pyw from source) with --play <game>."""
    if FROZEN:
        exe = Path(sys.executable).resolve()
        return str(exe), f"--play {game}", str(exe.parent)
    py = Path(sys.executable)
    pyw = py.with_name("pythonw.exe") if py.with_name("pythonw.exe").exists() else py
    return str(pyw), f'"{Path(__file__).resolve()}" --play {game}', str(ROOT)


def game_icon(game):
    """An .ico of the game's own icon (ICON0.PNG off the player's disc) for shortcuts, else the launcher's."""
    png, ico = art_dir(game) / "ICON0.PNG", art_dir(game) / "icon.ico"
    if png.exists() and not ico.exists():
        try:
            from PIL import Image
            img = Image.open(png).convert("RGBA")
            side = max(img.size)
            sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
            sq.paste(img, ((side - img.width) // 2, (side - img.height) // 2))
            sq.save(ico, sizes=[(256, 256), (64, 64), (48, 48), (32, 32), (16, 16)])
        except (OSError, ImportError, ValueError):
            pass
    if ico.exists():
        return str(ico)
    return str(Path(sys.executable).resolve()) if FROZEN else str(Path(__file__).resolve().parent / "launcher_assets" / "icon.ico")


def shortcut_name(game):
    return GAMES[game]["title"].replace(":", " -")


def create_desktop_shortcut(game):
    """A desktop shortcut that starts the game directly. Returns its path."""
    exe, args, start = shortcut_target(game)
    ps = ("$d = [Environment]::GetFolderPath('Desktop'); $l = Join-Path $d $env:GOW_LNK; "
          "$s = (New-Object -ComObject WScript.Shell).CreateShortcut($l); $s.TargetPath = $env:GOW_EXE; "
          "$s.Arguments = $env:GOW_ARGS; $s.WorkingDirectory = $env:GOW_START; $s.IconLocation = $env:GOW_ICON; "
          "$s.Description = $env:GOW_DESC; $s.WindowStyle = 7; $s.Save(); $l")
    env = dict(os.environ, GOW_LNK=shortcut_name(game) + ".lnk", GOW_EXE=exe, GOW_ARGS=args, GOW_START=start,
               GOW_ICON=game_icon(game), GOW_DESC=f"Play {GAMES[game]['title']} (native PC port)")
    r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=60, env=env,
                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    path = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
    if r.returncode or not path or not Path(path).exists():
        raise OSError(r.stderr.strip() or "the shortcut could not be created")
    return path


def add_to_steam(game):
    """Adds the game to Steam as a non-Steam game (see steam_shortcuts.py). Returns (vdf, backup, state)."""
    import steam_shortcuts
    exe, args, start = shortcut_target(game)
    return steam_shortcuts.add_shortcut(shortcut_name(game), exe, args, start, game_icon(game))


def cli_streams():
    """The exe is a windowed program (no console window when started from Explorer, a shortcut or Steam), so its
    command-line modes have no stdout. Started from a terminal, print there (attach to its console); with the output
    redirected, the pipe or file already works; otherwise write to GoW_PSP_data/build/launcher_cli.log."""
    if not (FROZEN and os.name == "nt") or sys.stdout is not None:
        return
    import ctypes
    if ctypes.windll.kernel32.AttachConsole(-1):  # ATTACH_PARENT_PROCESS
        sys.stdout = open("CONOUT$", "w", encoding="utf-8", errors="replace", buffering=1)
        sys.stderr = sys.stdout
        sys.stdin = open("CONIN$", "r", encoding="utf-8", errors="replace")
        print()  # the shell already printed its prompt: start on a new line
        return
    log = ROOT / "build" / "launcher_cli.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    sys.stdout = sys.stderr = open(log, "a", encoding="utf-8", errors="replace", buffering=1)


def show_error(title, text):
    """A message box when there is no launcher window (--play from a shortcut or Steam), else stderr."""
    if FROZEN and os.name == "nt":
        import ctypes
        ctypes.windll.user32.MessageBoxW(None, text, title, 0x10)
    else:
        print(f"{title}: {text}", file=sys.stderr)


# --- self-update (launcher_update.py) ---------------------------------------------------------------------------
def update_dir():
    return ROOT / "build" / "update"


def run_update(out=print):
    """--update: check, download, verify, swap the exe, restart it (frozen only). Returns an exit code."""
    import launcher_update as U
    try:
        info = U.check(VERSION)
        if not info:
            out(f"up to date (v{VERSION})")
            return 0
        out(f"v{info['version']} is available (this is v{VERSION})")
        if not FROZEN:
            out(f"running from source: update with git, or download {info['page']}")
            return 0
        new = U.download(info, update_dir(), lambda d, t: None)
        out(f"downloaded and verified {info['zip_name']}")
        U.apply(new, sys.executable, ["--version"])
        out("updated; the new launcher starts")
        return 0
    except U.UpdateError as e:
        out(f"update failed: {e}")
        return 1


def gui():
    import launcher_qt
    launcher_qt.run(sys.modules[__name__])


def main(argv):
    if argv:
        cli_streams()
    if argv and argv[0] == "--recomp":
        return run_recomp(argv[1:])
    if len(argv) >= 2 and argv[0] == "--updated":  # started by a self-update: remove the old exe, then go on
        import launcher_update
        launcher_update.cleanup(argv[1])
        argv = argv[2:]
    if argv == ["--version"]:
        print(VERSION)
        return 0
    if argv == ["--update"]:
        return run_update()
    if argv == ["--check-update"]:
        import launcher_update
        try:
            info = launcher_update.check(VERSION)
        except launcher_update.UpdateError as e:
            print(f"check failed: {e}")
            return 1
        print(f"v{info['version']} available: {info['page']}" if info else f"up to date (v{VERSION})")
        return 0
    if argv == ["--check-tools"]:
        msvc, portable = toolchain.detect(), toolchain.find_portable(TOOLCHAIN_DIR)
        print(f"toolchain mode: {toolchain.mode()}")
        print(f"portable {'ready in ' + str(TOOLCHAIN_DIR) if portable else 'not downloaded (' + str(toolchain.PORTABLE_MB) + ' MB, on first Build)'}")
        for tool, path in msvc.items():
            print(f"{tool:7} {path or 'MISSING'}")
        return 0 if portable or all(msvc.values()) else 1
    if len(argv) == 2 and argv[0] == "--render-menu" and argv[1] in GAMES:
        return 0 if render_menu(load_config(), argv[1]) else 1
    if len(argv) == 2 and argv[0] in ("--build", "--play", "--print-command") and argv[1] in GAMES:
        cfg = load_config()
        if argv[0] == "--build":
            return 0 if run_build(cfg, argv[1]) else 1
        if argv[0] == "--print-command":
            print(subprocess.list2cmdline(play_command(cfg, argv[1])))
            return 0
        try:
            return start_game(cfg, argv[1]).wait()
        except ValueError as e:  # started from a shortcut or Steam: say why instead of doing nothing
            show_error(GAMES[argv[1]]["title"], f"{e}\n\nOpen the launcher to set it up.")
            return 1
    if argv:
        print(__doc__)
        return 2
    gui()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
