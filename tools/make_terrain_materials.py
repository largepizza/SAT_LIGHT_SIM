"""Bake the close-up terrain material array (terrain v2 P3, .plans/TERRAIN_V2_PLAN.md).

Downloads CC0 texture sets from ambientCG (https://ambientcg.com, CC0 1.0 — no attribution required,
credited anyway in THIRD_PARTY_NOTICES.txt), caches the zips under build/terrain_material_cache/, and
writes assets/textures/terrain_materials.rgba8: a raw RGBA8 2D array the app uploads as-is (no image
decoding at launch — docs/FREEZES.md). Two layers per material, in MATERIALS order:

  layer 2m     albedo as a RATIO to the material's own mean colour (x 0.4, so 0.4 = the mean; the
               renderer multiplies the day map's colour by 2.5 x this: the day map stays the authority on
               what colour the ground is, the texture adds its structure), A = height (displacement)
  layer 2m+1   RG = tangent-space normal XY (OpenGL convention, 0.5 = flat), B = roughness, A = AO

File: char[4] "SLTA" | uint32 width | uint32 height | uint32 layers | then layers x height x width x 4
bytes, rows top to bottom (little-endian header).

    python tools/make_terrain_materials.py [--size 1024]
"""
import argparse
import io
import os
import struct
import urllib.request
import zipfile

import numpy as np
from PIL import Image

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CACHE = os.path.join(REPO, "build", "terrain_material_cache")
OUT = os.path.join(REPO, "assets", "textures", "terrain_materials.rgba8")

# Order = the material index in sat_sky.frag (kMat*). Keep them in step.
MATERIALS = [
    ("grass", "Grass004"),
    ("forest", "Ground037"),
    ("rock", "Rock051"),
    ("snow", "Snow010A"),
    ("sand", "Ground054"),
    ("dirt", "Ground109"),
]


def fetch(asset):
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, f"{asset}_1K-JPG.zip")
    if not os.path.exists(path):
        url = f"https://ambientcg.com/get?file={asset}_1K-JPG.zip"
        print("download", url)
        req = urllib.request.Request(url, headers={"User-Agent": "SatLightSim-bake"})
        with urllib.request.urlopen(req) as r, open(path, "wb") as f:
            f.write(r.read())
    return zipfile.ZipFile(path)


def member(z, suffix):
    for n in z.namelist():
        if n.lower().endswith(suffix.lower()):
            return Image.open(io.BytesIO(z.read(n)))
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=1024)
    a = ap.parse_args()
    S = a.size
    layers = []
    for name, asset in MATERIALS:
        z = fetch(asset)
        col = member(z, "_Color.jpg")
        nrm = member(z, "_NormalGL.jpg")
        dsp = member(z, "_Displacement.jpg")
        rgh = member(z, "_Roughness.jpg")
        ao = member(z, "_AmbientOcclusion.jpg")
        if col is None or nrm is None:
            raise SystemExit(f"{asset}: missing Color or NormalGL in {z.namelist()}")

        def load(im, mode, default):
            if im is None:
                return np.full((S, S) if mode == "L" else (S, S, 3), default, np.float32)
            return np.asarray(im.convert(mode).resize((S, S), Image.LANCZOS), np.float32) / 255.0

        c = load(col, "RGB", 0.5)
        lin = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
        mean = lin.reshape(-1, 3).mean(0)
        ratio = np.clip(lin / mean * 0.4, 0.0, 1.0)          # linear ratio x 0.4
        h = load(dsp, "L", 0.5)
        n = load(nrm, "RGB", 0.5)
        r = load(rgh, "L", 0.8)
        o = load(ao, "L", 1.0)
        l0 = np.dstack([ratio, h])
        l1 = np.dstack([n[..., 0], n[..., 1], r, o])
        layers += [l0, l1]
        clip = (lin / mean * 0.4 > 1.0).mean() * 100
        print(f"{name:7s} {asset:10s} mean linear albedo {mean.round(3)}  ratio clipped {clip:.2f}%")
    data = np.clip(np.round(np.stack(layers) * 255.0), 0, 255).astype(np.uint8)
    with open(OUT, "wb") as f:
        f.write(b"SLTA" + struct.pack("<III", S, S, data.shape[0]))
        f.write(data.tobytes())
    print(f"{os.path.relpath(OUT, REPO)}: {data.shape[0]} layers {S}x{S}, {os.path.getsize(OUT) / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
