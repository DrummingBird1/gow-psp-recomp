#!/usr/bin/env python3
"""Package a single launcher APK with separate ARM64 native runtimes for both PSP games."""
from pathlib import Path
import sys
import zipfile

LIBDIR = "lib/arm64-v8a/"
COO_LIB = LIBDIR + "libmain_coo.so"
GOS_LIB = LIBDIR + "libmain_gos.so"
SDL_LIB = LIBDIR + "libSDL3.so"
CPP_LIB = LIBDIR + "libc++_shared.so"


def main() -> int:
    if len(sys.argv) != 5:
        print("Usage: package_dual_game_launcher.py BUILT_STUB.apk COO_SOURCE.apk GOS_SOURCE.apk OUTPUT.apk", file=sys.stderr)
        return 2
    built, coo, gos, output = map(lambda value: Path(value).resolve(), sys.argv[1:])
    if not built.is_file() or not coo.is_file() or not gos.is_file():
        print("Built launcher or one of the source game APKs does not exist.", file=sys.stderr)
        return 1
    output.parent.mkdir(parents=True, exist_ok=True)

    def read_libs(apk: Path, label: str):
        with zipfile.ZipFile(apk, "r") as src:
            names = set(src.namelist())
            required = [LIBDIR + "libmain.so", SDL_LIB, CPP_LIB]
            missing = [name for name in required if name not in names or not src.getinfo(name).file_size]
            if missing:
                raise ValueError(f"{label} source APK missing required ARM64 libraries: " + ", ".join(missing))
            return src.read(required[0]), src.read(SDL_LIB), src.read(CPP_LIB)

    try:
        coo_main, sdl, cpp = read_libs(coo, "Chains of Olympus")
        gos_main, _, _ = read_libs(gos, "Ghost of Sparta")
    except (OSError, zipfile.BadZipFile, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 1

    libraries = {COO_LIB: coo_main, GOS_LIB: gos_main, SDL_LIB: sdl, CPP_LIB: cpp}
    with zipfile.ZipFile(built, "r") as base:
        with zipfile.ZipFile(output, "w") as out:
            out.comment = base.comment
            seen = set()
            for info in base.infolist():
                name = info.filename
                if name.startswith("lib/") and name.endswith(".so") and not name.startswith(LIBDIR):
                    continue
                if name == LIBDIR + "libmain.so":
                    # Drop the generated stub entry; two separately named real runtimes are added below.
                    continue
                if name in libraries:
                    out.writestr(info, libraries[name], compress_type=zipfile.ZIP_STORED)
                    seen.add(name)
                    continue
                upper = name.upper()
                if upper.startswith("META-INF/") and upper.endswith((".RSA", ".DSA", ".EC", ".SF", ".MF")):
                    continue
                out.writestr(info, base.read(name))
            for name, data in libraries.items():
                if name not in seen:
                    out.writestr(name, data, compress_type=zipfile.ZIP_STORED)

    with zipfile.ZipFile(output, "r") as result:
        bad = result.testzip()
        if bad:
            print(f"Output APK ZIP integrity check failed at {bad}", file=sys.stderr)
            return 1
        names = set(result.namelist())
        if "AndroidManifest.xml" not in names:
            print("Output APK has no AndroidManifest.xml.", file=sys.stderr)
            return 1
    print(f"Packaged dual-game launcher from {coo.name} and {gos.name}: {output}")
    print("Included native runtimes: libmain_coo.so and libmain_gos.so. Next: zipalign, sign, and test both titles.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
