"""Refine a glyph's fitted outline against the game's pixels, for tools/fonttrace.py.

The fit settles the outline's shape: which runs are lines, where the corners and nodes are.
This moves each node, and each curve handle's angle and length, so the outline rasterized
(boxed down from SS-times supersampling, as fontstrip draws it) covers the game's pixels as
closely as it can, while staying near the fit. A smooth node's two handles turn together
(a handle beside a line keeps to the line), so curves stay tangent where they were.

A fit's "refine": {} (or true) turns it on; its keys: pull, weight per px^2 a node moves
(default 0.05); turn, per rad^2 a handle turns (0.5); grow, per log-length^2 (0.2); reach,
px a node may move (0.6); iters, optimizer iterations (60); eps, the optimizer's step (0.06);
smooth_deg, joins this straight count as smooth (8).
"""
import numpy as np
from PIL import Image, ImageDraw
from scipy import optimize

SS = 8


def start(s):
    return np.asarray(s[1] if s[0] == "L" else s[1][0], float)


def unit(v):
    n = np.hypot(*v)
    return v / n if n > 1e-9 else np.array([1.0, 0.0])


def rotate(u, t):
    c, s = np.cos(t), np.sin(t)
    return np.array([c * u[0] - s * u[1], s * u[0] + c * u[1]])


class Loop:
    """One loop's segments as offsets from the fit: 2 per node, a log-length per curve
    handle, and an angle per handle, shared by a smooth node's two."""

    def __init__(self, segs, smooth_deg):
        self.kind = [s[0] for s in segs]
        n = self.n = len(segs)
        self.p0 = np.array([start(s) for s in segs])
        # handles: out of node i (seg i's ctrl1 - ctrl0), into node i+1 (seg i's ctrl2 - ctrl3)
        self.hout, self.hin = {}, {}
        for i, s in enumerate(segs):
            if s[0] == "C":
                c = np.asarray(s[1], float)
                self.hout[i] = c[1] - c[0]
                self.hin[i] = c[2] - c[3]

        def leaving(i):
            return self.hout[i] if self.kind[i] == "C" and np.hypot(*self.hout[i]) > 1e-6 else \
                self.p0[(i + 1) % n] - self.p0[i]

        def arriving(i):
            j = (i - 1) % n
            return -self.hin[j] if self.kind[j] == "C" and np.hypot(*self.hin[j]) > 1e-6 else \
                self.p0[i] - self.p0[j]

        self.smooth = [np.degrees(np.arccos(np.clip(unit(arriving(i)) @ unit(leaving(i)), -1, 1))) <= smooth_deg
                       for i in range(n)]
        self.idx = {}
        k = 2 * n
        for i in range(n):
            if self.kind[i] == "C":
                self.idx[("sout", i)] = k
                self.idx[("sin", i)] = k + 1
                k += 2
        for i in range(n):
            j = (i - 1) % n
            cin, cout = self.kind[j] == "C", self.kind[i] == "C"
            if self.smooth[i] and cin and cout:
                self.idx[("tnode", i)] = k
                k += 1
            elif self.smooth[i] and (cin or cout):
                pass  # the handle follows the line beside it
            else:
                if cout:
                    self.idx[("tout", i)] = k
                    k += 1
                if cin:
                    self.idx[("tin", j)] = k
                    k += 1
        self.size = k

    def build(self, x):
        n = self.n
        p = self.p0 + x[:2 * n].reshape(n, 2)
        segs = []
        for i in range(n):
            a, b = p[i], p[(i + 1) % n]
            if self.kind[i] == "L":
                segs.append(("L", a, b))
                continue
            j = (i + 1) % n
            ho, hi = self.hout[i], self.hin[i]
            lo = np.hypot(*ho) * np.exp(x[self.idx[("sout", i)]])
            if ("tnode", i) in self.idx:
                uo = rotate(unit(ho), x[self.idx[("tnode", i)]])
            elif ("tout", i) in self.idx:
                uo = rotate(unit(ho), x[self.idx[("tout", i)]])
            else:  # smooth beside an incoming line
                uo = unit(p[i] - p[(i - 1) % n])
            li = np.hypot(*hi) * np.exp(x[self.idx[("sin", i)]])
            if ("tnode", j) in self.idx:
                ui = rotate(unit(hi), x[self.idx[("tnode", j)]])
            elif ("tin", i) in self.idx:
                ui = rotate(unit(hi), x[self.idx[("tin", i)]])
            else:  # smooth beside an outgoing line at node j
                ui = -unit(p[(j + 1) % n] - p[j])
            segs.append(("C", np.array([a, a + uo * lo, b + ui * li, b])))
        return segs


def flatten(segs, per=16):
    out = []
    t = np.linspace(0, 1, per, endpoint=False)[:, None]
    for s in segs:
        if s[0] == "L":
            out.append(s[1][None])
        else:
            c = s[1]
            out.append((1 - t) ** 3 * c[0] + 3 * (1 - t) ** 2 * t * c[1] + 3 * (1 - t) * t ** 2 * c[2] + t ** 3 * c[3])
    return np.concatenate(out)


def raster(seg_loops, shape, SS=SS):
    acc = np.zeros((shape[0] * SS, shape[1] * SS), bool)
    for segs in seg_loops:
        im = Image.new("1", (shape[1] * SS, shape[0] * SS), 0)
        ImageDraw.Draw(im).polygon([tuple(q) for q in flatten(segs) * SS + SS / 2 - 0.5], fill=1)
        acc ^= np.asarray(im)
    return acc.reshape(shape[0], SS, shape[1], SS).mean((1, 3))


def to_native(o, shape, sx, sy, dx, dy):
    """Master coverage o area-averaged into a source's own cells (shape), fonttrace.warp's mapping
    undone: native pixel j spans master [sx * (j - 0.5) + dx, sx * (j + 0.5) + dx]."""
    def weights(n, m, s, d):
        lo = s * (np.arange(n) - 0.5) + d
        mi = np.arange(m) - 0.5
        return np.clip(np.minimum(lo[:, None] + s, mi[None] + 1) - np.maximum(lo[:, None], mi[None]), 0, None) / s
    return weights(shape[0], o.shape[0], sy, dy) @ o @ weights(shape[1], o.shape[1], sx, dx).T


def split(segs, longer):
    """Each curve longer than longer px halved at t=0.5, so the refine has a node mid-bowl."""
    out = []
    for s in segs:
        c = np.asarray(s[1], float) if s[0] == "C" else None
        if c is None or np.hypot(*np.diff(flatten([s], 8), axis=0).T).sum() <= longer:
            out.append(s)
            continue
        ab, bc, cd = (c[0] + c[1]) / 2, (c[1] + c[2]) / 2, (c[2] + c[3]) / 2
        abc, bcd = (ab + bc) / 2, (bc + cd) / 2
        m = (abc + bcd) / 2
        out += [("C", np.array([c[0], ab, abc, m])), ("C", np.array([m, bcd, cd, c[3]]))]
    return out


def refine(a, loops, cfg, native=None):
    """loops ((pts, segs) from fonttrace.trace) moved to cover a's coverage best; with native
    (a source's own cell, its mapping onto a), the coverage is matched there instead."""
    cfg = {} if cfg is True else cfg
    pull, turn, grow = cfg.get("pull", 0.05), cfg.get("turn", 0.5), cfg.get("grow", 0.2)
    reach = cfg.get("reach", 0.6)
    if cfg.get("split"):
        loops = [(p, split(s, cfg["split"])) for p, s in loops]
    ls = [Loop(s, cfg.get("smooth_deg", 8)) for _, s in loops]
    offs = np.cumsum([0] + [l.size for l in ls])
    kinds = []
    for l in ls:
        k = np.zeros(l.size, int)  # 0 node, 1 log-length, 2 angle
        for name, i in l.idx.items():
            k[i] = 1 if name[0] in ("sout", "sin") else 2
        kinds.append(k)
    kinds = np.concatenate(kinds)
    w = np.where(kinds == 0, pull, np.where(kinds == 1, grow, turn))
    bounds = [(-reach, reach) if k == 0 else (-0.7, 0.7) if k == 1 else (-0.4, 0.4) for k in kinds]

    def segs_of(x):
        return [l.build(x[offs[i]:offs[i + 1]]) for i, l in enumerate(ls)]

    def loss(x):
        r = raster(segs_of(x), a.shape, cfg.get("ss", SS))
        if native is None:
            return float(((r - a) ** 2).sum() + (w * x * x).sum())
        n, p = native  # a native pixel covers p[0] x p[1] master pixels
        return float(((to_native(r, n.shape, *p) - n) ** 2).sum() * p[0] * p[1] + (w * x * x).sum())

    x0 = np.zeros(offs[-1])
    res = optimize.minimize(loss, x0, method="L-BFGS-B", bounds=bounds,
                            options={"maxiter": cfg.get("iters", 60), "eps": cfg.get("eps", 0.06)})
    x = res.x if res.fun <= loss(x0) else x0
    return [(p, s) for (p, _), s in zip(loops, segs_of(x))]
