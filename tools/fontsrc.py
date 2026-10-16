#!/usr/bin/env python3
"""Every font's texture off the discs, for tools/fonttrace.py and fontpack.py.

    tools/fontsrc.py [image-or-dir ...] [--out .planning/fonts/src]

The images default to every .chd and .iso in game/. Each scene in a disc's archive that holds a Font is read; each Font's
Mat names its Tex, whose bitmap is decoded to RGBA and kept once as
tex/<hash>.png, the hash the first 16 hex of the pixels' SHA-1 (the "tex" a
trace config names). fonts.json lists every Font found: its disc, scene,
cell, characters and texture.
"""
import argparse
import hashlib
import io
import json
import re
import struct
import zlib
from pathlib import Path

from PIL import Image

import inventory as inv
from releases import discs_dir

ROOT = Path(__file__).resolve().parent.parent
POW = {1 << i for i in range(2, 13)}
MARK = b"\xad\xde\xad\xde"
FONT = re.compile(rb"\x04\x00\x00\x00Font([\x01-\x7f]\x00\x00\x00)")


def inflate(f):
    """A compressed milo's blocks joined, else f as it is."""
    if len(f) < 16:
        return f
    magic, first, count, _ = struct.unpack_from("<4I", f)
    if magic not in (0xCABEDEAF, 0xCBBEDEAF, 0xCDBEDEAF):
        return f
    out, at = [], first
    for i in range(count):
        n = struct.unpack_from("<I", f, 16 + 4 * i)[0] & 0xFFFFFF
        block = f[at:at + n]
        out.append(block if magic == 0xCABEDEAF else zlib.decompress(block if magic == 0xCBBEDEAF else block[4:], -15))
        at += n
    return b"".join(out)


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def lstr(b, o):
    n = u32(b, o)
    return b[o + 4:o + 4 + n].decode("latin-1"), o + 4 + n


def parse(raw, strict_end=True):
    """A milo dir's (version, root, entries, bodies), bodies as (class, name, bytes); after src/milo/milo.cpp."""
    ver = u32(raw, 0)
    o = 4
    if ver not in (10, 24, 25):
        return None
    root = None
    if ver != 10:
        cls, o = lstr(raw, o)
        name, o = lstr(raw, o)
        root = (cls, name)
        o += 8
    n = u32(raw, o)
    o += 4
    if n > 20000:
        return None
    entries = []
    for _ in range(n):
        c, o = lstr(raw, o)
        nm, o = lstr(raw, o)
        if not c or not c.isalnum():
            return None
        entries.append((c, nm))
    if ver == 10:
        ne = u32(raw, o)
        o += 4
        for _ in range(ne):
            _, o = lstr(raw, o)
    order = ([root] if root else []) + entries
    bodies = []
    start = search = o
    for i in range(len(order)):
        last = i + 1 == len(order)
        while True:
            m = raw.find(MARK, search)
            if m < 0:
                return ver, root, entries, bodies
            after = m + 4
            ends = (after == len(raw) or not strict_end) if last else (after + 4 <= len(raw) and u32(raw, after) < 0x100)
            if ends:
                bodies.append((order[i][0], order[i][1], raw[start:m]))
                start = search = after
                break
            search = m + 1
    return ver, root, entries, bodies


def parse_all(raw):
    """The top dir, then any inline subdir found later in the file."""
    top = parse(raw)
    out = [top] if top else []
    ver = u32(raw, 0)
    if ver == 10:
        return out
    pat = struct.pack("<I", ver)
    i = 4
    while (i := raw.find(pat, i + 1)) >= 0:
        try:
            n = u32(raw, i + 4)
            cls = raw[i + 8:i + 8 + n]
            if not 3 <= n <= 40 or not cls.endswith(b"Dir") or not cls.isalpha():
                continue
            sub = parse(raw[i:], strict_end=False)
        except (struct.error, UnicodeDecodeError):
            continue
        if sub and sub[2]:
            out.append(sub)
    return out


def strings(b):
    """Every length-prefixed printable string in a body, with its offset."""
    out = []
    for m in re.finditer(rb"([\x01-\xff])\x00\x00\x00", b):
        n, o = m.group(1)[0], m.start()
        t = b[o + 4:o + 4 + n]
        if len(t) == n and all(32 <= c < 127 for c in t):
            out.append((o, t.decode()))
    return out


def ps2_bitmap(b, i):
    bpp = b[i + 1]
    w, h, row = struct.unpack_from("<3H", b, i + 7)
    colours = 16 if bpp == 4 else 256
    at = i + 32
    pal = [b[at + 4 * c:at + 4 * c + 4] for c in range(colours)]
    if bpp == 8:
        # the GS's CSM1 palette order
        for c in range(256):
            if c & 0x18 == 8:
                pal[c], pal[c + 8] = pal[c + 8], pal[c]
    at += 4 * colours
    px = bytearray()
    for y in range(h):
        line = b[at + y * row:at + (y + 1) * row]
        for x in range(w):
            k = line[x] if bpp == 8 else (line[x // 2] >> (4 * (x & 1))) & 15
            r, g, bb, a = pal[k]
            px += bytes((r, g, bb, min(255, a * 2)))
    return Image.frombytes("RGBA", (w, h), bytes(px)), f"PS2 {bpp}-bit palette"


def xbox_bitmap(b, i):
    enc = b[i + 2]
    w, h = struct.unpack_from("<2H", b, i + 7)
    fourcc = {8: b"DXT1", 24: b"DXT5", 32: b"ATI2"}[enc]
    size = w * h // 2 if fourcc == b"DXT1" else w * h
    body = bytearray(b[i + 32:i + 32 + size])
    # the 360's 16-bit words, byte-swapped
    for k in range(0, len(body) - 1, 2):
        body[k], body[k + 1] = body[k + 1], body[k]
    dds = bytearray(b"DDS ")
    dds += struct.pack("<7I", 124, 0x1007 | 0x80000, h, w, len(body), 0, 1)
    dds += b"\0" * 44
    dds += struct.pack("<2I4s5I", 32, 4, fourcc, 0, 0, 0, 0, 0)
    dds += struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    return Image.open(io.BytesIO(bytes(dds) + bytes(body))).convert("RGBA"), "360 " + fourcc.decode()


def bitmap(b, xbox):
    """A Tex body's bitmap: the first header that reads as one."""
    for i in range(len(b) - 32):
        if b[i] != 1 or b[i + 13:i + 32] != b"\0" * 19:
            continue
        w, h = struct.unpack_from("<2H", b, i + 7)
        if w not in POW or h not in POW:
            continue
        if xbox and b[i + 2] in (8, 24, 32):
            return xbox_bitmap(b, i)
        if not xbox and b[i + 1] in (4, 8):
            return ps2_bitmap(b, i)
    return None, None


def font_fields(body):
    """A Font body's Mat, cell, size and characters."""
    ss = strings(body)
    mat = next(((o, t) for o, t in ss if t.endswith(".mat")), None)
    if mat is None:
        return None
    o = mat[0] + 4 + len(mat[1])
    cw, ch, f3 = struct.unpack_from("<3f", body, o)
    o += 12
    # GH1's (rev 7) and GH2's (rev 15) put up to 12 bytes before the string
    for skip in range(0, 16, 4):
        n = u32(body, o + skip)
        if 0 < n < 2000 and o + skip + 4 + n <= len(body):
            chars = body[o + skip + 4:o + skip + 4 + n]
            if all(c >= 32 for c in chars):
                out = dict(mat=mat[1], cell=[cw, ch], f3=f3, chars=chars.decode("latin-1"))
                if body[:4] == b"\x0f\0\0\0":
                    boxes = packed_boxes(body, o + skip + 4 + n, out["chars"])
                    if boxes:
                        out["boxes"] = boxes
                return out
    return dict(mat=mat[1], cell=[cw, ch], f3=f3, chars=None)


def packed_boxes(body, o, chars):
    """A packed rev 15 Font's box per character, [x, y, w, h] in its bitmap's
    pixels, from what follows its characters (RndFont::Load 0x1fa9f0): the
    kerning, a string, monospace and packed bytes, the bitmap's size, the
    cell in uv, then 256 of {u, v, width in cells, advance}. None unpacked."""
    try:
        if body[o]:
            o += 5 + 8 * u32(body, o + 1)
        else:
            o += 1
        o += 4 + u32(body, o)
        packed = body[o + 1]
        o += 2
        bw, bh = struct.unpack_from("<II", body, o)
        cell = struct.unpack_from("<2f", body, o + 8)
        o += 16
        info = [struct.unpack_from("<4f", body, o + 16 * i) for i in range(256)]
    except (IndexError, struct.error):
        return None
    if not packed:
        return None
    cw, ch = cell[0] * bw, cell[1] * bh
    return {c: [round(info[ord(c)][0] * bw, 3), round(info[ord(c)][1] * bh, 3), round(info[ord(c)][2] * cw, 3),
                round(ch, 3)] for c in chars if ord(c) < 256}


def scan(image, tag, out, records):
    vol = inv.open_volume(Path(image))
    by = {p.upper(): (p, size, start) for p, size, start in vol.files}
    hdr = by["GEN/MAIN.HDR"]
    ark = inv.Ark(vol.read(hdr[2], 0, hdr[1]))
    parts = [by[f"GEN/MAIN_{i}.ARK"] for i in range(len(ark.part_sizes))]
    for path, size, usize, part, offset in ark.entries:
        if not size or not path.lower().endswith((".milo_ps2", ".rnd_ps2", ".milo_xbox", ".rnd_xbox")):
            continue
        raw = inflate(inv.unpack(vol.read(parts[part][2], offset, size), size, usize))
        if not FONT.search(raw):
            continue
        tables = parse_all(raw)
        if not tables:
            continue
        bodies = [b for t in tables for b in t[3]]
        byname = {n: (c, b) for c, n, b in bodies}
        seen = set()
        for c, name, body in reversed(bodies):
            if c != "Font" or name in seen or body[:4] not in (b"\x07\0\0\0", b"\x0e\0\0\0", b"\x0f\0\0\0"):
                continue
            seen.add(name)
            fields = font_fields(body)
            mat = byname.get(fields["mat"]) if fields else None
            tex = next((t for _, t in strings(mat[1]) if t.endswith(".tex")), None) if mat else None
            texbody = byname.get(tex) if tex else None
            img, kind = bitmap(texbody[1], tables[0][0] == 25) if texbody else (None, None)
            if img is None:
                continue
            h = hashlib.sha1(img.tobytes()).hexdigest()[:16]
            if not (out / "tex" / f"{h}.png").exists():
                img.save(out / "tex" / f"{h}.png")
            records.append(dict(disc=tag, scene=path, font=name, **fields, tex=tex, tex_hash=h,
                                tex_size=list(img.size), tex_kind=kind))
    print(f"{tag}: {sum(r['disc'] == tag for r in records)} fonts")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("images", type=Path, nargs="*", help="disc images or folders of them, default game/")
    ap.add_argument("--out", type=Path, default=ROOT / ".planning/fonts/src")
    args = ap.parse_args()
    out = args.out
    images = []
    for a in args.images or [discs_dir()]:
        images += sorted(p for p in a.rglob("*") if p.suffix.lower() in (".chd", ".iso")) if a.is_dir() else [a]
    (out / "tex").mkdir(parents=True, exist_ok=True)
    records = []
    for image in images:
        try:
            scan(image, inv.slug(image), out, records)
        except (KeyError, OSError, ValueError) as e:
            print(f"{image.name}: skipped, {e}")
    (out / "fonts.json").write_text(json.dumps(records, indent=1, ensure_ascii=False))
    print(f"{len(records)} fonts, {len({r['tex_hash'] for r in records})} textures, in {out}")


if __name__ == "__main__":
    main()
