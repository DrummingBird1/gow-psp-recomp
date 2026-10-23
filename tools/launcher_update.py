"""Launcher self-update: checks GitHub's latest release, downloads its ZIP, verifies it against the release's
SHA256SUMS.txt and swaps the exe in place.

A running exe can't be overwritten on Windows but it can be renamed: the update renames GoWLauncher.exe to
GoWLauncher.old.exe, moves the new exe into its place, starts it with --updated <old exe> and exits; the new
launcher deletes the old exe. GoW_PSP_data (builds, saves, settings) next to the exe is never touched.
If any step fails the old exe is put back. GOW_UPDATE_API points at another releases/latest JSON (tests): https, or
http on this PC only, as are the download links.

What the check proves: SHA256SUMS.txt comes from the same GitHub release as the ZIP, so a matching hash means the
download arrived whole, not that the release itself is genuine (the exe is not signed).
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile
from pathlib import Path

API = "https://api.github.com/repos/DrummingBird1/gow-psp-recomp/releases/latest"
UA = {"User-Agent": "GoWLauncher-update", "Accept": "application/vnd.github+json"}


class UpdateError(Exception):
    pass


def version_tuple(v):
    return tuple(int(x) for x in re.findall(r"\d+", v)[:3]) or (0,)


def _allowed(url):
    """https anywhere, plain http only to this PC (the tests' local server)."""
    return url.startswith("https://") or re.match(r"http://(127\.0\.0\.1|localhost)(:\d+)?/", url) is not None


def _get(url, timeout=10):
    if not _allowed(url):
        raise UpdateError(f"not an https link: {url}")
    req = urllib.request.Request(url, headers=UA)
    return urllib.request.urlopen(req, timeout=timeout)


def check(current, timeout=6):
    """None when up to date, else {"version", "notes", "zip_url", "zip_name", "sums_url", "page"}. Raises
    UpdateError when offline or the release has no ZIP or SHA256SUMS.txt."""
    try:
        with _get(os.environ.get("GOW_UPDATE_API") or API, timeout) as r:
            rel = json.load(r)
    except (OSError, ValueError) as e:
        raise UpdateError(f"couldn't reach GitHub ({e})") from e
    tag = rel.get("tag_name", "")
    if rel.get("draft") or rel.get("prerelease") or version_tuple(tag) <= version_tuple(current):
        return None
    assets = {a["name"]: a["browser_download_url"] for a in rel.get("assets", [])}
    zips = [n for n in assets if re.fullmatch(r"GoW_PSP_Launcher-v[\d.]+\.zip", n)]
    if not zips or "SHA256SUMS.txt" not in assets:
        raise UpdateError(f"release {tag} has no launcher ZIP or SHA256SUMS.txt")
    return {"version": tag.lstrip("v"), "notes": rel.get("body") or "", "zip_url": assets[zips[0]], "zip_name": zips[0],
            "sums_url": assets["SHA256SUMS.txt"], "page": rel.get("html_url", "")}


def download(info, workdir, progress=lambda done, total: None):
    """Downloads and verifies; returns the new exe's path (extracted next to the ZIP)."""
    workdir = Path(workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    try:
        with _get(info["sums_url"]) as r:
            sums = {}
            for line in r.read().decode("utf-8", "replace").splitlines():
                m = re.match(r"([0-9a-fA-F]{64})\s+\*?(.+)$", line.strip())
                if m:
                    sums[m.group(2).strip()] = m.group(1).lower()
        zpath = workdir / info["zip_name"]
        h = hashlib.sha256()
        with _get(info["zip_url"], 30) as r, open(zpath, "wb") as f:
            total, done = int(r.headers.get("Content-Length") or 0), 0
            while True:
                b = r.read(1 << 16)
                if not b:
                    break
                f.write(b)
                h.update(b)
                done += len(b)
                progress(done, total)
    except OSError as e:
        raise UpdateError(f"download failed ({e})") from e
    want = sums.get(info["zip_name"])
    if not want:
        raise UpdateError(f"SHA256SUMS.txt doesn't list {info['zip_name']}")
    if h.hexdigest() != want:
        zpath.unlink(missing_ok=True)
        raise UpdateError("the downloaded ZIP doesn't match SHA256SUMS.txt (damaged in the download); nothing was changed")
    new = workdir / "GoWLauncher.new.exe"
    try:
        with zipfile.ZipFile(zpath) as z, z.open("GoW_PSP/GoWLauncher.exe") as src, open(new, "wb") as dst:
            shutil.copyfileobj(src, dst)
    except (OSError, KeyError, zipfile.BadZipFile) as e:
        raise UpdateError(f"the ZIP has no GoW_PSP/GoWLauncher.exe ({e})") from e
    exe_sum = sums.get("GoW_PSP/GoWLauncher.exe")
    if exe_sum and hashlib.sha256(new.read_bytes()).hexdigest() != exe_sum:
        new.unlink(missing_ok=True)
        raise UpdateError("the exe in the ZIP doesn't match SHA256SUMS.txt; nothing was changed")
    return new


def apply(new_exe, current_exe, restart_args=()):
    """Swaps the exe (rename the running one away, move the new one in) and starts the new one. On failure the
    old exe is put back and UpdateError raised. The caller exits right after."""
    cur = Path(current_exe)
    old = cur.with_name(cur.stem + ".old.exe")
    if old.exists():
        try:
            old.unlink()
        except OSError:
            old = cur.with_name(f"{cur.stem}.old{int(time.time())}.exe")
    try:
        os.replace(cur, old)
    except OSError as e:
        raise UpdateError(f"couldn't move the running launcher aside ({e}); nothing was changed") from e
    try:
        shutil.move(str(new_exe), str(cur))
    except OSError as e:
        os.replace(old, cur)
        raise UpdateError(f"couldn't put the new launcher in place ({e}); the old one is back") from e
    # A one-file exe started from another one would reuse its (soon deleted) unpack folder; start it fresh.
    env = {k: v for k, v in os.environ.items() if not k.startswith(("_PYI", "_MEI"))}
    env["PYINSTALLER_RESET_ENVIRONMENT"] = "1"
    subprocess.Popen([str(cur), "--updated", str(old), *restart_args], cwd=str(cur.parent), env=env,
                     creationflags=getattr(subprocess, "DETACHED_PROCESS", 0))


def cleanup(old_exe):
    """Run by the new launcher: deletes the old exe (it may take a moment to exit). Only an .old*.exe next to the
    running one, whatever path --updated was given."""
    p = Path(old_exe)
    if not p.name.lower().endswith(".exe") or ".old" not in p.name:
        return False
    try:
        if p.resolve().parent != Path(sys.executable).resolve().parent:
            return False
    except OSError:
        return False
    for _ in range(40):
        try:
            p.unlink(missing_ok=True)
            return True
        except OSError:
            time.sleep(0.25)
    return False
