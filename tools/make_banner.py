"""Draws the README's banner (docs/images/banner.png, 1280x300): the original helmet icon (make_icon.py) and the
title in Cinzel on the stone background of the social preview, between two meander bands. Original art only: no game
images or official logos."""
from pathlib import Path

from PIL import Image, ImageDraw

from make_icon import helmet
from make_social_preview import GOLD, GOLD_DK, TEXT2, font, meander, stone

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "images" / "banner.png"
W, H = 1280, 300


def main():
    im = stone(W, H, glow_box=(90, 30, 330, 270))
    d = ImageDraw.Draw(im)
    meander(d, 22, step=12, w=W)
    meander(d, H - 22, step=12, w=W)
    size = 190
    icon = helmet().resize((size, size), Image.LANCZOS)
    im.paste(icon, (120, (H - size) // 2), icon)
    x = 370
    d.text((x, 78), "God of War PSP", font=font(64, 700), fill=GOLD)
    d.text((x, 160), "native PC ports (unofficial)", font=font(34, 600), fill=(240, 239, 236))
    d.line((x, 214, W - 110, 214), fill=GOLD_DK, width=2)
    d.text((x, 226), "Chains of Olympus · Ghost of Sparta · bring your own disc", font=font(21, 500), fill=TEXT2)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    im.quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG).save(OUT, optimize=True)
    print("wrote", OUT, im.size)


if __name__ == "__main__":
    main()
