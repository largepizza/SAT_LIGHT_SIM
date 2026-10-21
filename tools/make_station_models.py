"""Writes the space stations' models: data/satellite_models/{haven1,haven2,axiom_station,orbital_reef,ross,
bharatiya_station,starlab}.json, and appends render-only detail ("gb_*" components) to the hand-made iss.json and
tiangong.json (any earlier gb_* components there are replaced, so re-running is idempotent).

    python tools/make_station_models.py

Station art pass, 2026-10-20 (issue 20261019_225545 "Station Art Pass"). Body frame for every station: +X along
velocity, +Y starboard, +Z nadir (zenith is -Z); one root group "station" flown like the ISS (XVV: +Z nadir, +X
along velocity). Solar wings sit in child groups that turn each wing about its OWN long axis to face the Sun
("arrays_y": wings along +-Y, "arrays_x": wings along +-X; per-component pivots put several parallel axes in one
group). Every wing is attached: a render-only boom or mast joins it to its module.

Most of these stations are future designs: dimensions come from the operators' published figures where there are
any (each model's `sources` cite them) and the layout from their concept art; where nothing is published the
`sources` say "estimate". Detail - docking ports, berthing rings, antennas, dishes, windows, RCS pods, external
payloads, MMOD sleeves, radiator manifolds, visiting vehicles - is render_only: it costs no lobes and no occlusion,
and is kept to a few dozen parts per station (the mesh renderer draws every part; prefer patterns - panel_seams
reads as MMOD panels and handrails - over many tiny parts).
"""

import json
import math
import os

OUT = os.path.join(os.path.dirname(__file__), "..", "data", "satellite_models")

MATERIALS = {
    "module_skin": {"name": "module_skin", "preset": "white_paint", "diffuse_albedo": 0.6, "roughness": 0.6,
                    "color": [0.9, 0.9, 0.88], "pattern": "panel_seams"},
    "module_metal": {"name": "module_metal", "preset": "aluminum", "diffuse_albedo": 0.35, "roughness": 0.35,
                     "color": [0.82, 0.82, 0.84], "pattern": "panel_seams"},
    "soft_goods": {"name": "soft_goods", "preset": "white_paint", "diffuse_albedo": 0.7, "roughness": 0.8,
                   "color": [0.93, 0.92, 0.88], "pattern": "mli"},
    "mli": {"name": "mli", "preset": "mli_foil"},
    "radiator": {"name": "radiator", "preset": "white_paint", "diffuse_albedo": 0.8, "roughness": 0.5,
                 "color": [0.95, 0.95, 0.95], "pattern": "panel_seams"},
    "cells": {"name": "cells", "preset": "solar_cell"},
    "cells_back": {"name": "cells_back", "preset": "solar_array_back"},
    "flex_cells": {"name": "flex_cells", "preset": "solar_cell_flex"},
    "flex_back": {"name": "flex_back", "preset": "solar_array_flex_back"},
    "lattice": {"name": "lattice", "preset": "truss", "coverage": 0.3, "truss_pitch": 1.2},
    "metal": {"name": "metal", "preset": "aluminum", "diffuse_albedo": 0.2, "roughness": 0.3,
              "color": [0.7, 0.7, 0.72]},
    "dark": {"name": "dark", "preset": "black_paint"},
    "glass": {"name": "glass", "preset": "black_paint", "diffuse_albedo": 0.02, "roughness": 0.05,
              "color": [0.05, 0.07, 0.09]},
    "gold": {"name": "gold", "preset": "mli_foil", "color": [0.95, 0.72, 0.30]},
    "dragon_white": {"name": "dragon_white", "preset": "white_paint", "diffuse_albedo": 0.75},
    "cn_green": {"name": "cn_green", "preset": "white_paint", "diffuse_albedo": 0.35,
                 "color": [0.55, 0.62, 0.45]},
}
MATERIAL_NOTES = {
    "module_skin": "white beta-cloth / MMOD blanket exterior (albedo 0.6), panel seams for the shield panels",
    "module_metal": "bare / anodised aluminium pressure shell exterior",
    "soft_goods": "inflatable habitat's woven outer layer (Vectran restraint under beta cloth), quilted",
    "radiator": "white (Z-93-class) radiator coating",
    "cells": "rigid solar array, cells under cover glass",
    "cells_back": "rigid array substrate back",
    "flex_cells": "flexible (ISS-style) array, cells on a Kapton blanket",
    "flex_back": "flexible array's Kapton back (translucent)",
    "lattice": "open aluminium truss",
    "mli": "multi-layer insulation",
    "metal": "bare metal fittings",
    "dark": "black paint",
    "glass": "window glass",
    "gold": "gold MLI",
    "dragon_white": "Dragon's white paint",
    "cn_green": "Shenzhou's grey-green paint",
}


def dumps(obj, ind=0, step=4):
    """JSON with lists of numbers kept on one line (the hand-made models' style)."""
    pad = " " * (ind + step)
    if isinstance(obj, dict):
        if not obj:
            return "{}"
        items = ["%s%s: %s" % (pad, json.dumps(k), dumps(v, ind + step, step)) for k, v in obj.items()]
        return "{\n" + ",\n".join(items) + "\n" + " " * ind + "}"
    if isinstance(obj, list):
        if all(not isinstance(v, (dict, list)) for v in obj) and not any(isinstance(v, str) for v in obj):
            return json.dumps(obj)
        if all(isinstance(v, list) and all(not isinstance(u, (dict, list)) for u in v) for v in obj) and obj:
            return "[" + ", ".join(json.dumps(v) for v in obj) + "]"
        items = [pad + dumps(v, ind + step, step) for v in obj]
        return "[\n" + ",\n".join(items) + "\n" + " " * ind + "]"
    return json.dumps(obj)


def dump_file(path, obj):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(dumps(obj) + "\n")


# ── Geometry helpers ─────────────────────────────────────────────────────────────────────────────────────────
def r4(v):
    return [round(float(a), 4) for a in v]


def norm(v):
    n = math.sqrt(sum(a * a for a in v))
    return [a / n for a in v]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def add(a, b, s=1.0):
    return [a[i] + s * b[i] for i in range(3)]


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


def frame(axis, xhint=None):
    """An orthonormal frame with local Z = axis; local X toward xhint (default: zenith -Z, else +X)."""
    z = norm(axis)
    if xhint is None:
        xhint = [0, 0, -1] if abs(z[2]) < 0.9 else [1, 0, 0]
    d = sum(xhint[i] * z[i] for i in range(3))
    x = norm([xhint[i] - d * z[i] for i in range(3)])
    y = cross(z, x)
    return x, y, z


def dome(r, depth, n=6, z0=0.0):
    """Quarter-ellipse cap from radius r at z0 up to the axis at z0 + depth ([z, r], ascending)."""
    return [[z0 + depth * math.sin(0.5 * math.pi * i / n), r * math.cos(0.5 * math.pi * i / n)] for i in range(n + 1)]


AX = {"+x": [1, 0, 0], "-x": [-1, 0, 0], "+y": [0, 1, 0], "-y": [0, -1, 0], "+z": [0, 0, 1], "-z": [0, 0, -1]}


class Station:
    def __init__(self, sid, name, note):
        self.sid, self.name, self.note = sid, name, note
        self.groups = [{"name": "station", "primary": {"axis": [0, 0, 1], "target": "nadir"},
                        "secondary": {"axis": [1, 0, 0], "target": "velocity"}}]
        self.c = []
        self.sources = []
        self.extra_mats = []
        self.ports = []  # docking ports a crew spacecraft can use (the sim's crew rotations: "docking_ports")

    # Groups ------------------------------------------------------------------------------------------------
    def arrays(self, name, axis, vector=(0, 0, -1)):
        """A child group turning every wing about a body axis (per-component pivots: parallel axes); `vector`
        is the wings' face normal at rest (perpendicular to the axis)."""
        self.groups.append({"name": name, "parent": "station",
                            "hinge": {"position": [0, 0, 0], "axis": r4(AX[axis] if isinstance(axis, str) else axis)},
                            "joint": {"mode": "track", "vector": r4(vector), "target": "sun"}})

    def radiator_group(self, name="radiators", axis="+y"):
        """Radiators turned about a body axis to stay edge-on to the Sun (the ISS's TRRJ scheme)."""
        self.groups.append({"name": name, "parent": "station",
                            "hinge": {"position": [0, 0, 0], "axis": AX[axis]},
                            "joint": {"mode": "edge_on_continuous", "vector": [0, 0, 1], "target": "sun"}})

    # Primitives --------------------------------------------------------------------------------------------
    def add(self, d, group="station", ro=False, pivot=None):
        d["group"] = group
        if ro:
            d["render_only"] = True
        if pivot is not None:
            d["pivot"] = r4(pivot)
        self.c.append(d)
        return d

    def cyl(self, name, base, axis, length, r, mat, group="station", ro=False, caps=True, pivot=None):
        """A cylinder from `base` along unit `axis` for `length`."""
        x, y, z = frame(AX[axis] if isinstance(axis, str) else axis)
        ctr = add(base, z, 0.5 * length)
        return self.add({"name": name, "type": "cylinder", "radius": round(r, 4), "height": round(length, 4),
                         "position": r4(ctr), "rotation_quat": quat_from_basis(x, y, z), "caps": caps,
                         "material": mat}, group, ro, pivot)

    def prof(self, name, base, axis, pts, mat, group="station", ro=False, caps=False, arc=None, inner=None,
             xhint=None, pivot=None):
        """A body of revolution from `base` along `axis`; pts = [[z, r], ...] ascending (z from base)."""
        x, y, z = frame(AX[axis] if isinstance(axis, str) else axis, xhint)
        d = {"name": name, "type": "profile", "profile": [r4(p) for p in pts], "position": r4(base),
             "rotation_quat": quat_from_basis(x, y, z), "caps": caps, "material": mat}
        if arc:
            d["arc_material"], d["arc_deg"] = arc[0], [arc[1], arc[2]]
        if inner:
            d["inner_material"] = inner
        return self.add(d, group, ro, pivot)

    def hull(self, name, base, axis, pts, mat, caps="both", arc=None):
        """A photometric body of revolution split into one profile per meridian segment: each piece's occluder
        is the exact frustum through its own end radii (a whole domed profile's occluder would be the cone
        through its two end radii - far from the shape, and the occlusion self-test fails). The longest piece
        keeps `name`, the others are `name`_segK (source() covers them with the name)."""
        a = AX[axis] if isinstance(axis, str) else norm(axis)
        segs = [(pts[i], pts[i + 1]) for i in range(len(pts) - 1)]
        longest = max(range(len(segs)), key=lambda i: segs[i][1][0] - segs[i][0][0])
        for i, (p0, p1) in enumerate(segs):
            c = False
            if caps in ("both", "bottom") and i == 0:
                c = "bottom"
            if caps in ("both", "top") and i == len(segs) - 1:
                c = "both" if c else "top"
            nm = name if i == longest else "%s_seg%d" % (name, i)
            self.prof(nm, add(base, a, p0[0]), a, [[0, p0[1]], [p1[0] - p0[0], p1[1]]], mat, caps=c, arc=arc)

    def box(self, name, ctr, size, mat, group="station", ro=True, axes=None, pivot=None):
        d = {"name": name, "type": "box", "size": r4(size), "position": r4(ctr), "material": mat}
        if axes:
            d["rotation_quat"] = quat_from_basis(*axes)
        return self.add(d, group, ro, pivot)

    def sphere(self, name, ctr, r, mat, group="station", ro=False):
        return self.add({"name": name, "type": "sphere", "radius": round(r, 4), "position": r4(ctr),
                         "material": mat}, group, ro)

    def plane(self, name, ctr, along, normal, length, width, mat, back, group="station", ro=False, pivot=None):
        """A plane `length` along `along`, `width` across it, facing `normal`."""
        yax = norm(along)
        zax = norm(normal)
        xax = cross(yax, zax)
        return self.add({"name": name, "type": "plane", "size": r4([width, length]), "position": r4(ctr),
                         "rotation_quat": quat_from_basis(xax, yax, zax), "material": mat,
                         "back_material": back}, group, ro, pivot)

    # Assemblies --------------------------------------------------------------------------------------------
    def wing(self, name, group, root, along, length, width, axis_pt, rigid=True, boom=0.6):
        """A Sun-tracking rigid wing: from `root` (on its rotation axis) along `along`, normal zenith at rest; a
        render-only yoke boom joins it to the module and a frame edge stiffens it."""
        a = AX[along]
        start = add(root, a, boom)
        ctr = add(start, a, 0.5 * length)
        if rigid:
            self.plane(name, ctr, a, [0, 0, -1], length, width, "cells", "cells_back", group, pivot=axis_pt)
        else:
            self.plane(name, ctr, a, [0, 0, -1], length, width, "flex_cells", "flex_back", group, pivot=axis_pt)
        # yoke: a boom on the rotation axis from the module to the wing's root, and a spar along the wing
        bctr = add(root, a, 0.5 * boom + 0.1)
        sz = [abs(a[0]) * (boom + 0.2) + 0.18, abs(a[1]) * (boom + 0.2) + 0.18, 0.18]
        self.box(name + "_yoke", bctr, sz, "metal", group, pivot=axis_pt)
        spar = [abs(a[0]) * length + 0.08, abs(a[1]) * length + 0.08, 0.08]
        self.box(name + "_spar", add(ctr, [0, 0, 0.07]), spar, "metal", group, pivot=axis_pt)

    def kapton_wing(self, name, group, root, along, length, blanket_w, gap, axis_pt):
        """An ISS-style flexible wing: two Kapton blankets either side of a deployable mast, a blanket box at
        each end. The mast is the rotation axis."""
        a = AX[along]
        across = [abs(a[1]), abs(a[0]), 0.0]  # horizontal, perpendicular to the mast
        start = add(root, a, 1.2)
        ctr = add(start, a, 0.5 * length)
        for s, tag in ((1, "a"), (-1, "b")):
            c = add(ctr, across, s * (0.5 * blanket_w + 0.5 * gap))
            self.plane("%s_%s" % (name, tag), c, a, [0, 0, -1], length, blanket_w, "flex_cells", "flex_back",
                       group, pivot=axis_pt)
        mast = [abs(a[0]) * (length + 1.4) + 0.35, abs(a[1]) * (length + 1.4) + 0.35, 0.35]
        self.box(name + "_mast", add(root, a, 0.5 * (length + 1.4)), mast, "lattice", group, pivot=axis_pt)
        for end, off in (("root", 0.9), ("tip", length + 1.45)):
            bsz = [abs(a[0]) * 0.5 + abs(a[1]) * (2 * blanket_w + gap),
                   abs(a[1]) * 0.5 + abs(a[0]) * (2 * blanket_w + gap), 0.5]
            self.box(name + "_box_" + end, add(root, a, off), bsz, "gold", group, pivot=axis_pt)

    def port(self, name, base, axis, r=0.8, length=0.7, ring=True, dock=False, use="crew"):
        """A docking / berthing port: a short barrel and a lip ring. dock: a visiting spacecraft docks here (the sim's
        crew rotations, 2026-10-20, and cargo ships, 2026-10-21: the contact point is the barrel's end); use: which
        ("crew" / "cargo")."""
        self.prof(name, base, axis, [[0, r], [length, r]], "metal", ro=True, caps="top")
        if dock:
            self.dock(name, add(base, AX[axis] if isinstance(axis, str) else norm(axis), length), axis, use=use)
        if ring:
            self.prof(name + "_ring", add(base, AX[axis] if isinstance(axis, str) else axis, length - 0.12),
                      axis, [[0, r + 0.12], [0.12, r + 0.12]], "metal", ro=True, caps="both")

    def dock(self, name, tip, axis, use="crew"):
        """A docking port's contact point (station body frame) and its outward axis, for the sim's visiting vehicles
        (use: "crew" or "cargo"). Visiting spacecraft are NOT modelled on the station any more (they were render-only
        greebles): the sim flies real ones there (crews 2026-10-20, Progress / Tianzhou 2026-10-21) and draws them
        docked."""
        a = AX[axis] if isinstance(axis, str) else norm(axis)
        e = {"name": name, "position": r4(tip), "axis": r4(a)}
        if use != "crew":
            e["use"] = use
        self.ports.append(e)

    def dish(self, name, base, axis, r, mast=1.0):
        """A dish antenna on a mast: the mast from `base` along `axis`, the dish facing further out."""
        a = AX[axis] if isinstance(axis, str) else norm(axis)
        self.cyl(name + "_mast", base, a, mast, 0.06, "metal", ro=True)
        p = add(base, a, mast)
        self.prof(name, p, a, [[0, 0.08], [0.06 * r, 0.45 * r], [0.16 * r, 0.8 * r], [0.25 * r, r]],
                  "metal", ro=True, inner="dragon_white")

    def window(self, name, ctr, normal, r):
        self.prof(name, add(ctr, normal, -0.02), normal, [[0, r], [0.05, r]], "glass", ro=True, caps="top")

    def sleeve(self, name, base, axis, length, r, mat="mli"):
        """A band around a module (MMOD shield, equipment ring): render-only, slightly proud of the skin."""
        self.prof(name, base, axis, [[0, r], [length, r]], mat, ro=True)

    def rcs(self, name, ctr, out):
        """A small RCS thruster pod standing off the hull toward `out`."""
        o = norm(out)
        x, y, z = frame(o)
        self.box(name, add(ctr, o, 0.15), [0.35, 0.35, 0.3], "metal", axes=(x, y, z))

    def dragon(self, name, port_tip, axis_into):
        """A Crew Dragon docked nose-first at `port_tip`; axis_into points from the Dragon into the station."""
        a = AX[axis_into] if isinstance(axis_into, str) else axis_into
        out = [-v for v in a]
        trunk_base = add(port_tip, out, 0.35 + 3.3 + 2.8)
        self.prof(name + "_trunk", trunk_base, a, [[0, 1.85], [2.8, 1.85]], "dragon_white", ro=True,
                  caps="bottom", arc=("cells", 0, 180))
        cap_base = add(trunk_base, a, 2.8)
        self.prof(name + "_capsule", cap_base, a,
                  [[0, 1.95], [0.15, 1.95], [2.4, 1.25], [2.9, 0.95], [3.3, 0.55]], "dragon_white", ro=True,
                  caps="top")
        self.prof(name + "_nose", add(cap_base, a, 3.3), a, [[0, 0.55], [0.35, 0.45]], "metal", ro=True,
                  caps="top")

    def soyuz(self, name, port_tip, axis_into, mat="cn_green", wing_span=4.0):
        """A Soyuz / Shenzhou-class ship docked orbital-module-first."""
        a = AX[axis_into] if isinstance(axis_into, str) else axis_into
        out = [-v for v in a]
        base = add(port_tip, out, 2.4 + 2.2 + 2.9 + 0.3)
        self.prof(name + "_service", base, a, [[0, 1.35], [0.6, 1.36], [2.9, 1.1]], mat, ro=True, caps="bottom")
        self.prof(name + "_descent", add(base, a, 2.9), a, [[0, 1.1], [1.8, 1.0], [2.2, 0.5]], mat, ro=True,
                  caps=False)
        self.prof(name + "_orbital", add(base, a, 5.1), a,
                  [[0, 0.6], [0.4, 1.05], [1.2, 1.1], [2.0, 1.0], [2.4, 0.5]], mat, ro=True, caps="top")
        x, y, z = frame(a)
        for s in (1, -1):
            ctr = add(add(base, a, 1.4), y, s * (1.36 + 0.2 + 0.5 * wing_span))
            self.plane(name + ("_wing_p" if s > 0 else "_wing_m"), ctr, [s * v for v in y], x, wing_span, 1.6,
                       "cells", "cells_back", ro=True)

    # Output ------------------------------------------------------------------------------------------------
    def source(self, subjects, status, value, src):
        e = {"status": status, "value": value, "source": src}
        subs = [subjects] if isinstance(subjects, str) else list(subjects)
        segs = [c["name"] for c in self.c for n in subs if c["name"].startswith(n + "_seg")]
        subs += segs
        if len(subs) == 1:
            e["subject"] = subs[0]
        else:
            e["subjects"] = subs
        self.sources.append(e)

    def finish(self, refs):
        """Default provenance for whatever the station's own entries did not cover."""
        covered = set()
        for s in self.sources:
            covered |= set([s["subject"]] if "subject" in s else s["subjects"])
        phot = [c["name"] for c in self.c if not c.get("render_only") and c["name"] not in covered]
        if phot:
            self.source(phot, "estimate", "sizes and placement from the operator's concept art",
                        " | ".join(refs))
        groups = [g["name"] for g in self.groups if g["name"] not in covered]
        if groups:
            self.source(groups, "estimate",
                        "station flown XVV like the ISS; wings turn about their long axes to face the Sun",
                        "Unpublished; the ISS scheme.")
        used = set()
        for c in self.c:
            for k in ("material", "back_material", "arc_material", "inner_material"):
                if c.get(k):
                    used.add(c[k])
        mats = [MATERIALS[m] for m in MATERIALS if m in used]
        self.source([m["name"] for m in mats], "estimate", "; ".join(MATERIAL_NOTES[m["name"]] for m in mats),
                    "Liveries from the operator's renders; reflectance from the presets.")
        names = set()
        for c in self.c:  # unique names (render-only parts may repeat a stem)
            n, k = c["name"], 1
            while c["name"] in names:
                k += 1
                c["name"] = "%s_%d" % (n, k)
            names.add(c["name"])
        out = {"name": self.name, "note": self.note, "materials": mats, "attitude_groups": self.groups,
               "components": self.c, "sources": self.sources}
        if self.ports:
            out["docking_ports"] = self.ports
        return out

    def write(self, refs):
        path = os.path.join(OUT, self.sid + ".json")
        dump_file(path, self.finish(refs))
        n_ro = sum(1 for c in self.c if c.get("render_only"))
        print("%-18s %3d parts (%d photometric, %d render-only)" % (self.sid, len(self.c), len(self.c) - n_ro, n_ro))


REF_HAVEN1 = ("Wikipedia 'Haven-1': 10.1 m long, 4.4 m diameter, 425 km, 51.6 deg (Falcon 9); vastspace.com Haven-1 "
              "renders: a conical forward end to the Dragon port, two wings at the aft end, a nadir dome window")
REF_HAVEN2 = ("Wikipedia 'Haven-2': first module stretched from Haven-1's 10.1 m to 16 m (4.4 m wide), more modules "
              "2029-30, a 7 m core module (Starship) with a fifth port and an airlock, complete 2032; each module "
              "two 1.1 m Earth-facing windows; vastspace.com Haven-2 concept art (May 2025): modules docked radially "
              "to the core in a cross, each with its own pair of wings, radiators on the core")
REF_AXIOM = ("Wikipedia 'Axiom Station': PPTM (power/thermal, 'ISS-equivalent' solar array), Hab-1 ~11 m long and "
             "4.2 m wide with four radial ports, Airlock, Hab-2 (with a Canadarm-class arm), RMF with the glass Earth "
             "Observatory; axiomspace.com renders: universal modules with curved body-mounted solar panels over half "
             "the hull, ISS-style flexible (Kapton) wings on the power module")
REF_REEF = ("Wikipedia 'Orbital Reef': 830 m3; LIFE, Node, Core and Research modules; Blue Origin large-diameter "
            "(7 m, New Glenn) core modules, Sierra Space LIFE inflatables, Boeing science module; Sierra Space: LIFE "
            "285 inflates to ~8.2 m diameter (27 ft) in three storeys; orbitalreef.com concept art: a long central "
            "core with inflatable habitats off its sides, large arrays and radiators on trusses at its ends")
REF_ROS = ("Wikipedia 'Russian Orbital Station' (Dec 2025 plan): assembled at the ISS from Nauka + the Universal Node "
           "Module (UUM) + the Science-Power Module (NEM, on UUM's lower port) + the gateway airlock (ShM, side port), "
           "separated before the ISS deorbit; later a base module (an upgraded NEM) and two target modules (2033); "
           "51.6 deg, ~400 km (was ~98 deg until Dec 2025); Nauka 13.1 m long, 4.25 m wide (Wikipedia 'Nauka'); "
           "RKK Energia NEM renders: two large wings at the aft end, side radiators")
REF_BAS = ("Wikipedia 'Bharatiya Antariksh Station': five modules, all 3.8 m diameter - BAS-01 base 8.0 m, BAS-02 "
           "core 9.25 m, BAS-03 science, BAS-04 laboratory, BAS-05 common berthing module (4 CBM ports) 9.25 m; "
           "27 x 20 m overall, ~52 t; every module but BAS-05 has its own solar panels; cupola; 400-450 km, "
           "51.5 deg; ISRO BAS-01 scale model (Aug 2025)")
REF_STARLAB = ("Wikipedia 'Starlab Space Station': two modules - an 8 m diameter metallic habitat/lab (450 m3, the "
               "inflatable dropped for a rigid module after Airbus joined) and a service module with 60 kW of solar "
               "arrays; a large robotic arm; one Starship launch NET 2029; starlab-space.com renders: four large "
               "arrays in a cross on the service module, a domed habitat with a docking node")


# ── Vast Haven-1 ─────────────────────────────────────────────────────────────────────────────────────────────
def haven1():
    s = Station("haven1", "Vast Haven-1",
                "Station art pass 2026-10-20 (tools/make_station_models.py). Body frame: +X along velocity, +Z "
                "nadir. One 10.1 x 4.4 m module: a cylindrical hull and a conical forward end to the docking port "
                "(the sim docks its Crew Dragons there); two rigid wings on yokes at the aft end turn "
                "about the cross-track axis. Detail: berthing ring, nadir dome window, RCS pods, dish, radiator "
                "panels, MMOD bands.")
    s.arrays("arrays", "+y")
    L, R, x0 = 10.1, 2.2, -5.05
    s.hull("haven1_hull", [x0, 0, 0], "+x", [[0, R], [7.6, R], [9.4, 1.25], [L, 1.05]], "module_skin",
           caps="both")
    s.source("haven1_hull", "sourced", "10.1 m long, 4.4 m diameter; the conical forward end an estimate", REF_HAVEN1)
    for sgn, tag in ((1, "starboard"), (-1, "port")):
        s.wing("array_" + tag, "arrays", [x0 + 1.9, sgn * R, 0], "+y" if sgn > 0 else "-y", 7.8, 3.2,
               [x0 + 1.9, 0, 0], boom=0.7)
    s.source(["array_starboard", "array_port"], "estimate", "two rigid wings 3.2 x 7.8 m (50 m2) at the aft end",
             "Vast renders show two wings at the aft end; their size is not published (estimated from the renders).")
    s.port("dock_fwd", [x0 + L, 0, 0], "+x", r=0.75, length=0.6, dock=True)
    s.prof("aft_bulkhead", [x0 - 0.4, 0, 0], "+x", [[0, 1.5], [0.4, 2.0]], "metal", ro=True, caps="bottom")
    s.prof("aft_engine", [x0 - 0.9, 0, 0], "+x", [[0, 0.35], [0.5, 0.18]], "dark", ro=True)
    s.dish("hga", [x0 + 0.8, 0, -R], "-z", 0.55, mast=0.7)
    s.prof("nadir_window", [x0 + 4.2, 0, R - 0.25], "+z", [[0, 1.0], [0.25, 0.95], [0.42, 0.6], [0.48, 0.0]],
           "glass", ro=True)
    for k, ang in enumerate((45, 135, 225, 315)):
        a = math.radians(ang)
        out = [0, math.cos(a), math.sin(a)]
        s.rcs("rcs_%d" % k, [x0 + 0.6, R * out[1], R * out[2]], out)
    for k, xx in enumerate((x0 + 3.1, x0 + 6.0)):
        s.sleeve("band_%d" % k, [xx, 0, 0], "+x", 0.22, R + 0.04)
    for sgn in (1, -1):
        s.box("radiator_%s" % ("s" if sgn > 0 else "p"), [x0 + 4.4, sgn * (R + 0.04), -0.9], [3.2, 0.06, 1.3],
              "radiator")
    s.write([REF_HAVEN1])


# ── Vast Haven-2 ─────────────────────────────────────────────────────────────────────────────────────────────
def haven2():
    s = Station("haven2", "Vast Haven-2 (2032 configuration)",
                "Station art pass 2026-10-20 (tools/make_station_models.py), after Vast's concept art: a 7 m core "
                "standing nadir-zenith with four radial ports, four 16 m modules docked radially in a cross in the "
                "orbit-horizontal plane, each with its own pair of wings near its outer end (the +-X spokes' wings "
                "along +-Y turn about Y, the +-Y spokes' along +-X turn about X), four radiator fins on the core "
                "between the spokes, an airlock on the core's zenith port. Body frame: +X along velocity, +Z nadir. "
                "Detail: windows, ports (two crew docking ports: the sim docks real Crew Dragons there), dishes, bands.")
    s.arrays("arrays_y", "+y")
    s.arrays("arrays_x", "+x")
    Rc, Hc, R, L = 3.5, 9.0, 2.2, 16.0
    s.hull("haven2_core", [0, 0, 0.5 * Hc], "-z",
           [[0, 2.0], [0.5, 2.9], [1.1, Rc], [Hc - 1.1, Rc], [Hc - 0.5, 2.9], [Hc, 2.0]], "module_skin", caps="both")
    s.cyl("core_airlock", [0, 0, -0.5 * Hc], "-z", 3.2, 1.3, "module_skin")
    d0 = Rc + 0.6
    spokes = (("fwd", [1, 0, 0]), ("aft", [-1, 0, 0]), ("stbd", [0, 1, 0]), ("port", [0, -1, 0]))
    mods, wings = [], []
    for tag, d in spokes:
        name = "haven2_module_" + tag
        s.cyl(name, [d0 * d[0], d0 * d[1], 0], d, L, R, "module_skin")
        mods.append(name)
        s.prof("adapter_" + tag, [(Rc - 0.2) * d[0], (Rc - 0.2) * d[1], 0], d, [[0, 1.1], [0.8, 1.1]],
               "metal", ro=True)
        tip = d0 + L
        s.port("port_" + tag, [tip * d[0], tip * d[1], 0], d, r=0.75, length=0.55, dock=tag in ("fwd", "aft"))
        wc = d0 + L - 3.0
        if d[1] == 0:
            for along, sg in (("+y", 1), ("-y", -1)):
                n = "array_%s_%s" % (tag, "s" if sg > 0 else "p")
                s.wing(n, "arrays_y", [wc * d[0], sg * R, 0], along, 9.0, 3.6, [wc * d[0], 0, 0], boom=0.6)
                wings.append(n)
        else:
            for along, sg in (("+x", 1), ("-x", -1)):
                n = "array_%s_%s" % (tag, "f" if sg > 0 else "a")
                s.wing(n, "arrays_x", [sg * R, wc * d[1], 0], along, 9.0, 3.6, [0, wc * d[1], 0], boom=0.6)
                wings.append(n)
        for k, f in enumerate((0.35, 0.62)):
            p = d0 + f * L
            s.window("window_%s_%d" % (tag, k), [p * d[0], p * d[1], R], [0, 0, 1], 0.55)
        s.sleeve("band_" + tag, [(d0 + 0.5 * L) * d[0], (d0 + 0.5 * L) * d[1], 0], d, 0.25, R + 0.04)
    s.source(["haven2_core", "core_airlock"], "estimate", "core 7 m wide, 9 m tall (height an estimate), a zenith "
             "airlock", REF_HAVEN2)
    s.source(mods, "sourced", "four 16 x 4.4 m modules docked radially to the core", REF_HAVEN2)
    s.source(wings, "estimate", "eight rigid wings 3.6 x 9 m (259 m2), one pair per module",
             "Concept art shows a pair per module; sizes unpublished (estimated from the art).")
    fins = []
    for k in range(4):
        a = math.radians(45 + 90 * k)
        u = [math.cos(a), math.sin(a), 0]
        t = [-math.sin(a), math.cos(a), 0]
        n = "core_radiator_%d" % k
        s.plane(n, [(Rc + 0.15 + 3.5) * u[0], (Rc + 0.15 + 3.5) * u[1], 0.3], u, t, 7.0, 6.5, "radiator",
                "radiator")
        s.box(n + "_manifold", [(Rc + 0.08) * u[0], (Rc + 0.08) * u[1], 0.3], [0.3, 0.3, 6.5], "metal",
              axes=(u, t, [0, 0, 1]))
        fins.append(n)
    s.source(fins, "estimate", "four radiator fins 7 x 6.5 m on the core between the spokes",
             "Concept art places the radiators on the core; size estimated.")
    s.dish("hga_1", [1.5, 0, -0.5 * Hc - 0.1], [0.5, 0.5, -0.7], 0.7, mast=2.0)
    s.dish("hga_2", [0, 0, 0.5 * Hc], [0, 0, 1], 0.5, mast=0.8)
    s.prof("airlock_hatch", [0, 0, -0.5 * Hc - 3.2], "-z", [[0, 1.0], [0.3, 0.9], [0.4, 0.0]], "metal", ro=True)
    s.write([REF_HAVEN2])


# ── Axiom Station ────────────────────────────────────────────────────────────────────────────────────────────
def axiom():
    s = Station("axiom_station", "Axiom Station (free-flying, five modules)",
                "Station art pass 2026-10-20 (tools/make_station_models.py), after Axiom's renders: universal "
                "modules ~11 x 4.2 m with a curved body-mounted solar panel over the zenith half of the hull, in a "
                "line along velocity (PPTM forward, Hab-1, Hab-2, the RMF with its glass Earth Observatory), the "
                "Airlock on Hab-1's port radial port; the PPTM carries two ISS-style flexible (Kapton) wings on "
                "masts that turn about the cross-track axis, and the RMF radiators turned edge-on to the Sun. Body "
                "frame: +X along velocity, +Z nadir. Detail: ports (crew docking ports for the sim's Dragons), a Canadarm-class arm, dishes.")
    s.arrays("arrays", "+y")
    s.radiator_group("radiators", "+y")
    R, L, gap = 2.1, 11.0, 0.5
    mods = [("axiom_pptm", 0.5 * L + gap), ("axiom_hab1", -0.5 * L), ("axiom_hab2", -1.5 * L - gap),
            ("axiom_rmf", -2.5 * L - 2 * gap)]
    for name, x0 in mods:
        arc = None if name == "axiom_pptm" else ("cells", 270, 450)
        s.hull(name, [x0, 0, 0], "+x", [[0, 1.4], [0.6, R], [L - 0.6, R], [L, 1.4]], "module_skin",
               caps="both", arc=arc)
        s.sleeve(name + "_band", [x0 + 0.5 * L - 0.15, 0, 0], "+x", 0.3, R + 0.04, "metal")
    s.source([m[0] for m in mods], "sourced",
             "modules 11 x 4.2 m (Hab-1's published size, the others assumed equal); curved cells over the zenith "
             "half of Hab-1, Hab-2 and the RMF (layout from the renders)", REF_AXIOM)
    xa = mods[1][1] + 0.5 * L
    s.cyl("axiom_airlock", [xa, -(R + 0.4), 0], "-y", 5.0, 1.5, "module_skin")
    s.source("axiom_airlock", "estimate", "airlock 5 x 3 m on Hab-1's port radial port", REF_AXIOM)
    xp = mods[0][1] + 0.5 * L
    for sgn, tag in ((1, "s"), (-1, "p")):
        s.kapton_wing("pptm_wing_" + tag, "arrays", [xp, sgn * (R + 0.2), 0], "+y" if sgn > 0 else "-y", 20.0,
                      3.4, 0.7, [xp, 0, 0])
    kap = [c["name"] for c in s.c if c["name"].startswith("pptm_wing_") and c["type"] == "plane"]
    s.source(kap, "estimate", "two flexible wings of two 3.4 x 20 m Kapton blankets (272 m2)",
             "Axiom: the PPTM's arrays give 'ISS-equivalent' power; ISS-style wings in the renders; sizes estimated.")
    xr = mods[3][1] + 0.5 * L
    rads = []
    for sgn, tag in ((1, "s"), (-1, "p")):
        n = "radiator_" + tag
        s.plane(n, [xr, sgn * (R + 0.6 + 5.0), 0], [0, sgn, 0], [0, 0, 1], 10.0, 3.6, "radiator", "radiator",
                group="radiators", pivot=[xr, 0, 0])
        s.box(n + "_yoke", [xr, sgn * (R + 0.35), 0], [0.3, 0.7, 0.3], "metal", group="radiators",
              pivot=[xr, 0, 0])
        rads.append(n)
    s.source(rads, "estimate", "two 3.6 x 10 m radiators on the RMF, edge-on to the Sun",
             "Radiators not published; placement and size estimated.")
    xf = mods[0][1] + L
    s.port("port_fwd", [xf, 0, 0], "+x", r=0.75, dock=True)
    s.port("port_aft", [mods[3][1], 0, 0], "-x", r=0.75)
    for k, (name, x0) in enumerate(mods[1:3]):
        s.port("port_zenith_%d" % k, [x0 + 0.5 * L, 0, -R], "-z", r=0.65, length=0.4, dock=k == 0)
        s.port("port_nadir_%d" % k, [x0 + 0.5 * L, 0, R], "+z", r=0.65, length=0.4)
    s.prof("earth_observatory", [mods[3][1] + 0.5 * L, 0, R - 0.2], "+z",
           [[0, 1.5], [0.5, 1.45], [1.0, 1.15], [1.35, 0.6], [1.5, 0.0]], "glass", ro=True)
    xh = mods[2][1] + 0.5 * L
    s.box("arm_boom_1", [xh + 2.0, R + 0.4, -1.2], [7.0, 0.35, 0.35], "metal")
    s.box("arm_boom_2", [xh - 1.5, R + 0.4, -2.6], [0.35, 0.35, 3.0], "metal")
    s.dish("hga", [mods[0][1] + 2.0, 0, -R], "-z", 0.6, mast=1.0)
    s.write([REF_AXIOM])


# ── Orbital Reef ─────────────────────────────────────────────────────────────────────────────────────────────
def orbital_reef():
    s = Station("orbital_reef", "Orbital Reef (core + LIFE habitats)",
                "Station art pass 2026-10-20 (tools/make_station_models.py), after the Blue Origin / Sierra Space "
                "concept art: a 7 m wide, 28 m core along velocity, a Boeing science module forward, two Sierra "
                "Space LIFE inflatables (8.2 m wide) off its sides, an aft truss with four large rigid wings turning "
                "about the cross-track axis, a forward truss with radiators turned edge-on to the Sun. Body frame: "
                "+X along velocity, +Z nadir. Detail: ports, dishes, windows, straps, bands.")
    s.arrays("arrays", "+y")
    s.radiator_group("radiators", "+y")
    Rc, Lc = 3.5, 28.0
    s.hull("reef_core", [-0.5 * Lc, 0, 0], "+x",
           [[0, 2.0], [0.6, 3.1], [1.2, Rc], [Lc - 1.2, Rc], [Lc - 0.6, 3.1], [Lc, 2.0]], "module_skin",
           caps="both")
    s.cyl("reef_science", [0.5 * Lc + 0.3, 0, 0], "+x", 9.0, 2.2, "module_skin")
    s.source(["reef_core", "reef_science"], "estimate",
             "core 7 x 28 m (New Glenn's 7 m fairing), Boeing science module 4.4 x 9 m", REF_REEF)
    lifes = []
    for sgn, tag in ((1, "s"), (-1, "p")):
        n = "reef_life_" + tag
        s.hull(n, [-2.0, sgn * (Rc + 0.7), 0], [0, sgn, 0],
               [[0, 1.3], [0.7, 3.4], [1.5, 4.1], [7.4, 4.1], [8.4, 3.3], [9.1, 1.8], [9.4, 0.0]], "soft_goods",
               caps="bottom")
        s.prof("life_neck_" + tag, [-2.0, sgn * (Rc - 0.2), 0], [0, sgn, 0], [[0, 1.1], [1.0, 1.1]], "metal",
               ro=True)
        for k in range(3):
            s.sleeve("life_strap_%s_%d" % (tag, k), [-2.0, sgn * (Rc + 0.7 + 2.5 + 2.1 * k), 0], [0, sgn, 0], 0.12,
                     4.15, "metal")
        lifes.append(n)
    s.source(lifes, "sourced", "two LIFE habitats ~8.2 m diameter, ~9.4 m long (inflated)", REF_REEF)
    xa, za = -0.5 * Lc + 2.5, -(Rc + 1.8)
    s.box("aft_pylon", [xa, 0, -(Rc + 0.9)], [1.2, 1.2, 1.9], "lattice", ro=False)
    s.box("aft_truss", [xa, 0, za], [1.6, 64.0, 1.6], "lattice", ro=False)
    s.source(["aft_pylon", "aft_truss"], "estimate", "a 64 m open truss on a pylon over the core's aft end",
             "Truss layout from the concept art; size estimated.")
    wings = []
    for sgn in (1, -1):
        for k, y0 in enumerate((5.0, 19.0)):
            n = "array_%s%d" % ("s" if sgn > 0 else "p", k)
            s.plane(n, [xa, sgn * (y0 + 6.5), za], [0, sgn, 0], [0, 0, -1], 13.0, 10.0, "cells", "cells_back",
                    group="arrays", pivot=[xa, 0, za])
            s.box(n + "_frame", [xa, sgn * (y0 + 6.5), za + 0.06], [10.1, 0.12, 0.08], "metal", group="arrays",
                  pivot=[xa, 0, za])
            wings.append(n)
    s.source(wings, "estimate", "four rigid wings 10 x 13 m (520 m2) on the aft truss",
             "Concept art shows large arrays at the core's end; sizes estimated.")
    xf = 0.5 * Lc - 3.0
    s.box("fwd_pylon", [xf, 0, -(Rc + 0.7)], [1.0, 1.0, 1.5], "lattice", ro=False)
    s.box("fwd_truss", [xf, 0, -(Rc + 1.4)], [1.0, 34.0, 1.0], "lattice", ro=False)
    s.source(["fwd_pylon", "fwd_truss"], "estimate", "a 34 m forward truss for the radiators", REF_REEF)
    rads = []
    for sgn in (1, -1):
        n = "radiator_%s" % ("s" if sgn > 0 else "p")
        s.plane(n, [xf, sgn * 10.5, -(Rc + 1.4)], [0, sgn, 0], [0, 0, 1], 12.0, 4.5, "radiator", "radiator",
                group="radiators", pivot=[xf, 0, -(Rc + 1.4)])
        rads.append(n)
    s.source(rads, "estimate", "two 4.5 x 12 m radiators, edge-on to the Sun", "Placement and size estimated.")
    s.port("port_fwd", [0.5 * Lc + 9.3, 0, 0], "+x", r=0.75, dock=True)
    s.port("port_aft", [-0.5 * Lc, 0, 0], "-x", r=0.9)
    s.port("port_nadir", [4.0, 0, Rc], "+z", r=0.8, length=0.5, dock=True)
    for k in range(3):
        s.window("window_%d" % k, [0.5 * Lc + 2.5 + 2.2 * k, 0, 2.2], [0, 0, 1], 0.4)
    for k, xx in enumerate((-8.0, 6.0)):
        s.sleeve("core_band_%d" % k, [xx, 0, 0], "+x", 0.35, Rc + 0.05, "metal")
    s.dish("hga", [8.0, 0, -Rc], [0.0, 0.6, -0.8], 0.8, mast=1.4)
    s.write([REF_REEF])


# ── ROS ──────────────────────────────────────────────────────────────────────────────────────────────────────
def ross():
    s = Station("ross", "Russian Orbital Station (2030s configuration)",
                "Station art pass 2026-10-20 (tools/make_station_models.py), the December 2025 plan: the core "
                "assembled at the ISS from Nauka, the Universal Node Module (UUM) and the Science-Power Module (NEM) "
                "and separated from it, then the gateway airlock (ShM), a base module (an upgraded NEM) and two "
                "target modules. Body frame: +X along velocity, +Z nadir: Nauka forward, the base module aft, NEM "
                "on the node's nadir port, ShM to starboard, target modules to port and zenith. NEM and the base "
                "module each carry two large rigid wings at their far ends, turning about the cross-track axis; "
                "NEM has two radiator panels. Detail (the crew Soyuz and the cargo Progress are the sim's own), Nauka's ERA arm and radiator, "
                "ports, dishes.")
    s.arrays("arrays", "+y")
    Rn = 1.8
    s.sphere("ros_uum", [0, 0, 0], Rn, "module_skin")
    s.cyl("ros_nauka", [Rn + 0.2, 0, 0], "+x", 13.1, 2.125, "module_skin")
    s.cyl("ros_base_module", [-(Rn + 0.2), 0, 0], "-x", 11.0, 2.05, "module_skin")
    s.cyl("ros_nem", [0, 0, Rn + 0.2], "+z", 10.5, 2.05, "module_skin")
    s.cyl("ros_shm", [0, Rn + 0.1, 0], "+y", 5.0, 1.3, "module_skin")
    s.cyl("ros_target_port", [0, -(Rn + 0.1), 0], "-y", 9.0, 2.05, "module_skin")
    s.cyl("ros_target_zenith", [0, 0, -(Rn + 0.1)], "-z", 8.0, 2.05, "module_skin")
    s.source("ros_nauka", "sourced", "Nauka 13.1 x 4.25 m", REF_ROS)
    s.source(["ros_uum", "ros_base_module", "ros_nem", "ros_shm", "ros_target_port", "ros_target_zenith"],
             "estimate", "UUM a 3.6 m node; NEM / base module 4.1 x 10.5-11 m; ShM 2.6 x 5 m; target modules "
             "4.1 x 8-9 m; ports as the plan assigns them", REF_ROS)
    wings = []
    zn = Rn + 0.2 + 10.5 - 1.6
    xb = -(Rn + 0.2 + 11.0 - 1.6)
    for sgn in (1, -1):
        t = "s" if sgn > 0 else "p"
        s.wing("nem_array_" + t, "arrays", [0, sgn * 2.05, zn], "+y" if sgn > 0 else "-y", 13.0, 4.2, [0, 0, zn],
               boom=0.8)
        s.wing("base_array_" + t, "arrays", [xb, sgn * 2.05, 0], "+y" if sgn > 0 else "-y", 13.0, 4.2, [xb, 0, 0],
               boom=0.8)
        wings += ["nem_array_" + t, "base_array_" + t]
    s.source(wings, "estimate", "two pairs of rigid wings 4.2 x 13 m (218 m2)",
             "NEM renders show two large wings at its aft end; sizes estimated.")
    rads = []
    for sgn in (1, -1):
        n = "nem_radiator_%s" % ("f" if sgn > 0 else "a")
        s.plane(n, [sgn * (2.1 + 3.0), 0, Rn + 0.2 + 5.0], [sgn, 0, 0], [0, 1, 0], 6.0, 4.0, "radiator", "radiator")
        rads.append(n)
    s.source(rads, "estimate", "two 4 x 6 m radiator panels on NEM", "NEM renders; size estimated.")
    xn = Rn + 0.2 + 13.1
    s.port("port_fwd", [xn, 0, 0], "+x", r=0.7, dock=True)
    xa = -(Rn + 0.2 + 11.0)
    s.port("port_aft", [xa, 0, 0], "-x", r=0.7, dock=True, use="cargo")
    s.box("nauka_radiator", [8.5, 2.125 + 0.05, -0.8], [4.0, 0.08, 1.6], "radiator")
    s.box("era_boom_1", [6.0, -2.4, -1.4], [5.5, 0.3, 0.3], "metal")
    s.box("era_boom_2", [10.2, -2.4, -2.6], [3.0, 0.3, 0.3], "metal")
    s.dish("hga", [-6.0, 0, -2.05], "-z", 0.6, mast=1.2)
    s.sleeve("nem_band", [0, 0, Rn + 0.2 + 7.0], "+z", 0.3, 2.1, "mli")
    s.sleeve("base_band", [-(Rn + 0.2 + 7.5), 0, 0], "-x", 0.3, 2.1, "mli")
    s.write([REF_ROS])


# ── Bharatiya Antariksh Station ──────────────────────────────────────────────────────────────────────────────
def bas():
    s = Station("bharatiya_station", "Bharatiya Antariksh Station (BAS, five modules)",
                "Station art pass 2026-10-20 (tools/make_station_models.py), ISRO's five-module plan: BAS-01 (base, "
                "8 m), BAS-02 (core, 9.25 m) and BAS-05 (the common berthing module, 9.25 m, four CBM ports) in a "
                "line along velocity, BAS-03 (science) and BAS-04 (laboratory) berthed to BAS-05's side ports; all "
                "3.8 m wide. BAS-01 and BAS-02 each carry a pair of wings along +-Y (turning about Y), BAS-03 and "
                "BAS-04 a pair along +-X (turning about X); BAS-05 has none. Body frame: +X along velocity, +Z "
                "nadir. Detail: the BDS docking port, CBM rings, a cupola, the robotic arm, dishes, radiators.")
    s.arrays("arrays_y", "+y")
    s.arrays("arrays_x", "+x")
    R, g = 1.9, 0.2
    x02 = -4.625
    x01 = x02 + 9.25 + g
    x05 = x02 - g - 9.25
    s.cyl("bas_01", [x01, 0, 0], "+x", 8.0, R, "module_skin")
    s.cyl("bas_02", [x02, 0, 0], "+x", 9.25, R, "module_skin")
    s.cyl("bas_05", [x05, 0, 0], "+x", 9.25, R, "module_skin")
    xc = x05 + 0.5 * 9.25
    s.cyl("bas_03", [xc, R + g, 0], "+y", 9.25, R, "module_skin")
    s.cyl("bas_04", [xc, -(R + g), 0], "-y", 9.25, R, "module_skin")
    s.source(["bas_01", "bas_02", "bas_03", "bas_04", "bas_05"], "sourced",
             "3.8 m diameter; 8.0 / 9.25 m long; layout (27 x 22 m) from the published envelope", REF_BAS)
    wings = []
    for sgn in (1, -1):
        t = "s" if sgn > 0 else "p"
        s.wing("bas01_array_" + t, "arrays_y", [x01 + 4.0, sgn * R, 0], "+y" if sgn > 0 else "-y", 8.0, 3.4,
               [x01 + 4.0, 0, 0], boom=0.6)
        s.wing("bas02_array_" + t, "arrays_y", [0.0, sgn * R, 0], "+y" if sgn > 0 else "-y", 6.5, 3.4, [0, 0, 0],
               boom=0.6)
        wings += ["bas01_array_" + t, "bas02_array_" + t]
    for sgn, tag in ((1, "03"), (-1, "04")):
        yw = sgn * (R + g + 9.25 - 1.6)
        for sx in (1, -1):
            n = "bas%s_array_%s" % (tag, "f" if sx > 0 else "a")
            s.wing(n, "arrays_x", [xc + sx * R, yw, 0], "+x" if sx > 0 else "-x", 5.0, 3.0, [0, yw, 0], boom=0.6)
            wings.append(n)
    s.source(wings, "estimate", "pairs of rigid wings on BAS-01 (3.4 x 8 m), BAS-02 (3.4 x 6.5 m), BAS-03/04 "
             "(3 x 5 m): 158 m2", "ISRO: every module but BAS-05 has its own panels; sizes estimated from the "
             "BAS-01 model.")
    s.port("bds_port", [x01 + 8.0, 0, 0], "+x", r=0.7, dock=True)
    s.port("cbm_nadir", [xc, 0, R], "+z", r=0.9, length=0.4)
    s.prof("cupola", [xc, 0, R + 0.4], "+z", [[0, 1.1], [0.35, 1.0], [0.6, 0.6], [0.7, 0.0]], "glass", ro=True)
    s.port("cbm_aft", [x05, 0, 0], "-x", r=0.9, length=0.4)
    s.box("arm_boom_1", [x02 + 3.0, 0.9, -(R + 0.35)], [6.0, 0.3, 0.3], "metal")
    s.box("arm_boom_2", [x02 - 0.2, 0.9, -(R + 1.6)], [0.3, 0.3, 2.6], "metal")
    s.dish("hga", [x01 + 2.0, 0, -R], "-z", 0.55, mast=0.8)
    for nm, x0, ln in (("bas01", x01, 8.0), ("bas02", x02, 9.25), ("bas05", x05, 9.25)):
        s.sleeve(nm + "_band", [x0 + 0.5 * ln - 0.15, 0, 0], "+x", 0.3, R + 0.04, "gold")
    for sgn in (1, -1):
        s.box("bas02_radiator_%s" % ("n" if sgn > 0 else "z"), [x02 + 6.5, 0, sgn * (R + 0.04)], [3.0, 1.6, 0.06],
              "radiator")
    s.write([REF_BAS])


# ── Starlab ──────────────────────────────────────────────────────────────────────────────────────────────────
def starlab():
    s = Station("starlab", "Starlab (habitat + service module)",
                "Station art pass 2026-10-20 (tools/make_station_models.py), after Starlab's renders: an 8 m wide "
                "metallic habitat with domed ends and a docking node forward, a service module aft carrying four "
                "large wings in a cross at 45 deg (each turning about its own long axis), a robotic arm along the "
                "habitat. Body frame: +X along velocity, +Z nadir. (The early concept's inflatable habitat was "
                "replaced by a rigid metal module in 2023.) Detail: node ports (crew docking for the sim's Dragons), windows, dishes.")
    d1 = norm([0, 1, 1])
    d2 = norm([0, 1, -1])
    s.arrays("arrays_a", d1, vector=cross([1, 0, 0], d1))
    s.arrays("arrays_b", d2, vector=cross([1, 0, 0], d2))
    R = 4.0
    xh = -1.0
    s.hull("starlab_habitat", [xh, 0, 0], "+x",
           [[0, 2.2], [0.5, 3.6], [1.1, R], [7.6, R], [8.5, 3.5], [9.3, 2.3], [9.6, 1.3]], "module_skin",
           caps="both")
    s.cyl("starlab_node", [xh + 9.6, 0, 0], "+x", 2.6, 1.3, "module_skin")
    s.cyl("starlab_service_module", [xh - 0.1, 0, 0], "-x", 6.4, 2.2, "module_skin")
    s.source(["starlab_habitat", "starlab_node", "starlab_service_module"], "sourced",
             "habitat 8 m wide, ~9.6 m long with domed ends (shape estimated); node 2.6 m; service module 4.4 x "
             "6.4 m (estimate)", REF_STARLAB)
    xm = xh - 3.6
    wings = []
    for gname, d in (("arrays_a", d1), ("arrays_b", d2)):
        n = cross([1, 0, 0], d)
        for sgn in (1, -1):
            dd = [sgn * v for v in d]
            nm = "array_%s%s" % (gname[-1], "p" if sgn > 0 else "m")
            root = [xm, 2.2 * dd[1], 2.2 * dd[2]]
            s.plane(nm, add(root, dd, 0.8 + 7.0), dd, n, 14.0, 6.0, "cells", "cells_back", group=gname,
                    pivot=[xm, 0, 0])
            s.cyl(nm + "_boom", root, dd, 14.8, 0.1, "metal", group=gname, ro=True, pivot=[xm, 0, 0])
            wings.append(nm)
    s.source(wings, "estimate", "four rigid wings 6 x 14 m (336 m2, ~60 kW) in a cross at 45 deg",
             "Starlab renders show four arrays on the service module; the 60 kW figure sets the area.")
    xn = xh + 9.6 + 2.6
    s.port("node_fwd", [xn, 0, 0], "+x", r=0.75, dock=True)
    for k, d in enumerate(([0, 1, 0], [0, -1, 0], [0, 0, -1])):
        s.port("node_port_%d" % k, [xh + 10.9, 1.3 * d[1], 1.3 * d[2]], d, r=0.6, length=0.4, dock=k == 0)
    for k, xx in enumerate((1.5, 4.5)):
        s.window("window_%d" % k, [xh + xx, 0, R], [0, 0, 1], 0.45)
        s.sleeve("band_%d" % k, [xh + xx + 1.2, 0, 0], "+x", 0.3, R + 0.04, "metal")
    s.box("arm_boom_1", [xh + 4.0, -(R + 0.35), -1.0], [7.5, 0.4, 0.4], "metal")
    s.box("arm_boom_2", [xh + 7.6, -(R + 0.35), -2.9], [0.4, 0.4, 3.8], "metal")
    s.dish("hga", [xh - 5.5, 0, -2.2], "-z", 0.7, mast=1.0)
    s.prof("sm_engine", [xh - 6.5 - 0.6, 0, 0], "+x", [[0, 0.5], [0.6, 0.25]], "dark", ro=True)
    s.write([REF_STARLAB])


# ── Detail on the hand-made ISS and Tiangong models ─────────────────────────────────────────────────────────
GB_NOTE = "render-only detail materials (tools/make_station_models.py)"


def patch(sid, build, extra_mats):
    """Replace the gb_* components of a hand-made model with what `build` adds, and add the materials it uses."""
    path = os.path.join(OUT, sid + ".json")
    with open(path, encoding="utf-8") as f:
        m = json.load(f)
    m["components"] = [c for c in m["components"] if not c["name"].startswith("gb_")]
    m["sources"] = [e for e in m["sources"] if e.get("value") != GB_NOTE]
    s = Station(sid, m["name"], m["note"])
    build(s)
    for c in s.c:
        c["name"] = "gb_" + c["name"]
        c["render_only"] = True
    names = set()
    for c in s.c:
        n, k = c["name"], 1
        while c["name"] in names:
            k += 1
            c["name"] = "%s_%d" % (n, k)
        names.add(c["name"])
    have = {x["name"] for x in m["materials"]}
    used = set()
    for c in s.c:
        for k in ("material", "back_material", "arc_material", "inner_material"):
            if c.get(k):
                used.add(c[k])
    added = [MATERIALS[n] for n in MATERIALS if n in used and n not in have]
    m["materials"] = [x for x in m["materials"] if x["name"] not in {a["name"] for a in added}] + added
    if added:
        m["sources"].append({"subjects": [a["name"] for a in added], "status": "estimate", "value": GB_NOTE,
                             "source": "; ".join(MATERIAL_NOTES[a["name"]] for a in added)})
    m["components"] += s.c
    if s.ports:
        m["docking_ports"] = s.ports
    else:
        m.pop("docking_ports", None)
    dump_file(path, m)
    print("%-18s +%d render-only detail parts (%d parts in all)" % (sid, len(s.c), len(m["components"])))


def iss_detail(s):
    # Mast between each wing's two blankets, and the blanket boxes (beta group, the wing's own pivot): the
    # blankets were drawn floating either side of an empty gap.
    for yc in (31.5, 46.0, -31.5, -46.0):
        for zs in (1, -1):
            s.box("saw_mast", [0, yc, zs * 19.3], [0.3, 0.3, 33.4], "metal", group="beta", pivot=[0, yc, 0])
            s.box("saw_box", [0, yc, zs * 36.0], [0.5, 9.6, 0.6], "gold", group="beta", pivot=[0, yc, 0])
    # PMA-2 / IDA-2 on Harmony forward, PMA-3 / IDA-3 on Harmony zenith: the crew docking ports (the sim flies the
    # Crew Dragons there, docked and leaving: they are no longer drawn here); Zvezda aft: the cargo port (the sim docks
    # its Progress ships there, 2026-10-21)
    s.prof("pma2", [12.2, 0, 5.0], "+x", [[0, 1.0], [1.4, 0.85], [1.9, 0.8]], "metal")
    s.prof("pma3", [8.6, 0, 2.8], "-z", [[0, 1.0], [1.4, 0.85], [1.9, 0.8]], "metal")
    s.dock("ida2_harmony_fwd", [14.1, 0, 5.0], "+x")
    s.dock("ida3_harmony_zenith", [8.6, 0, 0.9], "-z")
    s.dock("zvezda_aft", [-38.46, 0, 5.0], "-x", use="cargo")
    # Canadarm2 on the mobile base, ELCs and AMS on the inboard truss
    s.box("canadarm_boom_1", [-1.0, -12.0, -3.0], [0.35, 8.5, 0.35], "metal")
    s.box("canadarm_boom_2", [2.6, -16.0, -3.0], [7.5, 0.35, 0.35], "metal")
    s.box("canadarm_base", [0.0, -8.0, -2.6], [2.0, 2.0, 0.6], "metal")
    for ys in (1, -1):
        for zs in (1, -1):
            s.box("elc", [0.0, ys * 23.5, zs * 2.9], [4.2, 2.6, 1.2], "module_skin")
    s.box("ams02", [0.0, 21.0, -3.6], [3.0, 3.0, 2.8], "metal")
    # Ku-band dish on Z1, the cupola's windows, Destiny's window, module bands
    s.dish("ku_band", [0.5, 2.0, -2.3], [0.3, 0.3, -0.9], 1.0, mast=1.6)
    s.prof("cupola_glass", [-7.75, -5.903, 9.14], "+z", [[0, 1.3], [0.25, 1.1], [0.35, 0.0]], "glass")
    s.window("destiny_window", [0.0, 0.0, 7.15], [0, 0, 1], 0.25)
    for name, x, r in (("destiny", 0.0, 2.15), ("harmony", 8.6, 2.2), ("zarya", -18.68, 2.05),
                       ("zvezda", -31.91, 2.1)):
        s.sleeve(name + "_band", [x - 0.2, 0, 5.0], "+x", 0.4, r + 0.04, "metal")
    s.box("zvezda_radiator_s", [-35.0, 2.17, 3.6], [3.5, 0.06, 1.8], "dark")
    s.box("zvezda_radiator_p", [-35.0, -2.17, 3.6], [3.5, 0.06, 1.8], "dark")


def tiangong_detail(s):
    # Masts and blanket boxes for the flexible wings (beta, the wing's pivot): they hung off the booms by nothing
    for ys in (1, -1):
        for zs in (1, -1):
            s.box("wing_mast", [0, ys * 20.5, zs * 15.3], [0.25, 0.25, 28.0], "metal", group="beta",
                  pivot=[0, ys * 20.5, 0])
            s.box("wing_box", [0, ys * 20.5, zs * 29.0], [4.4, 0.6, 0.5], "gold", group="beta",
                  pivot=[0, ys * 20.5, 0])
    # Docking ports on the node (the sim docks real Shenzhous there, forward or nadir); Tianhe aft: the cargo port
    # (the sim docks its Tianzhou ships there, 2026-10-21)
    s.port("node_port_fwd", [1.4, 0, 0], "+x", r=0.7, length=0.5, dock=True)
    s.port("node_port_nadir", [0, 0, 1.4], "+z", r=0.7, length=0.5, dock=True)
    xa = -15.2
    s.dock("tianhe_aft", [xa, 0, 0], "-x", use="cargo")
    # Tianhe's robotic arm, radiators and bands; a dish; Mengtian's exposed platform
    s.box("arm_boom_1", [-8.0, 0, -2.5], [8.0, 0.35, 0.35], "metal")
    s.box("arm_boom_2", [-12.2, 0, -4.0], [0.35, 0.35, 3.0], "metal")
    for ys in (1, -1):
        s.box("tianhe_radiator", [-11.5, ys * 2.13, 0.6], [5.0, 0.06, 2.0], "radiator")
    for x in (-8.0, -12.6):
        s.sleeve("tianhe_band", [x, 0, 0], "+x", 0.35, 2.14, "metal")
    s.dish("dish", [-6.0, 0, -2.1], "-z", 0.6, mast=1.0)
    s.box("mengtian_platform", [0, -16.5, -2.6], [3.0, 4.5, 1.0], "metal")
    s.box("wentian_airlock", [1.8, 15.0, 0], [1.2, 3.0, 2.6], "module_skin")
    for ys in (1, -1):
        s.sleeve("lab_band", [0, ys * 9.0, 0], [0, ys, 0], 0.35, 2.14, "metal")


def main():
    haven1()
    haven2()
    axiom()
    orbital_reef()
    ross()
    bas()
    starlab()
    patch("iss", iss_detail, None)
    patch("tiangong", tiangong_detail, None)


if __name__ == "__main__":
    main()
