"""Bitmaps as RndBitmap reads them (formats/bitmap.h), for the tools here that
draw one for the game to load."""
import struct


def clut(index):
    """The GS's 8-bit CLUT order: entries 8-15 and 16-23 of each 32 swap."""
    bits = index & 0x18
    return index + 8 if bits == 0x08 else index - 8 if bits == 0x10 else index


def header(width, height):
    """RndBitmap rev 1, 8bpp, kRgba | kPs2Alpha (milo/layout.h), unswizzled."""
    out = bytearray(32)
    out[0] = 1
    out[1] = 8
    struct.pack_into("<I", out, 2, 3)
    struct.pack_into("<HHH", out, 7, width, height, width)
    return bytes(out)


def palette(colours):
    """256 (r, g, b, a) as the bitmap holds them, alpha 0..128, in CLUT order."""
    entries = bytearray(1024)
    for i, c in enumerate(colours):
        entries[clut(i) * 4:clut(i) * 4 + 4] = bytes(c)
    return bytes(entries)
