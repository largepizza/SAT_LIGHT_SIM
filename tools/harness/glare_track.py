"""Track a glare's position over a sequence of captures and compare it with the Sun's image.

    python tools/harness/glare_track.py <run>/captures m_ [--plot out.png]

For every <prefix>NN.png (and its .json sidecar) it finds the glare core — the centroid of the
brightest blob (pixels within 3% of the frame's peak luminance, the largest such component near the
peak) — and reads where the CPU puts the Sun's image in the selected satellite's mirror
(`state.selection.mesh.sprite_px`, the review-28 sprite placement). Prints both per frame and the
frame-to-frame motion; with --plot, draws them. The glare's position is the GPU's own glint list
(`state.selection.glints`, the brightest entry) when the sidecar has it, else found in the image from
the point its spikes radiate from. A glare that sits on the Sun's image follows the
predicted track; one that wanders shows up as the gap between the two lines.
(2026-10-06, the release cinematic's mirror shot.)
"""
import argparse
import glob
import json
import os
import sys

import numpy as np
from PIL import Image


def glare_core(path, near=None, radius=260):
    """The glare's centre as the point its spikes radiate from: every pixel on a thin bright ridge
    (outside the saturated mirror, which hides the core) contributes the line through it along the
    ridge, and the point nearest all those lines (least squares) is the centre. Robust to a saturated
    mirror under the core, where a brightest-blob centroid lands on the mirror instead."""
    from scipy import ndimage
    im = np.asarray(Image.open(path).convert("RGB")).astype(np.float32) / 255.0
    lum = 0.2126 * im[..., 0] + 0.7152 * im[..., 1] + 0.0722 * im[..., 2]
    H, W = lum.shape
    # Ridge strength: the line minus its surroundings (a top-hat), so the sky glow under the glare
    # does not count, only the spikes.
    ridge = lum - ndimage.uniform_filter(lum, size=9)
    sat = ndimage.binary_dilation(lum > 0.97, iterations=8)
    gx = ndimage.sobel(ndimage.gaussian_filter(lum, 1.0), axis=1)
    gy = ndimage.sobel(ndimage.gaussian_filter(lum, 1.0), axis=0)
    Jxx = ndimage.gaussian_filter(gx * gx, 2.0)
    Jyy = ndimage.gaussian_filter(gy * gy, 2.0)
    Jxy = ndimage.gaussian_filter(gx * gy, 2.0)
    tr = Jxx + Jyy
    det = Jxx * Jyy - Jxy * Jxy
    disc = np.sqrt(np.maximum(tr * tr / 4 - det, 0))
    l1, l2 = tr / 2 + disc, tr / 2 - disc
    coh = (l1 - l2) / np.maximum(l1 + l2, 1e-9)
    # The gradient's dominant direction is ACROSS the line; the line runs perpendicular to it.
    ang = 0.5 * np.arctan2(2 * Jxy, Jxx - Jyy)
    dx, dy = -np.sin(ang), np.cos(ang)
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    w = np.maximum(ridge, 0) * (coh > 0.6) * (~sat)
    if near is not None:
        w = w * ((xx - near[0]) ** 2 + (yy - near[1]) ** 2 < radius * radius)
    if w.sum() < 1e-3:
        return None, float(lum.max())
    # Sum of (I - d d^T) over the line pixels, and its product with the pixel positions.
    a11 = (w * (1 - dx * dx)).sum(); a12 = (w * (-dx * dy)).sum(); a22 = (w * (1 - dy * dy)).sum()
    b1 = (w * ((1 - dx * dx) * xx - dx * dy * yy)).sum()
    b2 = (w * (-dx * dy * xx + (1 - dy * dy) * yy)).sum()
    A = np.array([[a11, a12], [a12, a22]]); b = np.array([b1, b2])
    p = np.linalg.solve(A, b)
    # One reweighting pass: drop line pixels whose line misses the estimate by more than 6 px (stars,
    # the satellite rings, the mirror's own edges).
    for _ in range(2):
        rx, ry = xx - p[0], yy - p[1]
        dist = np.abs(rx * dy - ry * dx)
        w2 = w * (dist < 6.0)
        a11 = (w2 * (1 - dx * dx)).sum(); a12 = (w2 * (-dx * dy)).sum(); a22 = (w2 * (1 - dy * dy)).sum()
        b1 = (w2 * ((1 - dx * dx) * xx - dx * dy * yy)).sum()
        b2 = (w2 * (-dx * dy * xx + (1 - dy * dy) * yy)).sum()
        p = np.linalg.solve(np.array([[a11, a12], [a12, a22]]), np.array([b1, b2]))
    return (float(p[0]), float(p[1])), float(lum.max())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("prefix")
    ap.add_argument("--plot")
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.dir, a.prefix + "*.png")))
    rows = []
    for f in files:
        sp = cp = None
        side = os.path.splitext(f)[0] + ".json"
        if os.path.exists(side):
            st = json.load(open(side)).get("state", {})
            m = st.get("selection", {}).get("mesh", {})
            sp = m.get("sprite_px")
            cp = m.get("centre_px")
            gl = st.get("selection", {}).get("glints")
        else:
            gl = None
        if gl:   # the GPU's own glint list (state.selection.glints): the brightest glint is the glare
            b = max(gl, key=lambda e: e["flare"])
            g, peak = tuple(b["px"]), b["flare"]
        else:    # older runs: find it in the image
            g, peak = glare_core(f, near=cp or (800, 450))
        rows.append((os.path.basename(f), g, sp, cp, peak))
    prev = None
    print("%-14s %18s %18s %18s %8s %8s" % ("frame", "glare", "sun image", "mirror centre", "err px", "step px"))
    for name, g, sp, cp, peak in rows:
        err = step = float("nan")
        if g and sp:
            err = float(np.hypot(g[0] - sp[0], g[1] - sp[1]))
        if g and prev:
            step = float(np.hypot(g[0] - prev[0], g[1] - prev[1]))
        prev = g
        fmt = lambda p: "(%7.1f,%7.1f)" % tuple(p[:2]) if p else "        -        "
        print("%-14s %18s %18s %18s %8.1f %8.1f" % (name, fmt(g), fmt(sp), fmt(cp), err, step))
    if a.plot:
        # A plain PIL chart (no matplotlib): x and y panels, glare (red) vs the Sun's image (cyan) vs the
        # mirror centre (grey).
        from PIL import ImageDraw
        W, H, pad = 1200, 460, 40
        img = Image.new("RGB", (W, H), (16, 16, 18))
        d = ImageDraw.Draw(img)
        n = max(len(rows) - 1, 1)
        for j, lbl in ((0, "x"), (1, "y")):
            x0 = pad + j * W // 2
            pw = W // 2 - 2 * pad
            vals = [r[k][j] for r in rows for k in (1, 2, 3) if r[k]]
            if not vals:
                continue
            lo, hi = min(vals) - 5, max(vals) + 5
            def P(i, v):
                return (x0 + pw * i / n, pad + (H - 2 * pad) * (1 - (v - lo) / (hi - lo)))
            d.rectangle([x0, pad, x0 + pw, H - pad], outline=(70, 70, 75))
            d.text((x0, 8), "screen %s (px) %.0f..%.0f over %d frames" % (lbl, lo, hi, len(rows)), fill=(200, 200, 205))
            for k, col in ((3, (120, 120, 125)), (2, (80, 200, 230)), (1, (230, 60, 50))):
                pts = [P(i, r[k][j]) for i, r in enumerate(rows) if r[k]]
                if len(pts) > 1:
                    d.line(pts, fill=col, width=2)
                for q in pts:
                    d.ellipse([q[0] - 2, q[1] - 2, q[0] + 2, q[1] + 2], fill=col)
        d.text((pad, H - 22), "red: glare core (image)   cyan: Sun's image (CPU)   grey: mirror centre", fill=(200, 200, 205))
        img.save(a.plot)
        print("plot:", a.plot)

if __name__ == "__main__":
    sys.exit(main())
