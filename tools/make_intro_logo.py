"""Builds the intro logo of this mod: the title is drawn into the dark sky of the artwork.

usage: python make_intro_logo.py <artwork.png> <out_dir>   (needs Pillow)

Writes <out_dir>/ModIntroLogo.tga (2048x1024, the texture the game loads; the picture is drawn back at
its own aspect ratio, see Intro.cpp) and <out_dir>/ModIntroLogo_preview.png (at the aspect ratio of the artwork).
"""
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

FONTS = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")
W, H = 1820, 1024  # working size, aspect of the artwork (16:9)
CX = W // 2


def font(name, size):
    return ImageFont.truetype(os.path.join(FONTS, name), size)


def text_mask(text, fnt, spacing=0):
    """White text on black, with extra letter spacing, cropped to the text."""
    widths = [fnt.getbbox(c)[2] if c != " " else fnt.getbbox("n")[2] for c in text]
    width = sum(widths) + spacing * (len(text) - 1) + 40
    asc, desc = fnt.getmetrics()
    mask = Image.new("L", (width, asc + desc + 40), 0)
    d = ImageDraw.Draw(mask)
    x = 20
    for c, w in zip(text, widths):
        d.text((x, 20), c, font=fnt, fill=255)
        x += w + spacing
    return mask.crop(mask.getbbox())


def vertical_gradient(size, stops):
    """stops: list of (position 0..1, (r, g, b))"""
    w, h = size
    grad = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
            if p0 <= t <= p1:
                f = (t - p0) / max(1e-6, p1 - p0)
                grad.putpixel((0, y), tuple(int(a + (b - a) * f) for a, b in zip(c0, c1)))
                break
    return grad.resize((w, h))


def place(canvas, layer, mask, cx, top):
    x = cx - mask.width // 2
    canvas.paste(layer, (x, top), mask)


def shadow(canvas, mask, cx, top, radius, offset, opacity):
    sh = Image.new("L", canvas.size, 0)
    sh.paste(mask, (cx - mask.width // 2 + offset, top + offset))
    sh = sh.filter(ImageFilter.GaussianBlur(radius)).point(lambda v: int(v * opacity))
    canvas.paste(Image.new("RGB", canvas.size, (0, 0, 0)), (0, 0), sh)


def glow(canvas, mask, cx, top, radius, color, opacity):
    g = Image.new("L", canvas.size, 0)
    g.paste(mask, (cx - mask.width // 2, top))
    g = g.filter(ImageFilter.GaussianBlur(radius)).point(lambda v: min(255, int(v * opacity)))
    lit = ImageChops.screen(canvas, Image.new("RGB", canvas.size, color))
    canvas.paste(lit, (0, 0), g)


def metal(mask, stops, bevel=3):
    """Gradient filled letters with a light upper and a dark lower edge."""
    fill = vertical_gradient(mask.size, stops)
    up = ImageChops.subtract(mask, ImageChops.offset(mask, 0, bevel))
    down = ImageChops.subtract(mask, ImageChops.offset(mask, 0, -bevel))
    fill.paste(Image.new("RGB", mask.size, (255, 255, 255)), (0, 0), up.point(lambda v: v * 0.6))
    fill.paste(Image.new("RGB", mask.size, (20, 20, 24)), (0, 0), down.point(lambda v: v * 0.7))
    return fill


def outline(mask, width):
    return mask.filter(ImageFilter.MaxFilter(width * 2 + 1))


def main(src, out_dir):
    art = Image.open(src).convert("RGB").resize((W, H), Image.LANCZOS)

    # deepen the dark sky behind the title so it reads on any screen
    shade = Image.new("L", (W, H), 0)
    ImageDraw.Draw(shade).ellipse((CX - 520, -260, CX + 520, 560), fill=170)
    shade = shade.filter(ImageFilter.GaussianBlur(90))
    art.paste(Image.new("RGB", (W, H), (0, 0, 0)), (0, 0), shade)

    top = 46
    # studio
    m = text_mask("JEAN-BÉDEL BOKASSA PRODUCTIONS", font("bahnschrift.ttf", 27), spacing=6)
    shadow(art, m, CX, top, 4, 2, 0.9)
    place(art, metal(m, [(0, (240, 226, 190)), (1, (176, 150, 100))], bevel=1), m, CX, top)
    top += m.height + 16

    # presents
    m = text_mask("presents", font("georgiai.ttf", 25), spacing=2)
    place(art, Image.new("RGB", m.size, (205, 200, 190)), m, CX, top)
    top += m.height + 34

    # COMMAND & CONQUER
    m = text_mask("COMMAND & CONQUER", font("bahnschrift.ttf", 40), spacing=9)
    shadow(art, m, CX, top, 5, 3, 1.0)
    place(art, metal(m, [(0, (250, 250, 252)), (0.5, (170, 176, 186)), (1, (110, 116, 128))], bevel=1), m, CX, top)
    top += m.height + 18

    # GENERALS: brushed gunmetal steel with a dark rim
    m = text_mask("GENERALS", font("ariblk.ttf", 126), spacing=4)
    rim = outline(m, 4)
    shadow(art, rim, CX, top, 12, 6, 1.0)
    place(art, Image.new("RGB", rim.size, (18, 20, 24)), rim, CX, top - 4 + (m.height - rim.height) // 2 + 4)
    steel = metal(m, [(0, (236, 240, 246)), (0.42, (150, 158, 170)), (0.5, (86, 92, 104)), (1, (176, 184, 196))], bevel=4)
    place(art, steel, m, CX, top)
    top += m.height + 22

    # ZERO HOUR: molten orange with a fiery glow
    m = text_mask("ZERO HOUR", font("impact.ttf", 104), spacing=10)
    glow(art, m, CX, top, 22, (255, 90, 10), 1.6)
    rim = outline(m, 3)
    place(art, Image.new("RGB", rim.size, (40, 6, 0)), rim, CX, top - 3)
    place(art, metal(m, [(0, (255, 236, 150)), (0.45, (255, 150, 30)), (1, (200, 30, 10))], bevel=2), m, CX, top)
    top += m.height + 30

    # REIMAGINED: clean, wide and light, between two thin lines
    m = text_mask("REIMAGINED", font("bahnschrift.ttf", 40), spacing=22)
    glow(art, m, CX, top, 10, (255, 200, 120), 0.8)
    place(art, metal(m, [(0, (255, 250, 236)), (1, (232, 200, 140))], bevel=1), m, CX, top)
    d = ImageDraw.Draw(art)
    y = top + m.height // 2
    for side in (-1, 1):
        x0 = CX + side * (m.width // 2 + 26)
        x1 = CX + side * (m.width // 2 + 150)
        d.line((x0, y, x1, y), fill=(232, 200, 140), width=2)

    os.makedirs(out_dir, exist_ok=True)
    art.save(os.path.join(out_dir, "ModIntroLogo_preview.png"))
    art.resize((2048, 1024), Image.LANCZOS).save(os.path.join(out_dir, "ModIntroLogo.tga"))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
