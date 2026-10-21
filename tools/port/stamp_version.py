#!/usr/bin/env python3
"""Writes the port version small into a corner of a LiveArea image.

    stamp_version.py IN.png OUT.png TEXT
"""
import sys
from PIL import Image, ImageDraw, ImageFont


def main():
    src, dst, text = sys.argv[1], sys.argv[2], sys.argv[3]
    im = Image.open(src).convert("RGBA")
    size = max(12, im.height // 26)
    font = ImageFont.load_default(size=size)
    draw = ImageDraw.Draw(im)
    l, t, r, b = draw.textbbox((0, 0), text, font=font)
    x = im.width - (r - l) - size // 2 - l
    y = im.height - (b - t) - size // 2 - t
    pad = max(2, size // 5)
    box = Image.new("RGBA", im.size, (0, 0, 0, 0))
    ImageDraw.Draw(box).rounded_rectangle((x + l - pad, y + t - pad, x + r + pad, y + b + pad), radius=pad,
                                          fill=(0, 0, 0, 120))
    im = Image.alpha_composite(im, box)
    ImageDraw.Draw(im).text((x, y), text, font=font, fill=(255, 255, 255, 235))
    im.convert("RGB" if Image.open(src).mode == "RGB" else im.mode).save(dst, optimize=True)


if __name__ == "__main__":
    main()
