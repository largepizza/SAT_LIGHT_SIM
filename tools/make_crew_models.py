"""Writes the visiting spacecraft's render models: data/satellite_models/rocket_capsule_<kind>[_cm|_chute].json.

Crew spacecraft (Crew Dragon, Shenzhou, Soyuz MS, Gaganyaan) and, since 2026-10-21, the cargo ships (Progress MS,
Tianzhou; Cargo Dragon flies the Crew Dragon model). A cargo ship that burns up on re-entry has only the in-orbit
model.

The sim flies a crewed spacecraft as its own object once it separates from its upper stage (2026-10-20,
SatelliteSimLaunches.cpp "Crew vehicles"): phasing, the approach to its station, docked, undocking, the deorbit
burn, entry, parachutes and splashdown / landing. Three models per spacecraft:

  rocket_capsule_<kind>.json        the whole spacecraft in orbit (arrays deployed; Dragon's nose cone open)
  rocket_capsule_<kind>_cm.json     the crew / descent module alone (the trunk / service and orbital modules are
                                    jettisoned before entry)
  rocket_capsule_<kind>_chute.json  the descent module under its main parachute(s) (render-only canopies)

Body frame: +Z out of the DOCKING PORT, the ORIGIN AT THE PORT's contact point (the sim docks it by putting the
origin on the station's port with +Z into the station), the vehicle hanging below along -Z. The same origin in all
three, so the switch from one to the next does not move the descent module. The "rocket_" prefix makes the sim load
them with the launch vehicles (rocketTypes) and the accuracy gate validate them with the rockets.

    python tools/make_crew_models.py

Dimensions are public figures, rounded (each model's `sources` say which); shapes are stand-ins.
"""

import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import make_rocket_models as rm  # noqa: E402  (Builder, materials, helpers)

OUT = rm.OUT

REFS = {
    "dragon": ["SpaceX Dragon page: 8.1 m tall with trunk, 4 m across; trunk 3.7 m with body-mounted solar cells on "
               "half its circumference; the nose cone hinges open to expose the docking adapter (IDSS)",
               "Wikipedia 'SpaceX Dragon 2': four main parachutes, ~35 m across"],
    "shenzhou": ["Wikipedia 'Shenzhou (spacecraft)': service module 2.8 m x 2.9 m, descent module 2.5 m across, "
                 "orbital module 2.25 m x 2.8 m with the docking mechanism forward, two pairs of service-module "
                 "solar wings (~17 m tip to tip); one main parachute (1200 m2)"],
    "soyuz": ["Wikipedia 'Soyuz MS': service module 2.72 m wide, 2.58 m long; descent module 2.17 m; orbital module "
              "2.25 m with the SSVP probe forward; arrays 10.6 m tip to tip; one main parachute (1000 m2)"],
    "progress": ["Wikipedia 'Progress MS' / RSC Energia: 7.2 m long, 2.72 m across; instrumentation-propulsion module "
                 "as Soyuz MS's, a refuelling module (tanks) in place of the descent module, a cargo module (~2.2 m "
                 "across) with the SSVP probe forward; two arrays 10.6 m tip to tip; ~7 t at launch"],
    "tianzhou": ["Wikipedia 'Tianzhou (spacecraft)' / CMSA: 10.6 m long, 3.35 m across; a cargo module forward with the "
                 "docking mechanism, a propulsion module aft carrying two wings of three panels (14.9 m tip to tip); "
                 "13.5 t at launch"],
    "cygnus": ["Northrop Grumman / Wikipedia 'Cygnus (spacecraft)' (Enhanced): pressurized cargo module 3.07 m across, "
               "~5.1 m long, with the CBM forward; service module ~1.3 m with two UltraFlex circular arrays ~3.8 m "
               "across; berthed by Canadarm2 at Unity nadir; ~7.5 t at launch; destroyed on re-entry",
               "NASA 'Cygnus capture' imagery: the grapple fixture on the cargo module's side; arrays face along the "
               "axis"],
    "htvx": ["JAXA HTV-X: ~8 m long, 4.4 m across; pressurized module forward with the CBM, service module aft "
             "carrying deployable solar wings; berthed by Canadarm2 at Harmony nadir; ~16 t at launch, 4+ t cargo; "
             "destroyed on re-entry"],
    "dreamchaser": rm.DREAMCHASER_REFS,
    "gaganyaan": ["ISRO / Wikipedia 'Gaganyaan': crew module ~3.1 m tall, ~3.5 m across; service module ~2.9 m, "
                  "3.9 m across, with deployable solar panels; parachutes: three mains"],
}
NOTES = {
    "dragon": "Crew Dragon in orbit: the trunk (cells on half of it, four fins), the capsule, the docking ring; the "
              "nose cone hinged open forward.",
    "shenzhou": "Shenzhou in orbit: orbital module forward (docking), descent module, service module and its two "
                "pairs of wings.",
    "soyuz": "Soyuz MS in orbit: orbital module forward (docking), descent module, service module and its two wings.",
    "progress": "Progress MS in orbit: cargo module forward (docking probe), refuelling module, instrumentation-"
                "propulsion module and its two wings.",
    "tianzhou": "Tianzhou in orbit: cargo module forward (docking ring), propulsion module aft and its two wings.",
    "cygnus": "Cygnus in orbit: the cargo module (CBM forward, grapple fixtures on both sides), the service module "
              "and its two round UltraFlex arrays.",
    "htvx": "HTV-X in orbit: the pressurized module (CBM forward, grapple fixtures on both sides), the service module "
            "and its two wings of three panels.",
    "dreamchaser": "Dream Chaser in orbit: the spaceplane (nose -Z, belly tiles -Y, wings deployed) with the Shooting Star "
                   "cargo module on its aft end (arrays out, grapple fixtures on both sides, the CBM on top: berthed by "
                   "the station's arm). The _cm model is the spaceplane alone (the module jettisoned before the deorbit).",
    "gaganyaan": "Gaganyaan in orbit: a docking port on the crew module's apex (an estimate: Gaganyaan G1 has none; "
                 "the scenario's Bharatiya crews dock), service module and two wings.",
}


def dragon(b, cm_only=False):
    """Dragon from its trunk's base at z = 0 up; returns the docking ring's top."""
    tr, tl = 1.83, 3.7
    z = 0.0
    if not cm_only:
        b.profile("dragon_trunk", [[z + 0.02, tr], [z + tl, tr]], "white", caps=True, arc=("cells", 200.0, 340.0))
        for k in range(4):
            b.fin("trunk_fin", 45.0 + 90.0 * k, tr, z + 1.1, 2.0, 1.0, 0.7, 1.0, 0.1, "white", ro=True)
    z += tl
    cap = [[z + 0.02, 2.0], [z + 0.25, 2.0], [z + 2.95, 1.25], [z + 3.15, 1.12]]
    b.pieces("dragon_capsule", cap, "white", [z + 2.95])
    b.disc("dragon_heat_shield", 1.98, z + 0.03, "black")
    for k in range(4):
        b.radial_box("superdraco_pod", 45.0 + 90.0 * k, 1.75 - 0.25, z + 1.2, 0.25, 0.6, 0.9, "black")
    top = z + 3.15
    # The docking adapter's ring and, in orbit, the nose cone hinged open forward (+X); closed for the entry.
    b.tube("docking_ring", 0.62, top, top + 0.3, "grey", ro=True, caps=True)
    if cm_only:
        b.profile("nose_cone", rm.ogive(1.12, 1.3, top, tip_r=0.5, n=6), "white", ro=True, caps="bottom")
        return top + 0.3
    hinge_x, ang = 1.05, math.radians(105.0)
    q = rm.quat_from_basis((math.cos(ang), 0.0, -math.sin(ang)), (0.0, 1.0, 0.0), (math.sin(ang), 0.0, math.cos(ang)))
    # The cone's base circle turned about the hinge (x = 1.05 at the ring's top): open, lying forward.
    cx = hinge_x - 1.12 * math.cos(ang)
    cz = top + 1.12 * math.sin(ang)
    nose = [[p[0] - top, p[1]] for p in rm.ogive(1.12, 1.3, top, tip_r=0.5, n=6)]
    c = b.profile("nose_cone_open", nose, "white", ro=True, caps="bottom", rot=q)
    c["position"] = rm.r4([cx, 0.0, cz])
    return top + 0.3


def soyuz_like(b, kind, cm_only=False):
    """Soyuz MS / Shenzhou from the service module's base at z = 0; returns the orbital module's docking port."""
    z = 0.0
    if kind == "shenzhou":
        sm_r, sm_l, dm, om_r, om_l, span, wing_w, mat_om = 1.4, 2.9, (1.25, 2.05), 1.1, 2.8, 7.2, 2.1, "white"
    else:
        sm_r, sm_l, dm, om_r, om_l, span, wing_w, mat_om = 1.36, 2.6, (1.08, 2.0), 1.1, 2.2, 4.1, 1.15, "green"
    if not cm_only:
        b.profile("service_module", [[z, sm_r], [z + sm_l - 0.1, sm_r], [z + sm_l, sm_r * 0.82]], "grey", caps=True)
        b.engines("sm_engines", [(4, 0.6)] if kind == "shenzhou" else [(1, 0.0)], 0.15 if kind == "shenzhou" else 0.3,
                  0.45, z - 0.3, mat="nozzle")
        for k in range(2):
            # A wing standing out from the service module: span radially, its face across the axis (2026-10-21: the
            # cells face along the vehicle's axis, as seen in photographs of an approaching Soyuz / Progress; the
            # first cut had them edge-on to it). Shenzhou's wings turn about their span to the Sun (group "wings").
            b.radial_box("solar_wing", 90.0 + 180.0 * k, sm_r + 0.4, z + 0.5 * sm_l, span, wing_w, 0.04, "cells",
                         ro=False)
            b.radial_box("wing_yoke", 90.0 + 180.0 * k, sm_r, z + 0.5 * sm_l, 0.45, 0.12, 0.3, "grey")
            if kind == "shenzhou":
                b.c[-2]["_wing"] = z + 0.5 * sm_l
                b.c[-1]["_wing"] = z + 0.5 * sm_l
    z += sm_l
    r0, h = dm
    b.profile("descent_module", [[z, r0], [z + 0.25, r0], [z + 0.9 * h, 0.55 * r0], [z + h, 0.35 * r0]], "grey",
              caps=True)
    b.disc("heat_shield", r0 * 0.98, z + 0.02, "black")
    z += h
    if cm_only:
        return z
    n = 8
    om = [[z + 0.05, 0.4]] + [[z + om_l * 0.5 * (1 - math.cos(math.pi * k / n)),
                               om_r * math.sin(math.pi * k / n) + 0.15] for k in range(1, n)] + [[z + om_l, 0.45]]
    b.profile("orbital_module", om, mat_om, caps=True)
    z += om_l
    b.tube("docking_probe", 0.35, z, z + 0.35, "grey", ro=True, caps=True)
    return z + 0.35


def progress(b):
    """Progress MS from the instrumentation-propulsion module's base at z = 0; returns the docking probe's tip."""
    z = 0.0
    b.profile("service_module", [[z, 1.36], [z + 2.5, 1.36], [z + 2.6, 1.1]], "grey", caps=True)
    b.engines("sm_engine", [(1, 0.0)], 0.3, 0.5, z - 0.3, mat="nozzle")
    for k in range(2):
        # Fixed wings, the cells facing along the axis (as Soyuz MS's; 2026-10-21: they were edge-on to it).
        b.radial_box("solar_wing", 90.0 + 180.0 * k, 1.36 + 0.4, z + 1.3, 4.1, 1.15, 0.04, "cells", ro=False)
        b.radial_box("wing_yoke", 90.0 + 180.0 * k, 1.36, z + 1.3, 0.45, 0.12, 0.3, "grey")
    z += 2.6
    b.profile("refuelling_module", [[z, 1.05], [z + 1.2, 1.05], [z + 1.6, 0.7]], "grey", caps=True)
    z += 1.6
    b.profile("cargo_module", [[z, 0.6], [z + 0.4, 1.1], [z + 2.2, 1.1], [z + 2.65, 0.5]], "green", caps=True)
    z += 2.65
    b.tube("docking_probe", 0.35, z, z + 0.35, "grey", ro=True, caps=True)
    return z + 0.35


def tianzhou(b):
    """Tianzhou from the propulsion module's base at z = 0; returns the docking ring's top."""
    z = 0.0
    b.profile("propulsion_module", [[z, 1.4], [z + 3.3, 1.4], [z + 3.9, 1.675]], "white", caps=True)
    b.engines("pm_engines", [(4, 0.7)], 0.18, 0.5, z - 0.3, mat="nozzle")
    for k in range(2):
        # The wings turn about their span to the Sun (a solar array drive; 2026-10-21: they were fixed edge-on to
        # the axis, a thin line seen from behind): rest pose facing along the axis, group "wings".
        b.radial_box("solar_wing", 90.0 + 180.0 * k, 1.4 + 0.6, z + 1.6, 5.6, 2.0, 0.04, "cells", ro=False)
        b.radial_box("wing_yoke", 90.0 + 180.0 * k, 1.4, z + 1.6, 0.65, 0.12, 0.3, "grey")
        b.c[-2]["_wing"] = z + 1.6
        b.c[-1]["_wing"] = z + 1.6
    z += 3.9
    b.profile("cargo_module", [[z, 1.675], [z + 5.6, 1.675], [z + 6.3, 0.85]], "white", caps=True)
    z += 6.3
    b.tube("docking_ring", 0.55, z, z + 0.3, "grey", ro=True, caps=True)
    return z + 0.3


def cbm(b, z):
    """A common berthing mechanism ring on top (its contact plane 0.35 m up: the model's origin)."""
    b.tube("cbm_ring", 1.0, z, z + 0.35, "grey", ro=True, caps=True)
    return z + 0.35


def grapple_fixtures(b, r, z):
    """Flight-releasable grapple fixtures on both sides (+-Y), gZ below the port: the arm takes the one facing it."""
    for k in range(2):
        b.radial_box("grapple_fixture", 90.0 + 180.0 * k, r, z, 0.25, 0.35, 0.35, "grey")
        b.radial_box("grapple_pin", 90.0 + 180.0 * k, r + 0.25, z, 0.15, 0.06, 0.06, "black")


def cygnus(b):
    """Cygnus from its service module's base at z = 0; returns the CBM's contact plane."""
    z = 0.0
    b.profile("service_module", [[z, 1.4], [z + 1.3, 1.4]], "grey", caps=True)
    b.engines("sm_engine", [(1, 0.0)], 0.3, 0.5, z - 0.3, mat="nozzle")
    for k in range(2):
        # UltraFlex arrays: round fans on booms either side of the service module, the cells facing along the axis.
        y = (1.4 + 0.35 + 1.9) * (1 if k == 0 else -1)
        b.add({"name": "ultraflex", "type": "cylinder", "radius": 1.9, "height": 0.04,
               "position": rm.r4([0.0, y, z + 0.65]), "material": "cells"})
        b.radial_box("array_boom", 90.0 + 180.0 * k, 1.4, z + 0.65, 0.4, 0.15, 0.15, "grey")
    z += 1.3
    b.profile("cargo_module", [[z, 1.535], [z + 4.6, 1.535], [z + 5.1, 1.0]], "white", caps=True)
    top = z + 5.1
    grapple_fixtures(b, 1.535, top + 0.35 - 2.5)
    return cbm(b, top)


def htvx(b):
    """HTV-X from its service module's base at z = 0; returns the CBM's contact plane."""
    z = 0.0
    b.profile("service_module", [[z, 2.2], [z + 3.6, 2.2]], "white", caps=True)
    b.engines("sm_engines", [(4, 1.2)], 0.2, 0.5, z - 0.3, mat="nozzle")
    for k in range(2):
        # Fixed wings of three panels, the cells facing along the axis.
        b.radial_box("solar_wing", 90.0 + 180.0 * k, 2.2 + 0.5, z + 1.8, 6.0, 1.7, 0.04, "cells", ro=False)
        b.radial_box("wing_yoke", 90.0 + 180.0 * k, 2.2, z + 1.8, 0.55, 0.15, 0.3, "grey")
    z += 3.6
    b.profile("pressurized_module", [[z, 2.2], [z + 3.6, 2.2], [z + 4.2, 1.0]], "white", caps=True)
    top = z + 4.2
    grapple_fixtures(b, 2.2, top + 0.35 - 2.0)
    return cbm(b, top)


CARGO = ("progress", "tianzhou", "cygnus", "htvx")
RUNWAY = ("dreamchaser",)  # (2026-10-21c) cargo, but it comes home on a runway: an in-orbit and a "_cm" model


def dreamchaser(b, cm_only=False):
    """Dream Chaser + Shooting Star from the spaceplane's nose at z = 0; returns the CBM's contact plane. cm_only: the
    spaceplane alone (the same origin: the full vehicle's port)."""
    if cm_only:
        return rm.dreamchaser_body(b, 0.0, folded=False, module=False)
    top = rm.dreamchaser_body(b, 0.0, folded=False, module=True)
    grapple_fixtures(b, 1.55, top + 0.35 - 2.2)
    return cbm(b, top)


def body(b, kind, cm_only=False):
    if kind == "dragon":
        return dragon(b, cm_only=cm_only)
    if kind == "gaganyaan":
        return gaganyaan(b, cm_only=cm_only)
    if kind == "progress":
        return progress(b)
    if kind == "tianzhou":
        return tianzhou(b)
    if kind == "cygnus":
        return cygnus(b)
    if kind == "htvx":
        return htvx(b)
    if kind == "dreamchaser":
        return dreamchaser(b, cm_only=cm_only)
    return soyuz_like(b, kind, cm_only=cm_only)


def gaganyaan(b, cm_only=False):
    z = 0.0
    if not cm_only:
        b.profile("service_module", [[z, 1.95], [z + 2.9, 1.95]], "white", caps=True)
        b.engines("sm_engines", [(5, 1.2)], 0.14, 0.4, z - 0.3, mat="nozzle")
        for k in range(2):
            b.radial_box("solar_wing", 90.0 + 180.0 * k, 1.95 + 0.3, z + 1.45, 4.2, 0.04, 1.6, "cells", ro=False)
            b.radial_box("wing_yoke", 90.0 + 180.0 * k, 1.95, z + 1.45, 0.35, 0.12, 0.3, "grey")
    z += 2.9
    b.profile("crew_module", [[z, 1.75], [z + 0.25, 1.75], [z + 2.7, 0.75], [z + 3.1, 0.55]], "white", caps=True)
    b.disc("heat_shield", 1.72, z + 0.02, "black")
    z += 3.1
    b.tube("docking_ring", 0.5, z, z + 0.3, "grey", ro=True, caps=True)
    return z + 0.3


# Canopy liveries (2026-10-21, the daylight review): alternating gores, from photographs of the recoveries.
CANOPY_COLORS = {"dragon": ("white", "chute_red"), "soyuz": ("chute_orange", "white"),
                 "shenzhou": ("chute_red", "white"), "gaganyaan": ("chute_orange", "white")}


def line(b, name, p0, p1, r):
    """A thin render-only line (a suspension line, a riser) from p0 to p1."""
    d = [p1[i] - p0[i] for i in range(3)]
    L = math.sqrt(sum(v * v for v in d))
    z = [v / L for v in d]
    ref = (1.0, 0.0, 0.0) if abs(z[0]) < 0.9 else (0.0, 1.0, 0.0)
    x = [ref[1] * z[2] - ref[2] * z[1], ref[2] * z[0] - ref[0] * z[2], ref[0] * z[1] - ref[1] * z[0]]
    n = math.sqrt(sum(v * v for v in x))
    x = [v / n for v in x]
    y = [z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]]
    c = b.profile(name, [[0.0, r], [L, r]], "grey", ro=True, rot=rm.quat_from_basis(x, y, z))
    c["position"] = rm.r4(p0)
    c["_line"] = True


def chutes(b, top, kind):
    """Render-only main canopies over the module (+Z up in descent): shells of alternating gores on their suspension
    lines, which gather at a confluence above the module (a riser from there to the module's top)."""
    if kind == "dragon":
        cans = [(17.0, 18.0, 42.0, a) for a in (45.0, 135.0, 225.0, 315.0)]
    elif kind == "gaganyaan":
        cans = [(12.0, 13.0, 38.0, a) for a in (90.0, 210.0, 330.0)]
    else:
        cans = [(0.0, 18.0, 40.0, 0.0)]
    c0, c1 = CANOPY_COLORS[kind]
    ngore, nline = 12, 16
    for off, rad, hgt, az in cans:
        x, y = off * math.cos(math.radians(az)), off * math.sin(math.radians(az))
        pts = [[top + hgt + rad * math.sin(math.pi * 0.5 * k / 6) * 0.55, rad * math.cos(math.pi * 0.5 * k / 6)]
               for k in range(7)]
        pts[-1][1] = 0.0
        for g in range(ngore):
            col = c0 if g % 2 == 0 else c1
            c = b.profile("canopy", pts, col, x=x, y=y, ro=True, inner=col)  # (the fabric shows its colour inside too)
            c["sweep_deg"] = [360.0 * g / ngore, 360.0 * (g + 1) / ngore]
        conf = [x * 0.6, y * 0.6, top + 0.3 * hgt]
        line(b, "riser", [0.0, 0.0, top], conf, 0.08)
        for k in range(nline):
            t = 2.0 * math.pi * k / nline
            line(b, "suspension_line", conf, [x + rad * math.cos(t), y + rad * math.sin(t), top + hgt], 0.06)


def shift(comps, dz):
    for c in comps:
        if c.pop("_line", False):
            # a line (its profile runs along its own rotated axis): only its start point moves
            c["position"] = rm.r4([c["position"][0], c["position"][1], c["position"][2] + dz])
        elif c["type"] == "profile":
            c["profile"] = [rm.r4([p[0] + dz, p[1]]) for p in c["profile"]]
            # A rotated profile's position carries its own z (the open nose cone).
            if c.get("rotation_quat"):
                c["position"] = rm.r4([c["position"][0], c["position"][1], c["position"][2] + dz])
        else:
            c["position"] = rm.r4([c["position"][0], c["position"][1], c["position"][2] + dz])


def build(kind, variant):
    b = rm.Builder()
    cm = variant != ""
    top = body(b, kind, cm_only=cm)
    # The port's height in the full spacecraft: the cm / chute variants share its origin.
    if cm:
        full = body(rm.Builder(), kind)
    else:
        full = top
    if variant == "chute":
        chutes(b, top, kind)
    shift(b.c, -full)
    # Sun-tracking wings (2026-10-21): a child group turning about the span axis (body Y) through the wings' centre;
    # its parts' positions are relative to the hinge.
    for c in b.c:
        zw = c.pop("_wing", None)
        if zw is not None:
            c["group"] = "wings"
            c["position"] = rm.r4([c["position"][0], c["position"][1], c["position"][2] - (zw - full)])
            c["_hinge_z"] = zw - full
    return b.c


def model(kind, variant, comps):
    groups = [{"name": "body", "primary": {"axis": [0, 0, 1], "target": "velocity"},
               "secondary": {"axis": [0, -1, 0], "target": "nadir"}}]
    hz = [c.pop("_hinge_z") for c in comps if "_hinge_z" in c]
    if hz:
        groups.append({"name": "wings", "parent": "body",
                       "hinge": {"position": [0.0, 0.0, round(hz[0], 4)], "axis": [0, 1, 0]},
                       "joint": {"mode": "track", "vector": [0, 0, 1], "target": "sun"}})
    phot = [c["name"] for c in comps if not c.get("render_only")]
    used = sorted({c["material"] for c in comps} | {c.get("arc_material") for c in comps if c.get("arc_material")} |
                  {c.get("inner_material") for c in comps if c.get("inner_material")})
    mats = [m for m in rm.MATERIALS if m["name"] in used]
    part = {"": "in orbit", "cm": ("the spaceplane alone (entry, glide and landing)" if kind in RUNWAY
                                   else "the crew / descent module alone (entry)"),
            "chute": "the descent module under its main parachutes"}[variant]
    what = "Cargo ship" if kind in CARGO + RUNWAY else "Crew spacecraft"
    return {
        "name": "%s %s" % (kind, part),
        "note": ("%s render model (tools/make_crew_models.py), %s. %s Body frame: +Z out of the docking "
                 "port, origin at the port's contact point." % (what, part, NOTES[kind])),
        "materials": mats,
        "attitude_groups": groups,
        "components": comps,
        "sources": [
            {"subject": "attitude", "status": "estimate",
             "value": "the sim hands in a full attitude (approach, docked, entry); this law is only its fallback",
             "source": "SatelliteSimLaunches.cpp crew vehicles."}] + ([
            {"subject": "wings", "status": "estimate",
             "value": "the wings turn about their span to face the Sun (a solar array drive), rest facing along +Z",
             "source": "Photographs of Tianzhou / Shenzhou in flight: their wings are steered; the law is estimated."}]
            if hz else []) + [
            {"subject": "body", "status": "estimate", "value": "the rigid body",
             "source": "Render model."},
            {"subjects": phot, "status": "sourced",
             "value": "diameters and lengths rounded from the references; shapes estimated from photographs",
             "source": " | ".join(REFS[kind])},
            {"subjects": [m["name"] for m in mats], "status": "estimate",
             "value": "; ".join("%s: %s" % (m["name"], rm.MATERIAL_SOURCES[m["name"]]) for m in mats),
             "source": "Liveries from photographs; reflectance from the presets."},
        ],
    }


# ── The station's arm (2026-10-21): Canadarm2 ─────────────────────────────────────────────────────────────────
# data/launches.json "station_arms" sets its geometry (boom_m, wrist_m); the sim poses three meshes: the boom twice
# (shoulder -> elbow, elbow -> wrist) and the wrist + end effector. Body frame: +Z along the part toward the next
# joint (the tip), +Y the plane normal the booms bend about (the pitch joints' axis), origin at the part's own joint.
ARM_REFS = ["CSA / Wikipedia 'Mobile Servicing System' (Canadarm2): 17.6 m long, two booms ~7.1 m, 35 cm across, "
            "seven joints (shoulder roll / yaw / pitch, elbow pitch, wrist pitch / yaw / roll), a latching end "
            "effector (LEE) at each end, 1.8 t"]


def arm_parts(part, boom_m, wrist_m):
    b = rm.Builder()
    ry = rm.quat_from_basis((1.0, 0.0, 0.0), (0.0, 0.0, -1.0), (0.0, 1.0, 0.0))  # a cylinder's axis along +Y
    joint = b.profile("joint_housing", [[-0.42, 0.34], [0.42, 0.34]], "white", ro=True, caps=True, rot=ry)
    if part == "boom":
        b.profile("boom", [[0.4, 0.19], [boom_m - 0.4, 0.19]], "white", caps=True)
        b.profile("boom_band", [[0.4, 0.21], [0.75, 0.21]], "grey", ro=True, caps=True)
        b.profile("boom_band", [[boom_m - 0.75, 0.21], [boom_m - 0.4, 0.21]], "grey", ro=True, caps=True)
        b.add({"name": "camera", "type": "box", "size": [0.25, 0.3, 0.4],
               "position": [0.3, 0.0, 0.5 * boom_m], "material": "black"}, True)
    else:
        b.profile("wrist_roll", [[0.3, 0.28], [0.75, 0.28]], "grey", ro=True, caps=True)
        b.profile("end_effector", [[0.75, 0.45], [wrist_m - 0.05, 0.45]], "white", caps=True)
        b.profile("snare", [[wrist_m - 0.05, 0.3], [wrist_m, 0.3]], "black", ro=True, caps=True)
        b.add({"name": "camera", "type": "box", "size": [0.3, 0.25, 0.35],
               "position": [0.55, 0.0, 1.0], "material": "black"}, True)
    return b.c


def arm_model(part, comps, boom_m, wrist_m):
    phot = [c["name"] for c in comps if not c.get("render_only")]
    used = sorted({c["material"] for c in comps})
    mats = [m for m in rm.MATERIALS if m["name"] in used]
    what = ("a boom (%.1f m) with its joint housing at the origin" % boom_m if part == "boom"
            else "the wrist joints and the latching end effector (%.1f m to its tip)" % wrist_m)
    return {
        "name": "canadarm2 %s" % part,
        "note": ("Station arm render model (tools/make_crew_models.py): %s. Body frame: +Z toward the next joint, +Y "
                 "the bending plane's normal; posed per frame by the sim (launches.json station_arms)." % what),
        "materials": mats,
        "attitude_groups": [{"name": "body", "primary": {"axis": [0, 0, 1], "target": "velocity"},
                             "secondary": {"axis": [0, -1, 0], "target": "nadir"}}],
        "components": comps,
        "sources": [
            {"subject": "attitude", "status": "estimate", "value": "the sim hands in the pose (the arm's kinematics)",
             "source": "SatelliteSimLaunches.cpp stationArmPose."},
            {"subject": "body", "status": "estimate", "value": "one rigid part of the arm", "source": "Render model."},
            {"subjects": phot, "status": "sourced", "value": "lengths and diameters rounded from the reference",
             "source": " | ".join(ARM_REFS)},
            {"subjects": [m["name"] for m in mats], "status": "estimate",
             "value": "; ".join("%s: %s" % (m["name"], rm.MATERIAL_SOURCES[m["name"]]) for m in mats),
             "source": "White thermal blankets and booms from photographs."},
        ],
    }


def write_arm():
    L = json.load(open(rm.LAUNCHES, encoding="utf-8"))
    for a in L.get("station_arms", []):
        boom_m, wrist_m = a.get("boom_m", 7.1), a.get("wrist_m", 1.6)
        for part in ("boom", "wrist"):
            comps = arm_parts(part, boom_m, wrist_m)
            name = "rocket_arm_%s_%s.json" % (a.get("model", "canadarm2"), part)
            with open(os.path.join(OUT, name), "w", encoding="utf-8") as f:
                json.dump(arm_model(part, comps, boom_m, wrist_m), f, indent=1)
            print("%-36s %2d parts" % (name, len(comps)))


def main():
    n = 0
    for kind in ("dragon", "shenzhou", "soyuz", "gaganyaan") + CARGO + RUNWAY:
        for variant in (("",) if kind in CARGO else ("", "cm") if kind in RUNWAY else ("", "cm", "chute")):
            comps = build(kind, variant)
            name = "rocket_capsule_%s%s.json" % (kind, "_" + variant if variant else "")
            with open(os.path.join(OUT, name), "w", encoding="utf-8") as f:
                json.dump(model(kind, variant, comps), f, indent=1)
            n += 1
            zs = []
            for c in comps:
                if c["type"] == "profile":
                    zs += [p[0] for p in c["profile"]]
                else:
                    zs.append(c["position"][2])
            print("%-36s %2d parts, z %.2f .. %.2f" % (name, len(comps), min(zs), max(zs)))
    print("wrote %d models" % n)
    write_arm()


if __name__ == "__main__":
    main()
