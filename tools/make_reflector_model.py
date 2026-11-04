#!/usr/bin/env python3
"""Generate data/satellite_models/reflect_orbital.json: Reflect Orbital's quad-sail reflector (2026-11-04).

The published deployed configuration (Reflect Orbital, "Figure 2: Deployed Configuration"; the user's reference
qa/waves/20261104/1.png and the render 2.png): a square sail 18000 mm per side made of four triangular membrane
quadrants, held at their corners by two crossed telescopic booms along the square's diagonals, a ~2.44 m bus at the
crossing. The quadrant edges are scalloped (concave catenary-like edges: edge tension keeps the film flat).

Body frame (unchanged from the old 55 m plate): +Z = the mirror normal (aimed by sun_reflect_ground_site), +X toward
velocity. The booms lie along body X and Y, so the square's sides run at 45 deg.

Parameters (all have defaults matching the diagram; --cant-deg is the knob the lighting analysis varies):
  --side M          square side, corner to corner along an edge (18.0)
  --cant-deg DEG    each quadrant tilted about its hub edge, outer corners raised toward +Z (a shallow concave
                    "dish": the four normals lean toward the centre axis by DEG). 0 = coplanar (default).
  --outer-sag M     the outer (scalloped) edge's sag toward the centre at its midpoint (0.9)
  --boom-sag M      the boom-side edges' sag away from the boom (0.25)
  --gap M           film-to-boom clearance (0.15)

    python tools/make_reflector_model.py                    # writes the model
    python tools/make_reflector_model.py --cant-deg 0.5 -o some.json
Also imported by tools/reflector_quad_analysis.py (quadrant_outline / facet_normals / sail_area).
"""
import argparse
import json
import math
import os

HUB_R = 0.40      # deployer hub radius: booms start here
INNER_R = 1.30    # the film's inner (hub) edge, along each boom
EDGE_SEGS = 10    # segments per curved edge


def quadrant_outline(side=18.0, outer_sag=0.9, boom_sag=0.25, gap=0.15):
    """Base quadrant (between the +X and +Y booms) in body XY, counter-clockwise seen from +Z."""
    d = side / math.sqrt(2.0)  # boom-tip corner distance from the centre

    def edge(a, b, inward, sag, include_end=False):
        pts = []
        n = EDGE_SEGS + (1 if include_end else 0)
        for i in range(n):
            t = i / EDGE_SEGS
            s = 4.0 * sag * t * (1.0 - t)
            pts.append((a[0] + t * (b[0] - a[0]) + inward[0] * s, a[1] + t * (b[1] - a[1]) + inward[1] * s))
        return pts

    p1, p2, p3, p4 = (INNER_R, gap), (d, gap), (gap, d), (gap, INNER_R)
    r2 = 1.0 / math.sqrt(2.0)
    out = []
    out += edge(p1, p2, (0.0, 1.0), boom_sag)        # along the +X boom, bowing away from it
    out += edge(p2, p3, (-r2, -r2), outer_sag)       # the scalloped outer edge, bowing toward the centre
    out += edge(p3, p4, (1.0, 0.0), boom_sag)        # back in along the +Y boom
    return out                                       # the hub edge p4 -> p1 closes it (straight)


def polygon_area(pts):
    a = 0.0
    for i in range(len(pts)):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % len(pts)]
        a += x0 * y1 - x1 * y0
    return 0.5 * a


def sail_area(**kw):
    return 4.0 * polygon_area(quadrant_outline(**kw))


def rot2(p, k):
    c, s = [(1, 0), (0, 1), (-1, 0), (0, -1)][k]
    return (c * p[0] - s * p[1], s * p[0] + c * p[1])


def facet_normals(cant_deg):
    """Unit normals of the four quadrants (body frame): each leans toward the centre axis by cant_deg."""
    th = math.radians(cant_deg)
    out = []
    for k in range(4):
        u = rot2((math.sqrt(0.5), math.sqrt(0.5)), k)  # the quadrant's bisector (outward)
        out.append((-u[0] * math.sin(th), -u[1] * math.sin(th), math.cos(th)))
    return out


def quat_axis_angle(axis, ang):
    s = math.sin(0.5 * ang)
    return [math.cos(0.5 * ang), axis[0] * s, axis[1] * s, axis[2] * s]


def r6(x):
    return round(x, 6)


def build(side=18.0, cant_deg=0.0, outer_sag=0.9, boom_sag=0.25, gap=0.15):
    base = quadrant_outline(side, outer_sag, boom_sag, gap)
    d = side / math.sqrt(2.0)
    boom_tip = d + 0.25
    comps = []
    for k in range(4):
        pts = [rot2(p, k) for p in base]
        # The quadrant turns about its hub edge (outer corners up for cant > 0): position = the hub edge's midpoint.
        hub = rot2((0.5 * (INNER_R + gap), 0.5 * (INNER_R + gap)), k)
        u = rot2((math.sqrt(0.5), math.sqrt(0.5)), k)
        t = (-u[1], u[0], 0.0)  # z x u: the in-plane axis across the bisector
        comp = {
            "name": "sail_%d" % k,
            "type": "polygon",
            "group": "mirror",
            "vertices": [[r6(p[0] - hub[0]), r6(p[1] - hub[1])] for p in pts],
            "position": [r6(hub[0]), r6(hub[1]), 0.0],
            "material": "mirror_film",
            "back_material": "film_back",
            # Pulled out by the booms: the film grows from a bundle at the hub (radial stow, same window as the
            # booms so the corners ride the boom tips).
            "stow": {"mode": "radial", "axis": [0, 0, 1], "scale": 0.08, "phase": [0.0, 1.0]},
        }
        if cant_deg != 0.0:
            comp["rotation_quat"] = [r6(v) for v in quat_axis_angle(t, -math.radians(cant_deg))]
        comps.append(comp)
    # Booms: four half-booms of three telescoping sections each, behind the film; each section stows toward the hub.
    sections = [(HUB_R, 4.6, 0.060), (4.6, 8.8, 0.048), (8.8, boom_tip, 0.036)]
    for k in range(4):
        ax = rot2((1.0, 0.0), k)
        for si, (r0, r1, rad) in enumerate(sections):
            mid = 0.5 * (r0 + r1)
            comps.append({
                "name": "boom_%d_%d" % (k, si),
                "type": "cylinder",
                "group": "mirror",
                "radius": rad,
                "height": r6(r1 - r0 + (0.12 if si < 2 else 0.0)),  # sections overlap a little at each joint
                "position": [r6(ax[0] * mid), r6(ax[1] * mid), -0.12],
                # A cylinder runs along local Z: turn it onto the boom (X booms about Y, Y booms about X).
                "rotation_deg": [0, 90, 0] if k % 2 == 0 else [-90, 0, 0],
                "material": "boom",
                "caps": si == 2,
                "stow": {"axis": [ax[0], ax[1], 0], "root": HUB_R, "scale": 0.06, "phase": [0.0, 1.0]},
            })
    comps.append({
        "name": "hub", "type": "cylinder", "group": "mirror", "radius": HUB_R, "height": 0.30,
        "position": [0, 0, -0.15], "material": "bus_dark",
        "render_only": True,  # a small black drum behind the film: 34 lobes of nothing, kept out of the budget
    })
    comps.append({
        "name": "bus", "type": "box", "group": "mirror", "size": [2.44, 0.70, 0.50],
        "position": [0, 0, -0.55], "material": "bus_dark",
    })
    comps.append({
        "name": "bus_panel", "type": "plane", "group": "mirror", "size": [2.30, 0.60],
        "position": [0, 0, -0.81], "rotation_deg": [180, 0, 0], "material": "solar", "single_sided": True,
    })

    area = sail_area(side=side, outer_sag=outer_sag, boom_sag=boom_sag, gap=gap)
    srcs = "Reflect Orbital, 'Figure 2: Deployed Configuration' (four triangular quadrants, 18000 mm per side, " \
           "two crossed booms along the diagonals, a 2439.59 mm central bus; the user's reference " \
           "qa/waves/20261104/1.png, with the render 2.png), 2026-11-04"
    model = {
        "name": "Reflect Orbital quad-sail mirror",
        "note": ("Generated by tools/make_reflector_model.py (side %.1f m, cant %.2f deg, outer sag %.2f m, boom sag "
                 "%.2f m, gap %.2f m): film %.1f m2. Body frame: +Z = mirror normal (sun_reflect_ground_site), +X "
                 "toward velocity; booms along X and Y. Four triangular aluminised-film quadrants on two crossed "
                 "telescoping booms; the film and booms unfold after release (deployment)."
                 % (side, cant_deg, outer_sag, boom_sag, gap, area)),
        "deployment": {"unfold_delay_s": 600, "unfold_s": 900},
        "materials": [
            {"name": "mirror_film", "preset": "mirror"},
            {"name": "film_back", "preset": "aluminum", "diffuse_albedo": 0.1, "roughness": 0.3},
            {"name": "boom", "preset": "aluminum", "diffuse_albedo": 0.35, "roughness": 0.25},
            {"name": "bus_dark", "preset": "black_paint"},
            {"name": "solar", "preset": "solar_cell"},
        ],
        "attitude_groups": [{
            "name": "mirror",
            "primary": {"axis": [0, 0, 1], "target": "sun_reflect_ground_site"},
            "secondary": {"axis": [1, 0, 0], "target": "velocity"},
        }],
        "components": comps,
        "sources": [
            {"subjects": ["attitude", "mirror"], "status": "sourced",
             "value": "the mirror normal bisects the Sun and the chosen ground site",
             "source": "Wikipedia, 'Reflect Orbital' (Earendil-1 reoriented 'within seconds to keep its beam on a "
                       "target'). The site choice, lock windows and slew are the app's TargetedReflector machinery."},
            {"subjects": ["sail_0", "sail_1", "sail_2", "sail_3"], "status": "derived",
             "value": "four triangular quadrants of an 18 x 18 m square (%.1f m2 of film after the hub cut-out, the "
                      "boom clearances and the scallops); outer-edge sag %.2f m and boom-edge sag %.2f m read off "
                      "the figure; cant %.2f deg" % (area, outer_sag, boom_sag, cant_deg),
             "source": srcs + ". Coplanar by default: the scalloped edges are the tension design that keeps each "
                       "quadrant flat, and any cant splits the ground spot (docs/subsystems/satellites.md, "
                       "'Reflect Orbital quad sail')."},
            {"subjects": ["boom_0_0", "boom_0_1", "boom_0_2", "boom_1_0", "boom_1_1", "boom_1_2",
                          "boom_2_0", "boom_2_1", "boom_2_2", "boom_3_0", "boom_3_1", "boom_3_2"],
             "status": "derived",
             "value": "two crossed booms along the diagonals, %.2f m tip to tip, three telescoping sections of radius "
                      "60 / 48 / 36 mm" % (2.0 * boom_tip),
             "source": srcs + "; the section radii are estimates (unpublished)."},
            {"subjects": ["bus", "bus_panel"], "status": "derived",
             "value": "2.44 x 0.70 x 0.50 m bus behind a 0.4 m deployer hub, a solar panel on its back",
             "source": srcs + " (the 2439.59 mm dimension); width, depth and the panel are estimates."},
            {"subject": "mirror_film", "status": "derived", "value": "the mirror preset (F0 0.95, alpha 0.0005)",
             "source": "Aluminised film (Wikipedia, 'Reflect Orbital': 'plastic film coated in aluminum'). A ~5 km "
                       "spot from ~625 km is ~8 mrad, about the Sun's own 9.3 mrad disk, so the film adds almost no "
                       "blur - near-perfect specular is consistent."},
            {"subject": "film_back", "status": "estimate", "value": "dull aluminium (albedo 0.1, roughness 0.3)",
             "source": "Back of the membrane unpublished."},
            {"subject": "boom", "status": "estimate", "value": "aluminium (albedo 0.35, roughness 0.25)",
             "source": "Boom finish unpublished; the render 2.png shows light metallic booms."},
            {"subject": "bus_dark", "status": "estimate", "value": "black paint", "source": "Unpublished."},
            {"subject": "solar", "status": "estimate", "value": "solar cells on the bus's back",
             "source": "Unpublished; a small satellite's body-mounted array."},
            {"subject": "deployment", "status": "estimate",
             "value": "booms telescope out and the film unfurls over 15 min, starting 10 min after release",
             "source": "Unpublished; NASA's ACS3 (the same four-quadrant, crossed-boom layout) took ~25 min."},
        ],
    }
    return model


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--side", type=float, default=18.0)
    ap.add_argument("--cant-deg", type=float, default=0.0)
    ap.add_argument("--outer-sag", type=float, default=0.9)
    ap.add_argument("--boom-sag", type=float, default=0.25)
    ap.add_argument("--gap", type=float, default=0.15)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("-o", "--out", default=os.path.join(root, "data", "satellite_models", "reflect_orbital.json"))
    a = ap.parse_args()
    m = build(a.side, a.cant_deg, a.outer_sag, a.boom_sag, a.gap)
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(m, f, indent=2)
        f.write("\n")
    print("wrote %s (film %.1f m2)" % (a.out, sail_area(side=a.side, outer_sag=a.outer_sag, boom_sag=a.boom_sag,
                                                      gap=a.gap)))


if __name__ == "__main__":
    main()
