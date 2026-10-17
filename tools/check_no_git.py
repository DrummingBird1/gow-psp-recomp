"""Proves the build gets its third-party sources without Git (players' PCs usually have none).

1. Static: CMakeLists.txt and cmake/ must not fetch with Git (GIT_REPOSITORY, gow_fetch_git, find_package(Git)).
2. Unless --static-only: configures the tools-only build (libchdr) into a temporary folder with an empty download
   folder, Git made unfindable for CMake (-DCMAKE_DISABLE_FIND_PACKAGE_Git=ON, GIT_EXECUTABLE pointing nowhere) and a
   PATH without any Git folder, with the launcher's portable toolchain (build/toolchain, downloaded if missing).
   --full also configures the full build (SDL3), with a stub in place of the recompiled game code.

    python tools/check_no_git.py [--static-only] [--full] [--keep DIR]
"""
import argparse, os, re, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
BAD = re.compile(r"^[^#\n]*\b(GIT_REPOSITORY|gow_fetch_git|find_package\s*\(\s*Git)\b", re.M | re.I)


def static_check():
    files = [ROOT / "CMakeLists.txt", *sorted((ROOT / "cmake").rglob("*.cmake")), *sorted((ROOT / "cmake").rglob("CMakeLists.txt"))]
    hits = []
    for f in files:
        s = f.read_text(encoding="utf-8", errors="replace")
        for m in BAD.finditer(s):
            hits.append(f"{f.relative_to(ROOT)}:{s.count(chr(10), 0, m.start()) + 1}: {m.group(0).strip()}")
    for h in hits:
        print("git fetch found:", h)
    print("static: no git fetches" if not hits else "static: FAILED")
    return not hits


def no_git_path(path):
    return os.pathsep.join(p for p in path.split(os.pathsep)
                           if p and "git" not in p.lower() and not (Path(p) / "git.exe").exists())


def configure(tools, tmp, full):
    build = tmp / ("full" if full else "tools")
    args = [tools["cmake"], "-S", str(ROOT), "-B", str(build), f"-DGOW_DEPS_DIR={tmp / 'deps'}",
            "-DCMAKE_DISABLE_FIND_PACKAGE_Git=ON", f"-DGIT_EXECUTABLE={tmp / 'no-git' / 'git.exe'}",
            "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_MAKE_PROGRAM={tools['ninja']}",
            f"-DCMAKE_C_COMPILER={tools['cc']}", f"-DCMAKE_CXX_COMPILER={tools['cxx']}"]
    stub = None
    if not full:
        args.append("-DGOW_TOOLS_ONLY=ON")
    else:  # configuring the full build only needs some generated C to exist (CI has no disc): a stub game
        stub = ROOT / "build" / "gen" / "NOGIT_STUB"
        stub.mkdir(parents=True, exist_ok=True)
        (stub / "stub.c").write_text("/* tools/check_no_git.py: configure-only stub */\n", encoding="utf-8")
        args.append("-DGAME=NOGIT_STUB")
    env = dict(tools.get("env") or os.environ)
    env["PATH"] = no_git_path(env.get("PATH", ""))
    for k in ("GIT_EXEC_PATH", "GIT_DIR"):
        env.pop(k, None)
    print(">", " ".join(map(str, args)), flush=True)
    print("  PATH:", env["PATH"], flush=True)
    try:
        r = subprocess.run(args, env=env, text=True, capture_output=True)
    finally:
        if stub:
            shutil.rmtree(stub, ignore_errors=True)
    log = r.stdout + r.stderr
    sys.stdout.write(log[-2500:])
    if r.returncode or re.search(r"could not find git|git clone|Cloning into", log, re.I):
        print(f"configure ({build.name}): FAILED")
        return False
    print(f"configure ({build.name}): ok without Git")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--static-only", action="store_true")
    ap.add_argument("--no-static", action="store_true", help=argparse.SUPPRESS)  # to reproduce old failures
    ap.add_argument("--full", action="store_true")
    ap.add_argument("--keep")
    a = ap.parse_args()
    ok = True if a.no_static else static_check()
    if a.static_only or not ok:
        return 0 if ok else 1
    import toolchain
    tc = ROOT / "build" / "toolchain"
    tools = toolchain.find_portable(tc) or (toolchain.download_portable(tc) and toolchain.find_portable(tc))
    if not tools:
        print("portable toolchain unavailable")
        return 1
    tmp = Path(a.keep) if a.keep else Path(tempfile.mkdtemp(prefix="gow_nogit_"))
    try:
        ok = configure(tools, tmp, False) and (not a.full or configure(tools, tmp, True))
    finally:
        if not a.keep:
            shutil.rmtree(tmp, ignore_errors=True)
    print("no-git check:", "ok" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
