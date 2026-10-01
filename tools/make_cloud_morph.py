#!/usr/bin/env python3
"""Cloud MORPHOLOGY textures for clouds v2 from REAL satellite imagery (review 18; the procedural set of
review 17 is tools/make_cloud_morph_procedural.py).

    python tools/make_cloud_morph.py [--size 1024] [--preview out.png] [--cache build/cloud_imagery]

Writes assets/textures/cloud_morph.rgba8: raw RGBA8, SIZE x SIZE, row-major, tiling seamlessly. One period of
the texture is CV2 "Morphology period" (default 320 km). Each channel is one MODIS true-colour scene, ~330 km
square (NASA GIBS, public domain; credit in THIRD_PARTY_NOTICES.txt), chosen for its cloud type:

  R  closed cells    stratocumulus deck off Peru         Aqua  2021-09-15  15 S  80 W
  G  open cells      cold-air outbreak, North Atlantic   Aqua  2022-02-05  58 N  20 W
  B  cloud streets   cold-air outbreak, Sea of Japan     Terra 2023-01-10  41 N 138.6 E
  A  clustered cu    popcorn cumulus over the Amazon     Aqua  2023-09-01   8 S  63 W (local background)

Per scene:
 1. the cloud signal: the DARKEST of the three linear channels (cloud is white; the sea is dark blue, forest
    dark green, rivers brown), minus the scene's clear background (its 5th percentile);
 2. + blurred copies (1.5 and 6 km) at a smaller weight: they order the CLEAR pixels by their distance from
    cloud, so a coverage above the scene's own grows the clouds outward instead of speckling the sea;
 3. scales past ~60 km are taken out (the weather map sets those) — and with them the scene's swath and
    sun-glint gradients;
 4. tiling: the periodic component of the periodic-plus-smooth decomposition (Moisan 2011, "Periodic plus
    smooth image decomposition"): every detail kept, no seam where the texture wraps;
 5. its clear stretches (no cloud within ~3 km) take a share of another scene's field, so they fill with real
    structure as the coverage rises, not featureless blobs;
 6. resampled to SIZE and rank-equalised to a UNIFORM distribution, so a threshold at 1 - coverage covers
    exactly that fraction (the shader turns it into the far field's normal deviate).
"""
import argparse
import math
import os
import sys
import urllib.request

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
URL = ("https://gibs.earthdata.nasa.gov/wms/epsg4326/best/wms.cgi?SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap"
       "&LAYERS={layer}&CRS=EPSG:4326&BBOX={s},{w},{n},{e}&WIDTH={px}&HEIGHT={px}&FORMAT=image/jpeg&TIME={date}")
LAYERS = {"T": "MODIS_Terra_CorrectedReflectance_TrueColor", "A": "MODIS_Aqua_CorrectedReflectance_TrueColor"}
SCENE_KM = 330.0
SCENES = [  # (channel, lat, lon, date, satellite, local background km: 0 = the scene's 5th percentile)
    ("closed cells", -15.0, -80.0, "2021-09-15", "A", 0.0),
    ("open cells", 58.0, -20.0, "2022-02-05", "A", 0.0),
    ("cloud streets", 41.0, 138.6, "2023-01-10", "T", 0.0),
    ("clustered cumulus", -8.0, -63.0, "2023-09-01", "A", 6.0),
]


def fetch(lat, lon, date, sat, px, cache):
    os.makedirs(cache, exist_ok=True)
    out = os.path.join(cache, f"gibs_{sat}_{lat}_{lon}_{date}_{px}.jpg")
    if not os.path.exists(out):
        dlat = SCENE_KM / 111.2
        dlon = dlat / math.cos(math.radians(lat))   # a square on the ground, not in degrees
        u = URL.format(layer=LAYERS[sat], s=lat - dlat / 2, n=lat + dlat / 2, w=lon - dlon / 2, e=lon + dlon / 2,
                       px=px, date=date)
        print(f"fetching {out}", file=sys.stderr)
        urllib.request.urlretrieve(u, out)
    return np.asarray(Image.open(out).convert("RGB")).astype(np.float64) / 255.0


def blur(f, sigma_px, periodic=False):
    """Gaussian blur by FFT; reflect-padded unless periodic."""
    if sigma_px <= 0:
        return f
    if not periodic:
        p = int(min(3 * sigma_px, f.shape[0] - 1))
        g = np.pad(f, p, mode="reflect")
    else:
        g, p = f, 0
    ky = np.fft.fftfreq(g.shape[0])[:, None]
    kx = np.fft.rfftfreq(g.shape[1])[None, :]
    h = np.exp(-2.0 * (np.pi * sigma_px) ** 2 * (kx ** 2 + ky ** 2))
    r = np.fft.irfft2(np.fft.rfft2(g) * h, s=g.shape)
    return r[p:p + f.shape[0], p:p + f.shape[1]] if p else r


def periodic_component(u):
    """Moisan's periodic + smooth decomposition: returns the periodic part of u."""
    M, N = u.shape
    v = np.zeros_like(u)
    v[0, :] += u[-1, :] - u[0, :]
    v[-1, :] += u[0, :] - u[-1, :]
    v[:, 0] += u[:, -1] - u[:, 0]
    v[:, -1] += u[:, 0] - u[:, -1]
    q = np.arange(M)[:, None]
    r = np.arange(N)[None, :]
    den = 2.0 * np.cos(2 * np.pi * q / M) + 2.0 * np.cos(2 * np.pi * r / N) - 4.0
    den[0, 0] = 1.0
    s = np.fft.fft2(v) / den
    s[0, 0] = 0.0
    return u - np.real(np.fft.ifft2(s))


def equalise(f):
    r = np.argsort(np.argsort(f.ravel(), kind="stable"), kind="stable").reshape(f.shape)
    return (r + 0.5) / f.size


def scene_field(rgb, size, bg_km=0.0):
    lin = np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4)
    c = lin.min(-1)
    kmpx = SCENE_KM / c.shape[0]
    if bg_km > 0.0:
        # A LOCAL background (a grey opening: min then max over bg_km, smoothed): the haze and smoke over land
        # and the forest's own texture go, only compact clouds stay. Not for decks (a deck is "background").
        from scipy import ndimage
        k = max(3, int(round(bg_km / kmpx)) | 1)
        b = ndimage.maximum_filter(ndimage.minimum_filter(c, size=k, mode="reflect"), size=k, mode="reflect")
        c = np.maximum(c - blur(b, k / 3.0), 0.0)
    else:
        c = np.maximum(c - np.percentile(c, 5), 0.0)
    # Clear ground is exactly 0 (half the Otsu threshold off the signal): what is left of the sea's glint, the
    # forest's texture and smoke plumes otherwise ordered the clear pixels instead of the distance to cloud.
    h, e = np.histogram(c, bins=256)
    m = (e[:-1] + e[1:]) / 2
    w0 = np.cumsum(h); w1 = w0[-1] - w0
    m0 = np.cumsum(h * m) / np.maximum(w0, 1); m1 = ((h * m).sum() - np.cumsum(h * m)) / np.maximum(w1, 1)
    c = np.maximum(c - 0.5 * m[np.argmax(w0 * w1 * (m0 - m1) ** 2)], 0.0)
    v = c + 0.5 * blur(c, 1.5 / kmpx) + 0.3 * blur(c, 6.0 / kmpx)
    v = periodic_component(v)
    # Locally standardised over ~30 km (the mean and spread about each point, the spread floored): a scene's
    # own density varies over ~100 km, and globally ranked those regions filled in whole as the coverage
    # rose (blocks of solid cloud). The weather map sets coverage at those scales; this texture only says how
    # the cloud is arranged inside them. On the torus (after the decomposition), so the tiling holds.
    sg = 30.0 / kmpx
    mu = blur(v, sg, periodic=True)
    sd = np.sqrt(np.maximum(blur((v - mu) ** 2, sg, periodic=True), 0.0))
    v = (v - mu) / np.maximum(sd, 0.6 * sd.mean())
    # Where the scene has no cloud within a few km (its clear stretches), the field is the smooth distance
    # term alone, and as the coverage rose those stretches filled as featureless blobs: the share of another
    # scene's imagery mixed in there (main) gives them real cloud structure instead.
    near = periodic_component(blur((c > 0).astype(np.float64), 3.0 / kmpx))
    rs = lambda f: np.asarray(Image.fromarray(f.astype(np.float32), mode="F").resize((size, size), Image.LANCZOS),
                              dtype=np.float64)
    return rs(v), np.clip(1.0 - rs(near) / 0.3, 0.0, 1.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--size", type=int, default=1024)
    ap.add_argument("--fetch-px", type=int, default=1280, help="pixels per scene side fetched (~260 m)")
    ap.add_argument("--cache", default=os.path.join(ROOT, "build", "cloud_imagery"))
    ap.add_argument("--preview", help="also write a PNG preview (the four channels side by side, tiled 2x2)")
    a = ap.parse_args()
    fields = []
    for name, lat, lon, date, sat, bg in SCENES:
        fields.append(scene_field(fetch(lat, lon, date, sat, a.fetch_px, a.cache), a.size, bg))
        print(f"{name}: done", file=sys.stderr)
    chans = []
    for i, (v, clear) in enumerate(fields):
        # The clear stretches take the clustered cumulus (the cumulus channel the closed cells), rolled half a
        # period so the two never line up.
        fill = np.roll(fields[0 if i == 3 else 3][0], (a.size // 2, a.size // 2), (0, 1))
        u = equalise(v + 0.8 * clear * fill)
        chans.append(np.clip(np.round(u * 255.0), 0, 255).astype(np.uint8))
    out = np.stack(chans, -1)
    path = os.path.join(ROOT, "assets", "textures", "cloud_morph.rgba8")
    out.tofile(path)
    print(f"wrote {path} ({a.size}x{a.size} RGBA8, {out.nbytes} bytes)")
    if a.preview:
        tiles = [np.tile(c, (2, 2))[:: 2, :: 2] for c in chans]
        Image.fromarray(np.concatenate(tiles, axis=1)).save(a.preview)
        print(f"preview {a.preview}")


if __name__ == "__main__":
    main()
