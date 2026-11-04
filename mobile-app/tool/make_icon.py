#!/usr/bin/env python3
"""The app icon: the board's full moon with its rings radiating out (the same silk as
the top bar, tool/make_header_art.py), light grey at the moon fading to grey, on the
app's black. Writes every Android mipmap and iOS AppIcon size in place.

    python3 tool/make_icon.py
"""
import glob, os
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.join(HERE, "..")
MOON = os.path.join(HERE, "moon-silk.png")
N = 1024 * 3
BG = (7, 9, 12)
MOON_D, R0, PITCH, W0, W_END = 0.34, 0.205, 0.052, 0.026, 0.011   # fractions of the icon


def render():
    c = N / 2
    ring = Image.new("L", (N, N), 0)
    d = ImageDraw.Draw(ring)
    radii, r = [], R0
    while r < 0.75:
        radii.append(r)
        r += PITCH
    for r in reversed(radii):
        f = (r - R0) / (0.75 - R0)
        w = W0 + (W_END - W0) * f
        ro, ri = (r + w) * N, r * N
        d.ellipse((c - ro, c - ro, c + ro, c + ro), fill=255)
        d.ellipse((c - ri, c - ri, c + ri, c + ri), fill=0)
    md = int(MOON_D * N)
    moon = Image.open(MOON).convert("L").point(lambda v: 255 if v < 128 else 0).resize((md, md), Image.LANCZOS)
    disc = Image.new("L", (md, md), 0)
    ImageDraw.Draw(disc).ellipse((0, 0, md - 1, md - 1), fill=255)
    m = Image.new("L", (N, N), 0)
    m.paste(Image.fromarray(np.minimum(np.array(moon), np.array(disc))), (int(c - md / 2),) * 2)
    yy, xx = np.mgrid[0:N, 0:N]
    rad = np.hypot(xx - c, yy - c) / (N / 2)
    tone = 0xC8 * (1 - rad.clip(0, 1)) + 0x40 * rad.clip(0, 1)                 # light grey at the moon, grey out at the edge
    a = np.maximum(np.array(ring, np.float32) / 255 * 0.8, np.array(m, np.float32) / 255)
    rgb = np.empty((N, N, 3), np.float32)
    for i, b in enumerate(BG):
        rgb[..., i] = b + (tone + (i * 2) - b) * a
    return Image.fromarray(rgb.clip(0, 255).astype(np.uint8))


def main():
    im = render().resize((1024, 1024), Image.LANCZOS)
    for dpi, px in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
        p = os.path.join(APP, "android/app/src/main/res/mipmap-%s/ic_launcher.png" % dpi)
        # Android masks launcher icons round on most launchers: a rounded square of the art
        sq = im.resize((px, px), Image.LANCZOS)
        mask = Image.new("L", (px * 4, px * 4), 0)
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, px * 4 - 1, px * 4 - 1), radius=px * 4 * 0.22, fill=255)
        out = Image.new("RGBA", (px, px), (0, 0, 0, 0))
        out.paste(sq, (0, 0), mask.resize((px, px), Image.LANCZOS))
        out.save(p)
    for p in glob.glob(os.path.join(APP, "ios/Runner/Assets.xcassets/AppIcon.appiconset/*.png")):
        w = Image.open(p).size[0]
        im.resize((w, w), Image.LANCZOS).save(p)        # iOS icons must be opaque; the system rounds them
    im.save(os.path.join(HERE, "icon-1024.png"))
    print("icons written")


if __name__ == "__main__":
    main()
