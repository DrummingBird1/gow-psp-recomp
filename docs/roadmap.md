# Roadmap

Where the project is going, from the requests on GitHub (issues, discussions, pull requests) and the project's own
list. Updated for v1.6.0 (November 2026). Ideas and requests are welcome in
[Discussions](https://github.com/DrummingBird1/gow-psp-recomp/discussions).

## Shipped

The details are in the [changelog](../CHANGELOG.md).

| Version | Highlights | Asked in |
|---|---|---|
| v1.6.0 | The Vortex no longer loops forever; builds in folders with [ ] in their name; European releases (preview); Auto QTE for Chains of Olympus; the launcher in Turkish; xBRZ texture upscaling | #51, #56, #61, #62, #65, #66, #6, #64, #26, #60 |
| v1.5.0 | Keypad keys can be bound; a damaged libchdr download is fetched again; the mouse cursor hides; a freeze leaves a trace in the log; build provenance for the downloads | #57, #46, #44, #52, #54 |
| v1.4.0 | Crash in the Depths of Tartarus and slow motion in Ghost of Sparta fixed; V-Sync above the display's refresh; spin keys and turbo for the prompts; the buttons pressed on screen; HDR Auto and a software fallback without a GPU device; a new look and five more launcher languages | #24, #33, #35, #34, #26, #31, #28, #27, #29 |
| v1.3.0 | Custom controls and mouse buttons; right stick evades; frame rate counter; movie skip; Ghost of Sparta's languages; 4:3 and a CRT filter; separate volumes; PPSSPP save import; the Light preset; rumble; AI texture upscaling; the launcher in Spanish, French and Hebrew | #3, #4, #12, Discussions #19, #20 |

## Next

- **Ghost of Sparta's busiest scenes at 60 FPS on more PCs** (#33, #63). Measured in The Vortex: the game's graphics
  commands are turned into PC draws on the game's own thread, 11 to 17 ms per frame, over about 7000 draws; about 99%
  of the vertices are in draws of fewer than 64. Next: doing that work in parallel with the game, and fewer, larger
  draws.
- **The European releases confirmed on a real disc** (#6, #64), then out of preview.
- **The Vortex fix checked in a full playthrough** and with players' saves.
- **SMAA**, a sharper alternative to FXAA.
- **Above 60 FPS without small hitches** (#3): the game's own frame pacing at 120+.
- **Xbox button prompts** in the game: replacement textures for the button icons, drawn from scratch (no art from
  the games), loaded like a texture pack.

## Later (v2.0)

- **A full playthrough of both games**, start to end credits. This is what turns the preview into a release.
- **Linux and Steam Deck.** SDL3's GPU API already has a Vulkan backend; the runtime, the build and the launcher
  need porting.
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

## Measurements behind the presets

GPU time per frame at each internal resolution, on Chains of Olympus gameplay (frames 4700-5400 of the reference
input, headless, `RT_GPU_SYNC=1`, `gpu_ms` of `--perf-csv`):

| GPU | x1 | x2 | x4 | x6 |
|---|---|---|---|---|
| Intel UHD 770 (integrated) | 8.2 ms | 13.8 ms | 21.3 ms | 37.6 ms |
| RTX 4070 SUPER | 1.1 ms | 1.0 ms | 1.4 ms | 1.9 ms |

60 FPS needs under 16.7 ms, so integrated GPUs start on the Light preset (x2).
