#!/usr/bin/env python3
"""Pack the shipped vector fonts into the binary the game draws text from.

    tools/fontpack.py [--src .planning/fonts/src] [--out fonts/vector_fonts.bin]

Every fonts/<name>/trace*.json with "ship" goes in; unfinished traces wait in
fonts/wip/<name>/, which git and this leave out. "ship" is true, or
{"font": name, "layer": n, "color": [r, g, b], "aliases": [...], "masked": true}
for a layer of a font drawn in more than one (cutout's shadow under its face).
A font is found in the game by its Font object's name less ".font", or an
alias. A masked font's look is its texture's (chalk, grime, grain): the outline
only cuts the texture's own pixels to a sharp edge, and where the texture fades
toward that edge its glyphs' usual inside opacity, its tone, stands in.

Each glyph's outline is read from the OTF, flattened to lines within FLAT of
the curves, and placed in its game cell (the master source's pixels, a
pixel's center on its index) where it best covers the game's own pixels
(fontstrip.registered). Its lines are split into horizontal bands so a pixel
only tests the lines near it.

Layout, little-endian, zlib-compressed after an 8-byte "GHVZ" + raw size:
    "GHVF" u32 version u32 fonts
    per font: u8 len, name; u8 aliases, each u8 len + name; f32 cell w, h;
              u8 layers, each r g b a; u8 flags (1 masked), f32 tone; u16
              glyphs, each u16 code + u32 per layer (a glyph layer index,
              0xffffffff for none)
    u32 glyph layers, each f32 x0 y0 x1 y1 (bounds), f32 band y0, band h,
        u32 first band, band count
    u32 bands, each u32 first line, line count
    u32 lines, each i16 x0 y0 x1 y1 in 1/LINE_UNIT pixels
"""
import argparse
import json
import struct
import zlib
from pathlib import Path

import numpy as np
from fontTools.pens.basePen import BasePen
from fontTools.ttLib import TTFont
from PIL import ImageFont

import fontstrip
import fonttrace

ROOT = fonttrace.ROOT
FLAT = 0.05       # master pixels a flattened curve may stray
LINE_UNIT = 32    # line coordinates are in 1/LINE_UNIT pixels
BAND = 6.0        # master pixels a band spans at most
BAND_PAD = 3.0    # lines this near a band count in it, for the edge distance
MAX_BANDS = 16
VERSION = 2


class Flatten(BasePen):
    """A glyph's outline as closed polylines, in font units."""

    def __init__(self, glyphs, tol_units):
        super().__init__(glyphs)
        self.tol = tol_units
        self.loops, self.cur = [], []

    def _moveTo(self, p):
        self.cur = [p]

    def _lineTo(self, p):
        self.cur.append(p)

    def _curveToOne(self, p1, p2, p3):
        p0 = self.cur[-1]
        # enough steps that a chord's sag stays under tol: sag ~ |second difference| / 8n^2
        d = max(np.hypot(*(np.subtract(p0, p1) - np.subtract(p1, p2))), np.hypot(*(np.subtract(p1, p2) - np.subtract(p2, p3))))
        n = max(1, int(np.ceil(np.sqrt(6 * d / (8 * self.tol)))))
        for t in np.linspace(0, 1, n + 1)[1:]:
            u = 1 - t
            self.cur.append(tuple(u ** 3 * np.array(p0) + 3 * u * u * t * np.array(p1) +
                                  3 * u * t * t * np.array(p2) + t ** 3 * np.array(p3)))

    def _qCurveToOne(self, p1, p2):
        p0 = self.cur[-1]
        self._curveToOne(tuple(np.add(p0, np.multiply(2 / 3, np.subtract(p1, p0)))),
                         tuple(np.add(p2, np.multiply(2 / 3, np.subtract(p1, p2)))), p2)

    def _closePath(self):
        if len(self.cur) > 2:
            self.loops.append(np.array(self.cur, dtype=float))
        self.cur = []

    _endPath = _closePath


def placement(cfg, picked, otf, xmin):
    """char -> (x shift, y) taking a font-unit point to master pixels:
    x = x_u / UNITS + shift, y = baseline - y_u / UNITS + dy."""
    ft = ImageFont.truetype(str(otf), round(1000 / fonttrace.UNITS * fontstrip.SS))
    out = {}
    for c, xm in xmin.items():
        if c not in picked:
            continue
        a = picked[c][0].astype(np.float32)
        _, dx, dy = fontstrip.registered(ft, c, a, xm, cfg["baseline"], offsets=True)
        out[c] = (fontstrip.ink_left(a) - xm / fonttrace.UNITS - dx / fontstrip.SS,
                  cfg["baseline"] - dy / fontstrip.SS)
    return out


def glyph_lines(otf):
    """char -> lines (n, 4) in font units."""
    f = TTFont(otf)
    gs = f.getGlyphSet()
    out = {}
    for code, name in f.getBestCmap().items():
        if code == 32 or code > 255:
            continue
        pen = Flatten(gs, FLAT * fonttrace.UNITS)
        gs[name].draw(pen)
        segs = [np.hstack([loop, np.roll(loop, -1, 0)]) for loop in pen.loops]
        if segs:
            out[chr(code)] = np.vstack(segs)
    return out


def banded(lines):
    """Bounds, band y0 and height, and each band's lines, of a glyph's lines in master pixels."""
    x0, x1 = min(lines[:, 0].min(), lines[:, 2].min()), max(lines[:, 0].max(), lines[:, 2].max())
    y0, y1 = min(lines[:, 1].min(), lines[:, 3].min()), max(lines[:, 1].max(), lines[:, 3].max())
    n = int(np.clip(np.ceil((y1 - y0) / BAND), 1, MAX_BANDS))
    h = max((y1 - y0) / n, 1e-3)
    lo = np.minimum(lines[:, 1], lines[:, 3]) - BAND_PAD
    hi = np.maximum(lines[:, 1], lines[:, 3]) + BAND_PAD
    bands = []
    for b in range(n):
        top, bottom = y0 + b * h, y0 + (b + 1) * h
        bands.append(lines[(hi >= top) & (lo <= bottom)])
    return (x0, y0, x1, y1), y0, h, bands


def ship_of(cfg, config):
    s = cfg.get("ship")
    if not s:
        return None
    s = {} if s is True else dict(s)
    return {"font": s.get("font", config.parent.name), "layer": s.get("layer", 0),
            "color": s.get("color", [255, 255, 255]), "aliases": s.get("aliases", []),
            "masked": bool(s.get("masked"))}


def tone_of(picked):
    """The median opacity inside a font's glyphs, two pixels in from their edges."""
    from scipy import ndimage
    inner = [a[ndimage.binary_erosion(a > 0.5, iterations=2)] for a, _ in picked.values()]
    inner = np.concatenate([v for v in inner if v.size]) if any(v.size for v in inner) else np.ones(1)
    return float(np.median(inner))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--src", type=Path, default=ROOT / ".planning/fonts/src")
    ap.add_argument("--out", type=Path, default=ROOT / "fonts/vector_fonts.bin")
    args = ap.parse_args()
    fonts = {}
    for config in sorted((ROOT / "fonts").glob("*/trace*.json")):
        cfg = json.loads(config.read_text())
        ship = ship_of(cfg, config)
        if ship:
            f = fonts.setdefault(ship["font"], {"layers": {}, "aliases": set(), "masked": False})
            f["aliases"].update(ship["aliases"])
            f["masked"] |= ship["masked"]
            f["layers"][ship["layer"]] = (config, cfg, ship["color"])

    glyph_layers, bands_out, lines_out = [], [], []
    body = bytearray(b"GHVF" + struct.pack("<II", VERSION, len(fonts)))
    for name, f in fonts.items():
        layers = [f["layers"][k] for k in sorted(f["layers"])]
        per_layer, cell, tone = [], None, 1.0
        for config, cfg, color in layers:
            otf = config.parent / f"{cfg['name'].replace(' ', '')}.otf"
            picked, shape = fonttrace.pick(cfg, args.src / "tex")
            if cell is None:
                cell = (shape[1], shape[0])
                tone = tone_of(picked)
            place = placement(cfg, picked, otf, fontstrip.glyph_xmin(otf))
            glyphs = {}
            for c, lines in glyph_lines(otf).items():
                if c not in place:
                    continue
                sx, by = place[c]
                m = lines / fonttrace.UNITS
                m[:, [0, 2]] += sx
                m[:, [1, 3]] = by - m[:, [1, 3]]
                bounds, y0, h, bands = banded(m)
                glyphs[c] = len(glyph_layers)
                glyph_layers.append((bounds, y0, h, len(bands_out), len(bands)))
                for band in bands:
                    bands_out.append((len(lines_out), len(band)))
                    lines_out.extend(band)
            per_layer.append(glyphs)
            print(f"{name} layer {len(per_layer) - 1}: {config.relative_to(ROOT)}, {len(glyphs)} glyphs")
        codes = sorted(set().union(*per_layer), key=ord)
        nb = name.encode("latin-1")
        body += struct.pack("<B", len(nb)) + nb
        body += struct.pack("<B", len(f["aliases"]))
        for alias in sorted(f["aliases"]):
            ab = alias.encode("latin-1")
            body += struct.pack("<B", len(ab)) + ab
        body += struct.pack("<ffB", cell[0], cell[1], len(layers))
        for _, _, color in layers:
            body += bytes(list(color) + [255])
        body += struct.pack("<Bf", 1 if f["masked"] else 0, tone)
        body += struct.pack("<H", len(codes))
        for c in codes:
            body += struct.pack("<H", ord(c))
            for glyphs in per_layer:
                body += struct.pack("<I", glyphs.get(c, 0xFFFFFFFF))
    body += struct.pack("<I", len(glyph_layers))
    for (x0, y0, x1, y1), by0, bh, first, count in glyph_layers:
        body += struct.pack("<ffffffII", x0, y0, x1, y1, by0, bh, first, count)
    body += struct.pack("<I", len(bands_out))
    for first, count in bands_out:
        body += struct.pack("<II", first, count)
    lines = np.round(np.array(lines_out) * LINE_UNIT).astype(np.int16) if lines_out else np.zeros((0, 4), np.int16)
    body += struct.pack("<I", len(lines)) + lines.astype("<i2").tobytes()
    packed = b"GHVZ" + struct.pack("<I", len(body)) + zlib.compress(bytes(body), 9)
    args.out.write_bytes(packed)
    print(f"{args.out.relative_to(ROOT)}: {len(fonts)} fonts, {len(glyph_layers)} glyph layers, "
          f"{len(lines)} lines, {len(body)} bytes raw, {len(packed)} packed")


if __name__ == "__main__":
    main()
