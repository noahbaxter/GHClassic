#!/usr/bin/env python3
"""Draw the custom list printout's instrument icons, assets/instruments/<name>.svg,
as the PS2 bitmaps content/setlist_printout.cpp gives their Texes.

    tools/procedural/instrument_icons.py [--side 64] [--preview]

Each SVG is rendered whole at the side, in its own ink, beside it as a .bin:
a bitmap as crown.py encodes one.
"""
import argparse
import io
from pathlib import Path

import resvg_py
from PIL import Image

from crown import encode

ROOT = Path(__file__).resolve().parents[2]
DIR = ROOT / "assets" / "instruments"
NAMES = ("guitar", "bass", "drums", "vocals")


def draw(name, side):
    png = resvg_py.svg_to_bytes(svg_path=str(DIR / f"{name}.svg"), width=side, height=side)
    return Image.open(io.BytesIO(bytes(png))).convert("RGBA")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--side", type=int, default=64)
    parser.add_argument("--preview", action="store_true", help="also write each as a .png beside it")
    args = parser.parse_args()
    for name in NAMES:
        icon = draw(name, args.side)
        (DIR / f"{name}.bin").write_bytes(encode(icon))
        if args.preview:
            icon.save(DIR / f"{name}.png")


if __name__ == "__main__":
    main()
