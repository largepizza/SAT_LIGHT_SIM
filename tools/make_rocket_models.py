"""Writes the launch vehicles' render models: data/satellite_models/rocket_<vehicle>_{stack,booster,upper}.json.

The sim draws a rocket as a mesh once it is a few pixels across (SatelliteSimLaunches.cpp, launchMeshSlots):
'stack' is the whole vehicle before staging (side boosters included), 'booster' the first stage after it,
'upper' the upper stage with its payload fairing (Starship: the ship). Body frame: +Z along the vehicle
toward the nose, the ORIGIN AT THE AFT END of the first stage's (or the upper stage's) body, where the sim's
track point is; engine bells hang below it. One rigid group whose +Z follows the vehicle's thrust axis (the sim
hands it in as the "velocity" direction) and -Y (the ship's heat shield) turns down.

    python tools/make_rocket_models.py

Shapes (2026-10-19 rework, issue 20261019_181320): bodies, noses, fairings, domes and engine bells are PROFILE
primitives (a lathe of an r(z) polyline: tangent ogives with blunt tips, cone frustums, bells with a dark inside),
flaps and fins FIN primitives (tapered, swept, thick, bevelled), Starship's heat shield an arc of the body in the
'hex_tiles' material. Every engine is an individual render-only bell named engines_* / upper_nozzle_* /
side_nozzle_* (the flame and plume read their exit plane and the radius they span: engineGeomOf).

Dimensions are public figures (each vehicle's `refs` cite them), rounded; where a figure is not published the
`notes` say what was estimated. Detail (grid fins, legs, bands, bells, chines, raceways, escape towers) is
render_only: it costs no lobes and does not light.
"""

import json
import math
import os

OUT = os.path.join(os.path.dirname(__file__), "..", "data", "satellite_models")

MATERIALS = [
    {"name": "white", "preset": "white_paint", "pattern": "panel_seams"},
    {"name": "black", "preset": "black_paint"},
    {"name": "grey", "preset": "white_paint", "diffuse_albedo": 0.3, "color": [0.45, 0.46, 0.48]},
    {"name": "steel", "preset": "stainless_steel", "pattern": "panel_seams"},
    {"name": "tan", "preset": "white_paint", "diffuse_albedo": 0.45, "color": [0.86, 0.56, 0.33]},
    {"name": "nozzle", "preset": "aluminum", "diffuse_albedo": 0.08, "roughness": 0.3, "color": [0.40, 0.37, 0.35]},
    {"name": "copper", "preset": "aluminum", "diffuse_albedo": 0.12, "roughness": 0.35, "color": [0.62, 0.38, 0.24]},
    {"name": "soot", "preset": "black_paint", "diffuse_albedo": 0.03, "roughness": 0.7},
    {"name": "tiles", "preset": "black_paint", "diffuse_albedo": 0.06, "roughness": 0.6,
     "color": [0.10, 0.10, 0.11], "pattern": "hex_tiles"},
    {"name": "gridfin", "preset": "truss", "coverage": 0.55, "truss_pitch": 0.14, "color": [0.35, 0.35, 0.37]},
    {"name": "lattice", "preset": "truss", "coverage": 0.3, "truss_pitch": 0.7},
    {"name": "cells", "preset": "solar_cell"},
    {"name": "green", "preset": "white_paint", "diffuse_albedo": 0.3, "color": [0.36, 0.42, 0.30]},
    {"name": "chute_orange", "preset": "white_paint", "diffuse_albedo": 0.55, "color": [0.96, 0.45, 0.10]},
    {"name": "chute_red", "preset": "white_paint", "diffuse_albedo": 0.45, "color": [0.85, 0.12, 0.10]},
]
MATERIAL_SOURCES = {
    "white": "white paint (operator liveries in launch photographs)",
    "black": "black paint / carbon composite (interstages, legs)",
    "grey": "grey paint (Soyuz, Long March detail)",
    "steel": "bare 304L-class stainless (Starship, Zhuque-3)",
    "tan": "orange-tan spray-on foam insulation (Vulcan's first stage)",
    "nozzle": "dark regeneratively cooled nozzle metal",
    "copper": "copper-alloy chamber and bell (Raptor, Merlin), oxidised",
    "soot": "a nozzle's sooty inside",
    "tiles": "black silica heat-shield tiles (Starship), hexagonal pattern",
    "gridfin": "titanium / steel grid fins: an open lattice",
    "lattice": "an open truss interstage (Soyuz Block A / Block I)",
    "cells": "Crew Dragon trunk's body-mounted solar cells; a crewed spacecraft's folded arrays",
    "green": "Soyuz orbital module's green insulation blanket",
    "chute_orange": "orange parachute nylon (Soyuz, Gaganyaan canopies)",
    "chute_red": "red parachute nylon (Shenzhou, Dragon canopies)",
}


# ── Geometry helpers ─────────────────────────────────────────────────────────────────────────────────────────
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


def ogive(r_base, length, z0, tip_r=0.0, n=14):
    """Tangent ogive from radius r_base at z0 to a tip at z0 + length (points [z, r], z ascending, base first).
    tip_r > 0: blunted by a spherical cap of that radius, tangent to the ogive."""
    rho = (r_base * r_base + length * length) / (2.0 * r_base)
    pts = []
    if tip_r <= 0.0:
        for i in range(n + 1):
            h = length * (1.0 - (1.0 - i / n) ** 1.3)  # denser toward the tip
            r = math.sqrt(max(rho * rho - h * h, 0.0)) + r_base - rho
            pts.append([z0 + h, max(r, 0.0)])
        pts[-1][1] = 0.0
        return pts
    # The blunting sphere: centre on the axis at height zc, tangent to the ogive arc (centre (-(rho - r_base), 0)
    # in (r, h) coordinates, radius rho): |centre_arc - centre_sphere| = rho - tip_r.
    cr = -(rho - r_base)
    hc = math.sqrt(max((rho - tip_r) ** 2 - cr * cr, 0.0))
    # Tangent point: on the line from the arc centre through the sphere centre, at distance rho.
    d = math.hypot(0.0 - cr, hc)
    tr, th = cr + rho * (0.0 - cr) / d, rho * hc / d
    for i in range(n + 1):
        h = th * (1.0 - (1.0 - i / n) ** 1.3)
        r = math.sqrt(max(rho * rho - h * h, 0.0)) + r_base - rho
        pts.append([z0 + h, r])
    a0 = math.atan2(tr, th - hc)  # angle of the tangent point from the sphere's axis
    for i in range(1, 6):
        a = a0 * (1.0 - i / 5.0)
        pts.append([z0 + hc + tip_r * math.cos(a), tip_r * math.sin(a)])
    pts[-1][1] = 0.0
    return pts


def dome(r, depth, z0, up=True, n=6):
    """Elliptical dome from radius r at z0 to the axis, depth up (or down)."""
    pts = []
    for i in range(n + 1):
        a = 0.5 * math.pi * i / n
        pts.append([z0 + (1 if up else -1) * depth * math.sin(a), r * math.cos(a)])
    if not up:
        pts.reverse()
    return pts


def bell(re, length, z_exit, rt_frac=0.36, chamber=0.32):
    """An engine: bell from exit radius re at z_exit up to the throat, then the chamber; points ascending."""
    rt = rt_frac * re
    lb = (1.0 - chamber) * length
    pts = []
    for i in range(7):
        u = i / 6.0
        pts.append([z_exit + lb * u, rt + (re - rt) * (1.0 - u) ** 1.7])
    rc = 1.55 * rt
    zt = z_exit + lb
    pts += [[zt + 0.25 * (length - lb), rc], [zt + 0.8 * (length - lb), rc], [z_exit + length, 0.6 * rc],
            [z_exit + length + 0.05, 0.0]]
    return pts


class Builder:
    def __init__(self):
        self.c = []

    def add(self, d, render_only=False):
        d["group"] = "body"
        if render_only:
            d["render_only"] = True
        d["name"] = "%s_%d" % (d["name"], len(self.c))
        self.c.append(d)
        return d

    def profile(self, name, pts, mat, x=0.0, y=0.0, ro=False, caps=False, arc=None, inner=None, rot=None):
        d = {"name": name, "type": "profile", "profile": [r4(p) for p in pts], "position": r4([x, y, 0.0]),
             "caps": caps, "material": mat}
        if arc:
            d["arc_material"], d["arc_deg"] = arc[0], [arc[1], arc[2]]
        if inner:
            d["inner_material"] = inner
        if rot:
            d["rotation_quat"] = rot
        return self.add(d, ro)

    def pieces(self, name, pts, mat, cuts, caps=True, **kw):
        """A profile split at the given heights into separate components (shared rings, caps only at the ends):
        each piece's occluder is the frustum through ITS end radii, so a cylinder + nose is not shadowed as one
        long cone."""
        segs, cur = [], [pts[0]]
        for p in pts[1:]:
            for zc in cuts:
                if cur[-1][0] < zc < p[0]:
                    t = (zc - cur[-1][0]) / (p[0] - cur[-1][0])
                    q = [zc, cur[-1][1] + t * (p[1] - cur[-1][1])]
                    cur.append(q)
                    segs.append(cur)
                    cur = [q]
            cur.append(p)
            if any(abs(p[0] - zc) < 1e-9 for zc in cuts) and p is not pts[-1]:
                segs.append(cur)
                cur = [p]
        segs.append(cur)
        segs = [g for g in segs if len(g) >= 2]
        for i, g in enumerate(segs):
            c = ("both" if len(segs) == 1 else "bottom" if i == 0 else "top" if i == len(segs) - 1 else False)                 if caps else False
            self.profile(name, g, mat, caps=c, **kw)

    def tube(self, name, r, z0, z1, mat, ro=False, caps=False, x=0.0, y=0.0, arc=None):
        return self.profile(name, [[z0, r], [z1, r]], mat, x, y, ro, caps, arc)

    def disc(self, name, r, z, mat, x=0.0, y=0.0):
        """A thin render-only plate facing -Z (an aft heat shield)."""
        return self.profile(name, [[z - 0.04, r], [z, r]], mat, x, y, ro=True, caps=True)

    def engines(self, name, rings, re, length, z_exit, mat="copper", x=0.0, y=0.0, phase=0.0, cant=0.0):
        """Individual bells: rings = [(count, ring radius), ...]; cant tilts outer bells outward (deg)."""
        for (n, rr) in rings:
            for k in range(n):
                p = math.radians(phase + 360.0 * k / n)
                ex, ey = x + rr * math.cos(p), y + rr * math.sin(p)
                rot = None
                if cant and rr > 0:
                    t = math.radians(cant)
                    er = (math.cos(p), math.sin(p), 0.0)
                    et = (-math.sin(p), math.cos(p), 0.0)
                    zax = (-math.sin(t) * er[0], -math.sin(t) * er[1], math.cos(t))  # exits splay outward
                    xax = (math.cos(t) * er[0], math.cos(t) * er[1], math.sin(t))
                    rot = quat_from_basis(xax, et, zax)
                self.profile(name, bell(re, length, z_exit), mat, ex, ey, ro=True, inner="soot", rot=rot)

    def fin(self, name, phi, r_root, z_mid, root, tip, span, sweep, thick, mat, back=None, ro=False,
            slope_deg=0.0, tip_thick=None, bevel=0.15):
        """A fin whose root chord (centred at z_mid, leading edge up) sits on the body at radius r_root, azimuth
        phi (deg); slope_deg tilts the root line to follow a nose that narrows upward. Local Y is the body's
        tangential direction at phi: `back` is the material of the face toward -Y local."""
        p = math.radians(phi)
        er = (math.cos(p), math.sin(p), 0.0)
        et = (-math.sin(p), math.cos(p), 0.0)
        a = math.radians(slope_deg)
        z = (-math.sin(a) * er[0], -math.sin(a) * er[1], math.cos(a))
        x = (math.cos(a) * er[0], math.cos(a) * er[1], math.sin(a))
        d = {"name": name, "type": "fin", "root_chord": round(root, 4), "tip_chord": round(tip, 4),
             "span": round(span, 4), "sweep": round(sweep, 4), "thickness": round(thick, 4), "bevel": bevel,
             "position": r4([r_root * er[0], r_root * er[1], z_mid]), "rotation_quat": quat_from_basis(x, et, z),
             "material": mat}
        if tip_thick is not None:
            d["tip_thickness"] = round(tip_thick, 4)
        if back:
            d["back_material"] = back
        return self.add(d, ro)

    def radial_box(self, name, phi, rad, z, sr, st, sz, mat, ro=True, tilt_deg=0.0):
        """A box standing out from the body at azimuth phi: sr radial, st tangential, sz along the axis; tilt
        turns it about the tangential axis (a grid fin folded up: 90)."""
        p = math.radians(phi)
        er = (math.cos(p), math.sin(p), 0.0)
        et = (-math.sin(p), math.cos(p), 0.0)
        t = math.radians(tilt_deg)
        xr = (math.cos(t) * er[0], math.cos(t) * er[1], math.sin(t))
        zr = (-math.sin(t) * er[0], -math.sin(t) * er[1], math.cos(t))
        cx = rad + 0.5 * sr * math.cos(t)
        cz = z + 0.5 * sr * math.sin(t)
        self.add({"name": name, "type": "box", "size": r4([sr, st, sz]),
                  "position": r4([cx * er[0], cx * er[1], cz]),
                  "rotation_quat": quat_from_basis(xr, et, zr), "material": mat}, ro)


# ── Vehicles ─────────────────────────────────────────────────────────────────────────────────────────────────
STARSHIP_REFS = [
    "Wikipedia 'SpaceX Starship' (Block 3): stack 124.4 m, Super Heavy 72.3 m, both stages 9 m across, 33 Raptors "
    "on the booster (13 gimballed in the centre and middle rings: 3 + 10, 20 fixed outside), the ship 3 sea-level "
    "+ 3 vacuum Raptors, three grid fins on Block 3 boosters (four before), forward flaps ~140 deg apart from Block 2",
    "SpaceX Starship page (spacex.com/vehicles/starship): ship 52 m; Raptor sea-level nozzle ~1.3 m, RVac ~2.3 m "
    "across",
]


def starship_booster(b, z0=0.0):
    r, L = 4.5, 72.3
    top = z0 + L
    b.profile("booster_body", [[z0, r], [top - 0.4, r], [top, r - 0.05]], "steel", caps=True)
    # Integrated hot-staging ring: a vented band at the top (dark vents between steel posts).
    b.tube("hot_stage_ring", r + 0.03, top - 2.6, top - 0.5, "black", ro=True)
    for k in range(24):
        b.radial_box("hot_stage_post", 7.5 + 15.0 * k, r, top - 1.55, 0.06, 0.45, 2.1, "steel")
    b.disc("forward_dome", r - 0.08, top + 0.02, "grey")
    # Three grid fins (Block 3: larger, three), deployed, 120 deg apart; their hinge boxes.
    for k in range(3):
        phi = 90.0 + 120.0 * k
        b.radial_box("grid_fin", phi, r + 0.35, top - 6.0, 3.6, 4.0, 0.5, "gridfin")
        b.radial_box("grid_fin_hinge", phi, r, top - 6.0, 0.4, 1.4, 1.6, "steel")
    # Chines along the lower tank and the electrical raceway.
    for phi in (0.0, 180.0):
        b.fin("chine", phi, r, z0 + 18.0, 26.0, 26.0, 0.45, 0.0, 0.2, "steel", ro=True, bevel=0.3)
    b.radial_box("raceway", 90.0, r, z0 + 38.0, 0.35, 0.7, 64.0, "steel")
    # Aft: the heat shield, 33 Raptor 3 (3 centre / 10 middle / 20 outer), bells below the skirt.
    b.disc("aft_heat_shield", r - 0.02, z0 - 0.01, "black")
    # (Exits 1.65 m below: each throat just under the heat shield, so the cap cuts no bell.)
    b.engines("engines", [(3, 0.85), (10, 2.3), (20, 3.85)], 0.62, 2.4, z0 - 1.65)
    return top


def starship_ship(b, z0=0.0):
    r, L = 4.5, 52.1
    nose_len = 13.5
    zb = z0 + L - nose_len
    tps = ("tiles", 176.0, 364.0)  # the windward half (-Y = 270 deg) and a little past the flap hinges
    # The aft skirt (2026-10-20, issue 20261020_181510): the hull runs on below the aft heat shield as an open
    # skirt round the engines, its bottom (z0, the track point) about flush with the RVac exits. The heat shield
    # sits above every bell's throat, so no surface cuts through a bell (a cap at z0 sliced the bells at mid-length
    # and their glowing interiors read as crescents). The photometric barrel starts at the heat shield.
    hs = 3.3
    b.profile("ship_barrel", [[z0 + hs, r], [zb, r]], "steel", caps="bottom", arc=tps)
    b.pieces("ship_nose", ogive(r, nose_len, zb, tip_r=1.0, n=16), "steel", [zb + 0.45 * nose_len], caps=False, arc=tps)
    b.profile("aft_skirt", [[z0, r], [z0 + hs, r]], "steel", ro=True, arc=tps, inner="black")
    b.disc("aft_heat_shield", r - 0.03, z0 + hs - 0.01, "black")
    # Aft flaps: straight trailing edge at the skirt, the leading edge swept; tiled underneath (windward).
    for phi, mat, back in ((-8.0, "steel", "tiles"), (188.0, "tiles", "steel")):
        b.fin("aft_flap", phi, r - 0.15, z0 + 6.4, 10.0, 4.8, 4.6, 5.2, 0.75, mat, back, tip_thick=0.35)
    # Forward flaps (Block 2+): smaller, shifted leeward (~140 deg apart), on the nose's slope.
    for phi, mat, back in ((20.0, "steel", "tiles"), (160.0, "tiles", "steel")):
        h = 5.4  # their root's middle above the barrel
        rho = (r * r + nose_len * nose_len) / (2 * r)
        rr = math.sqrt(rho * rho - h * h) + r - rho
        b.fin("fwd_flap", phi, rr - 0.1, zb + h, 7.4, 3.4, 3.4, 3.4, 0.55, mat, back, slope_deg=13.0,
              tip_thick=0.3)
    # Chines along the heat shield's edges between the flaps, and the leeward raceway.
    for phi in (-4.0, 184.0):
        b.fin("chine", phi, r, z0 + 24.0, 27.0, 27.0, 0.35, 0.0, 0.18, "steel", ro=True, bevel=0.3)
    b.radial_box("raceway", 90.0, r, z0 + 19.0, 0.3, 0.6, 20.0, "steel")
    b.radial_box("raceway", 90.0, r, z0 + 35.6, 0.3, 0.6, 5.6, "steel")
    # The payload door (2026-10-20, issue 20261019_223922): the "PEZ" slot on the leeward side the satellites leave
    # through one at a time, 31.1 m up (launches.json door_m), open: a dark slot 6.6 m across and 2.6 m tall
    # (the widest stowed payload, Starlink V3 with its array packs, and the thickest, a folded Starmind).
    b.profile("payload_door", [[z0 + 29.8, r + 0.025], [z0 + 32.4, r + 0.025]], "soot", ro=True)
    b.c[-1]["sweep_deg"] = [43.0, 137.0]
    # 3 sea-level Raptors in the middle, 3 RVacs outside them, 60 deg apart: the RVac exits just inside the skirt's
    # bottom, the sea-level bells recessed within it; every throat below the heat shield (hs).
    b.engines("engines", [(3, 1.05)], 0.62, 2.4, z0 + 0.8, phase=30.0)
    b.engines("engines_rvac", [(3, 3.1)], 1.15, 4.4, z0 + 0.15, phase=90.0)
    return z0 + L


VEHICLES = {
    # Generic launchers. Lengths (m) from the aft end: core = the first stage's body incl. interstage (`inter` =
    # the interstage's share), upper = the upper stage's exposed tank, fairing = cyl + ogive, `fr` its radius.
    "falcon9": dict(
        r=1.83, core=41.2, core_mat="white", inter=(6.0, "black"),
        eng=dict(rings=[(1, 0.0), (8, 1.32)], re=0.46, L=1.7, z=-1.2), aft="black",
        grid=(4, 1.25, 1.5, 0.18, "gridfin"), grid_stowed=True, legs=(4, 9.6, 0.75, "black"),
        upper=dict(len=9.4, mat="white", eng=dict(rings=[(1, 0.0)], re=1.6, L=4.4, protrude=3.7)),
        fairing=dict(fr=2.6, cyl=6.7, nose=6.4, tip=0.35),
        refs=["SpaceX Falcon User's Guide (2021): fairing 13.1 m tall, 5.2 m across; vehicle 70 m, 3.7 m across",
              "Wikipedia 'Falcon 9 Block 5': 9 Merlin 1D in the octaweb (8 around 1), MVac upper stage, four "
              "titanium grid fins, four carbon landing legs"],
        notes="Interstage share (6 m), leg size, the MVac's 3.2 m exit and the stage split are estimates from "
              "photographs."),
    "falcon9_dragon": dict(
        r=1.83, core=41.2, core_mat="white", inter=(6.0, "black"),
        eng=dict(rings=[(1, 0.0), (8, 1.32)], re=0.46, L=1.7, z=-1.2), aft="black",
        grid=(4, 1.25, 1.5, 0.18, "gridfin"), grid_stowed=True, legs=(4, 9.6, 0.75, "black"),
        upper=dict(len=9.4, mat="white", eng=dict(rings=[(1, 0.0)], re=1.6, L=4.4, protrude=3.7)),
        dragon=True,
        refs=["SpaceX Dragon page: 8.1 m tall with trunk, 4 m across; trunk 3.7 m with body-mounted solar cells "
              "on half its circumference and four fins",
              "Wikipedia 'Falcon 9 Block 5' / 'SpaceX Dragon 2'"],
        notes="Capsule side-wall angle and nosecone shape from photographs."),
    "new_glenn": dict(
        r=3.5, core=57.5, core_mat="white", inter=(4.0, "grey"),
        eng=dict(rings=[(1, 0.0), (6, 2.45)], re=0.85, L=2.6, z=-1.9), aft="black",
        strakes=(2, 9.0, 1.5), fwd_fins=(4, 2.6, 1.6, 1.4),
        legs=(6, 8.0, 0.9, "grey"),
        upper=dict(len=19.0, mat="white", eng=dict(rings=[(2, 1.3)], re=1.0, L=3.2, protrude=2.6)),
        fairing=dict(fr=3.5, cyl=12.0, nose=9.9, tip=0.5),
        refs=["Blue Origin New Glenn page / Payload User's Guide: 98 m tall, 7 m across, fairing 7 m x 21.9 m",
              "Wikipedia 'New Glenn': 7 BE-4 on GS1, 2 BE-3U on GS2, six landing legs, four forward fins, aft "
              "strakes"],
        notes="Stage split, strake and fin sizes estimated from photographs."),
    "vulcan": dict(
        r=2.7, core=33.3, core_mat="tan", inter=(3.6, "white"),
        eng=dict(rings=[(2, 1.3)], re=0.85, L=2.6, z=-1.9), aft="grey",
        side=dict(n=2, r=0.8, len=21.9, mat="white", nose=2.6, nose_kind="ogive", re=0.62, eL=1.9, cant=5.0,
                  phase=90.0),
        upper=dict(len=11.7, mat="white", eng=dict(rings=[(2, 0.9)], re=0.55, L=2.3, protrude=1.8)),
        fairing=dict(fr=2.7, cyl=9.5, nose=6.0, tip=0.35),
        refs=["ULA Vulcan Centaur page / User's Guide (2023): 5.4 m booster 33.3 m, Centaur V 11.7 m, GEM 63XL "
              "1.6 m x 22 m, 5.4 m fairing 15.5 m (standard)",
              "Wikipedia 'Vulcan Centaur': 2 BE-4, 2 RL10C-X, 0/2/4/6 GEM 63XL"],
        notes="Shown as VC2 (two solids). Interstage adapter length estimated."),
    "ariane6": dict(
        r=2.7, core=30.0, core_mat="white", inter=(2.0, "white"),
        eng=dict(rings=[(1, 0.0)], re=1.05, L=3.4, z=-2.6), aft="grey",
        side=dict(n=4, r=1.7, len=13.5, mat="white", nose=4.0, nose_kind="ogive", re=1.1, eL=2.4, cant=0.0,
                  phase=45.0, lift=2.0),
        upper=dict(len=11.6, mat="white", eng=dict(rings=[(1, 0.0)], re=1.07, L=3.3, protrude=2.6)),
        fairing=dict(fr=2.7, cyl=13.0, nose=7.0, tip=0.4),
        refs=["Arianespace Ariane 6 User's Manual (2021): 5.4 m core, P120C boosters 3.4 m x 13.5 m, long "
              "fairing 20 m, A64 62 m",
              "Wikipedia 'Ariane 6': Vulcain 2.1, Vinci"],
        notes="Shown as A64. Core and upper-stage split estimated."),
    "long_march_8a": dict(
        r=1.675, core=29.0, core_mat="white", inter=(2.0, "white"),
        eng=dict(rings=[(2, 0.75)], re=0.5, L=1.8, z=-1.3), aft="grey",
        side=dict(n=2, r=1.125, len=26.0, mat="white", nose=3.4, nose_kind="cone", re=0.5, eL=1.8, cant=0.0,
                  phase=90.0),
        upper=dict(len=10.0, mat="white", eng=dict(rings=[(2, 0.55)], re=0.42, L=1.6, protrude=1.3)),
        fairing=dict(fr=2.6, cyl=8.5, nose=5.5, tip=0.3),
        refs=["Wikipedia 'Long March 8': 3.35 m core, two 2.25 m boosters, 2 YF-100 on the core, 1 per booster, "
              "a hydrolox second stage (2 YF-75), 5.2 m fairing"],
        notes="Booster nose and fairing lengths estimated from photographs (the fairing long enough for Guowang's two tiers of five, 2026-10-20)."),
    "long_march_12": dict(
        r=1.9, core=40.0, core_mat="white", inter=(2.0, "black"),
        eng=dict(rings=[(4, 1.1)], re=0.52, L=1.9, z=-1.4), aft="black",
        grid=(4, 1.2, 1.5, 0.15, "gridfin"), legs=(4, 9.0, 0.7, "black"),
        upper=dict(len=12.0, mat="white", eng=dict(rings=[(2, 0.6)], re=0.45, L=1.7, protrude=1.4)),
        fairing=dict(fr=2.1, cyl=6.0, nose=5.0, tip=0.3),
        refs=["Wikipedia 'Long March 12': 3.8 m stages, 4 YF-100K, 2 YF-115 on the second stage, 4.2 m fairing"],
        notes="Shown with grid fins and legs (the reusable variant). Lengths estimated."),
    "long_march_6a": dict(
        r=1.675, core=28.0, core_mat="white", inter=(2.0, "white"),
        eng=dict(rings=[(2, 0.75)], re=0.5, L=1.8, z=-1.3), aft="grey",
        side=dict(n=4, r=1.0, len=16.0, mat="white", nose=3.0, nose_kind="cone", re=0.65, eL=1.6, cant=0.0,
                  phase=45.0),
        upper=dict(len=10.0, mat="white", eng=dict(rings=[(1, 0.0)], re=0.55, L=1.8, protrude=1.4)),
        fairing=dict(fr=2.1, cyl=6.0, nose=5.0, tip=0.3),
        refs=["Wikipedia 'Long March 6A': 3.35 m core (2 YF-100), four 2 m solid boosters, 1 YF-115 upper, "
              "4.2 m fairing, ~50 m"],
        notes="Booster length and nose estimated."),
    "long_march_5b": dict(
        r=2.5, core=33.0, core_mat="white", inter=(1.0, "white"),
        eng=dict(rings=[(2, 1.05)], re=0.75, L=2.6, z=-2.0), aft="grey",
        side=dict(n=4, r=1.675, len=27.0, mat="white", nose=5.5, nose_kind="cone", re=0.55, eL=1.9, cant=0.0,
                  phase=45.0, rings=[(2, 0.75)]),
        upper=None,
        fairing=dict(fr=2.6, cyl=12.0, nose=8.0, tip=0.4),
        refs=["Wikipedia 'Long March 5': 5 m core (2 YF-77), four 3.35 m boosters (2 YF-100 each) 27.6 m, 5B: no "
              "second stage, 5.2 m x 20.5 m fairing, 53.7 m"],
        notes="Booster nose cone length estimated."),
    "lvm3": dict(
        r=2.0, core=21.0, core_mat="white", inter=(2.0, "white"),
        eng=dict(rings=[(2, 0.9)], re=0.55, L=2.0, z=-1.5), aft="grey",
        side=dict(n=2, r=1.6, len=25.0, mat="white", nose=4.5, nose_kind="ogive", re=1.4, eL=3.0, cant=0.0,
                  phase=90.0, lift=-2.5),
        upper=dict(len=13.0, mat="white", eng=dict(rings=[(1, 0.0)], re=1.0, L=2.6, protrude=2.0)),
        fairing=dict(fr=2.5, cyl=6.0, nose=5.0, tip=0.35),
        refs=["ISRO LVM3 page / Wikipedia 'LVM3': L110 core 4 m (2 Vikas), two S200 3.2 m x 25 m, C25 4 m "
              "(CE-20), 5 m fairing, 43.4 m"],
        notes="S200 nozzle size and the strap-on's lower offset estimated."),
    "zhuque3": dict(
        r=2.25, core=46.0, core_mat="steel", inter=(3.0, "black"),
        eng=dict(rings=[(1, 0.0), (8, 1.55)], re=0.5, L=1.9, z=-1.4), aft="black",
        grid=(4, 1.4, 1.8, 0.2, "gridfin"), legs=(4, 10.0, 0.8, "black"),
        upper=dict(len=12.0, mat="steel", eng=dict(rings=[(1, 0.0)], re=1.0, L=2.8, protrude=2.2)),
        fairing=dict(fr=2.6, cyl=8.0, nose=6.0, tip=0.35),
        refs=["LandSpace / Wikipedia 'Zhuque-3': stainless steel, 4.5 m across, 66.1 m, 9 TQ-12A on the first "
              "stage, 1 TQ-15A, grid fins and four landing legs, 5.2 m fairing"],
        notes="Stage split estimated."),
    "tianlong3": dict(
        r=1.9, core=45.0, core_mat="white", inter=(3.0, "black"),
        eng=dict(rings=[(1, 0.0), (8, 1.32)], re=0.45, L=1.8, z=-1.3), aft="black",
        grid=(4, 1.2, 1.5, 0.15, "gridfin"), legs=(4, 9.0, 0.7, "black"),
        upper=dict(len=11.0, mat="white", eng=dict(rings=[(1, 0.0)], re=0.9, L=2.6, protrude=2.0)),
        fairing=dict(fr=2.1, cyl=7.0, nose=6.0, tip=0.3),
        refs=["Space Pioneer / Wikipedia 'Tianlong-3': 3.8 m across, ~71 m, 9 TH-12 engines, 4.2 m fairing"],
        notes="Stage split, grid fins and legs (planned reuse) estimated."),
    "pallas1": dict(
        r=1.675, core=30.0, core_mat="white", inter=(2.0, "black"),
        eng=dict(rings=[(1, 0.0), (6, 1.12)], re=0.42, L=1.6, z=-1.2), aft="black",
        grid=(4, 1.0, 1.3, 0.12, "gridfin"), legs=(4, 8.0, 0.6, "black"),
        upper=dict(len=9.0, mat="white", eng=dict(rings=[(1, 0.0)], re=0.8, L=2.2, protrude=1.7)),
        fairing=dict(fr=2.1, cyl=5.0, nose=4.5, tip=0.3),
        refs=["Galactic Energy / Wikipedia 'Pallas-1': 3.35 m, 7 Welkin engines, 4.2 m fairing"],
        notes="Lengths estimated."),
    # Crewed (the Missions window's crew rotations): the fairing is the spacecraft's shroud with an escape tower.
    "hlvm3": dict(
        r=2.0, core=21.0, core_mat="white", inter=(2.0, "white"),
        eng=dict(rings=[(2, 0.9)], re=0.55, L=2.0, z=-1.5), aft="grey",
        side=dict(n=2, r=1.6, len=25.0, mat="white", nose=4.5, nose_kind="ogive", re=1.4, eL=3.0, cant=0.0,
                  phase=90.0, lift=-2.5),
        upper=dict(len=13.0, mat="white", eng=dict(rings=[(1, 0.0)], re=1.0, L=2.6, protrude=2.0)),
        fairing=dict(fr=2.5, cyl=7.0, nose=4.0, tip=0.6), les=(0.45, 9.0), capsule="gaganyaan",
        refs=["ISRO / Wikipedia 'LVM3' (HLVM3, the human-rated variant): as LVM3, 53 m with the crew escape system; "
              "a crew-module fairing (CMF) over the Gaganyaan orbital module"],
        notes="Crew escape system and shroud lengths estimated from the TV-D1 / G1 imagery."),
    "long_march_2f": dict(
        r=1.675, core=28.5, core_mat="white", inter=(2.0, "white"),
        eng=dict(rings=[(4, 0.75)], re=0.42, L=1.7, z=-1.3, phase=45.0), aft="grey",
        side=dict(n=4, r=1.125, len=15.3, mat="white", nose=3.2, nose_kind="cone", re=0.42, eL=1.7, cant=0.0,
                  phase=45.0, fin=(2.5, 1.0, 1.4)),
        core_fins=(4, 3.0, 1.2, 1.8),
        upper=dict(len=11.4, mat="white", eng=dict(rings=[(1, 0.0)], re=0.55, L=1.9, protrude=1.4)),
        fairing=dict(fr=1.9, cyl=9.0, nose=4.5, tip=0.55), les=(0.32, 7.5), shroud_fins=4, capsule="shenzhou",
        refs=["Wikipedia 'Long March 2F': 3.35 m core (4 YF-20), four 2.25 m x 15.3 m boosters with fins, 3.8 m "
              "Shenzhou shroud with escape tower and four grid fins, 62 m"],
        notes="Escape tower length and shroud nose estimated."),
    "soyuz2": dict(
        r=1.475, core=27.1, core_mat="white", soyuz_core=True, inter=(0.0, "white"),
        eng=dict(rings=[(4, 0.45)], re=0.36, L=1.5, z=-1.1, phase=45.0), aft="grey",
        side=dict(n=4, r=1.34, len=19.6, mat="white", nose_kind="soyuz", re=0.36, eL=1.4, cant=0.0, phase=45.0,
                  rings=[(4, 0.45)], fin=(1.8, 0.9, 1.1)),
        upper=dict(len=6.7, mat="white", r=1.33, truss=1.6,
                   eng=dict(rings=[(4, 0.4)], re=0.26, L=1.0, protrude=0.6)),
        fairing=dict(fr=1.5, cyl=6.0, nose=4.0, tip=0.45), les=(0.28, 6.0), shroud_fins=4, capsule="soyuz",
        refs=["Wikipedia 'Soyuz-2': core (Block A) 2.95 m x 27.1 m, four tapered boosters 2.68 m x 19.6 m, Block I "
              "2.66 m x 6.7 m on an open truss, 46 m with escape tower; RD-107A / RD-108A four chambers each"],
        notes="Shown as the crewed configuration. Core narrowing and booster taper from drawings; verniers omitted."),
    # Cargo (2026-10-21, the visiting cargo ships): Progress MS under its own fairing (no escape tower), Tianzhou on
    # Long March 7.
    "soyuz2_progress": dict(
        r=1.475, core=27.1, core_mat="white", soyuz_core=True, inter=(0.0, "white"),
        eng=dict(rings=[(4, 0.45)], re=0.36, L=1.5, z=-1.1, phase=45.0), aft="grey",
        side=dict(n=4, r=1.34, len=19.6, mat="white", nose_kind="soyuz", re=0.36, eL=1.4, cant=0.0, phase=45.0,
                  rings=[(4, 0.45)], fin=(1.8, 0.9, 1.1)),
        upper=dict(len=6.7, mat="white", r=1.33, truss=1.6,
                   eng=dict(rings=[(4, 0.4)], re=0.26, L=1.0, protrude=0.6)),
        fairing=dict(fr=1.5, cyl=6.6, nose=3.6, tip=0.45), capsule="progress",
        refs=["Wikipedia 'Soyuz-2' / 'Progress MS': as the crewed Soyuz-2.1a, Progress under a 2.7-3 m fairing "
              "without an escape tower, ~46 m"],
        notes="Shown as the Progress configuration. Fairing length estimated from photographs."),
    "long_march_7": dict(
        r=1.675, core=26.7, core_mat="white", inter=(2.0, "white"),
        eng=dict(rings=[(2, 0.75)], re=0.5, L=1.8, z=-1.3), aft="grey",
        side=dict(n=4, r=1.125, len=26.3, mat="white", nose=3.6, nose_kind="cone", re=0.5, eL=1.8, cant=0.0,
                  phase=45.0),
        upper=dict(len=11.5, mat="white", eng=dict(rings=[(4, 0.75)], re=0.35, L=1.4, protrude=1.1)),
        fairing=dict(fr=2.1, cyl=10.4, nose=4.6, tip=0.4), capsule="tianzhou",
        refs=["Wikipedia 'Long March 7': 3.35 m core (2 YF-100) 26.7 m, four 2.25 m boosters (1 YF-100 each) 26.3 m, "
              "a 3.35 m second stage (4 YF-115), 4.2 m fairing, 53.1 m; Tianzhou flies from Wenchang LC-201"],
        notes="Booster nose and fairing lengths estimated from photographs (the fairing long enough for Tianzhou)."),
}


def core_parts(b, v, z0=0.0, stack=True):
    r, L = v["r"], v["core"]
    top = z0 + L
    mat = v["core_mat"]
    if v.get("soyuz_core"):
        # Block A: narrow at the base, widest under the top where the boosters' noses meet it, a neck above.
        pts = [[z0, 1.03], [z0 + 18.0, 1.40], [z0 + 21.0, r], [z0 + 23.5, r], [z0 + 24.6, 1.2],
               [top, 1.2]]
        b.pieces("core_body", pts, mat, [z0 + 18.0, z0 + 23.5])
    else:
        b.profile("core_body", [[z0, r], [top, r]], mat, caps=True)
    inter, imat = v["inter"]
    if inter > 0 and imat != mat:
        b.tube("interstage", r * 1.004 + 0.003, top - inter, top, imat, ro=True)
    b.disc("aft_heat_shield", r * 0.99, z0 - 0.01, v.get("aft", "black"))
    e = v["eng"]
    b.engines("engines", e["rings"], e["re"], e["L"], z0 + e["z"], phase=e.get("phase", 22.5 if len(e["rings"]) > 1 else 0.0))
    g = v.get("grid")
    if g:
        n, sr, st, sz, gm = g
        for k in range(n):
            phi = 45.0 + 360.0 * k / n
            if stack and v.get("grid_stowed"):
                b.radial_box("grid_fin", phi, r + 0.02, top - 1.6, sr, st, sz, gm, tilt_deg=88.0)
            else:
                b.radial_box("grid_fin", phi, r + 0.12, top - 0.8, sr, st, sz, gm)
                b.radial_box("grid_fin_hinge", phi, r, top - 0.8, 0.15, 0.5, 0.6, "grey")
    legs = v.get("legs")
    if legs:
        # Landing legs (2026-10-20): drawn DEPLOYED (the rest pose), hinged at the base and swung out and down so
        # their feet stand level with the engine exits; a "fold" turns each back up flat against the body for the
        # flight (the launch code sets the unfolding progress: deployed in the last seconds before touchdown).
        n, ll, w, lm = legs
        zh = z0 + 0.25 * ll                             # the hinge (above the engine section), at the leg's lower end
        z_foot = z0 + e["z"] - 0.25                     # the feet, a little below the engine exits
        alpha = math.acos(max(-1.0, min(1.0, (z_foot - zh) / ll)))   # from straight up, outward
        for k in range(n):
            phi = 360.0 * k / n
            p = math.radians(phi)
            er = (math.cos(p), math.sin(p), 0.0)
            et = (-math.sin(p), math.cos(p), 0.0)
            ax = (math.sin(alpha) * er[0], math.sin(alpha) * er[1], math.cos(alpha))     # along the leg, to the foot
            xr = (math.cos(alpha) * er[0], math.cos(alpha) * er[1], -math.sin(alpha))   # its outward face
            hx = r + 0.16
            hinge = (hx * er[0], hx * er[1], zh)
            fold = {"point": r4(list(hinge)), "axis": r4(list(et)), "angle_deg": round(-math.degrees(alpha), 4),
                    "phase": [0.0, 1.0]}
            q = quat_from_basis(xr, et, ax)
            for name, d0, sz, sr, st in (("leg", 0.5 * ll, ll, 0.32, w), ("leg_foot", ll - 0.3, 0.6, 0.5, w * 1.3)):
                c = [hinge[i] + d0 * ax[i] for i in range(3)]
                b.add({"name": name, "type": "box", "size": r4([sr, st, sz]), "position": r4(c),
                       "rotation_quat": q, "material": lm, "fold": dict(fold)}, True)
    if v.get("strakes"):
        n, ln, sp = v["strakes"]
        for k in range(n):
            b.fin("strake", 90.0 + 180.0 * k, r, z0 + 0.5 * ln + 1.0, ln, ln * 0.55, sp, ln * 0.45, 0.35, "grey",
                  ro=True)
    if v.get("fwd_fins"):
        n, rc, tc, sp = v["fwd_fins"]
        for k in range(n):
            b.fin("fwd_fin", 45.0 + 360.0 * k / n, r, top - v["inter"][0] - 1.5, rc, tc, sp, rc - tc, 0.3, "grey",
                  ro=True)
    if v.get("core_fins"):
        n, rc, tc, sp = v["core_fins"]
        for k in range(n):
            b.fin("core_fin", 360.0 * k / n, r, z0 + 0.5 * rc, rc, tc, sp, rc - tc, 0.18, mat, ro=True)
    if stack and v.get("side"):
        side_parts(b, v)
    return top


def side_parts(b, v, z0=0.0):
    s, r = v["side"], v["r"]
    lift = s.get("lift", 0.0)
    for k in range(s["n"]):
        phi = s["phase"] + 360.0 * k / s["n"]
        p = math.radians(phi)
        if s["nose_kind"] == "soyuz":
            # A tapered cone, widest at the base, closing to a point on the core's waist.
            d = 1.03 + s["r"] + 0.02
            x, y = d * math.cos(p), d * math.sin(p)
            pts = [[z0, s["r"]], [z0 + 0.5 * s["len"], 0.7 * s["r"]], [z0 + 0.85 * s["len"], 0.32 * s["r"]],
                   [z0 + s["len"], 0.0]]
            # Lean the booster in so its tip meets the core: tilt about the tangential axis.
            lean = math.atan2(d - 1.2, s["len"])
            er = (math.cos(p), math.sin(p), 0.0)
            et = (-math.sin(p), math.cos(p), 0.0)
            zax = (-math.sin(lean) * er[0], -math.sin(lean) * er[1], math.cos(lean))
            xax = (math.cos(lean) * er[0], math.cos(lean) * er[1], math.sin(lean))
            b.pieces("side_booster", pts, s["mat"], [z0 + 0.5 * s["len"]], x=x, y=y, rot=quat_from_basis(xax, et, zax))
        else:
            d = r + s["r"] + 0.15
            x, y = d * math.cos(p), d * math.sin(p)
            zb = z0 + lift
            zt = zb + s["len"]
            if s["nose_kind"] == "ogive":
                pts = [[zb, s["r"]]] + ogive(s["r"], s["nose"], zt, tip_r=0.15 * s["r"], n=8)
            else:
                pts = [[zb, s["r"]], [zt, s["r"]], [zt + s["nose"], 0.08 * s["r"]], [zt + s["nose"] + 0.05, 0.0]]
            b.pieces("side_booster", pts, s["mat"], [zt], x=x, y=y)
            b.disc("side_aft", s["r"] * 0.98, zb - 0.01, "grey", x, y)
        rings = s.get("rings", [(1, 0.0)])
        zb = z0 + lift
        b.engines("side_nozzle", rings, s["re"], s["eL"], zb - 0.75 * s["eL"], x=x, y=y, cant=s.get("cant", 0.0),
                  phase=phi)
        if s.get("fin"):
            rc, tc, sp = s["fin"]
            # The stabiliser fin on the booster's outboard side.
            fx = x + s["r"] * math.cos(p) * (0.6 if s["nose_kind"] == "soyuz" else 0.98)
            fy = y + s["r"] * math.sin(p) * (0.6 if s["nose_kind"] == "soyuz" else 0.98)
            rr = math.hypot(fx, fy)
            b.fin("side_fin", phi, rr, zb + 0.5 * rc + 0.3, rc, tc, sp, rc - tc, 0.12, "grey", ro=True)


def fairing_profile(v, z):
    """The fairing's meridian from the top of what it sits on (z) up: [[z, r], ...] and the cut heights."""
    u = v.get("upper")
    f = v["fairing"]
    fr = f["fr"]
    base_r = u.get("r", v["r"]) if u else v["r"]
    pts = []
    if fr > base_r + 0.05:
        bt = 0.9 * (fr - base_r) + 0.5
        pts += [[z + 0.02, base_r], [z + bt, fr]]
        z += bt
    else:
        pts += [[z + 0.02, fr]]
    pts += [[z + f["cyl"], fr]]
    z += f["cyl"]
    cuts = [pts[1][0]] if len(pts) > 2 else []
    pts += ogive(fr, f["nose"], z, tip_r=f["tip"], n=12)[1:]
    return pts, cuts + [z, z + 0.45 * f["nose"]], z + f["nose"]


def upper_parts(b, v, z0=0.0, mode="full", info=None):
    """The upper stage from its nozzle exit (z0) up, then the fairing (or spacecraft). Returns the top.
    mode: "full" with its fairing; "bare" the fairing gone (2026-10-20: separated at ~110 km), the payload adapter
    on top (and a crewed vehicle's spacecraft); "empty" a crewed vehicle's stage once its spacecraft has left;
    "fairing" ONE fairing half alone (azimuth -90..90 deg, the half toward +X; the other is it turned 180 deg about
    Z), at the place it had on the stage. info (dict): gets payload_base (z of the payload adapter's top), the
    fairing's base, radius and cylinder length."""
    u = v.get("upper")
    r = v["r"]
    z = z0
    draw = mode != "fairing"
    if u:
        ru = u.get("r", r)
        e = u["eng"]
        if draw:
            # The engine(s): bell exit at z0, the bell's upper part inside the tank's aft skirt.
            b.engines("upper_nozzle", e["rings"], e["re"], e["L"], z0, mat="nozzle")
        z = z0 + e["protrude"]
        if u.get("truss"):
            # Soyuz: Block I sits on an open truss over the core's neck; the tank above.
            if draw:
                b.tube("upper_truss", ru * 0.95, z, z + u["truss"], "lattice", ro=True)
            z += u["truss"]
        if draw:
            b.profile("upper_tank", [[z, ru], [z + u["len"], ru]], u["mat"], caps=True)
            b.disc("upper_aft", ru * 0.98, z - 0.01, "grey")
        z += u["len"]
    else:
        # Long March 5B: the fairing sits on the core; bare, a short stub of the core's top stands for it.
        z = z0
        if mode in ("bare", "empty"):
            b.profile("core_top", [[z - 3.0, r], [z, r]], v["core_mat"], caps=True)
    if v.get("dragon"):
        if mode == "empty":
            # (2026-10-20) The Dragon has left: the stage's top and the trunk's separation ring.
            b.tube("dragon_sep_ring", 1.83, z, z + 0.12, "grey", ro=True)
            return z + 0.12
        if info is not None:
            info["spacecraft_top"] = dragon_parts(Builder(), z)
        return dragon_parts(b, z)
    if info is not None:
        info["fairing_base"] = z
        info["fr"] = v["fairing"]["fr"]
        info["cyl"] = v["fairing"]["cyl"]
        info["nose"] = v["fairing"]["nose"]
    if mode in ("bare", "empty"):
        # The payload adapter: a dark cone from the tank's top to a 1.6-m separation ring.
        base_r = u.get("r", r) if u else r
        ra = min(0.8, 0.9 * base_r)
        b.profile("payload_adapter", [[z + 0.01, 0.97 * base_r], [z + 0.55, ra], [z + 0.6, ra]], "black", ro=True,
                  caps="top")
        b.tube("separation_ring", ra + 0.03, z + 0.5, z + 0.62, "grey", ro=True)
        if info is not None:
            info["payload_base"] = z + 0.62
        if mode == "bare" and v.get("capsule"):
            top = capsule_parts(b, v["capsule"], z + 0.62)
            if info is not None:
                info["spacecraft_top"] = top
            return top
        return z + 0.62
    pts, cuts, top = fairing_profile(v, z)
    if mode == "fairing":
        b.pieces("fairing_half", pts, "white", cuts, caps=False)
        for c in b.c:
            c["sweep_deg"] = [-90.0, 90.0]
            c["inner_material"] = "grey"
        if v.get("shroud_fins"):
            f = v["fairing"]
            for k in range(v["shroud_fins"]):
                phi = 45.0 + 90.0 * k
                if -90.0 < ((phi + 180.0) % 360.0) - 180.0 < 90.0:
                    b.radial_box("shroud_grid_fin", phi, f["fr"], z + f["cyl"] - 1.2, 0.9, 1.1, 0.15, "gridfin")
        # The model's origin at the half's centre (what Go to rides and it tumbles about); its place on the stage is
        # launches.json's "fairing_center_m" (the stage frame).
        cx, cz = 0.5 * v["fairing"]["fr"], 0.5 * (pts[0][0] + top)
        for c in b.c:
            if c["type"] == "profile":
                c["profile"] = [r4([q[0] - cz, q[1]]) for q in c["profile"]]
                c["position"] = r4([c["position"][0] - cx, c["position"][1], c["position"][2]])
            else:
                c["position"] = r4([c["position"][0] - cx, c["position"][1], c["position"][2] - cz])
        if info is not None:
            info["fairing_center"] = [round(cx, 2), 0.0, round(cz, 2)]
        return top
    b.pieces("fairing", pts, "white", cuts)
    z = top - v["fairing"]["nose"]
    if v.get("les"):
        # Launch escape tower: a slim tower over the shroud nose with its motor nozzles' ring.
        tr, tl = v["les"]
        zt = top - 0.6
        b.profile("escape_tower", [[zt, tr], [zt + 0.75 * tl, tr], [zt + 0.75 * tl, tr * 1.25],
                                   [zt + 0.88 * tl, tr * 1.25], [zt + tl, 0.0]], "white", ro=True, caps=True)
        top = zt + tl
    if v.get("shroud_fins"):
        for k in range(v["shroud_fins"]):
            b.radial_box("shroud_grid_fin", 45.0 + 90.0 * k, v["fairing"]["fr"], z - 1.2, 0.9, 1.1, 0.15, "gridfin")
    return top


def capsule_parts(b, kind, z):
    """A crewed spacecraft on its adapter (shown once its shroud is gone): Shenzhou, Soyuz MS or Gaganyaan, stand-ins
    of public dimensions (service module, descent module, orbital module; the arrays folded). deployed: in orbit,
    the arrays out (tools/make_crew_models.py: the spacecraft on its own); cm_only: the descent / crew module alone
    (after the others separate for the entry). Returns the top (the docking port)."""
    if kind == "gaganyaan":
        # Service module ~2.9 m x 3.9 m wide, crew module ~3.1 m tall, 3.5 m across at the heat shield (published
        # figures, rounded; the docking port is a future addition: on top, as on Bharatiya's BDS).
        b.profile("service_module", [[z, 1.95], [z + 2.9, 1.95]], "white", caps=True)
        b.engines("sm_engines", [(5, 1.2)], 0.14, 0.4, z - 0.3, mat="nozzle")
        for k in range(2):
            b.radial_box("sm_array_folded", 90.0 + 180.0 * k, 1.95, z + 1.45, 0.2, 2.0, 2.6, "cells")
        z += 2.9
        b.profile("crew_module", [[z, 1.75], [z + 0.25, 1.75], [z + 2.7, 0.75], [z + 3.1, 0.55]], "white",
                  caps=True)
        return z + 3.1
    if kind == "shenzhou":
        # Service module 2.8 m x 2.9 m, descent module (bell) 2.5 m, orbital module 2.25 m x 2.8 m.
        b.profile("service_module", [[z, 1.4], [z + 2.9, 1.4]], "white", caps=True)
        b.engines("sm_engines", [(4, 0.6)], 0.15, 0.4, z - 0.3, mat="nozzle")
        for k in range(2):
            b.radial_box("sm_array_folded", 90.0 + 180.0 * k, 1.4, z + 1.45, 0.25, 2.0, 2.6, "cells")
        z += 2.9
        b.profile("descent_module", [[z, 1.25], [z + 0.3, 1.25], [z + 2.0, 0.6], [z + 2.05, 0.5]], "grey",
                  caps=True)
        z += 2.05
        b.profile("orbital_module", dome(1.1, 0.4, z + 0.4, up=False) + [[z + 2.4, 1.1]] +
                  dome(1.1, 0.4, z + 2.4, up=True)[1:], "white", caps=False)
        return z + 2.8
    if kind == "progress":
        # Progress MS (2026-10-21): Soyuz MS's service module, a refuelling module, the cargo module; arrays folded.
        b.profile("service_module", [[z, 1.36], [z + 2.5, 1.36], [z + 2.6, 1.1]], "grey", caps=True)
        b.engines("sm_engine", [(1, 0.0)], 0.3, 0.5, z - 0.3, mat="nozzle")
        for k in range(2):
            b.radial_box("sm_array_folded", 90.0 + 180.0 * k, 1.36, z + 1.3, 0.2, 1.8, 2.2, "cells")
        z += 2.6
        b.profile("refuelling_module", [[z, 1.05], [z + 1.2, 1.05], [z + 1.6, 0.7]], "grey", caps=True)
        z += 1.6
        b.profile("cargo_module", [[z, 0.6], [z + 0.4, 1.1], [z + 2.2, 1.1], [z + 2.65, 0.5]], "green", caps=True)
        return z + 2.65
    if kind == "tianzhou":
        # Tianzhou (2026-10-21): propulsion module aft, cargo module forward; wings folded against the propulsion module.
        b.profile("propulsion_module", [[z, 1.4], [z + 3.3, 1.4], [z + 3.9, 1.675]], "white", caps=True)
        b.engines("pm_engines", [(4, 0.7)], 0.18, 0.5, z - 0.3, mat="nozzle")
        for k in range(2):
            b.radial_box("pm_array_folded", 90.0 + 180.0 * k, 1.4, z + 1.6, 0.25, 2.0, 2.8, "cells")
        z += 3.9
        b.profile("cargo_module", [[z, 1.675], [z + 5.6, 1.675], [z + 6.3, 0.85]], "white", caps=True)
        return z + 6.3
    # Soyuz MS: service module 2.7 m x 2.5 m, descent module 2.17 m, orbital module 2.25 m (a sphere-ish).
    b.profile("service_module", [[z, 1.36], [z + 2.5, 1.36], [z + 2.6, 1.1]], "grey", caps=True)
    b.engines("sm_engine", [(1, 0.0)], 0.3, 0.5, z - 0.3, mat="nozzle")
    for k in range(2):
        b.radial_box("sm_array_folded", 90.0 + 180.0 * k, 1.36, z + 1.3, 0.2, 1.8, 2.2, "cells")
    z += 2.6
    b.profile("descent_module", [[z, 1.08], [z + 0.2, 1.08], [z + 1.9, 0.5], [z + 2.0, 0.3]], "grey", caps=True)
    z += 2.0
    b.profile("orbital_module", [[z, 0.4]] + [[z + 1.1 - 1.1 * math.cos(math.pi * k / 8), 1.1 * math.sin(math.pi * k / 8) + 0.001]
                                            for k in range(1, 8)] + [[z + 2.2, 0.4]], "green", caps=True)
    return z + 2.2


def dragon_parts(b, z):
    """Crew Dragon: the trunk (half of it solar cells, four fins), then the capsule and its nosecone."""
    tr, tl = 1.83, 3.7
    b.profile("dragon_trunk", [[z + 0.02, tr], [z + tl, tr]], "white", caps=True, arc=("cells", 200.0, 340.0))
    for k in range(4):
        b.fin("trunk_fin", 45.0 + 90.0 * k, tr, z + 1.1, 2.0, 1.0, 0.7, 1.0, 0.1, "white", ro=True)
    z += tl
    cap = [[z + 0.02, 2.0], [z + 0.25, 2.0], [z + 2.95, 1.25], [z + 3.15, 1.12]] + \
        ogive(1.12, 1.3, z + 3.15, tip_r=0.5, n=6)[1:]
    b.pieces("dragon_capsule", cap, "white", [z + 3.15])
    b.disc("dragon_heat_shield", 1.98, z + 0.03, "black")
    for k in range(4):
        b.radial_box("superdraco_pod", 45.0 + 90.0 * k, 1.75 - 0.25, z + 1.2, 0.25, 0.6, 0.9, "black")
    return z + 3.15 + 1.3


# ── Payload stacks (2026-10-20, issue 20261019_225243) ─────────────────────────────────────────────────────────────
# data/launches.json's programs carry a "stack": how their satellites sit on the upper stage under the fairing (or in
# Starship's bay). The sim draws them there (SatelliteSimLaunches.cpp, launchStackJobs) and releases them in sequence.
# This script measures each satellite model STOWED (its "stow" scaling and "fold" applied, as sat_mesh.vert does),
# derives the layout's numbers (pitch, offsets, the member centre, the column radius), checks they fit every fairing
# the program flies in, writes the dispensers (rocket_dispenser_<name>.json) and prints the numbers launches.json must
# hold (--check fails if they differ).
LAUNCHES = os.path.join(os.path.dirname(__file__), "..", "data", "launches.json")
CONSTELLATIONS = os.path.join(os.path.dirname(__file__), "..", "data", "constellations.json")
BAY_RADIUS, BAY_LENGTH = 4.1, 17.24  # Starship: SpaceX's payload user's guide (2020): 8 m usable, 17.24 m cylinder;
# the bay of a PEZ ship has no fairing-like dynamic envelope, so 8.2 m across of its ~8.8 m inner skin is allowed
BAY_TAPER = 4.8                      # + the usable part of the nose (to the guide's 22 m)
FAIRING_WALL = 0.15                  # a fairing's skin, acoustic blankets and the dynamic envelope: inner = outer - this
STACK_GAP = 0.06                     # between neighbours in a stack


def quat_rot(q, v):
    w, x, y, z = q
    tx, ty, tz = 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])
    return [v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx)]


def euler_quat(e):
    def aa(axis, a):
        h = math.radians(a) / 2
        return [math.cos(h)] + [math.sin(h) * c for c in axis]

    def mul(a, b):
        w1, x1, y1, z1 = a
        w2, x2, y2, z2 = b
        return [w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2, w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
                w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2, w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2]
    return mul(mul(aa([1, 0, 0], e[0]), aa([0, 1, 0], e[1])), aa([0, 0, 1], e[2]))


def rodrigues(v, k, a):
    c, s_ = math.cos(a), math.sin(a)
    d = sum(k[i] * v[i] for i in range(3))
    cr = [k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0]]
    return [v[i] * c + cr[i] * s_ + k[i] * d * (1 - c) for i in range(3)]


def stowed_points(path):
    """Sample points of a satellite model with every part stowed and folded (rest body frame), as sat_mesh.vert."""
    m = json.load(open(path, encoding="utf-8"))
    groups = {g["name"]: g for g in m["attitude_groups"]}
    names = [g["name"] for g in m["attitude_groups"]]

    def hinge(gname):
        g = groups[gname]
        if "parent" in g:
            h = g.get("hinge", {}).get("position", [0, 0, 0])
            ph = hinge(g["parent"])
            return [h[i] + ph[i] for i in range(3)]
        return [0.0, 0.0, 0.0]
    out = []
    for c in m["components"]:
        t = c["type"]
        if t == "box":
            sx, sy, sz = [a / 2 for a in c["size"]]
            loc = [[a, b, d] for a in (-sx, sx) for b in (-sy, sy) for d in (-sz, sz)]
        elif t == "plane":
            w, h = c["size"][0] / 2, c["size"][1] / 2
            loc = [[a, b, 0.0] for a in (-w, 0.0, w) for b in (-h, 0.0, h)]
        elif t in ("cylinder", "cone"):
            r0 = c.get("radius", c.get("radius_bottom", 0.1))
            r1 = c.get("radius", c.get("radius_top", r0))
            hh = c["height"] / 2
            loc = [[r * math.cos(a), r * math.sin(a), z] for (r, z) in ((r0, -hh), (r1, hh))
                   for a in [k * math.pi / 8 for k in range(16)]]
        elif t == "sphere":
            r = c["radius"]
            loc = [[r * a, r * b, r * d] for (a, b, d) in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1),
                                                         (0, 0, -1))]
        else:
            continue
        q = c.get("rotation_quat") or (euler_quat(c["rotation_deg"]) if "rotation_deg" in c else [1, 0, 0, 0])
        pos = c.get("position", [0, 0, 0])
        g = c["group"]
        hg = hinge(g if isinstance(g, str) else names[g])
        st, fo = c.get("stow"), c.get("fold")
        for p in loc:
            v = quat_rot(q, p)
            v = [v[i] + pos[i] + hg[i] for i in range(3)]
            if st:
                ax = st["axis"]
                n = math.sqrt(sum(a * a for a in ax))
                ax = [a / n for a in ax]
                d = sum(ax[i] * v[i] for i in range(3)) - st.get("root", 0.0)
                if d > 0:
                    v = [v[i] - ax[i] * (1 - st.get("scale", 0.05)) * d for i in range(3)]
            if fo:
                ax = fo["axis"]
                n = math.sqrt(sum(a * a for a in ax))
                ax = [a / n for a in ax]
                pt = fo.get("point", [0, 0, 0])
                r = rodrigues([v[i] - pt[i] for i in range(3)], ax, math.radians(fo.get("angle_deg", 0.0)))
                v = [pt[i] + r[i] for i in range(3)]
            out.append(v)
    return out


def fairing_radius_at(v, z_rel):
    """The fairing's outer radius z_rel above its base (the vehicle's dict); 0 past its tip."""
    pts, _, _ = fairing_profile(v, 0.0)
    for (z0, r0), (z1, r1) in zip(pts, pts[1:]):
        if z0 <= z_rel <= z1:
            return r0 + (r1 - r0) * (z_rel - z0) / max(z1 - z0, 1e-9)
    return 0.0


def r3(x):
    return round(x, 3)


def plan_stacks(infos):
    """Per program with a stack: its numbers (to put in launches.json), the fit in each of its vehicles, and the
    dispenser's dimensions. infos: vehicle id -> the upper-stage info (payload_base, fairing_base, fr, cyl, nose)."""
    L = json.load(open(LAUNCHES, encoding="utf-8"))
    C = json.load(open(CONSTELLATIONS, encoding="utf-8"))
    tmodel = {t["name"]: t.get("model") for t in C["satellite_types"]}
    plans = []
    for pg in L["programs"]:
        st = pg.get("stack")
        if not st:
            continue
        # Every model the program flies (Starlink: V2 Mini and V3): the stack is laid out for the largest.
        ids = []
        for c in C["constellations"]:
            if any(mm in c["name"] for mm in pg["match"]):
                mid = tmodel.get(c["type"])
                if mid and mid not in ids:
                    ids.append(mid)
        if not ids:
            continue
        model_id = "+".join(ids)
        lo, hi = [1e9] * 3, [-1e9] * 3
        corner_pez = 0.0
        for mid in ids:
            pts = stowed_points(os.path.join(OUT, mid + ".json"))
            l1 = [min(p[i] for p in pts) for i in range(3)]
            h1 = [max(p[i] for p in pts) for i in range(3)]
            corner_pez = max(corner_pez, 0.5 * math.hypot(h1[0] - l1[0], h1[1] - l1[1]))
            lo = [min(lo[i], l1[i]) for i in range(3)]
            hi = [max(hi[i], h1[i]) for i in range(3)]
        ext = [hi[i] - lo[i] for i in range(3)]
        cen = [r3(0.5 * (hi[i] + lo[i])) for i in range(3)]
        lay = st["layout"]
        n = pg["payload"]
        P = {"program": pg["name"], "model": model_id, "layout": lay, "extent": [r3(e) for e in ext],
             "member_center": cen, "payload": n, "fits": {}}
        if lay == "pez":
            pitch = ext[2] + STACK_GAP
            corner = corner_pez  # (each model about its own centre)
            P["pitch_m"] = r3(pitch)
            cap = int((BAY_LENGTH + BAY_TAPER) // pitch)
            P["fits"]["starship"] = {"corner_radius_m": r3(corner), "bay_radius_m": BAY_RADIUS,
                                     "stack_length_m": r3(n * pitch), "bay_length_m": BAY_LENGTH + BAY_TAPER,
                                     "capacity": cap if corner <= BAY_RADIUS else 0,
                                     "ok": corner <= BAY_RADIUS and n <= cap}
            # The slot the satellites leave through (the ship model's door): the stowed member's width across the
            # ship (its Y) and thickness (its Z).
            P["slot_w"], P["slot_h"] = r3(ext[1]), r3(ext[2])
        elif lay == "flat":
            per = st.get("per_tier", 1)
            side = st.get("side_axis", "y")
            si = 0 if side == "x" else 1
            pitch = ext[2] + STACK_GAP
            off = 0.5 * (ext[si] + STACK_GAP) if per == 2 else 0.0
            tiers = (n + per - 1) // per
            P.update({"pitch_m": r3(pitch), "offset_m": r3(off), "tiers": tiers})
            # Footprint corners in the stage's cross-section: the member's X along stage Y, Y along stage X.
            hx, hy = 0.5 * ext[0], 0.5 * ext[1]
            if per == 2:
                if si == 0:
                    corner = math.hypot(off + hx, hy)
                else:
                    corner = math.hypot(hx, off + hy)
            else:
                corner = math.hypot(hx, hy)
            P["corner_m"] = r3(corner)
            height = st.get("base_m", 0.3) + tiers * pitch
            for vid in pg["vehicles"]:
                P["fits"][vid] = fit_column(vid, infos, corner, height, st.get("base_m", 0.3))
        elif lay == "column":
            per = st["per_tier"]
            # Zenith face on the column, Z radially out, X along the stage, Y tangential.
            half_t = 0.5 * ext[1] + 0.5 * STACK_GAP
            r_in = half_t / math.tan(math.pi / per)  # where neighbours' inner corners meet
            col = st.get("column_radius_m") or 0.0
            r_face = max(r_in, 0.0)
            col = r3(max(0.35, r_face - 0.05))
            radius = r_face + 0.5 * ext[2]  # the member centre's distance from the axis
            pitch = ext[0] + STACK_GAP
            tiers = (n + per - 1) // per
            corner = math.hypot(r_face + ext[2], 0.5 * ext[1])
            P.update({"pitch_m": r3(pitch), "offset_m": r3(radius), "column_radius_m": col, "tiers": tiers,
                      "corner_m": r3(corner)})
            height = st.get("base_m", 0.3) + tiers * pitch
            for vid in pg["vehicles"]:
                P["fits"][vid] = fit_column(vid, infos, corner, height, st.get("base_m", 0.3))
        P["height_m"] = r3(st.get("base_m", 0.3) + P.get("tiers", 0) * P.get("pitch_m", 0))
        plans.append(P)
    return plans


def fit_column(vid, infos, corner, height, base):
    """Whether a stack `height` tall needing `corner` of radius fits vid's fairing (from the payload adapter up)."""
    v = VEHICLES.get(vid)
    inf = infos.get(vid)
    if not v or not inf or "fairing" not in v:
        return {"ok": False, "why": "no fairing"}
    z0 = inf["payload_base"] - inf["fairing_base"]
    worst = 1e9
    z = z0 + base
    while z <= z0 + height + 1e-9:
        worst = min(worst, fairing_radius_at(v, z) - FAIRING_WALL - corner)
        z += 0.1
    usable = inf["cyl"] + inf["nose"]
    return {"ok": worst >= 0.0, "margin_m": r3(worst), "inner_radius_m": r3(v["fairing"]["fr"] - FAIRING_WALL),
            "fairing_length_m": r3(usable + (inf["payload_base"] - inf["fairing_base"]) * 0.0)}


def dispenser_model(P, st):
    """The dispenser left on the stage after the satellites go: a column with a ring per tier and the mounting
    fittings (Guowang, OneWeb), or a base plate and four tension rods (a flat stack)."""
    b = Builder()
    h = P["height_m"]
    if P["layout"] == "column":
        rc = P["column_radius_m"]
        b.profile("column", [[0.0, rc + 0.15], [0.15, rc], [h, rc], [h + 0.05, rc * 0.8]], "grey", caps="top")
        per, pitch, base = st["per_tier"], P["pitch_m"], st.get("base_m", 0.3)
        for k in range(P["tiers"] + 1):
            b.tube("tier_ring", rc + 0.06, base + k * pitch - 0.05, base + k * pitch + 0.05, "black", ro=True)
        for k in range(P["tiers"]):
            for j in range(per):
                phi = 360.0 * j / per + (180.0 / per if k % 2 else 0.0)
                for dz in (0.25, 0.75):
                    b.radial_box("mount_fitting", phi, rc, base + (k + dz) * pitch, 0.12, 0.25, 0.12, "black")
        notes = "A central column the satellites are mounted around, %d to a tier (tier rings, separation fittings)." % per
    else:
        r_plate = P["corner_m"] + 0.05
        b.profile("base_plate", [[0.0, r_plate], [0.12, r_plate]], "black", caps=True)
        for k in range(4):
            phi = 45.0 + 90.0 * k
            p = math.radians(phi)
            rr = 0.92 * r_plate
            b.profile("tension_rod", [[0.12, 0.04], [h, 0.04]], "grey", x=rr * math.cos(p), y=rr * math.sin(p),
                      ro=True, caps=True)
        notes = "A flat stack's base plate and four tension rods (released all at once, the rods fall away)."
    return b.c, notes


def model(vid, part, refs, notes, comps):
    phot = [c["name"] for c in comps if not c.get("render_only")]
    used = sorted({c["material"] for c in comps} | {c.get("arc_material") for c in comps if c.get("arc_material")} |
                  {c.get("back_material") for c in comps if c.get("back_material")} |
                  {c.get("inner_material") for c in comps if c.get("inner_material")})
    mats = [m for m in MATERIALS if m["name"] in used]
    note = ("Launch vehicle render model (tools/make_rocket_models.py), %s: %s. Body frame: +Z toward the nose, "
            "origin at the aft end; engine bells below it. %s" % (vid, part, notes))
    return {
        "name": "%s %s" % (vid, part),
        "note": note,
        "materials": mats,
        "attitude_groups": [{"name": "body", "primary": {"axis": [0, 0, 1], "target": "velocity"},
                             "secondary": {"axis": [0, -1, 0], "target": "nadir"}}],
        "components": comps,
        "sources": [
            {"subject": "attitude", "status": "estimate",
             "value": "+Z along the thrust axis (the sim passes it as 'velocity'), -Y (the ship's heat shield) down",
             "source": "A rocket flies along its thrust axis; a gravity turn and an entry fly belly down."},
            {"subject": "body", "status": "estimate", "value": "one rigid group",
             "source": "Render model; no moving parts."},
            {"subjects": phot, "status": "sourced",
             "value": "diameters and lengths rounded from the references; shapes (ogives, flaps, tapers) "
                      "estimated from photographs",
             "source": " | ".join(refs)},
            {"subjects": [m["name"] for m in mats], "status": "estimate",
             "value": "; ".join("%s: %s" % (m["name"], MATERIAL_SOURCES[m["name"]]) for m in mats),
             "source": "Liveries from photographs; reflectance from the presets."},
        ],
    }


def write(vid, part, refs, notes, comps):
    path = os.path.join(OUT, "rocket_%s_%s.json" % (vid, part))
    with open(path, "w", encoding="utf-8") as f:
        json.dump(model(vid, part, refs, notes, comps), f, indent=1)


def main():
    import sys
    n = 0
    # Starship.
    notes = ("Ogive nose 13.5 m with a 1 m blunt tip, flap planforms, chines and engine heights are estimates "
             "from photographs of Block 2/3 vehicles.")
    b = Builder()
    top = starship_booster(b)
    starship_ship(b, top + 0.02)
    write("starship", "stack", STARSHIP_REFS, notes, b.c)
    b = Builder()
    starship_booster(b)
    write("starship", "booster", STARSHIP_REFS, notes, b.c)
    b = Builder()
    starship_ship(b)
    write("starship", "upper", STARSHIP_REFS, notes, b.c)
    n += 3
    infos = {}
    # A crewed vehicle's spacecraft: its docking port's contact point (the origin of tools/make_crew_models.py's
    # rocket_capsule_* models: the probe / ring on top) above the upper stage's origin, launches.json spacecraft_dock_m.
    tops = {}
    for vid, v in VEHICLES.items():
        refs, notes = v["refs"], v.get("notes", "")
        u = v.get("upper")
        b = Builder()
        top = core_parts(b, v, 0.0, stack=True)
        upper_parts(b, v, top + 0.02 - (u["eng"]["protrude"] if u else 0.0))
        write(vid, "stack", refs, notes, b.c)
        b = Builder()
        core_parts(b, v, 0.0, stack=False)
        write(vid, "booster", refs, notes, b.c)
        b = Builder()
        uinfo = {}
        upper_parts(b, v, 0.0, info=uinfo)
        write(vid, "upper", refs, notes, b.c)
        n += 3
        if v.get("dragon"):
            # 2026-10-20: the stage after the Dragon has separated (crew rotations: the capsule flies on its own).
            b = Builder()
            upper_parts(b, v, 0.0, mode="empty")
            write(vid, "upper_empty", refs, notes + " After its spacecraft has separated.", b.c)
            n += 1
            tops[vid] = uinfo["spacecraft_top"] - 1.0  # the docking ring under the closed nose cone
        if "fairing" in v and not v.get("dragon"):
            # 2026-10-20: the fairing separates (~110 km); the stage flies on bare, its payload on the adapter.
            info = {}
            b = Builder()
            upper_parts(b, v, 0.0, mode="bare", info=info)
            write(vid, "upper_bare", refs, notes + " Shown after fairing separation.", b.c)
            b = Builder()
            upper_parts(b, v, 0.0, mode="fairing", info=info)
            write(vid, "fairing", refs, notes + " One fairing half (the other is it turned 180 deg about +Z), in the "
                  "stage's frame: separated, it falls away tumbling.", b.c)
            n += 2
            if v.get("capsule"):
                tops[vid] = info["spacecraft_top"] + (0.3 if v["capsule"] in ("gaganyaan", "tianzhou") else 0.35)
                b = Builder()
                upper_parts(b, v, 0.0, mode="empty")
                write(vid, "upper_empty", refs, notes + " After its spacecraft has separated.", b.c)
                n += 1
            infos[vid] = info
    print("wrote %d models" % n)
    print("\nvehicle payload_base_m (launches.json):")
    for vid, inf in infos.items():
        print("  %-16s payload_base_m %.2f   fairing_center_m %s   fairing: r %.2f, base %.2f, cylinder %.1f + nose %.1f" %
              (vid, inf["payload_base"], inf["fairing_center"], inf["fr"], inf["fairing_base"], inf["cyl"], inf["nose"]))
    plans = plan_stacks(infos)
    L = json.load(open(LAUNCHES, encoding="utf-8"))
    bad = []
    for P in plans:
        print("\n%s (%s, %s, %d a launch): stowed %s m, centre %s" % (P["program"], P["model"], P["layout"],
              P["payload"], P["extent"], P["member_center"]))
        keys = [k for k in ("pitch_m", "offset_m", "column_radius_m", "tiers", "corner_m", "height_m") if k in P]
        print("   " + ", ".join("%s %s" % (k, P[k]) for k in keys))
        for vid, f in P["fits"].items():
            print("   %-16s %s" % (vid, "FITS" if f.get("ok") else "DOES NOT FIT") + "  " +
                  ", ".join("%s %s" % (k, v) for k, v in f.items() if k != "ok"))
            if not f.get("ok"):
                bad.append((P["program"], vid))
        st = next(p for p in L["programs"] if p["name"] == P["program"])["stack"]
        for k in ("pitch_m", "offset_m", "column_radius_m", "member_center"):
            if k in P and k in st and st[k] != P[k]:
                bad.append((P["program"], "launches.json %s %s != %s" % (k, st[k], P[k])))
        if st.get("dispenser"):
            comps, dn = dispenser_model(P, st)
            write("dispenser", st["dispenser"], ["Generated from data/launches.json's stack layout."], dn, comps)
    for vh in L["vehicles"]:
        if vh["id"] in infos and abs(vh.get("payload_base_m", -1) - round(infos[vh["id"]]["payload_base"], 2)) > 0.005:
            bad.append((vh["id"], "launches.json payload_base_m %s != %.2f" % (vh.get("payload_base_m"),
                                                                                infos[vh["id"]]["payload_base"])))
        if vh["id"] in infos and vh.get("fairing_center_m") != infos[vh["id"]]["fairing_center"]:
            bad.append((vh["id"], "launches.json fairing_center_m %s != %s" % (vh.get("fairing_center_m"),
                                                                               infos[vh["id"]]["fairing_center"])))
    print("\nvehicle spacecraft_dock_m (launches.json, crewed vehicles):")
    for vid, top in tops.items():
        print("  %-16s spacecraft_dock_m %.2f" % (vid, top))
    for vh in L["vehicles"]:
        if vh["id"] in tops and abs(vh.get("spacecraft_dock_m", -1) - round(tops[vh["id"]], 2)) > 0.005:
            bad.append((vh["id"], "launches.json spacecraft_dock_m %s != %.2f" % (vh.get("spacecraft_dock_m"),
                                                                                  tops[vh["id"]])))
    if bad:
        print("\nCHECK:", *bad, sep="\n  ")
        if "--check" in sys.argv:
            sys.exit(1)


if __name__ == "__main__":
    main()
