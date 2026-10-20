# Android launcher integration

The native launcher sources live in `android/launcher/` and are separate from SDL's Java sources.

## Integrate into a generated SDL project

Run the SDL Android project generator into a **dedicated temporary output directory**. The SDL generator deletes the package destination before recreating it, so do not point it at a folder containing work you want to keep.

Then run:

```sh
python3 tools/install_android_launcher.py /absolute/path/to/org.gow.shell
```

The script adds `LauncherActivity` and `GameActivity`, changes the manifest entry point to the launcher, and keeps SDL's own Java sources intact.

## Important build limitation

This integration does not by itself produce a playable APK. The app must package the actual ARM64 `libmain.so` compiled from the generated God of War sources and the matching `libSDL3.so`. The existing `android-sdl.yml` workflow uses a stub `main()` and is only an SDL shell smoke test; it must not be mistaken for a game build.

The launcher looks for `image.bin` and `game.iso` in the app-specific external files directory, normally:

```
Android/data/org.gow.shell/files/
```

It intentionally allows the Play button even if files are missing, so the runtime can produce its diagnostic log. No game image or copyrighted game assets are included.
