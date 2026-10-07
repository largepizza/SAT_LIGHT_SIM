#!/usr/bin/env python3
"""Temporal flicker heat map over a frame sequence (cinematic export or `path play record=`).

    tools/harness/.venv/Scripts/python tools/harness/flicker_heat.py <dir> --first 0 --last 359 -o heat.png [--prefix frame_]

Per pixel, the mean of |f(t) - (f(t-1) + f(t+1)) / 2| (luminance, 0-255): the second temporal difference, which
cancels smooth camera motion (a pan or tilt is ~linear frame to frame) and keeps what changes back and forth —
flicker, sparkle, TAA jitter residue. Prints the mean per region of a 4x4 grid and the worst frames of the
bottom-left quarter; writes an amplified heat map (x8) beside a mid frame.
"""
import argparse
import os

import numpy as np
from PIL import Image


def lum(path):
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("--first", type=int, default=0)
    ap.add_argument("--last", type=int, required=True)
    ap.add_argument("--prefix", default="frame_")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()
    names = [os.path.join(a.dir, f"{a.prefix}{i:05d}.png") for i in range(a.first, a.last + 1)]
    prev = lum(names[0])
    cur = lum(names[1])
    acc = np.zeros_like(cur)
    per_frame = []
    h, w = cur.shape
    for k in range(2, len(names)):
        nxt = lum(names[k])
        d = np.abs(cur - 0.5 * (prev + nxt))
        acc += d
        per_frame.append((a.first + k - 1, float(d[h // 2:, : w // 2].mean())))
        prev, cur = cur, nxt
    acc /= max(1, len(names) - 2)
    print("grid (rows top->bottom) of mean |2nd temporal diff|:")
    for r in range(4):
        print("  " + "  ".join(f"{acc[r * h // 4:(r + 1) * h // 4, c * w // 4:(c + 1) * w // 4].mean():6.2f}" for c in range(4)))
    worst = sorted(per_frame, key=lambda x: -x[1])[:8]
    print("worst frames, bottom-left quarter:", ", ".join(f"{f}:{v:.2f}" for f, v in worst))
    heat = np.clip(acc * 8.0, 0, 255).astype(np.uint8)
    mid = Image.open(names[len(names) // 2]).convert("RGB")
    hm = Image.fromarray(heat).convert("RGB")
    out = Image.new("RGB", (w * 2, h))
    out.paste(mid, (0, 0))
    out.paste(hm, (w, 0))
    out.save(a.out)
    print(a.out)


if __name__ == "__main__":
    main()
