"""Writes the Kessler debris kit: data/satellite_models/debris_{panel,radiator,chunk}_{s,m,l}.json.

The large pieces a breakup leaves (Lc >= 1 m) are drawn with these models, chosen by the parent's surface
composition (its solar arrays, radiators and structure) and by size class. A fragment's area is snapped to
its class's mean cross-section, so its flux scale is 1 and it can be drawn as a mesh (Go to works).

    python tools/make_debris_models.py

Shapes are torn and bent versions of the parts they came from: a solar array section folded along a crease
with a strip hanging off it and a stub of spar; a radiator section with its manifold pipe, bent; a piece of
bus (a box) with a flap of MLI and a sheared fitting. All tumble about their record's random axis.
Run it after changing a shape; the class areas the sim snaps to are computed from the baked model at load
(sum of facet areas / 4), so nothing else needs updating.
"""

import json
import os

OUT = os.path.join(os.path.dirname(__file__), "..", "data", "satellite_models")

SOURCES_COMMON = [
    {"subject": "attitude", "status": "sourced", "value": "uncontrolled tumble",
     "source": "Fragments are uncontrolled; the tumble law uses the record's random axis, rate and phase."},
    {"subject": "fragment", "status": "estimate", "value": "one rigid tumbling body", "source": "As 'attitude'."},
]


def model(name, note, materials, components, extra_sources):
    return {
        "name": name,
        "note": note,
        "materials": materials,
        "attitude_groups": [{"name": "fragment", "law": "tumble"}],
        "components": components,
        "sources": SOURCES_COMMON + extra_sources,
    }


def plane(name, w, h, pos, rot, mat, back):
    return {"name": name, "type": "plane", "group": "fragment", "size": [round(w, 4), round(h, 4)],
            "position": [round(p, 4) for p in pos], "rotation_deg": rot, "material": mat, "back_material": back}


def box(name, sx, sy, sz, pos, rot, mat):
    return {"name": name, "type": "box", "group": "fragment", "size": [round(sx, 4), round(sy, 4), round(sz, 4)],
            "position": [round(p, 4) for p in pos], "rotation_deg": rot, "material": mat}


def cyl(name, r, h, pos, rot, mat):
    return {"name": name, "type": "cylinder", "group": "fragment", "radius": round(r, 4), "height": round(h, 4),
            "position": [round(p, 4) for p in pos], "rotation_deg": rot, "material": mat}


def panel(cls, w, h):
    # A torn array section: the main sheet, a flap folded 28 deg along its long edge, a strip hanging off
    # the far end, and a stub of spar along the root.
    comps = [
        plane("sheet", 0.62 * w, h, [-0.19 * w, 0, 0], [0, 0, 0], "cells", "back"),
        plane("flap", 0.38 * w, 0.82 * h, [0.31 * w + 0.02, -0.06 * h, 0.06 * w], [0, -28, 3], "cells", "back"),
        plane("strip", 0.16 * w, 0.45 * h, [-0.34 * w, 0.66 * h, -0.05 * h], [-35, 0, 8], "cells", "back"),
        box("spar", 0.06 + 0.01 * w, h * 0.9, 0.05 + 0.01 * w, [-0.52 * w, 0, 0.04], [0, 0, 0], "spar"),
    ]
    mats = [{"name": "cells", "preset": "solar_cell"},
            {"name": "back", "preset": "solar_array_back"},
            {"name": "spar", "preset": "aluminum", "roughness": 0.3}]
    src = [{"subjects": ["sheet", "flap", "strip", "spar"], "status": "estimate",
            "value": f"a {w:.1f} x {h:.1f} m array section, folded, with a strip and a spar stub",
            "source": "Representative of a torn solar array (NASA SBM large fragments are panels and structure)."},
           {"subjects": ["cells", "back", "spar"], "status": "estimate", "value": "the array presets",
            "source": "Same materials as the arrays they came from."}]
    return model(f"Debris: array section ({cls})",
                 "Kessler debris kit (tools/make_debris_models.py): a torn solar array section. Its cover glass "
                 "glints as it tumbles.", mats, comps, src)


def radiator(cls, w, h):
    comps = [
        plane("panel", w, 0.7 * h, [0, -0.15 * h, 0], [0, 0, 0], "white", "white"),
        plane("bent", 0.8 * w, 0.32 * h, [-0.08 * w, 0.33 * h + 0.03, 0.08 * h], [-24, 0, 0], "osr", "white"),
        cyl("manifold", 0.04 + 0.012 * h, w * 0.95, [0, -0.5 * h, 0.02], [0, 90, 0], "pipe"),
    ]
    mats = [{"name": "white", "preset": "white_paint", "diffuse_albedo": 0.85},
            {"name": "osr", "preset": "osr_radiator"},
            {"name": "pipe", "preset": "aluminum", "roughness": 0.25}]
    src = [{"subjects": ["panel", "bent", "manifold"], "status": "estimate",
            "value": f"a {w:.1f} x {h:.1f} m radiator section with its manifold, the end bent",
            "source": "Representative of a torn deployable radiator (white paint, OSR mirror tiles)."},
           {"subjects": ["white", "osr", "pipe"], "status": "estimate", "value": "the radiator presets",
            "source": "White paint as the Starmind radiators; OSR tiles mirror the Sun."}]
    return model(f"Debris: radiator section ({cls})",
                 "Kessler debris kit (tools/make_debris_models.py): a torn radiator section. Its OSR tiles "
                 "flash like mirrors.", mats, comps, src)


def chunk(cls, sx, sy, sz):
    comps = [
        box("bus", sx, sy, sz, [0, 0, 0], [0, 0, 0], "bus"),
        plane("mli", 0.8 * sx, 0.6 * sy, [0.15 * sx, 0.35 * sy, 0.5 * sz + 0.12 * sy], [32, -12, 0], "mli", "mli"),
        box("fitting", 0.25 * sx, 0.12 * sy, 0.12 * sy, [-0.45 * sx, -0.5 * sy - 0.04 * sy, 0.1 * sz], [0, 0, 20],
            "metal"),
        cyl("stub", 0.06 * sy, 0.5 * sy, [0.3 * sx, -0.2 * sy, -0.5 * sz - 0.25 * sy], [0, 0, 0], "metal"),
    ]
    mats = [{"name": "bus", "preset": "aluminum", "roughness": 0.25, "pattern": "panel_seams"},
            {"name": "mli", "preset": "mli_foil"},
            {"name": "metal", "preset": "aluminum", "roughness": 0.35}]
    src = [{"subjects": ["bus", "mli", "fitting", "stub"], "status": "estimate",
            "value": f"a {sx:.1f} x {sy:.1f} x {sz:.1f} m piece of bus with MLI and fittings",
            "source": "Representative of structural breakup fragments."},
           {"subjects": ["bus", "mli", "metal"], "status": "estimate", "value": "aluminium and MLI presets",
            "source": "Representative."}]
    return model(f"Debris: structure ({cls})",
                 "Kessler debris kit (tools/make_debris_models.py): a piece of a satellite's bus with its MLI.",
                 mats, comps, src)


KIT = {
    "debris_panel_s": panel("S", 1.6, 1.0),
    "debris_panel_m": panel("M", 3.2, 2.0),
    "debris_panel_l": panel("L", 3.2, 9.6),
    "debris_radiator_s": radiator("S", 1.2, 1.0),
    "debris_radiator_m": radiator("M", 2.6, 2.0),
    "debris_radiator_l": radiator("L", 3.0, 7.0),
    "debris_chunk_s": chunk("S", 0.8, 0.6, 0.4),
    "debris_chunk_m": chunk("M", 1.6, 1.2, 0.6),
    "debris_chunk_l": chunk("L", 3.0, 2.4, 0.6),
}

if __name__ == "__main__":
    for mid, m in KIT.items():
        with open(os.path.join(OUT, mid + ".json"), "w", newline="\n") as f:
            json.dump(m, f, indent=4)
            f.write("\n")
        print("wrote", mid)
