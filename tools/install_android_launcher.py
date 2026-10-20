#!/usr/bin/env python3
"""Integrate the native God of War PSP Recomp launcher into a generated SDL Android project.

Usage: python3 tools/install_android_launcher.py PATH/TO/org.gow.shell
This script edits only the project path passed on the command line.
"""
from pathlib import Path
import shutil
import sys
import xml.etree.ElementTree as ET

ANDROID = "{http://schemas.android.com/apk/res/android}"
PACKAGE = "org.gow.shell"


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: install_android_launcher.py PATH/TO/org.gow.shell", file=sys.stderr)
        return 2
    project = Path(sys.argv[1]).expanduser().resolve()
    manifest = project / "app/src/main/AndroidManifest.xml"
    java_dir = project / "app/src/main/java" / Path(PACKAGE.replace(".", "/"))
    source_dir = Path(__file__).resolve().parents[1] / "android/launcher/src/main/java" / Path(PACKAGE.replace(".", "/"))
    if not manifest.is_file() or not (project / "gradlew").is_file():
        print(f"Not an SDL Android project (missing manifest or gradlew): {project}", file=sys.stderr)
        return 1
    if not (java_dir / "SDLActivity.java").is_file():
        print(f"SDLActivity.java not found at {java_dir}; verify the generated package name.", file=sys.stderr)
        return 1

    # Copy new Java classes without deleting or replacing SDL's own sources.
    java_dir.mkdir(parents=True, exist_ok=True)
    for name in ("LauncherActivity.java", "GameActivity.java"):
        src = source_dir / name
        if not src.is_file():
            print(f"Launcher source is missing: {src}", file=sys.stderr)
            return 1
        shutil.copy2(src, java_dir / name)

    tree = ET.parse(manifest)
    root = tree.getroot()
    app = root.find("application")
    if app is None:
        raise RuntimeError("AndroidManifest.xml has no <application>")
    app.set(ANDROID + "label", "God of War PSP Recomp")

    sdl = None
    for activity in list(app.findall("activity")):
        name = activity.get(ANDROID + "name", "")
        if name in ("SDLActivity", ".SDLActivity", PACKAGE + ".SDLActivity"):
            sdl = activity
        # Remove any launcher filter from every activity before assigning it below.
        for intent in list(activity.findall("intent-filter")):
            actions = [x.get(ANDROID + "name") for x in intent.findall("action")]
            categories = [x.get(ANDROID + "name") for x in intent.findall("category")]
            if "android.intent.action.MAIN" in actions and "android.intent.category.LAUNCHER" in categories:
                activity.remove(intent)
    if sdl is None:
        raise RuntimeError("Could not find SDLActivity in the manifest")

    sdl.set(ANDROID + "name", "GameActivity")
    sdl.set(ANDROID + "label", "God of War PSP Recomp")
    sdl.set(ANDROID + "exported", "false")
    sdl.set(ANDROID + "launchMode", "singleInstance")

    launcher = next((a for a in app.findall("activity") if a.get(ANDROID + "name") == "LauncherActivity"), None)
    if launcher is None:
        launcher = ET.Element("activity")
        app.insert(0, launcher)
    launcher.set(ANDROID + "name", "LauncherActivity")
    launcher.set(ANDROID + "label", "God of War PSP Recomp")
    launcher.set(ANDROID + "exported", "true")
    launcher.set(ANDROID + "configChanges", "orientation|screenSize|keyboardHidden")
    intent = ET.SubElement(launcher, "intent-filter")
    ET.SubElement(intent, "action", {ANDROID + "name": "android.intent.action.MAIN"})
    ET.SubElement(intent, "category", {ANDROID + "name": "android.intent.category.LAUNCHER"})

    ET.indent(tree, space="    ")
    tree.write(manifest, encoding="utf-8", xml_declaration=True)
    print("Launcher integrated.")
    print(f"Project: {project}")
    print("LauncherActivity is the app entry; GameActivity hosts SDL and libmain.so.")
    print("Build the generated project only after libSDL3.so and the real libmain.so are included.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
