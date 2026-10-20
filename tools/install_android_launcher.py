#!/usr/bin/env python3
"""Integrate the native Android launcher into a generated SDL Android project.

Usage: python3 tools/install_android_launcher.py PATH/TO/org.gow.shell
Only the explicitly supplied generated project is modified.
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

    for name in ("LauncherActivity.java", "GameActivity.java"):
        if not (source_dir / name).is_file():
            print(f"Launcher source is missing: {source_dir / name}", file=sys.stderr)
            return 1

    tree = ET.parse(manifest)
    root = tree.getroot()
    app = root.find("application")
    if app is None:
        raise RuntimeError("AndroidManifest.xml has no <application>")

    # The SDL project generator creates an activity named after the package's last segment
    # (for org.gow.shell, this is ShellActivity). Reuse its manifest slot for GameActivity.
    generated_activity = None
    for activity in list(app.findall("activity")):
        for intent in list(activity.findall("intent-filter")):
            actions = [x.get(ANDROID + "name") for x in intent.findall("action")]
            categories = [x.get(ANDROID + "name") for x in intent.findall("category")]
            if "android.intent.action.MAIN" in actions and "android.intent.category.LAUNCHER" in categories:
                generated_activity = activity
                activity.remove(intent)

    if generated_activity is None:
        raise RuntimeError("Could not find the generated SDL launcher activity in the manifest")

    java_dir.mkdir(parents=True, exist_ok=True)
    for name in ("LauncherActivity.java", "GameActivity.java"):
        shutil.copy2(source_dir / name, java_dir / name)

    generated_activity.set(ANDROID + "name", "GameActivity")
    generated_activity.set(ANDROID + "label", "God of War PSP Recomp")
    generated_activity.set(ANDROID + "exported", "false")
    generated_activity.set(ANDROID + "launchMode", "singleInstance")
    app.set(ANDROID + "label", "God of War PSP Recomp")

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
    print("Ensure the real libmain.so and libSDL3.so are packaged before building a playable APK.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
