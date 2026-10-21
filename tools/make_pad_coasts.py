"""Bake the coastlines around the coastal launch sites: data/launch_coasts.json.

2026-10-20 (pad polish). The water map (tools/make_water_map.py) is an 8K signed distance field, ~5 km a texel: it
cannot hold the narrow land and water around most launch sites — Boca Chica's strip between the Gulf and the bay
behind it, Cape Canaveral's barrier island between the Atlantic and the Banana River — and the DEM reads 0 m on that
low coastal land, so the 5-km day map's water colour was drawn on it: from the ground at Starbase the foreground was
"sea" and the pads read as islands. This tool bakes the Natural Earth 10 m coastline (the same source
make_water_map.py rasterises, cached in build/ne_cache/) around each group of coastal sites into a local signed
distance grid: GRID_N x GRID_N cells over the patch (radius <= MAX_RADIUS_KM, so a cell is <= ~420 m against the map's
~5 km), metres to the shore, > 0 in water, every pad's graded disc (PAD_LAND_M) forced to land, clamped to +-SDF_MAX_M
and stored as bytes (127.5 + d / SDF_MAX_M x 127.5). Inside / outside comes from the whole Natural Earth rings
(crossing count), the distance from their edges. The renderer (terrain_detail.glsl tdCoastPatch) takes its shoreline
from the grid inside the patch, lifts the land to at least LAND_FLOOR_M (land at 0 m is drawn like the sea), and
draws the land with the patch's ground colour where the day map's texel is water (sat_sky.frag).

Only sites with coast within COAST_REACH_KM get a patch; sites within MERGE_KM share one. The pads' radii match
SatelliteSimLaunches.cpp buildPads (flatR: 450 m Starship, 350 m others). Needs numpy, pyshp.
    python tools/make_pad_coasts.py
"""
import base64
import io
import json
import math
import os
import zipfile

import numpy as np
import shapefile

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
NE = [os.path.join(REPO, "build", "ne_cache"), os.path.join(REPO, "..", "..", "..", "build", "ne_cache")]
R_EARTH = 6371000.0
COAST_REACH_KM = 10.0   # a site needs coast this near to get a patch
MERGE_KM = 6.0          # sites this close share one
MARGIN_KM = 6.0         # the patch reaches this far past its farthest site
MAX_RADIUS_KM = 10.0
GRID_N = 48             # == kCoastGridN (SatelliteSim.h) / COAST_GRID_N (cloud_params.glsl)
SDF_MAX_M = 3000.0      # == COAST_SDF_MAX_M (terrain_detail.glsl)
LAND_FLOOR_M = 2.5
STARSHIP_PAD_M = 450.0  # the pad's graded disc (buildPads' flatR): Starship pads, others 350 m

# The ground's colour (linear albedo, the day map's units) where the day map shows water on land. Hand-picked from
# what the sites are: Boca Chica's sand flats and coastal prairie, the Cape's scrub and marsh, the Louisiana marsh.
LAND_COLOR = {
    "starbase": [0.30, 0.27, 0.20],
    "ksc": [0.11, 0.14, 0.08], "ccsfs": [0.11, 0.14, 0.08],
    "louisiana": [0.12, 0.15, 0.08],
    "vandenberg": [0.20, 0.19, 0.13],
}
DEFAULT_COLOR = [0.14, 0.16, 0.10]


def ne_reader(name):
    for d in NE:
        p = os.path.join(d, name + ".zip")
        if os.path.exists(p):
            z = zipfile.ZipFile(p)
            f = {n.rsplit(".", 1)[1]: io.BytesIO(z.read(n)) for n in z.namelist() if n.startswith(name)}
            return shapefile.Reader(shp=f["shp"], shx=f["shx"], dbf=f["dbf"])
    raise SystemExit(f"{name}.zip not found in build/ne_cache (run tools/make_water_map.py once to download it)")


def rings():
    """Every ring as (bbox, [(lat, lon)], is_lake)."""
    out = []
    for name, lake in (("ne_10m_land", False), ("ne_10m_lakes", True)):
        r = ne_reader(name)
        for sh in r.iterShapes():
            parts = list(sh.parts) + [len(sh.points)]
            for i in range(len(parts) - 1):
                pts = sh.points[parts[i]:parts[i + 1]]
                lons = [p[0] for p in pts]
                lats = [p[1] for p in pts]
                out.append(((min(lons), min(lats), max(lons), max(lats)), [(p[1], p[0]) for p in pts], lake))
    return out


def local(lat0, lon0, lat, lon):
    cl = math.cos(math.radians(lat0))
    dl = (lon - lon0 + 540.0) % 360.0 - 180.0
    return (math.radians(dl) * cl * R_EARTH, math.radians(lat - lat0) * R_EARTH)


def sdf_grid(lat0, lon0, R, rg, pads):
    """GRID_N^2 signed distances (m, > 0 water) at the cell centres of the square [-R, R]^2 (x east, y north)."""
    c = (np.arange(GRID_N) + 0.5) / GRID_N * 2.0 * R - R
    X, Y = np.meshgrid(c, c)          # [row = y (south to north), col = x (west to east)]
    dist = np.full(X.shape, 1e30)
    land = np.zeros(X.shape, bool)
    lake = np.zeros(X.shape, bool)
    reach = math.degrees((R + 3.0 * SDF_MAX_M) / R_EARTH)
    reachLon = reach / max(math.cos(math.radians(lat0)), 0.2)
    for bb, pts, isLake in rg:
        if bb[0] > lon0 + reachLon or bb[2] < lon0 - reachLon or bb[1] > lat0 + reach or bb[3] < lat0 - reach:
            continue
        P = np.array([local(lat0, lon0, a, b) for a, b in pts])
        A, B = P[:-1], P[1:]
        # Crossing count (a ray toward +x) over the WHOLE ring: inside the ring or not.
        ymin, ymax = Y.min(), Y.max()
        span = ~(((A[:, 1] > ymax) & (B[:, 1] > ymax)) | ((A[:, 1] < ymin) & (B[:, 1] < ymin)))
        inside = np.zeros(X.shape, bool)
        for (x0, y0), (x1, y1) in zip(A[span], B[span]):
            cond = (y0 > Y) != (y1 > Y)
            xi = x0 + (Y - y0) * (x1 - x0) / ((y1 - y0) if y1 != y0 else 1e-9)
            inside ^= cond & (X < xi)
        if isLake:
            lake |= inside
        else:
            land |= inside
        # Distance to the ring's edges near the grid.
        m = SDF_MAX_M
        near = (np.minimum(A[:, 0], B[:, 0]) < R + m) & (np.maximum(A[:, 0], B[:, 0]) > -R - m) & \
               (np.minimum(A[:, 1], B[:, 1]) < R + m) & (np.maximum(A[:, 1], B[:, 1]) > -R - m)
        for (x0, y0), (x1, y1) in zip(A[near], B[near]):
            dx, dy = x1 - x0, y1 - y0
            t = np.clip(((X - x0) * dx + (Y - y0) * dy) / (dx * dx + dy * dy + 1e-9), 0.0, 1.0)
            dist = np.minimum(dist, np.hypot(X - x0 - t * dx, Y - y0 - t * dy))
    isLand = land & ~lake
    d = np.where(isLand, -dist, dist)
    for (px, py, r) in pads:   # the pads' graded discs are land
        d = np.minimum(d, np.hypot(X - px, Y - py) - r)
    return np.clip(d, -SDF_MAX_M, SDF_MAX_M)


def main():
    lj = json.load(open(os.path.join(REPO, "data", "launches.json"), encoding="utf-8"))
    sites = lj["sites"]
    # The Starship pads (buildPads: kind 2, a 450-m graded disc): every site Starship flies from.
    starship_sites = set()
    for v in lj["vehicles"]:
        if v["id"] == "starship":
            starship_sites.update(v["sites"])
    rg = rings()

    def coast_dist(lat, lon):
        best = 1e30
        for bb, pts, _ in rg:
            if bb[0] > lon + 0.3 or bb[2] < lon - 0.3 or bb[1] > lat + 0.3 or bb[3] < lat - 0.3:
                continue
            P = [local(lat, lon, a, b) for a, b in pts]
            for (x0, y0), (x1, y1) in zip(P, P[1:]):
                dx, dy = x1 - x0, y1 - y0
                t = max(0.0, min(1.0, -(x0 * dx + y0 * dy) / (dx * dx + dy * dy + 1e-9)))
                best = min(best, math.hypot(x0 + t * dx, y0 + t * dy))
        return best

    coastal = []
    for s in sites:
        d = coast_dist(s["lat"], s["lon"])
        if d < COAST_REACH_KM * 1000.0:
            coastal.append(s)
        print(f"{s['id']:22s} coast {d / 1000.0:7.2f} km{'' if d < COAST_REACH_KM * 1000 else '  (no patch)'}")
    groups = []
    # (2026-10-20, the deployment era's pad expansion) a group never spans more than a patch can hold: the 20 Louisiana
    # pads in a row (~27 km) would have shared one 10-km patch and left its ends uncovered.
    span = 2.0 * (MAX_RADIUS_KM - MARGIN_KM) * 1000.0
    for s in coastal:
        hit = [g for g in groups
               if any(math.hypot(*local(s["lat"], s["lon"], o["lat"], o["lon"])) < MERGE_KM * 1000.0 for o in g)
               and all(math.hypot(*local(s["lat"], s["lon"], o["lat"], o["lon"])) < span for o in g)]
        hit = hit[:1]
        if hit:
            g0 = hit[0]
            for g in hit[1:]:
                g0.extend(g)
                groups.remove(g)
            g0.append(s)
        else:
            groups.append([s])
    patches = []
    for g in groups:
        lat0 = sum(s["lat"] for s in g) / len(g)
        lon0 = sum(s["lon"] for s in g) / len(g)
        far = max(math.hypot(*local(lat0, lon0, s["lat"], s["lon"])) for s in g)
        R = min(far + MARGIN_KM * 1000.0, MAX_RADIUS_KM * 1000.0)
        pads = []
        for s in sites:   # every pad inside the patch, as buildPads sizes it (Starship: Starbase, Louisiana, 39A)
            x, y = local(lat0, lon0, s["lat"], s["lon"])
            if math.hypot(x, y) < R + 500.0:
                big = s["id"] in starship_sites or s.get("pad_kind") == 2
                pads.append((x, y, STARSHIP_PAD_M if big else 350.0))
        d = sdf_grid(lat0, lon0, R, rg, pads)
        b = np.clip(np.round(127.5 + d / SDF_MAX_M * 127.5), 0, 255).astype(np.uint8)
        key = g[0]["id"].split("_")[0]
        col = LAND_COLOR.get(key, DEFAULT_COLOR)
        water = float((d > 0).mean())
        patches.append({"name": g[0]["name"], "sites": [s["id"] for s in g], "lat": round(lat0, 6),
                        "lon": round(lon0, 6), "radius_m": round(R), "land_color": col, "land_floor_m": LAND_FLOOR_M,
                        "grid_n": GRID_N, "sdf_max_m": SDF_MAX_M,
                        "sdf_b64": base64.b64encode(b.tobytes()).decode("ascii")})
        print(f"patch {g[0]['id']:16s} {len(g)} sites, R {R / 1000:.1f} km, cell {2 * R / GRID_N:.0f} m, "
              f"water {water:.0%}")
        for r in range(GRID_N - 1, -1, -2):
            print("   " + "".join("~" if d[r, cc] > 0 else ("#" if d[r, cc] > -400 else ".") for cc in range(GRID_N)))
    out = {"format": "sat-light-sim-launch-coasts/1",
           "source": "Natural Earth 10 m land + lakes (public domain), baked by tools/make_pad_coasts.py",
           "note": "sdf_b64: grid_n x grid_n bytes, rows south to north, columns west to east, over [-R, R]^2 "
                   "metres east / north of (lat, lon); d = (byte - 127.5) / 127.5 x sdf_max_m, > 0 water",
           "patches": patches}
    path = os.path.join(REPO, "data", "launch_coasts.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
        f.write("\n")
    print(f"wrote {os.path.relpath(path, REPO)}: {len(patches)} patches")


if __name__ == "__main__":
    main()
