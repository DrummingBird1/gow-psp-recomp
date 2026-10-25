"""The build tools the launcher needs.

Default for players: a portable toolchain the launcher downloads once, with the player's consent, into its data
folder (build/toolchain): llvm-mingw (Clang, lld, mingw-w64 headers and libraries), CMake and Ninja, each from its
official GitHub release and checked against a pinned SHA-256. No administrator rights, nothing installed in Windows.

Also supported (developers, and the fallback when the portable path fails): Visual Studio's C++ tools with CMake,
found on the PC or, with consent, installed with winget (docs/phase14-plan.md, task 3).

    GOW_TOOLCHAIN=portable|msvc|auto     which one to use; default portable for GoWLauncher.exe, auto (Visual Studio
                                         when it is installed, else portable) when run from source
    python tools/toolchain.py            report what is found
    GOW_SIMULATE_MISSING=all|python,cmake,vs[,winget]   pretend these are missing; installs are then only
                                         printed (dry run); "winget" also pretends winget is missing
    GOW_DRY_RUN=1                        print the winget commands instead of running them
"""
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

VC_COMPONENT = "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
TOOLS = {
    "python": {"name": "Python 3.12", "winget": "Python.Python.3.12", "override": None},
    "cmake": {"name": "CMake", "winget": "Kitware.CMake", "override": None},
    "vs": {"name": "Visual Studio 2022 Build Tools with the C++ workload (about 6 GB)",
           "winget": "Microsoft.VisualStudio.2022.BuildTools",
           "override": "--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"},
}
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

# The portable toolchain: official release archives, pinned by SHA-256 (licenses: tools/launcher_assets/
# THIRD_PARTY_NOTICES.txt). "skip" leaves out parts a 64-bit Windows build never uses (other targets, Python).
PORTABLE = [
    {"name": "llvm-mingw 20261006 (Clang, lld, mingw-w64)", "dir": "llvm-mingw",
     "url": "https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-x86_64.zip",
     "sha256": "317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a", "size": 190836485,
     "skip": ("aarch64-w64-mingw32/", "armv7-w64-mingw32/", "i686-w64-mingw32/", "python/")},
    {"name": "CMake 4.4.4", "dir": "cmake",
     "url": "https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-windows-x86_64.zip",
     "sha256": "bace36e94b31c68ab6fa295f26dfa11219e0701cf7c94b0284a7d1cb13dac536", "size": 54400562,
     "skip": ("doc/", "man/")},
    {"name": "Ninja 1.13.2", "dir": "ninja",
     "url": "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip",
     "sha256": "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65", "size": 291570, "skip": ()},
]
PORTABLE_MB = round(sum(p["size"] for p in PORTABLE) / 1e6)


def mode():
    m = os.environ.get("GOW_TOOLCHAIN", "").strip().lower()
    if m in ("portable", "msvc", "auto"):
        return m
    return "portable" if getattr(sys, "frozen", False) else "auto"


def _one_subdir(d):
    """The archive's top folder (llvm-mingw-20261006-ucrt-x86_64/...), or d itself."""
    subs = [p for p in d.iterdir() if p.is_dir()] if d.is_dir() else []
    return subs[0] if len(subs) == 1 and not any(p.is_file() and not p.name.startswith(".") for p in d.iterdir()) else d


def find_portable(root):
    """{"kind": "portable", cmake, ninja, cc, cxx, env} when the toolchain under root is complete, else None."""
    root = Path(root)
    if "portable" in simulated_missing() or not all((root / p["dir"] / ".complete").read_text(errors="replace").strip() == p["sha256"]
                                                     if (root / p["dir"] / ".complete").is_file() else False for p in PORTABLE):
        return None
    llvm, cmake = _one_subdir(root / "llvm-mingw"), _one_subdir(root / "cmake")
    tools = {"kind": "portable", "cmake": str(cmake / "bin" / "cmake.exe"), "ninja": str(root / "ninja" / "ninja.exe"),
             "cc": str(llvm / "bin" / "clang.exe"), "cxx": str(llvm / "bin" / "clang++.exe"), "python": find_python()}
    if not all(Path(tools[k]).is_file() for k in ("cmake", "ninja", "cc", "cxx")):
        return None
    # the compiler's own folder first (lld, llvm-rc), nothing from a Visual Studio prompt
    tools["env"] = dict(os.environ, PATH=os.pathsep.join([str(llvm / "bin"), str(root / "ninja"), os.environ.get("PATH", "")]))
    for v in ("INCLUDE", "LIB", "LIBPATH", "VCINSTALLDIR", "CC", "CXX"):
        tools["env"].pop(v, None)
    return tools


def download_portable(root, out=print):
    """Downloads, checks (SHA-256) and unpacks the portable toolchain into root. True when complete."""
    root = Path(root)
    for p in PORTABLE:
        if not download_archive(p, root, out):
            return False
    return find_portable(root) is not None


def download_archive(p, root, out=print):
    """Downloads one pinned archive (an entry like those of PORTABLE: name, dir, url, sha256, size, skip), checks its
    SHA-256 and unpacks it into root/dir; a folder already unpacked from the same archive is kept. True when there."""
    root = Path(root)
    dest = root / p["dir"]
    if (dest / ".complete").is_file() and (dest / ".complete").read_text().strip() == p["sha256"]:
        return True
    archive = root / (p["dir"] + ".zip.part")
    root.mkdir(parents=True, exist_ok=True)
    out(f"downloading {p['name']} ({p['size'] / 1e6:.0f} MB) from {p['url']}")
    h, done, shown = hashlib.sha256(), 0, -1
    try:
        req = urllib.request.Request(p["url"], headers={"User-Agent": "GoWLauncher"})
        with urllib.request.urlopen(req, timeout=60) as r, open(archive, "wb") as f:
            while True:
                block = r.read(1 << 20)
                if not block:
                    break
                f.write(block)
                h.update(block)
                done += len(block)
                pct = done * 100 // max(p["size"], 1)
                if pct // 10 != shown:
                    shown = pct // 10
                    out(f"  {p['dir']}: {min(pct, 100)}% ({done / 1e6:.0f} MB)")
    except OSError as e:
        out(f"download failed: {e}")
        return False
    if h.hexdigest() != p["sha256"]:
        archive.unlink(missing_ok=True)
        out(f"{p['name']}: wrong SHA-256 ({h.hexdigest()}), expected {p['sha256']}; not used")
        return False
    out(f"  checked SHA-256, unpacking into {dest}")
    shutil.rmtree(dest, ignore_errors=True)
    with zipfile.ZipFile(archive) as z:
        for m in z.infolist():
            rel = m.filename.split("/", 1)[1] if "/" in m.filename.rstrip("/") else ""
            if rel and rel.startswith(p["skip"]):
                continue
            z.extract(m, dest)
    archive.unlink(missing_ok=True)
    (dest / ".complete").write_text(p["sha256"])
    return True


def portable_consent_text():
    lines = [f"Building the games needs a C/C++ compiler. Download the portable build tools now (about {PORTABLE_MB} MB, "
             "once; about 550 MB unpacked)?", ""]
    lines += [f"  - {p['name']}" for p in PORTABLE]
    lines += ["", "They come from the projects' official GitHub releases, are checked against pinned SHA-256 hashes and stay "
              "in the launcher's GoW_PSP_data folder. Nothing is installed in Windows and no administrator rights are needed."]
    return "\n".join(lines)


def simulated_missing():
    v = os.environ.get("GOW_SIMULATE_MISSING", "").strip().lower()
    return {t.strip() for t in v.split(",") if t.strip()}


def dry_run():
    return bool(simulated_missing()) or os.environ.get("GOW_DRY_RUN", "") not in ("", "0")


def refresh_path():
    """After an install, take PATH from the registry (machine + user) so new tools are found without a restart."""
    if os.name != "nt":
        return
    import winreg
    parts = []
    for hive, key in ((winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment"),
                      (winreg.HKEY_CURRENT_USER, "Environment")):
        try:
            with winreg.OpenKey(hive, key) as k:
                parts.append(os.path.expandvars(winreg.QueryValueEx(k, "Path")[0]))
        except OSError:
            pass
    seen, merged = set(), []
    for p in ";".join(parts + [os.environ.get("PATH", "")]).split(";"):
        if p and p.lower() not in seen:
            seen.add(p.lower())
            merged.append(p)
    os.environ["PATH"] = ";".join(merged)


def _vswhere():
    p = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    return p if p.is_file() else None


def find_vs():
    """Installation path of a Visual Studio (any edition, 2022 or later) that has the x64 C++ tools, or None."""
    vswhere = _vswhere()
    if not vswhere:
        return None
    try:
        r = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires", VC_COMPONENT, "-property", "installationPath"],
                           capture_output=True, text=True, timeout=60, creationflags=NO_WINDOW)
    except (OSError, subprocess.TimeoutExpired):
        return None
    path = r.stdout.strip().splitlines()[0] if r.stdout.strip() else ""
    return path if path and Path(path).is_dir() else None


def find_cmake():
    p = shutil.which("cmake")
    if p:
        return p
    for base in (os.environ.get("ProgramFiles", r"C:\Program Files"), os.environ.get("LOCALAPPDATA", "")):
        c = Path(base) / "CMake" / "bin" / "cmake.exe" if base else None
        if c and c.is_file():
            return str(c)
    vs = find_vs()
    if vs:
        c = Path(vs) / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "CMake" / "bin" / "cmake.exe"
        if c.is_file():
            return str(c)
    return None


def find_python():
    """A Python 3 to run recomp/recomp.py: this one, unless the launcher is a frozen exe."""
    if not getattr(sys, "frozen", False):
        return sys.executable
    for cand in (["py", "-3"], ["python"]):
        exe = shutil.which(cand[0])
        if not exe:
            continue
        try:
            r = subprocess.run([exe] + cand[1:] + ["-c", "import sys; print(sys.executable)"], capture_output=True, text=True,
                               timeout=30, creationflags=NO_WINDOW)
        except (OSError, subprocess.TimeoutExpired):
            continue
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    local = Path(os.environ.get("LOCALAPPDATA", "")) / "Programs" / "Python" / "Python312" / "python.exe"
    return str(local) if local.is_file() else None


FINDERS = {"python": find_python, "cmake": find_cmake, "vs": find_vs}


def detect():
    """Visual Studio path: {tool: path or None}; tools named in GOW_SIMULATE_MISSING report None. The frozen
    launcher runs the recompiler itself, so it needs no Python."""
    sim = simulated_missing()
    if getattr(sys, "frozen", False):
        return {t: (sys.executable if t == "python" else None if t in sim or "all" in sim else FINDERS[t]()) for t in TOOLS}
    return {t: (None if t in sim or "all" in sim else FINDERS[t]()) for t in TOOLS}


def find_winget():
    if "winget" in simulated_missing():
        return None
    w = shutil.which("winget")
    if w:
        return w
    w = Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft" / "WindowsApps" / "winget.exe"
    return str(w) if w.is_file() else None


def winget_command(tool, winget="winget"):
    t = TOOLS[tool]
    cmd = [winget, "install", "--id", t["winget"], "--exact", "--source", "winget",
           "--accept-package-agreements", "--accept-source-agreements", "--disable-interactivity"]
    if t["override"]:
        cmd += ["--override", t["override"]]
    return cmd


def consent_text(missing):
    lines = ["These build tools are missing. Install them now with winget (Windows Package Manager)?", ""]
    lines += [f"  - {TOOLS[t]['name']}  (winget id {TOOLS[t]['winget']})" for t in missing]
    lines += ["", "Windows may ask for administrator permission. Building the game needs them once."]
    return "\n".join(lines)


def install(missing, out=print):
    """Installs the missing tools with winget (or prints the commands in a dry run). Returns True when all are
    present afterwards; messages for the player go to out."""
    winget = find_winget()
    if not winget:
        out("winget (Windows Package Manager) was not found. Install \"App Installer\" from the Microsoft Store, or install "
            "these yourself: " + ", ".join(TOOLS[t]["name"] for t in missing) + " (see README.md).")
        return False
    for t in missing:
        cmd = winget_command(t, winget or "winget")
        out("> " + subprocess.list2cmdline(cmd))
        if dry_run():
            out("  (dry run: not executed)")
            continue
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                             creationflags=NO_WINDOW)
        for line in p.stdout:
            if line.strip():
                out(line.rstrip())
        # 0 = installed; 0x8A15002B = already installed / no newer version
        if p.wait() not in (0, 0x8A15002B, -1978335189):
            out(f"Installing {TOOLS[t]['name']} failed (winget exit 0x{p.returncode & 0xFFFFFFFF:08X}). "
                "Try again, or install it yourself (see README.md).")
            return False
    if dry_run():
        out("dry run: nothing was installed")
        return False
    refresh_path()
    still = [t for t, p in detect().items() if not p]
    if still:
        out("Still not found after installing: " + ", ".join(TOOLS[t]["name"] for t in still)
            + ". Restart the launcher (or Windows) and try again.")
        return False
    return True


def ensure(ask, out=print, root=None):
    """The tools for a build: the portable toolchain under root (downloaded once after ask(text) -> bool), or Visual
    Studio (GOW_TOOLCHAIN, see the top). Returns a dict (cmake, python, kind, ...) or None."""
    m = mode()
    msvc = detect()
    has_msvc = all(msvc.values())
    if m == "msvc" or (m == "auto" and has_msvc) or root is None:
        return ensure_msvc(ask, out)
    tools = find_portable(root)
    if tools:
        return tools
    if ask(portable_consent_text()):
        if download_portable(root, out):
            return find_portable(root)
        out("The portable build tools could not be set up.")
    else:
        out("Not downloaded: the portable build tools. Building needs them (or Visual Studio).")
    if has_msvc:
        out("Using Visual Studio's C++ tools instead.")
        return dict(msvc, kind="msvc")
    return None


def ensure_msvc(ask, out=print):
    """Visual Studio path: detects the tools; if some are missing, asks ask(text) -> bool and installs them with
    winget. Returns {tool: path, kind: "msvc"} or None."""
    found = detect()
    missing = [t for t, p in found.items() if not p]
    if not missing:
        return dict(found, kind="msvc")
    if not ask(consent_text(missing)):
        out("Not installed: " + ", ".join(TOOLS[t]["name"] for t in missing) + ". Building needs them.")
        return None
    return dict(detect(), kind="msvc") if install(missing, out) else None


def ask_console(text):
    print(text)
    if not sys.stdin or not sys.stdin.isatty():
        try:
            answer = sys.stdin.readline() if sys.stdin else ""
        except OSError:
            answer = ""
    else:
        answer = input("Install? [y/N] ")
    return answer.strip().lower() in ("y", "yes")


if __name__ == "__main__":
    for tool, path in detect().items():
        print(f"{tool:7} {path or 'MISSING'}")
    print(f"winget  {find_winget() or 'MISSING'}")