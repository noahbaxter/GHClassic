#!/usr/bin/env python3
"""Draw the 80s song list's wall, a zebra shag rug, as the PS2 bitmap the game
tiles behind the paper.

    tools/procedural/zebra_wall.py [--out assets/zebra/wall.bin] [--side 512] [--colors 256] [--preview]

The stripes are a tilted cosine, bent by smooth noise, forked and tapered by
more, with fine noise for hairy edges. Every noise is made in the frequency
domain, so the tile wraps. The pile is drawn over that fibre by fibre, black
first and cream on top so the cream fringes into the black, lit from the top
left as if the cream stood proud. Drawn at 1024 and resized to --side.

wall.bin is a bitmap as RndBitmap reads one (formats/bitmap.h): 8 bits a
pixel, the palette in the GS's CLUT order, opaque.
"""
import argparse
import math
import random
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

import bitmap

ROOT = Path(__file__).resolve().parents[2]
N = 1024
SEED = 3
# Stripes: p and q cycles across and down (whole, so the tile wraps), how far
# the bends and forks push, how much of each cycle is ink, edge hair.
STRIPES = dict(p=1, q=7, warp=1.1, fork=0.35, width=0.2, fuzz=0.10)
CREAM, INK = (228, 222, 208), (14, 12, 12)
LENGTH = 9  # fibre length in pixels at 1024
LEAN = 1.2  # fibre angle, radians clockwise from the right
RELIEF = 1.0


def pnoise(rng, cycles):
    """Periodic smooth noise, std 1, features about `cycles` per tile."""
    f = np.fft.fftfreq(N) * N
    fx, fy = np.meshgrid(f, f)
    spec = np.fft.fft2(rng.standard_normal((N, N))) * np.exp(-(fx ** 2 + fy ** 2) / (2 * cycles ** 2))
    n = np.real(np.fft.ifft2(spec))
    return (n - n.mean()) / n.std()


def blur(a, sigma):
    """Gaussian blur that wraps at the tile's edges."""
    f = np.fft.fftfreq(N)
    fx, fy = np.meshgrid(f, f)
    return np.real(np.fft.ifft2(np.fft.fft2(a) * np.exp(-2 * (math.pi * sigma) ** 2 * (fx ** 2 + fy ** 2))))


def stripes(seed, p, q, warp, fork, width, fuzz):
    """1 cream, 0 ink."""
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:N, 0:N] / N
    phase = 2 * math.pi * (p * x + q * y) + warp * pnoise(rng, 1.5) + warp * 0.12 * pnoise(rng, 3)
    s = np.cos(phase) + fork * pnoise(rng, 3) - width + 0.3 * pnoise(rng, 2) * abs(width + 0.2)
    s = s + fuzz * pnoise(rng, 90)
    return np.clip(0.5 + s * 6, 0, 1)


def pile(m, seed):
    rng = random.Random(seed)
    # The cream sits proud of the black: light from the top left, shadow under each cream edge.
    gy, gx = np.gradient(blur(m, 5))
    lit = np.clip(1 + RELIEF * (-gx - gy) * 12, 0.55, 1.3)
    shadow = np.clip(blur(m, 9) - m, 0, 1)  # ink near cream gets a soft grey haze
    base = lambda k: [INK[i] + (CREAM[i] - INK[i]) * k for i in range(3)]
    # Under the fibres, the backing in the pile's own colour, so gaps between them don't show black.
    under = np.stack([INK[i] + (CREAM[i] - INK[i]) * blur(m, 2) for i in range(3)], -1) * 0.85
    im = Image.fromarray(np.clip(under, 0, 255).astype(np.uint8))
    d = ImageDraw.Draw(im)
    # The black pile first, the taller cream pile over it, so cream fringes into the black.
    roots = [(rng.uniform(0, N), rng.uniform(0, N)) for _ in range(int(N * N / 6))]
    roots.sort(key=lambda r: m[int(r[1]), int(r[0])])
    for x, y in roots:
        k = m[int(y), int(x)]
        a = LEAN + rng.gauss(0, 0.45)
        L = LENGTH * rng.uniform(0.6, 1.4)
        dx, dy = math.cos(a) * L, math.sin(a) * L
        g = rng.uniform(0.86, 1.04) * lit[int(y), int(x)]
        c0 = base(k)
        c0 = [c0[i] + (90 * shadow[int(y), int(x)] if k < 0.5 else 0) for i in range(3)]
        root = tuple(int(min(255, c * g * 0.92)) for c in c0)
        tip = tuple(int(min(255, c * g * 1.12 + 6)) for c in c0)
        # A fibre over an edge is drawn again on the far side, so the tile wraps.
        for ox in (-N, 0, N):
            for oy in (-N, 0, N):
                if -L < x + ox < N + L and -L < y + oy < N + L:
                    d.line((x + ox, y + oy, x + ox + dx / 2, y + oy + dy / 2), fill=root, width=2)
                    d.line((x + ox + dx / 2, y + oy + dy / 2, x + ox + dx, y + oy + dy), fill=tip, width=1)
    return im


def encode(image, side, colors):
    """RndBitmap rev 1, 8bpp, kRgba | kPs2Alpha, opaque, unswizzled."""
    im = image.resize((side, side), Image.LANCZOS)
    quant = im.quantize(colors=colors, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = quant.getpalette() or []
    pal += [0] * (768 - len(pal))
    colours = [(*pal[i * 3:i * 3 + 3], 128) for i in range(256)]
    return bitmap.header(side, side) + bitmap.palette(colours) + quant.tobytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, default=ROOT / "assets" / "zebra" / "wall.bin")
    parser.add_argument("--side", type=int, default=512)
    parser.add_argument("--colors", type=int, default=256)
    parser.add_argument("--preview", action="store_true", help="also write the 1024 render as wall.png beside it")
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    tile = pile(stripes(SEED, **STRIPES), SEED)
    args.out.write_bytes(encode(tile, args.side, args.colors))
    if args.preview:
        tile.save(args.out.with_suffix(".png"))


if __name__ == "__main__":
    main()
