#!/usr/bin/env python3
"""Replace the generated SDL stub's ARM64 native libraries with those from a playable APK.

The generated project supplies the launcher/manifest/Java code. The source APK supplies the
matching, already-working native game runtime. The result must be zipaligned and signed after
this script runs.
"""
from pathlib import Path
import sys
import zipfile

REQUIRED = (
    "lib/arm64-v8a/libmain.so",
    "lib/arm64-v8a/libSDL3.so",
    "lib/arm64-v8a/libc++_shared.so",
)


def main() -> int:
    if len(sys.argv) != 4:
        print("Usage: package_playable_launcher.py BUILT_STUB.apk SOURCE_GAME.apk OUTPUT.apk", file=sys.stderr)
        return 2
    built, source, output = map(lambda value: Path(value).resolve(), sys.argv[1:])
    if not built.is_file() or not source.is_file():
        print("Built APK or source game APK does not exist.", file=sys.stderr)
        return 1
    output.parent.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(source, "r") as src:
        names = set(src.namelist())
        missing = [name for name in REQUIRED if name not in names or not src.getinfo(name).file_size]
        if missing:
            print("Source APK is missing required non-empty ARM64 libraries: " + ", ".join(missing), file=sys.stderr)
            return 1
        libraries = {name: src.read(name) for name in REQUIRED}

    with zipfile.ZipFile(built, "r") as base:
        with zipfile.ZipFile(output, "w") as out:
            out.comment = base.comment
            replaced = set()
            seen = set()
            for info in base.infolist():
                name = info.filename
                # The target game is ARM64-only. Remove stub native libraries for other ABIs.
                if name.startswith("lib/") and name.endswith(".so") and not name.startswith("lib/arm64-v8a/"):
                    continue
                if name in REQUIRED:
                    out.writestr(info, libraries[name])
                    replaced.add(name)
                    seen.add(name)
                    continue
                # Rewriting the ZIP invalidates APK signatures; remove v1 signature files too.
                upper = name.upper()
                if upper.startswith("META-INF/") and upper.endswith((".RSA", ".DSA", ".EC", ".SF", ".MF")):
                    continue
                out.writestr(info, base.read(name))
            # SDL's generated project may not bundle libc++_shared.so, so add any missing
            # required game libraries as fresh ZIP entries instead of requiring placeholder slots.
            for name in REQUIRED:
                if name not in seen:
                    out.writestr(name, libraries[name], compress_type=zipfile.ZIP_STORED)
                    replaced.add(name)

    with zipfile.ZipFile(output, "r") as result:
        bad = result.testzip()
        if bad:
            print(f"Output APK ZIP integrity check failed at {bad}", file=sys.stderr)
            return 1
        names = set(result.namelist())
        if "AndroidManifest.xml" not in names:
            print("Output APK has no AndroidManifest.xml.", file=sys.stderr)
            return 1
    print(f"Packaged launcher with native libraries from {source.name}: {output}")
    print("Next steps: zipalign and sign this APK before installing.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
