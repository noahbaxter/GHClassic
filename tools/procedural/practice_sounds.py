#!/usr/bin/env python3
"""Pack practice's metronome and note click, as content/practice_sounds.cpp
adds them to its practice bank. Noah's own recordings.

    tools/procedural/practice_sounds.py [metro_bar=<wav>] [metro_beat=<wav>] [note_click=<wav>]
        [--out assets/practice_sounds/practice_sounds.bin]

Each is assets/practice_sounds/<name>.wav unless given: to swap one, put a
new WAV there under its name and run this again (tools/ghc.py procedural
practice_sounds), then build. Each is made mono at 22050 Hz, GH2's practice
samples' rate, raised by PITCH's semitones, its silent tail cut, its last
5 ms faded and its peak brought to PEAK. practice_sounds.bin is a count, then each: a byte of name length,
the name, a frame count, and that many 16-bit samples, little-endian.
"""
import argparse
import struct
import wave
from math import gcd
from pathlib import Path

import numpy as np
from scipy.signal import resample_poly

ROOT = Path(__file__).resolve().parents[2]
RATE = 22050
PIECES = ("metro_bar", "metro_beat", "note_click")
PEAK = 0.9  # of full scale, each sound's loudest
# Semitones each is raised, by playing it faster: the note click above the
# guitars it is heard over.
PITCH = {"note_click": 3}


def load(path):
    with wave.open(str(path)) as w:
        frames, width, channels, rate = w.readframes(w.getnframes()), w.getsampwidth(), w.getnchannels(), w.getframerate()
    if width == 3:
        b = np.frombuffer(frames, np.uint8).reshape(-1, 3).astype(np.int32)
        x = (b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16))
        x = np.where(x & 0x800000, x - 0x1000000, x) / 8388608.0
    else:
        x = np.frombuffer(frames, {1: np.uint8, 2: np.int16, 4: np.int32}[width]).astype(np.float64)
        x = (x - 128) / 128 if width == 1 else x / float(2 ** (8 * width - 1))
    x = x.reshape(-1, channels).mean(axis=1)
    g = gcd(rate, RATE)
    return resample_poly(x, RATE // g, rate // g)


def piece(name, path):
    x = load(path)
    if PITCH.get(name):
        ratio = 2 ** (PITCH[name] / 12)
        x = np.interp(np.arange(0, len(x), ratio), np.arange(len(x)), x)
    loud = np.nonzero(np.abs(x) > np.abs(x).max() / 1000)[0]
    x = x[: loud[-1] + 1 if len(loud) else len(x)]
    fade = min(len(x), RATE * 5 // 1000)
    x[len(x) - fade:] *= np.linspace(1, 0, fade)
    # Each brought to the same peak, so one is heard as well as another
    # whatever it was recorded at.
    x *= PEAK / max(np.abs(x).max(), 1e-9)
    return np.clip(np.round(x * 32767), -32768, 32767).astype("<i2")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("pieces", nargs="*", metavar="name=wav")
    parser.add_argument("--out", default=ROOT / "assets" / "practice_sounds" / "practice_sounds.bin", type=Path)
    args = parser.parse_args()
    given = {name: ROOT / "assets" / "practice_sounds" / f"{name}.wav" for name in PIECES}
    for p in args.pieces:
        name, path = p.split("=", 1)
        if name not in PIECES:
            parser.error(f"{name} is none of {', '.join(PIECES)}")
        given[name] = Path(path)
    out = bytearray(struct.pack("<I", len(PIECES)))
    for name in PIECES:
        pcm = piece(name, given[name])
        out += struct.pack("<B", len(name)) + name.encode() + struct.pack("<I", len(pcm)) + pcm.tobytes()
        print(f"{name}: {len(pcm) / RATE * 1000:.0f} ms from {given[name]}")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(bytes(out))
    print(f"{args.out} ({len(out)} bytes)")


if __name__ == "__main__":
    main()
