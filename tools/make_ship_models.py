"""Writes the booster landing ships' render models: data/satellite_models/ship_{droneship,jacklyn}.json.

A booster that cannot fly home lands downrange on a ship (SatelliteSimLaunches.cpp, planRecovery: launches.json
"landing_ships"); the ship is drawn as a mesh on the sea from hours before the launch until it has steamed some way
home with the booster on its deck (launchMeshJobs, the ship slots).

Body frame: +Z up, +X the bow (the sim points it at the launch site: the way home), +Y to port. The ORIGIN IS THE
LANDING TARGET'S CENTRE AT THE WATERLINE: the booster's track comes down on (0, 0, deck_m) - launches.json's
"deck_m" must equal the deck height written here (printed). One rigid group: +Z to the zenith, +X along the sim's
"velocity" axis (the heading it hands in).

    python tools/make_ship_models.py

Sizes: SpaceX's autonomous spaceport drone ships (ASOG, JRTI, OCISLY) - a deck ~91 x 52 m (the barge with its wing
extensions), the equipment housings and blast walls aft, the "Octagrabber" securing robot, the landing target's ring
and X; deck ~3 m above the water (estimate). Blue Origin's landing ship Jacklyn - a converted ship ~120 m long and
~30 m in beam (estimates), its superstructure forward and the landing deck aft. Detail is render_only.
"""

import json
import math
import os

OUT = os.path.join(os.path.dirname(__file__), "..", "data", "satellite_models")

MATERIALS = [
    {"name": "hull", "preset": "black_paint", "diffuse_albedo": 0.07, "color": [0.17, 0.18, 0.2]},
    {"name": "deck", "preset": "black_paint", "diffuse_albedo": 0.11, "color": [0.32, 0.32, 0.33]},
    {"name": "white", "preset": "white_paint"},
    {"name": "grey", "preset": "white_paint", "diffuse_albedo": 0.45, "color": [0.7, 0.71, 0.72]},
    {"name": "steel", "preset": "stainless_steel"},
    {"name": "red", "preset": "white_paint", "diffuse_albedo": 0.25, "color": [0.62, 0.12, 0.08]},
    {"name": "yellow", "preset": "white_paint", "diffuse_albedo": 0.5, "color": [0.9, 0.72, 0.12]},
]


def box(name, size, pos, mat, render_only=False, rot=None):
    c = {"name": name, "type": "box", "group": "ship", "size": list(size), "position": list(pos), "material": mat}
    if rot:
        c["rotation_deg"] = list(rot)
    if render_only:
        c["render_only"] = True
    return c


def cyl(name, r, h, pos, mat, render_only=False, rot=None):
    c = {"name": name, "type": "cylinder", "group": "ship", "radius": r, "height": h, "position": list(pos),
         "caps": True, "material": mat}
    if rot:
        c["rotation_deg"] = list(rot)
    if render_only:
        c["render_only"] = True
    return c


def target(comps, z, r_ring, w_ring, x_len, x_w, mat="white", segs=36):
    """The landing target painted on the deck: a ring of segments and an X (render-only, a few cm proud)."""
    for i in range(segs):
        a = 360.0 * i / segs
        ar = math.radians(a)
        seg = 2.0 * math.pi * r_ring / segs * 1.04
        comps.append(box(f"target_ring_{i}", (w_ring, seg, 0.03), (r_ring * math.cos(ar), r_ring * math.sin(ar), z + 0.02),
                         mat, True, rot=(0, 0, a)))
    for k, a in enumerate((45.0, -45.0)):
        comps.append(box(f"target_x_{k}", (x_len, x_w, 0.03), (0.0, 0.0, z + 0.035 + 0.005 * k), mat, True, rot=(0, 0, a)))


def droneship():
    D = 3.2  # deck above the waterline (launches.json "deck_m")
    comps = []
    # The barge: one hull to its deck (with the wing extensions it is ~52 m wide), 1.5 m below the water so waves lap it.
    comps.append(box("hull", (91.0, 52.0, D + 1.5), (0.0, 0.0, (D - 1.5) / 2.0), "deck"))
    # The hull's sides below the deck edge (darker band) and the wings' supports (render-only detail).
    for sd in (-1, 1):
        comps.append(box(f"side_band_{sd + 1}", (91.2, 0.15, 1.6), (0.0, sd * 26.05, D - 1.0), "hull", True))
        for i in range(7):
            comps.append(box(f"wing_strut_{sd + 1}_{i}", (0.5, 0.5, 2.0), (-36.0 + 12.0 * i, sd * 25.6, D - 1.2), "hull", True))
        # Deck-edge rails
        comps.append(box(f"rail_{sd + 1}", (88.0, 0.12, 0.9), (1.0, sd * 25.7, D + 0.45), "yellow", True))
    comps.append(box("rail_bow", (0.12, 50.0, 0.9), (45.3, 0.0, D + 0.45), "yellow", True))
    # Aft: the two equipment housings (generators, thruster power, the Octagrabber's garage) and the blast walls.
    for sd in (-1, 1):
        comps.append(box(f"housing_{sd + 1}", (9.0, 13.0, 7.0), (-40.0, sd * 18.5, D + 3.5), "grey"))
        comps.append(box(f"blast_wall_{sd + 1}", (1.2, 13.5, 8.5), (-35.0, sd * 18.5, D + 4.25), "steel", True))
        comps.append(cyl(f"mast_{sd + 1}", 0.25, 9.0, (-42.0, sd * 21.0, D + 7.0 + 4.5), "steel", True))
        comps.append(box(f"vent_{sd + 1}", (3.0, 4.0, 1.4), (-41.5, sd * 15.0, D + 7.7), "hull", True))
    comps.append(box("garage", (8.0, 10.0, 3.5), (-40.0, 0.0, D + 1.75), "grey", True))
    # The Octagrabber is its own model (ship_robot, below): it drives out to a landed booster and clamps it
    # (SatelliteSimLaunches.cpp shipRobotAt), parked in front of the garage until then.
    # Bollards and the tow bitts forward.
    for i, y in enumerate((-20.0, -8.0, 8.0, 20.0)):
        comps.append(cyl(f"bitt_{i}", 0.45, 1.0, (43.5, y, D + 0.5), "steel", True))
    target(comps, D, 13.0, 1.3, 24.0, 2.6)
    return comps, D


def jacklyn():
    D = 6.0  # the landing deck above the waterline (launches.json "deck_m")
    comps = []
    # Hull: the landing deck aft (the target at the origin), the superstructure forward, a raked bow.
    comps.append(box("hull", (106.0, 30.0, D + 3.0), (27.0, 0.0, (D - 3.0) / 2.0), "hull"))
    comps.append(box("bow", (21.2, 21.2, D + 3.0), (80.0, 0.0, (D - 3.0) / 2.0), "hull", rot=(0, 0, 45)))
    comps.append(box("deck", (78.0, 29.6, 0.2), (-2.0, 0.0, D + 0.1), "deck", True))
    comps.append(box("forecastle", (16.0, 22.0, 2.4), (82.0, 0.0, D + 1.2), "hull", True))
    # Superstructure: accommodation block, bridge, funnel, mast.
    comps.append(box("superstructure", (20.0, 24.0, 12.0), (63.0, 0.0, D + 6.0), "white"))
    comps.append(box("bridge", (9.0, 28.0, 4.0), (67.0, 0.0, D + 14.0), "white"))
    comps.append(box("bridge_windows", (0.2, 26.0, 1.4), (71.6, 0.0, D + 14.6), "hull", True))
    comps.append(box("funnel", (6.0, 5.0, 8.0), (56.0, 0.0, D + 16.0), "grey", True))
    comps.append(cyl("mast", 0.4, 14.0, (66.0, 0.0, D + 23.0), "steel", True))
    # The deck: edge walls (wind and blast), service hatches, cranes forward of it.
    for sd in (-1, 1):
        comps.append(box(f"bulwark_{sd + 1}", (78.0, 0.3, 1.6), (-2.0, sd * 14.85, D + 0.8), "grey", True))
        comps.append(box(f"crane_base_{sd + 1}", (3.0, 3.0, 4.0), (45.0, sd * 11.0, D + 2.0), "yellow", True))
        comps.append(box(f"crane_jib_{sd + 1}", (16.0, 0.8, 0.8), (37.5, sd * 11.0, D + 4.3), "yellow", True, rot=(0, -12, 0)))
    comps.append(box("bulwark_stern", (0.3, 29.6, 1.6), (-41.0, 0.0, D + 0.8), "grey", True))
    # (Its securing robot is the ship_robot model, parked at (24, 0) on the deck.)
    target(comps, D, 11.5, 1.1, 20.0, 2.2)
    return comps, D


def robot():
    """The booster-securing robot (SpaceX's "Octagrabber"; estimates from photographs: a low red tracked vehicle ~4.5 m
    square). Origin at its centre on the deck, +X its front. Four clamp arms reach forward and up under a booster's
    engine section: stowed (folded into the body) while it drives, deployed when it clamps (the unfolding progress)."""
    comps = []
    comps.append(box("body", (4.4, 4.0, 0.9), (0.0, 0.0, 0.65), "red"))
    comps.append(box("top", (3.0, 3.0, 0.3), (-0.2, 0.0, 1.25), "hull", True))
    for sd in (-1, 1):
        comps.append(box(f"track_{sd + 1}", (4.6, 0.7, 0.75), (0.0, sd * 2.25, 0.375), "hull", True))
        comps.append(box(f"beacon_{sd + 1}", (0.25, 0.25, 0.3), (-1.8, sd * 1.6, 1.25), "yellow", True))
    for k, y in enumerate((-1.5, -0.5, 0.5, 1.5)):
        a = box(f"clamp_arm_{k}", (2.6, 0.35, 0.35), (3.4, y, 1.0), "steel", True, rot=(0, -12, 0))
        a["stow"] = {"axis": [1, 0, 0], "root": 2.1, "scale": 0.08, "phase": [0.0, 1.0]}
        comps.append(a)
    return comps


def write(name, note, comps):
    model = {
        "name": name.replace("_", " "),
        "note": note,
        "materials": MATERIALS,
        "attitude_groups": [{"name": "ship",
                             "primary": {"axis": [0, 0, 1], "target": "zenith"},
                             "secondary": {"axis": [1, 0, 0], "target": "velocity"}}],
        "components": comps,
    }
    path = os.path.join(OUT, name + ".json")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(model, f, indent=1)
        f.write("\n")
    print("wrote", path, len(comps), "components")


if __name__ == "__main__":
    base = ("Booster landing ship render model (tools/make_ship_models.py). Body frame: +Z up, +X the bow, origin at "
            "the landing target's centre at the waterline. Sizes public figures rounded or estimates; detail "
            "render-only.")
    c, d = droneship()
    write("ship_droneship", base + " SpaceX autonomous spaceport drone ship (ASOG / JRTI / OCISLY class): deck ~91 x 52 "
          "m, housings and blast walls aft, the Octagrabber, the landing target.", c)
    print("  deck_m", d)
    write("ship_robot", base.replace("Booster landing ship render model", "Booster-securing robot render model").replace(
          "+X the bow, origin at the landing target's centre at the waterline", "+X its front, origin at its centre on "
          "the deck") + " SpaceX's Octagrabber (estimates): a tracked vehicle ~4.5 m square with four clamp arms "
          "(stowed while it drives).", robot())
    c, d = jacklyn()
    write("ship_jacklyn", base + " Blue Origin's landing ship Jacklyn (estimates): superstructure forward, the "
          "landing deck aft.", c)
    print("  deck_m", d)
