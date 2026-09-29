"""Bake the water map: a smooth shoreline distance field + the water level of every lake and sea.

Terrain v2 P2 (.plans/TERRAIN_V2_PLAN.md). Source: assets/textures/8k_earth_specular_map.png (the
8192x4096 water mask, white = water) and earth_elevation.png (the 14999x7500 DEM, 15 = sea level).
Output, next to them, in make_raw_textures.py's SLR8 format (read by SatelliteSim.cpp's readRawR8,
interleaved into one R8G8 image bound where the mask was — no new texture binding):

  earth_water_sdf.r8    R: signed distance to the shore in mask texels, smoothed; 127.5 + d * 127.5 / kSdfMax,
                        d > 0 in water. So `> 0.5` is still "water" for every consumer of the old mask,
                        and the filtered field draws a smooth shoreline where thresholding the filtered
                        binary mask drew its 5 km texel staircase.
  earth_water_level.r8  G: the surface of the nearest water body, in DEM units (15 = sea level; decode
                        exactly like the DEM). Each connected water body takes the median DEM value over
                        its interior — the DEM is flat over a lake (it stores the surface). Land texels
                        hold their nearest water body's level, so the shore ramp knows what it meets.

Before this the renderer drew water only where the DEM was < 160 m, at SEA level: Superior, Victoria,
Titicaca, Baikal were flat blue land, and low lakes above sea level were pits.

Needs numpy, scipy, Pillow. Re-run after editing either source:
    python tools/make_water_map.py [--preview out.png]
"""
import argparse
import os
import struct

import numpy as np
from PIL import Image
from scipy import ndimage

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TEX = os.path.join(REPO, "assets", "textures")
Image.MAX_IMAGE_PIXELS = None

K_SDF_MAX = 4.0       # texels of distance at full scale (~20 km; 1 code ~ 0.03 texel ~ 150 m)
K_SMOOTH = 0.5        # Gaussian sigma (texels) on the distance field: rounds the staircase; 0.9 ate
                      # lakes 2-3 texels wide (Geneva) — 0.5 keeps every masked texel (measured)
K_PAD = 256           # columns of wrap-around padding (the antimeridian)
K_SEA = 15            # DEM byte of sea level


def write_slr8(path, data):
    h, w = data.shape
    with open(path, "wb") as f:
        f.write(b"SLR8" + struct.pack("<III", w, h, 0))
        f.write(np.ascontiguousarray(data, dtype=np.uint8).tobytes())
    print(f"{os.path.relpath(path, REPO)}: {w}x{h}, {os.path.getsize(path) / 1e6:.1f} MB")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", help="write a downsampled RGB preview here")
    a = ap.parse_args()

    mask = np.asarray(Image.open(os.path.join(TEX, "8k_earth_specular_map.png")).convert("L")) > 127
    h, w = mask.shape
    # The source mask's last row is all water — over the Antarctic plateau, an artifact of the map (it
    # became a 3600-m "lake" around the pole). Take the row above it.
    mask = mask.copy()
    mask[-1] = mask[-2]
    dem_full = np.asarray(Image.open(os.path.join(TEX, "earth_elevation.png")).convert("L"))
    dh, dw = dem_full.shape
    # The DEM at the mask's texel centres (nearest: a level is a median, filtering adds nothing).
    ys = np.clip(((np.arange(h) + 0.5) / h * dh).astype(np.int64), 0, dh - 1)
    xs = np.clip(((np.arange(w) + 0.5) / w * dw).astype(np.int64), 0, dw - 1)
    dem = dem_full[ys][:, xs]
    del dem_full
    print(f"mask {w}x{h}, water {mask.mean() * 100:.1f}%")

    # ── Water bodies, joined across the antimeridian ─────────────────────────────────────────────
    lab, n = ndimage.label(mask, structure=np.ones((3, 3), bool))
    left, right = lab[:, 0], lab[:, -1]
    both = (left > 0) & (right > 0)
    parent = np.arange(n + 1)

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for p, q in zip(left[both], right[both]):
        rp, rq = find(p), find(q)
        if rp != rq:
            parent[max(rp, rq)] = min(rp, rq)
    roots = np.array([find(i) for i in range(n + 1)])
    lab = roots[lab]
    ids = np.unique(lab[lab > 0])
    print(f"{len(ids)} water bodies")

    # Interior texels (away from the shore, whose DEM samples mix in land) where a body has any.
    interior = ndimage.binary_erosion(mask, structure=np.ones((3, 3), bool), border_value=1)
    lab_in = np.where(interior, lab, 0)
    med_in = ndimage.median(dem, lab_in, ids)
    med_all = ndimage.median(dem, lab, ids)
    has_in = ndimage.sum(interior, lab, ids) > 0
    level_of = np.where(has_in, med_in, med_all)
    level_of = np.maximum(np.round(level_of), K_SEA).astype(np.uint8)
    lut = np.full(lab.max() + 1, K_SEA, np.uint8)
    lut[ids] = level_of
    level_water = lut[lab]                       # valid on water texels

    sizes = ndimage.sum(mask, lab, ids)
    order = np.argsort(-sizes)[:25]
    for i in order:
        print(f"  body {ids[i]:7d}: {int(sizes[i]):9d} texels, level byte {level_of[i]:3d} "
              f"= {max(0.0, level_of[i] / 255 * 9000 - K_SEA / 255 * 9000):6.0f} m")

    # ── Distance fields, padded for the wrap ─────────────────────────────────────────────────────
    mp = np.concatenate([mask[:, -K_PAD:], mask, mask[:, :K_PAD]], axis=1)
    d_out, idx = ndimage.distance_transform_edt(~mp, return_indices=True)   # land: to nearest water
    d_in = ndimage.distance_transform_edt(mp)                               # water: to nearest land
    lp = np.concatenate([level_water[:, -K_PAD:], level_water, level_water[:, :K_PAD]], axis=1)
    level = lp[idx[0], idx[1]][:, K_PAD:K_PAD + w]                          # nearest water body's level
    del idx, lp
    sdf = np.where(mp, d_in - 0.5, -(d_out - 0.5)).astype(np.float32)
    del d_in, d_out
    sdf = ndimage.gaussian_filter(sdf, K_SMOOTH, mode="nearest")[:, K_PAD:K_PAD + w]
    r = np.clip(np.round(127.5 + sdf * (127.5 / K_SDF_MAX)), 0, 255).astype(np.uint8)
    # The smoothing may drop sub-texel water; keep every lake the mask marks (its level still applies).
    kept = (r > 127)
    print(f"water after smoothing {kept.mean() * 100:.2f}% (mask {mask.mean() * 100:.2f}%), "
          f"lake texels (level > sea) {((level > K_SEA) & kept).mean() * 100:.3f}%")

    write_slr8(os.path.join(TEX, "earth_water_sdf.r8"), r)
    write_slr8(os.path.join(TEX, "earth_water_level.r8"), level)

    if a.preview:
        prev = np.stack([r, level, (kept * 255).astype(np.uint8)], axis=-1)[::4, ::4]
        Image.fromarray(prev).save(a.preview)
        print("preview", a.preview)


if __name__ == "__main__":
    main()
