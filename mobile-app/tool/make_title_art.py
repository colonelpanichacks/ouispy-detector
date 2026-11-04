#!/usr/bin/env python3
"""The top bar's title: the board's own OUI SPY RECON banner (ascii-banner-trim.png,
from hardware/xiao147-carrier/make_ascii_banner.py; the figlet lettering on the front),
as white lettering cut out of the art behind it by a bare black outline, as on the
board (make_art.py's ART_GAP round each piece). The app tints the white (modulate), so
the outline stays black.

    python3 tool/make_title_art.py   # -> assets/art/title.png
"""
import os
import numpy as np
from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "ascii-banner-trim.png")
OUT = os.path.join(HERE, "..", "assets", "art", "title.png")
W = 1600
GROW = 9                                        # source px the strokes grow by (odd)
GAP = 0.055                                     # the outline, as a fraction of the lettering's height


def main():
    im = Image.open(SRC).convert("L")
    # thicken the glyph strokes so they still read at phone size (as the board's fab pass grows fine copper)
    im = im.filter(ImageFilter.MinFilter(GROW))
    a = 255 - np.array(im)                       # black ink in the source
    ys, xs = np.where(a > 128)
    a = a[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    al = Image.fromarray(a)
    h = round(W * al.size[1] / al.size[0])
    g = max(2, round(GAP * h))
    al = al.resize((W, h), Image.LANCZOS)
    pad = Image.new("L", (W + 2 * g, h + 2 * g), 0)
    pad.paste(al, (g, g))
    al = pad
    ring = al.point(lambda v: 255 if v > 40 else 0)
    for _ in range(g):
        ring = ring.filter(ImageFilter.MaxFilter(3))
    ring = ring.filter(ImageFilter.GaussianBlur(0.8))
    a = np.maximum(np.array(ring), np.array(al))
    out = Image.merge("RGBA", (al, al, al, Image.fromarray(a)))   # white letters, black outline
    out.save(OUT, optimize=True)
    print(OUT, out.size)


if __name__ == "__main__":
    main()
