"""The GS's local memory: where a pixel of each format sits, and textures read out of it.

A buffer starts at a block (256 bytes) and is laid out in pages of 32 blocks. Within a page the blocks, and
within a block the pixels, are in the orders below (GS User's Manual, "Details of GS Local Memory"), which
differ by pixel format. tools/gsdump.py keeps one of these up to date with a dump's transfers.
"""
import struct

SIZE = 0x400000

# The order of a page's blocks: 32-bit and 8-bit pages are 8 blocks by 4, 16-bit and 4-bit ones 4 by 8.
# Then the order of a block's pixels, a row of the block to a row here.
BLOCK_32 = (
    (0, 1, 4, 5, 16, 17, 20, 21),
    (2, 3, 6, 7, 18, 19, 22, 23),
    (8, 9, 12, 13, 24, 25, 28, 29),
    (10, 11, 14, 15, 26, 27, 30, 31),
)
BLOCK_16 = (
    (0, 2, 8, 10),
    (1, 3, 9, 11),
    (4, 6, 12, 14),
    (5, 7, 13, 15),
    (16, 18, 24, 26),
    (17, 19, 25, 27),
    (20, 22, 28, 30),
    (21, 23, 29, 31),
)
BLOCK_16S = (
    (0, 2, 16, 18),
    (1, 3, 17, 19),
    (8, 10, 24, 26),
    (9, 11, 25, 27),
    (4, 6, 20, 22),
    (5, 7, 21, 23),
    (12, 14, 28, 30),
    (13, 15, 29, 31),
)
COLUMN_32 = (
    (0, 1, 4, 5, 8, 9, 12, 13),
    (2, 3, 6, 7, 10, 11, 14, 15),
    (16, 17, 20, 21, 24, 25, 28, 29),
    (18, 19, 22, 23, 26, 27, 30, 31),
    (32, 33, 36, 37, 40, 41, 44, 45),
    (34, 35, 38, 39, 42, 43, 46, 47),
    (48, 49, 52, 53, 56, 57, 60, 61),
    (50, 51, 54, 55, 58, 59, 62, 63),
)
COLUMN_16 = (
    (0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27),
    (4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31),
    (32, 34, 40, 42, 48, 50, 56, 58, 33, 35, 41, 43, 49, 51, 57, 59),
    (36, 38, 44, 46, 52, 54, 60, 62, 37, 39, 45, 47, 53, 55, 61, 63),
    (64, 66, 72, 74, 80, 82, 88, 90, 65, 67, 73, 75, 81, 83, 89, 91),
    (68, 70, 76, 78, 84, 86, 92, 94, 69, 71, 77, 79, 85, 87, 93, 95),
    (96, 98, 104, 106, 112, 114, 120, 122, 97, 99, 105, 107, 113, 115, 121, 123),
    (100, 102, 108, 110, 116, 118, 124, 126, 101, 103, 109, 111, 117, 119, 125, 127),
)
COLUMN_8 = (
    (0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54),
    (8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62),
    (33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23),
    (41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31),
    (96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86),
    (104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94),
    (65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119),
    (73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127),
    (128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182),
    (136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190),
    (161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151),
    (169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159),
    (224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214),
    (232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222),
    (193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247),
    (201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255),
)
COLUMN_4 = (
    (0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46,
     70, 78, 102, 110),
    (16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30,
     54, 62, 86, 94, 118, 126),
    (65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103,
     111, 7, 15, 39, 47),
    (81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95,
     119, 127, 23, 31, 55, 63),
    (192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140,
     164, 172, 198, 206, 230, 238, 134, 142, 166, 174),
    (208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156,
     180, 188, 214, 222, 246, 254, 150, 158, 182, 190),
    (129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205,
     229, 237, 135, 143, 167, 175, 199, 207, 231, 239),
    (145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221,
     245, 253, 151, 159, 183, 191, 215, 223, 247, 255),
    (256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362, 260, 268, 292, 300, 324, 332,
     356, 364, 262, 270, 294, 302, 326, 334, 358, 366),
    (272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378, 276, 284, 308, 316, 340, 348,
     372, 380, 278, 286, 310, 318, 342, 350, 374, 382),
    (321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299, 325, 333, 357, 365, 261, 269,
     293, 301, 327, 335, 359, 367, 263, 271, 295, 303),
    (337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315, 341, 349, 373, 381, 277, 285,
     309, 317, 343, 351, 375, 383, 279, 287, 311, 319),
    (448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426, 452, 460, 484, 492, 388, 396,
     420, 428, 454, 462, 486, 494, 390, 398, 422, 430),
    (464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442, 468, 476, 500, 508, 404, 412,
     436, 444, 470, 478, 502, 510, 406, 414, 438, 446),
    (385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491, 389, 397, 421, 429, 453, 461,
     485, 493, 391, 399, 423, 431, 455, 463, 487, 495),
    (401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507, 405, 413, 437, 445, 469, 477,
     501, 509, 407, 415, 439, 447, 471, 479, 503, 511),
)

PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH = 0, 1, 2, 10, 19, 20, 27, 36, 44
PSMZ32, PSMZ24, PSMZ16, PSMZ16S = 48, 49, 50, 58
# Bits a pixel takes in a transfer.
BITS = {PSMCT32: 32, PSMCT24: 24, PSMCT16: 16, PSMCT16S: 16, PSMT8: 8, PSMT4: 4, PSMT8H: 8, PSMT4HL: 4, PSMT4HH: 4,
        PSMZ32: 32, PSMZ24: 24, PSMZ16: 16, PSMZ16S: 16}
# Where the formats that are part of a 32-bit pixel start in it.
SHIFT = {PSMT8H: 24, PSMT4HL: 24, PSMT4HH: 28}


def _layout(psm):
    """A format's page size in pixels, its block size, its tables, and the bits one address step is."""
    if psm in (PSMCT32, PSMCT24, PSMT8H, PSMT4HL, PSMT4HH, PSMZ32, PSMZ24):
        return (64, 32), (8, 8), BLOCK_32, COLUMN_32, 32
    if psm in (PSMCT16, PSMZ16):
        return (64, 64), (16, 8), BLOCK_16, COLUMN_16, 16
    if psm in (PSMCT16S, PSMZ16S):
        return (64, 64), (16, 8), BLOCK_16S, COLUMN_16, 16
    if psm == PSMT8:
        return (128, 64), (16, 16), BLOCK_32, COLUMN_8, 8
    if psm == PSMT4:
        return (128, 128), (32, 16), BLOCK_16, COLUMN_4, 4
    raise ValueError(f"pixel format {psm:#x}")


_offsets = {}


def offsets(psm, bw, width, height, x0=0, y0=0):
    """Where each pixel of a rectangle sits, row by row, in bits from the buffer's first block."""
    (pw, ph), (cw, ch), blocks, columns, step = _layout(psm)
    key = (pw, ph, id(blocks), bw, width, height, x0, y0)
    found = _offsets.get(key)
    if found is None:
        # A buffer's width is in units of 64 pixels, whatever the page's.
        across = max(bw * 64 // pw, 1)
        found = []
        for y in range(y0, y0 + height):
            for x in range(x0, x0 + width):
                page = (y // ph) * across + (x // pw) % across
                block = blocks[(y % ph) // ch][(x % pw) // cw]
                found.append((page * 32 + block) * 2048 + columns[y % ch][x % cw] * step)
        _offsets[key] = found
    return found


class Memory:
    def __init__(self, data=None):
        self.data = bytearray(data) if data is not None else bytearray(SIZE)

    def read(self, psm, bp, bw, width, height, x0=0, y0=0):
        """A rectangle's pixels as the format's raw values, row by row."""
        data, base = self.data, bp * 2048 + SHIFT.get(psm, 0)
        spots = offsets(psm, bw, width, height, x0, y0)
        if BITS[psm] == 4:
            return [data[((base + offset) >> 3) % SIZE] >> ((base + offset) & 4) & 15 for offset in spots]
        if BITS[psm] == 8:
            return [data[((base + offset) >> 3) % SIZE] for offset in spots]
        size = 2 if BITS[psm] == 16 else 4
        mask = 0xFFFFFF if BITS[psm] == 24 else 0xFFFFFFFF
        out = []
        for offset in spots:
            at = ((base + offset) >> 3) % SIZE
            out.append(int.from_bytes(data[at:at + size], "little") & mask)
        return out

    def write(self, psm, bp, bw, width, height, x0, y0, values, skip=0):
        """As many of a rectangle's pixels, row by row from the one after `skip`, as there are values."""
        data, base = self.data, bp * 2048 + SHIFT.get(psm, 0)
        spots = offsets(psm, bw, width, height, x0, y0)
        if skip:
            spots = spots[skip:]
        if BITS[psm] == 4:
            for offset, value in zip(spots, values):
                at, low = ((base + offset) >> 3) % SIZE, (base + offset) & 4
                data[at] = data[at] & (0xF0 if low == 0 else 0x0F) | value << low
        elif BITS[psm] == 8:
            for offset, value in zip(spots, values):
                data[((base + offset) >> 3) % SIZE] = value
        else:
            size = BITS[psm] // 8
            for offset, value in zip(spots, values):
                at = ((base + offset) >> 3) % SIZE
                data[at:at + size] = value.to_bytes(size, "little")

    def clut(self, tex0, texa):
        """The colours a texture's indices pick, as (r, g, b, a) each: CSM1's arrangement at cbp."""
        if tex0["csm"]:
            raise ValueError("CSM2 colour table")
        # 16 colours are 8 by 2; 256 are 16 by 16 with each 32's middle two rows of 8 changed over.
        if BITS[tex0["psm"]] == 4:
            raw = self.read(tex0["cpsm"], tex0["cbp"], 1, 8, 2)
        else:
            grid = self.read(tex0["cpsm"], tex0["cbp"], 1, 16, 16)
            raw = [grid[i + 8 if i & 0x18 == 0x08 else i - 8 if i & 0x18 == 0x10 else i] for i in range(256)]
        return [colour(tex0["cpsm"], value, texa) for value in raw]

    def texture(self, tex0, texa):
        """A texture's first level as RGBA bytes, alpha as the GS has it (0x80 is 1)."""
        width, height, psm = 1 << tex0["tw"], 1 << tex0["th"], tex0["psm"]
        raw = self.read(psm, tex0["tbp"], tex0["tbw"], width, height)
        if BITS[psm] <= 8:
            table = self.clut(tex0, texa)
            pixels = [table[index] for index in raw]
        else:
            pixels = [colour(psm, value, texa) for value in raw]
        return bytes(c for pixel in pixels for c in pixel)


def colour(psm, value, texa):
    """A pixel's value as r, g, b, a. TEXA gives the alpha a format has no room for."""
    if psm == PSMCT32:
        return value & 255, value >> 8 & 255, value >> 16 & 255, value >> 24
    if psm == PSMCT24:
        return value & 255, value >> 8 & 255, value >> 16 & 255, 0 if texa["aem"] and not value else texa["ta0"]
    if psm in (PSMCT16, PSMCT16S):
        alpha = texa["ta1"] if value & 0x8000 else 0 if texa["aem"] and not value else texa["ta0"]
        return (value & 31) << 3, (value >> 5 & 31) << 3, (value >> 10 & 31) << 3, alpha
    raise ValueError(f"pixel format {psm:#x}")


def unpack(psm, data):
    """A transfer's bytes as pixel values, in the order sent."""
    bits = BITS[psm]
    if bits == 32:
        return struct.unpack(f"<{len(data) // 4}I", data[:len(data) // 4 * 4])
    if bits == 24:
        return [int.from_bytes(data[i:i + 3], "little") for i in range(0, len(data) - 2, 3)]
    if bits == 16:
        return struct.unpack(f"<{len(data) // 2}H", data[:len(data) // 2 * 2])
    if bits == 8:
        return data
    return [nibble for byte in data for nibble in (byte & 15, byte >> 4)]
