"""Build assets/campaign_select.bin, what the campaign picker draws that no
disc has: a DVD case's geometry, each game's case wrap and the GH2 Demo's
amp poster (content/campaign_case.h reads it).

    tools/campaign_art.py <case.glb> <poster.png> <game>=<wrap image>...

The case is "Silent Hill 3 Ps2 Game cover" (https://skfb.ly/oR9ZT) by
eyewideasca, CC-BY 4.0: its sleeve and shell meshes, not its textures. A
wrap is a full case scan, back, spine and front left to right.

The pack: "GHCP", a version, the meshes (sleeve then shell: vert count,
each vert's position, normal and uv as 8 floats in the scene's axes, x right,
y into the screen, z up; face count, each three u16), then the pictures (a
name, a size, and a 360 bitmap: DXT1, or DXT5 for the poster, which has
alpha, with every 16-bit word swapped, as render/texture_store.cpp decodes).
The poster is the GH2 Demo's ui/gen/loading.milo_ps2 loading_amp.tex.
"""
import json
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

OUT = Path(__file__).resolve().parents[1] / "assets" / "campaign_select.bin"
WRAP_SIZE = (1280, 856)
POSTER_SIZE = (512, 512)


def glb_meshes(path):
    b = Path(path).read_bytes()
    n = struct.unpack_from("<I", b, 12)[0]
    j = json.loads(b[20:20 + n])
    blob = b[20 + n + 8:]

    def acc(i):
        a = j["accessors"][i]
        v = j["bufferViews"][a["bufferView"]]
        off = v.get("byteOffset", 0) + a.get("byteOffset", 0)
        dt = np.dtype({5126: "<f4", 5123: "<u2", 5125: "<u4"}[a["componentType"]])
        k = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
        stride = v.get("byteStride", dt.itemsize * k)
        return np.stack([np.frombuffer(blob, dt, k, off + i * stride) for i in range(a["count"])])

    out = []
    for m in j["meshes"]:
        p = m["primitives"][0]
        pos, nrm = acc(p["attributes"]["POSITION"]), acc(p["attributes"]["NORMAL"])
        uv, tri = acc(p["attributes"]["TEXCOORD_0"]), acc(p["indices"]).reshape(-1, 3)
        # glTF's y up and z out of the front, as the scene's z up and -y.
        pos = np.stack([pos[:, 0], -pos[:, 2], pos[:, 1]], 1)
        nrm = np.stack([nrm[:, 0], -nrm[:, 2], nrm[:, 1]], 1)
        out.append((pos, nrm, uv, tri))
    return out


def dxt1(img):
    """An RGB image, sides a multiple of 4, as DXT1 blocks."""
    a = np.asarray(img.convert("RGB"), np.float32)
    h, w = a.shape[:2]
    blocks = a.reshape(h // 4, 4, w // 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3)
    mean = blocks.mean(1, keepdims=True)
    d = blocks - mean
    axis = np.einsum("bij,bik->bjk", d, d)
    v = np.ones((len(blocks), 3, 1), np.float32)
    for _ in range(4):
        v = axis @ v
        v /= np.linalg.norm(v, axis=1, keepdims=True) + 1e-9
    t = (d @ v)[:, :, 0]
    lo = mean[:, 0] + v[:, :, 0] * t.min(1, keepdims=True)
    hi = mean[:, 0] + v[:, :, 0] * t.max(1, keepdims=True)

    def q565(c):
        c = c.clip(0, 255)
        r, g, b = (c[:, 0] * 31 / 255 + 0.5).astype(np.uint32), (c[:, 1] * 63 / 255 + 0.5).astype(
            np.uint32), (c[:, 2] * 31 / 255 + 0.5).astype(np.uint32)
        return (r << 11) | (g << 5) | b

    def rgb(q):
        return np.stack([(q >> 11 & 31) * 255 / 31, (q >> 5 & 63) * 255 / 63, (q & 31) * 255 / 31], 1)

    c0, c1 = q565(hi), q565(lo)
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    p0, p1 = rgb(c0), rgb(c1)
    palette = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], 1)
    dist = ((blocks[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(-1)
    idx = dist.argmin(2).astype(np.uint32)
    idx = np.where((c0 == c1)[:, None], 0, idx)
    bits = (idx << (2 * np.arange(16, dtype=np.uint32))).sum(1).astype(np.uint32)
    out = np.zeros((len(blocks), 4), np.uint16)
    out[:, 0], out[:, 1] = c0, c1
    out[:, 2], out[:, 3] = bits & 0xFFFF, bits >> 16
    # The 360's byte order: each 16-bit word's two bytes swapped.
    return out.astype(">u2").tobytes()


def dxt5(img):
    """An RGBA image, sides a multiple of 4, as DXT5 blocks: each block's
    alpha (its lightest and darkest, then six between) before its colour."""
    a = np.asarray(img.convert("RGBA"))[:, :, 3].astype(np.float32)
    h, w = a.shape
    blocks = a.reshape(h // 4, 4, w // 4, 4).transpose(0, 2, 1, 3).reshape(-1, 16)
    a0, a1 = blocks.max(1), blocks.min(1)
    palette = np.stack([a0, a1] + [((7 - i) * a0 + i * a1) / 7 for i in range(1, 7)], 1)
    idx = np.abs(blocks[:, :, None] - palette[:, None, :]).argmin(2).astype(np.uint64)
    idx = np.where((a0 == a1)[:, None], 0, idx)
    bits = (idx << (3 * np.arange(16, dtype=np.uint64))).sum(1)
    alpha = np.zeros((len(blocks), 4), np.uint16)
    alpha[:, 0] = a0.astype(np.uint16) | (a1.astype(np.uint16) << 8)
    for i in range(3):
        alpha[:, i + 1] = (bits >> np.uint64(16 * i)) & np.uint64(0xFFFF)
    colour = np.frombuffer(dxt1(img), ">u2").reshape(-1, 4)
    return np.concatenate([alpha, colour], 1).astype(">u2").tobytes()


def bitmap(img):
    w, h = img.size
    if img.mode == "RGBA":
        header = struct.pack("<BBIBHHH", 1, 8, 24, 0, w, h, w) + b"\0" * 19
        return header + dxt5(img)
    header = struct.pack("<BBIBHHH", 1, 4, 8, 0, w, h, w // 2) + b"\0" * 19
    return header + dxt1(img)


def put_str(out, s):
    b = s.encode()
    out += struct.pack("<I", len(b)) + b


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    out = bytearray(b"GHCP" + struct.pack("<I", 1))
    meshes = glb_meshes(argv[0])
    out += struct.pack("<I", len(meshes))
    for pos, nrm, uv, tri in meshes:
        out += struct.pack("<I", len(pos))
        out += np.concatenate([pos, nrm, uv], 1).astype("<f4").tobytes()
        out += struct.pack("<I", len(tri)) + tri.astype("<u2").tobytes()
    # The poster keeps its alpha: its torn edges and corners show the wall.
    pictures = [("poster", Image.open(argv[1]).convert("RGBA").resize(POSTER_SIZE, Image.LANCZOS))]
    for arg in argv[2:]:
        game, path = arg.split("=", 1)
        pictures.append((game, Image.open(path).convert("RGB").resize(WRAP_SIZE, Image.LANCZOS)))
    out += struct.pack("<I", len(pictures))
    for name, img in pictures:
        b = bitmap(img)
        put_str(out, name)
        out += struct.pack("<I", len(b)) + b
    OUT.write_bytes(out)
    print(f"{OUT}: {len(out)} bytes, {len(meshes)} meshes, {len(pictures)} pictures")


if __name__ == "__main__":
    main(sys.argv[1:])
