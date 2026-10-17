"""Draws the project's own app icon (original art: a Spartan helmet in the launcher's red and gold; nothing from
the games) into tools/launcher_assets/icon.ico (16-256 px) and icon.png (256 px), and runtime/gow_rt.ico for the
game executables.

    .venv/Scripts/python.exe tools/make_icon.py
"""
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

OUT = Path(__file__).resolve().parent / "launcher_assets"
RED, RED_DK, GOLD, GOLD_DK, GOLD_LT, BG = (179, 38, 30), (110, 18, 14), (201, 162, 39), (128, 96, 18), (238, 206, 110), (20, 15, 16)
S = 1024


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def helmet():
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    # background: dark rounded square with a red glow behind the helmet
    bg = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(bg).rounded_rectangle((16, 16, S - 16, S - 16), 190, fill=BG + (255,))
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((170, 230, 854, 914), fill=RED + (150,))
    glow = glow.filter(ImageFilter.GaussianBlur(110))
    mask = bg.split()[3]
    bg.alpha_composite(Image.composite(glow, Image.new("RGBA", (S, S)), mask))
    im.alpha_composite(bg)

    d = ImageDraw.Draw(im)
    # crest: a red arch of plumes over the helmet
    for k in range(34):
        a = math.pi * (0.06 + 0.88 * k / 33)
        x, y = 512 - math.cos(a) * 380, 500 - math.sin(a) * 380
        x2, y2 = 512 - math.cos(a) * 230, 500 - math.sin(a) * 230
        col = lerp(RED, RED_DK, (k % 2) * 0.45)
        d.line((x2, y2, x, y), fill=col + (255,), width=40)
    
    # helmet: gold dome and cheek guards
    body = Image.new("L", (S, S), 0)
    bd = ImageDraw.Draw(body)
    bd.ellipse((252, 270, 772, 790), fill=255)
    bd.polygon([(252, 520), (772, 520), (760, 760), (700, 900), (612, 950), (412, 950), (324, 900), (264, 760)], fill=255)
    shade = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shade)
    for y in range(290, 940):
        t = (y - 290) / 650
        sd.line((0, y, S, y), fill=lerp(GOLD_LT, GOLD_DK, min(1, t * 1.15)) + (255,))
    im.paste(shade, (0, 0), body)
    d = ImageDraw.Draw(im)
    d.ellipse((252, 270, 772, 790), outline=GOLD_DK + (255,), width=10)
    # brow ridge
    d.arc((290, 400, 734, 690), 198, 342, fill=GOLD_DK + (255,), width=18)
    # face opening: two swept eye slits beside the nose guard, and the gap between the cheek guards
    d.polygon([(290, 590), (400, 528), (488, 540), (494, 650), (440, 690), (350, 660)], fill=BG + (255,))
    d.polygon([(734, 590), (624, 528), (536, 540), (530, 650), (584, 690), (674, 660)], fill=BG + (255,))
    d.polygon([(482, 700), (542, 700), (580, 956), (444, 956)], fill=BG + (255,))
    # rim highlight
    d.arc((272, 290, 752, 770), 205, 300, fill=GOLD_LT + (200,), width=8)
    return im


def main():
    im = helmet()
    sizes = [16, 24, 32, 48, 64, 128, 256]
    big = im.resize((256, 256), Image.LANCZOS)
    big.save(OUT / "icon.png", optimize=True)
    big.save(OUT / "icon.ico", sizes=[(s, s) for s in sizes])
    big.save(OUT.parent.parent / "runtime" / "gow_rt.ico", sizes=[(s, s) for s in sizes])  # the game exe (gow_rt.rc)
    print("wrote", OUT / "icon.ico", OUT / "icon.png")


if __name__ == "__main__":
    main()