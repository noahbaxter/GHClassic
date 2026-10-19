#!/usr/bin/env python3
"""Draw each game's "Classic", the word the menus put under their logo, as the
PS2 bitmap the game loads.

    tools/procedural/classic_logo.py [--out assets/classic] [--calibrate]

In GH1's hand face (fonts/hand/Hand.otf), in the game's colour and at its
angle, with a dark keyline round each letter. A letter's own counter (the
A's) is filled with the keyline so the poster does not show through it;
spaces shut only where neighbouring letters' keylines meet (inside the C)
stay clear. The word is drawn as the 4:3 UI holds it, unstretched: the game
widens it with the rest. It is mirrored left to right, as the help bar's
round end the menus draw it on (main_menu.dta) maps its texture.

Each <game>.bin is a bitmap as RndBitmap reads one (formats/bitmap.h):
8 bits a pixel, the 256-entry palette in the GS's CLUT order with alpha
0..128 (milo/layout.h kRgba | kPs2Alpha), pixels unswizzled. --calibrate
draws a grid with each corner marked instead, for finding where a quad's
texture lands on screen.
"""
import argparse
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

import bitmap

ROOT = Path(__file__).resolve().parents[2]
HAND = ROOT / "fonts" / "hand" / "Hand.otf"
WIDTH, HEIGHT = 512, 128
KEYLINE = (14, 10, 12)
STROKE = 0.03  # of the font size
# Fill colour and angle (degrees, anticlockwise) per game.
GAMES = {
    "gh1": ((222, 199, 26), 4.0),
    "gh2": ((160, 30, 24), 6.0),
    "gh80s": ((190, 15, 115), -2.0),
}
WORD = "Classic"


def word(color, angle, size):
    """The word at that font size, keyline and all, turned, cropped to its ink."""
    font = ImageFont.truetype(str(HAND), size)
    sw = round(size * STROKE)
    l, t, r, b = font.getbbox(WORD, stroke_width=sw)
    pad = sw + 4
    im = Image.new("RGBA", (r - l + pad * 2, b - t + pad * 2), (0, 0, 0, 0))
    ImageDraw.Draw(im).text((pad - l, pad - t), WORD, font=font, fill=color, stroke_width=sw, stroke_fill=KEYLINE)
    # Holes each letter's keyline closes on its own: the A's counter.
    holes = Image.new("L", im.size, 0)
    for i, ch in enumerate(WORD):
        one = Image.new("L", im.size, 0)
        ImageDraw.Draw(one).text((pad - l + font.getlength(WORD[:i]), pad - t), ch, font=font, fill=255,
                                 stroke_width=sw, stroke_fill=255)
        one = one.point(lambda v: 255 if v > 32 else 0)
        ImageDraw.floodfill(one, (0, 0), 128)
        holes = ImageChops.lighter(holes, one.point(lambda v: 255 if v == 0 else 0))
    under = Image.new("RGBA", im.size, (0, 0, 0, 0))
    under.paste(KEYLINE + (255,), (0, 0), holes.filter(ImageFilter.MaxFilter(3)))
    under.alpha_composite(im)
    turned = under.rotate(angle, resample=Image.BICUBIC, expand=True)
    return turned.crop(turned.getbbox())


def classic(color, angle):
    """The word as large as fits the bitmap with a 4-pixel margin, centred."""
    probe = word(color, angle, 200)
    scale = min((WIDTH - 8) / probe.width, (HEIGHT - 8) / probe.height)
    w = word(color, angle, max(8, int(200 * scale)))
    out = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    out.alpha_composite(w, ((WIDTH - w.width) // 2, (HEIGHT - w.height) // 2))
    return out


def calibration():
    out = Image.new("RGBA", (WIDTH, HEIGHT), (255, 0, 255, 255))
    d = ImageDraw.Draw(out)
    for x in range(0, WIDTH, 32):
        d.line((x, 0, x, HEIGHT), fill=(255, 255, 255, 255))
    for y in range(0, HEIGHT, 32):
        d.line((0, y, WIDTH, y), fill=(255, 255, 255, 255))
    for (x, y), c in (((0, 0), (255, 0, 0)), ((WIDTH - 16, 0), (0, 255, 0)), ((0, HEIGHT - 16), (0, 0, 255)),
                      ((WIDTH - 16, HEIGHT - 16), (255, 255, 0))):
        d.rectangle((x, y, x + 15, y + 15), fill=c + (255,))
    return out


def encode(image):
    """RndBitmap rev 1, 8bpp, kRgba | kPs2Alpha, unswizzled."""
    # Entry 0 is clear and holds every clear pixel: quantized with the rest,
    # they share entries with the faint edge's and the word's box shows.
    # The other 255 hold the rest, each at its pixels' mean alpha.
    quant = image.quantize(colors=255, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
    pal = quant.getpalette(rawmode="RGBA") or []
    pal += [0] * (1024 - len(pal))
    source = image.getchannel("A").tobytes()
    pixels = bytes(0 if a < 4 else i + 1 for i, a in zip(quant.tobytes(), source))
    total, count = [0] * 256, [0] * 256
    for i, a in zip(pixels, source):
        total[i] += a
        count[i] += 1
    colours = [(0, 0, 0, 0)]
    for i in range(1, 256):
        r, g, b = pal[(i - 1) * 4:(i - 1) * 4 + 3]
        a = total[i] // count[i] if count[i] else 0
        colours.append((r, g, b, (a + 1) // 2))
    return bitmap.header(WIDTH, HEIGHT) + bitmap.palette(colours) + pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, default=ROOT / "assets" / "classic")
    parser.add_argument("--calibrate", action="store_true")
    parser.add_argument("--preview", action="store_true", help="also write each as <game>.png")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for game, (color, angle) in GAMES.items():
        image = calibration() if args.calibrate else classic(color, angle)
        (args.out / f"{game}.bin").write_bytes(encode(image.transpose(Image.Transpose.FLIP_LEFT_RIGHT)))
        if args.preview:
            image.save(args.out / f"{game}.png")


if __name__ == "__main__":
    main()
