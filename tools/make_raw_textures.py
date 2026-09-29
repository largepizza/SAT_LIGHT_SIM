"""Pre-decode the launch's single-channel PNG textures into raw R8 files the app reads with no decoding.

Why (docs/FREEZES.md, 2026-09-29): stb_image decoding earth_elevation.png freezes the development
machine within a few decodes (tools/upload_stress --decode-only reproduces it with no Vulkan at all),
and the launch froze on the PNG steps more than any other. PIL decodes these files safely, so the
decode happens here, once, and the app (SatelliteSim.cpp, readRawR8) just reads bytes. The PNGs stay
the source and the app's fallback when a .r8 file is missing.

Format, per output file: a 16-byte header followed by its rows, top to bottom, one byte per texel:
    char[4] "SLR8" | uint32 width | uint32 total height | uint32 first row      (little-endian)
A map larger than GitHub's 100 MB file limit is split into consecutive row bands (_0.r8, _1.r8, ...).
Values are the PNG's exactly (for the DEM: sea level = 15, CLAUDE.md "Elevation texture encoding").
Re-run after editing a source PNG:
    python tools/make_raw_textures.py
"""
import os
import struct
import sys

from PIL import Image, ImageChops

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TEX = os.path.join(REPO, "assets", "textures")

# (source PNG, output stem, parts) — parts > 1 writes <stem>_<i>.r8, else <stem>.r8.
TEXTURES = [
    ("earth_elevation.png", "earth_elevation", 2),          # 14999x7500 = 112.5 MB
    ("8k_earth_specular_map.png", "8k_earth_specular_map", 1),  # 8192x4096 = 33.5 MB
]

Image.MAX_IMAGE_PIXELS = None  # the DEM is 112 Mpx, past PIL's decompression-bomb guard


def grey_of(im, name):
    """The single channel stbi_load(..., 1) returned: (77r + 150g + 29b) >> 8, i.e. r when grey."""
    if im.mode == "L":
        return im
    if im.mode in ("RGB", "RGBA"):
        r, g, b = im.getchannel("R"), im.getchannel("G"), im.getchannel("B")
        if ImageChops.difference(r, g).getbbox() or ImageChops.difference(r, b).getbbox():
            sys.exit(f"{name} is not grey (R, G, B differ): decide the conversion first")
        return r
    sys.exit(f"{name}: unexpected PNG mode {im.mode}")


def convert(src, stem, parts):
    im = Image.open(os.path.join(TEX, src))
    im.load()
    grey = grey_of(im, src)
    w, h = grey.size
    data = grey.tobytes()
    bounds = [h * i // parts for i in range(parts + 1)]
    paths = [os.path.join(TEX, f"{stem}_{i}.r8" if parts > 1 else f"{stem}.r8") for i in range(parts)]
    for i, path in enumerate(paths):
        y0, y1 = bounds[i], bounds[i + 1]
        with open(path, "wb") as f:
            f.write(b"SLR8" + struct.pack("<III", w, h, y0))
            f.write(data[y0 * w:y1 * w])
        print(f"{os.path.relpath(path, REPO)}: rows {y0}-{y1 - 1}, {os.path.getsize(path) / 1e6:.1f} MB")

    back = bytearray()
    for i, path in enumerate(paths):
        with open(path, "rb") as f:
            magic, ww, hh, y0 = struct.unpack("<4sIII", f.read(16))
            assert magic == b"SLR8" and ww == w and hh == h and y0 == bounds[i]
            back += f.read()
    assert bytes(back) == data, f"{src}: round trip differs"
    print(f"verified {src}: {w}x{h}, identical")


def main():
    for src, stem, parts in TEXTURES:
        convert(src, stem, parts)


if __name__ == "__main__":
    main()
