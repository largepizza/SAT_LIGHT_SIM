"""Bake the major-road network for the procedural city lights (.plans/CITIES_PLAN.md, phase 1).

Source: Natural Earth 10m roads (public domain, https://www.naturalearthdata.com), cached in
build/city_cache/. Output: assets/textures/city_roads.bin, read by SatelliteSim.cpp into ONE storage
buffer (sky binding 28) that sat_sky.frag indexes as uint words:

  word 0..3   "SLRD" magic (0x44524c53), grid width W, grid height H, segment count N
  word 4..    cell offsets, W*H + 1 words: cell c's segment indices are idx[off[c] .. off[c+1])
  then        idx, off[W*H] words (segment numbers)
  then        segments, 8 words each: a.xyz (float ECEF, metres, on the sea-level sphere), class,
              b.xyz, width (m)

Cells are an equirectangular lat/lon grid (row 0 at the north pole, column 0 at -180 deg, like the
Earth maps). A segment is listed in every cell its bounding box, grown by kReachM, touches, so a
shader lookup at a point sees every segment whose light can reach it. Ferries and tracks are dropped.
Classes: 0 = road/unknown, 1 = secondary highway, 2 = major highway / beltway / bypass.

    python tools/make_city_roads.py
"""
import io
import math
import os
import struct
import urllib.request
import zipfile

import numpy as np
import shapefile  # pyshp

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CACHE = os.path.join(REPO, "build", "city_cache")
OUT = os.path.join(REPO, "assets", "textures", "city_roads.bin")
URL = "https://naciscdn.org/naturalearth/10m/cultural/ne_10m_roads.zip"

R_EARTH = 6371000.0          # common.glsl
GRID_DEG = 0.125       # 0.25 put ~40 segments in an LA cell: 0.47 ms of road light per frame (2026-09-29)
K_REACH_M = 2000.0
CLASS_OF = {"Major Highway": 2, "Beltway": 2, "Bypass": 2, "Secondary Highway": 1, "Road": 0, "Unknown": 0}
WIDTH_OF = {0: 12.0, 1: 18.0, 2: 30.0}


def ecef(lat, lon):
    la, lo = math.radians(lat), math.radians(lon)
    return (R_EARTH * math.cos(la) * math.cos(lo), R_EARTH * math.cos(la) * math.sin(lo), R_EARTH * math.sin(la))


def main():
    os.makedirs(CACHE, exist_ok=True)
    zpath = os.path.join(CACHE, "ne_10m_roads.zip")
    if not os.path.exists(zpath):
        print("download", URL)
        with urllib.request.urlopen(URL) as r, open(zpath, "wb") as f:
            f.write(r.read())
    z = zipfile.ZipFile(zpath)
    rd = shapefile.Reader(shp=io.BytesIO(z.read("ne_10m_roads.shp")), dbf=io.BytesIO(z.read("ne_10m_roads.dbf")),
                          shx=io.BytesIO(z.read("ne_10m_roads.shx")))
    W, H = int(round(360 / GRID_DEG)), int(round(180 / GRID_DEG))
    segs = []
    cells = [[] for _ in range(W * H)]
    for sr in rd.iterShapeRecords():
        t = sr.record["type"]
        if t not in CLASS_OF:
            continue
        cls = CLASS_OF[t]
        pts = sr.shape.points
        parts = list(sr.shape.parts) + [len(pts)]
        for pi in range(len(parts) - 1):
            ring = pts[parts[pi]:parts[pi + 1]]
            for (x0, y0), (x1, y1) in zip(ring[:-1], ring[1:]):
                if abs(x1 - x0) > 180:          # antimeridian wrap: skip the (rare) crossing segment
                    continue
                n = len(segs)
                segs.append((ecef(y0, x0), ecef(y1, x1), cls))
                # bbox in degrees, grown by the reach (longitude by 1/cos(lat))
                la_pad = math.degrees(K_REACH_M / R_EARTH)
                lat_c = max(abs(y0), abs(y1))
                lo_pad = la_pad / max(math.cos(math.radians(min(lat_c + la_pad, 89.0))), 0.02)
                la0, la1 = min(y0, y1) - la_pad, max(y0, y1) + la_pad
                lo0, lo1 = min(x0, x1) - lo_pad, max(x0, x1) + lo_pad
                r0 = max(0, int(math.floor((90 - la1) / GRID_DEG)))
                r1 = min(H - 1, int(math.floor((90 - la0) / GRID_DEG)))
                c0 = int(math.floor((lo0 + 180) / GRID_DEG))
                c1 = int(math.floor((lo1 + 180) / GRID_DEG))
                for r in range(r0, r1 + 1):
                    for c in range(c0, c1 + 1):
                        cells[r * W + (c % W)].append(n)
    off = np.zeros(W * H + 1, np.uint32)
    counts = np.array([len(c) for c in cells], np.uint32)
    off[1:] = np.cumsum(counts)
    idx = np.fromiter((i for c in cells for i in c), np.uint32, count=int(off[-1]))
    seg = np.zeros((len(segs), 8), np.float32)
    for i, (a, b, cls) in enumerate(segs):
        seg[i, 0:3] = a
        seg[i, 3] = cls
        seg[i, 4:7] = b
        seg[i, 7] = WIDTH_OF[cls]
    with open(OUT, "wb") as f:
        f.write(struct.pack("<4I", 0x44524C53, W, H, len(segs)))
        f.write(off.tobytes())
        f.write(idx.tobytes())
        f.write(seg.tobytes())
    nz = counts[counts > 0]
    print(f"{len(segs)} segments, {len(idx)} cell entries, cells with roads {len(nz)} "
          f"(max {nz.max()}, p99 {int(np.percentile(nz, 99))}, mean {nz.mean():.1f})")
    print(f"{os.path.relpath(OUT, REPO)}: {os.path.getsize(OUT) / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
