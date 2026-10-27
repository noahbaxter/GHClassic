"""Build assets/controllers_poster.bin, the Controllers screen's poster
(ui/controllers.cpp): Calibrate Lag's with its art painted out, near-black
inside the paper border, the paper's own wrinkles kept faintly for grain.

    tools/controllers_poster.py <syncoff_backdrop.png> <syncoff_backdrop_mask.png>

Both are the 360's ui/gen/lag.milo textures. The poster is a 360 bitmap,
DXT5 at 512, by tools/campaign_art.py's encoder.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

import campaign_art

OUT = Path(__file__).resolve().parents[1] / "assets" / "controllers_poster.bin"


def main():
    src, mask = sys.argv[1:3]
    im = np.asarray(Image.open(src).convert("RGBA")).astype(np.float32)
    wr = np.asarray(Image.open(mask).convert("RGBA")).astype(np.float32)
    x0, y0, x1, y1 = 46, 54, 978, 970
    inner = np.zeros(im.shape[:2], bool)
    inner[y0:y1, x0:x1] = True
    # The wrinkles: the mask's alpha, without the hair over its top right.
    grain = wr[:, :, 3] / 255.0
    grain[:310, 640:] = 0.0
    base = np.array([17.0, 17.0, 19.0])
    fill = base[None, None, :] + grain[:, :, None] * 60.0
    out = im.copy()
    out[inner, :3] = fill[inner]
    out[inner, 3] = 255.0
    img = Image.fromarray(out.clip(0, 255).astype(np.uint8), "RGBA").resize((512, 512), Image.LANCZOS)
    OUT.write_bytes(campaign_art.bitmap(img))


if __name__ == "__main__":
    main()
