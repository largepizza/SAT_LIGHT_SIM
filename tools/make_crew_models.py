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
            # A wing standing out from the service module: span radially, its width along the axis.
            b.radial_box("solar_wing", 90.0 + 180.0 * k, sm_r + 0.4, z + 0.5 * sm_l, span, 0.04, wing_w, "cells",
                         ro=False)
            b.radial_box("wing_yoke", 90.0 + 180.0 * k, sm_r, z + 0.5 * sm_l, 0.45, 0.12, 0.3, "grey")
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
        b.radial_box("solar_wing", 90.0 + 180.0 * k, 1.36 + 0.4, z + 1.3, 4.1, 0.04, 1.15, "cells", ro=False)
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
        b.radial_box("solar_wing", 90.0 + 180.0 * k, 1.4 + 0.6, z + 1.6, 5.6, 0.04, 2.0, "cells", ro=False)
        b.radial_box("wing_yoke", 90.0 + 180.0 * k, 1.4, z + 1.6, 0.65, 0.12, 0.3, "grey")
    z += 3.9
    b.profile("cargo_module", [[z, 1.675], [z + 5.6, 1.675], [z + 6.3, 0.85]], "white", caps=True)
    z += 6.3
    b.tube("docking_ring", 0.55, z, z + 0.3, "grey", ro=True, caps=True)
    return z + 0.3


CARGO = ("progress", "tianzhou")


def body(b, kind, cm_only=False):
    if kind == "dragon":
        return dragon(b, cm_only=cm_only)
    if kind == "gaganyaan":
        return gaganyaan(b, cm_only=cm_only)
    if kind == "progress":
        return progress(b)
    if kind == "tianzhou":
        return tianzhou(b)
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


def chutes(b, top, kind):
    """Render-only main canopies over the module (+Z up in descent): hemispherical shells on their riser."""
    if kind == "dragon":
        cans = [(17.0, 18.0, 42.0, a) for a in (45.0, 135.0, 225.0, 315.0)]
    elif kind == "gaganyaan":
        cans = [(12.0, 13.0, 38.0, a) for a in (90.0, 210.0, 330.0)]
    else:
        cans = [(0.0, 18.0, 40.0, 0.0)]
    for off, rad, hgt, az in cans:
        x, y = off * math.cos(math.radians(az)), off * math.sin(math.radians(az))
        pts = [[top + hgt + rad * math.sin(math.pi * 0.5 * k / 6) * 0.55, rad * math.cos(math.pi * 0.5 * k / 6)]
               for k in range(7)]
        pts[-1][1] = 0.0
        b.profile("canopy", pts, "white", x=x, y=y, ro=True, inner="grey")
        b.tube("riser", 0.05, top, top + hgt, "grey", ro=True, x=x * 0.5, y=y * 0.5)


def shift(comps, dz):
    for c in comps:
        if c["type"] == "profile":
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
    return b.c


def model(kind, variant, comps):
    phot = [c["name"] for c in comps if not c.get("render_only")]
    used = sorted({c["material"] for c in comps} | {c.get("arc_material") for c in comps if c.get("arc_material")} |
                  {c.get("inner_material") for c in comps if c.get("inner_material")})
    mats = [m for m in rm.MATERIALS if m["name"] in used]
    part = {"": "in orbit", "cm": "the crew / descent module alone (entry)",
            "chute": "the descent module under its main parachutes"}[variant]
    what = "Cargo ship" if kind in CARGO else "Crew spacecraft"
    return {
        "name": "%s %s" % (kind, part),
        "note": ("%s render model (tools/make_crew_models.py), %s. %s Body frame: +Z out of the docking "
                 "port, origin at the port's contact point." % (what, part, NOTES[kind])),
        "materials": mats,
        "attitude_groups": [{"name": "body", "primary": {"axis": [0, 0, 1], "target": "velocity"},
                             "secondary": {"axis": [0, -1, 0], "target": "nadir"}}],
        "components": comps,
        "sources": [
            {"subject": "attitude", "status": "estimate",
             "value": "the sim hands in a full attitude (approach, docked, entry); this law is only its fallback",
             "source": "SatelliteSimLaunches.cpp crew vehicles."},
            {"subject": "body", "status": "estimate", "value": "one rigid group",
             "source": "Render model; no moving parts."},
            {"subjects": phot, "status": "sourced",
             "value": "diameters and lengths rounded from the references; shapes estimated from photographs",
             "source": " | ".join(REFS[kind])},
            {"subjects": [m["name"] for m in mats], "status": "estimate",
             "value": "; ".join("%s: %s" % (m["name"], rm.MATERIAL_SOURCES[m["name"]]) for m in mats),
             "source": "Liveries from photographs; reflectance from the presets."},
        ],
    }


def main():
    n = 0
    for kind in ("dragon", "shenzhou", "soyuz", "gaganyaan") + CARGO:
        for variant in (("",) if kind in CARGO else ("", "cm", "chute")):
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


if __name__ == "__main__":
    main()
