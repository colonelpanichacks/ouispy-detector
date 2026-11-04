#!/usr/bin/env python3
"""The top bar's art: the OUI-SPY Recon board's back strip (two periodical cicadas,
heads toward LED2, rings radiating out from the LED behind them), redrawn from the
same silk sources (cicada-silk.png and moon-silk.png, from hardware/xiao147-carrier
make_cicada.py / make_moon.py) with the board's own numbers (place_led_ring.py,
make_art.py), so it matches the board. The full moon takes the LED's place.

White on transparent; the app tints it. Rings are drawn at RING_A alpha, the cicadas
solid, so one tint gives the board's two tones.

    python3 tool/make_header_art.py   # -> assets/art/header.png
"""
import os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "cicada-silk.png")
MOON = os.path.join(HERE, "moon-silk.png")
OUT = os.path.join(HERE, "..", "assets", "art", "header.png")
MOON_OUT = os.path.join(HERE, "..", "assets", "art", "moon.png")   # the moon on its own: the app tints it as the live light

PPM = 44                                # output px per mm
SS = 3                                  # supersampling
W_MM, BAND_MM = 60.0, 8.2               # canvas width, and the band the cicadas and moon sit in (the board strip is 47.9 x 5.6)
TAIL_MM = 13.0                          # the rings run on below the band, fading out to nothing
TOP_MM = 10.0                           # and up past the title, fading out just above it
MOON_D = 4.6                            # the moon, where the LED sits on the board
R0, W0, PITCH, W_END, R_END = 2.75, 0.42, 0.72, 0.14, 31.0
CIC_DX, CIC_W, CIC_H = 13.5, 13.73, 5.4  # each cicada's centre from the LED, its size
ART_GAP = 0.3
RING_A = 0.55


def main():
    k = PPM * SS
    W, H = int(W_MM * k), int((TOP_MM + BAND_MM + TAIL_MM) * k)
    cx, cy = W / 2, (TOP_MM + BAND_MM / 2) * k

    rings = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(rings)
    radii = []
    r = R0
    while r < R_END:
        radii.append(r)
        r += PITCH
    for r in reversed(radii):                                   # outermost first, so a ring's hole never eats the ones inside it
        f = (r - R0) / (R_END - R0)
        w = W0 + (W_END - W0) * f
        ro, ri = (r + w) * k, r * k
        d.ellipse((cx - ro, cy - ro, cx + ro, cy + ro), fill=255)
        d.ellipse((cx - ri, cy - ri, cx + ri, cy + ri), fill=0)

    silk = Image.open(SRC).convert("L")
    ink = silk.point(lambda v: 255 if v < 128 else 0)          # black in the source is silk
    cw, ch = int(CIC_W * k), int(CIC_H * k)
    ink = ink.resize((cw, ch), Image.LANCZOS).point(lambda v: 255 if v >= 128 else 0)
    cic = Image.new("L", (W, H), 0)
    for side in (-1, 1):
        im = ink if side < 0 else ink.transpose(Image.FLIP_LEFT_RIGHT)   # heads toward the LED
        x = int(cx + side * CIC_DX * k - cw / 2)
        y = int(cy - ch / 2)
        cic.paste(im, (x, y), im)

    md = int(MOON_D * k)
    moon = Image.open(MOON).convert("L").point(lambda v: 255 if v < 128 else 0).resize((md, md), Image.LANCZOS)
    disc = Image.new("L", (md, md), 0)
    ImageDraw.Draw(disc).ellipse((0, 0, md - 1, md - 1), fill=255)
    moon = Image.fromarray(np.minimum(np.array(moon), np.array(disc)))
    # the moon is its own image (the live light); the band keeps its bare ring round it
    mo = Image.new("LA", (md, md), (255, 0))
    mo.putalpha(moon)
    mo = mo.resize((md // SS, md // SS), Image.LANCZOS)
    # nothing outside the disc: resampling leaves a faint fringe the app's tint would show
    o = md // SS
    clip = Image.new("L", (o, o), 0)
    ImageDraw.Draw(clip).ellipse((0.5, 0.5, o - 1.5, o - 1.5), fill=255)
    mo.putalpha(Image.fromarray(np.minimum(np.array(mo.getchannel("A")), np.array(clip))))
    mo.save(MOON_OUT, optimize=True)

    # the bare outline round each cicada, cut out of the rings
    gap = int(ART_GAP * k) * 2 + 1
    halo = cic.filter(ImageFilter.MaxFilter(gap if gap % 2 else gap + 1))
    halo = halo.filter(ImageFilter.MaxFilter(3))
    ringa = np.array(rings, np.float32) * (1 - np.array(halo, np.float32) / 255) * RING_A
    # below the band only the rings' lower arcs carry on, fading out
    yy = np.arange(H, dtype=np.float32) / k
    down = 1 - np.clip((yy - TOP_MM - BAND_MM) / TAIL_MM, 0, 1)
    up = np.clip(yy / TOP_MM, 0, 1) ** 0.8          # a gentle rise: still faintly there under the title
    fade = np.minimum(down, up)
    fade = (fade * fade * (3 - 2 * fade))[:, None]
    a = np.maximum(ringa * fade, np.array(cic, np.float32))

    out = Image.new("LA", (W, H), (255, 0))
    out.putalpha(Image.fromarray(a.clip(0, 255).astype(np.uint8)))
    out = out.resize((W // SS, H // SS), Image.LANCZOS)
    out.save(OUT, optimize=True)
    print(OUT, out.size)


if __name__ == "__main__":
    main()
