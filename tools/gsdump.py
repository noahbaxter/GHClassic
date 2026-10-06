#!/usr/bin/env python3
"""Read a PCSX2 GS dump (uncompressed .gs) into each frame's draws.

    tools/gsdump.py <dump.gs>              one line per draw of the last frame
    tools/gsdump.py <dump.gs> --json out   that frame's draws, with every vertex

A draw is a run of vertices kicked under one set of registers: its PRIM, the
context registers PRIM selects (FRAME, ZBUF, TEX0, TEX1, CLAMP, ALPHA, TEST,
FBA, SCISSOR, XYOFFSET), the shared ones (TEXA, FOGCOL, COLCLAMP, PABE, DTHE)
and its vertices as the GS takes them: x and y in pixels after XYOFFSET, z,
rgba, q, and s, t or u, v. A strip's or fan's vertices are kept in order, with
`skip` on those ADC or XYZ3 hold back from drawing.

read(path, textures=True) also gives each textured draw of the last frame its
texture as `texels`: the first level's RGBA out of the GS's memory as it stood
at the draw, which the dump's transfers are played into. None for a texture
the frame drew into or copied the picture into, which is not in the dump.

The dump opens with the GS's whole state (pcsx2/GS/GSState.cpp, Freeze), so a
register no packet writes still has its value.
"""
import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gsmem  # noqa: E402

CONTEXT = ("XYOFFSET", "TEX0", "TEX1", "CLAMP", "MIPTBP1", "MIPTBP2", "SCISSOR", "ALPHA", "TEST", "FBA", "FRAME",
           "ZBUF")
SHARED = ("PRIM", "PRMODECONT", "TEXCLUT", "SCANMSK", "TEXA", "FOGCOL", "DIMX", "DTHE", "COLCLAMP", "PABE",
          "BITBLTBUF", "TRXDIR", "TRXPOS", "TRXREG")
# A+D addresses.
ADDRESS = {
    0x00: "PRIM", 0x01: "RGBAQ", 0x02: "ST", 0x03: "UV", 0x04: "XYZF2", 0x05: "XYZ2", 0x06: "TEX0_1",
    0x07: "TEX0_2", 0x08: "CLAMP_1", 0x09: "CLAMP_2", 0x0A: "FOG", 0x0C: "XYZF3", 0x0D: "XYZ3", 0x14: "TEX1_1",
    0x15: "TEX1_2", 0x16: "TEX2_1", 0x17: "TEX2_2", 0x18: "XYOFFSET_1", 0x19: "XYOFFSET_2", 0x1A: "PRMODECONT",
    0x1B: "PRMODE", 0x1C: "TEXCLUT", 0x22: "SCANMSK", 0x34: "MIPTBP1_1", 0x35: "MIPTBP1_2", 0x36: "MIPTBP2_1",
    0x37: "MIPTBP2_2", 0x3B: "TEXA", 0x3D: "FOGCOL", 0x3F: "TEXFLUSH", 0x40: "SCISSOR_1", 0x41: "SCISSOR_2",
    0x42: "ALPHA_1", 0x43: "ALPHA_2", 0x44: "DIMX", 0x45: "DTHE", 0x46: "COLCLAMP", 0x47: "TEST_1",
    0x48: "TEST_2", 0x49: "PABE", 0x4A: "FBA_1", 0x4B: "FBA_2", 0x4C: "FRAME_1", 0x4D: "FRAME_2", 0x4E: "ZBUF_1",
    0x4F: "ZBUF_2", 0x50: "BITBLTBUF", 0x51: "TRXPOS", 0x52: "TRXREG", 0x53: "TRXDIR", 0x54: "HWREG",
    0x60: "SIGNAL", 0x61: "FINISH", 0x62: "LABEL",
}
PRIMS = ("point", "line", "linestrip", "tri", "tristrip", "trifan", "sprite", "invalid")


def bits(value, at, count):
    return value >> at & ((1 << count) - 1)


def fields(name, v):
    """A register's value as named fields."""
    if name == "PRIM":
        return {"type": PRIMS[bits(v, 0, 3)], "iip": bits(v, 3, 1), "tme": bits(v, 4, 1), "fge": bits(v, 5, 1),
                "abe": bits(v, 6, 1), "aa1": bits(v, 7, 1), "fst": bits(v, 8, 1), "ctxt": bits(v, 9, 1),
                "fix": bits(v, 10, 1)}
    if name == "TEX0":
        return {"tbp": bits(v, 0, 14), "tbw": bits(v, 14, 6), "psm": bits(v, 20, 6), "tw": bits(v, 26, 4),
                "th": bits(v, 30, 4), "tcc": bits(v, 34, 1), "tfx": bits(v, 35, 2), "cbp": bits(v, 37, 14),
                "cpsm": bits(v, 51, 4), "csm": bits(v, 55, 1), "csa": bits(v, 56, 5)}
    if name == "TEX1":
        return {"lcm": bits(v, 0, 1), "mxl": bits(v, 2, 3), "mmag": bits(v, 5, 1), "mmin": bits(v, 6, 3),
                "mtba": bits(v, 9, 1), "l": bits(v, 19, 2), "k": bits(v, 32, 12)}
    if name == "CLAMP":
        return {"wms": bits(v, 0, 2), "wmt": bits(v, 2, 2), "minu": bits(v, 4, 10), "maxu": bits(v, 14, 10),
                "minv": bits(v, 24, 10), "maxv": bits(v, 34, 10)}
    if name == "ALPHA":
        return {"a": bits(v, 0, 2), "b": bits(v, 2, 2), "c": bits(v, 4, 2), "d": bits(v, 6, 2),
                "fix": bits(v, 32, 8)}
    if name == "TEST":
        return {"ate": bits(v, 0, 1), "atst": bits(v, 1, 3), "aref": bits(v, 4, 8), "afail": bits(v, 12, 2),
                "date": bits(v, 14, 1), "datm": bits(v, 15, 1), "zte": bits(v, 16, 1), "ztst": bits(v, 17, 2)}
    if name == "FRAME":
        return {"fbp": bits(v, 0, 9), "fbw": bits(v, 16, 6), "psm": bits(v, 24, 6), "fbmsk": bits(v, 32, 32)}
    if name == "ZBUF":
        return {"zbp": bits(v, 0, 9), "psm": bits(v, 24, 4), "zmsk": bits(v, 32, 1)}
    if name == "SCISSOR":
        return {"x0": bits(v, 0, 11), "x1": bits(v, 16, 11), "y0": bits(v, 32, 11), "y1": bits(v, 48, 11)}
    if name == "XYOFFSET":
        return {"ofx": bits(v, 0, 16), "ofy": bits(v, 32, 16)}
    if name == "TEXA":
        return {"ta0": bits(v, 0, 8), "aem": bits(v, 15, 1), "ta1": bits(v, 32, 8)}
    return {"value": v}


class GS:
    """The registers and the draws that come of writing them."""

    def __init__(self, state):
        # The GS's memory ends the state but for the four paths' tags (16 bytes and 4 each) and Q
        # (GSState::Freeze).
        end = len(state) - 84
        self.memory = gsmem.Memory(state[end - gsmem.SIZE:end])
        self.transfer = None  # a host to local transfer under way
        self.written = {}     # block pointer: how many transfers have begun there
        self.drawn = set()    # block pointers of the buffers drawn into
        self.decode = False
        self.textures = {}
        self.shared = {}
        self.context = [{}, {}]
        at = 4
        for name in SHARED:
            self.shared[name] = struct.unpack_from("<Q", state, at)[0]
            at += 8
        at += 8  # TRXREG again
        for ctxt in self.context:
            for name in CONTEXT:
                ctxt[name] = struct.unpack_from("<Q", state, at)[0]
                at += 8
        rgbaq, st, uv, fog = struct.unpack_from("<QQQQ", state, at)
        self.rgba = [bits(rgbaq, 0, 8), bits(rgbaq, 8, 8), bits(rgbaq, 16, 8), bits(rgbaq, 24, 8)]
        self.q = struct.unpack("<f", struct.pack("<I", bits(rgbaq, 32, 32)))[0]
        self.st = list(struct.unpack("<ff", struct.pack("<Q", st)))
        self.uv = [bits(uv, 0, 14), bits(uv, 16, 14)]
        self.fog = bits(fog, 56, 8)
        self.draws = []
        self.key = None
        self.uploads = 0

    def write(self, name, value):
        if name in ("XYZ2", "XYZF2", "XYZ3", "XYZF3"):
            return self.vertex(name, value)
        if name == "RGBAQ":
            self.rgba = [bits(value, 0, 8), bits(value, 8, 8), bits(value, 16, 8), bits(value, 24, 8)]
            self.q = struct.unpack("<f", struct.pack("<I", bits(value, 32, 32)))[0]
        elif name == "ST":
            self.st = list(struct.unpack("<ff", struct.pack("<Q", value)))
        elif name == "UV":
            self.uv = [bits(value, 0, 14), bits(value, 16, 14)]
        elif name == "FOG":
            self.fog = bits(value, 56, 8)
        elif name == "PRIM":
            self.shared["PRIM"] = value
            self.key = None  # a PRIM write restarts the primitive
        elif name == "PRMODE":
            if not self.shared["PRMODECONT"] & 1:
                self.shared["PRIM"] = self.shared["PRIM"] & 7 | value & ~7
        elif name == "TRXDIR":
            self.uploads += 1
            self.begin_transfer(value & 3)
        elif name[-2:] in ("_1", "_2") and name[:-2] in CONTEXT:
            self.context[int(name[-1]) - 1][name[:-2]] = value
        elif name in self.shared:
            self.shared[name] = value

    def begin_transfer(self, direction):
        blt, pos, reg = self.shared["BITBLTBUF"], self.shared["TRXPOS"], self.shared["TRXREG"]
        to = {"psm": bits(blt, 56, 6), "bp": bits(blt, 32, 14), "bw": bits(blt, 48, 6), "x": bits(pos, 32, 11),
              "y": bits(pos, 48, 11), "width": bits(reg, 0, 12), "height": bits(reg, 32, 12)}
        self.transfer = None
        if direction == 0:
            self.transfer = {**to, "sent": 0, "left": b""}
        elif direction == 2:
            values = self.memory.read(bits(blt, 24, 6), bits(blt, 0, 14), bits(blt, 16, 6), to["width"],
                                      to["height"], bits(pos, 0, 11), bits(pos, 16, 11))
            self.memory.write(to["psm"], to["bp"], to["bw"], to["width"], to["height"], to["x"], to["y"], values)
            self.drawn.add(to["bp"])  # a copy of what was drawn
        if direction in (0, 2):
            self.written[to["bp"]] = self.written.get(to["bp"], 0) + 1

    def image(self, data):
        """Pixels of the host to local transfer under way."""
        t = self.transfer
        if t is None:
            return
        data = t["left"] + data
        whole = len(data) * 8 // gsmem.BITS[t["psm"]] * gsmem.BITS[t["psm"]] // 8
        values = gsmem.unpack(t["psm"], data[:whole])
        t["left"] = data[whole:]
        self.memory.write(t["psm"], t["bp"], t["bw"], t["width"], t["height"], t["x"], t["y"], values, t["sent"])
        t["sent"] += len(values)

    def texels(self, tex0, texa):
        """The texture as it stands, read once for each state of its memory."""
        if tex0["tbp"] in self.drawn:
            return None
        key = (tuple(tex0[k] for k in ("tbp", "tbw", "psm", "tw", "th", "cbp", "cpsm", "csm")), tuple(texa.values()),
               self.written.get(tex0["tbp"]), self.written.get(tex0["cbp"]))
        if key not in self.textures:
            self.textures[key] = self.memory.texture(tex0, texa)
        return self.textures[key]

    def vertex(self, name, value):
        prim = self.shared["PRIM"]
        ctxt = self.context[bits(prim, 9, 1)]
        key = (prim, tuple(ctxt[n] for n in CONTEXT),
               tuple(self.shared[n] for n in ("TEXA", "FOGCOL", "COLCLAMP", "PABE", "DTHE", "TEXCLUT")))
        if key != self.key:
            self.key = key
            self.draws.append({
                "prim": fields("PRIM", prim),
                **{n: fields(n, ctxt[n]) for n in CONTEXT if not n.startswith("MIPTBP")},
                **{n: fields(n, self.shared[n]) for n in ("TEXA", "FOGCOL", "COLCLAMP", "PABE", "DTHE")},
                "verts": [],
            })
            # FRAME's pointer is in pages of 32 blocks.
            self.drawn.add(bits(ctxt["FRAME"], 0, 9) * 32)
            if self.decode and bits(prim, 4, 1):
                self.draws[-1]["texels"] = self.texels(self.draws[-1]["TEX0"], self.draws[-1]["TEXA"])
        f = name.startswith("XYZF")
        ofx, ofy = bits(ctxt["XYOFFSET"], 0, 16), bits(ctxt["XYOFFSET"], 32, 16)
        vert = {
            "x": (bits(value, 0, 16) - ofx) / 16.0,
            "y": (bits(value, 16, 16) - ofy) / 16.0,
            "z": bits(value, 32, 24) if f else bits(value, 32, 32),
            "rgba": list(self.rgba),
            "q": self.q,
        }
        if bits(prim, 8, 1):
            vert["uv"] = [self.uv[0] / 16.0, self.uv[1] / 16.0]
        else:
            vert["st"] = list(self.st)
        if f:
            vert["fog"] = bits(value, 56, 8)
        if name.endswith("3"):
            vert["skip"] = True
        self.draws[-1]["verts"].append(vert)


class GifPath:
    """One GIF path's place in its packet stream, across transfers."""

    def __init__(self):
        self.words = 0  # 64-bit words of the reglist so far
        self.nloop = 0
        self.flg = 0
        self.regs = []
        self.reg = 0
        self.image = 0  # bytes of image data still to pass

    def feed(self, gs, data):
        at = 0
        while at + 8 <= len(data):
            if self.image:
                step = min(self.image, len(data) - at)
                gs.image(data[at:at + step])
                self.image -= step
                at += step
                continue
            if self.nloop == 0:
                if at + 16 > len(data):
                    break
                lo, hi = struct.unpack_from("<QQ", data, at)
                at += 16
                self.nloop = bits(lo, 0, 15)
                self.flg = bits(lo, 58, 2)
                nreg = bits(lo, 60, 4) or 16
                self.regs = [bits(hi, 4 * n, 4) for n in range(nreg)]
                self.reg = 0
                self.words = 0
                if self.flg == 0 and bits(lo, 46, 1) and self.nloop:
                    gs.write("PRIM", bits(lo, 47, 11))
                if self.flg >= 2:
                    self.image = self.nloop * 16
                    self.nloop = 0
                continue
            if self.flg == 0:
                if at + 16 > len(data):
                    break
                lo, hi = struct.unpack_from("<QQ", data, at)
                at += 16
                self.packed(gs, self.regs[self.reg], lo, hi)
            else:
                value = struct.unpack_from("<Q", data, at)[0]
                at += 8
                self.words += 1
                name = ADDRESS.get(self.regs[self.reg])
                if name and self.regs[self.reg] != 0xE:
                    gs.write(name, value)
            self.reg += 1
            if self.reg == len(self.regs):
                self.reg = 0
                self.nloop -= 1
                if self.nloop == 0 and self.flg == 1 and self.words & 1:
                    at += 8  # a reglist pads to 128 bits

    @staticmethod
    def packed(gs, reg, lo, hi):
        if reg == 0x0:
            gs.write("PRIM", bits(lo, 0, 11))
        elif reg == 0x1:
            gs.rgba = [bits(lo, 0, 8), bits(lo, 32, 8), bits(hi, 0, 8), bits(hi, 32, 8)]
        elif reg == 0x2:
            gs.st = list(struct.unpack("<ff", struct.pack("<Q", lo)))
            gs.q = struct.unpack("<f", struct.pack("<I", bits(hi, 0, 32)))[0]
        elif reg == 0x3:
            gs.uv = [bits(lo, 0, 14), bits(lo, 32, 14)]
        elif reg == 0x4:
            value = bits(lo, 0, 16) | bits(lo, 32, 16) << 16 | bits(hi, 4, 24) << 32 | bits(hi, 36, 8) << 56
            gs.write("XYZF3" if bits(hi, 47, 1) else "XYZF2", value)
        elif reg == 0x5:
            value = bits(lo, 0, 16) | bits(lo, 32, 16) << 16 | bits(hi, 0, 32) << 32
            gs.write("XYZ3" if bits(hi, 47, 1) else "XYZ2", value)
        elif reg == 0xA:
            gs.fog = bits(hi, 36, 8)
        elif reg == 0xE:
            name = ADDRESS.get(bits(hi, 0, 8))
            if name:
                gs.write(name, lo)
        elif reg != 0xF:
            name = ADDRESS.get(reg)
            if name:
                gs.write(name, lo)


def packets(data, at):
    """Where each frame's vsync packet is."""
    out = []
    while at < len(data):
        kind = data[at]
        if kind == 0:
            at += 6 + struct.unpack_from("<I", data, at + 2)[0]
        elif kind == 1:
            out.append(at)
            at += 2
        elif kind == 2:
            at += 5
        elif kind == 3:
            at += 1 + 0x2000
        else:
            raise SystemExit(f"unknown packet {kind} at {at:#x}")
    return out


def read(path, textures=False):
    """The dump's frames, each a list of draws, and its header."""
    data = Path(path).read_bytes()
    if struct.unpack_from("<I", data, 0)[0] != 0xFFFFFFFF:
        raise SystemExit("not a new-format uncompressed GS dump")
    header_size = struct.unpack_from("<I", data, 4)[0]
    version, state_size, serial_at, serial_size, crc, width, height = struct.unpack_from("<7I", data, 8)
    serial = data[8 + serial_at:8 + serial_at + serial_size].decode()
    at = 8 + header_size
    gs = GS(data[at:at + state_size])
    at += state_size + 0x2000
    paths = [GifPath(), GifPath(), GifPath()]
    frames = []
    last = len(packets(data, at)) - 1 if textures else None
    while at < len(data):
        gs.decode = len(frames) == last
        kind = data[at]
        at += 1
        if kind == 0:
            index, size = struct.unpack_from("<BI", data, at)
            at += 5
            paths[max(index - 1, 0)].feed(gs, data[at:at + size])
            at += size
        elif kind == 1:
            at += 1
            frames.append(gs.draws)
            gs.draws = []
            gs.key = None
        elif kind == 2:
            at += 4
        elif kind == 3:
            at += 0x2000
        else:
            raise SystemExit(f"unknown packet {kind} at {at - 1:#x}")
    return {"serial": serial, "crc": crc, "frames": frames}


def summary(draw):
    prim, tex, test, alpha = draw["prim"], draw["TEX0"], draw["TEST"], draw["ALPHA"]
    parts = [f"{prim['type']:9}", f"{len(draw['verts']):5}v", f"fb {draw['FRAME']['fbp']:#05x}"]
    parts.append(f"tex {tex['tbp']:#06x} {1 << tex['tw']}x{1 << tex['th']} psm {tex['psm']:#x} tfx {tex['tfx']}"
                 if prim["tme"] else "untextured")
    parts.append(f"blend {alpha['a']}{alpha['b']}{alpha['c']}{alpha['d']} fix {alpha['fix']:#x}"
                 if prim["abe"] else "opaque")
    parts.append(f"ztst {test['ztst'] if test['zte'] else '-'} zmsk {draw['ZBUF']['zmsk']}")
    if test["ate"]:
        parts.append(f"atst {test['atst']} aref {test['aref']:#x}")
    if test["date"]:
        parts.append(f"date {test['datm']}")
    if draw["FBA"]["value"]:
        parts.append("fba")
    xs = [v["x"] for v in draw["verts"]]
    ys = [v["y"] for v in draw["verts"]]
    parts.append(f"x {min(xs):.1f}..{max(xs):.1f} y {min(ys):.1f}..{max(ys):.1f}")
    return "  ".join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dump")
    parser.add_argument("--frame", type=int, default=-1)
    parser.add_argument("--json")
    args = parser.parse_args()
    dump = read(args.dump)
    draws = dump["frames"][args.frame]
    print(f"{dump['serial']} crc {dump['crc']:08X}: {len(dump['frames'])} frames, "
          f"{[len(f) for f in dump['frames']]} draws", file=sys.stderr)
    if args.json:
        Path(args.json).write_text(json.dumps(draws), newline="\n")
    else:
        for n, draw in enumerate(draws):
            print(f"{n:4} {summary(draw)}")


if __name__ == "__main__":
    main()
