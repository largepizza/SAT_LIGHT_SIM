"""Writes the launch pads' render models: data/satellite_models/pad_{starship,falcon,generic}.json.

The sim draws each launch pad's tall parts as a static mesh once it is a pixel or two across
(SatelliteSimLaunches.cpp, launchMeshJobs): the tower, the launch mount, lightning masts, flood masts and tanks.
The flat parts (graded ground, concrete, the flame trench, roads, a landing zone) are sat_sky.frag's ground pattern
(padGround), whose layout must agree with this one.

Body frame: +Z up, +X the pad's heading u (the middle of its launch azimuths), +Y = v (to its left), the ORIGIN AT
THE PAD'S CENTRE ON THE GROUND (the plateau the terrain is graded to). One rigid group: +Z to the zenith, +X along
the sim's "velocity" axis (the heading it hands in).

    python tools/make_pad_models.py

Sizes are public figures rounded (Starbase's catch tower ~146 m, its chopsticks ~95 m up over the mount; a Falcon
pad's fixed service structure ~80 m; lightning masts ~110 m); the rest is an estimate. Detail is render_only.
"""

import json
import os

OUT = os.path.join(os.path.dirname(__file__), "..", "data", "satellite_models")

MATERIALS = [
    {"name": "lattice", "preset": "truss", "coverage": 0.6, "diffuse_albedo": 0.22, "color": [0.32, 0.32, 0.34]},
    {"name": "lattice_red", "preset": "truss", "coverage": 0.55, "diffuse_albedo": 0.25, "color": [0.55, 0.22, 0.16]},
    {"name": "steel", "preset": "stainless_steel"},
    {"name": "dark", "preset": "black_paint"},
    {"name": "white", "preset": "white_paint"},
    {"name": "concrete", "preset": "white_paint", "diffuse_albedo": 0.45, "color": [0.62, 0.61, 0.58]},
]

TOWER_V = -32.0  # == kPadTowerV (SatelliteSimLaunches.cpp)


def box(name, size, pos, mat, render_only=False):
    c = {"name": name, "type": "box", "group": "pad", "size": list(size), "position": list(pos), "material": mat}
    if render_only:
        c["render_only"] = True
    return c


def cyl(name, r, h, pos, mat, render_only=False, rot=None):
    c = {"name": name, "type": "cylinder", "group": "pad", "radius": r, "height": h, "position": list(pos),
         "caps": True, "material": mat}
    if rot:
        c["rotation_deg"] = list(rot)
    if render_only:
        c["render_only"] = True
    return c


def sphere(name, r, pos, mat, render_only=False):
    c = {"name": name, "type": "sphere", "group": "pad", "radius": r, "position": list(pos), "material": mat}
    if render_only:
        c["render_only"] = True
    return c


def starship():
    comps = []
    # The orbital launch mount: a ring table on six legs, its top 20 m up (the vehicle stands on it: mount_m).
    for i in range(6):
        import math
        a = math.radians(60 * i + 30)
        comps.append(cyl(f"olm_leg_{i}", 1.4, 18.0, (9.5 * math.cos(a), 9.5 * math.sin(a), 9.0), "steel", True))
    comps.append(cyl("olm_table", 11.0, 3.0, (0, 0, 18.5), "steel"))
    # The catch tower: a 12-m square lattice 146 m tall beside the mount, its chopsticks reaching over the mount.
    comps.append(box("tower", (12.0, 12.0, 146.0), (0, TOWER_V, 73.0), "lattice"))
    comps.append(cyl("tower_rod", 0.4, 14.0, (0, TOWER_V, 153.0), "steel", True))
    comps.append(box("carriage", (15.0, 15.0, 7.0), (0, TOWER_V, 96.0), "dark", True))
    for sd in (-1, 1):
        comps.append(box(f"chopstick_{sd + 1}", (2.4, 36.0, 3.2), (sd * 6.2, TOWER_V + 6.0 + 18.0, 95.0), "steel"))
    comps.append(box("qd_arm", (3.0, 22.0, 3.0), (0, TOWER_V + 6.0 + 11.0, 72.0), "dark", True))
    # Flood masts at the apron's corners.
    for i, (u, v) in enumerate(((105, 105), (105, -105), (-105, 105), (-105, -105))):
        comps.append(cyl(f"flood_mast_{i}", 0.6, 45.0, (u, v, 22.5), "steel", True))
    # The tank farm: vertical propellant tanks (methane, oxygen, nitrogen) on their slab.
    for row in range(2):
        for col in range(3):
            comps.append(cyl(f"tank_{row}_{col}", 5.0, 26.0, (-244.0 + 14.0 * col, 126.0 + 15.0 * row, 13.0), "white"))
    comps.append(cyl("tank_farm_lox", 7.0, 30.0, (-205.0, 170.0, 15.0), "white"))
    return comps


def fss(tv, th):
    """A fixed service structure (lattice) at (0, tv), th tall, its crew / service arm toward the pad."""
    return [
        box("fss", (10.0, 10.0, th), (0, tv, th / 2), "lattice_red"),
        box("fss_arm", (2.4, 18.0, 3.0), (0, tv + 5.0 + 9.0, th * 0.72), "dark", True),
        cyl("fss_mast", 0.4, 20.0, (0, tv, th + 10.0), "steel", True),
    ]


def masts():
    return [cyl(f"lightning_mast_{i}", 1.0, 110.0, (u, v, 55.0), "steel", True)
            for i, (u, v) in enumerate(((60, 60), (60, -60), (-60, 55)))]


def falcon():
    comps = fss(-25.0, 80.0) + masts()
    comps.append(sphere("lox_sphere", 11.0, (-60.0, 110.0, 11.0), "white"))
    comps.append(cyl("rp1_tank", 4.0, 26.0, (-20.0, 120.0, 4.5), "white", rot=(0, 90, 0)))
    comps.append(cyl("water_tower_leg", 1.2, 40.0, (100.0, -90.0, 20.0), "steel", True))
    comps.append(sphere("water_tower", 6.0, (100.0, -90.0, 44.0), "white"))
    return comps


def generic():
    comps = fss(-22.0, 70.0) + masts()
    comps.append(sphere("lox_sphere", 9.0, (-70.0, 100.0, 9.0), "white"))
    comps.append(cyl("fuel_tank", 4.0, 20.0, (-30.0, 112.0, 4.5), "white", rot=(0, 90, 0)))
    return comps


def write(name, note, comps):
    model = {
        "name": name.replace("_", " "),
        "note": note,
        "materials": MATERIALS,
        "attitude_groups": [{"name": "pad",
                             "primary": {"axis": [0, 0, 1], "target": "zenith"},
                             "secondary": {"axis": [1, 0, 0], "target": "velocity"}}],
        "components": comps,
    }
    path = os.path.join(OUT, name + ".json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(model, f, indent=1)
        f.write("\n")
    print("wrote", path, len(comps), "components")


if __name__ == "__main__":
    base = ("Launch pad render model (tools/make_pad_models.py). Body frame: +Z up, +X the pad's heading, origin at "
            "the pad's centre on the ground. Sizes public figures rounded or estimates; detail render-only.")
    write("pad_starship", base + " Starship: the launch mount, the catch tower and its chopsticks, flood masts, "
          "the tank farm.", starship())
    write("pad_falcon", base + " Falcon: the fixed service structure, three lightning masts, LOX sphere, RP-1 "
          "tank, water tower.", falcon())
    write("pad_generic", base + " A service tower, lightning masts, propellant tanks.", generic())
