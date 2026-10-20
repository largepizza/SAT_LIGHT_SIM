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
    {"name": "gridfin", "preset": "truss", "coverage": 0.55, "truss_pitch": 0.5, "color": [0.35, 0.35, 0.37]},
    {"name": "lattice", "preset": "truss", "coverage": 0.3, "truss_pitch": 0.7},
    {"name": "cells", "preset": "solar_cell"},
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
    "cells": "Crew Dragon trunk's body-mounted solar cells",
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
    b.engines("engines", [(3, 0.85), (10, 2.3), (20, 3.85)], 0.62, 2.4, z0 - 1.5)
    return top


def starship_ship(b, z0=0.0):
    r, L = 4.5, 52.1
    nose_len = 13.5
    zb = z0 + L - nose_len
    tps = ("tiles", 176.0, 364.0)  # the windward half (-Y = 270 deg) and a little past the flap hinges
    b.profile("ship_barrel", [[z0, r], [zb, r]], "steel", caps="bottom", arc=tps)
    b.pieces("ship_nose", ogive(r, nose_len, zb, tip_r=1.0, n=16), "steel", [zb + 0.45 * nose_len], caps=False, arc=tps)
    b.disc("aft_skirt_plate", r - 0.03, z0 - 0.01, "grey")
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
    b.radial_box("raceway", 90.0, r, z0 + 24.0, 0.3, 0.6, 30.0, "steel")
    # 3 sea-level Raptors in the middle, 3 RVacs outside them, 60 deg apart.
    b.engines("engines", [(3, 1.05)], 0.62, 2.4, z0 - 0.9, phase=30.0)
    b.engines("engines_rvac", [(3, 3.1)], 1.15, 4.4, z0 - 0.5, phase=90.0)
    return z0 + L


VEHICLES = {
    # Generic launchers. Lengths (m) from the aft end: core = the first stage's body incl. interstage (`inter` =
    # the interstage's share), upper = the upper stage's exposed tank, fairing = cyl + ogive, `fr` its radius.
    "falcon9": dict(
        r=1.83, core=41.2, core_mat="white", inter=(6.0, "black"),
        eng=dict(rings=[(1, 0.0), (8, 1.32)], re=0.46, L=1.7, z=-1.1), aft="black",
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
        eng=dict(rings=[(1, 0.0), (8, 1.32)], re=0.46, L=1.7, z=-1.1), aft="black",
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
        fairing=dict(fr=2.6, cyl=6.0, nose=5.0, tip=0.3),
        refs=["Wikipedia 'Long March 8': 3.35 m core, two 2.25 m boosters, 2 YF-100 on the core, 1 per booster, "
              "a hydrolox second stage (2 YF-75), 5.2 m fairing"],
        notes="Booster nose and fairing lengths estimated from photographs."),
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
    "long_march_2f": dict(
        r=1.675, core=28.5, core_mat="white", inter=(2.0, "white"),
        eng=dict(rings=[(4, 0.75)], re=0.42, L=1.7, z=-1.3, phase=45.0), aft="grey",
        side=dict(n=4, r=1.125, len=15.3, mat="white", nose=3.2, nose_kind="cone", re=0.42, eL=1.7, cant=0.0,
                  phase=45.0, fin=(2.5, 1.0, 1.4)),
        core_fins=(4, 3.0, 1.2, 1.8),
        upper=dict(len=11.4, mat="white", eng=dict(rings=[(1, 0.0)], re=0.55, L=1.9, protrude=1.4)),
        fairing=dict(fr=1.9, cyl=9.0, nose=4.5, tip=0.55), les=(0.32, 7.5), shroud_fins=4,
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
        fairing=dict(fr=1.5, cyl=6.0, nose=4.0, tip=0.45), les=(0.28, 6.0), shroud_fins=4,
        refs=["Wikipedia 'Soyuz-2': core (Block A) 2.95 m x 27.1 m, four tapered boosters 2.68 m x 19.6 m, Block I "
              "2.66 m x 6.7 m on an open truss, 46 m with escape tower; RD-107A / RD-108A four chambers each"],
        notes="Shown as the crewed configuration. Core narrowing and booster taper from drawings; verniers omitted."),
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
        n, ll, w, lm = legs
        for k in range(n):
            phi = 360.0 * k / n
            b.radial_box("leg", phi, r, z0 + 0.4 + 0.5 * ll, 0.32, w, ll, lm)
            b.radial_box("leg_foot", phi, r, z0 + 0.3, 0.5, w * 1.3, 0.6, lm)
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


def upper_parts(b, v, z0=0.0):
    """The upper stage from its nozzle exit (z0) up, then the fairing (or spacecraft). Returns the top."""
    u = v.get("upper")
    r = v["r"]
    z = z0
    if u:
        ru = u.get("r", r)
        e = u["eng"]
        # The engine(s): bell exit at z0, the bell's upper part inside the tank's aft skirt.
        b.engines("upper_nozzle", e["rings"], e["re"], e["L"], z0, mat="nozzle")
        z = z0 + e["protrude"]
        if u.get("truss"):
            # Soyuz: Block I sits on an open truss over the core's neck; the tank above.
            b.tube("upper_truss", ru * 0.95, z, z + u["truss"], "lattice", ro=True)
            z += u["truss"]
        b.profile("upper_tank", [[z, ru], [z + u["len"], ru]], u["mat"], caps=True)
        b.disc("upper_aft", ru * 0.98, z - 0.01, "grey")
        z += u["len"]
    else:
        # Long March 5B: the fairing sits on the core.
        z = z0
    if v.get("dragon"):
        return dragon_parts(b, z)
    f = v["fairing"]
    fr = f["fr"]
    base_r = u.get("r", r) if u else r
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
    b.pieces("fairing", pts, "white", cuts + [z, z + 0.45 * f["nose"]])
    top = z + f["nose"]
    if v.get("les"):
        # Launch escape tower: a slim tower over the shroud nose with its motor nozzles' ring.
        tr, tl = v["les"]
        zt = top - 0.6
        b.profile("escape_tower", [[zt, tr], [zt + 0.75 * tl, tr], [zt + 0.75 * tl, tr * 1.25],
                                   [zt + 0.88 * tl, tr * 1.25], [zt + tl, 0.0]], "white", ro=True, caps=True)
        top = zt + tl
    if v.get("shroud_fins"):
        for k in range(v["shroud_fins"]):
            b.radial_box("shroud_grid_fin", 45.0 + 90.0 * k, fr, z - 1.2, 0.9, 1.1, 0.15, "gridfin")
    return top


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
        upper_parts(b, v, 0.0)
        write(vid, "upper", refs, notes, b.c)
        n += 3
    print("wrote %d models" % n)


if __name__ == "__main__":
    main()
