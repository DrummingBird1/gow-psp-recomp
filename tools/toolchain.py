"""Finds the build tools the launcher needs (Python, CMake, Visual Studio C++ tools) and, with the player's
consent, installs the missing ones with winget (docs/phase14-plan.md, task 3).

    python tools/toolchain.py            report what is found
    GOW_SIMULATE_MISSING=all|python,cmake,vs[,winget]   pretend these are missing; installs are then only
                                         printed (dry run); "winget" also pretends winget is missing
    GOW_DRY_RUN=1                        print the winget commands instead of running them
"""
import os
import shutil
import subprocess
import sys
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
    """{tool: path or None}; tools named in GOW_SIMULATE_MISSING report None."""
    sim = simulated_missing()
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


def ensure(ask, out=print):
    """Detects the tools; if some are missing, asks ask(text) -> bool and installs them. Returns {tool: path} or None."""
    found = detect()
    missing = [t for t, p in found.items() if not p]
    if not missing:
        return found
    if not ask(consent_text(missing)):
        out("Not installed: " + ", ".join(TOOLS[t]["name"] for t in missing) + ". Building needs them.")
        return None
    return detect() if install(missing, out) else None


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