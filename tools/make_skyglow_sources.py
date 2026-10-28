"""Bake the skyglow source list: every lit texel of the night map as a light source for the sky-brightness model.

SatelliteSim::updateSkyglow (CLAUDE.md "Light Pollution Dome") sums these sources round the observer with a
Walker's-law propagation kernel to get the ARTIFICIAL sky brightness (ratio to the natural sky, Falchi et al. 2016's
convention) at the zenith and per compass direction. That ratio drives the dark-sky gate (Milky Way, zodiacal light,
aurora, comets: shaders/include/darksky.glsl) and the stars' naked-eye limit.

Source: assets/textures/8k_earth_nightmap.jpg (8192 x 4096, a tone-mapped night-lights composite with a blue base
under every texel). Per texel:
  * L = Rec.709 luminance of the sRGB bytes (what the app's CPU copy has always used);
  * linear light = (L/255)^2.2 - (16/255)^2.2 (the base removed; the base's luminance is 2.5-16.5 worldwide), kept only
    above 1e-3 (luminance ~19: below that it is the base's JPEG noise, and dropping it moves no reference site);
  * x (1 + 5 f), f = the share of SATURATED texels (L > 235) in the 5 x 5 round it: the map clips every town's core,
    so a big city's core and a small town's read alike; the share of clipped neighbours restores the difference (fitted
    against the reference sites, see below);
  * x the texel's area in km^2 (the rows shrink with cos(latitude)).
Stored with each source: the DEM height of its texel (the observer's height ABOVE the source sets how much of its glow
scatters above the observer).

Calibration (2026-10-28, the reference sites and fit in CLAUDE.md): with the app's kernel ((d^2 + 4^2)^-1.25 e^(-d/120 km),
d in km, 300 km reach, altitude weight 0.2 e^(-dh/8 km) + 0.8 e^(-dh/1.2 km)) and K = 4.37 the modelled zenith
22 - 2.5 log10(1 + ratio) tracks 16 reference sites from Paranal to Manhattan at 0.28 mag rms.

Format (little-endian): "SLSG", uint32 width (8192), height (4096), tile size (64), entry count N; then
(width/tile) x (height/tile) + 1 uint32 tile offsets (row-major tiles, top row first); then N entries of
{uint16 local = (y & 63) << 6 | (x & 63), uint16 ground height (m), float32 flux (linear light x km^2)}.

Needs numpy, scipy, Pillow. Re-run after changing the night map:
    python tools/make_skyglow_sources.py
"""
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.ndimage import uniform_filter

ROOT = Path(__file__).resolve().parent.parent
TEX = ROOT / "assets" / "textures"
TILE = 64
GAMMA = 2.2
BASE_LUM = 16.0
MIN_LIGHT = 1e-3
SAT_LUM = 235.0
SAT_BOOST = 5.0
R_EARTH_KM = 6371.0


def load_dem(h_out, w_out):
    rows = []
    for part in ("_0", "_1"):
        b = (TEX / f"earth_elevation{part}.r8").read_bytes()
        assert b[:4] == b"SLR8"
        w, h, _ = struct.unpack("<III", b[4:16])
        rows.append(np.frombuffer(b[16:], np.uint8).reshape(-1, w))
    d = np.vstack(rows).astype(np.float32)
    m = np.maximum(0.0, (d / 255.0 - 15.0 / 255.0) * 8848.0)
    ys = (np.arange(h_out) * d.shape[0] / h_out).astype(int)
    xs = (np.arange(w_out) * d.shape[1] / w_out).astype(int)
    return m[np.ix_(ys, xs)]


def main():
    Image.MAX_IMAGE_PIXELS = None
    rgb = np.asarray(Image.open(TEX / "8k_earth_nightmap.jpg").convert("RGB")).astype(np.float32)
    H, W, _ = rgb.shape
    lum = 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]
    del rgb
    lin = np.maximum((np.clip(lum, 0, 255) / 255.0) ** GAMMA - (BASE_LUM / 255.0) ** GAMMA, 0.0)
    sat = uniform_filter((lum > SAT_LUM).astype(np.float32), size=5, mode="wrap")
    lin *= 1.0 + SAT_BOOST * sat
    lin[lin <= MIN_LIGHT] = 0.0
    dy = np.pi / H * R_EARTH_KM
    lat = 90.0 - (np.arange(H) + 0.5) * 180.0 / H
    area = (dy * dy * np.cos(np.radians(lat)) * (H * 2 / W))[:, None]  # dx = 2 pi R cos / W, dy = pi R / H
    flux = (lin * area).astype(np.float32)
    dem = load_dem(H, W)

    tx, ty = W // TILE, H // TILE
    offsets = [0]
    chunks = []
    n = 0
    for j in range(ty):
        for i in range(tx):
            sub = flux[j * TILE:(j + 1) * TILE, i * TILE:(i + 1) * TILE]
            yy, xx = np.nonzero(sub)
            if len(yy):
                e = np.zeros(len(yy), dtype=[("loc", "<u2"), ("h", "<u2"), ("f", "<f4")])
                e["loc"] = (yy << 6) | xx
                e["h"] = np.clip(dem[j * TILE + yy, i * TILE + xx], 0, 65535).astype(np.uint16)
                e["f"] = sub[yy, xx]
                chunks.append(e.tobytes())
                n += len(yy)
            offsets.append(n)
    out = TEX / "skyglow_sources.bin"
    with open(out, "wb") as f:
        f.write(b"SLSG")
        f.write(struct.pack("<IIII", W, H, TILE, n))
        f.write(np.asarray(offsets, dtype="<u4").tobytes())
        for c in chunks:
            f.write(c)
    print(f"{out}: {n} sources, {out.stat().st_size / 1e6:.1f} MB")


if __name__ == "__main__":
    sys.exit(main())
