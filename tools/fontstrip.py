#!/usr/bin/env python3
"""Compare a traced font against the game's pixels, glyph for glyph.

    tools/fontstrip.py fonts/<name>/trace.json [...] [--overview fonts/compare.png]

Each glyph of the OTF, drawn from its own outline as the game draws it, is
boxed down into the game cell's own pixels, the coverage each texture pixel
would hold (a cell warped in from a source at another scale goes down to that
source's pixels and back up, as its game pixels did), and lined up with the game's by the best sub-pixel shift, so the two are compared like
for like. Two rows per line: the game's pixels, and the difference (ink
both have dim gray, ink only the game has red, only the OTF cyan; under
NOISE coverage left dark as anti-aliasing), with each glyph's shape
mismatch under it: the difference past NOISE over the game's ink. Writes
<name>_compare.png beside each config, and --scorecard a JSON of each
font's mismatch, texture and worst glyphs; --overview also stacks every
font's first line.
"""
import argparse
import json
from functools import lru_cache
from pathlib import Path

import numpy as np
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.recordingPen import RecordingPen
from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage

import fontrefine
import fonttrace

ROOT = fonttrace.ROOT
CELL_PX = 72   # screen height of a master cell
WIDTH = 2000   # line wrap
LABEL_W = 110
SS = 8         # glyphs drawn at SS times the cell, boxed down to it
SHIFT = 1.0    # master pixels the registration searches each way
NOISE = 0.15   # coverage difference left dark as anti-aliasing
GAIN = 2.5
SHARED = 0.3   # how bright ink both have is drawn in the difference row
PAD = 3        # master pixels around a glyph's ink in its tile
BG = 14


def glyph_xmin(otf):
    """char -> ink x min in font units."""
    f = TTFont(otf)
    gs = f.getGlyphSet()
    out = {}
    for code, name in f.getBestCmap().items():
        pen = BoundsPen(gs)
        gs[name].draw(pen)
        if code != 32 and pen.bounds:
            out[chr(code)] = pen.bounds[0]
    return out


def ink_left(a):
    """x where a cell's ink first reaches half coverage, to a fraction of a pixel."""
    prof = a.max(0)
    i = int(np.argmax(prof >= 0.5))
    if i == 0:
        return -0.5
    return i - 1 + (0.5 - prof[i - 1]) / max(prof[i] - prof[i - 1], 1e-6)


@lru_cache(None)
def glyph_set(otf):
    f = TTFont(otf)
    return f.getGlyphSet(), f.getBestCmap()


def polygons(otf, c):
    """c's contours in font units, curves flattened, as fontpack reads them."""
    gs, cmap = glyph_set(str(otf))
    pen = RecordingPen()
    gs[cmap[ord(c)]].draw(pen)
    out, cur = [], []
    for op, args in pen.value:
        if op == "moveTo":
            cur = [args[0]]
        elif op == "lineTo":
            cur.append(args[0])
        elif op == "curveTo":
            p0, (c1, c2, p3) = np.array(cur[-1]), map(np.array, args)
            for t in np.linspace(0, 1, 25)[1:]:
                cur.append(tuple((1 - t) ** 3 * p0 + 3 * (1 - t) ** 2 * t * c1 + 3 * (1 - t) * t ** 2 * c2 + t ** 3 * p3))
        elif op in ("closePath", "endPath"):
            out.append(np.array(cur, float))
            cur = []
    return out


def registered(otf, c, a, xmin, baseline, offsets=False):
    """The OTF glyph, drawn from its own outline as the game draws it, as coverage of
    a's pixels at the shift that matches a best; with offsets, also that shift in 1/SS
    pixels (dx, dy)."""
    h, w = a.shape
    m = round(SHIFT * SS)
    W, H = int((w + 2 * SHIFT) * SS), int((h + 2 * SHIFT) * SS)
    acc = np.zeros((H, W), bool)
    # pixel i's center is master x = i, so its left edge is canvas (i + SHIFT) * SS
    pen = ink_left(a) - xmin / fonttrace.UNITS + 0.5 + SHIFT
    for q in polygons(otf, c):
        x = (q[:, 0] / fonttrace.UNITS + pen) * SS
        y = (baseline + 0.5 + SHIFT - q[:, 1] / fonttrace.UNITS) * SS
        im = Image.new("1", (W, H), 0)
        ImageDraw.Draw(im).polygon(list(zip(x - 0.5, y - 0.5)), fill=1)
        acc ^= np.asarray(im)
    big = acc.astype(np.float32)
    best = None
    for dy in range(-m, m + 1):
        for dx in range(-m, m + 1):
            win = big[m + dy:m + dy + h * SS, m + dx:m + dx + w * SS]
            o = win.reshape(h, SS, w, SS).mean((1, 3))
            err = np.abs(o - a).sum()
            if best is None or err < best[0]:
                best = (err, o, dx, dy)
    return best[1:] if offsets else best[1]


def difference(a, o):
    """RGB: shared ink dim gray, game-only red, OTF-only cyan, past NOISE."""
    base = np.minimum(a, o) * SHARED
    lost = np.clip((a - o - NOISE) * GAIN, 0, 1)
    added = np.clip((o - a - NOISE) * GAIN, 0, 1)
    rgb = np.dstack([base + lost, base + added, base + added])
    return (rgb.clip(0, 1) * 255).astype(np.uint8)


def tone_of(picked):
    """The median opacity inside a font's glyphs, two pixels in from their edges."""
    inner = [a[ndimage.binary_erosion(a > 0.5, iterations=2)] for a, *_ in picked.values()]
    inner = np.concatenate([v for v in inner if v.size]) if any(v.size for v in inner) else np.ones(1)
    return float(np.median(inner))


def masked(a, o, tone, edge=2.0, feather=0.0):
    """What mesh.frag draws for a masked font: the game's own pixels cut to the
    outline's coverage o, the font's tone standing in for them over the last
    edge texels inside it, and kept past it dying out over feather texels."""
    body = a
    if edge > 0:
        inside = ndimage.distance_transform_edt(o > 0.5) - 0.5
        t = np.clip((inside - 0.25 * edge) / (0.75 * edge), 0, 1)
        body = tone + (a - tone) * t * t * (3 - 2 * t)
    if feather > 0:
        o = np.maximum(o, np.clip(1 - ndimage.distance_transform_edt(o < 0.5) / feather, 0, 1))
    return o * body


def masked_edge(cfg):
    """A masked font's (edge, feather) in texels, None when it isn't masked."""
    s = cfg.get("ship")
    if not (isinstance(s, dict) and s.get("masked")):
        return None
    return float(s.get("edge", 2.0)), float(s.get("feather", 0.0))


def tiles(cfg, picked, otf, shape):
    """(char, game tile, difference tile, mismatch) per glyph, at screen size."""
    z = CELL_PX / shape[0]
    xmin = glyph_xmin(otf)
    edge = masked_edge(cfg)
    tone = tone_of(picked) if edge is not None else None
    out = []
    for c in cfg.get("order") or "".join(picked):
        if c not in picked or c not in xmin:
            continue
        a = picked[c][0].astype(np.float32)
        o = registered(otf, c, a, xmin[c], cfg["baseline"])
        if len(picked[c]) > 2:  # warped in from another scale: the font goes the same way
            n, p = picked[c][2]
            o = fonttrace.warp(fontrefine.to_native(o, n.shape, *p), a.shape, *p).astype(np.float32)
        if tone is not None:
            o = masked(a, o, tone, *edge)
        cols = np.nonzero(((a > 0.1) | (o > 0.1)).any(0))[0]
        x0, x1 = (max(0, cols[0] - PAD), min(a.shape[1], cols[-1] + 1 + PAD)) if len(cols) else (0, a.shape[1])
        size = (max(1, round((x1 - x0) * z)), round(a.shape[0] * z))
        game = Image.fromarray((a[:, x0:x1] * 255).astype(np.uint8)).resize(size, Image.BILINEAR)
        diff = Image.fromarray(difference(a[:, x0:x1], o[:, x0:x1])).resize(size, Image.BILINEAR)
        excess = float(np.clip(np.abs(a - o) - NOISE, 0, None).sum())
        out.append((c, np.asarray(game), np.asarray(diff), excess / max(float(a.sum()), 1e-6), excess, float(a.sum()),
                    texture(a)))
    return out


def texture(a):
    """Share of a glyph's interior (ink all round) that is partly see-through:
    chalk, grime, grain, which an outline can't carry."""
    ink = a > 0.1
    inner = ndimage.binary_erosion(ink, iterations=2)
    return float(((a < 0.75) & inner).sum() / max(inner.sum(), 1))


def strip(config, tex):
    cfg = json.loads(config.read_text())
    otf = config.parent / f"{cfg['name'].replace(' ', '')}.otf"
    picked, shape = fonttrace.pick(cfg, tex)
    lines, line, x = [], [], 0
    for t in tiles(cfg, picked, otf, shape):
        if line and x + t[1].shape[1] > WIDTH - LABEL_W:
            lines.append(line)
            line, x = [], 0
        line.append(t)
        x += t[1].shape[1] + 2
    return cfg["name"], lines + [line] if line else lines


def grade(m):
    """A shape mismatch's label colour: green, amber, red."""
    return (110, 220, 120) if m < 0.02 else (240, 200, 80) if m < 0.05 else (255, 90, 90)


def card(name, lines):
    """A font's scorecard: shape mismatch and texture over all its glyphs, the worst glyphs."""
    ts = [t for line in lines for t in line]
    ink = sum(t[5] for t in ts)
    worst = sorted(ts, key=lambda t: -t[3])[:8]
    return {"name": name, "shape_mismatch": sum(t[4] for t in ts) / max(ink, 1e-6),
            "texture": sum(t[6] * t[5] for t in ts) / max(ink, 1e-6),
            "glyphs_over_5pct": sum(t[3] >= 0.05 for t in ts), "glyphs": len(ts),
            "worst": [[t[0], round(t[3], 4)] for t in worst]}


def compose(sections, out):
    """sections: [(title, [line of tiles, ...])] stacked into one image."""
    lab = ImageFont.truetype(fonttrace.LABEL, 22)
    small = ImageFont.truetype(fonttrace.LABEL, 18)
    tiny = ImageFont.truetype(fonttrace.LABEL, 13)
    row_h = lambda line: line[0][1].shape[0]
    heads = []
    for title, ls in sections:
        sc = card(title, ls)
        heads.append(f"{title}   shape off by {sc['shape_mismatch'] * 100:.1f}%   texture {sc['texture'] * 100:.0f}%   "
                     f"RED = in the game, missing from the font   CYAN = in the font, not in the game   "
                     f"number = % of the glyph that's off")
    W = max(max(LABEL_W + sum(t[1].shape[1] + 2 for t in line) for _, ls in sections for line in ls),
            max(int(lab.getlength(h)) for h in heads)) + 20
    H = sum(40 + sum(2 * (row_h(line) + 4) + 34 for line in ls) for _, ls in sections) + 20
    img = Image.new("RGB", (W, H), (BG, BG, BG + 4))
    d = ImageDraw.Draw(img)
    y = 10
    for head, (title, ls) in zip(heads, sections):
        d.text((10, y + 6), head, font=lab, fill=(255, 220, 120))
        y += 40
        for line in ls:
            h = row_h(line)
            for name, k in (("game", 1), ("font errors", 2)):
                d.text((10, y + h // 2 - 10), name, font=small, fill=(170, 170, 170))
                x = LABEL_W
                for t in line:
                    img.paste(Image.fromarray(t[k]).convert("RGB"), (x, y))
                    x += t[1].shape[1] + 2
                y += h + 4
            x = LABEL_W
            for c, g, _, m, _, _, _ in line:
                text = f"{m * 100:.0f}"
                d.text((x + (g.shape[1] - d.textlength(text, font=tiny)) / 2, y), text, font=tiny, fill=grade(m))
                x += g.shape[1] + 2
            y += 34
    img.save(out)
    print(out.relative_to(ROOT), img.size)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("configs", type=Path, nargs="+")
    ap.add_argument("--src", type=Path, default=ROOT / ".planning/fonts/src")
    ap.add_argument("--overview", type=Path, help="also stack every font's first line here")
    ap.add_argument("--scorecard", type=Path, help="also write every font's scorecard here, as JSON")
    args = ap.parse_args()
    firsts, cards = [], []
    for config in args.configs:
        config = config.resolve()
        name, lines = strip(config, args.src / "tex")
        slug = config.parent.name + ("" if config.stem == "trace" else "_" + config.stem.removeprefix("trace_"))
        compose([(name, lines)], config.parent / f"{slug}_compare.png")
        firsts.append((name, lines[:1]))
        cards.append({**card(name, lines), "config": str(config.relative_to(ROOT))})
    if args.scorecard:
        args.scorecard.write_text(json.dumps(cards, indent=1, ensure_ascii=False))
    if args.overview:
        compose(firsts, args.overview.resolve())


if __name__ == "__main__":
    main()
