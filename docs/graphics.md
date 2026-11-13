# Graphics guide

What the launcher's graphics options do and which to pick. All of them apply to the **Hardware** renderer (the
default). Changing any option switches the preset to *Custom*; the game picks the change up at the next Play.

## Presets

| Preset | Internal resolution | MSAA | Anisotropic | For |
|---|---|---|---|---|
| Original | x1 (480x272, the PSP's) | off | off | the look of the PSP |
| Light | x2 (960x544) | off | 4x | integrated graphics, older PCs |
| Balanced | x4 (1920x1088) | off | 8x | most PCs with a dedicated GPU |
| High | x6 (2880x1632) | 4x | 4x | a recent dedicated GPU |
| Ultra | x8 (3840x2176, 4K) | 4x | 4x | a fast GPU and a 4K display |

The launcher starts on **High** when it finds a dedicated GPU and on **Light** otherwise, at 60 FPS. On an Intel UHD
770, x4 takes about 21 ms per frame (too slow for 60 FPS) and x2 about 14 ms.

## Options

- **Resolution** (x1 to x8): the size the game's 3D is drawn at, as a multiple of the PSP's 480x272. The biggest effect
  on both sharpness and GPU load.
- **Anti-aliasing:** MSAA 2x, 4x or 8x smooths the edges of polygons; FXAA is a cheap filter on the finished picture,
  useful at x1 or x2 without MSAA.
- **Frame rate:** 60 (original), 120, 144, 165, 240 or Unlimited (for a VRR display). Both games step by elapsed time, so
  the game runs at the right speed at any rate. If the PC can't hold the rate every frame, the game skips ahead to keep
  time and the sound can crackle: then choose 60 or a lower preset.
- **V-Sync:** the most even frame pacing on a 60 Hz display.
- **Aspect ratio:** Original (16:9 like the PSP), 21:9 and 32:9 (a wider field of view, not a stretched picture), or 4:3
  (crops the sides).
- **HDR:** Auto uses HDR10 when Windows has HDR on for the display; the brightness follows Windows' HDR settings.
- **Window size / Fullscreen:** the window's size when not in fullscreen. F11 or Alt+Enter switches while playing.
- **CRT filter:** scanlines, for the look of an old screen.

### Advanced

- **Renderer:** *Hardware* draws on the GPU at any resolution. *Software* is the accurate reference: slow, at the PSP's
  own resolution and without the options on this page. Use it to check whether a graphics bug comes from the hardware
  renderer. When the PC has no Direct3D 12 or Vulkan device, the game falls back to it by itself.
- **Effects resolution:** *Match resolution* draws the glow and blur effects at the chosen resolution; *Original glow*
  keeps them at the PSP's size, softer, as on the console.
- **Anisotropic filtering:** keeps textures sharp on surfaces seen at an angle (floors, walls). Cheap on most GPUs.
- **Texture upscaling (xBRZ):** smooths the game's textures to 2, 3 or 4 times their size, as in PPSSPP. It suits
  flat and drawn textures better than photographic ones, and costs time the first time each texture is seen.
- **Sharpening:** a sharpening filter on the finished picture, for x1 or x2 in a big window, or over a texture pack.
- **Graphics API:** Direct3D 12 (the default) or Vulkan; try the other one if the game doesn't start or draws wrongly.
- **GPU:** on a laptop with two GPUs, *Fast* uses the dedicated one and *Power saving* the integrated one.

## Texture packs

1. Turn on **Dump textures (to make a pack)** and play: each texture the game shows is written once as a PNG.
2. Make the pack: either **Upscale textures**, which runs Real-ESRGAN (an AI upscaler, downloaded once) over the dumped
   textures on your PC, or edit or upscale the PNGs yourself with any tool, keeping their names.
3. Choose the pack's folder in **Texture pack folder**. Textures are replaced at any size; the others stay as they are.

Textures come from your disc, so packs stay on your PC: don't share them.

## If the game is slow

- Press F3 in the game to see the frame rate.
- Lower the preset (Light) and choose 60 FPS. The resolution matters most on integrated graphics.
- Ghost of Sparta's busiest scenes are limited by the CPU, not the GPU: a lower preset helps less there. Improving this is
  [planned](roadmap.md).
- More in [Troubleshooting and FAQ](troubleshooting.md).

The technical notes behind these options (in Hebrew) are in [phase 13](dev-history/phase13-graphics.md).
