#!/usr/bin/env python3
"""Cloud MORPHOLOGY textures for clouds v2 (review 17): what cloud fields look like at 3-100 km — the
organisation a weather map's 5-km coverage and a Perlin field cannot give (seen from orbit, Perlin
thresholded to the coverage reads as blotches).

    python tools/make_cloud_morph.py [--size 1024] [--preview out.png]

Writes assets/textures/cloud_morph.rgba8: raw RGBA8, SIZE x SIZE, row-major, tiling seamlessly (every
field is periodic: FFT-filtered noise, Voronoi on a wrapped lattice). One period of the texture is
CV2 "Morphology period" (default 320 km), so a texel is ~310 m. Channels, each rank-equalised to a
UNIFORM distribution so that thresholding it at (1 - coverage) covers exactly that fraction (the
shader turns it into the far field's normal deviate):

  R  closed cells — stratocumulus decks over cool oceans: polygons 20-40 km across, bright lumpy
     interiors, thin dark rims where the cells subside (the classic "honeycomb" of the eastern
     subtropical oceans and the Southern Ocean).
  G  open cells — cold-air outbreaks behind fronts: clear centres 30-60 km across ringed by
     cumulus walls, broken into clumps.
  B  cloud streets — horizontal convective rolls: rows ~5 km apart along the wind, meandering,
     breaking into puffs and merging.
  A  clustered cumulus — fair-weather fields: puffs of 1-4 km gathered into clumps and bands
     with clear lanes between them (popcorn cumulus), and the fractal edges of broken fields.

Everything here is generated from scratch; no imagery is copied. The forms follow published
descriptions of mesoscale cellular convection (Agee 1984; Wood & Hartmann 2006) and roll
convection (Etling & Brown 1993).
"""
import argparse
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def fbm(n, rng, beta=2.0, lo=1.0, hi=None):
    """Periodic fractal noise: white noise filtered by k^-beta/2 between wavenumbers lo..hi (cycles per
    tile). Zero mean, unit standard deviation."""
    kx = np.fft.fftfreq(n) * n
    ky = np.fft.rfftfreq(n) * n
    k = np.sqrt(kx[:, None] ** 2 + ky[None, :] ** 2)
    amp = np.where(k >= lo, np.power(np.maximum(k, 1e-6), -beta / 2.0), 0.0)
    if hi is not None:
        amp *= np.exp(-(k / hi) ** 2)
    spec = (rng.standard_normal(k.shape) + 1j * rng.standard_normal(k.shape)) * amp
    f = np.fft.irfft2(spec, s=(n, n))
    return (f - f.mean()) / (f.std() + 1e-12)


def warp(field, dx, dy):
    """Sample a periodic field at (x + dx, y + dy) (texels), bilinear, wrapping."""
    n = field.shape[0]
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    xs, ys = (x + dx) % n, (y + dy) % n
    x0, y0 = np.floor(xs).astype(int), np.floor(ys).astype(int)
    fx, fy = xs - x0, ys - y0
    x1, y1 = (x0 + 1) % n, (y0 + 1) % n
    return (field[y0, x0] * (1 - fx) * (1 - fy) + field[y0, x1] * fx * (1 - fy)
            + field[y1, x0] * (1 - fx) * fy + field[y1, x1] * fx * fy)


def voronoi(n, cells, rng, jitter=0.9, dx=None, dy=None):
    """F1, F2 distances (texels) to a jittered lattice of cells x cells points on the torus, optionally at
    warped sample positions. Returns (f1, f2, id of the nearest point)."""
    c = n / cells
    pts = (np.stack(np.meshgrid(np.arange(cells), np.arange(cells), indexing="xy"), -1).reshape(-1, 2) + 0.5
           + (rng.random((cells * cells, 2)) - 0.5) * jitter) * c
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    if dx is not None:
        x, y = (x + dx) % n, (y + dy) % n
    gx, gy = np.floor(x / c).astype(int), np.floor(y / c).astype(int)
    f1 = np.full((n, n), 1e9)
    f2 = np.full((n, n), 1e9)
    idx = np.zeros((n, n), dtype=np.int64)
    for oy in (-1, 0, 1):
        for ox in (-1, 0, 1):
            cx, cy = (gx + ox) % cells, (gy + oy) % cells
            pid = cy * cells + cx
            px, py = pts[pid, 0], pts[pid, 1]
            ddx = x - px
            ddy = y - py
            ddx -= np.round(ddx / n) * n
            ddy -= np.round(ddy / n) * n
            d = np.sqrt(ddx * ddx + ddy * ddy)
            closer = d < f1
            f2 = np.where(closer, f1, np.minimum(f2, d))
            idx = np.where(closer, pid, idx)
            f1 = np.where(closer, d, f1)
    return f1, f2, idx


def equalise(f):
    """Rank transform to a uniform distribution on [0, 1]."""
    r = np.argsort(np.argsort(f.ravel(), kind="stable"), kind="stable").reshape(f.shape)
    return (r + 0.5) / f.size


def smooth(f, sigma):
    """Periodic Gaussian blur (FFT)."""
    n = f.shape[0]
    kx = np.fft.fftfreq(n)
    ky = np.fft.rfftfreq(n)
    g = np.exp(-2.0 * (np.pi * sigma) ** 2 * (kx[:, None] ** 2 + ky[None, :] ** 2))
    return np.fft.irfft2(np.fft.rfft2(f) * g, s=f.shape)


def closed_cells(n, rng):
    # Two scales of cells (big ~35 km, some split into ~20 km), warped so the polygons are not straight.
    wx, wy = fbm(n, rng, 2.6, 2, 40) * 6.0, fbm(n, rng, 2.6, 2, 40) * 6.0
    f1, f2, idx = voronoi(n, 9, rng, 0.95, wx, wy)          # ~36 km cells at 320 km
    g1, g2, _ = voronoi(n, 16, rng, 0.95, wx * 0.7, wy * 0.7)  # ~20 km
    split = (np.random.default_rng(idx.ravel() * 7 + 3).random(idx.size).reshape(idx.shape) < 0.35)
    edge = np.where(split, np.minimum(f2 - f1, (g2 - g1) * 1.3), f2 - f1)
    rim = 1.0 - np.exp(-(edge / 9.0) ** 2)                 # dark subsiding rims, ~3-5 km
    interior = fbm(n, rng, 2.2, 8, 400)                     # lumps inside the cells
    cellBright = np.random.default_rng(idx.ravel() * 13 + 1).random(idx.size).reshape(idx.shape)
    v = rim * (0.75 + 0.25 * cellBright) + 0.18 * interior + 0.10 * fbm(n, rng, 1.6, 1, 20)
    return smooth(v, 0.8)


def open_cells(n, rng):
    wx, wy = fbm(n, rng, 2.6, 2, 30) * 9.0, fbm(n, rng, 2.6, 2, 30) * 9.0
    f1, f2, idx = voronoi(n, 7, rng, 0.95, wx, wy)          # ~46 km cells
    edge = f2 - f1
    size = np.random.default_rng(idx.ravel() * 5 + 11).random(idx.size).reshape(idx.shape)
    wall = np.exp(-(edge / (7.0 + 6.0 * size)) ** 2)        # cumulus walls on the cell boundaries
    clumps = smooth(np.maximum(fbm(n, rng, 1.4, 20, 400), -0.5), 1.0)
    v = wall * (0.55 + 0.45 * np.clip(clumps * 0.7 + 0.6, 0.0, 1.0)) + 0.015 * fbm(n, rng, 2.0, 2, 60)
    return smooth(v, 0.7)


def streets(n, rng):
    rows = 64                                               # ~5 km apart at 320 km
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    meander = fbm(n, rng, 3.0, 1, 12) * 5.0 + fbm(n, rng, 2.4, 6, 40) * 1.5
    phase = (y + meander) / n * rows * 2.0 * np.pi
    roll = np.cos(phase) * 0.5 + 0.5
    roll = roll ** 3                                        # narrow cloud lines, wide clear lanes
    # Along the rows: puffs (stretched noise) and breaks; some rolls fade out regionally.
    puffs = fbm(n, rng, 1.2, 30, 500)
    puffs = smooth(np.maximum(puffs, -0.8), 0.6)
    along = warp(puffs, 0.0, 0.0)
    region = fbm(n, rng, 2.8, 1, 10)
    v = roll * (0.6 + 0.4 * np.clip(along * 0.6 + 0.5, 0.0, 1.0)) * (0.55 + 0.45 * np.clip(region * 0.6 + 0.6, 0.0, 1.0))
    return v + 0.04 * fbm(n, rng, 2.0, 2, 80)


def cumulus_field(n, rng):
    # Puffs (inverted Worley, ~2.5 km) gathered by a clumping field into patches and bands.
    f1, f2, idx = voronoi(n, 128, rng, 0.9)
    puff = np.exp(-(f1 / 3.2) ** 2)                         # each cell a round puff
    size = np.random.default_rng(idx.ravel() * 17 + 5).random(idx.size).reshape(idx.shape)
    puff *= 0.5 + 0.5 * size
    clump = fbm(n, rng, 2.4, 2, 120)
    bands = fbm(n, rng, 3.2, 1, 8)
    gate = 1.0 / (1.0 + np.exp(-(clump * 1.6 + bands * 0.8)))
    v = puff * gate + 0.25 * gate
    return smooth(v, 0.5)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--size", type=int, default=1024)
    ap.add_argument("--seed", type=int, default=20261001)
    ap.add_argument("--preview", help="also write a PNG preview (the four channels side by side)")
    a = ap.parse_args()
    n = a.size
    rng = np.random.default_rng(a.seed)
    chans = []
    for name, fn in (("closed cells", closed_cells), ("open cells", open_cells), ("streets", streets),
                     ("cumulus field", cumulus_field)):
        f = fn(n, rng)
        u = equalise(f)
        chans.append(np.clip(np.round(u * 255.0), 0, 255).astype(np.uint8))
        print(f"{name}: done", file=sys.stderr)
    out = np.stack(chans, -1)
    path = os.path.join(ROOT, "assets", "textures", "cloud_morph.rgba8")
    out.tofile(path)
    print(f"wrote {path} ({n}x{n} RGBA8, {out.nbytes} bytes)")
    if a.preview:
        from PIL import Image
        Image.fromarray(np.concatenate([chans[i] for i in range(4)], axis=1)).save(a.preview)
        print(f"preview {a.preview}")


if __name__ == "__main__":
    main()
