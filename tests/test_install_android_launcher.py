#!/usr/bin/env python3
"""Regression test for Android launcher manifest integration (no Android SDK required)."""
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "install_android_launcher.py"
ANDROID = "{http://schemas.android.com/apk/res/android}"
USB_ATTACH = "android.hardware.usb.action.USB_DEVICE_ATTACHED"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="gow-launcher-test-") as tmp:
        project = Path(tmp)
        manifest = project / "app/src/main/AndroidManifest.xml"
        manifest.parent.mkdir(parents=True)
        (project / "gradlew").write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        manifest.write_text(
            """<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="org.gow.shell">
  <application android:label="SDL">
    <activity android:name="ShellActivity" android:exported="true"
      android:launchMode="singleTask" android:configChanges="orientation|screenSize|keyboardHidden">
      <intent-filter>
        <action android:name="android.intent.action.MAIN" />
        <category android:name="android.intent.category.LAUNCHER" />
      </intent-filter>
      <intent-filter>
        <action android:name="android.hardware.usb.action.USB_DEVICE_ATTACHED" />
      </intent-filter>
    </activity>
  </application>
</manifest>
""",
            encoding="utf-8",
        )

        # Running twice catches duplicate-activity and duplicate-launcher regressions.
        for _ in range(2):
            subprocess.run(
                [sys.executable, str(SCRIPT), str(project)],
                cwd=ROOT,
                check=True,
                capture_output=True,
                text=True,
            )

        root = ET.parse(manifest).getroot()
        app = root.find("application")
        activities = app.findall("activity")
        launcher_activities = [a for a in activities if a.get(ANDROID + "name", "").rsplit(".", 1)[-1] == "LauncherActivity"]
        game_activities = [a for a in activities if a.get(ANDROID + "name", "").rsplit(".", 1)[-1] == "GameActivity"]
        assert len(launcher_activities) == 1, f"expected one LauncherActivity, got {len(launcher_activities)}"
        assert len(game_activities) == 1, f"expected one GameActivity, got {len(game_activities)}"

        def is_launcher(intent):
            actions = {x.get(ANDROID + "name") for x in intent.findall("action")}
            categories = {x.get(ANDROID + "name") for x in intent.findall("category")}
            return "android.intent.action.MAIN" in actions and "android.intent.category.LAUNCHER" in categories

        launcher = launcher_activities[0]
        game = game_activities[0]
        launcher_filters = [i for i in launcher.findall("intent-filter") if is_launcher(i)]
        game_filters = [i for i in game.findall("intent-filter") if is_launcher(i)]
        assert len(launcher_filters) == 1, f"expected one launcher intent-filter, got {len(launcher_filters)}"
        assert not game_filters, "GameActivity must not remain the launcher"

        usb_filters = [
            i for i in game.findall("intent-filter")
            if USB_ATTACH in {a.get(ANDROID + "name") for a in i.findall("action")}
        ]
        assert len(usb_filters) == 1, "USB_DEVICE_ATTACHED filter must remain on GameActivity"
        assert game.get(ANDROID + "exported") == "true", "GameActivity must stay exported for USB attach dispatch"
        assert game.get(ANDROID + "launchMode") == "singleTask", "original SDL launchMode must be preserved"
        assert game.get(ANDROID + "configChanges") == "orientation|screenSize|keyboardHidden", "original SDL configChanges must be preserved"
        assert (project / "app/src/main/java/org/gow/shell/LauncherActivity.java").is_file()
        assert (project / "app/src/main/java/org/gow/shell/GameActivity.java").is_file()
        print("PASS: launcher is idempotent; USB filter and original SDL activity settings are preserved.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
