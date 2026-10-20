#!/usr/bin/env python3
"""Personal LiveArea artwork for the PS Vita package, made from the game's
own graphics: the title screen (a screenshot of the port's host build) and
the front sprite of the version's legendary Pokémon.

The result goes to platform/vita/sce_sys_local_<game>/, which is git-ignored:
the images are Nintendo / Game Freak artwork and stay on your machine.

usage: make_livearea_local.py <game: sapphire|ruby> <title_screenshot.ppm>
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter


def palette_sprite(png, pal):
    """The decomp's sprite PNGs carry their palette in a JASC .pal file."""
    im = Image.open(png).convert("P")
    lines = open(pal).read().split()
    count = int(lines[2])
    colors = []
    for i in range(count):
        r, g, b = (int(x) for x in lines[3 + i * 3:6 + i * 3])
        colors += [r, g, b]
    im.putpalette(colors + [0] * (768 - len(colors)))
    rgba = im.convert("RGBA")
    px = rgba.load()
    idx = im.load()
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            if idx[x, y] == 0:  # colour 0 is transparent
                px[x, y] = (0, 0, 0, 0)
    return rgba


def radial(w, h, inner, outer):
    img = Image.new("RGB", (w, h))
    px = img.load()
    cx, cy = w / 2, h / 2
    md = (cx * cx + cy * cy) ** 0.5
    for y in range(h):
        for x in range(w):
            t = min(1.0, (((x - cx) ** 2 + (y - cy) ** 2) ** 0.5) / md)
            px[x, y] = tuple(int(inner[i] + (outer[i] - inner[i]) * t) for i in range(3))
    return img


def main():
    game, shot = sys.argv[1], sys.argv[2]
    out = os.path.join("platform", "vita", "sce_sys_local_%s" % game)
    os.makedirs(os.path.join(out, "livearea", "contents"), exist_ok=True)
    title = Image.open(shot).convert("RGB")  # 240x160

    # Background 840x500: title screen at 3.125x (pixel art kept sharp by
    # scaling 4x nearest first), the water continued to the sides.
    big = title.resize((960, 640), Image.NEAREST).resize((750, 500), Image.LANCZOS)
    bg = Image.new("RGB", (840, 500))
    # the water continued to the sides: mirrored strips of the edges
    left = big.crop((0, 0, 45, 500)).transpose(Image.FLIP_LEFT_RIGHT)
    right = big.crop((705, 0, 750, 500)).transpose(Image.FLIP_LEFT_RIGHT)
    bg.paste(left, (0, 0))
    bg.paste(right, (795, 0))
    bg.paste(big, (45, 0))
    bg.quantize(256, dither=Image.Dither.NONE).save(os.path.join(out, "livearea", "contents", "bg.png"), optimize=True)

    # Startup tile 280x158: the whole logo.
    logo = title.crop((38, 0, 202, 92)).resize((656, 368), Image.NEAREST).resize((280, 158), Image.LANCZOS)
    logo.quantize(256, dither=Image.Dither.NONE).save(os.path.join(out, "livearea", "contents", "startup.png"), optimize=True)

    # Icon 128x128: the legendary on a glowing background.
    mon = "kyogre" if game == "sapphire" else "groudon"
    inner, outer = ((150, 210, 255), (20, 60, 150)) if game == "sapphire" else ((255, 150, 90), (120, 20, 10))
    icon = radial(128, 128, inner, outer).convert("RGBA")
    sprite = palette_sprite("graphics/pokemon/%s/front.png" % mon, "graphics/pokemon/%s/normal.pal" % mon)
    sprite = sprite.resize((128, 128), Image.NEAREST)
    shadow = Image.new("RGBA", (128, 128), (0, 0, 0, 0))
    shadow.paste((0, 0, 0, 110), (3, 4), sprite.split()[3].point(lambda a: 255 if a else 0))
    icon.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(3)))
    icon.alpha_composite(sprite)
    icon.convert("RGB").quantize(256, dither=Image.Dither.NONE).save(os.path.join(out, "icon0.png"), optimize=True)
    print("make_livearea_local: wrote %s" % out)


main()
