"""Bake the Moon's albedo + elevation map (sky binding 2, sat_sky.frag "Moon disc").

Sources (NASA Scientific Visualization Studio, CGI Moon Kit, https://svs.gsfc.nasa.gov/4720 — public domain,
credited in THIRD_PARTY_NOTICES.txt):
  lroc_color_poles_4k.tif  LRO Camera Wide Angle Camera colour mosaic (4096 x 2048, simple cylindrical)
  ldem_16.tif              LOLA elevation, 16 px/deg (5760 x 2880, float km relative to 1737.4 km)
Both are equirectangular in selenographic (mean Earth / polar axis) coordinates, 0 deg longitude in the image's
centre, east to the right, north up — checked here: the albedo correlates with the elevation (maria are low) only in
that orientation, and the South Pole-Aitken basin is the lowest region at ~170 W.

Output: assets/textures/moon_maps.rgba16f, a raw RGBA16F mip chain the app uploads as-is (no image decoding at launch,
docs/FREEZES.md):
  RGB  albedo in the old full_moon.png's units (its 8-bit values used as linear, as the shader always read them), one
       luminance scale so the near side's mean seen face-on equals the old texture's disc mean: the disc's brightness
       is unchanged, only its detail and the far side are new
  A    elevation in KILOMETRES (half float: ~4 m steps at 8 km)
Layout 2048 x 1024 (u = lon / 360 + 0.5, v = 0.5 - lat / 180), rows top (north) to bottom.

File: char[4] "SLMN" | uint32 width | uint32 height | uint32 mips | then the levels, each height x width x 4 float16
(little-endian), level 0 first. Downloads are cached under build/moon_cache/.

    python tools/make_moon_maps.py [--width 2048] [--preview out.png]
"""
import argparse
import os
import struct
import urllib.request

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CACHE = os.path.join(REPO, "build", "moon_cache")
OUT = os.path.join(REPO, "assets", "textures", "moon_maps.rgba16f")
OLD = os.path.join(REPO, "assets", "textures", "full_moon.png")
BASE = "https://svs.gsfc.nasa.gov/vis/a000000/a004700/a004720/"


def fetch(name):
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, name)
    if not os.path.exists(path):
        print("download", BASE + name)
        req = urllib.request.Request(BASE + name, headers={"User-Agent": "SatLightSim-bake"})
        with urllib.request.urlopen(req) as r, open(path + ".part", "wb") as f:
            f.write(r.read())
        os.replace(path + ".part", path)
    return path


def box_down(a):
    """2x2 box filter (width wraps, as longitude does: the sizes are powers of two)."""
    return 0.25 * (a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2])


def ortho_near_side(albedo, n=256):
    """The near side seen face-on (orthographic, north up, east = right of the image, i.e. as on the Earth's sky
    with north up: selenographic east is the sky's WEST, to the right)."""
    H, W = albedo.shape[:2]
    y, x = np.mgrid[0:n, 0:n]
    px = (x + 0.5) / n * 2 - 1
    py = 1 - (y + 0.5) / n * 2
    r2 = px * px + py * py
    inside = r2 < 1
    pz = np.sqrt(np.clip(1 - r2, 0, 1))
    lat = np.arcsin(np.clip(py, -1, 1))
    lon = np.arctan2(px, pz)                     # +x = east longitude
    u = ((lon / (2 * np.pi) + 0.5) * W).astype(int) % W
    v = np.clip(((0.5 - lat / np.pi) * H).astype(int), 0, H - 1)
    img = albedo[v, u]
    return img, inside


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--width", type=int, default=2048)
    ap.add_argument("--preview", default="")
    args = ap.parse_args()
    W = args.width
    H = W // 2

    col = Image.open(fetch("lroc_color_poles_4k.tif")).convert("RGB")
    dem = Image.open(fetch("ldem_16.tif"))
    albedo = np.asarray(col.resize((W, H), Image.BOX), dtype=np.float64) / 255.0
    elev = np.asarray(dem.resize((W, H), Image.BOX), dtype=np.float64)          # km

    # Orientation check: maria are low ground.
    lum = albedo.mean(2)
    band = slice(H // 4, 3 * H // 4)
    c_same = np.corrcoef(lum[band].ravel(), elev[band].ravel())[0, 1]
    c_flip = np.corrcoef(lum[band, ::-1].ravel(), elev[band].ravel())[0, 1]
    print(f"albedo/elevation correlation: {c_same:.3f} (mirrored {c_flip:.3f})")
    assert c_same > c_flip + 0.05, "albedo and elevation maps disagree on orientation"

    # Brightness: the near side's face-on mean = the old full-moon texture's disc mean (8-bit values as linear).
    old = np.asarray(Image.open(OLD).convert("RGB"), dtype=np.float64) / 255.0
    on = old.shape[0]
    yy, xx = np.mgrid[0:on, 0:on]
    oin = ((xx + 0.5) / on * 2 - 1) ** 2 + ((yy + 0.5) / on * 2 - 1) ** 2 < 1
    old_mean = old[oin].mean(0)
    near, nin = ortho_near_side(albedo)
    new_mean = near[nin].mean(0)
    k = float(old_mean.mean() / new_mean.mean())
    albedo *= k
    print(f"old disc mean {old_mean.round(4)}, new near side {new_mean.round(4)} -> scale {k:.4f}")
    print(f"elevation range {elev.min():.2f} .. {elev.max():.2f} km")
    if args.preview:
        near, nin = ortho_near_side(albedo, 512)
        Image.fromarray(np.clip(near * nin[..., None] * 255, 0, 255).astype(np.uint8)).save(args.preview)

    level = np.concatenate([albedo, elev[..., None]], axis=2).astype(np.float64)
    levels = [level]
    while levels[-1].shape[0] > 1 and levels[-1].shape[1] > 1:
        levels.append(box_down(levels[-1]))
    while levels[-1].shape[1] > 1:                    # height reached 1: finish the width
        a = levels[-1]
        levels.append(0.5 * (a[:, 0::2] + a[:, 1::2]))
    with open(OUT, "wb") as f:
        f.write(b"SLMN" + struct.pack("<III", W, H, len(levels)))
        for lv in levels:
            f.write(lv.astype("<f2").tobytes())
    print(f"wrote {OUT}: {W}x{H}, {len(levels)} mips, {os.path.getsize(OUT) / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
