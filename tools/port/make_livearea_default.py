#!/usr/bin/env python3
"""Default LiveArea artwork of the repository (original, no game assets):
a cut gem (sapphire blue / ruby red) on a dark gradient with the title.
Your own artwork goes to platform/vita/sce_sys_local_<game>/ instead
(see tools/port/make_livearea_local.py).
usage: make_livearea_default.py
"""
import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
GAMES = {
    "sapphire": ("Saphir", (40, 110, 255), (10, 20, 60), (170, 210, 255)),
    "ruby": ("Rubin", (235, 40, 60), (50, 8, 16), (255, 190, 190)),
}


def gradient(w, h, top, bottom):
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    return img.convert("RGBA")


def gem(size, color):
    """A brilliant-cut gem seen from the side, lit from the top left."""
    s = size
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    top, girdle, bottom = s * 0.18, s * 0.42, s * 0.92
    l, r, tl, tr = s * 0.06, s * 0.94, s * 0.28, s * 0.72
    shade = lambda k: tuple(max(0, min(255, int(c * k))) for c in color) + (255,)
    facets = [
        ([(tl, top), (tr, top), (s * 0.62, girdle), (s * 0.38, girdle)], 1.35),
        ([(l, girdle), (tl, top), (s * 0.38, girdle)], 1.15),
        ([(r, girdle), (tr, top), (s * 0.62, girdle)], 0.8),
        ([(l, girdle), (s * 0.38, girdle), (s / 2, bottom)], 0.95),
        ([(s * 0.38, girdle), (s * 0.62, girdle), (s / 2, bottom)], 1.2),
        ([(s * 0.62, girdle), (r, girdle), (s / 2, bottom)], 0.6),
    ]
    for poly, k in facets:
        d.polygon(poly, fill=shade(k))
    for poly, _ in facets:
        d.line(poly + [poly[0]], fill=(255, 255, 255, 90), width=max(1, s // 90))
    d.polygon([(tl + s * 0.04, top + s * 0.03), (s * 0.45, top + s * 0.03), (s * 0.36, girdle - s * 0.04)],
              fill=(255, 255, 255, 120))
    return img


def text(img, xy, s, size, fill):
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(FONT, size)
    d.text((xy[0] + 2, xy[1] + 2), s, font=f, fill=(0, 0, 0, 170), anchor="mm")
    d.text(xy, s, font=f, fill=fill, anchor="mm")


def scene(w, h, gsize, gpos, color, dark):
    img = gradient(w, h, tuple(int(c * 0.35) for c in color), dark)
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((gpos[0] - gsize * 0.7, gpos[1] - gsize * 0.6, gpos[0] + gsize * 0.7,
                                  gpos[1] + gsize * 0.7), fill=color + (120,))
    img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(gsize // 4)))
    g = gem(gsize, color)
    img.alpha_composite(g, (int(gpos[0] - gsize / 2), int(gpos[1] - gsize / 2)))
    return img


def main():
    for game, (name, color, dark, pale) in GAMES.items():
        out = os.path.join("platform", "vita", "sce_sys_%s" % game)
        os.makedirs(os.path.join(out, "livearea", "contents"), exist_ok=True)
        bg = scene(840, 500, 230, (420, 190), color, dark)
        text(bg, (420, 380), "Pokémon %s" % name, 58, (255, 255, 255, 255))
        text(bg, (420, 440), "PS Vita Port", 26, pale + (255,))
        bg.convert("RGB").quantize(256, dither=Image.Dither.NONE).save(os.path.join(out, "livearea", "contents", "bg.png"), optimize=True)
        icon = scene(128, 128, 96, (64, 60), color, dark)
        icon.convert("RGB").quantize(256, dither=Image.Dither.NONE).save(os.path.join(out, "icon0.png"), optimize=True)
        st = scene(280, 158, 110, (70, 79), color, dark)
        text(st, (195, 66), name, 30, (255, 255, 255, 255))
        text(st, (195, 102), "Start", 20, pale + (255,))
        st.convert("RGB").quantize(256, dither=Image.Dither.NONE).save(os.path.join(out, "livearea", "contents", "startup.png"), optimize=True)


main()
