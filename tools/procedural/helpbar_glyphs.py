#!/usr/bin/env python3
"""Draw the help bar's labels, assets/helpbar/glyphs.bin: the sheet ui/help_bar.cpp
lays over the disc's own gems, so the bar shows the control a player's menus
read (a pad's LT, a key's 1) on the fret it plays.

    tools/procedural/helpbar_glyphs.py [--out assets/helpbar] [--preview]

The sheet is CELL-pixel cells, COLS to a row: each label in LABELS' order,
white with a dark keyline, centred in its cell; then, in a row of their own,
the strum bar's two stand-ins (a pad's d-pad and the keyboard's arrow keys),
STRUM wide by CELL / 2. The help bar draws a fret's 32 by 32 Tex square,
and the strum bar's 64 by 16 STRUM_SQUASH times taller than wide (it shows
only the bar's middle rows), in 4:3 and widescreen alike, so a strum
stand-in is drawn that much shorter, to read upright there: measured, a
circle on a gem and a square key on the bar, at 1080p. A 360 bitmap, DXT5,
by tools/campaign_art.py's encoder.
"""
import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import campaign_art  # noqa: E402

FONT = ROOT / "fonts" / "helveticablackcondensed" / "Helveticablackcondensed.otf"
CELL, COLS = 96, 16
STRUM = 192
STRUM_SQUASH = 1.95
SCALE = 4  # drawn this many times larger, then brought down
KEYLINE = (22, 16, 16, 255)
WHITE = (255, 255, 255, 255)
# In ui/help_bar.cpp's kLabels order. Words are a pad's or a key's names;
# the shapes are drawn.
SHAPES = ["CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "UP", "DOWN", "LEFT", "RIGHT"]
LABELS = (["A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "L1", "R1", "L2", "R2", "L3", "R3"] + SHAPES
          + ["START", "BACK", "SELECT", "OPTIONS", "ENTER", "ESC", "TAB", "SPACE", "SHIFT"]
          + [str(d) for d in range(10)] + [c for c in "CDEFGHIJKLMNOPQRSTUVWZ"])


def shape(d, name, cx, cy, size, width, fill):
    h = size / 2
    if name == "CROSS":
        d.line((cx - h, cy - h, cx + h, cy + h), fill=fill, width=width)
        d.line((cx - h, cy + h, cx + h, cy - h), fill=fill, width=width)
    elif name == "CIRCLE":
        d.ellipse((cx - h, cy - h, cx + h, cy + h), outline=fill, width=width)
    elif name == "SQUARE":
        d.rectangle((cx - h * 0.9, cy - h * 0.9, cx + h * 0.9, cy + h * 0.9), outline=fill, width=width)
    elif name == "TRIANGLE":
        pts = [(cx, cy - h), (cx + h * 1.05, cy + h * 0.8), (cx - h * 1.05, cy + h * 0.8)]
        d.line(pts + [pts[0], pts[1]], fill=fill, width=width, joint="curve")
    else:
        turn = {"UP": 0, "RIGHT": 1, "DOWN": 2, "LEFT": 3}[name]
        pts = [(0, -h), (h * 0.95, h * 0.7), (-h * 0.95, h * 0.7)]
        for _ in range(turn):
            pts = [(-y, x) for x, y in pts]
        d.polygon([(cx + x, cy + y) for x, y in pts], fill=fill)


def label(text):
    """A label to lay over a gem, centred in its cell."""
    s = CELL * SCALE
    im = Image.new("RGBA", (s, s))
    d = ImageDraw.Draw(im)
    cx, cy = s / 2, s / 2
    keyline = int(s * 0.04)
    if text in SHAPES:
        size = s * 0.52
        if text in ("UP", "DOWN", "LEFT", "RIGHT"):
            shape(d, text, cx, cy, size + keyline * 2.2, 0, KEYLINE)
            shape(d, text, cx, cy, size, 0, WHITE)
        else:
            width = int(s * 0.07)
            shape(d, text, cx, cy, size, width + keyline * 2, KEYLINE)
            shape(d, text, cx, cy, size, width, WHITE)
    else:
        size = int(s * (0.8 if len(text) <= 2 else 0.5))
        while True:
            font = ImageFont.truetype(str(FONT), size)
            l, t, r, b = d.textbbox((0, 0), text, font=font, stroke_width=keyline)
            if r - l <= s * 0.72 or size < 8:
                break
            size -= 4
        d.text((cx - (r - l) / 2 - l, cy - (b - t) / 2 - t), text, font=font, fill=WHITE, stroke_width=keyline,
               stroke_fill=KEYLINE)
    return im.resize((CELL, CELL), Image.LANCZOS)


def keycap(d, box, u):
    x0, y0, x1, y1 = box
    d.rounded_rectangle((x0, y0 + 2 * u, x1, y1), radius=4 * u, fill=KEYLINE)
    d.rounded_rectangle((x0 + u, y0 + 3 * u, x1 - u, y1 - u), radius=3 * u, fill=(120, 120, 120, 255))
    d.rounded_rectangle((x0 + u, y0 + u, x1 - u, y1 - 4 * u), radius=3 * u, fill=(232, 232, 232, 255))


def strum(kind):
    """The strum bar's stand-in, STRUM by CELL / 2: a d-pad, up and down marked,
    or the up and down arrow keys, drawn STRUM_SQUASH times taller to be
    brought down to read upright."""
    w, h = STRUM * SCALE, round(CELL // 2 * SCALE * STRUM_SQUASH)
    u = h / 32
    im = Image.new("RGBA", (w, h))
    d = ImageDraw.Draw(im)
    cx, cy = w / 2, h / 2
    if kind == "dpad":
        a, t = 15 * u, 5.5 * u
        d.rectangle((cx - t - u, cy - a - u, cx + t + u, cy + a + u), fill=KEYLINE)
        d.rectangle((cx - a - u, cy - t - u, cx + a + u, cy + t + u), fill=KEYLINE)
        d.rectangle((cx - a, cy - t, cx + a, cy + t), fill=(226, 226, 226, 255))
        d.rectangle((cx - t, cy - a, cx + t, cy + a), fill=(226, 226, 226, 255))
        for name, y in (("UP", cy - a + 5 * u), ("DOWN", cy + a - 5 * u)):
            shape(d, name, cx, y, 6 * u, 0, KEYLINE)
    else:
        for dx, name in ((-17 * u, "UP"), (17 * u, "DOWN")):
            x = cx + dx
            keycap(d, (x - 14 * u, cy - 15 * u, x + 14 * u, cy + 15 * u), u)
            shape(d, name, x, cy - 2 * u, 12 * u, 0, (30, 30, 30, 255))
    return im.resize((STRUM, CELL // 2), Image.LANCZOS)


def sheet():
    rows = (len(LABELS) + COLS - 1) // COLS
    out = Image.new("RGBA", (COLS * CELL, (rows + 1) * CELL))
    for i, text in enumerate(LABELS):
        out.alpha_composite(label(text), ((i % COLS) * CELL, (i // COLS) * CELL))
    for i, kind in enumerate(("dpad", "keys")):
        out.alpha_composite(strum(kind), (i * STRUM, rows * CELL))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, default=ROOT / "assets" / "helpbar")
    parser.add_argument("--preview", action="store_true", help="also write the sheet as glyphs.png")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    image = sheet()
    (args.out / "glyphs.bin").write_bytes(campaign_art.bitmap(image))
    if args.preview:
        image.save(args.out / "glyphs.png")


if __name__ == "__main__":
    main()
