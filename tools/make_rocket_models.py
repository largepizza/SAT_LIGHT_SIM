"""Writes the launch vehicles' render models: data/satellite_models/rocket_<vehicle>_{stack,booster,upper}.json.

The sim draws a rocket as a mesh once it is a few pixels across (SatelliteSimLaunches.cpp, launchMeshSlots):
'stack' is the whole vehicle before staging (side boosters included), 'booster' the first stage after it,
'upper' the upper stage with its payload fairing (Starship: the ship). Body frame: +Z along the vehicle
toward the nose, the ORIGIN AT THE AFT END (the engines), where the sim's track point is. One rigid group
whose +Z follows the vehicle's thrust axis (the sim hands it in as the "velocity" direction) and -Y (the ship's
heat shield) turns down.

    python tools/make_rocket_models.py

Every dimension is a public figure rounded, or an estimate where noted in the table; colours are the
liveries' broad strokes (white paint, bare steel, tan foam, black interstages). Detail (grid fins, legs,
flaps, the ship's heat shield, engine bells) is render_only: it costs no lobes and does not light.
"""

import json
import math
import os

OUT = os.path.join(os.path.dirname(__file__), "..", "data", "satellite_models")

MATERIALS = [
    {"name": "white", "preset": "white_paint", "pattern": "panel_seams"},
    {"name": "black", "preset": "black_paint"},
    {"name": "steel", "preset": "stainless_steel", "pattern": "panel_seams"},
    {"name": "tan", "preset": "white_paint", "diffuse_albedo": 0.45, "color": [0.86, 0.56, 0.33]},
    {"name": "nozzle", "preset": "aluminum", "diffuse_albedo": 0.08, "roughness": 0.3, "color": [0.40, 0.37, 0.35]},
    {"name": "tiles", "preset": "black_paint", "roughness": 0.6},
]

# Vehicle table (metres). b_*: the first stage (core); band: a band at its top (interstage);
# side: strap-on boosters (count, radius, length, material) drawn in the stack only;
# u_*: the upper stage (its tank above an exposed nozzle of u_noz), then the payload fairing.
VEHICLES = {
    "starship": dict(r=4.5, b_len=69.0, b_mat="steel", band=(2.0, "black"), fins=(2.5, 4.0, 0.4), legs=0,
                     ship=True),
    "falcon9": dict(r=1.83, b_len=35.2, b_mat="white", band=(6.0, "black"), fins=(1.1, 1.4, 0.15), legs=4,
                    u_len=9.4, u_noz=3.0, u_mat="white", fr=2.6, f_cyl=6.7, f_cone=6.4),
    "new_glenn": dict(r=3.5, b_len=54.5, b_mat="white", band=(3.0, "black"), fins=(1.8, 2.4, 0.25), legs=6,
                      u_len=21.0, u_noz=2.4, u_mat="white", fr=3.5, f_cyl=12.0, f_cone=9.9),
    "vulcan": dict(r=2.7, b_len=33.3, b_mat="tan", side=(2, 0.8, 21.9, "white"),
                   u_len=11.7, u_noz=2.0, u_mat="white", fr=2.7, f_cyl=9.0, f_cone=6.5),
    "ariane6": dict(r=2.7, b_len=28.4, b_mat="white", side=(4, 1.7, 13.5, "white"),
                    u_len=11.6, u_noz=1.5, u_mat="white", fr=2.7, f_cyl=12.0, f_cone=8.0),
    "long_march_8a": dict(r=1.675, b_len=29.0, b_mat="white", side=(2, 1.125, 26.0, "white"),
                          u_len=10.0, u_noz=1.5, u_mat="white", fr=2.6, f_cyl=6.0, f_cone=5.0),
    "long_march_12": dict(r=1.9, b_len=40.0, b_mat="white", band=(2.0, "black"), fins=(1.1, 1.5, 0.15), legs=4,
                          u_len=12.0, u_noz=1.5, u_mat="white", fr=2.1, f_cyl=6.0, f_cone=5.0),
    "long_march_6a": dict(r=1.675, b_len=28.0, b_mat="white", side=(4, 1.0, 16.0, "white"),
                          u_len=10.0, u_noz=1.5, u_mat="white", fr=2.1, f_cyl=6.0, f_cone=5.0),
    "long_march_5b": dict(r=2.5, b_len=33.0, b_mat="white", side=(4, 1.675, 27.0, "white"),
                          u_len=0.0, u_noz=0.0, u_mat="white", fr=2.6, f_cyl=12.0, f_cone=8.0),
    "lvm3": dict(r=2.0, b_len=21.0, b_mat="white", side=(2, 1.6, 25.0, "white"),
                 u_len=13.0, u_noz=1.5, u_mat="white", fr=2.5, f_cyl=6.0, f_cone=5.0),
    "zhuque3": dict(r=2.25, b_len=46.0, b_mat="steel", band=(3.0, "black"), fins=(1.3, 1.8, 0.2), legs=4,
                    u_len=12.0, u_noz=2.0, u_mat="steel", fr=2.6, f_cyl=8.0, f_cone=6.0),
    "tianlong3": dict(r=1.9, b_len=45.0, b_mat="white", band=(3.0, "black"), fins=(1.1, 1.5, 0.15), legs=4,
                      u_len=11.0, u_noz=2.0, u_mat="white", fr=2.1, f_cyl=7.0, f_cone=6.0),
    "pallas1": dict(r=1.675, b_len=30.0, b_mat="white", band=(2.0, "black"), fins=(1.0, 1.3, 0.12), legs=4,
                    u_len=9.0, u_noz=1.5, u_mat="white", fr=2.1, f_cyl=5.0, f_cone=4.5),
}


def quat_from_basis(x, y, z):
    """(w, x, y, z) of the rotation whose columns are the given orthonormal axes."""
    m = [[x[0], y[0], z[0]], [x[1], y[1], z[1]], [x[2], y[2], z[2]]]
    tr = m[0][0] + m[1][1] + m[2][2]
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        w, qx, qy, qz = 0.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2
        w, qx, qy, qz = (m[2][1] - m[1][2]) / s, 0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s
    elif m[1][1] > m[2][2]:
        s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2
        w, qx, qy, qz = (m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s
    else:
        s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2
        w, qx, qy, qz = (m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s
    return [round(w, 6), round(qx, 6), round(qy, 6), round(qz, 6)]


def r4(v):
    return [round(a, 4) for a in v]


class Builder:
    def __init__(self):
        self.c = []

    def add(self, d, render_only=False):
        d["group"] = "body"
        if render_only:
            d["render_only"] = True
        d["name"] = "%s_%d" % (d["name"], len(self.c))
        self.c.append(d)

    def cyl(self, name, r, z0, z1, mat, caps=True, ro=False):
        self.add({"name": name, "type": "cylinder", "radius": round(r, 4), "height": round(z1 - z0, 4),
                  "position": r4([0, 0, 0.5 * (z0 + z1)]), "caps": caps, "material": mat}, ro)

    def cyl_at(self, name, r, x, y, z0, z1, mat, ro=False):
        self.add({"name": name, "type": "cylinder", "radius": round(r, 4), "height": round(z1 - z0, 4),
                  "position": r4([x, y, 0.5 * (z0 + z1)]), "caps": True, "material": mat}, ro)

    def cone(self, name, r0, r1, z0, z1, mat, x=0.0, y=0.0, ro=False):
        self.add({"name": name, "type": "cone", "radius_bottom": round(r0, 4), "radius_top": round(r1, 4),
                  "height": round(z1 - z0, 4), "position": r4([x, y, 0.5 * (z0 + z1)]), "caps": True,
                  "material": mat}, ro)

    def radial_plane(self, name, phi, rad, z0, z1, width, mat, ro=True, tangential=True):
        """A plane at angle phi (deg) about +Z: tangential = lying on the cylinder (normal radial), else a fin
        sticking out radially (normal tangential), spanning z0..z1."""
        p = math.radians(phi)
        er = (math.cos(p), math.sin(p), 0.0)
        et = (-math.sin(p), math.cos(p), 0.0)
        ez = (0.0, 0.0, 1.0)
        if tangential:
            x, y, n = et, ez, er
            pos = (rad * er[0], rad * er[1], 0.5 * (z0 + z1))
        else:
            x, y, n = er, ez, et
            pos = ((rad + 0.5 * width) * er[0], (rad + 0.5 * width) * er[1], 0.5 * (z0 + z1))
        self.add({"name": name, "type": "plane", "size": [round(width, 4), round(z1 - z0, 4)],
                  "position": r4(pos), "rotation_quat": quat_from_basis(x, y, n), "material": mat}, ro)

    def radial_box(self, name, phi, rad, z, sr, st, sz, mat, ro=True):
        p = math.radians(phi)
        er = (math.cos(p), math.sin(p), 0.0)
        et = (-math.sin(p), math.cos(p), 0.0)
        self.add({"name": name, "type": "box", "size": r4([sr, st, sz]),
                  "position": r4([(rad + 0.5 * sr) * er[0], (rad + 0.5 * sr) * er[1], z]),
                  "rotation_quat": quat_from_basis(er, et, (0, 0, 1)), "material": mat}, ro)


def booster_parts(b, v, z0=0.0, side=False):
    r = v["r"]
    top = z0 + v["b_len"]
    b.cone("engines", 0.92 * r, 0.6 * r, z0 - 1.2, z0, "nozzle", ro=True)
    band = v.get("band")
    if band:
        b.cyl("core", r, z0, top - band[0], v["b_mat"])
        b.cyl("interstage", r, top - band[0], top, band[1], caps=False)
    else:
        b.cyl("core", r, z0, top, v["b_mat"])
    fins = v.get("fins")
    if fins:
        for k in range(4):
            b.radial_box("grid_fin", 45 + 90 * k, r, top - 0.8 * fins[1], fins[0], fins[1], fins[2], "nozzle")
    for k in range(v.get("legs", 0)):
        b.radial_plane("leg", 360.0 * k / v["legs"], r + 0.03, z0 + 0.3, z0 + 0.3 + 0.28 * v["b_len"],
                       0.35 * r, "black")
    if side and v.get("side"):
        n, rs, ls, mat = v["side"]
        for k in range(n):
            p = math.radians(90.0 + 360.0 * k / n)
            d = r + rs + 0.05
            b.cyl_at("side_booster", rs, d * math.cos(p), d * math.sin(p), z0, z0 + ls, mat)
            b.cone("side_nose", rs, 0.25 * rs, z0 + ls, z0 + ls + 2.0 * rs, mat, d * math.cos(p), d * math.sin(p))
            b.cone("side_nozzle", 0.8 * rs, 0.5 * rs, z0 - 0.8, z0, "nozzle", d * math.cos(p), d * math.sin(p),
                   ro=True)
    return top


def ship_parts(b, v, z0=0.0):
    r = v["r"]
    b.cone("engines", 0.85 * r, 0.55 * r, z0 - 1.5, z0, "nozzle", ro=True)
    b.cyl("tank", r, z0, z0 + 40.0, "steel")
    b.cone("nose", r, 0.6, z0 + 40.0, z0 + 52.0, "steel")
    # The heat shield: the windward half (-Y), black tiles, to the start of the nose.
    for k in range(5):
        b.radial_plane("heat_shield", -126.0 + 36.0 * k, r + 0.03, z0 + 0.5, z0 + 44.0,
                       2.0 * (r + 0.03) * math.sin(math.radians(18.0)), "tiles")
    for side in (0.0, 180.0):
        b.radial_plane("aft_flap", side, r, z0 + 1.5, z0 + 11.0, 3.6, "tiles", tangential=False)
        b.radial_plane("fwd_flap", side, 0.75 * r, z0 + 42.0, z0 + 48.0, 2.4, "tiles", tangential=False)
    return z0 + 52.0


def upper_parts(b, v, z0=0.0):
    r = v["r"]
    z = z0
    if v["u_len"] > 0:
        b.cone("upper_nozzle", 0.75 * r, 0.25 * r, z, z + v["u_noz"], "nozzle", ro=v["u_noz"] < 0.5)
        z += v["u_noz"]
        b.cyl("upper_tank", r, z, z + v["u_len"], v["u_mat"])
        z += v["u_len"]
    fr = v["fr"]
    if fr > r + 0.05:
        b.cone("fairing_boattail", r, fr, z, z + 0.8 * (fr - r) + 0.3, "white")
        z += 0.8 * (fr - r) + 0.3
    b.cyl("fairing", fr, z, z + v["f_cyl"], "white", caps=False)
    z += v["f_cyl"]
    b.cone("fairing_nose", fr, 0.3, z, z + v["f_cone"], "white")
    return z + v["f_cone"]


def model(vid, part, v, comps):
    note = ("Launch vehicle render model (tools/make_rocket_models.py), %s: %s. Body frame: +Z toward the nose, "
            "origin at the aft end. Sizes are public figures rounded; liveries broad strokes; detail render-only."
            % (vid, part))
    return {
        "name": "%s %s" % (vid, part),
        "note": note,
        "materials": MATERIALS,
        "attitude_groups": [{"name": "body", "primary": {"axis": [0, 0, 1], "target": "velocity"},
                             "secondary": {"axis": [0, -1, 0], "target": "nadir"}}],
        "components": comps,
        "sources": [
            {"subject": "attitude", "status": "estimate",
             "value": "+Z along the thrust axis (the sim passes it as 'velocity'), -Y (the ship's heat shield) down",
             "source": "A rocket flies along its thrust axis; a gravity turn and an entry fly belly down."},
            {"subject": "body", "status": "estimate", "value": "one rigid group",
             "source": "Render model; no moving parts."},
            {"subjects": [c["name"] for c in comps], "status": "estimate",
             "value": "rounded public dimensions (Wikipedia / operator pages), broad liveries",
             "source": "tools/make_rocket_models.py VEHICLES table."},
            {"subjects": [m["name"] for m in MATERIALS], "status": "estimate",
             "value": "presets: white paint, black paint, stainless, tan foam, dark nozzles, black tiles",
             "source": "Liveries from photographs; reflectance from the presets."},
        ],
    }


def main():
    n = 0
    for vid, v in VEHICLES.items():
        parts = {}
        b = Builder()
        top = booster_parts(b, v, 0.0, side=True)
        if v.get("ship"):
            ship_parts(b, v, top)
        else:
            upper_parts(b, v, top - v["u_noz"] * 0.9)
        parts["stack"] = b.c
        b = Builder()
        booster_parts(b, v, 0.0, side=False)
        parts["booster"] = b.c
        b = Builder()
        if v.get("ship"):
            ship_parts(b, v, 0.0)
        else:
            upper_parts(b, v, 0.0)
        parts["upper"] = b.c
        for part, comps in parts.items():
            path = os.path.join(OUT, "rocket_%s_%s.json" % (vid, part))
            with open(path, "w", encoding="utf-8") as f:
                json.dump(model(vid, part, v, comps), f, indent=4)
            n += 1
    print("wrote %d models" % n)


if __name__ == "__main__":
    main()
