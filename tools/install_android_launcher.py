#!/usr/bin/env python3
"""Integrate the native Android launcher into a generated SDL Android project.

Usage: python3 tools/install_android_launcher.py PATH/TO/org.gow.shell
Only the explicitly supplied generated project is modified. Safe to run repeatedly.
"""
from pathlib import Path
import shutil
import re
import sys
import xml.etree.ElementTree as ET

ANDROID = "{http://schemas.android.com/apk/res/android}"
PACKAGE = "org.gow.shell"
USB_ATTACH = "android.hardware.usb.action.USB_DEVICE_ATTACHED"


def class_name(activity: ET.Element) -> str:
    """Return the simple activity class name for relative or fully qualified manifest names."""
    return activity.get(ANDROID + "name", "").rsplit(".", 1)[-1]


def is_launcher_filter(intent: ET.Element) -> bool:
    actions = {x.get(ANDROID + "name") for x in intent.findall("action")}
    categories = {x.get(ANDROID + "name") for x in intent.findall("category")}
    return (
        "android.intent.action.MAIN" in actions
        and "android.intent.category.LAUNCHER" in categories
    )


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: install_android_launcher.py PATH/TO/org.gow.shell", file=sys.stderr)
        return 2
    project = Path(sys.argv[1]).expanduser().resolve()
    manifest = project / "app/src/main/AndroidManifest.xml"
    java_dir = project / "app/src/main/java" / Path(PACKAGE.replace(".", "/"))
    source_dir = (
        Path(__file__).resolve().parents[1]
        / "android/launcher/src/main/java"
        / Path(PACKAGE.replace(".", "/"))
    )
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

    # Permite baixar um APK e abrir o instalador do Android dentro do launcher.
    install_permission = "android.permission.REQUEST_INSTALL_PACKAGES"
    if not any(p.get(ANDROID + "name") == install_permission for p in root.findall("uses-permission")):
        root.insert(0, ET.Element("uses-permission", {ANDROID + "name": install_permission}))

    providers = app.findall("provider")
    provider = next((p for p in providers if p.get(ANDROID + "authorities") == "${applicationId}.fileprovider"), None)
    if provider is None:
        provider = ET.SubElement(app, "provider")
    provider.set(ANDROID + "name", "androidx.core.content.FileProvider")
    provider.set(ANDROID + "authorities", "${applicationId}.fileprovider")
    provider.set(ANDROID + "exported", "false")
    provider.set(ANDROID + "grantUriPermissions", "true")
    for old_meta in list(provider.findall("meta-data")):
        if old_meta.get(ANDROID + "name") == "android.support.FILE_PROVIDER_PATHS":
            provider.remove(old_meta)
    ET.SubElement(provider, "meta-data", {
        ANDROID + "name": "android.support.FILE_PROVIDER_PATHS",
        ANDROID + "resource": "@xml/gow_file_paths",
    })

    # FileProvider só expõe o APK temporário que o próprio launcher baixou.
    xml_dir = project / "app/src/main/res/xml"
    xml_dir.mkdir(parents=True, exist_ok=True)
    (xml_dir / "gow_file_paths.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<paths xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <cache-path name="updates" path="updates/" />\n'
        '</paths>\n', encoding="utf-8")

    # A SDL gera o projeto Gradle em tempo de build; adicionar FileProvider nele.
    gradle = project / "app/build.gradle"
    gradle_kts = project / "app/build.gradle.kts"
    gradle_file = gradle if gradle.is_file() else gradle_kts
    if gradle_file.is_file():
        gradle_text = gradle_file.read_text(encoding="utf-8")
        if "androidx.core:core" not in gradle_text:
            match = re.search(r"dependencies\s*\{", gradle_text)
            if not match:
                raise RuntimeError("Não foi encontrado o bloco dependencies do Gradle")
            if gradle_file.suffix == ".kts":
                dep = '\n    implementation("androidx.core:core:1.13.1")'
            else:
                dep = '\n    implementation "androidx.core:core:1.13.1"'
            gradle_text = gradle_text[:match.end()] + dep + gradle_text[match.end():]
            gradle_file.write_text(gradle_text, encoding="utf-8")

    # O FileProvider usa AndroidX; habilitar explicitamente no projeto SDL gerado.
    gradle_properties = project / "gradle.properties"
    properties_text = gradle_properties.read_text(encoding="utf-8") if gradle_properties.is_file() else ""
    if not re.search(r"(?m)^android\.useAndroidX\s*=\s*true\s*$", properties_text):
        properties_text += "\nandroid.useAndroidX=true\n"
        gradle_properties.write_text(properties_text.lstrip("\n"), encoding="utf-8")

    activities = list(app.findall("activity"))
    launcher = next((a for a in activities if class_name(a) == "LauncherActivity"), None)
    game_activity = next((a for a in activities if class_name(a) == "GameActivity"), None)

    # If this script has already run, reuse GameActivity. Otherwise convert SDL's generated
    # MAIN/LAUNCHER activity (e.g. ShellActivity) into GameActivity.
    if game_activity is None:
        generated_activity = None
        for activity in activities:
            if activity is launcher:
                continue
            matching = [i for i in list(activity.findall("intent-filter")) if is_launcher_filter(i)]
            if matching:
                generated_activity = activity
                for intent in matching:
                    activity.remove(intent)
                break
        if generated_activity is None:
            raise RuntimeError("Could not find the generated SDL launcher activity in the manifest")
    else:
        generated_activity = game_activity
        for intent in list(generated_activity.findall("intent-filter")):
            if is_launcher_filter(intent):
                generated_activity.remove(intent)

    # Preserve all non-launcher filters (notably USB_DEVICE_ATTACHED) and the original
    # SDL activity attributes. These filters may require the activity to remain exported.
    remaining_filters = list(generated_activity.findall("intent-filter"))
    has_usb_attach = any(
        any(action.get(ANDROID + "name") == USB_ATTACH for action in intent.findall("action"))
        for intent in remaining_filters
    )

    java_dir.mkdir(parents=True, exist_ok=True)
    for name in ("LauncherActivity.java", "GameActivity.java"):
        shutil.copy2(source_dir / name, java_dir / name)

    generated_activity.set(ANDROID + "name", "GameActivity")
    generated_activity.set(ANDROID + "label", "God of War PSP Recomp")
    generated_activity.set(ANDROID + "icon", "@mipmap/ic_launcher")  # gow_icon_v1
    generated_activity.set(ANDROID + "exported", "true" if has_usb_attach else "false")
    # Do not overwrite launchMode/configChanges/other SDL-generated attributes.

    app.set(ANDROID + "label", "God of War PSP Recomp")
    app.set(ANDROID + "icon", "@mipmap/ic_launcher")  # gow_icon_v1

    if launcher is None:
        launcher = ET.Element("activity")
        app.insert(0, launcher)
    # Make reruns deterministic: keep exactly one launcher intent-filter.
    for intent in list(launcher.findall("intent-filter")):
        if is_launcher_filter(intent):
            launcher.remove(intent)
    launcher.set(ANDROID + "name", "LauncherActivity")
    launcher.set(ANDROID + "label", "God of War PSP Recomp")
    launcher.set(ANDROID + "icon", "@mipmap/ic_launcher")  # gow_icon_v1
    launcher.set(ANDROID + "exported", "true")
    launcher.set(ANDROID + "configChanges", "orientation|screenSize|keyboardHidden")
    launcher.set(ANDROID + "screenOrientation", "sensorLandscape")
    intent = ET.SubElement(launcher, "intent-filter")
    ET.SubElement(intent, "action", {ANDROID + "name": "android.intent.action.MAIN"})
    ET.SubElement(intent, "category", {ANDROID + "name": "android.intent.category.LAUNCHER"})

    ET.indent(tree, space="    ")
    # gow_icon_v1: icone proprio (substitui o icone padrao do SDL)
    icon_src = Path(__file__).resolve().parent.parent / "android/launcher/icon"
    res = project / "app/src/main/res"
    for dens in ("mdpi", "hdpi", "xhdpi", "xxhdpi", "xxxhdpi"):
        dst = res / ("mipmap-" + dens)
        dst.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(icon_src / ("ic_launcher-" + dens + ".png"), dst / "ic_launcher.png")
    xx = res / "mipmap-xxxhdpi"
    shutil.copyfile(icon_src / "ic_launcher_foreground.png", xx / "ic_launcher_foreground.png")
    shutil.copyfile(icon_src / "ic_launcher_background.png", xx / "ic_launcher_background.png")
    ad = res / "mipmap-anydpi-v26"
    ad.mkdir(parents=True, exist_ok=True)
    (ad / "ic_launcher.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <background android:drawable="@mipmap/ic_launcher_background"/>\n'
        '    <foreground android:drawable="@mipmap/ic_launcher_foreground"/>\n'
        '</adaptive-icon>\n', encoding="utf-8")
    tree.write(manifest, encoding="utf-8", xml_declaration=True)
    print("Launcher integrated.")
    print(f"Project: {project}")
    print("LauncherActivity is the app entry; GameActivity hosts SDL and libmain.so.")
    if has_usb_attach:
        print("USB_DEVICE_ATTACHED filter preserved on exported GameActivity.")
    print("Ensure the real libmain.so and libSDL3.so are packaged before building a playable APK.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
