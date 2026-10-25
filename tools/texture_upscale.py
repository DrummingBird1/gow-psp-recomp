"""AI upscaling of the textures "Dump textures" wrote, into a texture pack, on the player's own PC.

The colour goes through Real-ESRGAN (realesrgan-ncnn-vulkan, BSD-3-Clause, downloaded once from its official release
and checked by SHA-256, after asking); the alpha channel is scaled with Lanczos and put back (the model works on
colour). Textures under 16 pixels on a side make that build lose the Vulkan device (vkQueueSubmit -4, on an RTX 4070
SUPER), and every texture after them in the same run with it, so they get Lanczos; so does any result that no longer
matches its original (MIN_PSNR). The pack is always complete. Nothing leaves the PC.

Usage: python tools/texture_upscale.py DUMP_DIR PACK_DIR [--tool-dir DIR]
"""
import argparse
import math
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain  # noqa: E402

REALESRGAN = {"name": "Real-ESRGAN ncnn Vulkan 0.2.5.0", "dir": "realesrgan",
              "url": "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/"
                     "realesrgan-ncnn-vulkan-20220424-windows.zip",
              "sha256": "abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d", "size": 45474481, "skip": ()}
MODEL = "realesrgan-x4plus"
# a texture narrower than this loses the Vulkan device (and with it every texture after it in the same run): Lanczos
MIN_SIDE = 16
MIN_PSNR = 18.0  # the AI result scaled back to 1x must stay this close to the original (dB), else Lanczos


def psnr(a, b):
    """PSNR (dB) of the colour of two same-size RGBA pictures over a's visible pixels."""
    pa, pb = a.tobytes(), b.tobytes()
    s = n = 0
    for i in range(0, len(pa), 4):
        if pa[i + 3]:
            for c in range(3):
                d = pa[i + c] - pb[i + c]
                s += d * d
            n += 3
    return 99.0 if not n or not s else 10 * math.log10(255 * 255 * n / s)


def consent_text():
    return (f"Upscaling the dumped textures uses Real-ESRGAN, an AI upscaler. Download it now (about "
            f"{REALESRGAN['size'] / 1e6:.0f} MB, once) from its official release on GitHub ({REALESRGAN['url']})? It runs "
            "on your graphics card; the textures never leave your PC.")


def find_exe(tool_dir):
    d = Path(tool_dir) / REALESRGAN["dir"]
    hits = list(d.rglob("realesrgan-ncnn-vulkan.exe")) if d.is_dir() else []
    return hits[0] if hits else None


def upscale(dump_dir, pack_dir, tool_dir, scale=4, out=print):
    """Upscales every <key>.png of dump_dir into pack_dir (same names). Returns (by the AI, by Lanczos)."""
    from PIL import Image
    src = sorted(p for p in Path(dump_dir).glob("*.png"))
    if not src:
        out(f"no dumped textures in {dump_dir}: play with \"Dump textures\" on first")
        return 0, 0
    exe = find_exe(tool_dir)
    pack = Path(pack_dir)
    pack.mkdir(parents=True, exist_ok=True)
    ai = lanczos = 0
    with tempfile.TemporaryDirectory() as tmp:
        rgb_in, rgb_out = Path(tmp, "in"), Path(tmp, "out")
        rgb_in.mkdir()
        rgb_out.mkdir()
        for p in src:
            im = Image.open(p)
            if min(im.size) >= MIN_SIDE:
                im.convert("RGB").save(rgb_in / p.name)
        if exe:
            out(f"upscaling {len(src)} textures x{scale} with Real-ESRGAN")
            r = subprocess.run([str(exe), "-i", str(rgb_in), "-o", str(rgb_out), "-n", MODEL, "-s", str(scale), "-f", "png"],
                               cwd=exe.parent, capture_output=True, text=True, errors="replace",
                               creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            if r.returncode:
                out(f"Real-ESRGAN failed (exit {r.returncode}): scaling with Lanczos instead")
        for p in src:
            im = Image.open(p).convert("RGBA")
            size = (im.width * scale, im.height * scale)
            up = rgb_out / p.name
            colour = None
            if up.is_file():
                try:
                    colour = Image.open(up).convert("RGB")
                    if colour.size != size or psnr(im, colour.convert("RGBA").resize(im.size, Image.Resampling.BOX)) < MIN_PSNR:
                        colour = None  # wrong size, or not the same picture any more: a failed run
                except OSError:
                    colour = None
            if colour is None:
                colour = im.convert("RGB").resize(size, Image.Resampling.LANCZOS)
                lanczos += 1
            else:
                ai += 1
            colour.putalpha(im.getchannel("A").resize(size, Image.Resampling.LANCZOS))
            colour.save(pack / p.name)
    out(f"texture pack in {pack}: {ai} upscaled by the AI, {lanczos} by Lanczos")
    return ai, lanczos


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("pack")
    ap.add_argument("--tool-dir", default=str(Path(__file__).resolve().parent.parent / "build" / "tools"))
    ap.add_argument("--scale", type=int, default=4, choices=(4,))  # the x4plus model scales by 4
    a = ap.parse_args()
    if not find_exe(a.tool_dir) and not toolchain.download_archive(REALESRGAN, a.tool_dir):
        print("Real-ESRGAN is not there: scaling with Lanczos only")
    ai, lz = upscale(a.dump, a.pack, a.tool_dir, a.scale)
    return 0 if ai + lz else 1


if __name__ == "__main__":
    sys.exit(main())
