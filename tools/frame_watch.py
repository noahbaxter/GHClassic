#!/usr/bin/env python3
"""Follow a running game's frametimes.csv (settings.ini [dev] frame_log = true).

    tools/frame_watch.py <frametimes.csv> [--slow MS]

Every second: frames presented and game frames, the worst frame, frames over
--slow (20 ms) with their likely cause, repeats, and the GPU's time a frame.
When the game stops writing (or on Ctrl+C), a summary: slow frames by cause,
the gaps between them (regular means a clock, scattered means loading), a
histogram, and the GPU's time by pass.
"""
import csv
import sys
import time
from collections import Counter
from pathlib import Path

SLOW_MS = 20.0
IDLE_EXIT_S = 5.0


def cause(row):
    """The likeliest reason a frame ran long, from what it recorded."""
    if int(row["mesh_new"]) + int(row["tex_new"]) >= 4 or int(row["upload_kb"]) >= 1024:
        return "uploads"
    if float(row["acq_ms"]) >= 8.0:
        return "GPU / present queue"
    if float(row["rec_ms"]) >= 8.0:
        return "draw recording (CPU)"
    if float(row["wait_ms"]) >= 8.0:
        return "waiting for the game"
    return "host (OS / other)"


def follow(path):
    """Rows as they are appended, until the file goes quiet."""
    while not path.exists():
        time.sleep(0.5)
    with path.open(newline="") as f:
        header = f.readline().strip().split(",")
        idle = 0.0
        partial = ""
        while True:
            line = f.readline()
            if not line:
                if idle >= IDLE_EXIT_S:
                    return
                time.sleep(0.25)
                idle += 0.25
                yield None
                continue
            idle = 0.0
            partial += line
            if not partial.endswith("\n"):
                continue
            values = next(csv.reader([partial.strip()]))
            partial = ""
            if len(values) == len(header):
                yield dict(zip(header, values))


def median(values):
    ordered = sorted(values)
    return ordered[len(ordered) // 2]


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    slow = SLOW_MS
    if "--slow" in args:
        i = args.index("--slow")
        slow = float(args[i + 1])
        del args[i:i + 2]
    path = Path(args[0])

    all_rows, slow_rows = [], []
    second, last_print = [], time.monotonic()
    try:
        for row in follow(path):
            if row is not None:
                all_rows.append(row)
                second.append(row)
                if float(row["dt_ms"]) > slow:
                    slow_rows.append(row)
                    print(f"  ! {float(row['t_ms']) / 1000:8.2f}s  {float(row['dt_ms']):6.1f} ms  {cause(row)}"
                          f"  (+{row['mesh_new']} meshes, +{row['tex_new']} tex, {row['upload_kb']} KB,"
                          f" acq {float(row['acq_ms']):.1f}, rec {float(row['rec_ms']):.1f},"
                          f" gpu {float(row['gpu_ms']):.1f})")
            now = time.monotonic()
            if second and now - last_print >= 1.0:
                dts = [float(r["dt_ms"]) for r in second]
                span = sum(dts) / 1000 or 1.0
                game = sum(int(r["game"]) for r in second)
                repeats = sum(1 for r in second if r["new"] == "0")
                last = second[-1]
                print(f"{float(last['t_ms']) / 1000:8.1f}s  {len(second) / span:5.1f} fps  game {game / span:5.1f}"
                      f"  worst {max(dts):5.1f} ms  repeats {repeats}"
                      f"  gpu {max(float(r['gpu_ms']) for r in second):5.1f} ms")
                second, last_print = [], now
    except KeyboardInterrupt:
        pass

    if not all_rows:
        print("no frames logged")
        return
    dts = [float(r["dt_ms"]) for r in all_rows]
    total_s = float(all_rows[-1]["t_ms"]) / 1000
    print(f"\n== {len(all_rows)} frames over {total_s:.1f}s, {len(slow_rows)} over {slow:.0f} ms ==")
    for name, count in Counter(cause(r) for r in slow_rows).most_common():
        print(f"  {count:5d}  {name}")
    if len(slow_rows) >= 3:
        gaps = [float(b["t_ms"]) - float(a["t_ms"]) for a, b in zip(slow_rows, slow_rows[1:])]
        mean = sum(gaps) / len(gaps)
        spread = (sum((g - mean) ** 2 for g in gaps) / len(gaps)) ** 0.5
        kind = "regular (a clock drifting)" if spread < mean * 0.25 else "scattered (loads, camera cuts)"
        print(f"  gaps between them: mean {mean / 1000:.2f}s, {kind}")
    buckets = [(0, 17.5), (17.5, 20), (20, 34), (34, 50), (50, 100), (100, 1e9)]
    print("  frame time histogram:")
    for lo, hi in buckets:
        n = sum(1 for d in dts if lo <= d < hi)
        label = f"{lo:g}-{hi:g} ms" if hi < 1e9 else f"{lo:g}+ ms"
        print(f"    {label:>12}  {n:6d}  {'#' * min(60, n * 60 // max(1, len(dts)))}")
    timed = [r for r in all_rows if float(r["gpu_ms"]) > 0.0]
    if timed:
        print("  GPU ms a frame, median / worst:")
        for key in (k for k in timed[0] if k.startswith("gpu_")):
            label = key[4:-3] or "all"
            values = [float(r[key]) for r in timed]
            print(f"    {label:>12}  {median(values):6.2f} / {max(values):6.2f}")


if __name__ == "__main__":
    main()
