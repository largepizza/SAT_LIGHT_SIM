#!/usr/bin/env python3
"""Cloud-field statistics for benchmarking the clouds' look from space against real imagery (review 17).

    python tools/cloud_stats.py image.png [image2.jpg ...] [--km-per-px 0.8] [--crop x,y,w,h] [--mask out.png]

For each image: a cloud mask (bright and unsaturated: white cloud over a dark sea or land, Otsu threshold on
brightness - saturation), then
  cover        the cloud fraction
  D            the perimeter-area fractal dimension of the cloud objects: P ~ A^(D/2) over objects of 10 px
               and up (Lovejoy 1982: D ~ 1.35 for real cloud and rain areas from 1 to 1e6 km^2; smooth blobs
               come out near 1.0-1.15, the "Perlin" look)
  size_b       the exponent of the object-area distribution, N(> A) ~ A^-b (real cloud fields ~0.7-1.0)
  spec_slope   the radially averaged power spectrum slope of the cloud MASK (k^-slope; smaller = more fine
               structure; the brightness spectrum is confounded by glint and limb gradients in renders)
  holes        clear "holes" per 1000 km^2 inside cloudy areas (open cells and broken decks have many)
Prints a table (JSON with --json). Needs numpy, scipy, Pillow.
"""
import argparse
import json
import os
import sys

import numpy as np
from PIL import Image

try:
    from scipy import ndimage
except ImportError:
    sys.exit("cloud_stats needs scipy")


def otsu(v):
    h, e = np.histogram(v, bins=256)
    c = (e[:-1] + e[1:]) / 2
    w0 = np.cumsum(h)
    w1 = w0[-1] - w0
    m0 = np.cumsum(h * c) / np.maximum(w0, 1)
    mt = (h * c).sum()
    m1 = (mt - np.cumsum(h * c)) / np.maximum(w1, 1)
    return c[np.argmax(w0 * w1 * (m0 - m1) ** 2)]


def stats(path, kmpx, crop=None, maskout=None, scale=1.0):
    im = Image.open(path).convert("RGB")
    if crop:
        x, y, w, h = crop
        im = im.crop((x, y, x + w, y + h))
    if scale != 1.0:   # compare at one ground resolution: renders' clouds are half-res, MODIS thumbnails sharp
        im = im.resize((max(1, int(im.width * scale)), max(1, int(im.height * scale))), Image.BOX)
        kmpx /= scale
    a = np.asarray(im).astype(np.float64) / 255.0
    mx, mn = a.max(-1), a.min(-1)
    bright = 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]
    sat = (mx - mn) / np.maximum(mx, 1e-3)
    score = bright - 0.6 * sat
    thr = otsu(score)
    mask = score > thr
    mask = ndimage.binary_opening(mask, iterations=1)
    if maskout:
        Image.fromarray((mask * 255).astype(np.uint8)).save(maskout)
    lab, n = ndimage.label(mask)
    areas = ndimage.sum(np.ones_like(lab), lab, index=np.arange(1, n + 1))
    # perimeter: boundary pixels (4-neighbour) per object
    er = ndimage.binary_erosion(mask)
    edge = mask & ~er
    per = ndimage.sum(edge, lab, index=np.arange(1, n + 1))
    keep = (areas >= 10) & (per > 0)
    D = float("nan")
    if keep.sum() >= 8:
        slope = np.polyfit(np.log(areas[keep]), np.log(per[keep]), 1)[0]
        D = 2.0 * slope
    b = float("nan")
    A = np.sort(areas[areas >= 10])[::-1]
    if len(A) >= 8:
        N = np.arange(1, len(A) + 1)
        sel = (A > 30) & (A < A[0] * 0.5)
        if sel.sum() >= 5:
            b = -np.polyfit(np.log(A[sel]), np.log(N[sel]), 1)[0]
    # spectrum
    g = mask.astype(np.float64) - mask.mean()   # the MASK's spectrum: brightness gradients (glint, limb) confound it
    F = np.abs(np.fft.fftshift(np.fft.fft2(g))) ** 2
    hh, ww = g.shape
    yy, xx = np.indices(F.shape)
    r = np.sqrt((xx - ww / 2) ** 2 + (yy - hh / 2) ** 2).astype(int)
    rad = np.bincount(r.ravel(), F.ravel()) / np.maximum(np.bincount(r.ravel()), 1)
    k = np.arange(len(rad))
    sel = (k > 4) & (k < min(hh, ww) // 4)
    spec = float(-np.polyfit(np.log(k[sel]), np.log(rad[sel] + 1e-12), 1)[0])
    # holes inside cloud: clear components not touching the border, surrounded by cloud
    clear_lab, nc = ndimage.label(~mask)
    border = set(np.unique(np.concatenate([clear_lab[0], clear_lab[-1], clear_lab[:, 0], clear_lab[:, -1]])))
    holes = sum(1 for i in range(1, nc + 1) if i not in border)
    cloud_km2 = mask.sum() * kmpx * kmpx
    return {"image": os.path.basename(path), "cover": float(mask.mean()), "D": D, "size_b": b,
            "spec_slope": spec, "objects": int(keep.sum()), "holes_per_1000km2": 1000.0 * holes / max(cloud_km2, 1.0)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("images", nargs="+")
    ap.add_argument("--km-per-px", type=float, default=0.8)
    ap.add_argument("--crop", help="x,y,w,h")
    ap.add_argument("--mask", help="write the first image's cloud mask here")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--scale", type=float, default=1.0, help="box-resample first (match ground resolutions)")
    a = ap.parse_args()
    crop = tuple(int(v) for v in a.crop.split(",")) if a.crop else None
    rows = [stats(p, a.km_per_px, crop, a.mask if i == 0 else None, a.scale) for i, p in enumerate(a.images)]
    if a.json:
        print(json.dumps(rows, indent=1))
        return
    print(f"{'image':60s} {'cover':>6s} {'D':>5s} {'b':>5s} {'spec':>5s} {'objs':>5s} {'holes/1e3km2':>12s}")
    for r in rows:
        print(f"{r['image'][:60]:60s} {r['cover']:6.2f} {r['D']:5.2f} {r['size_b']:5.2f} {r['spec_slope']:5.2f} "
              f"{r['objects']:5d} {r['holes_per_1000km2']:12.2f}")


if __name__ == "__main__":
    main()
