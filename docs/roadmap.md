# Roadmap

Where the project is going, from the requests on GitHub (issues, discussions, pull requests) and the project's own
list. Updated for v1.3.0 (October 2026). Ideas and requests are welcome in
[Discussions](https://github.com/DrummingBird1/gow-psp-recomp/discussions).

## v1.3.0: what the requests asked for

| Change | Asked in |
|---|---|
| Custom controls work: a key goes to one button only (WASD no longer also blocks and attacks) | #3 |
| Mouse buttons can be bound | #3 |
| Right stick evades (L + R + stick), as in the PS2 games | project |
| Choice of graphics card on PCs with two | #3 |
| Frame rate counter (F3) | #3 ("benchmark toggle") |
| Skip the movies | #3 ("intro skip") |
| Language: English, French, Spanish (Ghost of Sparta's US disc) | Discussion #19 |
| 4:3 aspect ratio and a CRT filter | Discussion #20 |
| Music and effects volume, separately | project |
| Infinite health (cheat) | project |
| Import PPSSPP saves | project |
| Light preset (x2), the start on PCs without a dedicated GPU | #3 (x3/x4 stutter) |
| Gamepad rumble when Kratos is hit | project |
| Circle confirms, Cross cancels (Japanese layout) | project |
| Separate graphics settings for each game | project |
| AI upscaling of dumped textures into a pack (Real-ESRGAN, on the player's PC) | #4 (texture packs) |
| Discord Rich Presence (off by default) | project |
| The launcher in Spanish, French and Hebrew | Discussion #19 |
| HUD at the screen edges at 21:9 / 32:9 (experimental) | #12 |
| 32:9 (already there since v1.2.0: Options > Aspect ratio) | #12 |

The changes are in the [changelog](../CHANGELOG.md).

## Next (v1.4)

- **Playtest the new options** on more PCs: the 4:3 HUD, the HUD at the edges, the right-stick evade and the rumble
  in combat, the languages. The HUD at the edges leaves the experimental label once text no longer comes apart.
- **High internal resolutions on integrated GPUs** (#3: x3/x4 stutter). The GPU time per frame grows with the
  resolution; measured on Chains of Olympus gameplay (frames 4700-5400 of the reference input, headless,
  `RT_GPU_SYNC=1`, `gpu_ms` of `--perf-csv`, one run each on a PC that had other work running):

  | GPU | x1 | x2 | x4 | x6 |
  |---|---|---|---|---|
  | Intel UHD 770 (integrated) | 8.2 ms | 13.8 ms | 21.3 ms | 37.6 ms |
  | RTX 4070 SUPER | 1.1 ms | 1.0 ms | 1.4 ms | 1.9 ms |

  Average GPU milliseconds per frame; 60 FPS needs under 16.7. So x4 (the Balanced preset, which v1.2 chose on PCs
  without a dedicated GPU) can't hold 60 FPS on an integrated GPU. v1.3.0 starts such PCs on the new **Light**
  preset (x2). The effects at 1x (*Effects resolution: Original glow*) save only about 8% at x4 on the Intel GPU
  (25.2 to 23.3 ms in a second run) but halve the memory of the render targets (155 to 70 MB), so the glow and
  shadow passes are not the main cost. Next: per-pass GPU timestamps, starting with the render-to-texture snapshots
  (copies of a target that a later draw samples) and the per-draw state changes.
- **Above 60 FPS without small hitches** (#3): the game's own frame pacing at 120+.
- **Xbox button prompts** in the game: replacement textures for the button icons, drawn from scratch (no art from
  the games), loaded like a texture pack.

## Later (v2.0)

- **A full playthrough of both games**, start to end credits. This is what turns the preview into a release.
- **Linux and Steam Deck.** SDL3's GPU API already has a Vulkan backend; the runtime, the build and the launcher
  need porting.
- **The European (PAL) discs** (#6). Each release has its own executable, so each must be recompiled and checked
  like the US ones; they bring more languages.
- **Android**, together with the contributors of #7-#13: the runtime parts first, and the game code built on the
  device from the player's disc (no game code in an APK or in CI).
- Rumble on QTEs too, higher-resolution shadows, more launcher languages (help texts and dialogs as well).

## Needs the maintainer's decision

- **Code signing** of the launcher (SmartScreen warnings): a certificate costs money every year.
- **winget / Scoop** packages: each needs an account and an update per release.

## Not planned

- **Save states** (save anywhere): the recompiled game runs on host threads and fibers whose state can't be written
  out and restored reliably. The game's own save points work.
- **Game files in any download**: the game code is generated from your disc and stays on your PC.
