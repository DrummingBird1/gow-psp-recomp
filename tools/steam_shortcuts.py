"""Adds a game to Steam as a non-Steam game (Steam's userdata/<account>/config/shortcuts.vdf, binary KeyValues).

Steam reads shortcuts.vdf when it starts and writes it back when it exits, so the file is only changed while Steam
is closed; a copy of the old file is kept next to it (shortcuts.vdf.gowpsp-<time>.bak). An entry with the same name
is updated in place, so adding a game twice doesn't duplicate it. GOW_STEAM_DIR points at another Steam folder (tests).
"""
import os
import re
import shutil
import struct
import subprocess
import time
import zlib
from pathlib import Path

MAP, STR, INT, END = 0, 1, 2, 8
STEAMID64_BASE = 76561197960265728


# --- binary KeyValues ------------------------------------------------------------------------------------------
def parse(data):
    """bytes -> dict (nested maps; strings; ints). Keys keep their order."""
    def cstr(i):
        j = data.index(b"\0", i)
        return data[i:j].decode("utf-8", "replace"), j + 1

    def read_map(i):
        out = {}
        while True:
            t = data[i]
            i += 1
            if t == END:
                return out, i
            key, i = cstr(i)
            if t == MAP:
                out[key], i = read_map(i)
            elif t == STR:
                out[key], i = cstr(i)
            elif t == INT:
                out[key] = struct.unpack_from("<i", data, i)[0]
                i += 4
            else:
                raise ValueError(f"shortcuts.vdf: unknown type {t} at {i - 1}")
    root, i = read_map(0)
    return root


def dump(root):
    def w_map(d):
        b = bytearray()
        for k, v in d.items():
            kb = str(k).encode("utf-8") + b"\0"
            if isinstance(v, dict):
                b += bytes([MAP]) + kb + w_map(v)
            elif isinstance(v, int):
                b += bytes([INT]) + kb + struct.pack("<i", v)
            else:
                b += bytes([STR]) + kb + str(v).encode("utf-8") + b"\0"
        return bytes(b) + bytes([END])
    return w_map(root)


# --- Steam folders ---------------------------------------------------------------------------------------------
def steam_dir():
    env = os.environ.get("GOW_STEAM_DIR")
    if env:
        return Path(env)
    if os.name != "nt":
        return None
    import winreg
    for hive, key, name in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
                            (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath")):
        try:
            with winreg.OpenKey(hive, key) as k:
                p = Path(winreg.QueryValueEx(k, name)[0])
                if p.is_dir():
                    return p
        except OSError:
            pass
    return None


def account_id(steam):
    """The Steam account to add to: the one signed in most recently (config/loginusers.vdf), else the only one."""
    users = steam / "userdata"
    ids = [d.name for d in users.iterdir() if d.is_dir() and d.name.isdigit() and d.name != "0"] if users.is_dir() else []
    try:
        text = (steam / "config" / "loginusers.vdf").read_text(encoding="utf-8", errors="replace")
        best, best_ts = None, -1
        for m in re.finditer(r'"(\d{17})"\s*\{([^}]*)\}', text):
            acc = str(int(m.group(1)) - STEAMID64_BASE)
            if ids and acc not in ids:
                continue
            if re.search(r'"MostRecent"\s*"1"', m.group(2), re.I):
                return acc
            ts = re.search(r'"Timestamp"\s*"(\d+)"', m.group(2), re.I)  # newer Steam: the last sign-in time
            if ts and int(ts.group(1)) > best_ts:
                best, best_ts = acc, int(ts.group(1))
        if best:
            return best
    except OSError:
        pass
    return ids[0] if len(ids) == 1 else None


def steam_running():
    if os.environ.get("GOW_STEAM_DIR"):
        return os.environ.get("GOW_STEAM_RUNNING") == "1"
    if os.name != "nt":
        return False
    try:
        r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq steam.exe", "/NH"], capture_output=True, text=True, timeout=15,
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        return "steam.exe" in r.stdout.lower()
    except (OSError, subprocess.TimeoutExpired):
        return False


def shortcut_appid(exe, name):
    """Steam's id for a non-Steam shortcut: crc32(exe + name) with the top bit set (as a signed 32-bit value)."""
    v = zlib.crc32((exe + name).encode("utf-8")) | 0x80000000
    return struct.unpack("<i", struct.pack("<I", v))[0]


class SteamError(Exception):
    pass


def add_shortcut(name, exe, args, start_dir, icon=""):
    """Adds (or updates) a non-Steam game. Returns (shortcuts.vdf path, backup path or None, 'added'|'updated').
    Raises SteamError with a message for the player."""
    steam = steam_dir()
    if not steam:
        raise SteamError("Steam isn't installed (or its folder wasn't found).")
    if steam_running():
        raise SteamError("Steam is running. Close it first (Steam menu > Exit), then try again: Steam only reads its list "
                         "of non-Steam games when it starts and would overwrite the change when it exits.")
    acc = account_id(steam)
    if not acc:
        raise SteamError("Couldn't tell which Steam account to use: sign in to Steam once, exit it, and try again.")
    cfg = steam / "userdata" / acc / "config"
    cfg.mkdir(parents=True, exist_ok=True)
    f = cfg / "shortcuts.vdf"
    backup = None
    if f.exists():
        data = f.read_bytes()
        root = parse(data) if data else {"shortcuts": {}}
        backup = f.with_name(f"shortcuts.vdf.gowpsp-{time.strftime('%Y%m%d_%H%M%S')}.bak")
        shutil.copyfile(f, backup)
    else:
        root = {"shortcuts": {}}
    sc = root.setdefault("shortcuts", {})
    q = lambda s: f'"{s}"'
    entry = {"appid": shortcut_appid(q(exe), name), "AppName": name, "Exe": q(exe), "StartDir": q(start_dir), "icon": icon,
             "ShortcutPath": "", "LaunchOptions": args, "IsHidden": 0, "AllowDesktopConfig": 1, "AllowOverlay": 1,
             "OpenVR": 0, "Devkit": 0, "DevkitGameID": "", "DevkitOverrideAppID": 0, "LastPlayTime": 0,
             "FlatpakAppID": "", "tags": {}}
    key = next((k for k, v in sc.items() if isinstance(v, dict) and (v.get("AppName") or v.get("appname")) == name), None)
    state = "updated" if key is not None else "added"
    if key is None:
        key = str(max([int(k) for k in sc if str(k).isdigit()] or [-1]) + 1)
    else:
        entry["LastPlayTime"] = sc[key].get("LastPlayTime", 0)
        entry["tags"] = sc[key].get("tags", {})
    sc[key] = entry
    tmp = f.with_suffix(".vdf.tmp")
    tmp.write_bytes(dump(root))
    os.replace(tmp, f)
    return f, backup, state
