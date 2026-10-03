#!/usr/bin/env python3
"""Inventory every file on Guitar Hero disc images.

    tools/inventory.py <out-dir> <image-or-dir>...

Directories are searched recursively for .chd and .iso. PS2 images are read
through disc.Image, 360 images through their XDVDFS partition. Each image
gets <out-dir>/<slug>/ with:

    summary.json     image, platform, serial, release, sizes, counts
    disc_files.tsv   every file on the disc's own filesystem
    ark_files.tsv    every MAIN.HDR entry
    dtb/             every .dtb in the ark as DTA text, at its ark path
    text/            plain text files from the ark, copied as they are, and
                     the disc's own under text/_disc/
    errors.txt       whatever could not be read or decoded

Ark, cipher and .dtb formats follow src/disc/ark.cpp, src/disc/crypt.cpp and
src/formats/dtb.cpp.
"""
import json
import os
import re
import struct
import sys
import zlib
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import disc

SECTOR = 2048
XDVDFS_BASES = (0xFD90000, 0x2080000, 0x18300000, 0)
XDVDFS_MAGIC = b"MICROSOFT*XBOX*MEDIA"
TEXT_SUFFIXES = {".txt", ".dta", ".csv", ".ini", ".xml", ".cfg", ".htm", ".html", ".lst", ".log"}
TEXT_LIMIT = 4 << 20


class Ps2Volume:
    """A PS2 image's ISO 9660 files, as (path, size, start)."""

    platform = "ps2"

    def __init__(self, path):
        self.image = disc.Image(path)
        self.files = [(p, size, lba) for p, lba, size in self.image.walk()]
        self.boot = self.image.boot_name()
        self.serial = self.boot.replace("_", "-").replace(".", "").upper()
        self.data_bytes = self.image._frames.count * SECTOR

    def read(self, start, offset, size):
        return self.image.read_range(start, offset, size)

    def close(self):
        self.image.__exit__()


class XboxVolume:
    """A 360 image's XDVDFS files, as (path, size, start), as
    src/disc/volume.cpp reads them."""

    platform = "xbox360"

    def __init__(self, path):
        self._f = open(path, "rb")
        self.base = next((b for b in XDVDFS_BASES
                          if self._raw(b + 32 * SECTOR, 20) == XDVDFS_MAGIC), None)
        if self.base is None:
            raise ValueError(f"{path}: no XDVDFS volume")
        root, size = struct.unpack("<II", self._raw(self.base + 32 * SECTOR + 20, 8))
        self.files = []
        self._walk(root, size, "", set())
        self.data_bytes = self._f.seek(0, 2) - self.base
        xex = next((f for f in self.files if f[0].lower() == "default.xex"), None)
        self.serial, self.xex_version = xex_info(self.read(xex[2], 0, min(xex[1], 0x10000))) \
            if xex else (None, None)

    def _raw(self, offset, size):
        self._f.seek(offset)
        return self._f.read(size)

    def _walk(self, sector, size, prefix, seen):
        """A directory is a binary tree of entries: u16 left and right
        (in words), u32 sector, u32 size, u8 attributes, u8 name length."""
        if sector in seen or not size:
            return
        seen.add(sector)
        data = self._raw(self.base + sector * SECTOR, size)

        def node(word):
            at = word * 4
            if at + 14 > len(data):
                return
            left, right, start, length, attrs, n = struct.unpack_from("<HHIIBB", data, at)
            if left == right == 0xFFFF:
                return
            if left:
                node(left)
            name = data[at + 14:at + 14 + n].decode("latin-1")
            if attrs & 0x10:
                self._walk(start, length, f"{prefix}{name}/", seen)
            else:
                self.files.append((prefix + name, length, start))
            if right:
                node(right)
        node(0)

    def read(self, start, offset, size):
        return self._raw(self.base + start * SECTOR + offset, size)

    def close(self):
        self._f.close()


def xex_info(xex):
    """A XEX2's title ID and version, from its execution info header."""
    if xex[:4] != b"XEX2":
        return None, None
    for i in range(struct.unpack_from(">I", xex, 0x14)[0]):
        key, value = struct.unpack_from(">II", xex, 0x18 + 8 * i)
        if key == 0x00040006:
            version, _, title = struct.unpack_from(">III", xex, value + 4)
            return f"{title:08X}", f"{version:08X}"
    return None, None


def open_volume(path):
    return XboxVolume(path) if disc.xbox_title_id(path) else Ps2Volume(path)


# Ciphers (src/disc/crypt.cpp): a 4-byte seed, then each byte XORed.

def rand_stream(data):
    """PS2's table cipher, BinStream::EnableReadEncryption's Rand."""
    seed = struct.unpack_from("<I", data)[0]
    table = []
    for _ in range(256):
        a = (seed * 0x41C64E6D + 0x3039) & 0xFFFFFFFF
        b = (a * 0x41C64E6D + 0x3039) & 0xFFFFFFFF
        table.append((a >> 16) | (b & 0x7FFF0000))
        seed = b
    out = bytearray(len(data) - 4)
    i, j = 0, 0x67
    for o in range(len(out)):
        table[i] ^= table[j]
        out[o] = data[o + 4] ^ (table[i] & 0xFF)
        i = i + 1 if i + 1 < 0xF9 else 0
        j = j + 1 if j + 1 < 0xF9 else 0
    return bytes(out)


def park_miller(data):
    """360's minimal standard generator, its low byte XORed in."""
    key = struct.unpack_from("<i", data)[0]
    out = bytearray(len(data) - 4)
    for o in range(len(out)):
        hi, lo = int(key / 127773), key - int(key / 127773) * 127773  # C truncation
        key = 16807 * lo - 2836 * hi
        if key <= 0:
            key += 0x7FFFFFFF
        out[o] = data[o + 4] ^ (key & 0xFF)
    return bytes(out)


class Ark:
    """MAIN.HDR: version 3, part sizes, string buffer, slots, 20-byte
    entries. A 360's is Park-Miller encrypted whole."""

    def __init__(self, hdr):
        self.encrypted = struct.unpack_from("<I", hdr)[0] != 3
        if self.encrypted:
            hdr = park_miller(hdr)
        at = 0

        def word():
            nonlocal at
            at += 4
            return struct.unpack_from("<I", hdr, at - 4)[0]
        self.version = word()
        if self.version != 3:
            raise ValueError(f"MAIN.HDR version {self.version}, not 3")
        word()  # the part count again
        self.part_sizes = [word() for _ in range(word())]
        string_bytes = word()
        strings = hdr[at:at + string_bytes]
        at += string_bytes
        slots = [word() for _ in range(word())]

        def text(slot):
            at = slots[slot]
            return strings[at:strings.index(b"\0", at)].decode("latin-1")
        self.entries = []
        for _ in range(word()):
            offset, name, dirn, size, usize = (word() for _ in range(5))
            part = 0
            while part + 1 < len(self.part_sizes) and offset >= self.part_sizes[part]:
                offset -= self.part_sizes[part]
                part += 1
            self.entries.append((f"{text(dirn)}/{text(name)}", size, usize, part, offset))


# .dtb (src/formats/dtb.cpp): version byte 1, then the root array.

ARRAYS = {0x10: "()", 0x11: "{}", 0x13: "[]"}
TEXTS = {2: "var", 5: "symbol", 0x12: "string", 7: "#ifdef", 0x20: "#define",
         0x21: "#include", 0x22: "#merge", 0x23: "#ifndef", 0x24: "#autorun", 0x25: "#undef"}
WORDS = {6: "kDataUnhandled", 8: "#else", 9: "#endif"}
DIRECTIVES = {7, 8, 9, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25}


def parse_dtb(plain):
    """The root array as nested (type, value) nodes; raises on a bad file."""
    at = 1

    def take(fmt):
        nonlocal at
        value = struct.unpack_from(fmt, plain, at)
        at += struct.calcsize(fmt)
        return value[0]

    def array(kind):
        count = take("<H")
        take("<I")  # line, id
        return kind, [node() for _ in range(count)]

    def node():
        nonlocal at
        kind = take("<I")
        if kind in ARRAYS:
            return array(kind)
        if kind == 0:
            return kind, take("<i")
        if kind == 1:
            return kind, take("<f")
        if kind in TEXTS:
            n = take("<I")
            if at + n > len(plain):
                raise ValueError(f"string past end at {at}")
            at += n
            return kind, plain[at - n:at].decode("latin-1")
        if kind in WORDS:
            take("<I")
            return kind, None
        raise ValueError(f"node type {kind:#x} at {at - 4}")

    if not plain or plain[0] != 1:
        raise ValueError("version byte not 1")
    root = array(0x10)
    return root, len(plain) - at


def decode_dtb(data):
    """(cipher, root, trailing bytes): PS2's cipher, then 360's, then none."""
    errors = []
    for name, decrypt in (("rand_stream", rand_stream), ("park_miller", park_miller),
                          ("plain", lambda d: d)):
        try:
            root, trailing = parse_dtb(decrypt(data))
            return name, root, trailing
        except (ValueError, struct.error, IndexError) as error:
            errors.append(f"{name}: {error}")
    raise ValueError("; ".join(errors))


def f32(value):
    """The shortest decimal that is this float32 again."""
    for digits in range(1, 10):
        text = f"{value:.{digits}g}"
        if struct.unpack("<f", struct.pack("<f", float(text)))[0] == value:
            break
    if "e" not in text and "." not in text and text not in ("inf", "-inf", "nan"):
        text += ".0"
    return text


SAFE_SYMBOL = re.compile(r"^[^\s()\[\]{}'\";]+$")


def atom(kind, value):
    if kind == 0:
        return str(value)
    if kind == 1:
        return f32(value)
    if kind == 0x12:
        return '"' + value.replace("\\", "\\\\").replace('"', "\\q").replace("\n", "\\n") + '"'
    if kind == 5:
        return value if SAFE_SYMBOL.match(value) else "'" + value.replace("'", "\\'") + "'"
    if kind == 2:
        return f"${value}"
    if kind in WORDS:
        return WORDS[kind]
    return f"{TEXTS[kind]} {value}"


def render(node, indent=0, width=100):
    """DTA text: an array on one line when it fits and holds no directive,
    else its leading atoms on the first line and one child per line after."""
    kind, value = node
    if kind not in ARRAYS:
        return atom(kind, value)
    open_, close = ARRAYS[kind]
    line = open_ + " ".join(render(child, 0, 1 << 30) for child in value) + close
    directive = any(k in DIRECTIVES for k, _ in value)
    if not directive and "\n" not in line and indent + len(line) <= width:
        return line
    head = 0
    while head < len(value) and value[head][0] not in ARRAYS and value[head][0] not in DIRECTIVES:
        head += 1
    first = " ".join(atom(*child) for child in value[:head])
    body = lines(value[head:], indent + 1, width)
    sep = "\n" + " " * (indent + 1)
    return open_ + first + (sep if first and body else "") + sep.join(body) + close


def lines(nodes, indent=0, width=100):
    """One line per node, a #define's body array kept on its line."""
    out = []
    for i, node in enumerate(nodes):
        if i and nodes[i - 1][0] == 0x20 and node[0] in ARRAYS:
            out[-1] += " " + render(node, indent, width)
        else:
            out.append(render(node, indent, width))
    return out


def render_root(root):
    """The root array's children, one per line, as a .dta file holds them."""
    return "\n".join(lines(root[1])) + "\n"


def slug(path):
    return re.sub(r"[^a-z0-9]+", "-", path.stem.lower()).strip("-")


def release_of(serial):
    return next((r for r in disc.known("release") if r["serial"] == serial), None)


def unpack(data, size, usize):
    """An ark entry's bytes, inflated when its sizes differ."""
    if size == usize or usize == 0:
        return data
    for wbits in (15, -15, 31):
        try:
            return zlib.decompress(data, wbits)
        except zlib.error:
            pass
    raise ValueError(f"compressed ({size} -> {usize}) in an unknown format")


def inventory(image, out_root):
    """Write one image's inventory; returns its summary."""
    image = Path(image)
    volume = open_volume(image)
    platform = volume.platform
    out = Path(out_root) / f"{platform}-{slug(image)}"
    out.mkdir(parents=True, exist_ok=True)
    errors = []
    by_name = {p.upper(): (p, size, start) for p, size, start in volume.files}

    with open(out / "disc_files.tsv", "w", newline="\n") as f:
        f.write("path\tsize\n")
        for p, size, _ in sorted(volume.files, key=lambda e: e[0].upper()):
            f.write(f"{p}\t{size}\n")
    for p, size, start in volume.files:
        if (Path(p).suffix.lower() in TEXT_SUFFIXES | {".cnf"}) and size <= TEXT_LIMIT:
            target = out / "text" / "_disc" / p
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(volume.read(start, 0, size))

    summary = {"image": image.name, "platform": platform, "serial": volume.serial,
               "data_bytes": volume.data_bytes, "disc_files": len(volume.files),
               "disc_bytes": sum(s for _, s, _ in volume.files)}
    if platform == "ps2":
        summary["boot"] = volume.boot
    else:
        summary["xex_version"] = volume.xex_version
    rel = release_of(volume.serial)
    if rel:
        summary["release"] = {k: rel[k] for k in ("name", "game", "version", "role") if k in rel}

    hdr = by_name.get("GEN/MAIN.HDR")
    if not hdr:
        errors.append("no GEN/MAIN.HDR")
        ark = None
    else:
        ark = Ark(volume.read(hdr[2], 0, hdr[1]))
        parts = [by_name.get(f"GEN/MAIN_{i}.ARK") for i in range(len(ark.part_sizes))]
        for i, part in enumerate(parts):
            if not part:
                errors.append(f"no GEN/MAIN_{i}.ARK")
            elif part[1] != ark.part_sizes[i]:
                errors.append(f"GEN/MAIN_{i}.ARK is {part[1]} bytes, MAIN.HDR says {ark.part_sizes[i]}")
        stray = sorted(p for p in by_name if re.fullmatch(r"GEN/MAIN_\d+\.ARK", p)
                       and int(p[9:-4]) >= len(ark.part_sizes))
        errors += [f"{by_name[p][0]} not in MAIN.HDR" for p in stray]

        with open(out / "ark_files.tsv", "w", newline="\n") as f:
            f.write("path\tsize\tuncompressed\tpart\toffset\n")
            for p, size, usize, part, offset in sorted(ark.entries):
                f.write(f"{p}\t{size}\t{usize}\t{part}\t{offset}\n")

        suffixes = {}
        for p, *_ in ark.entries:
            suffix = Path(p).suffix.lower() or "(none)"
            suffixes[suffix] = suffixes.get(suffix, 0) + 1
        summary.update({
            "hdr_encrypted": ark.encrypted,
            "ark_parts": ark.part_sizes,
            "ark_files": len(ark.entries),
            "ark_bytes": sum(e[1] for e in ark.entries),
            "ark_compressed": sum(1 for e in ark.entries if e[1] != e[2] and e[2]),
            "ark_suffixes": dict(sorted(suffixes.items(), key=lambda kv: -kv[1])),
        })

        def fetch(entry):
            p, size, usize, part, offset = entry
            return unpack(volume.read(parts[part][2], offset, size), size, usize)

        decoded = failed = texts = 0
        ciphers = {}
        for entry in sorted(ark.entries):
            p = entry[0]
            suffix = Path(p).suffix.lower()
            safe = p.replace("../", "_up_/").lstrip("./")
            try:
                if suffix == ".dtb":
                    cipher, root, trailing = decode_dtb(fetch(entry))
                    ciphers[cipher] = ciphers.get(cipher, 0) + 1
                    if trailing:
                        errors.append(f"{p}: {trailing} bytes after the root array")
                    target = out / "dtb" / Path(safe).with_suffix(".dta")
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_text(render_root(root), encoding="utf-8", newline="\n")
                    decoded += 1
                elif suffix in TEXT_SUFFIXES and max(entry[1], entry[2]) <= TEXT_LIMIT:
                    target = out / "text" / safe
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(fetch(entry))
                    texts += 1
            except (ValueError, struct.error, OSError) as error:
                errors.append(f"{p}: {error}")
                if suffix == ".dtb":
                    failed += 1
        summary.update({"dtb_decoded": decoded, "dtb_failed": failed, "dtb_ciphers": ciphers,
                        "text_files": texts})

    volume.close()
    summary["errors"] = len(errors)
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", newline="\n")
    if errors:
        (out / "errors.txt").write_text("\n".join(errors) + "\n", newline="\n")
    return out.name, summary


def images(args):
    for arg in map(Path, args):
        if arg.is_dir():
            yield from sorted(p for p in arg.rglob("*") if p.suffix.lower() in (".chd", ".iso")
                              and not p.name.startswith("."))
        else:
            yield arg


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, paths = sys.argv[1], list(images(sys.argv[2:]))
    with ProcessPoolExecutor(max_workers=min(len(paths), os.cpu_count() or 4)) as pool:
        futures = {pool.submit(inventory, p, out): p for p in paths}
        for future, path in futures.items():
            try:
                name, s = future.result()
            except Exception as error:  # one bad image must not hide the rest
                print(f"{path.name}: FAILED {error!r}", file=sys.stderr)
                continue
            print(f"{name}: {s['serial']}, {s.get('ark_files', 0)} ark files, "
                  f"dtb {s.get('dtb_decoded', 0)} ok / {s.get('dtb_failed', 0)} failed, "
                  f"{s['errors']} errors")


if __name__ == "__main__":
    main()
