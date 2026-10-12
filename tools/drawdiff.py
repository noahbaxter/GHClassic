#!/usr/bin/env python3
"""Set our draws beside retail's, draw by draw.

    tools/drawdiff.py <retail.gs> <draws_name.json> [-v]

retail.gs is a GS dump of the retail game from PCSX2 (tools/pcsx2.py, the
scenario step (dump name)); draws_name.json is the same step in our build.
Both must be of the same frozen frame.

Retail sends a mesh as strips of up to 48 vertices and we send text a line a
draw, so on both sides consecutive draws under one state are taken as one
run. The draws into the picture are compared, then each pass of draws into
a rendered texture against the pass of ours in its place in the frame. A
retail run is paired with the run of ours, of the same texture size, that
most of its vertices land on. For each pair: the GS state our
material stands for against retail's registers, then every retail vertex that
a triangle uses against the nearest of ours in screen position and uv, for
how far apart they are in pixels, in colour (GS units, 0x80 is 1), in texels
and in Z (the Z buffer's units), and the texture's first level texel for
texel, read out of the dump's copy of the GS's memory against the pixels our
dump writes beside it (draws_name.rgba). Then what is left over: retail runs that
land on none of ours, and vertices of ours inside the picture with none of
retail's on them.

ok is a pair within tolerance, BAD one that is not, skn a BAD one with a
skinned mesh in ours: a character, whose pose is the same on both sides only
when ours draws retail's memory (the scenario step (transplant)).
"""
import argparse
import collections
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gsdump  # noqa: E402

# milo::mat::Blend to PRIM's ABE and ALPHA's a, b, c, d.
BLEND = {
    0: (1, "2201"),  # Dest: Cd
    1: (0, None),    # Src
    2: (1, "0221"),  # Add: Cs + Cd, through FIX 0x80
    3: (1, "0101"),  # SrcAlpha: (Cs - Cd) * As + Cd
    4: (1, "0201"),  # SrcAlphaAdd: Cs * As + Cd
}
# milo::mat::ZMode to TEST's ZTST and ZBUF's ZMSK.
ZMODE = {0: (1, 1), 1: (3, 0), 2: (2, 1), 3: (1, 0), 4: (2, 0)}
ZBITS = {0: 32, 1: 24, 2: 16, 10: 16}  # ZBUF's PSM
TOLERANCE = {"px": 0.13, "colour": 1, "texel": 0.6, "z": 1}
STATE = ("rect", "blend", "zMode", "alphaCut", "alphaWrite", "destAlphaTest", "texWrap", "highlight", "decal", "texture",
         "renderTarget")
SCREEN_COPY = 1  # render/frame.h, kScreenCopyTex: a draw's renderTarget when it samples a copy of the picture
POINTS = 400  # retail vertices of a run looked for on our triangles, each a pass over them all
SAME_Z = 16  # Z units within which two vertices at one pixel and texel are one place
NEAR = 0.6  # pixels, and texels, within which a vertex of one side is on one of the other's


def drawn(draw, width, height):
    """The vertices of a retail draw that some primitive it draws uses, each with whether any of those
    reaches into the buffer."""
    verts, kind = draw["verts"], draw["prim"]["type"]
    used = {}
    for i, vert in enumerate(verts):
        if vert.get("skip"):
            continue
        if kind == "tristrip" and i >= 2:
            corners = (i - 2, i - 1, i)
        elif kind == "trifan" and i >= 2:
            corners = (0, i - 1, i)
        elif kind == "tri" and i % 3 == 2:
            corners = (i - 2, i - 1, i)
        elif kind == "sprite" and i % 2 == 1:
            corners = (i - 1, i)
        elif kind in ("point", "line", "linestrip"):
            corners = (i,)
        else:
            continue
        xs, ys = [verts[c]["x"] for c in corners], [verts[c]["y"] for c in corners]
        reaches = max(xs) >= 0.0 and min(xs) <= width and max(ys) >= 0.0 and min(ys) <= height
        for c in corners:
            used[c] = used.get(c, False) or reaches
    return [(verts[i], used[i]) for i in sorted(used)]


def retail_verts(draw, width, height):
    """A retail draw's drawn vertices as x, y, rgba, u, v, z, and whether a primitive of it reaches into
    the buffer."""
    tw, th = 1 << draw["TEX0"]["tw"], 1 << draw["TEX0"]["th"]
    out = []
    for vert, reaches in drawn(draw, width, height):
        if "uv" in vert:
            u, v = vert["uv"][0] / tw, vert["uv"][1] / th
        else:
            q = vert["q"] or 1.0
            u, v = vert["st"][0] / q, vert["st"][1] / q
        out.append((vert["x"], vert["y"], tuple(vert["rgba"]), u, v, vert["z"], reaches))
    if draw["prim"]["type"] == "sprite":
        # Two opposite corners a sprite, the second's colour and Z over all of it: as the four we draw.
        corners = []
        for a, b in zip(out[0::2], out[1::2]):
            corners += [(x, y, b[2], u, v, b[5], b[6]) for x, u in ((a[0], a[3]), (b[0], b[3]))
                        for y, v in ((a[1], a[4]), (b[1], b[4]))]
        return corners
    return out


def our_verts(draw, width, height):
    """Each vertex as the GS would get it: x, y in pixels, rgba in GS units, u, v, depth from 0 to 1."""
    rect = draw["rect"]
    out = []
    for x, y, z, w, r, g, b, a, u, v in draw["verts"]:
        if w <= 0.0:
            continue  # behind the eye: nowhere on the screen
        out.append(((rect[0] + (x / w * 0.5 + 0.5) * rect[2]) * width,
                    (rect[1] + (y / w * 0.5 + 0.5) * rect[3]) * height,
                    (r * 128.0, g * 128.0, b * 128.0, a * 128.0), u, v, z / w))
    return out


def our_triangles(draw, width, height):
    """Each triangle's three vertices as x and y in pixels times w, then z, w, rgba in GS units, u, v: what
    a point on it is a weighted sum of."""
    rect = draw["rect"]
    verts = [((rect[0] * w + (x + w) * 0.5 * rect[2]) * width, (rect[1] * w + (y + w) * 0.5 * rect[3]) * height, z,
              w, (r * 128.0, g * 128.0, b * 128.0, a * 128.0), u, v)
             for x, y, z, w, r, g, b, a, u, v in draw["verts"]]
    indices = draw["indices"]
    return [(verts[indices[i]], verts[indices[i + 1]], verts[indices[i + 2]]) for i in range(0, len(indices), 3)]


def point_on(triangles, t, gaps):
    """A retail vertex as a point of one of our triangles: the gaps between it and what our vertices come
    to there, least first, or None if it is on none. VU1's clipper (0x45a) makes such vertices where it
    cuts a triangle, weighting the ends' colour, uv and position as here."""
    best = None
    for a, b, c in triangles:
        # Weights l with sum(l * (x - px * w)) = 0 and the same for y: across the two rows.
        ax, bx, cx = a[0] - t[0] * a[3], b[0] - t[0] * b[3], c[0] - t[0] * c[3]
        ay, by, cy = a[1] - t[1] * a[3], b[1] - t[1] * b[3], c[1] - t[1] * c[3]
        la, lb, lc = bx * cy - cx * by, cx * ay - ax * cy, ax * by - bx * ay
        total = la + lb + lc
        if total == 0.0:
            continue
        la, lb, lc = la / total, lb / total, lc / total
        if min(la, lb, lc) < -1e-3:
            continue
        w = la * a[3] + lb * b[3] + lc * c[3]
        if w <= 0.0:
            continue
        point = (t[0], t[1], tuple(la * a[4][k] + lb * b[4][k] + lc * c[4][k] for k in range(4)),
                 la * a[5] + lb * b[5] + lc * c[5], la * a[6] + lb * b[6] + lc * c[6],
                 (la * a[2] + lb * b[2] + lc * c[2]) / w)
        found = gaps.of(point, t)
        score = max(found[k] / TOLERANCE[k] for k in found)
        if best is None or score < best[0]:
            best = (score, found, point)
    return best and best[1:]


def split(draws, target_of, picture):
    """A frame's draws, each with its place in the frame, as those into the picture, and each pass into a
    rendered texture: the draws in a row into one."""
    into, passes = [], []
    last = None
    for n, draw in draws:
        target = target_of(draw)
        if target == picture:
            into.append((n, draw))
        elif target == last:
            passes[-1].append((n, draw))
        else:
            passes.append([(n, draw)])
        last = target
    return into, passes


def retail_runs(draws, width, height):
    """Consecutive draws under one state as one, and the clear before them."""
    runs, clear = [], None
    for n, draw in draws:
        state = {k: v for k, v in draw.items() if k != "verts"}
        if not runs and draw["prim"]["type"] == "sprite" and not draw["prim"]["tme"]:
            clear = draw
            continue
        if runs and runs[-1]["state"] == state:
            runs[-1]["verts"] += retail_verts(draw, width, height)
            runs[-1]["last"] = n
        else:
            runs.append({"state": state, "verts": retail_verts(draw, width, height), "first": n, "last": n})
    for run in runs:
        tex = run["state"]["TEX0"]
        run["size"] = (1 << tex["tw"], 1 << tex["th"]) if run["state"]["prim"]["tme"] else (0, 0)
    return [r for r in runs if r["verts"]], clear


def our_runs(draws, width, height):
    runs = []
    for _, draw in draws:
        # What the GS is told, as retail's runs are split: a skinned mesh and a rigid one of one material
        # are one run there.
        state = {k: draw[k] for k in STATE}
        state["fogged"] = "fog" in draw  # a fog block (start, end, colour) and each vertex's F, in 255ths
        if runs and runs[-1]["state"] == state:
            runs[-1]["verts"] += our_verts(draw, width, height)
            runs[-1]["draws"].append(draw)
            runs[-1]["last"] = draw["index"]
            runs[-1]["skinned"] |= draw["skinned"]
        else:
            runs.append({"state": state, "verts": our_verts(draw, width, height), "first": draw["index"],
                         "last": draw["index"], "skinned": draw["skinned"], "draws": [draw]})
    for run in runs:
        texture = run["state"]["texture"]
        run["size"] = (texture["width"], texture["height"]) if texture else (0, 0)
    return runs


class Grid:
    """Vertices by the pixel they are in, to find those near a point."""

    def __init__(self):
        self.cells = collections.defaultdict(list)

    def add(self, vert, tag):
        self.cells[(int(vert[0] // 1), int(vert[1] // 1))].append((vert, tag))

    def near(self, vert, size):
        """Each (vertex, tag) within NEAR of vert in pixels and, for a texture of that size, texels."""
        cx, cy = int(vert[0] // 1), int(vert[1] // 1)
        for ix in (cx - 1, cx, cx + 1):
            for iy in (cy - 1, cy, cy + 1):
                for other, tag in self.cells.get((ix, iy), ()):
                    if abs(other[0] - vert[0]) > NEAR or abs(other[1] - vert[1]) > NEAR:
                        continue
                    if size[0] and (abs(other[3] - vert[3]) * size[0] > NEAR or
                                    abs(other[4] - vert[4]) * size[1] > NEAR):
                        continue
                    yield other, tag


def state_notes(state, draw):
    """Where retail's registers are not what our material stands for."""
    notes = []
    prim, test, alpha = state["prim"], state["TEST"], state["ALPHA"]
    if draw["blend"] in BLEND:
        abe, abcd = BLEND[draw["blend"]]
        got = f"{alpha['a']}{alpha['b']}{alpha['c']}{alpha['d']}"
        if abe != prim["abe"] or (abcd and abcd != got):
            notes.append(f"blend {draw['blend']} wants abe {abe} {abcd}, retail abe {prim['abe']} {got} "
                         f"fix {alpha['fix']:#x}")
    else:
        notes.append(f"blend {draw['blend']} not in the table: retail abe {prim['abe']} "
                     f"{alpha['a']}{alpha['b']}{alpha['c']}{alpha['d']} fix {alpha['fix']:#x}")
    ztst = test["ztst"] if test["zte"] else 1
    zmsk = state["ZBUF"]["zmsk"]
    if ZMODE.get(draw["zMode"]) != (ztst, zmsk):
        notes.append(f"zMode {draw['zMode']} wants ztst/zmsk {ZMODE.get(draw['zMode'])}, retail {(ztst, zmsk)}")
    # A fragment whose alpha fails GREATER 0 leaves nothing: kept out, or only its Z written with Z masked.
    cut = bool(test["ate"] and test["atst"] == 6 and (test["afail"] == 0 or (test["afail"] == 2 and zmsk)))
    if cut != draw["alphaCut"]:
        notes.append(f"alpha cut {draw['alphaCut']}, retail ate {test['ate']} atst {test['atst']} "
                     f"aref {test['aref']:#x} afail {test['afail']} zmsk {zmsk}")
    elif test["ate"] and (test["atst"] != 6 or test["aref"] != 0):
        notes.append(f"retail alpha test atst {test['atst']} aref {test['aref']:#x}")
    if bool(state["FBA"]["value"]) == draw["alphaWrite"]:
        notes.append(f"alphaWrite {draw['alphaWrite']}, retail fba {state['FBA']['value']}")
    if bool(test["date"]) != draw["destAlphaTest"]:
        notes.append(f"destAlphaTest {draw['destAlphaTest']}, retail date {test['date']} datm {test['datm']}")
    clamp = state["CLAMP"]
    if prim["tme"] and (clamp["wms"], clamp["wmt"]) != ((0, 0) if draw["texWrap"] else (1, 1)):
        notes.append(f"texWrap {draw['texWrap']}, retail wms {clamp['wms']} wmt {clamp['wmt']}")
    # TEX0's TFX: 0 modulate, 1 decal, 2 highlight.
    if prim["tme"] and state["TEX0"]["tfx"] != (1 if draw["decal"] else 2 if draw["highlight"] else 0):
        notes.append(f"highlight {draw['highlight']}, decal {draw['decal']}, retail tfx {state['TEX0']['tfx']}")
    if bool(prim["fge"]) != draw["fogged"]:
        notes.append("retail fogs it" if prim["fge"] else "ours fogs it, retail does not")
    return notes


class Gaps:
    """How far a vertex of ours is from one of retail's, for a retail run's texture size and Z format."""

    def __init__(self, run):
        self.size = run["size"]
        # The Z buffer's largest value: retail's Z is the depth times it, truncated.
        self.far = (1 << ZBITS.get(run["state"]["ZBUF"]["psm"], 32)) - 1
        # With no texture the GS takes rgb as it is, 255 for 1, where a texture's modulate takes 128.
        self.scale = [1.0 if self.size[0] or c == 3 else 255.0 / 128.0 for c in range(4)]

    def of(self, o, t):
        return {
            "px": max(abs(o[0] - t[0]), abs(o[1] - t[1])),
            # VU1 hands the GS FTOI0 of the colour: truncated.
            "colour": max(abs(int(o[2][c] * self.scale[c]) - t[2][c]) for c in range(4)),
            "texel": max(abs(o[3] - t[3]) * self.size[0], abs(o[4] - t[4]) * self.size[1]),
            "z": abs(int(o[5] * self.far) - t[5]),
        }

    def distance(self, o, t):
        d = (o[0] - t[0]) ** 2 + (o[1] - t[1]) ** 2
        if self.size[0]:
            d += ((o[3] - t[3]) * self.size[0]) ** 2 + ((o[4] - t[4]) * self.size[1]) ** 2
        return d

    def nearest_here(self, vert, others, flip=False):
        """Of the other side's vertices in vert's place, the nearest in colour: (gaps, vertex) or None.

        A mesh has several vertices in one place with one uv where its normals split, each lit
        differently, so place alone does not say which is which."""
        best = None
        for other in others:
            o, t = (vert, other) if flip else (other, vert)
            found = self.of(o, t)
            if found["px"] > TOLERANCE["px"] or found["texel"] > TOLERANCE["texel"] or found["z"] > SAME_Z:
                continue
            key = (found["colour"], found["z"], self.distance(o, t))
            if best is None or key < best[0]:
                best = (key, found, other)
        return best and best[1:]


def vertex_gaps(theirs_run, their_verts, our_verts, triangles):
    """The widest gap of each kind between a retail vertex and the one of ours it is, and where.
    `triangles` gives ours, for a retail vertex with no vertex of ours in its place. A vertex none of
    whose primitives reaches into the buffer is kept apart: the last returned is the widest of its gaps,
    as so many tolerances, and where."""
    gaps = Gaps(theirs_run)
    tried = 0
    off = (0.0, None)
    grid = Grid()
    for o in our_verts:
        grid.add(o, None)
    worst = {"px": 0.0, "colour": 0.0, "texel": 0.0, "z": 0.0}
    where = {}
    for t in their_verts:
        close = [o for o, _ in grid.near(t, gaps.size)]
        here = gaps.nearest_here(t, close)
        if not here and tried < POINTS:
            tried += 1
            here = point_on(triangles(), t, gaps)
        if here:
            found, o = here
        else:
            o = min(close or our_verts, key=lambda o: gaps.distance(o, t))
            found = gaps.of(o, t)
        if not t[6]:
            most = max(found[k] / TOLERANCE[k] for k in found)
            if most > off[0]:
                off = (most, (t, o))
            continue
        for k, gap in found.items():
            if gap > worst[k]:
                worst[k] = gap
                where[k] = (t, o)
    return worst, where, off


def unmatched_colour(ours_run, retail_grid, gaps, width, height):
    """The widest colour gap between a vertex of ours and the retail vertex in its place nearest it in
    colour, over all retail draws with that size of texture: a colour only we draw. (gap, retail's, ours)
    or None. Only our vertices inside the buffer: VU1 drops a triangle that is wholly off it (FCAND
    0x3ffff, 0x3b7), and with it the one vertex at a seam that had that colour."""
    worst = None
    for o in ours_run["verts"]:
        if not (0.0 <= o[0] <= width and 0.0 <= o[1] <= height):
            continue
        here = gaps.nearest_here(o, [t for t, _ in retail_grid.near(o, gaps.size)], flip=True)
        if here and (worst is None or here[0]["colour"] > worst[0]):
            worst = (here[0]["colour"], here[1], o)
    return worst


def texel_note(theirs, ours):
    """How a retail texture's first level differs from ours, or nothing. Ours holds alpha with 255 for the
    GS's 0x80 (render/texture_capture.cpp, expandAlpha)."""
    if len(theirs) != len(ours):
        return f"texture: retail's is {len(theirs) // 4} texels, ours {len(ours) // 4}"
    differ, widest = 0, 0
    for at in range(0, len(theirs), 4):
        r, g, b, a = theirs[at:at + 4]
        mine = ours[at:at + 4]
        gap = max(abs(r - mine[0]), abs(g - mine[1]), abs(b - mine[2]), abs(min(a * 255 >> 7, 255) - mine[3]))
        if gap:
            differ += 1
            widest = max(widest, gap)
    return f"texture: {differ} of {len(theirs) // 4} texels differ, by up to {widest}" if differ else None


def box(verts):
    xs, ys = [v[0] for v in verts], [v[1] for v in verts]
    return f"x {min(xs):.0f}..{max(xs):.0f} y {min(ys):.0f}..{max(ys):.0f}"


class Textures:
    """Our textures' pixels by hash, and each pair of a retail texture and one of ours set side by side once."""

    def __init__(self, dump, path):
        pixels = path.with_suffix(".rgba").read_bytes()
        self.ours = {int(key): pixels[at:at + w * h * 4] for key, (at, w, h) in dump["textures"].items()}
        self.notes = {}

    def note(self, texels, texture, counts):
        if not texels or not texture or texture["hash"] not in self.ours:
            return None
        key = (id(texels), texture["hash"])
        if key not in self.notes:
            self.notes[key] = texel_note(texels, self.ours[texture["hash"]])
            counts["textures"] += 1
            counts["textures that differ"] += bool(self.notes[key])
        return self.notes[key]


def compare(theirs, ours, width, height, textures, counts, verbose):
    """Sets retail's runs into one buffer beside ours into it, a line a run."""
    grids = collections.defaultdict(Grid)  # our vertices, by texture size
    for n, run in enumerate(ours):
        for vert in run["verts"]:
            grids[run["size"]].add(vert, n)

    paired = set()
    order = []  # each paired retail run in turn: the run of ours it is, and the two's names
    covered = collections.defaultdict(Grid)  # retail's vertices, by the run of ours they were paired with
    gaps_of = {}
    everything = collections.defaultdict(Grid)  # retail's vertices, by texture size
    for t in theirs:
        for vert in t["verts"]:
            everything[t["size"]].add(vert, None)
    for t in theirs:
        verts = t["verts"]
        inside = [v for v in verts if 0.0 <= v[0] <= width and 0.0 <= v[1] <= height]
        # What VU1's clipper cuts (0x45a) it sends as fans, with vertices of its own where it cut: off the
        # buffer those are on the edge of the GS's coordinates, not where the mesh's are.
        if t["state"]["prim"]["type"] == "trifan":
            verts = inside
        if not verts:
            counts["off the picture"] += 1
            continue
        votes = collections.Counter()
        landed = 0
        for vert in verts:
            on = {tag for _, tag in grids[t["size"]].near(vert, t["size"])}
            landed += bool(on)
            for n in on:
                votes[n] += 1
        label = f"retail {t['first']:4}-{t['last']:<4}"
        if landed * 2 < len(verts):
            if not inside:
                counts["off the picture"] += 1
                continue
            counts["only in retail"] += 1
            print(f"BAD {label} on none of ours: tex {t['size'][0]}x{t['size'][1]} {len(verts)}v {box(verts)}")
            continue
        # A material's passes draw one mesh twice in one place: of the runs nearly all of it lands on,
        # the first not paired yet.
        level = sorted(n for n, count in votes.items() if count * 10 >= len(verts) * 9)
        level = level or [votes.most_common(1)[0][0]]
        # Of those, one that keeps the two sides' order if there is one: a mesh drawn twice, once wholly
        # off the buffer in retail, is else taken for its other draw. The run the last one was paired with
        # counts too: retail's run breaks where the clipper sends a fan, ours does not.
        latest = max((entry[0] for entry in order), default=-1)
        free = [n for n in level if n not in paired]
        ahead = [n for n in free if n >= latest]
        n = ahead[0] if ahead else latest if latest in level else free[0] if free else level[0]
        paired.add(n)
        o = ours[n]
        order.append((n, label, f"ours {o['first']}-{o['last']}"))
        # And the other runs of ours under that state a tenth of it lands on: retail leaves out a mesh
        # that is off the picture, so where we drew one between two it draws, its one run is two of ours.
        on = [n] + sorted(m for m, count in votes.items() if m not in level and count * 10 >= len(verts) and
                          ours[m]["state"] == o["state"])
        for m in on:
            for vert in verts:
                covered[m].add(vert, None)
            gaps_of[m] = Gaps(t)
        notes = state_notes(t["state"], o["state"])
        note = textures.note(t["state"].get("texels"), o["state"]["texture"], counts)
        if note:
            notes.append(note)
        def triangles(on=on):
            if "triangles" not in ours[on[0]]:
                ours[on[0]]["triangles"] = [tri for m in on for draw in ours[m]["draws"]
                                            for tri in our_triangles(draw, width, height)]
            return ours[on[0]]["triangles"]

        worst, where, off = vertex_gaps(t, verts, [vert for m in on for vert in ours[m]["verts"]], triangles)
        over = [k for k in worst if worst[k] > TOLERANCE[k]]
        flag = "ok " if not notes and not over else ("skn" if o["skinned"] else "BAD")
        counts[flag] += 1
        print(f"{flag} {label} ours {o['first']:4}-{o['last']:<4} tex {t['size'][0]}x{t['size'][1]:<4} "
              f"{len(verts):5}v  px {worst['px']:.3f}  colour {worst['colour']:.0f}  "
              f"texel {worst['texel']:.3f}  z {worst['z']:.0f}")
        for note in notes:
            print(f"    {note}")
        shown = [(k, where[k]) for k in over] if verbose else []
        if off[0] > 1.0:
            counts["off"] += 1
            shown.append(("of primitives wholly off the buffer, not counted", off[1]))
        for k, (a, b) in shown:
            print(f"    {k}: retail xy {a[0]:.2f},{a[1]:.2f} z {a[5]} rgba {list(a[2])} "
                  f"uv {a[3]:.4f},{a[4]:.4f}; ours xy {b[0]:.2f},{b[1]:.2f} depth {b[5]:.7f} "
                  f"rgba {[round(c, 2) for c in b[2]]} uv {b[3]:.4f},{b[4]:.4f}")

    # Draw order: the runs of ours that retail's are, in retail's order, should only go forward. Those
    # outside the longest stretch that does are out of place.
    best = []  # for each, the longest forward-going stretch ending there, as places in order
    for i, (n, _, _) in enumerate(order):
        before = max((best[j] for j in range(i) if order[j][0] <= n), key=len, default=[])
        best.append(before + [i])
    kept = set(max(best, key=len, default=[]))
    for i, (n, theirs_name, ours_name) in enumerate(order):
        if i not in kept:
            counts["out of order"] += 1
            print(f"BAD {theirs_name} is drawn out of turn: it is {ours_name}, after "
                  f"{order[i - 1][2] if i else 'nothing'} in retail")

    # What of ours, inside the buffer, has nothing of retail's on it, or none of its colour.
    for n, o in enumerate(ours):
        only = unmatched_colour(o, everything[o["size"]], gaps_of[n], width, height) if n in gaps_of else None
        if only and only[0] > TOLERANCE["colour"]:
            counts["colour only in ours"] += 1
            _, a, b = only
            print(f"BAD ours {o['first']:4}-{o['last']:<4} has a colour {only[0]:.0f} from any of retail's there: "
                  f"retail xy {a[0]:.2f},{a[1]:.2f} rgba {list(a[2])}; ours xy {b[0]:.2f},{b[1]:.2f} "
                  f"rgba {[round(c, 2) for c in b[2]]} uv {b[3]:.4f},{b[4]:.4f}")
        inside = [v for v in o["verts"] if 0.0 <= v[0] <= width and 0.0 <= v[1] <= height]
        bare = [v for v in inside if not any(True for _ in covered[n].near(v, o["size"]))]
        if not bare:
            continue
        flag = "skn" if o["skinned"] else "BAD"
        counts["only in ours" if n not in covered else "partly only in ours"] += flag == "BAD"
        counts["skn"] += flag == "skn" and n not in covered
        what = "on none of retail's" if n not in covered else f"has {len(bare)} of its {len(inside)} vertices " \
            f"in the buffer on none of retail's"
        print(f"{flag} ours {o['first']:4}-{o['last']:<4} {what}: tex {o['size'][0]}x{o['size'][1]} "
              f"{len(bare)}v {box(bare)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("retail")
    parser.add_argument("ours")
    parser.add_argument("-v", action="store_true", help="the worst vertex of each gap over tolerance")
    args = parser.parse_args()
    frame = gsdump.read(args.retail, textures=True)["frames"][-1]
    # A vertex our shader gives no number is written nan, which JSON spells NaN.
    dump = json.loads(re.sub(r"-?\bnan\b", "NaN", Path(args.ours).read_text()))
    textures = Textures(dump, Path(args.ours))
    counts = collections.Counter()
    # The picture is what the frame's last draw, the HUD's, goes into.
    picture = frame[-1]["FRAME"]["fbp"]
    # Depth of field is PsRnd::VSync's work on the finished picture (0x19a9a0), which a dump's frame
    # opens with, before the clear: quarters of the last picture into this one's buffer, then that back
    # over the last where the Z buffer passes. Ours is one copy of the picture drawn back over it at the
    # frame's end, so the two are not set side by side, only the Z they test against.
    clear = next((n for n, d in enumerate(frame) if d["FRAME"]["fbp"] == picture and d["prim"]["type"] == "sprite"
                  and not d["prim"]["tme"] and not d["prim"]["abe"]), 0)
    blur = [d for d in dump["draws"] if d["screen"] and d["renderTarget"] == SCREEN_COPY]
    if clear or blur:
        back = [d["verts"][0]["z"] for d in frame[:clear] if d["FRAME"]["fbp"] != picture]
        ours_z = [int(d["verts"][0][2] / d["verts"][0][3] * 65535.0) for d in blur]
        same = bool(back) == bool(ours_z) and (not back or back[0] == ours_z[0])
        counts["BAD"] += not same
        print(f"{'ok ' if same else 'BAD'} depth of field: retail {clear} draws before its clear, focus Z "
              f"{back[0] if back else 'none'}; ours {len(blur)} draws, focus Z {ours_z[0] if ours_z else 'none'}")
    their_picture, their_passes = split(list(enumerate(frame))[clear:], lambda d: d["FRAME"]["fbp"], picture)
    our_picture, our_passes = split([(d["index"], d) for d in dump["draws"] if d not in blur], lambda d: d["target"],
                                    0)

    width, height = dump["width"], dump["height"]
    theirs, clear = retail_runs(their_picture, width, height)
    if clear:
        want = [int(c * 255.0 + 0.5) for c in dump["clear"][:3]]
        print(f"clear: retail {clear['verts'][-1]['rgba'][:3]}, ours {want}")
    compare(theirs, our_runs(our_picture, width, height), width, height, textures, counts, args.v)

    # Rendered textures: a pass of retail's is the pass of ours in its place in the frame. Retail then
    # draws each rendered texture into its smaller levels, a sprite a level (PsTex::FinishDrawTarget
    # 0x1a0a38); ours has no such levels.
    fills = [p for p in their_passes if all(d["prim"]["type"] == "sprite" and d["prim"]["tme"] for _, d in p)]
    their_passes = [p for p in their_passes if p not in fills]
    if fills:
        print(f"retail fills rendered textures' smaller levels in {len(fills)} passes, which ours does not have")
    if len(their_passes) != len(our_passes):
        counts["passes that differ in number"] = 1
        print(f"BAD retail draws into rendered textures in {len(their_passes)} passes, ours in {len(our_passes)}")
    for theirs, ours in zip(their_passes, our_passes):
        width, height = ours[0][1]["targetSize"]
        print(f"into a rendered texture {width}x{height}: retail {theirs[0][0]}-{theirs[-1][0]}, "
              f"ours {ours[0][1]['index']}-{ours[-1][1]['index']}")
        compare(retail_runs(theirs, width, height)[0], our_runs(ours, width, height), width, height, textures, counts,
                args.v)

    bad = (counts["BAD"] + counts["only in retail"] + counts["only in ours"] + counts["partly only in ours"] +
           counts["colour only in ours"] + counts["textures that differ"] + counts["passes that differ in number"] +
           counts["out of order"])
    print(f"{counts['textures']} textures set beside ours, {counts['textures that differ']} differ")
    print(f"{counts['ok ']} retail runs match, {counts['BAD']} do not, {counts['only in retail']} are on none of "
          f"ours; {counts['only in ours']} runs of ours are on none of retail's and "
          f"{counts['partly only in ours']} partly, {counts['colour only in ours']} have a colour retail has not; "
          f"{counts['skn']} more are skinned; {counts['out of order']} are drawn out of turn; "
          f"{counts['off the picture']} unpaired retail runs are wholly off the buffer, and {counts['off']} "
          f"paired ones differ only in primitives that are")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
