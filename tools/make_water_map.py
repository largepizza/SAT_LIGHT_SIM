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

2026-09-30: the SHORELINE comes from Natural Earth 10 m land + lakes (1:10M, ~1 km; public domain,
naturalearthdata.com), rasterised at 16K between 60 S and 72 N, its distance field averaged down to the 8K
texels (the zero crossing is then sub-texel: coasts sat up to ~2.5 km off with the 5-km mask — Santa
Monica's beach was 2 km inland). Outside that band (ice shelves, sea ice coasts) the 8K mask as before.
The inputs are read from the RAW copies (make_raw_textures.py's .r8), not the PNGs: decoding the big PNGs
has frozen the development machine (docs/FREEZES.md). The zips are cached in build/ne_cache/ (downloaded
on first use).

Needs numpy, scipy, Pillow, pyshp. Re-run after editing either source:
    python tools/make_water_map.py [--preview out.png] [--no-ne]
"""
import argparse
import os
import struct

import io
import urllib.request
import zipfile

import numpy as np
from PIL import Image, ImageDraw
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


def read_slr8(*paths):
    """make_raw_textures.py's SLR8 (header: magic, width, total height, first row); split files joined."""
    rows, W, H = [], 0, 0
    for p in paths:
        with open(p, "rb") as f:
            hdr = f.read(16)
            assert hdr[:4] == b"SLR8", p
            W, H, first = struct.unpack("<III", hdr[4:])
            data = np.frombuffer(f.read(), np.uint8)
            rows.append((first, data.reshape(-1, W)))
    rows.sort(key=lambda r: r[0])
    out = np.concatenate([r[1] for r in rows], axis=0)
    assert out.shape == (H, W), (out.shape, H, W)
    return out


NE_URL = "https://naciscdn.org/naturalearth/10m/physical/{}.zip"
NE_LAT = (-60.0, 72.0)     # the band the Natural Earth coastline replaces the 8K mask in
NE_SCALE = 2               # 16K


def ne_shapes(name):
    import shapefile
    cache = os.path.join(REPO, "build", "ne_cache")
    os.makedirs(cache, exist_ok=True)
    zp = os.path.join(cache, name + ".zip")
    if not os.path.exists(zp):
        req = urllib.request.Request(NE_URL.format(name), headers={"User-Agent": "SatLightSim-dev"})
        open(zp, "wb").write(urllib.request.urlopen(req).read())
    z = zipfile.ZipFile(zp)
    rd = lambda ext: io.BytesIO(z.read(next(n for n in z.namelist() if n.endswith(ext))))
    return shapefile.Reader(shp=rd(".shp"), shx=rd(".shx"), dbf=rd(".dbf")).shapes()


def ne_water_mask(W, H):
    """Water (True) at W x H from Natural Earth: not land, or a lake."""
    img = Image.new("L", (W, H), 0)
    dr = ImageDraw.Draw(img)
    def rings(shape):
        pts, parts = shape.points, list(shape.parts) + [len(shape.points)]
        for i in range(len(parts) - 1):
            ring = pts[parts[i]:parts[i + 1]]
            if len(ring) < 3:
                continue
            a = sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(ring, ring[1:] + ring[:1]))
            yield [((x + 180.0) / 360.0 * W, (90.0 - y) / 180.0 * H) for x, y in ring], a < 0   # CW = exterior
    for shp in ne_shapes("ne_10m_land"):
        for xy, ext in rings(shp):
            if ext: dr.polygon(xy, fill=255)
    for shp in ne_shapes("ne_10m_land"):
        for xy, ext in rings(shp):
            if not ext: dr.polygon(xy, fill=0)
    for shp in ne_shapes("ne_10m_lakes"):
        for xy, ext in rings(shp):
            dr.polygon(xy, fill=0 if ext else 255)
    return np.asarray(img) < 128


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", help="write a downsampled RGB preview here")
    ap.add_argument("--no-ne", action="store_true", help="the 8K mask alone (the pre-2026-09-30 bake)")
    a = ap.parse_args()

    mask = read_slr8(os.path.join(TEX, "8k_earth_specular_map.r8")) > 127
    h, w = mask.shape
    # The source mask's last row is all water — over the Antarctic plateau, an artifact of the map (it
    # became a 3600-m "lake" around the pole). Take the row above it.
    mask = mask.copy()
    mask[-1] = mask[-2]
    dem_full = read_slr8(os.path.join(TEX, "earth_elevation_0.r8"), os.path.join(TEX, "earth_elevation_1.r8"))
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
    if not a.no_ne:
        # The shoreline from Natural Earth at 16K inside the band, the 8K mask (upsampled) outside it.
        W2, H2 = w * NE_SCALE, h * NE_SCALE
        ne = ne_water_mask(W2, H2)
        lat2 = 90.0 - (np.arange(H2) + 0.5) / H2 * 180.0
        band = (lat2 > NE_LAT[0]) & (lat2 < NE_LAT[1])
        m2 = np.repeat(np.repeat(mask, NE_SCALE, 0), NE_SCALE, 1)
        m2[band] = ne[band]
        del ne
        print(f"16K mask: water {m2.mean() * 100:.1f}%")
        P2 = K_PAD * NE_SCALE
        mp2 = np.concatenate([m2[:, -P2:], m2, m2[:, :P2]], axis=1)
        del m2
        d_in2 = ndimage.distance_transform_edt(mp2).astype(np.float32)
        d_out2 = ndimage.distance_transform_edt(~mp2).astype(np.float32)
        s2 = np.where(mp2, d_in2 - 0.5, -(d_out2 - 0.5))
        del d_in2, d_out2, mp2
        s2 = ndimage.gaussian_filter(s2, 0.7, mode="nearest")[:, P2:P2 + W2]
        # Down to the 8K texels (2x2 mean), in 8K texel units; blended into the band only (a 2-deg ramp).
        s8 = s2.reshape(h, NE_SCALE, w, NE_SCALE).mean(axis=(1, 3)) / NE_SCALE
        del s2
        lat8 = 90.0 - (np.arange(h) + 0.5) / h * 180.0
        wgt = np.clip(np.minimum(lat8 - NE_LAT[0], NE_LAT[1] - lat8) / 2.0, 0.0, 1.0)[:, None]
        sdf = (sdf * (1.0 - wgt) + s8 * wgt).astype(np.float32)
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
