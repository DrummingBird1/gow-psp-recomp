"""Draws the repository's social preview (docs/images/social-preview.png, 1280x640): the original helmet icon
(make_icon.py) and the title in Cinzel on a stone background, in the launcher's colours. Original art only: no game
images or official logos. Upload it by hand under Settings > Social preview (GitHub's API cannot set it)."""
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

from make_icon import helmet

ROOT = Path(__file__).resolve().parent.parent
FONT = ROOT / "tools" / "launcher_assets" / "Cinzel[wght].ttf"
OUT = ROOT / "docs" / "images" / "social-preview.png"
W, H = 1280, 640
GOLD, GOLD_DK, RED, TEXT2 = (201, 162, 39), (107, 90, 36), (179, 38, 30), (190, 186, 176)


def font(size, weight):
    f = ImageFont.truetype(str(FONT), size)
    f.set_variation_by_axes([weight])
    return f


def stone():
    rnd = random.Random(7)  # fixed seed: the same image every run
    small = Image.new("L", (W // 8, H // 8))
    small.putdata([rnd.randint(18, 40) for _ in range(small.width * small.height)])
    base = small.resize((W, H), Image.BICUBIC).filter(ImageFilter.GaussianBlur(6))
    fine = Image.effect_noise((W, H), 10).filter(ImageFilter.GaussianBlur(0.8))
    lum = Image.blend(base, fine.point(lambda v: 14 + v // 10), 0.35)
    im = Image.merge("RGB", (lum, lum.point(lambda v: v * 0.97), lum.point(lambda v: v * 0.93)))
    glow = Image.new("L", (W, H), 0)
    ImageDraw.Draw(glow).ellipse((60, 90, 520, 550), fill=120)
    glow = glow.filter(ImageFilter.GaussianBlur(90))
    return Image.composite(Image.new("RGB", (W, H), RED), im, glow.point(lambda v: v // 2))


def meander(d, y, step=16):
    """A gold Greek-key band across the image at height y (like the launcher's frame)."""
    d.line((0, y - step, W, y - step), fill=GOLD_DK, width=2)
    d.line((0, y + step, W, y + step), fill=GOLD_DK, width=2)
    for x in range(-step, W + step, step * 2):
        pts = [(x, y + step // 2), (x, y - step // 2), (x + step, y - step // 2), (x + step, y + step // 4),
               (x + step // 2, y + step // 4), (x + step // 2, y), ]
        d.line(pts, fill=GOLD, width=3)
        d.line((x + step, y + step // 2, x + 2 * step, y + step // 2), fill=GOLD, width=3)


def main():
    im = stone()
    d = ImageDraw.Draw(im)
    meander(d, 34)
    meander(d, H - 34)
    icon = helmet().resize((330, 330), Image.LANCZOS)
    im.paste(icon, (110, (H - 330) // 2), icon)
    x = 500
    title1, title2 = "God of War PSP", "native PC ports"
    f1, f2, f3 = font(76, 700), font(46, 600), font(25, 500)
    d.text((x, 190), title1, font=f1, fill=GOLD)
    d.text((x, 290), "\u2014 " + title2, font=f2, fill=(240, 239, 236))
    d.line((x, 372, W - 90, 372), fill=GOLD_DK, width=2)
    d.text((x, 392), "Chains of Olympus \u00b7 Ghost of Sparta", font=f3, fill=TEXT2)
    d.text((x, 432), "static recompilation \u00b7 bring your own disc", font=f3, fill=TEXT2)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    im.quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG).save(OUT, optimize=True)
    print("wrote", OUT, im.size)


if __name__ == "__main__":
    main()