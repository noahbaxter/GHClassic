#!/usr/bin/env python3
"""Draw the full combo crown, assets/crown.svg, as the PS2 bitmap the song
lists show beside a song full combo'd.

    tools/procedural/crown.py [--out assets/crown/crown.bin] [--side 64] [--preview]

The SVG is one closed path of straight lines: its shape alone is taken, drawn
at eight times the side and brought down, with no outline, in the song
lists' gold stars' own gold (list_song.milo's stars_gold_5.tex), bright in the
middle and deeper to the edge as theirs are. crown.bin is a bitmap as
RndBitmap reads one (formats/bitmap.h): 8 bits a pixel, the palette in the
GS's CLUT order, its alpha the crown's edge.
"""
import argparse
import re
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

import bitmap

ROOT = Path(__file__).resolve().parents[2]
SVG = ROOT / "assets" / "crown" / "crown.svg"
SCALE = 8
MARGIN = 0.04  # of the side, clear round the crown
# The gold stars' middle and edge.
MIDDLE, EDGE = np.array((235, 235, 0)), np.array((218, 187, 0))


def points(d):
    """A path of moves, lines and a close, absolute or relative, as points."""
    out, at, cmd = [], (0.0, 0.0), None
    tokens = re.findall(r"[MmLlZz]|-?[\d.]+(?:e-?\d+)?", d)
    i = 0
    while i < len(tokens):
        if tokens[i] in "MmLlZz":
            cmd = tokens[i]
            i += 1
            if cmd in "Zz":
                continue
        x, y = float(tokens[i]), float(tokens[i + 1])
        i += 2
        at = (x, y) if cmd in "ML" else (at[0] + x, at[1] + y)
        out.append(at)
        # Pairs after a move are lines of the same kind.
        cmd = {"m": "l", "M": "L"}.get(cmd, cmd)
    return out


def draw(side):
    path = ET.parse(SVG).getroot().find(".//{http://www.w3.org/2000/svg}path")
    pts = points(path.get("d"))
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    # The crown centred in the square.
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    big = side * SCALE
    fit = big * (1 - 2 * MARGIN) / max(x1 - x0, y1 - y0)
    ox, oy = (big - (x1 - x0) * fit) / 2 - x0 * fit, (big - (y1 - y0) * fit) / 2 - y0 * fit
    poly = [(x * fit + ox, y * fit + oy) for x, y in pts]
    mask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(mask).polygon(poly, fill=255)
    # The gold by how far a point is from the crown's middle, across its
    # width and height.
    cx, cy = big / 2, big / 2
    rx, ry = (x1 - x0) * fit / 2, (y1 - y0) * fit / 2
    yy, xx = np.mgrid[0:big, 0:big]
    far = np.clip(np.hypot((xx - cx) / rx, (yy - cy) / ry), 0, 1)[..., None]
    gold = (MIDDLE + (EDGE - MIDDLE) * far ** 2).astype(np.uint8)
    im = Image.fromarray(np.dstack([gold, np.asarray(mask)]), "RGBA")
    return im.resize((side, side), Image.LANCZOS)


def encode(im):
    """RndBitmap rev 1, 8bpp, kRgba | kPs2Alpha, unswizzled; alpha 0..128."""
    quant = im.quantize(colors=256, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
    pal = quant.getpalette(rawmode="RGBA") or []
    pal += [0] * (1024 - len(pal))
    colours = [(*pal[i * 4:i * 4 + 3], (pal[i * 4 + 3] * 128 + 127) // 255) for i in range(256)]
    return bitmap.header(im.width, im.height) + bitmap.palette(colours) + quant.tobytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, default=ROOT / "assets" / "crown" / "crown.bin")
    parser.add_argument("--side", type=int, default=64)
    parser.add_argument("--preview", action="store_true", help="also write the crown as crown.png beside it")
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    crown = draw(args.side)
    args.out.write_bytes(encode(crown))
    if args.preview:
        crown.save(args.out.with_suffix(".png"))


if __name__ == "__main__":
    main()
