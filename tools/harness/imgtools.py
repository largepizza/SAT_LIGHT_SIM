#!/usr/bin/env python3
"""Image helpers for harness captures (docs/HARNESS.md). Needs Pillow + numpy:

    py -3 -m venv tools/harness/.venv
    tools/harness/.venv/Scripts/python -m pip install -r tools/harness/requirements.txt

Subcommands (all print JSON to stdout, so an agent can read numbers before looking at pictures):

  sheet <png|dir>... -o sheet.png [--cols 3] [--width 480]
        Labelled contact sheet: many variants in one image (cheaper to look at than N images).
  diff a.png b.png [-o heat.png] [--gain 8] [--threshold 8]
        Per-pixel difference: mean/max abs, RMS, PSNR, fraction of pixels past the threshold,
        the bounding box of the changed region, and an amplified heatmap image.
  stats a.png [--grid 4]
        Luminance mean/percentiles, fraction clipped black/white, and a coarse grid of mean
        luminance (to spot a black or blown-out region without looking).
  crop a.png x y w h [--scale 4] -o out.png
        Cut a region out and enlarge it with nearest neighbour (pixel-exact).
  seams a.png [--axis both] [--top 10]
        Rows/columns whose mean luminance jumps against their neighbours: tile seams, bands,
        the horizontal lines of a bad terrain march.
"""
import argparse
import glob
import json
import math
import os
import sys

try:
    import numpy as np
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("imgtools needs Pillow + numpy: tools/harness/.venv/Scripts/python -m pip install -r tools/harness/requirements.txt")


def load(p):
    return np.asarray(Image.open(p).convert("RGB")).astype(np.float32)


def lum(a):
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def font(size):
    for name in ("arial.ttf", "DejaVuSans.ttf", "Helvetica.ttc"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def expand(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            cap = os.path.join(p, "captures")
            out += sorted(glob.glob(os.path.join(cap if os.path.isdir(cap) else p, "*.png")))
        else:
            out += sorted(glob.glob(p)) or [p]
    return out


def cmd_sheet(a):
    paths = expand(a.inputs)
    if not paths:
        sys.exit("no images")
    ims = [Image.open(p).convert("RGB") for p in paths]
    w = a.width
    thumbs = [im.resize((w, max(1, round(im.height * w / im.width))), Image.LANCZOS) for im in ims]
    cols = min(a.cols, len(thumbs))
    rows = math.ceil(len(thumbs) / cols)
    label_h = 22
    cell_h = max(t.height for t in thumbs) + label_h
    sheet = Image.new("RGB", (cols * w + (cols + 1) * 4, rows * cell_h + (rows + 1) * 4), (24, 24, 24))
    d = ImageDraw.Draw(sheet)
    f = font(14)
    for i, (t, p) in enumerate(zip(thumbs, paths)):
        x = 4 + (i % cols) * (w + 4)
        y = 4 + (i // cols) * (cell_h + 4)
        sheet.paste(t, (x, y + label_h))
        d.text((x + 3, y + 3), os.path.splitext(os.path.basename(p))[0], fill=(235, 235, 235), font=f)
    sheet.save(a.out)
    print(json.dumps({"sheet": os.path.abspath(a.out), "images": len(paths), "size": sheet.size}))


def cmd_diff(a):
    x, y = load(a.a), load(a.b)
    if x.shape != y.shape:
        sys.exit(f"size mismatch {x.shape} vs {y.shape}")
    d = np.abs(x - y)
    dl = d.max(axis=2)
    mse = float((d ** 2).mean())
    changed = dl > a.threshold
    res = {"mean_abs": float(d.mean()), "max_abs": float(d.max()), "rms": math.sqrt(mse),
           "psnr_db": float("inf") if mse == 0 else 10 * math.log10(255 ** 2 / mse),
           "changed_frac": float(changed.mean()), "threshold": a.threshold}
    if changed.any():
        ys, xs = np.nonzero(changed)
        res["changed_bbox"] = [int(xs.min()), int(ys.min()), int(xs.max() - xs.min() + 1), int(ys.max() - ys.min() + 1)]
    if a.out:
        heat = np.clip(dl * a.gain, 0, 255).astype(np.int32)
        # dark-red-yellow-white ramp
        rgb = np.stack([np.clip(heat * 3, 0, 255), np.clip(heat * 3 - 255, 0, 255), np.clip(heat * 3 - 510, 0, 255)], -1)
        Image.fromarray(rgb.astype(np.uint8)).save(a.out)
        res["heatmap"] = os.path.abspath(a.out)
    print(json.dumps(res, indent=1))


def cmd_stats(a):
    x = load(a.a)
    L = lum(x)
    g = a.grid
    h, w = L.shape
    grid = [[round(float(L[r * h // g:(r + 1) * h // g, c * w // g:(c + 1) * w // g].mean()), 1) for c in range(g)] for r in range(g)]
    res = {"size": [w, h], "lum_mean": float(L.mean()),
           "lum_p": {str(p): float(np.percentile(L, p)) for p in (1, 5, 50, 95, 99)},
           "black_frac": float((x.max(axis=2) <= 2).mean()), "white_frac": float((x.min(axis=2) >= 253).mean()),
           "rgb_mean": [float(v) for v in x.reshape(-1, 3).mean(axis=0)], "lum_grid": grid}
    print(json.dumps(res, indent=1))


def cmd_crop(a):
    im = Image.open(a.a).convert("RGB").crop((a.x, a.y, a.x + a.w, a.y + a.h))
    if a.scale != 1:
        im = im.resize((a.w * a.scale, a.h * a.scale), Image.NEAREST)
    im.save(a.out)
    print(json.dumps({"crop": os.path.abspath(a.out), "size": im.size}))


def cmd_seams(a):
    L = lum(load(a.a))
    out = {}
    for axis, name in ((1, "rows"), (0, "cols")):
        if a.axis not in ("both", name):
            continue
        prof = L.mean(axis=axis)  # mean per row (axis=1) or column (axis=0)
        # second difference: a one-line step or spike stands out against smooth gradients
        d2 = np.abs(prof[1:-1] - 0.5 * (prof[:-2] + prof[2:]))
        idx = np.argsort(d2)[::-1][:a.top]
        med = float(np.median(d2)) + 1e-6
        out[name] = [{"index": int(i + 1), "jump": round(float(d2[i]), 3), "x_median": round(float(d2[i] / med), 1)} for i in idx]
    print(json.dumps(out, indent=1))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("sheet"); s.add_argument("inputs", nargs="+"); s.add_argument("-o", "--out", required=True)
    s.add_argument("--cols", type=int, default=3); s.add_argument("--width", type=int, default=480)
    s = sp.add_parser("diff"); s.add_argument("a"); s.add_argument("b"); s.add_argument("-o", "--out")
    s.add_argument("--gain", type=float, default=8.0); s.add_argument("--threshold", type=float, default=8.0)
    s = sp.add_parser("stats"); s.add_argument("a"); s.add_argument("--grid", type=int, default=4)
    s = sp.add_parser("crop"); s.add_argument("a")
    for k in ("x", "y", "w", "h"):
        s.add_argument(k, type=int)
    s.add_argument("--scale", type=int, default=4); s.add_argument("-o", "--out", required=True)
    s = sp.add_parser("seams"); s.add_argument("a"); s.add_argument("--axis", default="both", choices=["both", "rows", "cols"])
    s.add_argument("--top", type=int, default=10)
    a = ap.parse_args()
    {"sheet": cmd_sheet, "diff": cmd_diff, "stats": cmd_stats, "crop": cmd_crop, "seams": cmd_seams}[a.cmd](a)


if __name__ == "__main__":
    main()
