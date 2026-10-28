"""Bake the resolved planets' surface maps (planet_disc.frag, CLAUDE.md "Planets").

Sources (Solar System Scope, https://www.solarsystemscope.com/textures/ — CC BY 4.0, based on NASA imagery; credited in
THIRD_PARTY_NOTICES.txt and the Attributions tab):
  2k_mars.jpg     Mars (Viking-era colour mosaic), east longitude: Syrtis Major (69 E) checked at u = 0.5 + 69/360
  2k_jupiter.jpg  Jupiter (Cassini global map): belts, zones and the Great Red Spot (its column is measured here and
                  written to stdout; the app puts it at the GRS's System II longitude for the date)
  2k_saturn.jpg   Saturn's ball (bands)
Layer 3 is Saturn's RINGS: a radial profile made here from the known structure (C ring, B ring, Cassini division, A
ring with the Encke and Keeler gaps; radii from the PDS ring-plane radii, optical depths from Cassini occultations,
rounded), with fine ripples, so no licence is involved.

Every layer is equirectangular, u = 0.5 + east longitude / 360 (planetary east to the right, north up), v = 0.5 - lat /
180, rows north to south. Stored as the square root of (linear / the layer's own
area-weighted mean luminance / 8): the shader squares and x 8 (0 .. 8x the mean; Mars's polar caps are ~5x its mean): the shader takes the disc's brightness from the planet's magnitude, and
the map only says where it is brighter or darker. The ring layer: RGB = the particles' albedo x colour, A = the normal
optical depth / 4 (u = (r - 1.10) / 1.25 in Saturn equatorial radii; every row the same).

File: char[4] "SLPL" | uint32 width | height | layers | mips | then per level, per layer, height x width x 4 bytes.
Downloads are cached under build/planet_cache/.

    python tools/make_planet_maps.py [--width 1024] [--preview out.png]
"""
import argparse
import os
import struct
import urllib.request

import numpy as np
from PIL import Image

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CACHE = os.path.join(REPO, "build", "planet_cache")
OUT = os.path.join(REPO, "assets", "textures", "planet_maps.rgba8")
BASE = "https://www.solarsystemscope.com/textures/download/"
MAPS = ["2k_mars.jpg", "2k_jupiter.jpg", "2k_saturn.jpg"]

RING_R0, RING_R1 = 1.10, 2.35   # the ring layer's span, Saturn equatorial radii


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


def srgb_to_lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def box_down(a):
    return 0.25 * (a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2])


def planet_layer(name, W, H):
    img = Image.open(fetch(name)).convert("RGB")
    lin = srgb_to_lin(np.asarray(img, dtype=np.float64) / 255.0)
    # Down in linear light (a box of whole source texels: the sources are 2048 x 1024).
    f = lin.shape[1] // W
    lin = lin[: H * f, : W * f].reshape(H, f, W, f, 3).mean(axis=(1, 3))
    lat = (0.5 - (np.arange(H) + 0.5) / H) * np.pi
    w = np.cos(lat)[:, None]
    lum = lin @ np.array([0.2126, 0.7152, 0.0722])
    mean = float((lum * w).sum() / (w.sum() * W))
    out = np.sqrt(np.clip(lin / mean / 8.0, 0.0, 1.0))
    clipped = float((lin / mean / 8.0 > 1.0).mean())
    print(f"{name}: mean luminance {mean:.4f}, clipped {clipped * 100:.3f}% of texels")
    return out, lin


def ring_layer(W, H):
    r = RING_R0 + (np.arange(W) + 0.5) / W * (RING_R1 - RING_R0)
    tau = np.zeros(W)
    alb = np.zeros(W)
    rng = np.random.default_rng(7)
    # Fine radial structure: a few octaves of smooth noise (ringlets, plateaus), deterministic.
    def ripples(scale, n):
        x = np.linspace(0, n, W)
        k = rng.standard_normal(int(n) + 3)
        i = np.floor(x).astype(int)
        t = x - i
        t = t * t * (3 - 2 * t)
        return k[i] * (1 - t) + k[i + 1] * t
    fine = 0.55 * ripples(1, 220) + 0.3 * ripples(1, 600) + 0.15 * ripples(1, 1400)
    C = (r >= 1.239) & (r < 1.527)
    B = (r >= 1.527) & (r < 1.951)
    CD = (r >= 1.951) & (r < 2.025)
    A = (r >= 2.025) & (r < 2.267)
    tau[C] = 0.08 + 0.05 * np.clip(fine[C], -1, 1)
    alb[C] = 0.22
    # B ring: optically thick, thickest (and brightest) in its outer half.
    xb = (r[B] - 1.527) / (1.951 - 1.527)
    tau[B] = 0.9 + 2.4 * np.clip(xb * 1.6, 0, 1) + 0.4 * fine[B]
    alb[B] = 0.52 + 0.08 * np.clip(xb, 0, 1)
    tau[CD] = 0.10 + 0.04 * fine[CD]
    alb[CD] = 0.25
    xa = (r[A] - 2.025) / (2.267 - 2.025)
    tau[A] = 0.65 - 0.25 * xa + 0.08 * fine[A]
    alb[A] = 0.48
    for g0, g1 in [(2.212, 2.218), (2.263, 2.2655)]:   # Encke, Keeler gaps
        m = (r >= g0) & (r < g1)
        tau[m] = 0.0
    # The Huygens ringlet region and the Cassini division's ramps: soft edges (one texel)
    tau = np.clip(tau, 0.0, 4.0)
    col = np.stack([alb * 1.0, alb * 0.90, alb * 0.76], axis=1)
    row = np.concatenate([np.clip(col, 0, 1), (tau / 4.0)[:, None]], axis=1)
    return np.broadcast_to(row[None], (H, W, 4)).copy()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--width", type=int, default=1024)
    ap.add_argument("--preview", default="")
    args = ap.parse_args()
    W = args.width
    H = W // 2
    layers = []
    for name in MAPS:
        out, lin = planet_layer(name, W, H)
        if name == "2k_jupiter.jpg":
            # The Great Red Spot: the reddest stretch of the 18-26 S band.
            y0, y1 = int((0.5 + 18 / 180) * H), int((0.5 + 26 / 180) * H)
            red = (lin[y0:y1, :, 0] - lin[y0:y1, :, 2]).mean(0)
            red = np.convolve(np.concatenate([red, red[: W // 20]]), np.ones(W // 20) / (W // 20), "valid")[:W]
            print(f"jupiter: Great Red Spot centre at u = {(np.argmax(red) + W // 40) / W % 1.0:.4f}")
        if name == "2k_mars.jpg":
            eq = lin[int(0.45 * H):int(0.5 * H)].mean((0, 2))
            eq = np.convolve(np.concatenate([eq, eq[: W // 30]]), np.ones(W // 30) / (W // 30), "valid")[:W]
            print(f"mars: darkest equatorial stretch at u = {(np.argmin(eq) + W // 60) / W % 1.0:.4f} (Syrtis Major 69 E -> 0.692)")
        a = np.concatenate([out, np.ones((H, W, 1))], axis=2)
        layers.append(a)
    layers.append(ring_layer(W, H))
    levels = [np.stack(layers)]
    while levels[-1].shape[1] > 1:
        a = levels[-1]
        levels.append(0.25 * (a[:, 0::2, 0::2] + a[:, 1::2, 0::2] + a[:, 0::2, 1::2] + a[:, 1::2, 1::2]))
    while levels[-1].shape[2] > 1:
        a = levels[-1]
        levels.append(0.5 * (a[:, :, 0::2] + a[:, :, 1::2]))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(b"SLPL" + struct.pack("<IIII", W, H, len(layers), len(levels)))
        for lv in levels:
            f.write(np.clip(np.round(lv * 255.0), 0, 255).astype(np.uint8).tobytes())
    print(f"wrote {OUT}: {W}x{H} x {len(layers)} layers, {len(levels)} mips, {os.path.getsize(OUT) / 1e6:.1f} MB")
    if args.preview:
        prev = np.concatenate([np.clip(l[..., :3] ** 2 * 2, 0, 1) for l in layers[:3]] + [np.clip(layers[3][..., :3], 0, 1)], axis=0)
        Image.fromarray((prev ** (1 / 2.2) * 255).astype(np.uint8)).save(args.preview)


if __name__ == "__main__":
    main()
