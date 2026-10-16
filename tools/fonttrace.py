#!/usr/bin/env python3
"""Trace a game font's glyph sheets into an OTF.

    tools/fonttrace.py fonts/<name>/trace.json [--src .planning/fonts/src]

Sources are font textures decoded from the discs (<src>/tex/<hash>.png,
RGBA). Each character comes from the first source listing it, or from the
one whose "take" names it, mapped into the first source's pixel space by a
scale and offset fitted on the letters both share. Outlines are fitted as
straight lines between sharp corners, with cubics only where an edge bends.

Writes <Name>.otf and <name>_compare.png (tools/fontstrip.py) beside the
config, and per-glyph scores and a per-glyph sheet into
.planning/fonts/out/<name>/.

Config keys:
    name          family name, also the OTF's file name
    ship          into the game's vector fonts (tools/fontpack.py): true, or
                  {font, layer, color, aliases} for one layer of several
    sources       [{tex, chars, cell: [w, h] in texture pixels, origin: [x, y],
                    cols, channel: alpha | white | black, take, exclude, tag,
                    boxes: {char: [x, y, w, h]} for packed fonts, cut into a cell's top left,
                    map: [sx, sy, dx, dy] pinned onto the master instead of fitted,
                    solid: fill enclosed holes, for a textured fill's silhouette}]
    metrics_from  sibling config of a layer drawn under or over this one: each glyph
                  takes that layer's ink bounds for its advance and placement, so the
                  two OTFs stack when set at the same origin
    order         characters to build, default every source character
    baseline      master-cell y of the baseline
    sidebearing   master pixels each side of a glyph's ink
    space         master pixels a space advances
    sample        text the compare sheet sets in the built font
    fit           {blur, step, corner_window, corner_angle, trim,
                   line_tol, curve_tol, snap, min_area,
                   smooth: curves for drawn fonts (SMOOTH presets; value is the
                     smoothing in px before corners are found),
                   upsample, corner_scale, line_min: smooth-mode knobs,
                   max_dev: px a loop's fit may stray before it falls back to a polygon,
                   level: coverage the outline follows (default 0.5); for chalk,
                     half the stroke body's coverage, scored at the same level,
                   split_depth: smooth mode's cubic split limit (default 6), deeper
                     lets a lopsided run keep splitting instead of giving up,
                   tidy: smooth mode with corners kept sharp and cubics merged to the
                     fewest within curve_tol (tangent_span, corner_snap px; fit_tidy),
                   local: px sigma; coverage is divided by the nearby ink's mean (never
                     below local_floor, default 0.3) before level applies, scored the same,
                   dots: radius, for a dot-matrix font drawn as true circles,
                   dot_pitch: [x, y] px of the dot grid; centers snap to it (dot_lattice),
                   poly: px tolerance; every loop a polygon of refitted straight sides
                     meeting at sharp corners, for cut-paper fonts (poly_min, poly_trim,
                     snap; fit_poly),
                   split: px; smooth mode for type designs: runs split into lines (stretches
                     this long within line_tol, free_line long when touching no corner,
                     within axis_snap degrees squared up) and G1 curves (fit_split);
                     extrema: nodes at level/plumb points and 45 degree shoulders,
                     corner_merge: px, one corner for two found this close, min_seg: px,
                     a shorter cubic is refitted away, split_caps: degrees, a thin stroke's
                     square end found as one corner becomes two,
                   small_fit / hole_fit: {size, fit overrides} for outer loops / counters
                     no bigger than size px, like accents or a small font's slit counters}
    glyph_fit     {chars: {fit overrides}}, e.g. less blur where a counter is a thin slit
    svg           {char: file beside the config}: that glyph drawn, not traced (svg_loops)
    compose       {chars: base}: these accented letters take the base letter's trace for
                  every loop the two share, moved onto it, so a letter and its accented
                  forms match; compose_tol [box px, area fraction] is how alike a loop must be
    fit straights {min, min_diag, diag_tol, sag, reach, bend, flank, flank_angle, diag_flank,
                   axis, snap}: smooth mode with straight stems inside curved runs (fit_straights)
"""
import argparse
import json
import re
from pathlib import Path

import numpy as np
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.t2CharStringPen import T2CharStringPen
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage
from skimage import feature, filters, measure, transform

ROOT = Path(__file__).resolve().parent.parent
UNITS = 10  # font units per master pixel
SS = 8      # supersampling when rasterizing a trace back
FIT = {"blur": 0.7, "step": 0.5, "corner_window": 4, "corner_angle": 15, "trim": 2,
       "line_tol": 0.5, "curve_tol": 0.4, "snap": 2.5, "min_area": 3}
# "smooth": drawn fonts, curves through everything but real corners
SMOOTH = {"blur": 0, "upsample": 4, "step": 0.25, "smooth": 0.4, "corner_scale": 1.5, "corner_angle": 45,
          "curve_tol": 0.3, "line_tol": 0.06, "max_dev": 0.8, "min_area": 1}
LABEL = "/System/Library/Fonts/Supplemental/Arial.ttf"


def fit_of(cfg, c=None):
    """A config's fit settings over the defaults, or over SMOOTH when it asks for smooth,
    with glyph_fit's overrides for character c on top."""
    own = cfg.get("fit", {})
    extra = {k: v for chars, o in cfg.get("glyph_fit", {}).items() if c is not None and c in chars for k, v in o.items()}
    return {**FIT, **(SMOOTH if own.get("smooth") else {}), **own, **extra}


def coverage(rgba, channel):
    a = rgba[..., 3] / 255
    lum = rgba[..., :3].mean(-1) / 255
    return {"alpha": a, "white": a * lum, "black": a * (1 - lum)}[channel]


def load(src, tex_dir):
    rgba = np.asarray(Image.open(tex_dir / f"{src['tex']}.png").convert("RGBA")).astype(float)
    a = coverage(rgba, src.get("channel", "alpha"))
    cw, ch = src["cell"]
    ox, oy = src.get("origin", (0, 0))
    cols = src.get("cols", int((a.shape[1] - ox) // cw))
    out = {}
    for i, c in enumerate(src["chars"]):
        if c == " " or c in src.get("exclude", ""):
            continue
        x, y = ox + (i % cols) * cw, oy + (i // cols) * ch
        bw, bh = cw, ch
        if c in src.get("boxes", {}):
            x, y, bw, bh = src["boxes"][c]
        cell = np.zeros((int(round(ch)), int(round(cw))))
        part = a[int(round(y)):int(round(y + bh)), int(round(x)):int(round(x + bw))]
        cell[:part.shape[0], :part.shape[1]] = part
        if src.get("solid"):
            cell[ndimage.binary_fill_holes(cell > 0.5)] = 1
        if cell.max() > 0.5:
            out[c] = cell
    return out


def warp(a, shape, sx, sy, dx, dy):
    """a resampled into a master cell: master (x, y) = (sx * x + dx, sy * y + dy)."""
    m = transform.AffineTransform(matrix=np.array([[sx, 0, dx], [0, sy, dy], [0, 0, 1]]))
    return transform.warp(a, m.inverse, output_shape=shape, order=3, mode="constant", cval=0, preserve_range=True).clip(0, 1)


def fit_mapping(master, other, shared):
    """Scale and offset taking other's cells onto master's, best IoU over the shared glyphs."""
    shape = next(iter(master.values())).shape
    ratio = shape[0] / next(iter(other.values())).shape[0]
    if len(shared) < 3:
        return None, (ratio, ratio, 0.0, 0.0)

    def iou(p):
        tot_i = tot_u = 0.0
        for c in shared:
            w = warp(other[c], shape, *p) > 0.5
            m = master[c] > 0.5
            tot_i += (w & m).sum()
            tot_u += (w | m).sum()
        return tot_i / tot_u

    best = (iou((ratio, ratio, 0, 0)), (ratio, ratio, 0.0, 0.0))
    for step in (0.04, 0.02, 0.01, 0.005):
        for _ in range(6):
            sx, sy, dx, dy = best[1]
            for p in [(sx + a * step * ratio, sy + b * step * ratio, dx + c * step * 40, dy + d * step * 40)
                      for a in (-1, 0, 1) for b in (-1, 0, 1) for c in (-1, 0, 1) for d in (-1, 0, 1)]:
                s = iou(p)
                if s > best[0]:
                    best = (s, p)
    return best


# ---- outline fitting -------------------------------------------------------

def resample(pts, step):
    d = np.r_[0, np.cumsum(np.hypot(*np.diff(pts, axis=0).T))]
    n = max(8, int(d[-1] / step))
    t = np.linspace(0, d[-1], n, endpoint=False)
    return np.c_[np.interp(t, d, pts[:, 0]), np.interp(t, d, pts[:, 1])]


def corners(p, k, thr):
    n = len(p)
    a = p - np.roll(p, k, 0)
    b = np.roll(p, -k, 0) - p
    ang = np.abs(np.arctan2(a[:, 0] * b[:, 1] - a[:, 1] * b[:, 0], (a * b).sum(1)))
    out = []
    for i in range(n):
        win = [ang[(i + j) % n] for j in range(-k, k + 1)]
        if ang[i] > thr and ang[i] == max(win) and not (out and i - out[-1] <= k):
            out.append(i)
    return out


def line_fit(pts):
    c = pts.mean(0)
    _, _, vt = np.linalg.svd(pts - c)
    d = vt[0]
    return c, d, np.abs((pts - c) @ np.array([-d[1], d[0]])).max()


def intersect(c1, d1, c2, d2):
    m = np.array([d1, -d2]).T
    if abs(np.linalg.det(m)) < 1e-6:
        return None
    t = np.linalg.solve(m, c2 - c1)
    return c1 + t[0] * d1


def bezier(ctrl, t):
    t = t[:, None]
    return ((1 - t) ** 3 * ctrl[0] + 3 * (1 - t) ** 2 * t * ctrl[1]
            + 3 * (1 - t) * t ** 2 * ctrl[2] + t ** 3 * ctrl[3])


def fit_cubic(pts, t0, t1, tol, depth=0, max_depth=6):
    """Schneider's curve fit: cubics through pts with end tangents t0, t1."""
    if len(pts) < 3:
        return [np.array([pts[0], pts[0] + (pts[-1] - pts[0]) / 3, pts[-1] - (pts[-1] - pts[0]) / 3, pts[-1]])]
    u = np.r_[0, np.cumsum(np.hypot(*np.diff(pts, axis=0).T))]
    u /= u[-1]
    for _ in range(8):
        b = np.array([(1 - u) ** 3, 3 * (1 - u) ** 2 * u, 3 * (1 - u) * u ** 2, u ** 3]).T
        a1, a2 = b[:, 1:2] * t0, b[:, 2:3] * t1
        rest = pts - (b[:, :1] + b[:, 1:2]) * pts[0] - (b[:, 2:3] + b[:, 3:]) * pts[-1]
        c = np.array([[(a1 * a1).sum(), (a1 * a2).sum()], [(a1 * a2).sum(), (a2 * a2).sum()]])
        x = np.array([(a1 * rest).sum(), (a2 * rest).sum()])
        det = np.linalg.det(c)
        chord = np.hypot(*(pts[-1] - pts[0]))
        al = np.linalg.solve(c, x) if abs(det) > 1e-9 else np.array([chord / 3, chord / 3])
        if (al < chord * 1e-3).any():
            al = np.array([chord / 3, chord / 3])
        ctrl = np.array([pts[0], pts[0] + al[0] * t0, pts[-1] + al[1] * t1, pts[-1]])
        # Newton step on the parameters
        q = bezier(ctrl, u)
        d1 = 3 * np.array([(1 - u) ** 2, 2 * (1 - u) * u, u ** 2]).T @ np.diff(ctrl, axis=0)
        d2 = 6 * np.array([1 - u, u]).T @ np.diff(ctrl, 2, axis=0)
        num = ((q - pts) * d1).sum(1)
        den = (d1 * d1).sum(1) + ((q - pts) * d2).sum(1)
        u = np.clip(u - np.where(np.abs(den) > 1e-9, num / den, 0), 0, 1)
        u[0], u[-1] = 0, 1
        u = np.maximum.accumulate(u)
    err = np.hypot(*(bezier(ctrl, u) - pts).T)
    if err.max() <= tol or depth > max_depth or len(pts) < 6:
        return [ctrl]
    i = int(np.clip(err.argmax(), 2, len(pts) - 3))
    tm = pts[i + 1] - pts[i - 1]
    tm /= np.hypot(*tm)
    return (fit_cubic(pts[:i + 1], t0, -tm, tol, depth + 1, max_depth)
            + fit_cubic(pts[i:], tm, t1, tol, depth + 1, max_depth))


def tangent(pts, n):
    v = pts[min(n, len(pts) - 1)] - pts[0]
    return v / (np.hypot(*v) or 1)


def fit_contour(p, cfg):
    """A closed point loop as segments: ('L', a, b) and ('C', ctrl)."""
    k = cfg["corner_window"]
    cs = corners(p, k, np.radians(cfg["corner_angle"]))
    n = len(p)
    if not cs:
        cs = [0]
    runs = []
    for j, s in enumerate(cs):
        e = cs[(j + 1) % len(cs)]
        idx = [(s + i) % n for i in range(((e - s) % n or n) + 1)]
        runs.append(p[idx])
    trim = cfg["trim"]
    kinds = []
    for r in runs:
        inner = r[trim:len(r) - trim] if len(r) > 2 * trim + 3 else r
        c, d, dev = line_fit(inner)
        kinds.append(("L", c, d) if dev <= cfg["line_tol"] else ("C",))
    # corner positions: lines meeting lines are extended to their intersection
    verts = []
    for j in range(len(runs)):
        a, b = kinds[j - 1], kinds[j]
        v = runs[j][0]
        if a[0] == "L" and b[0] == "L" and len(runs) > 1:
            x = intersect(a[1], a[2], b[1], b[2])
            if x is not None and np.hypot(*(x - v)) <= cfg["snap"]:
                v = x
        elif b[0] == "L" and a[0] == "C":
            v = b[1] + ((v - b[1]) @ b[2]) * b[2]
        elif a[0] == "L" and b[0] == "C":
            v = a[1] + ((v - a[1]) @ a[2]) * a[2]
        verts.append(v)
    segs = []
    for j, r in enumerate(runs):
        v0, v1 = verts[j], verts[(j + 1) % len(runs)]
        if kinds[j][0] == "L":
            segs.append(("L", v0, v1))
            continue
        pts = r.copy()
        pts[0], pts[-1] = v0, v1
        a, b = kinds[j - 1], kinds[(j + 1) % len(runs)]
        t0 = (a[2] if (a[2] @ (pts[1] - pts[0])) > 0 else -a[2]) if a[0] == "L" else tangent(pts, 3)
        t1 = (b[2] if (b[2] @ (pts[-2] - pts[-1])) > 0 else -b[2]) if b[0] == "L" else tangent(pts[::-1], 3)
        segs += [("C", ctrl) for ctrl in fit_cubic(pts, t0, t1, cfg["curve_tol"])]
    return segs


def fit_smooth(p, cfg):
    """A closed point loop as curves, for drawn fonts: corners only where the edge
    turns sharply over corner_scale pixels, one tangent-continuous run of cubics
    between them, and lines only where a run is truly straight."""
    if cfg.get("split"):
        return fit_split(p, cfg)
    if cfg.get("tidy"):
        return fit_tidy(p, cfg)
    if cfg.get("straights"):
        return fit_straights(p, cfg)
    step = cfg["step"]
    n = len(p)
    ps = ndimage.gaussian_filter1d(p, cfg.get("smooth", 0.4) / step, axis=0, mode="wrap")
    k = max(2, round(cfg.get("corner_scale", 1.5) / step))
    cs = corners(ps, k, np.radians(cfg["corner_angle"]))
    sharp = bool(cs)
    if not cs:
        cs = [0, n // 2]

    def tan(i, d):
        """Unit tangent at sample i, looking d samples along the loop."""
        v = ps[(i + d) % n] - ps[i % n] if sharp else ps[(i + d) % n] - ps[(i - d) % n]
        return v / (np.hypot(*v) or 1)

    segs = []
    for j, s in enumerate(cs):
        e = cs[(j + 1) % len(cs)]
        idx = [(s + i) % n for i in range(((e - s) % n or n) + 1)]
        run = ps[idx].copy()
        run[0], run[-1] = (p[s], p[e]) if sharp else (ps[s], ps[e])
        if len(run) < 4:
            segs.append(("L", run[0], run[-1]))
            continue
        _, _, dev = line_fit(run)
        if dev <= cfg["line_tol"] and np.hypot(*(run[-1] - run[0])) >= cfg.get("line_min", 2.0):
            segs.append(("L", run[0], run[-1]))
            continue
        m = min(k, len(run) // 3)
        t0 = tan(s, m)
        t1 = -tan(e, -m) if sharp else -tan(e, m)
        segs += [("C", ctrl) for ctrl in fit_cubic(run, t0, t1, cfg["curve_tol"], max_depth=cfg.get("split_depth", 6))]
    return segs


def fit_straights(p, cfg):
    """Smooth mode for type with straight stems inside curved runs: a stretch of a run
    that stays within line_tol (diag_tol off the axes) becomes a line, axis-snapped within
    "axis" degrees, joined to tangent-continuous cubics; lines meeting at a corner are
    extended to meet. A stretch is refused when it bows (sag), when one parabola carries
    on through it and reach px either side (bend: the flat top of a bowl), when an axis
    stretch's ends both leave it by less than flank_angle over flank px, or when a
    diagonal's do by less than diag_flank (unless by a corner). Loops without corners
    split at their top and bottom. cfg["straights"]: {min, min_diag, diag_tol, sag,
    reach, bend, flank, flank_angle, diag_flank, axis, snap, whole, whole_tol}."""
    st = cfg["straights"]
    step = cfg["step"]
    n = len(p)
    ps = ndimage.gaussian_filter1d(p, cfg.get("smooth", 0.4) / step, axis=0, mode="wrap")
    k = max(2, round(cfg.get("corner_scale", 1.5) / step))
    cs = corners(ps, k, np.radians(cfg["corner_angle"]))
    sharp = bool(cs)
    if not cs:
        cs = sorted({int(ps[:, 1].argmin()), int(ps[:, 1].argmax())})
        if len(cs) < 2:
            cs = [0, n // 2]
    tol, dtol = cfg["line_tol"], st.get("diag_tol", cfg["line_tol"])
    fl = max(2, round(st.get("flank", 3) / step))
    ext = max(2, round(st.get("reach", 3) / step))
    axis = np.radians(st.get("axis", 0))
    snap = st.get("snap", cfg["snap"])

    def line_of(r):
        """Best line through r, oriented along it and snapped to an axis when near one."""
        c, d, dev = line_fit(r)
        d = d if d @ (r[-1] - r[0]) >= 0 else -d
        a = np.arctan2(d[1], d[0])
        q = np.round(a / (np.pi / 2)) * (np.pi / 2)
        on = abs(a - q) <= axis
        if on:
            d = np.array([np.cos(q), np.sin(q)]).round(12)
            dev = np.abs((r - c) @ np.array([-d[1], d[0]])).max()
        return c, d, dev <= (tol if on else dtol), on

    def frame(r, c, d):
        return (r - c) @ d, (r - c) @ np.array([-d[1], d[0]])

    def sag(r, c, d):
        """Sagitta of the parabola through a stretch's middle 80%: what bends, not stair noise."""
        q = r[len(r) // 10:len(r) - len(r) // 10] if len(r) >= 10 else r
        t, h = frame(q, c, d)
        return 0.0 if len(q) < 4 or np.ptp(t) == 0 else abs(np.polyfit(t, h, 2)[0]) * (np.ptp(t) / 2) ** 2

    def bend(r, c, d):
        """Worst miss of one parabola through a stretch and its flanks."""
        t, h = frame(r, c, d)
        return np.inf if np.ptp(t) == 0 else np.abs(np.polyval(np.polyfit(t, h, 2), t) - h).max()

    def turned(d, v):
        return np.degrees(np.arccos(np.clip(d @ v / (np.hypot(*v) or 1), -1, 1)))

    runs = []
    for j, s in enumerate(cs):
        e = cs[(j + 1) % len(cs)]
        idx = [(s + i) % n for i in range(((e - s) % n or n) + 1)]
        run = ps[idx].copy()
        run[0], run[-1] = (p[s], p[e]) if sharp else (ps[s], ps[e])
        m = len(run)
        reach, jj = [], 1  # end of the longest straight stretch from each sample
        for i in range(m):
            jj = max(jj, i + 1)
            while jj + 1 < m and line_of(run[i:jj + 2])[2]:
                jj += 1
            reach.append(min(jj, m - 1))
        pieces, taken = [], np.zeros(m, bool)
        for i in sorted(range(m), key=lambda i: i - reach[i]):
            a, b = i, reach[i]
            while a < b and taken[a]:
                a += 1
            while b > a and taken[b]:
                b -= 1
            if taken[a:b + 1].any() or b - a < 2:
                continue
            c, d, ok, on = line_of(run[a:b + 1])
            sg = sag(run[a:b + 1], c, d)
            # a run straight but for its rounded-off ends is one line, corner to corner
            whole = (sharp and b - a >= st.get("whole", 0.75) * (m - 1)
                     and np.abs(frame(run, c, d)[1]).max() <= st.get("whole_tol", 0.5))
            if whole:
                a, b = 0, m - 1
            length = np.hypot(*(run[b] - run[a]))
            if not ok or sg > st.get("sag", 0.1) or length < (
                    cfg.get("line_min", 2.0) if whole else st.get("min", 4) if on else st.get("min_diag", 4)):
                continue
            lo, hi = max(0, a - ext), min(m - 1, b + ext)
            bd = np.inf if (a - lo < ext and hi - b < ext) else bend(run[lo:hi + 1], c, d)
            f0 = 180 if (sharp and a - lo < ext) or a < fl else turned(-d, run[a - fl] - run[a])
            f1 = 180 if (sharp and hi - b < ext) or b + fl >= m else turned(d, run[b + fl] - run[b])
            if not whole and (bd <= st.get("bend", 0.2)
                              or (on and max(f0, f1) < st.get("flank_angle", 0))
                              or (not on and min(f0, f1) < st.get("diag_flank", 0))):
                continue
            pieces.append((a, b, c, d))
            taken[max(0, a - 1):b + 2] = True
        runs.append((s, e, run, sorted(pieces, key=lambda q: q[0])))

    proj = lambda c, d, q: c + ((q - c) @ d) * d
    starts = []  # each run's first vertex; None where two lines at a corner do not meet nearby
    for j, (s, e, run, pieces) in enumerate(runs):
        prev = runs[j - 1][3]
        a_line = prev[-1] if prev and prev[-1][1] == len(runs[j - 1][2]) - 1 else None
        b_line = pieces[0] if pieces and pieces[0][0] == 0 else None
        v = run[0]
        if a_line and b_line and sharp:
            x = intersect(a_line[2], a_line[3], b_line[2], b_line[3])
            v = x if x is not None and np.hypot(*(x - v)) <= snap else None
        elif b_line:
            v = proj(b_line[2], b_line[3], v)
        elif a_line:
            v = proj(a_line[2], a_line[3], v)
        starts.append(v)

    def tan(i, d):
        v = ps[(i + d) % n] - ps[i % n] if sharp else ps[(i + d) % n] - ps[(i - d) % n]
        return v / (np.hypot(*v) or 1)

    gtol = 1.5 * cfg["curve_tol"]

    def gap_fit(g, t0, t1, depth=0):
        """Cubics through a gap held to its neighbours' tangents; where they miss the
        edge by more than gtol the gap splits near its worst point, smooth through it."""
        contour = [(None, [("L", g[i], g[i + 1]) for i in range(len(g) - 1)])]
        out = [("C", q) for q in fit_cubic(g, t0, t1, cfg["curve_tol"])]
        dist = edge_distance(g, [(None, out)])
        if dist.max() <= gtol and edge_distance(flatten(out, 24), contour).max() <= gtol:
            return out
        kk = max(1, min(len(g) // 4, round(0.5 / step)))
        if len(g) >= 4 * kk + 4 and depth < 6:
            i = int(np.clip(dist.argmax(), max(2 * kk, len(g) // 4), min(len(g) - 1 - 2 * kk, 3 * len(g) // 4)))
            tm = unit(g[i + kk] - g[i - kk])
            return gap_fit(g[:i + 1], t0, -tm, depth + 1) + gap_fit(g[i:], tm, t1, depth + 1)
        out = [("C", q) for q in fit_cubic(g, tangent(g, 1), tangent(g[::-1], 1), cfg["curve_tol"])]
        if edge_distance(g, [(None, out)]).max() <= gtol:
            return out
        poly = measure.approximate_polygon(g, cfg["curve_tol"] / 2)
        return [("L", poly[i], poly[i + 1]) for i in range(len(poly) - 1)]

    segs = []
    for j, (s, e, run, pieces) in enumerate(runs):
        m = len(run)
        v0, v1 = starts[j], starts[(j + 1) % len(runs)]
        if v0 is None:
            prev = runs[j - 1][3][-1]
            v0 = proj(pieces[0][2], pieces[0][3], run[0])
            segs.append(("L", proj(prev[2], prev[3], run[0]), v0))
        if v1 is None:
            v1 = proj(pieces[-1][2], pieces[-1][3], run[-1])
        cur, cur_i, cur_t = v0, 0, None
        mm = min(k, max(1, m // 3))
        for a, b, c, d in pieces:
            pa = cur if a == 0 else proj(c, d, run[a])
            pb = v1 if b == m - 1 else proj(c, d, run[b])
            if a > cur_i:
                gap = run[cur_i:a + 1].copy()
                if len(gap) < 5 and cur_t is not None:  # two lines a hair apart: meet instead
                    x = intersect(cur, cur_t, c, d)
                    if x is not None and np.hypot(*(x - run[a])) <= snap:
                        segs[-1] = ("L", segs[-1][1], x)
                        pa = cur = x
                if not np.allclose(cur, pa):
                    gap[0], gap[-1] = cur, pa
                    segs += gap_fit(gap, cur_t if cur_t is not None else tan(s, mm), -d)
            segs.append(("L", pa, pb))
            cur, cur_i, cur_t = pb, b, d
        if cur_i < m - 1:
            gap = run[cur_i:].copy()
            gap[0], gap[-1] = cur, v1
            if len(gap) < 4:
                segs.append(("L", cur, v1))
            else:
                segs += gap_fit(gap, cur_t if cur_t is not None else tan(s, mm),
                                -tan(e, -mm) if sharp else -tan(e, mm))
    return segs


def unit(v):
    return v / (np.hypot(*v) or 1)


def smooth_open(run, sig):
    """Gaussian along an open run with its ends pinned; the odd extension past each
    end keeps the end's heading, so smoothing never rounds a corner off."""
    if sig <= 0 or len(run) < 4:
        return run.copy()
    m = min(len(run) - 1, int(np.ceil(sig * 3)) + 1)
    ext = np.concatenate([2 * run[0] - run[1:m + 1][::-1], run, 2 * run[-1] - run[-m - 1:-1][::-1]])
    out = ndimage.gaussian_filter1d(ext, sig, axis=0, mode="nearest")[m:m + len(run)]
    out[0], out[-1] = run[0], run[-1]
    return out


def end_tangent(run, n, skip=0):
    """Heading leaving run[0], a line fitted through n samples after the first skip,
    as (a point on it, unit direction)."""
    skip = min(skip, max(0, len(run) - 3))
    seg = run[skip:skip + max(3, min(n, len(run) - skip))]
    c, d, _ = line_fit(seg)
    return c, (d if d @ (seg[-1] - seg[0]) >= 0 else -d)


def cubic_err(ctrl, pts):
    """Each point's distance to the cubic, densely sampled."""
    q = bezier(ctrl, np.linspace(0, 1, max(64, 4 * len(pts))))
    return np.sqrt(((pts[:, None] - q[None]) ** 2).sum(-1)).min(1)


def fit_run(pts, t0, t1, tol, kt):
    """Tangent-continuous cubics through an open run within tol px, as few as hold:
    each reaches as far along the run as one cubic can, then every joint slides to
    where its two neighbours fit best, so nodes spread out instead of bunching
    around a bend. A joint's heading is read kt samples either side."""
    n = len(pts)

    def one(i0, i1, a, b):
        ctrl = fit_cubic(pts[i0:i1 + 1], a, b, tol, max_depth=-1)[0]
        return ctrl, cubic_err(ctrl, pts[i0:i1 + 1]).max()

    def tan(i):
        w = max(1, min(kt, i, n - 1 - i))
        return unit(pts[i + w] - pts[i - w])

    def piece(j0, j1):
        return one(j0, j1, t0 if j0 == 0 else tan(j0), t1 if j1 == n - 1 else -tan(j1))

    joints, i0 = [0], 0
    while n >= 8 and piece(i0, n - 1)[1] > tol and i0 + 3 < n - 4:
        lo, hi, best = i0 + 3, n - 4, i0 + 3
        while lo <= hi:
            m = (lo + hi) // 2
            if piece(i0, m)[1] <= tol:
                best, lo = m, m + 1
            else:
                hi = m - 1
        joints.append(best)
        i0 = best
    joints.append(n - 1)
    for _ in range(2):
        for q in range(1, len(joints) - 1):
            lo, hi = joints[q - 1] + 3, joints[q + 1] - 3
            if lo <= hi:
                cands = set(np.linspace(lo, hi, min(24, hi - lo + 1)).astype(int))
                joints[q] = min(cands, key=lambda j: max(piece(joints[q - 1], j)[1], piece(j, joints[q + 1])[1]))
    return [piece(j0, j1)[0] for j0, j1 in zip(joints, joints[1:])]


def fit_tidy(p, cfg):
    """Smooth mode with fewer, better placed nodes ("tidy"): runs between corners are
    smoothed on their own so corners stay sharp, each corner sits where the headings
    of its two runs meet (within corner_snap px), and the fewest cubics that hold
    curve_tol are spread along each run (fit_run). tangent_span px sets how far headings are read, and
    the first corner_round px past a corner, where the source's antialiasing rounds
    it, are left out of both headings and fit."""
    step, n = cfg["step"], len(p)
    sig = cfg.get("smooth", 0.4) / step
    ps = ndimage.gaussian_filter1d(p, sig, axis=0, mode="wrap")
    k = max(2, round(cfg.get("corner_scale", 1.5) / step))
    kt = max(2, round(cfg.get("tangent_span", 1.0) / step))
    kr = round(cfg.get("corner_round", 0) / step)
    cs = corners(ps, k, np.radians(cfg["corner_angle"]))
    if cfg.get("corner_ratio"):
        # a corner turns as much over twice the span; a tight round bend turns about twice as much
        def turn(i, m):
            a, b = ps[i] - ps[(i - m) % n], ps[(i + m) % n] - ps[i]
            return abs(np.arctan2(a[0] * b[1] - a[1] * b[0], a @ b))
        cs = [i for i in cs if turn(i, 2 * k) < cfg["corner_ratio"] * turn(i, k)]
    segs = []
    if not cs:
        # no corner: two runs from the topmost point, headings from the closed loop
        top = int(np.argmin(ps[:, 1]))
        cs = [top, (top + n // 2) % n]
        for s, e in (cs, cs[::-1]):
            run = ps[[(s + i) % n for i in range((e - s) % n + 1)]]
            t0 = unit(ps[(s + kt) % n] - ps[(s - kt) % n])
            t1 = -unit(ps[(e + kt) % n] - ps[(e - kt) % n])
            segs += [("C", c) for c in fit_run(run, t0, t1, cfg["curve_tol"], kt)]
        return segs
    runs = []
    for j, s in enumerate(cs):
        e = cs[(j + 1) % len(cs)]
        runs.append(smooth_open(p[[(s + i) % n for i in range(((e - s) % n or n) + 1)]], sig))
    skips = [kr if len(r) > 4 * kr + 8 else 0 for r in runs]
    heads = [(end_tangent(r, 2 * kt, s), end_tangent(r[::-1], 2 * kt, s)) for r, s in zip(runs, skips)]
    verts = []
    for j, r in enumerate(runs):
        x = intersect(*heads[j - 1][1], *heads[j][0])
        verts.append(x if x is not None and np.hypot(*(x - r[0])) <= cfg.get("corner_snap", 0.6) else r[0])
    for j, r in enumerate(runs):
        r = r.copy()
        r[0], r[-1] = verts[j], verts[(j + 1) % len(runs)]
        if len(r) < 4:
            segs.append(("L", r[0], r[-1]))
            continue
        if line_fit(r)[2] <= cfg["line_tol"] and np.hypot(*(r[-1] - r[0])) >= cfg.get("line_min", 2.0):
            segs.append(("L", r[0], r[-1]))
            continue
        s = skips[j]
        if s:
            r = np.concatenate([r[:1], r[1 + s:len(r) - 1 - s], r[-1:]])
        segs += [("C", c) for c in fit_run(r, heads[j][0][1], heads[j][1][1], cfg["curve_tol"], kt)]
    return segs


def fit_split(p, cfg):
    """Smooth mode for type designs: each run between corners split into straight
    stretches (split px or longer within line_tol of a line, lines within axis_snap
    degrees of level or plumb squared up) and curves leaving each line on its tangent.
    A run that is straight corner to corner is one line at any length, and lines
    meeting at a corner are extended to cross there."""
    step, n = cfg["step"], len(p)
    ps = ndimage.gaussian_filter1d(p, cfg.get("smooth", 0.4) / step, axis=0, mode="wrap")
    k = max(2, round(cfg.get("corner_scale", 1.5) / step))
    cs = corners(ps, k, np.radians(cfg["corner_angle"]))
    gap = cfg.get("corner_merge", 0) / step
    while len(cs) > 1 and gap:
        # one rounded source corner found twice: a corner midway
        d = [(cs[(i + 1) % len(cs)] - cs[i]) % n for i in range(len(cs))]
        i = int(np.argmin(d))
        if d[i] > gap:
            break
        mid = (cs[i] + d[i] // 2) % n
        cs = sorted([c for j, c in enumerate(cs) if j not in (i, (i + 1) % len(cs))] + [mid])
    if cfg.get("split_caps") and cs:
        # a thin stroke's square end turns twice inside one corner window and reads as
        # one hairpin: two corners where it turns at half the scale
        h = max(1, k // 2)
        a, b = ps - np.roll(ps, h, 0), np.roll(ps, -h, 0) - ps
        ang = np.abs(np.arctan2(a[:, 0] * b[:, 1] - a[:, 1] * b[:, 0], (a * b).sum(1)))
        a, b = ps - np.roll(ps, 2 * k, 0), np.roll(ps, -2 * k, 0) - ps
        big = np.abs(np.arctan2(a[:, 0] * b[:, 1] - a[:, 1] * b[:, 0], (a * b).sum(1)))
        out = []
        for c in cs:
            if big[c] > np.radians(cfg["split_caps"]):
                lo = [(c - j) % n for j in range(1, 2 * k + 1)]
                hi = [(c + j) % n for j in range(1, 2 * k + 1)]
                out += [max(lo, key=lambda i: ang[i]), max(hi, key=lambda i: ang[i])]
            else:
                out.append(c)
        cs = sorted(set(out))
    sharp = bool(cs)
    if not sharp:
        # one run, starting where the loop bends most so no straight stretch wraps
        a, b = ps - np.roll(ps, k, 0), np.roll(ps, -k, 0) - ps
        cs = [int(ps[:, 1].argmax() if cfg.get("extrema") else np.abs(a[:, 0] * b[:, 1] - a[:, 1] * b[:, 0]).argmax())]
    tol, w = cfg["line_tol"], max(3, round(cfg["split"] / step))
    cos_snap = np.cos(np.radians(cfg.get("axis_snap", 0)))
    dev = lambda ix: line_fit(p[ix])[2]

    def line(ix):
        c, d, _ = line_fit(p[ix])
        for ax in np.eye(2):
            if abs(d @ ax) >= cos_snap:
                d = ax * np.sign(d @ ax)
        return c, d

    pieces = []  # (kind, first sample, last sample, line)
    for j, s in enumerate(cs):
        e = cs[(j + 1) % len(cs)]
        idx = np.array([(s + i) % n for i in range(((e - s) % n or n) + 1)])
        L = len(idx)
        found = []
        # a short run must be straighter: a small fillet bows little but is still a curve
        span = np.hypot(*(p[e] - p[s])) / cfg["split"]
        if L < 4 or (sharp and dev(idx[k:L - k] if L > 3 * k else idx) <= tol * min(1.5, max(0.4, span))):
            found = [(0, L - 1)]
        elif L > w:
            used = np.zeros(L, bool)
            devs = np.array([dev(idx[i:i + w + 1]) for i in range(L - w)])
            for i in np.argsort(devs):
                if devs[i] > tol:
                    break
                if used[i:i + w + 1].any():
                    continue
                a, b = i, i + w
                while a > 0 and not used[a - 1] and dev(idx[a - 1:b + 1]) <= tol:
                    a -= 1
                while b < L - 1 and not used[b + 1] and dev(idx[a:b + 2]) <= tol:
                    b += 1
                # end the line where the edge is still on it, not where a curve starts leaving
                c, d, _ = line_fit(p[idx[a:b + 1]])
                gone = lambda q: abs((p[idx[q]] - c) @ np.array([-d[1], d[0]])) > tol / 2
                while b - a > w and gone(a):
                    a += 1
                while b - a > w and gone(b):
                    b -= 1
                if sharp:
                    # run on into a corner the stretch nearly reaches
                    c, d, _ = line_fit(p[idx[a:b + 1]])
                    off = lambda ix: np.abs((p[ix] - c) @ np.array([-d[1], d[0]])).max()
                    a = 0 if a <= k or (not used[:a].any() and off(idx[:a + 1]) <= 2 * tol) else a
                    b = L - 1 if b >= L - 1 - k or (not used[b:].any() and off(idx[b:]) <= 2 * tol) else b
                used[a:b + 1] = True
                free = not (sharp and (a == 0 or b == L - 1))
                if not free or np.hypot(*(p[idx[b]] - p[idx[a]])) >= cfg.get("free_line", cfg["split"]):
                    found.append((a, b))
        merged = []
        for a, b in sorted(found):
            # one straight edge broken by a wobble: one line
            if merged and dev(idx[merged[-1][0]:b + 1]) <= 1.5 * tol:
                merged[-1] = (merged[-1][0], b)
            else:
                merged.append((a, b))
        # a short straight gap left between a line and a corner is a line too
        gaps, pos = [], 0
        for a, b in merged + [(L - 1, L - 1)]:
            g0, g1 = pos + (k if pos == 0 and sharp else 0), a - (k if a == L - 1 and sharp else 0)
            if a - pos >= 4 and g1 - g0 >= 3 and np.hypot(*(p[idx[a]] - p[idx[pos]])) >= 1.5 \
                    and (pos == 0 or a == L - 1) and sharp and dev(idx[g0:g1 + 1]) <= tol:
                gaps.append((pos, a))
            pos = b
        merged = sorted(merged + gaps)
        pos = 0
        for a, b in merged:
            if a > pos:
                pieces.append(("C", idx[pos], idx[a], None))
            pieces.append(("L", idx[a], idx[b], line(idx[a:b + 1])))
            pos = b
        if pos < L - 1:
            pieces.append(("C", idx[pos], idx[L - 1], None))
    m = len(pieces)
    if m == 1 and pieces[0][0] == "C" and not cfg.get("extrema"):
        return fit_smooth(p, {**cfg, "split": 0})
    at_corner = set(cs) if sharp else set()
    verts = []
    for i in range(m):
        A, B = pieces[i - 1], pieces[i]
        q = p[B[1]] if B[1] in at_corner else ps[B[1]]
        if A[0] == "L" and B[0] == "L":
            # lines near parallel (a hairpin) cross somewhere meaningless
            x = intersect(*A[3], *B[3]) if abs(A[3][1][0] * B[3][1][1] - A[3][1][1] * B[3][1][0]) > 0.4 else None
            q = x if x is not None and np.hypot(*(x - q)) <= cfg["snap"] else q
        elif A[0] == "L" or B[0] == "L":
            c, d = (A if A[0] == "L" else B)[3]
            x = None
            if B[1] in at_corner:
                # the line crosses the curve's heading just past the source's rounding
                f = 1 if A[0] == "L" else -1
                h0, h1 = ps[(B[1] + f * k) % n], ps[(B[1] + 2 * f * k) % n]
                h = (h1 - h0) / (np.hypot(*(h1 - h0)) or 1)
                x = intersect(c, d, h0, h) if abs(d[0] * h[1] - d[1] * h[0]) > 0.4 else None
            q = x if x is not None and np.hypot(*(x - q)) <= cfg["snap"] / 2 else c + ((q - c) @ d) * d
        verts.append(q)
    unit = lambda v: v / (np.hypot(*v) or 1)

    def heading(i, f, span):
        """Tangent leaving corner sample i along direction f, a quadratic through the
        edge past the source's rounding taken back to the corner."""
        lo, hi = min(k, span // 4), min(3 * k, span // 2)
        if hi - lo < 3:
            return unit(ps[(i + f * max(hi, 1)) % n] - ps[i])
        t = np.arange(lo, hi + 1) * step
        q = ps[[(i + f * j) % n for j in range(lo, hi + 1)]]
        return unit(np.array([np.polyfit(t, q[:, 0], 2)[1], np.polyfit(t, q[:, 1], 2)[1]]))

    def fit_run(q, ta, tb):
        """Cubics through q leaving along ta and arriving along tb: one if one fits; with
        extrema, else two meeting where the edge heads halfway between, as a designer
        splits a shoulder; else Schneider's splits wherever the error peaks."""
        tol = cfg["curve_tol"]
        one = fit_cubic(q, ta, -tb, np.inf)
        if fits([("C", one[0])], q, tol):
            return one
        half = ta + tb
        if cfg.get("extrema") and np.hypot(*half) > 0.3 and len(q) > 4 * k:
            g = np.gradient(q, axis=0)
            g /= np.maximum(np.hypot(*g.T), 1e-9)[:, None]
            j = k + int(np.argmax(g[k:len(q) - k] @ unit(half)))
            two = fit_cubic(q[:j + 1], ta, -unit(half), np.inf) + fit_cubic(q[j:], unit(half), -tb, np.inf)
            if fits([("C", c) for c in two], q, tol):
                return two
        return fit_cubic(q, ta, -tb, tol, max_depth=cfg.get("split_depth", 6))

    segs = []
    for i, (kind, s, e, _) in enumerate(pieces):
        v0, v1 = verts[i], verts[(i + 1) % m]
        idx = [(s + t) % n for t in range(((e - s) % n or n) + 1)]
        if kind == "L" or len(idx) < 4:
            segs.append(("L", v0, v1))
            continue
        pts = ps[idx].copy()
        pts[0], pts[-1] = v0, v1
        A, B = pieces[i - 1], pieces[(i + 1) % m]
        mm = min(k, len(pts) // 3)
        if A[0] == "L" and s not in at_corner:
            t0 = A[3][1] * np.sign(A[3][1] @ (v0 - verts[i - 1]) or 1)
        else:
            # at a corner, the edge's heading just past the source's rounding
            t0 = heading(s, 1, len(pts)) if s in at_corner else unit(ps[(s + mm) % n] - ps[(s - mm) % n])
        if B[0] == "L" and e not in at_corner:
            t1 = -B[3][1] * np.sign(B[3][1] @ (verts[(i + 2) % m] - v1) or 1)
        else:
            t1 = heading(e, -1, len(pts)) if e in at_corner else unit(ps[(e - mm) % n] - ps[(e + mm) % n])
        # the source's rounding of a corner is left out of the fit, so the curve meets it sharp
        c0 = k if s in at_corner and len(pts) > 4 * k else 0
        c1 = k if e in at_corner and len(pts) > 4 * k else 0
        keep = [0] + list(range(1 + c0, len(pts) - 1 - c1)) + [len(pts) - 1]
        pts, idx = pts[keep], [idx[q] for q in keep]
        if cfg.get("extrema") and not sharp and m == 1:
            t0, t1 = np.array([np.sign(t0[0]) or 1.0, 0]), np.array([np.sign(t1[0]) or -1.0, 0])
        cuts = [(0, t0)]
        if cfg.get("extrema"):
            # nodes where the curve runs level or plumb, as a type designer sets them
            d = ps[[(j + mm) % n for j in idx]] - ps[[(j - mm) % n for j in idx]]
            for ax in (0, 1):
                sg = np.sign(d[:, ax])
                for j in range(1, len(sg)):
                    sg[j] = sg[j] or sg[j - 1]
                flips = list(np.where(sg[1:] * sg[:-1] < 0)[0] + 1)
                groups = []
                for f in flips:
                    if groups and f - groups[-1][-1] < 2 * k:
                        groups[-1].append(f)
                    else:
                        groups.append([f])
                for g in groups:
                    j = int(round(np.mean(g)))
                    edge = max(k, min(3 * k, len(pts) // 4))
                    v = pts[:, ax] * sg[g[0] - 1]  # rises to an extremum, then falls
                    rise = min(v[j] - v[max(j - 6 * k, 0)], v[j] - v[min(j + 6 * k, len(pts) - 1)])
                    if sg[g[0] - 1] != sg[g[-1]] and edge <= j <= len(pts) - 1 - edge and rise >= 0.25:
                        tv = np.zeros(2)
                        tv[1 - ax] = np.sign(pts[min(j + mm, len(pts) - 1), 1 - ax] - pts[max(j - mm, 0), 1 - ax]) or 1
                        cuts.append((j, tv))
            cuts = [cuts[0]] + [c for c in sorted(cuts[1:], key=lambda c: c[0])]
            cuts = [c for q, c in enumerate(cuts) if q == 0 or c[0] - cuts[q - 1][0] >= 4]
        cuts.append((len(pts) - 1, -t1))
        for (a, ta), (b, tb) in zip(cuts, cuts[1:]):
            run = fit_run(pts[a:b + 1], ta, tb)
            if len(run) > 1 and min(np.hypot(*(r[3] - r[0])) for r in run) < cfg.get("min_seg", 0):
                # a sliver of a cubic is node clutter: one cubic if it stays near the edge
                one = fit_cubic(pts[a:b + 1], ta, -tb, np.inf)
                if fits([("C", one[0])], pts[a:b + 1], 1.5 * cfg["curve_tol"]):
                    run = one
            segs += [("C", ctrl) for ctrl in run]
    return segs


def dot_centers(a, r):
    """Dot-matrix dot centers, each row and column snapped to its mean."""
    s = filters.gaussian(a, sigma=r / 2, preserve_range=True)
    pts = []
    for y, x in feature.peak_local_max(np.pad(s, 1), min_distance=2, threshold_abs=0.4):
        par = lambda m, c, p: 0.5 * (m - p) / (m - 2 * c + p) if m - 2 * c + p < 0 else 0
        q = np.pad(s, 1)
        pts.append((x - 1 + par(q[y, x - 1], q[y, x], q[y, x + 1]), y - 1 + par(q[y - 1, x], q[y, x], q[y + 1, x])))
    pts = np.array(pts)
    for ax in (0, 1):
        order = np.argsort(pts[:, ax])
        groups = np.split(order, np.where(np.diff(pts[order, ax]) > r / 2)[0] + 1)
        for g in groups:
            pts[g, ax] = pts[g, ax].mean()
    return pts


def dot_lattice(a, r, pitch):
    """Dot centers on the glyph's printer grid: dot_centers' peaks, which merged
    neighbours pull inward, only pick the grid nodes; the grid's offset is the one
    whose discs best overlap the source's half-coverage area."""
    u = 4
    pitch = np.asarray(pitch, float)
    cs = dot_centers(a, r)
    src = transform.rescale(a, u, order=3, mode="constant", anti_aliasing=False) > 0.5
    yy, xx = (np.mgrid[0:src.shape[0], 0:src.shape[1]] + 0.5) / u - 0.5
    # circular mean of each axis' phase on the grid
    o = np.array([np.angle(np.exp(2j * np.pi * cs[:, ax] / pitch[ax]).mean()) / (2 * np.pi) * pitch[ax] for ax in (0, 1)])
    nodes = np.unique(np.round((cs - o) / pitch), axis=0) * pitch

    def iou(off):
        m = np.zeros(src.shape, bool)
        for x, y in nodes + off:
            m |= (xx - x) ** 2 + (yy - y) ** 2 <= r * r
        return (m & src).sum() / (m | src).sum()

    for span, step in ((0.75, 0.15), (0.15, 0.03)):
        grid = np.arange(-span, span + 1e-9, step)
        o = max((o + (dx, dy) for dx in grid for dy in grid), key=iou)
    return nodes + o


def dot_union(cs, r):
    """Outline of the union of equal circles, as loops of cubic arcs."""
    arcs = []
    for i, c in enumerate(cs):
        cover = []
        for j, o in enumerate(cs):
            d = np.hypot(*(o - c))
            if j != i and d < 2 * r:
                phi, al = np.arctan2(*(o - c)[::-1]), np.arccos(d / (2 * r))
                cover.append(((phi - al) % (2 * np.pi), al * 2))
        if not cover:
            arcs.append((c, 0.0, 2 * np.pi))
            continue
        # free arcs run from the end of one covered span to the start of the next
        ends = sorted(cover)
        merged = []
        for s, w in ends:
            if merged and s <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], s + w)
            else:
                merged.append([s, s + w])
        if len(merged) > 1 and merged[-1][1] >= merged[0][0] + 2 * np.pi:
            merged[0][0] = merged[-1][0] - 2 * np.pi
            merged[0][1] = max(merged[0][1], merged[-1][1] - 2 * np.pi)
            merged.pop()
        for k, (s, e) in enumerate(merged):
            nxt = merged[(k + 1) % len(merged)][0] + (2 * np.pi if k + 1 == len(merged) else 0)
            if nxt - e > 1e-6:
                arcs.append((c, e, nxt))
    at = lambda c, t: c + r * np.array([np.cos(t), np.sin(t)])
    loops, used = [], [False] * len(arcs)
    for k in range(len(arcs)):
        if used[k]:
            continue
        chain, cur = [], k
        while not used[cur]:
            used[cur] = True
            chain.append(arcs[cur])
            end = at(arcs[cur][0], arcs[cur][2])
            cur = min(range(len(arcs)), key=lambda m: np.hypot(*(at(arcs[m][0], arcs[m][1]) - end)))
        segs = []
        for c, s, e in chain:
            n = int(np.ceil((e - s) / (np.pi / 2) - 1e-9))
            for m in range(n):
                t0, t1 = s + (e - s) * m / n, s + (e - s) * (m + 1) / n
                k4 = 4 / 3 * np.tan((t1 - t0) / 4) * r
                p0, p3 = at(c, t0), at(c, t1)
                segs.append(("C", np.array([p0, p0 + k4 * np.array([-np.sin(t0), np.cos(t0)]),
                                            p3 - k4 * np.array([-np.sin(t1), np.cos(t1)]), p3])))
        loops.append(segs)
    return loops


def douglas_peucker(q, tol):
    """Indices an open polyline keeps when simplified to within tol."""
    keep, stack = [0, len(q) - 1], [(0, len(q) - 1)]
    while stack:
        a, b = stack.pop()
        if b - a < 2:
            continue
        d, rel = q[b] - q[a], q[a + 1:b] - q[a]
        span = np.hypot(*d)
        dist = np.abs(rel[:, 0] * d[1] - rel[:, 1] * d[0]) / span if span > 1e-9 else np.hypot(*rel.T)
        i = int(dist.argmax())
        if dist[i] > tol:
            keep.append(a + 1 + i)
            stack += [(a, a + 1 + i), (a + 1 + i, b)]
    return sorted(keep)


def fit_poly(pts, cfg):
    """A closed contour as a polygon, for cut-paper fonts: simplified to within "poly"
    px, each side refitted as a least-squares line away from its ends, sides extended
    to meet at corners (within snap), and sides shorter than poly_min px that only cut
    across a corner the source rounds dropped so their neighbours meet."""
    q = pts[:-1] if np.allclose(pts[0], pts[-1]) else pts
    n = len(q)
    q = np.roll(q, -int(np.hypot(*(q - q.mean(0)).T).argmax()), 0)  # start on a corner
    idx = douglas_peucker(np.vstack([q, q[:1]]), cfg["poly"])[:-1]
    if len(idx) < 3:
        return []
    trim = cfg.get("poly_trim", 0.5)
    lines = []
    for j, a in enumerate(idx):
        b = idx[(j + 1) % len(idx)] + (n if j == len(idx) - 1 else 0)
        side = q[np.arange(a, b + 1) % n]
        arc = np.r_[0, np.cumsum(np.hypot(*np.diff(side, axis=0).T))]
        inner = side[(arc >= trim) & (arc <= arc[-1] - trim)]
        if len(inner) >= 3:
            lines.append(line_fit(inner)[:2])
        else:
            d = side[-1] - side[0]
            lines.append(((side[0] + side[-1]) / 2, d / (np.hypot(*d) or 1)))
    anchors = [q[i] for i in idx]

    def meet():
        out = []
        for j, v in enumerate(anchors):
            x = intersect(*lines[j - 1], *lines[j])
            out.append(x if x is not None and np.hypot(*(x - v)) <= cfg["snap"] else v)
        return out

    verts = meet()
    while len(lines) > 3 and cfg.get("poly_min"):
        m = len(lines)
        ln = [np.hypot(*(verts[(j + 1) % m] - verts[j])) for j in range(m)]
        drop = None
        for j in sorted(range(m), key=lambda j: ln[j]):
            if ln[j] >= cfg["poly_min"]:
                break
            x = intersect(*lines[j - 1], *lines[(j + 1) % m])
            if x is not None and np.hypot(*(x - (verts[j] + verts[(j + 1) % m]) / 2)) <= cfg["snap"]:
                drop = j
                break
        if drop is None:
            break
        anchors[(drop + 1) % m] = (verts[drop] + verts[(drop + 1) % m]) / 2
        del lines[drop], anchors[drop]
        verts = meet()
    return [("L", verts[j], verts[(j + 1) % len(verts)]) for j in range(len(verts))]


def fits(segs, pts, tol):
    """Whether a fitted loop and its contour stay within tol pixels of each other both ways."""
    flat = flatten(segs)
    if len(flat) < 3 or not np.isfinite(flat).all():
        return False
    contour = [("L", pts[i], pts[i + 1]) for i in range(len(pts) - 1)]
    return edge_distance(flat, [(None, contour)]).max() <= tol and edge_distance(pts, [(None, segs)]).max() <= tol


def tone(a, cfg):
    """Coverage over the mean coverage of the ink around it when "local" (a gaussian
    sigma in px) is set, so a faint dry-marker stroke is cut at the same fraction
    of its own body as a solid one."""
    r = cfg.get("local")
    if not r:
        return a
    ink = (a > 0.2).astype(float)
    m = ndimage.gaussian_filter(a * ink, r) / np.maximum(ndimage.gaussian_filter(ink, r), 1e-3)
    return a / np.maximum(m, cfg.get("local_floor", 0.3))


def trace(a, cfg):
    pad = 4
    s = np.pad(tone(a, cfg), pad)
    if cfg["blur"] > 0:
        s = filters.gaussian(s, sigma=cfg["blur"], preserve_range=True)
    u = cfg.get("upsample", 1)
    if u > 1:
        # a bicubic edge instead of marching squares' straight hops between pixels
        s = transform.rescale(s, u, order=3, mode="constant", anti_aliasing=False)
    loops = []
    for c in measure.find_contours(s, cfg.get("level", 0.5)):
        pts = (c[:, ::-1] + 0.5) / u - 0.5 - pad  # (x, y), y down, master pixels
        area = 0.5 * (pts[:-1, 0] * pts[1:, 1] - pts[1:, 0] * pts[:-1, 1]).sum()
        if abs(area) < cfg["min_area"]:
            continue
        sf = cfg.get("small_fit")
        # small_fit: {size, overrides} for outer loops (not counters) no bigger than size px, like accents
        # hole_fit: the same for counters, like a small font's slit counters
        hf = cfg.get("hole_fit")
        span = (pts.max(0) - pts.min(0)).max()
        fc = ({**cfg, **sf} if sf and area > 0 and span <= sf["size"] else
              {**cfg, **hf} if hf and area < 0 and span <= hf["size"] else cfg)
        loop = resample(pts[:-1], fc["step"])
        if fc.get("poly"):
            segs = fit_poly(pts, fc)
        elif fc.get("smooth"):
            segs = fit_smooth(loop, fc)
            if fc.get("max_dev") is not None and not fits(segs, pts, fc["max_dev"]):
                # (a split fit that strays retries as plain smooth, all curves)
                segs = fit_smooth(loop, {**fc, "curve_tol": fc["curve_tol"] / 2, "smooth": fc["smooth"] / 2, "split": 0})
        else:
            segs = fit_contour(loop, fc)
        if fc.get("max_dev") is not None and not fits(segs, pts, fc["max_dev"]):
            # a fit that strays (tiny specks, runaway cubics) falls back to a polygon
            poly = measure.approximate_polygon(pts, fc["line_tol"])[:-1]
            segs = [("L", poly[i], poly[(i + 1) % len(poly)]) for i in range(len(poly))]
        if len(segs) < 3 and all(s[0] == "L" for s in segs):
            continue  # a speck fitted down to a point or a line has no area
        loops.append((pts, segs))
    if cfg.get("dots") and loops:
        # dot-matrix: every dot a true circle of radius "dots", overlaps unioned
        edge = np.concatenate([p for p, _ in loops])
        r = cfg["dots"]
        segs = dot_union(dot_lattice(a, r, cfg["dot_pitch"]) if cfg.get("dot_pitch") else dot_centers(a, r), r)
        return [(edge if i == 0 else np.empty((0, 2)), s) for i, s in enumerate(segs)]
    if cfg.get("dot_rect"):
        loops = rect_dots(loops, cfg["dot_rect"])
    return loops


def rect_dots(loops, most):
    """Each loop of at most `most` square pixels that sits inside no other (a
    dot, a tick, a comma's head) as an upright rectangle of its area, centroid
    and spread each way: a small font's dots then all come out alike, not each
    a blob of its own anti-aliasing."""
    polys = [flatten(s, 8) for _, s in loops]
    out = []
    for i, (pts, segs) in enumerate(loops):
        p = polys[i]
        x, y = p[:, 0], p[:, 1]
        x1, y1 = np.roll(x, -1), np.roll(y, -1)
        cross = x * y1 - x1 * y
        area = cross.sum() / 2
        nested = any(inside(p[0], polys[j]) for j in range(len(loops)) if j != i)
        if abs(area) > most or abs(area) < 1e-6 or nested:
            out.append((pts, segs))
            continue
        cx = ((x + x1) * cross).sum() / (6 * area)
        cy = ((y + y1) * cross).sum() / (6 * area)
        var_x = max(((x * x + x * x1 + x1 * x1) * cross).sum() / (12 * area) - cx * cx, 1e-6)
        var_y = max(((y * y + y * y1 + y1 * y1) * cross).sum() / (12 * area) - cy * cy, 1e-6)
        w, h = np.sqrt(12 * var_x), np.sqrt(12 * var_y)
        k = np.sqrt(abs(area) / (w * h))
        w, h = w * k / 2, h * k / 2
        c = [np.array(v) for v in ((cx - w, cy - h), (cx + w, cy - h), (cx + w, cy + h), (cx - w, cy + h))]
        out.append((pts, [("L", c[j], c[(j + 1) % 4]) for j in range(4)]))
    return out


def flatten(segs, per=24):
    out = []
    for s in segs:
        if s[0] == "L":
            out.append(s[1])
        else:
            out += list(bezier(s[1], np.linspace(0, 1, per, endpoint=False)))
    return np.array(out)


def raster(loops, shape):
    """Coverage of a trace in a cell, even-odd, SS-times supersampled."""
    acc = np.zeros((shape[0] * SS, shape[1] * SS), bool)
    for _, segs in loops:
        im = Image.new("1", (shape[1] * SS, shape[0] * SS), 0)
        ImageDraw.Draw(im).polygon([tuple(q) for q in flatten(segs) * SS + SS / 2 - 0.5], fill=1)
        acc ^= np.asarray(im)
    return acc.reshape(shape[0], SS, shape[1], SS).mean((1, 3))


def edge_distance(pts, loops):
    """Each point's distance to the nearest edge of the trace."""
    a = np.concatenate([flatten(s, 48) for _, s in loops])
    b = np.concatenate([np.roll(flatten(s, 48), -1, 0) for _, s in loops])
    ab = b - a
    out = np.empty(len(pts))
    for i in range(0, len(pts), 512):
        p = pts[i:i + 512]
        t = np.clip(((p[:, None] - a[None]) * ab[None]).sum(-1) / np.maximum((ab * ab).sum(-1), 1e-12)[None], 0, 1)
        q = a[None] + t[..., None] * ab[None]
        out[i:i + 512] = np.sqrt(((p[:, None] - q) ** 2).sum(-1)).min(1)
    return out


def true_edge(a, level=0.5):
    """The source's own half-coverage edge, unblurred, as (points, loop) pairs.
    Scoring against this, not the blurred edge a trace follows, shows what blur
    and simplification lose."""
    out = []
    u = 4
    up = transform.rescale(np.pad(a, 2), u, order=3, mode="constant", anti_aliasing=False)
    for c in measure.find_contours(up, level):
        pts = (c[:, ::-1] + 0.5) / u - 0.5 - 2
        area = 0.5 * (pts[:-1, 0] * pts[1:, 1] - pts[1:, 0] * pts[:-1, 1]).sum()
        if abs(area) >= 1.0:
            out.append((pts, [("L", pts[i], pts[i + 1]) for i in range(len(pts) - 1)]))
    return out


def score(a, loops, level=0.5):
    r = raster(loops, a.shape)
    src, got = a > level, r > 0.5
    truth = true_edge(a, level)
    edge = np.concatenate([p for p, _ in truth]) if truth else np.zeros((0, 2))
    d = edge_distance(edge, loops) if len(edge) else np.zeros(1)
    fit = np.concatenate([flatten(s, 12) for _, s in loops])
    stray = edge_distance(fit, truth) if truth else np.zeros(1)
    return r, {"iou": float((src & got).sum() / max(1, (src | got).sum())),
               "mean_err": float(np.abs(a - r)[(a > 0.02) | (r > 0.02)].mean()),
               "edge_mean_px": float(d.mean()), "edge_p95_px": float(np.percentile(d, 95)),
               "edge_max_px": float(d.max()), "stray_max_px": float(stray.max()),
               "segments": int(sum(len(s) for _, s in loops)),
               "curves": int(sum(k[0] == "C" for _, s in loops for k in s))}


# ---- font ------------------------------------------------------------------

def inside(pt, poly):
    x, y = pt
    xs, ys = poly[:, 0], poly[:, 1]
    xs2, ys2 = np.roll(xs, -1), np.roll(ys, -1)
    hit = ((ys > y) != (ys2 > y)) & (x < (xs2 - xs) * (y - ys) / np.where(ys2 == ys, 1e-12, ys2 - ys) + xs)
    return hit.sum() % 2 == 1


def reverse(segs):
    return [("L", s[2], s[1]) if s[0] == "L" else ("C", s[1][::-1]) for s in reversed(segs)]


def oriented(loops):
    """Outer loops counter-clockwise in y-up, holes clockwise, by nesting depth."""
    polys = [flatten(s, 8) for _, s in loops]
    out = []
    for i, (pts, segs) in enumerate(loops):
        depth = sum(inside(polys[i][0], polys[j]) for j in range(len(loops)) if j != i)
        p = polys[i]
        area = 0.5 * (p[:, 0] * np.roll(p[:, 1], -1) - np.roll(p[:, 0], -1) * p[:, 1]).sum()
        # y down: area > 0 is clockwise on screen, counter-clockwise in y-up
        out.append((pts, segs if (area > 0) == (depth % 2 == 0) else reverse(segs)))
    return out


def charstring(loops, base, shift, width):
    pen = T2CharStringPen(width, None)
    to = lambda q: (round((q[0] - shift) * UNITS, 1), round((base - q[1]) * UNITS, 1))
    for _, segs in loops:
        pen.moveTo(to(segs[0][1] if segs[0][0] == "L" else segs[0][1][0]))
        for s in segs:
            if s[0] == "L":
                pen.lineTo(to(s[2]))
            else:
                pen.curveTo(to(s[1][1]), to(s[1][2]), to(s[1][3]))
        pen.closePath()
    return pen.getCharString()


def build_font(cfg, cell, glyphs, out, bounds={}):
    cell_w, cell_h = cell
    base = cfg["baseline"]
    side = cfg["sidebearing"]
    names = {c: f"uni{ord(c):04X}" for c in glyphs}
    fb = FontBuilder(1000, isTTF=False)
    fb.setupGlyphOrder([".notdef", "space"] + list(names.values()))
    fb.setupCharacterMap({32: "space", **{ord(c): n for c, n in names.items()}})
    nd_w = round(cell_w * UNITS / 2)
    nd = T2CharStringPen(nd_w, None)
    for x, y in ((50, 0), (50, 700), (nd_w - 50, 700), (nd_w - 50, 0)):
        (nd.moveTo if x == 50 and y == 0 else nd.lineTo)((x, y))
    nd.closePath()
    strings = {".notdef": nd.getCharString(), "space": T2CharStringPen(round(cfg["space"] * UNITS), None).getCharString()}
    metrics = {".notdef": (nd_w, 50), "space": (round(cfg["space"] * UNITS), 0)}
    for c, loops in glyphs.items():
        pts = np.concatenate([flatten(s, 8) for _, s in loops])
        x0, x1 = bounds.get(c, (pts[:, 0].min(), pts[:, 0].max()))
        adv = round((x1 - x0 + 2 * side) * UNITS)
        strings[names[c]] = charstring(oriented(loops), base, x0 - side, adv)
        lsb = round((pts[:, 0].min() - x0 + side) * UNITS) if c in bounds else round(side * UNITS)
        metrics[names[c]] = (adv, lsb)
    name = cfg["name"]
    ps = name.replace(" ", "")
    fb.setupCFF(f"{ps}-Regular", {"FullName": name}, strings, {})
    fb.setupHorizontalMetrics(metrics)
    asc, desc = round(base * UNITS), round((cell_h - base) * UNITS)
    fb.setupHorizontalHeader(ascent=asc, descent=-desc)
    fb.setupNameTable({"familyName": name, "styleName": "Regular", "fullName": name, "psName": f"{ps}-Regular",
                       "version": "Version 1.000", "copyright": cfg.get("copyright", ""),
                       "licenseDescription": "This Font Software is licensed under the SIL Open Font License, Version 1.1.",
                       "licenseInfoURL": "https://openfontlicense.org"})
    fb.setupOS2(sTypoAscender=asc, sTypoDescender=-desc, sTypoLineGap=0, usWinAscent=asc, usWinDescent=desc)
    fb.setupPost()
    fb.save(out)


# ---- compare sheet ---------------------------------------------------------

def sheet(cfg, cell, items, otf, out):
    cw, ch = cell
    z = max(2, round(400 / ch))
    lab = ImageFont.truetype(LABEL, 22)
    small = ImageFont.truetype(LABEL, 18)
    tile_w, tile_h = cw * z * 3 + 40, ch * z + 70
    cols = 3 if cw * z < 400 else 2
    rows = -(-len(items) // cols)
    f = ImageFont.truetype(str(otf), 120)
    tmp = ImageDraw.Draw(Image.new("RGB", (1, 1)))
    box = tmp.multiline_textbbox((0, 0), cfg["sample"], font=f, spacing=20)
    W = max(cols * tile_w + 40, box[2] + 60)
    head = 80 + box[3]
    img = Image.new("RGB", (W, head + rows * tile_h + 40), (14, 14, 18))
    d = ImageDraw.Draw(img)
    d.text((20, 16), f"{cfg['name']} trace: source | outline on source (cyan line nodes, yellow curve nodes) | "
                     f"trace rendered (red lost, blue added)", font=lab, fill=(255, 255, 255))
    d.multiline_text((20, 60), cfg["sample"], font=f, fill=(255, 255, 255), spacing=20)
    for n, (c, a, r, loops, s, src) in enumerate(items):
        x0 = 20 + (n % cols) * tile_w
        y0 = head + (n // cols) * tile_h
        bad = s["iou"] < cfg.get("iou_min", 0.95) or s["edge_max_px"] > cfg.get("edge_max", 1.5)
        d.text((x0, y0), f"{c}  [{src}]  IoU {s['iou']:.3f}  edge {s['edge_mean_px']:.2f}/{s['edge_max_px']:.2f}px  "
                         f"{s['segments']} seg", font=small, fill=(255, 90, 90) if bad else (210, 210, 210))
        g = Image.fromarray((a * 255).astype(np.uint8)).resize((cw * z, ch * z), Image.NEAREST).convert("RGB")
        img.paste(g, (x0, y0 + 28))
        o = g.copy()
        od = ImageDraw.Draw(o)
        for _, segs in loops:
            poly = [tuple(q) for q in (flatten(segs) + 0.5) * z]
            od.line(poly + [poly[0]], fill=(255, 60, 60), width=2)
            for sg in segs:
                q = (sg[1] if sg[0] == "L" else sg[1][0]) + 0.5
                od.rectangle([q[0] * z - 3, q[1] * z - 3, q[0] * z + 3, q[1] * z + 3],
                             outline=(60, 220, 255) if sg[0] == "L" else (255, 220, 60))
        img.paste(o, (x0 + cw * z + 10, y0 + 28))
        diff = np.repeat((np.minimum(a, r) * 255)[..., None], 3, -1)
        diff[..., 0] += np.clip(a - r, 0, 1) * 255
        diff[..., 2] += np.clip(r - a, 0, 1) * 255
        img.paste(Image.fromarray(diff.clip(0, 255).astype(np.uint8)).resize((cw * z, ch * z), Image.NEAREST),
                  (x0 + 2 * (cw * z + 10), y0 + 28))
    img.save(out)


def pick(cfg, tex):
    """Each character's cell in master pixels, with the tag of the source it came from."""
    master = load(cfg["sources"][0], tex)
    shape = next(iter(master.values())).shape
    tag0 = cfg["sources"][0].get("tag", "0")
    picked = {c: (a, tag0) for c, a in master.items()}
    for i, src in enumerate(cfg["sources"][1:], 1):
        other = load(src, tex)
        shared = [c for c in other if c in master and c.isalnum()]
        s, p = (None, tuple(src["map"])) if "map" in src else fit_mapping(master, other, shared)
        tag = src.get("tag", str(i))
        print(f"{tag} -> master: scale {p[0]:.4f} x {p[1]:.4f}, offset {p[2]:.2f}, {p[3]:.2f}, "
              + ("pinned by map" if "map" in src else
                 f"IoU {s:.3f} over {len(shared)}" if s is not None else "cell ratio, too few shared letters"))
        for c, a in other.items():
            if c not in picked or c in src.get("take", ""):
                picked[c] = (warp(a, shape, *p), tag)
    return picked, shape


def composed(own, base, tol=(0.3, 0.03)):
    """An accented glyph's loops with each one its base letter also has swapped for the
    base's trace, moved onto it, so the two match. A loop matches when its box size is
    within tol[0] px and its area within tol[1] of a base loop's, and it sits within 2 px."""
    def key(loop):
        q = flatten(loop[1], 16)
        x, y = q[:, 0], q[:, 1]
        cr = x * np.roll(y, -1) - np.roll(x, -1) * y
        area = 0.5 * cr.sum()
        cen = np.array([((x + np.roll(x, -1)) * cr).sum(), ((y + np.roll(y, -1)) * cr).sum()]) / (6 * area)
        return q.max(0) - q.min(0), area, cen
    keys = [key(b) for b in base]
    out, swapped, kept = [], [], []
    for loop in own:
        size, ar, cen = key(loop)
        hit = [(b, cen - bc) for b, (bs, ba, bc) in zip(base, keys)
               if np.abs(bs - size).max() <= tol[0] and abs(ba - ar) <= tol[1] * abs(ar)
               and np.hypot(*(cen - bc)) <= 2]
        if not hit:
            out.append(loop)
            kept.append(flatten(loop[1], 8))
            continue
        b, v = hit[0]
        out.append((b[0] + v, [("L", s[1] + v, s[2] + v) if s[0] == "L" else ("C", s[1] + v) for s in b[1]]))
        swapped.append(flatten(out[-1][1], 8))
    # all or nothing within a letter: a counter swapped inside an outline kept, or kept
    # inside one swapped, would no longer line up with it
    inside = lambda q, r: (q.min(0) < r.mean(0)).all() and (r.mean(0) < q.max(0)).all()
    if any(inside(q, r) or inside(r, q) for q in swapped for r in kept):
        return own
    return out


def svg_loops(path, shape):
    """A drawn glyph's outline from an SVG of straight paths (absolute M, L, Z),
    its viewBox the cell in pixels, as trace's loops: the master's pixels put a
    pixel's center on its index, so half a pixel less."""
    text = Path(path).read_text()
    view = [float(v) for v in re.search(r'viewBox="([^"]+)"', text).group(1).split()]
    k = (shape[1] / view[2], shape[0] / view[3])
    loops = []
    for d in re.findall(r'\sd="([^"]+)"', text):
        for part in re.findall(r"M[^M]*", d):
            nums = [float(v) for v in re.findall(r"-?\d+(?:\.\d+)?", part)]
            pts = np.array(nums).reshape(-1, 2) * k - 0.5
            segs = [("L", pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts))]
            loops.append((np.vstack([pts, pts[:1]]), segs))
    return loops


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("config", type=Path)
    ap.add_argument("--src", type=Path, default=ROOT / ".planning/fonts/src")
    ap.add_argument("--no-sheet", action="store_true", help="skip the compare sheet")
    args = ap.parse_args()
    args.config = args.config.resolve()
    cfg = json.loads(args.config.read_text())
    fit = fit_of(cfg)
    tex = args.src / "tex"
    picked, shape = pick(cfg, tex)
    bounds = {}
    if "metrics_from" in cfg:
        partner = json.loads((args.config.parent / cfg["metrics_from"]).read_text())
        for c, (a, _) in pick(partner, tex)[0].items():
            loops = trace(a, fit_of(partner, c))
            if loops:
                pts = np.concatenate([flatten(s, 8) for _, s in loops])
                bounds[c] = (pts[:, 0].min(), pts[:, 0].max())
    order = cfg.get("order") or "".join(picked)
    missing = [c for c in order if c not in picked]
    if missing:
        print("no source has:", "".join(missing))
    glyphs, items, scores = {}, [], {}
    base_of = {c: b for chars, b in cfg.get("compose", {}).items() for c in chars}
    for c in order:
        if c not in picked:
            continue
        a, tag = picked[c]
        gfit = fit_of(cfg, c)
        if c in cfg.get("svg", {}):
            loops = svg_loops(args.config.parent / cfg["svg"][c], a.shape)
        else:
            loops = trace(a, gfit)
        if base_of.get(c) in picked and loops:
            comp = composed(loops, trace(picked[base_of[c]][0], fit_of(cfg, base_of[c])),
                            cfg.get("compose_tol", (0.3, 0.03)))
            lv = gfit.get("level", 0.5)
            # the base's letter only where it still sits on this cell's own edge
            if score(tone(a, gfit), comp, lv)[1]["edge_max_px"] <= score(tone(a, gfit), loops, lv)[1]["edge_max_px"] + 0.25:
                loops = comp
        if not loops:
            print("nothing traced for", repr(c))
            continue
        r, s = score(tone(a, gfit), loops, gfit.get("level", 0.5))
        glyphs[c], scores[c] = loops, {**s, "source": tag}
        items.append((c, a, r, loops, s, tag))
    cell = (shape[1], shape[0])
    otf = args.config.parent / f"{cfg['name'].replace(' ', '')}.otf"
    build_font(cfg, cell, glyphs, otf, bounds)
    slug = args.config.parent.name
    if args.config.stem != "trace":
        slug += "_" + args.config.stem.removeprefix("trace_")
    work = ROOT / ".planning/fonts/out" / slug
    work.mkdir(parents=True, exist_ok=True)
    (work / "scores.json").write_text(json.dumps(scores, indent=1, ensure_ascii=False))
    if "sample" not in cfg:
        chars = "".join(glyphs)
        cfg["sample"] = "\n".join(chars[i:i + 26] for i in range(0, len(chars), 26))
    if not args.no_sheet:
        import fontstrip
        sheet(cfg, cell, items, otf, work / "glyphs.png")
        name, blocks = fontstrip.strip(args.config, tex)
        fontstrip.compose([(name, blocks)], args.config.parent / f"{slug}_compare.png")
    worst = sorted(scores.items(), key=lambda kv: kv[1]["iou"])[:6]
    print(f"{otf.relative_to(ROOT)}: {len(glyphs)} glyphs, mean IoU {np.mean([s['iou'] for s in scores.values()]):.4f}, "
          f"mean edge {np.mean([s['edge_mean_px'] for s in scores.values()]):.3f}px, "
          f"worst edge {max(s['edge_max_px'] for s in scores.values()):.2f}px")
    print("lowest IoU:", ", ".join(f"{c} {s['iou']:.3f}/{s['edge_max_px']:.2f}px" for c, s in worst))


if __name__ == "__main__":
    main()
