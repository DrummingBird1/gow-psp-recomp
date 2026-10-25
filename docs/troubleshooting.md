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

- Try the **Light** preset (x2, the starting preset without a dedicated GPU) or **Original**, a lower internal
  resolution, or the original 60 FPS. F3 shows the frame rate in the game.
- The launcher minimizes itself while a game runs; if you restored it, minimize it again.
- V-Sync on gives the most even frame pacing on a 60 Hz display.

## The sound is choppy, but the music plays fine

This comes from a frame rate above 60 that the PC can't hold every frame: the game then skips ahead to keep time,
and a sound channel can run dry for a moment. Since this version the skips leave the sound alone as far as they can,
but the cure is **Frame rate: 60 (original)** in Options, or a lower preset. The game log reports `dropouts per
channel` and `virtual-time skips` when the game closes; include it in a bug report.

## My texture pack does nothing

**PPSSPP texture packs don't work here.** PPSSPP names each replacement by its own texture hash (listed in
`textures.ini`); this port uses a different key, so none of their files matches. The launcher says so under
**Options → Advanced → Texture pack folder**, and the game log does too.

What works is a pack in this port's own format:

1. Tick **Dump textures** in Advanced and play the parts you want to change. Each new texture is written once as
   `<16 hex digits>.png` into `GoW_PSP_data/build/texture_dump/<game>/` (`build/texture_dump/` from source).
2. Copy the ones you want into a folder of their own and edit or upscale them. Keep each file name; any size works
   (mipmaps are made on the GPU).
3. Choose that folder as the texture pack folder (the `.png` files directly in it, not in subfolders) and untick
   **Dump textures**. The launcher shows how many replacements it found; the log line `[gpu] texture pack: ...
   replacements loaded` confirms it in game.

Texture packs need the Hardware renderer.
## Controls, skipping movies, quitting

See [Controls](../README.md#controls) in the README. In short: Enter, Space, a click or gamepad Start/Cross skips a
movie; Esc asks before quitting; Alt+F4, the close button or holding gamepad Back+Start quits. The game saves only at
its own save points.

**My custom controls don't work.** Before v1.3.0 a key you gave to a button stayed on its default button too (WASD
for moving also blocked with W and attacked with A and S). v1.3.0 moves the key and fixes older settings when the
game starts; open *Controls…* once to see the result. The game's log names a key that is on two buttons.

## Bringing saves over from PPSSPP

Options > Advanced > *Import PPSSPP saves…*, then choose PPSSPP's `memstick` folder (the one with `PSP\SAVEDATA`).
The saves of both games are copied next to this port's own. PPSSPP encrypts saves by default, and the port reads only
unencrypted ones: turn off PPSSPP's save encryption (with PPSSPP closed, set `EncryptSave = False` in
`memstick\PSP\SYSTEM\ppsspp.ini`), load your save in PPSSPP and save again, then import it.

## Reporting a bug

See [Reporting a bug](../README.md#reporting-a-bug): attach the log and input files listed above, never game files,
saves or screenshots of the game.
