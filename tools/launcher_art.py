"""Original artwork for the launcher, drawn by code at run time (nothing here comes from the games):
a dark stone texture made from noise, a gold Greek-key (meander) band, rounded cards and a red glow."""
import random

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

BG = (18, 18, 18)
GOLD = (201, 162, 39)
RED = (179, 38, 30)


def stone(w, h, seed=7):
    """Dark marble/stone: two octaves of noise, soft veins, a vignette."""
    random.seed(seed)
    fine = Image.effect_noise((w, h), 40).filter(ImageFilter.GaussianBlur(1.2))
    coarse = Image.effect_noise((max(1, w // 24), max(1, h // 24)), 70).resize((w, h), Image.BICUBIC)
    veins = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(veins)
    for _ in range(max(6, w * h // 60000)):
        x, y = random.uniform(0, w), random.uniform(0, h)
        pts = []
        for _ in range(14):
            pts.append((x, y))
            x += random.uniform(-w / 10, w / 10)
            y += random.uniform(-h / 18, h / 18)
        d.line(pts, fill=random.randint(18, 40), width=random.randint(1, 3))
    veins = veins.filter(ImageFilter.GaussianBlur(2.5))
    mix = ImageChops.add(Image.blend(fine, coarse, 0.6), veins)
    # map 0..255 to a dark warm grey
    lut_r = [int(14 + v * 0.11) for v in range(256)]
    lut_g = [int(14 + v * 0.105) for v in range(256)]
    lut_b = [int(13 + v * 0.095) for v in range(256)]
    img = Image.merge("RGB", (mix.point(lut_r), mix.point(lut_g), mix.point(lut_b)))
    vign = Image.radial_gradient("L").resize((w, h)).point(lambda v: int(255 - v * 0.55))
    return Image.composite(img, Image.new("RGB", (w, h), (6, 6, 6)), vign)


def meander(w, cell=3, color=GOLD, ss=4):
    """A horizontal Greek-key band, w pixels wide (cell = pixels per grid step), RGBA."""
    w = max(8, int(w))
    c = cell * ss
    h = 9 * c
    img = Image.new("RGBA", (w * ss, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    lw = max(1, c - c // 3)
    d.line([(0, c // 2), (w * ss, c // 2)], fill=color + (255,), width=max(1, lw // 2))
    d.line([(0, h - c // 2), (w * ss, h - c // 2)], fill=color + (255,), width=max(1, lw // 2))
    unit = 8 * c
    for x0 in range(-unit, w * ss + unit, unit):
        pts = [(x0, 7 * c), (x0, 2 * c), (x0 + 6 * c, 2 * c), (x0 + 6 * c, 6 * c), (x0 + 3 * c, 6 * c), (x0 + 3 * c, 4 * c),
               (x0 + 4 * c, 4 * c)]
        d.line(pts, fill=color + (255,), width=lw, joint="curve")
        d.line([(x0, 7 * c), (x0 + unit, 7 * c)], fill=color + (255,), width=lw)
    return img.resize((w, h // ss), Image.LANCZOS)


def rounded_mask(w, h, r, ss=3):
    m = Image.new("L", (w * ss, h * ss), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, w * ss - 1, h * ss - 1), r * ss, fill=255)
    return m.resize((w, h), Image.LANCZOS)


def glow(w, h, color=RED, radius=26):
    """A soft glow (RGBA) of size w x h with a rounded core in the middle."""
    img = Image.new("RGBA", (w, h), color + (0,))
    a = Image.new("L", (w, h), 0)
    p = radius + 6
    ImageDraw.Draw(a).rounded_rectangle((p, p, w - p, h - p), radius, fill=170)
    img.putalpha(a.filter(ImageFilter.GaussianBlur(radius * 0.8)))
    return img


def font(path, size, bold=False):
    f = ImageFont.truetype(str(path), size)
    if bold:
        try:
            f.set_variation_by_name("Bold")
        except (OSError, ValueError, AttributeError):
            pass
    return f
