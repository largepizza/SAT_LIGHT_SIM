"""Writes the geostationary satellites' models: data/satellite_models/{geo_comsat_large, geo_comsat_medium,
geo_weather, geo_spinner}.json (the "GEO belt" List constellation's classes, data/geo_satellites.json).

    python tools/make_geo_models.py

Generic stand-ins, not one spacecraft each: a GEO comsat is a box bus with two long Sun-tracking wings on its
north and south faces, deployed antenna reflectors on its east and west faces, and the N/S faces covered in
OSR radiators; a three-axis weather satellite has one wing and an instrument deck; an MSG / Fengyun-2-class
weather satellite is a spinning drum of body-mounted cells. Body frame (as every model here): +Z nadir, +X along
velocity (east, for a prograde equatorial orbit), so +Y points south (orbit anti-normal). Wings turn about Y once a
day to face the Sun. Photometric parts are kept few (bus, radiators, wings, reflectors); detail - yokes, feed
horns, the apogee engine, thrusters, booms, sensors - is render_only. Sizes are class figures from the
manufacturers' published bus dimensions and typical spans (each model's `sources` say which, and what is an
estimate).
"""

import json
import os

OUT = os.path.join(os.path.dirname(__file__), "..", "data", "satellite_models")

FLIP_Y = [0.0, 0.0, 1.0, 0.0]  # 180 deg about Y: a plane's normal +Z -> -Z (the wing's cell side; joint vector)


def mats_common():
    return [
        {"name": "bus_mli", "preset": "mli_foil", "color": [0.92, 0.74, 0.36]},
        {"name": "radiator", "preset": "osr_radiator"},
        {"name": "reflector", "preset": "white_paint", "diffuse_albedo": 0.75, "roughness": 0.6,
         "color": [0.94, 0.94, 0.92]},
        {"name": "cells", "preset": "solar_cell"},
        {"name": "cells_back", "preset": "solar_array_back"},
        {"name": "metal", "preset": "aluminum", "diffuse_albedo": 0.25, "roughness": 0.35,
         "color": [0.72, 0.72, 0.74]},
        {"name": "dark", "preset": "black_paint"},
    ]


MAT_NOTE = ("Liveries from published on-orbit / factory photographs of the classes (gold or amber MLI on the bus, "
            "silver OSR radiator panels on the north and south faces, white-painted reflectors, blue-black cells "
            "under cover glass); reflectance from the presets.")

GROUPS_TWO = [
    {"name": "bus", "primary": {"axis": [0, 0, 1], "target": "nadir"},
     "secondary": {"axis": [1, 0, 0], "target": "velocity"}},
    {"name": "wings", "parent": "bus", "hinge": {"position": [0, 0, 0], "axis": [0.0, 1.0, 0.0]},
     "joint": {"mode": "track", "vector": [0.0, 0.0, -1.0], "target": "sun"}},
]


def comsat(model_id, title, bus, wing_panels, panel, yoke, dish_r, deck_dish_r, src_bus, src_wing, src_dish,
           span_note):
    bx, by, bz = bus
    pw, pl = panel  # panel width (along X) and length (along Y)
    wl = wing_panels * pl
    root = by / 2 + yoke
    comps = [
        {"name": "bus", "type": "box", "group": "bus", "size": [bx, by, bz], "material": "bus_mli"},
        # N/S radiators stand 5 cm off the bus faces (coincident surfaces break the float occlusion form).
        {"name": "radiator_south", "type": "plane", "group": "bus", "size": [bx + 0.04, bz + 0.04],
         "position": [0, by / 2 + 0.05, 0], "rotation_deg": [-90, 0, 0], "material": "radiator",
         "single_sided": True},
        {"name": "radiator_north", "type": "plane", "group": "bus", "size": [bx + 0.04, bz + 0.04],
         "position": [0, -by / 2 - 0.05, 0], "rotation_deg": [90, 0, 0], "material": "radiator",
         "single_sided": True},
        {"name": "wing_south", "type": "plane", "group": "wings", "size": [pw, wl],
         "position": [0, root + wl / 2, 0], "rotation_quat": FLIP_Y, "material": "cells",
         "back_material": "cells_back"},
        {"name": "wing_north", "type": "plane", "group": "wings", "size": [pw, wl],
         "position": [0, -(root + wl / 2), 0], "rotation_quat": FLIP_Y, "material": "cells",
         "back_material": "cells_back"},
        # Deployed reflectors on the east and west faces, tilted 22 deg off nadir, concave side to the Earth.
        {"name": "reflector_east", "type": "cylinder", "group": "bus", "radius": dish_r, "height": 0.12,
         "position": [bx / 2 + dish_r + 0.35, 0, bz * 0.18], "rotation_deg": [0, 22, 0], "material": "reflector"},
        {"name": "reflector_west", "type": "cylinder", "group": "bus", "radius": dish_r, "height": 0.12,
         "position": [-(bx / 2 + dish_r + 0.35), 0, bz * 0.18], "rotation_deg": [0, -22, 0],
         "material": "reflector"},
        {"name": "reflector_deck", "type": "cylinder", "group": "bus", "radius": deck_dish_r, "height": 0.1,
         "position": [0.0, 0.0, bz / 2 + 0.45], "material": "reflector"},
    ]
    ro = []
    # Yokes: the struts from the bus face to each wing root.
    for s, nm in ((1, "south"), (-1, "north")):
        ro.append({"name": f"yoke_{nm}", "type": "box", "group": "wings", "size": [0.12, yoke, 0.12],
                   "position": [0, s * (by / 2 + yoke / 2), 0], "material": "metal", "render_only": True})
        ro.append({"name": f"drive_{nm}", "type": "cylinder", "group": "bus", "radius": 0.22, "height": 0.18,
                   "position": [0, s * (by / 2 + 0.12), 0], "rotation_deg": [90, 0, 0], "material": "metal",
                   "render_only": True})
        # Panel hinge lines across each wing.
        for k in range(1, wing_panels):
            ro.append({"name": f"hinge_{nm}_{k}", "type": "box", "group": "wings", "size": [pw, 0.05, 0.05],
                       "position": [0, s * (root + k * pl), 0.03], "material": "metal", "render_only": True})
    for s, nm, ang in ((1, "east", 22), (-1, "west", -22)):
        # Reflector arms and feed horns on the Earth deck pointing at each reflector.
        ro.append({"name": f"arm_{nm}", "type": "box", "group": "bus", "size": [dish_r + 0.4, 0.1, 0.1],
                   "position": [s * (bx / 2 + (dish_r + 0.4) / 2), 0, bz * 0.18 - 0.2], "material": "metal",
                   "render_only": True})
        ro.append({"name": f"feed_{nm}", "type": "cone", "group": "bus", "radius_bottom": 0.12, "radius_top": 0.28,
                   "height": 0.6, "position": [s * bx * 0.3, 0.0, bz / 2 + 0.3], "rotation_deg": [0, -s * 60, 0],
                   "material": "dark", "render_only": True})
        ro.append({"name": f"dish_rim_{nm}", "type": "cylinder", "group": "bus", "radius": dish_r * 1.02,
                   "height": 0.04, "position": [s * (bx / 2 + dish_r + 0.35), 0, bz * 0.18 - 0.07],
                   "rotation_deg": [0, ang, 0], "material": "metal", "render_only": True, "caps": False})
    ro += [
        {"name": "apogee_engine", "type": "cone", "group": "bus", "radius_bottom": 0.45, "radius_top": 0.12,
         "height": 0.7, "position": [0, 0, -bz / 2 - 0.35], "material": "metal", "render_only": True},
        {"name": "deck_tower", "type": "box", "group": "bus", "size": [bx * 0.45, by * 0.45, 0.4],
         "position": [0, 0, bz / 2 + 0.2], "material": "bus_mli", "render_only": True},
        {"name": "earth_sensor", "type": "box", "group": "bus", "size": [0.3, 0.3, 0.25],
         "position": [bx * 0.3, by * 0.3, bz / 2 + 0.12], "material": "dark", "render_only": True},
        {"name": "omni_antenna", "type": "cylinder", "group": "bus", "radius": 0.05, "height": 0.8,
         "position": [-bx * 0.3, -by * 0.3, -bz / 2 - 0.4], "material": "metal", "render_only": True},
    ]
    for i, (x, y) in enumerate(((1, 1), (1, -1), (-1, 1), (-1, -1))):
        ro.append({"name": f"thruster_pod_{i}", "type": "box", "group": "bus", "size": [0.25, 0.25, 0.3],
                   "position": [x * (bx / 2 - 0.1), y * (by / 2 - 0.1), -bz / 2 - 0.1], "material": "metal",
                   "render_only": True})
    sources = [
        {"subject": "bus", "status": "derived", "value": f"{bx} x {by} x {bz} m box (E-W x N-S x nadir), gold MLI",
         "source": src_bus},
        {"subjects": ["radiator_south", "radiator_north"], "status": "sourced",
         "value": "OSR radiator panels over the whole north and south faces (5 cm off them: a full cover keeps the MLI face under it from being a sliver lobe)",
         "source": "Standard three-axis GEO comsat thermal design: the N/S faces, which see the Sun only at a grazing "
                   "angle all year, carry the optical solar reflector radiators (e.g. Gilmore, Spacecraft Thermal "
                   "Control Handbook, ch. on communications satellites)."},
        {"subjects": ["wing_south", "wing_north"], "status": "estimate",
         "value": f"two wings of {wing_panels} panels {pw} x {pl} m on {yoke} m yokes: {span_note}",
         "source": src_wing},
        {"subject": "wings", "status": "sourced", "value": "wings turn about the N-S axis to face the Sun (one turn a day)",
         "source": "The universal GEO comsat solar array drive (SADA) arrangement: arrays on the north and south "
                   "faces rotating once per day about the pitch (orbit-normal) axis."},
        {"subject": "bus", "status": "sourced", "value": "three-axis stabilised, Earth deck to nadir, yaw along velocity",
         "source": "Standard GEO comsat attitude (Earth-pointing, zero-momentum or momentum-bias)."},
        {"subjects": ["reflector_east", "reflector_west", "reflector_deck"], "status": "estimate",
         "value": f"deployed reflectors {2 * dish_r:.1f} m on the E/W faces (22 deg off nadir), a {2 * deck_dish_r:.1f} m "
                  f"dish on the Earth deck",
         "source": src_dish},
        {"subjects": ["bus_mli", "radiator", "reflector", "cells", "cells_back", "metal", "dark"],
         "status": "estimate", "value": "class liveries and the presets' reflectance", "source": MAT_NOTE},
    ]
    return {
        "name": title,
        "note": f"Generic GEO comsat ({model_id}), written by tools/make_geo_models.py - edit the script, not this file. "
                "Body frame: +Z nadir, +X velocity (east), +Y south. A stand-in for a class: real satellites of it "
                "differ in reflector count, size and placement.",
        "materials": mats_common(),
        "attitude_groups": GROUPS_TWO,
        "components": comps + ro,
        "sources": sources,
    }


def weather():
    bx, by, bz = 2.6, 2.5, 3.6
    pw, wl, yoke = 3.6, 10.0, 1.5
    root = by / 2 + yoke
    comps = [
        {"name": "bus", "type": "box", "group": "bus", "size": [bx, by, bz], "material": "bus_mli"},
        {"name": "radiator_north", "type": "plane", "group": "bus", "size": [bx + 0.04, bz + 0.04],
         "position": [0, -by / 2 - 0.05, 0], "rotation_deg": [90, 0, 0], "material": "radiator",
         "single_sided": True},
        {"name": "wing", "type": "plane", "group": "wings", "size": [pw, wl], "position": [0, root + wl / 2, 0],
         "rotation_quat": FLIP_Y, "material": "cells", "back_material": "cells_back"},
        # The imager (ABI / AHI / FCI class) on the Earth deck, 5 cm proud of it.
        {"name": "imager", "type": "box", "group": "bus", "size": [1.6, 1.2, 1.0],
         "position": [0.2, -0.3, bz / 2 + 0.55], "material": "radiator"},
        {"name": "dish", "type": "cylinder", "group": "bus", "radius": 0.6, "height": 0.08,
         "position": [-0.95, 0.95, bz / 2 + 0.3], "material": "reflector"},
    ]
    ro = [
        {"name": "yoke", "type": "box", "group": "wings", "size": [0.12, yoke, 0.12],
         "position": [0, by / 2 + yoke / 2, 0], "material": "metal", "render_only": True},
        {"name": "drive", "type": "cylinder", "group": "bus", "radius": 0.25, "height": 0.2,
         "position": [0, by / 2 + 0.12, 0], "rotation_deg": [90, 0, 0], "material": "metal", "render_only": True},
        {"name": "magnetometer_boom", "type": "box", "group": "bus", "size": [0.06, 8.0, 0.06],
         "position": [-bx / 2 + 0.2, -(by / 2 + 4.0), -bz / 2 + 0.4], "material": "metal", "render_only": True},
        {"name": "magnetometer", "type": "box", "group": "bus", "size": [0.18, 0.18, 0.18],
         "position": [-bx / 2 + 0.2, -(by / 2 + 8.0), -bz / 2 + 0.4], "material": "dark", "render_only": True},
        {"name": "sun_platform", "type": "box", "group": "wings", "size": [1.2, 0.6, 0.6],
         "position": [0, by / 2 + yoke * 0.6, -0.6], "material": "bus_mli", "render_only": True},
        {"name": "imager_aperture", "type": "cylinder", "group": "bus", "radius": 0.3, "height": 0.08,
         "position": [0.2, -0.3, bz / 2 + 1.07], "material": "dark", "render_only": True},
        {"name": "lightning_mapper", "type": "box", "group": "bus", "size": [0.7, 0.6, 0.6],
         "position": [0.8, 0.8, bz / 2 + 0.3], "material": "bus_mli", "render_only": True},
        {"name": "apogee_engine", "type": "cone", "group": "bus", "radius_bottom": 0.4, "radius_top": 0.1,
         "height": 0.6, "position": [0, 0, -bz / 2 - 0.3], "material": "metal", "render_only": True},
    ]
    for k in range(1, 5):
        ro.append({"name": f"hinge_{k}", "type": "box", "group": "wings", "size": [pw, 0.05, 0.05],
                   "position": [0, root + k * wl / 5, 0.03], "material": "metal", "render_only": True})
    return {
        "name": "GEO weather satellite (three-axis, GOES-R class)",
        "note": "Generic three-axis GEO weather satellite (GOES-R / Himawari / MTG / Fengyun-4 / Elektro-L class), "
                "written by tools/make_geo_models.py. Body frame: +Z nadir, +X velocity, +Y south. One solar wing on "
                "the south face, the imager on the Earth deck, a magnetometer boom to the north.",
        "materials": mats_common(),
        "attitude_groups": GROUPS_TWO,
        "components": comps + ro,
        "sources": [
            {"subject": "bus", "status": "derived", "value": f"{bx} x {by} x {bz} m box, MLI",
             "source": "GOES-R series (Lockheed Martin A2100A): 6.1 x 5.6 x 3.9 m including instruments, dry mass "
                       "2857 kg (NOAA/NASA GOES-R Data Book, 2019); the bus box derived from that envelope."},
            {"subject": "wing", "status": "estimate",
             "value": f"one wing {pw} x {wl} m (5 panels) on a {yoke} m yoke",
             "source": "GOES-R, Himawari-8/9 (DS2000) and MTG carry ONE solar wing (Data Books / JMA and EUMETSAT "
                       "fact sheets); GOES-R's array ~4 kW EOL - the panel size is an estimate from that."},
            {"subject": "wings", "status": "sourced", "value": "the wing turns about the N-S axis to face the Sun",
             "source": "GOES-R Data Book: solar array drive rotating once per day."},
            {"subjects": ["radiator_north", "imager"], "status": "estimate",
             "value": "OSR radiator on the north face; the imager (1.6 x 1.2 x 1.0 m, radiator-white) on the Earth deck",
             "source": "GOES-R Data Book (ABI on the Earth-pointing platform, radiators on the anti-Sun side); sizes "
                       "estimated from the ABI's published 2 x 1.2 x 1 m envelope."},
            {"subject": "dish", "status": "estimate", "value": "a 1.2 m data-relay dish on the Earth deck",
             "source": "GOES-R GRB / DCS antennas on the Earth deck; size an estimate."},
            {"subject": "bus", "status": "sourced", "value": "three-axis, Earth deck to nadir",
             "source": "GOES-R Data Book."},
            {"subjects": ["bus_mli", "radiator", "reflector", "cells", "cells_back", "metal", "dark"],
             "status": "estimate", "value": "class liveries and the presets' reflectance", "source": MAT_NOTE},
        ],
    }


def spinner():
    comps = [
        # Drum of body-mounted cells, axis north-south (body Y): rotation about X takes the cylinder's Z to Y.
        # Capped: the end platforms count as cells photometrically (one primitive keeps the lobe count in budget);
        # their MLI look is the render-only discs below.
        {"name": "drum", "type": "cylinder", "group": "bus", "radius": 1.6, "height": 2.4,
         "rotation_deg": [90, 0, 0], "material": "cells"},
    ]
    ro = [
        {"name": "end_south", "type": "cylinder", "group": "bus", "radius": 1.6, "height": 0.04,
         "position": [0, 1.215, 0], "rotation_deg": [90, 0, 0], "material": "bus_mli", "render_only": True},
        {"name": "end_north", "type": "cylinder", "group": "bus", "radius": 1.6, "height": 0.04,
         "position": [0, -1.215, 0], "rotation_deg": [90, 0, 0], "material": "bus_mli", "render_only": True},
        {"name": "antenna_mast", "type": "cylinder", "group": "bus", "radius": 0.35, "height": 1.0,
         "position": [0, -1.82, 0], "rotation_deg": [90, 0, 0], "material": "metal", "render_only": True},
        {"name": "antenna_drum", "type": "cylinder", "group": "bus", "radius": 0.55, "height": 0.5,
         "position": [0, -2.55, 0], "rotation_deg": [90, 0, 0], "material": "bus_mli", "render_only": True},
        {"name": "imager_port", "type": "box", "group": "bus", "size": [0.6, 0.8, 0.12],
         "position": [0, 0, 1.62], "material": "dark", "render_only": True},
        {"name": "thermal_band", "type": "cylinder", "group": "bus", "radius": 1.62, "height": 0.3, "caps": False,
         "position": [0, 0.0, 0], "rotation_deg": [90, 0, 0], "material": "radiator", "render_only": True},
    ]
    return {
        "name": "GEO spin-stabilised weather satellite (MSG / Fengyun-2 class)",
        "note": "Generic spinning GEO weather drum (Meteosat Second Generation, Fengyun-2), written by "
                "tools/make_geo_models.py. The drum spins at 100 rpm about the north-south axis; photometrically it "
                "is a cylinder of cells, so the spin is not modelled. Body frame: +Z nadir, +X velocity, +Y south.",
        "materials": mats_common(),
        "attitude_groups": [GROUPS_TWO[0]],
        "components": comps + ro,
        "sources": [
            {"subject": "drum", "status": "sourced", "value": "3.2 m diameter, 2.4 m tall drum of body-mounted cells",
             "source": "EUMETSAT, 'Meteosat Second Generation (MSG) design': cylindrical, 3.2 m diameter, 2.4 m high, "
                       "spin-stabilised at 100 rpm, solar cells on the cylindrical surface; Fengyun-2 is similar "
                       "(2.1 m x 1.6 m drum)."},
            {"subjects": ["end_south", "end_north"], "status": "estimate",
             "value": "MLI-covered end platforms (drawn; photometrically the drum's caps, which are cells)",
             "source": "MSG photographs; size from the drum."},
            {"subject": "bus", "status": "sourced", "value": "spin axis parallel to the Earth's axis",
             "source": "EUMETSAT MSG design: spin axis north-south; the spin is not modelled (axisymmetric drum)."},
            {"subjects": ["bus_mli", "radiator", "reflector", "cells", "cells_back", "metal", "dark"],
             "status": "estimate", "value": "class liveries and the presets' reflectance", "source": MAT_NOTE},
        ],
    }


def main():
    models = {
        "geo_comsat_large": comsat(
            "geo_comsat_large", "GEO comsat, large (SSL-1300 / Eurostar / 702 class)",
            (2.8, 2.6, 4.2), 5, (2.6, 3.4), 2.5, 1.4, 0.6,
            "SSL-1300 (Maxar): 'body 2.4 x 2.6 x 5.4 m stowed'-class figures; Eurostar E3000: 2.4 x 2.9 x 5.8 m "
            "launch configuration; Boeing 702HP similar (manufacturers' brochures as summarised by Gunter's Space "
            "Page). The on-orbit box (stowed reflectors and the launch adapter excluded) derived from those.",
            "Large GEO comsats of 15-20 kW span ~40-48 m tip to tip (e.g. Intelsat 29e 37.5 m, ViaSat-2 ~45 m, "
            "Astra 2E ~40 m: Gunter's Space Page); 5 panels per wing an estimate.",
            "Most large comsats carry two 2.3-3 m deployed reflectors on the east/west faces plus an Earth-deck "
            "antenna (factory photographs); exact counts and sizes vary by satellite - estimates.",
            "41.6 m tip to tip"),
        "geo_comsat_medium": comsat(
            "geo_comsat_medium", "GEO comsat, medium (Star-2 / A2100 / DFH-4 class)",
            (2.0, 1.9, 3.2), 3, (2.0, 2.6), 1.6, 1.1, 0.45,
            "Orbital ATK GEOStar-2: 1.7 x 1.7 x 3.0 m bus; Lockheed A2100A: ~2 x 2 x 3 m; CAST DFH-4: 2.36 x 2.1 x "
            "3.6 m (manufacturers' figures via Gunter's Space Page). The box is their middle.",
            "Mid-size GEO comsats span ~20-30 m (Star-2 ~20 m, A2100 ~26 m); 3 panels per wing an estimate.",
            "Two ~2.2 m deployed reflectors on the E/W faces (factory photographs of the class); estimates.",
            "20.7 m tip to tip"),
        "geo_weather": weather(),
        "geo_spinner": spinner(),
    }
    for mid, doc in models.items():
        p = os.path.join(OUT, mid + ".json")
        with open(p, "w", newline="\n") as f:
            json.dump(doc, f, indent=1)
            f.write("\n")
        print("wrote", os.path.normpath(p), len(doc["components"]), "components")


if __name__ == "__main__":
    main()
