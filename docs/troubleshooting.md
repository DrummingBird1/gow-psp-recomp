# Troubleshooting and FAQ

For players of the launcher (`GoWLauncher.exe`). Developers: see the main [README](../README.md#build-from-source).

## Windows says "Windows protected your PC", or the antivirus flags the exe

`GoWLauncher.exe` is not code-signed and is built with PyInstaller, which some antivirus programs flag as a false
positive. Check the SHA-256 against the release notes or the release's `SHA256SUMS.txt`
(PowerShell: `Get-FileHash GoWLauncher.exe`). If it matches, click **More info**, then **Run anyway**.

## Where are my files?

Everything the launcher creates is in `GoW_PSP_data` next to `GoWLauncher.exe`:

| Path | What |
|---|---|
| `GoW_PSP_data\build\ms0`, `ms0_gos` | Saves (Chains of Olympus, Ghost of Sparta) |
| `GoW_PSP_data\build\launcher.json` | Launcher settings |
| `GoW_PSP_data\build\launcher_coo.log`, `launcher_gos.log` | What the game printed during the last Play |
| `GoW_PSP_data\build\launcher_coo_input.txt`, `launcher_gos_input.txt` | Your button presses during the last Play |
| `GoW_PSP_data\build\screenshots` | F12 screenshots (keep them private: they show the game) |
| `GoW_PSP_data\build\toolchain` | The portable build tools (about 550 MB) |

Your disc images stay where they are; the launcher only reads them.

## How do I update?

Download the new ZIP and extract it over the old `GoW_PSP` folder (or copy the new `GoWLauncher.exe` in). Keep
`GoW_PSP_data`: your saves, settings and the build tools stay. The launcher rebuilds a game only when it needs to.

## The build fails

- Check your internet connection on the first Build: it downloads the build tools (246 MB) and some sources. Downloads
  are checked against pinned SHA-256 hashes, so a broken download fails instead of building something wrong.
- No Git, Python or Visual Studio is needed. If you have Visual Studio and want to rule it out, it is only used when
  you run from source.
- Make sure the disc image is the **US** release (UCUS98653 / UCUS98737); other regions are not supported.
- Press Build again: a failed configure step cleans its folder and retries.
- Still failing: open an [issue](https://github.com/DrummingBird1/gow-psp-recomp/issues/new/choose) with the
  launcher's log window text (the build output).

## The game stutters

- Try the **Original** or **Balanced** preset, a lower internal resolution, or the original 60 FPS.
- The launcher minimizes itself while a game runs; if you restored it, minimize it again.
- V-Sync on gives the most even frame pacing on a 60 Hz display.

## Controls, skipping movies, quitting

See [Controls](../README.md#controls) in the README. In short: Enter, Space, a click or gamepad Start/Cross skips a
movie; Esc asks before quitting; Alt+F4, the close button or holding gamepad Back+Start quits. The game saves only at
its own save points.

## Reporting a bug

See [Reporting a bug](../README.md#reporting-a-bug): attach the log and input files listed above, never game files,
saves or screenshots of the game.
