#!/usr/bin/env python3
"""Read PS2 disc images and identify the executables on them.

    tools/disc.py identify [--image-hash] <image-or-elf>...
    tools/disc.py extract <image> <path-on-disc> <out>
    tools/disc.py find <serial|game> <path>...  print the first image of that
                                             release, or of any of a game's
                                             content releases (gh1)
    tools/disc.py boot-elf <image-or-elf> <out>  extract the boot executable (or take
                                             one already extracted, named as on the
                                             disc), refusing anything but a known
                                             supported one

Images are .iso or .bin (ISO 9660, cooked or raw sectors) or .chd, read in
place through libchdr from lib/libchdr, which is built into build/chdr on
first use. Executables are identified by SHA-1 against config/executables.toml.
`--image-hash` also hashes the data track's 2048-byte sectors, which is what
Redump publishes for each disc.
"""
import ctypes
import hashlib
import struct
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KNOWN = ROOT / "config" / "executables.toml"
CHDR_SRC = ROOT / "lib" / "libchdr"
CHDR_BUILD = ROOT / "build" / "chdr"
SECTOR = 2048
IMAGE_SUFFIXES = (".iso", ".bin", ".chd")
# (frame size, offset of the 2048 data bytes in the frame)
PLAIN_LAYOUTS = ((2048, 0), (2448, 0), (2352, 16), (2352, 24), (2448, 16), (2448, 24))
CHD_OFFSETS = (0, 16, 24)
CHT2 = int.from_bytes(b"CHT2", "big")


class FileFrames:
    """Frames of a plain image file."""

    def __init__(self, path, frame):
        self._f = open(path, "rb")
        self.frame = frame
        self.count = self._f.seek(0, 2) // frame

    def read(self, n):
        self._f.seek(n * self.frame)
        return self._f.read(self.frame)

    def close(self):
        self._f.close()


def _chdr():
    """libchdr as a ctypes library, built from lib/libchdr the first time."""
    def built():
        # is_file drops symlinks left dangling by a build that never finished.
        return [p for pattern in ("libchdr*.dylib", "libchdr*.so*", "*chdr*.dll")
                for p in CHDR_BUILD.glob(pattern) if p.is_file()]

    libs = built()
    if not libs:
        print("building libchdr", file=sys.stderr)
        subprocess.run(["cmake", "-S", str(CHDR_SRC), "-B", str(CHDR_BUILD),
                        "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED_LIBS=ON",
                        "-DCHDR_WANT_TESTS=OFF"], check=True, capture_output=True)
        subprocess.run(["cmake", "--build", str(CHDR_BUILD), "--target", "chdr"],
                       check=True, capture_output=True)
        libs = built()
        if not libs:
            sys.exit(f"libchdr built no library in {CHDR_BUILD}")
    lib = ctypes.CDLL(str(sorted(libs)[0]))
    lib.chd_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p,
                             ctypes.POINTER(ctypes.c_void_p)]
    lib.chd_read.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
    lib.chd_get_metadata.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
                                     ctypes.c_void_p, ctypes.c_uint32,
                                     ctypes.POINTER(ctypes.c_uint32),
                                     ctypes.POINTER(ctypes.c_uint32),
                                     ctypes.POINTER(ctypes.c_uint8)]
    lib.chd_close.argtypes = [ctypes.c_void_p]
    lib.chd_error_string.argtypes = [ctypes.c_int]
    lib.chd_error_string.restype = ctypes.c_char_p
    return lib


class ChdFrames:
    """Frames of the first track of a CD-type CHD, decompressed hunk by hunk."""

    def __init__(self, path):
        with open(path, "rb") as f:
            header = f.read(64)
        if header[:8] != b"MComprHD" or struct.unpack_from(">I", header, 12)[0] != 5:
            raise ValueError(f"{path}: not a v5 CHD")
        self.hunk_bytes, self.frame = struct.unpack_from(">II", header, 56)

        self._lib = _chdr()
        self._chd = ctypes.c_void_p()
        err = self._lib.chd_open(str(path).encode(), 1, None, ctypes.byref(self._chd))
        if err:
            raise OSError(f"{path}: {self._lib.chd_error_string(err).decode()}")
        self.count = self._first_track_frames(path)
        self._buf = ctypes.create_string_buffer(self.hunk_bytes)
        self._hunk = -1

    def _first_track_frames(self, path):
        out = ctypes.create_string_buffer(256)
        length = ctypes.c_uint32()
        err = self._lib.chd_get_metadata(self._chd, CHT2, 0, out, 256, ctypes.byref(length),
                                         None, None)
        if err:
            raise ValueError(f"{path}: no CD track metadata; only CD-type CHDs are supported")
        fields = dict(f.split(":", 1) for f in out.value.decode().split())
        return int(fields["FRAMES"])

    def read(self, n):
        per_hunk = self.hunk_bytes // self.frame
        hunk = n // per_hunk
        if hunk != self._hunk:
            err = self._lib.chd_read(self._chd, hunk, self._buf)
            if err:
                raise OSError(f"hunk {hunk}: {self._lib.chd_error_string(err).decode()}")
            self._hunk = hunk
        start = (n % per_hunk) * self.frame
        return self._buf.raw[start:start + self.frame]

    def close(self):
        self._lib.chd_close(self._chd)


class Image:
    """An ISO 9660 filesystem on a .iso, .bin or .chd image."""

    def __init__(self, path):
        self.path = Path(path)
        if self.path.suffix.lower() == ".chd":
            self._frames = ChdFrames(self.path)
            self._offset = self._find_offset(CHD_OFFSETS)
        else:
            for frame, offset in PLAIN_LAYOUTS:
                self._frames = FileFrames(self.path, frame)
                if self._is_iso(offset):
                    self._offset = offset
                    break
                self._frames.close()
            else:
                raise ValueError(f"{self.path}: no ISO 9660 volume descriptor")

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self._frames.close()

    def _is_iso(self, offset):
        return self._frames.read(16)[offset + 1:offset + 6] == b"CD001"

    def _find_offset(self, offsets):
        for offset in offsets:
            if self._is_iso(offset):
                return offset
        raise ValueError(f"{self.path}: no ISO 9660 volume descriptor")

    def _sector(self, lba):
        return self._frames.read(lba)[self._offset:self._offset + SECTOR]

    def _read(self, lba, length):
        count = -(-length // SECTOR)
        return b"".join(self._sector(lba + k) for k in range(count))[:length]

    def _entries(self, lba, length):
        data = self._read(lba, length)
        i = 0
        while i < len(data):
            n = data[i]
            if n == 0:  # records never cross a sector; skip the padding
                i = (i // SECTOR + 1) * SECTOR
                continue
            ent_lba, ent_len = struct.unpack_from("<I", data, i + 2)[0], struct.unpack_from("<I", data, i + 10)[0]
            is_dir = bool(data[i + 25] & 2)
            name = data[i + 33:i + 33 + data[i + 32]].decode("latin-1").split(";")[0]
            if name not in ("\0", "\1"):
                yield name, ent_lba, ent_len, is_dir
            i += n

    def _root(self):
        pvd = self._sector(16)
        return struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]

    def _find(self, lba, length, name, want_dir):
        for ent, ent_lba, ent_len, is_dir in self._entries(lba, length):
            if ent.upper() == name.upper() and is_dir == want_dir:
                return ent_lba, ent_len
        raise FileNotFoundError(f"{self.path}: {name}")

    def listdir(self, path=""):
        lba, length = self._root()
        for part in [p for p in path.strip("/").split("/") if p]:
            lba, length = self._find(lba, length, part, want_dir=True)
        return [name for name, *_ in self._entries(lba, length)]

    def read(self, path):
        parts = [p for p in path.strip("/").split("/") if p]
        lba, length = self._root()
        for i, part in enumerate(parts):
            lba, length = self._find(lba, length, part, want_dir=i < len(parts) - 1)
        return self._read(lba, length)

    def walk(self, lba=None, length=None, prefix=""):
        """(path, lba, length) for every file, depth first."""
        if lba is None:
            lba, length = self._root()
        for name, ent_lba, ent_len, is_dir in self._entries(lba, length):
            if is_dir:
                yield from self.walk(ent_lba, ent_len, f"{prefix}{name}/")
            else:
                yield f"{prefix}{name}", ent_lba, ent_len

    def read_range(self, lba, offset, size):
        """`size` bytes from `offset` into the file starting at sector `lba`."""
        skip = offset % SECTOR
        return self._read(lba + offset // SECTOR, skip + size)[skip:]

    def boot_name(self):
        for line in self.read("SYSTEM.CNF").decode("latin-1").splitlines():
            if line.strip().startswith("BOOT2"):
                return line.split("\\")[-1].split(";")[0].strip()
        raise ValueError(f"{self.path}: SYSTEM.CNF has no BOOT2")

    def executables(self):
        """{name: bytes} for the boot executable and any other ELF in the root."""
        names = {self.boot_name()}
        names |= {n for n in self.listdir() if n.upper().endswith(".ELF")}
        return {n: self.read(n) for n in sorted(names)}

    def data_sha1(self):
        h = hashlib.sha1()
        for n in range(self._frames.count):
            h.update(self._sector(n))
        return h.hexdigest()


def known(table="executable"):
    return tomllib.loads(KNOWN.read_text()).get(table, []) if KNOWN.exists() else []


def lookup(sha1):
    return next((e for e in known() if e["sha1"] == sha1), None)


def identify_bytes(data):
    """An executable's SHA-1 and its known entry, or None."""
    sha1 = hashlib.sha1(data).hexdigest()
    return sha1, lookup(sha1)


def xbox_title_id(path):
    """An Xbox 360 disc image's title ID ("415607E7"), else None. As
    src/disc/volume.cpp reads it: XDVDFS's root directory, then
    default.xex's execution info."""
    if Path(path).suffix.lower() != ".iso":
        return None
    with open(path, "rb") as f:
        def read(offset, size):
            f.seek(offset)
            return f.read(size)

        base = next((b for b in (0xFD90000, 0x2080000, 0x18300000, 0)
                     if read(b + 32 * SECTOR, 20) == b"MICROSOFT*XBOX*MEDIA"), None)
        if base is None:
            return None
        root, size = struct.unpack("<II", read(base + 32 * SECTOR + 20, 8))
        listing = read(base + root * SECTOR, size)
        at = listing.find(b"\x0bdefault.xex")
        if at < 13:
            return None
        start, length = struct.unpack_from("<II", listing, at - 9)
        xex = read(base + start * SECTOR, min(length, 0x10000))
        if xex[:4] != b"XEX2":
            return None
        for i in range(struct.unpack_from(">I", xex, 0x14)[0]):
            key, value = struct.unpack_from(">II", xex, 0x18 + 8 * i)
            if key == 0x00040006:
                return f"{struct.unpack_from('>I', xex, value + 12)[0]:08X}"
    return None


def release(boot_name):
    """The release a boot executable belongs to, from the serial in its name:
    SLUS_214.47 is SLUS-21447."""
    serial = boot_name.replace("_", "-").replace(".", "").upper()
    return next((r for r in known("release") if r["serial"] == serial), None)


def is_image(path):
    return Path(path).suffix.lower() in IMAGE_SUFFIXES


def load_elf(path):
    """An ELF file, or the boot executable of a disc image."""
    if is_image(path):
        with Image(path) as img:
            return img.read(img.boot_name())
    return Path(path).read_bytes()


def executables_in(path):
    """[(label, bytes)] for an image or a bare ELF."""
    path = Path(path)
    if is_image(path):
        with Image(path) as img:
            return [(f"{path.name}:{n}", d) for n, d in img.executables().items()]
    return [(path.name, path.read_bytes())]


def identify(paths, image_hash=False):
    for path in paths:
        if is_image(path):
            with Image(path) as img:
                rel = release(img.boot_name())
            if rel:
                role = {"engine": "engine disc", "content": "content disc"}.get(
                    rel.get("role"), "not supported yet")
                print(f"{Path(path).name}: {rel['name']} v{rel['version']}, {role}")
            else:
                print(f"{Path(path).name}: not a known Guitar Hero release")
        for label, data in executables_in(path):
            sha1, entry = identify_bytes(data)
            name = entry["name"] if entry else "UNKNOWN"
            print(f"  {sha1}  {len(data):>9}  {name:<16} {label}")
        if image_hash and is_image(path):
            with Image(path) as img:
                sha1 = img.data_sha1()
            match = next((d["name"] for d in known("disc") if d["data_sha1"] == sha1), None)
            verdict = f"matches {match}" if match else "matches no known dump"
            print(f"  {sha1}  data track, {verdict}")


def main():
    args = sys.argv[1:]
    if args[:1] == ["identify"]:
        flag = "--image-hash" in args
        identify([a for a in args[1:] if a != "--image-hash"], image_hash=flag)
    elif args[:1] == ["extract"] and len(args) == 4:
        with Image(args[1]) as img:
            Path(args[3]).write_bytes(img.read(args[2]))
    elif args[:1] == ["find"] and len(args) >= 3:
        # A game (gh1) is any of its content releases; else one serial.
        serials = [r["serial"] for r in known("release")
                   if r.get("game") == args[1] and r.get("role") == "content"] or [args[1]]
        for path in (p for p in args[2:] if is_image(p)):
            title_id = xbox_title_id(path)
            if title_id in serials:
                print(path)
                return
            if title_id:
                continue
            # One unreadable image (another layout, a damaged dump) must not
            # hide the rest.
            try:
                with Image(path) as img:
                    rel = release(img.boot_name())
            except (OSError, ValueError) as error:
                print(f"skipping {error}", file=sys.stderr)
                continue
            if rel and rel["serial"] in serials:
                print(path)
                return
        sys.exit(f"no {args[1]} image among the given paths")
    elif args[:1] == ["boot-elf"] and len(args) == 3:
        boot_elf(args[1], args[2])
    else:
        sys.exit(__doc__)


def boot_elf(image, out):
    if is_image(image):
        with Image(image) as img:
            name = img.boot_name()
            data = img.read(name)
    else:
        name, data = Path(image).name, Path(image).read_bytes()
    rel = release(name)
    if not rel or rel.get("role") != "engine":
        what = f"{rel['name']} v{rel['version']}" if rel else name
        sys.exit(f"{image}: {what} is not the engine disc (Guitar Hero II (USA))")
    sha1, entry = identify_bytes(data)
    if not entry:
        sys.exit(f"{image}: {name} has SHA-1 {sha1}, not the known retail executable; "
                 "the dump may be damaged or modified")
    Path(out).write_bytes(data)
    print(f"{rel['name']} v{rel['version']}: {name}")


if __name__ == "__main__":
    main()
