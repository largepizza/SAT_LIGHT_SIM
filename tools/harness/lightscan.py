#!/usr/bin/env python3
"""Read the terrain-lighting numbers out of harness `debugview` captures.

The terrain term views in sat_sky.frag (debug views 10+) write LINEAR radiance straight
into the frame buffer, bypassing exposure/tonemap: the byte in the file is
srgb_encode(v * gain). This decodes a capture back to v by inverting the sRGB curve and
dividing by that view's gain, so a term's magnitude AND hue can be read at a pixel
instead of guessed from a picture. Needs Pillow only (numpy is not required).

    python tools/harness/lightscan.py <rundir> --x 800 --y 400 600 --view direct,skyamb
    python tools/harness/lightscan.py <rundir> --x 800 --split      # where the column breaks
    python tools/harness/lightscan.py <rundir> --x 800 --hue        # beauty frame hue profile

Views, their gains, and the channel meaning (sat_sky.frag debug block):

    terms      surfColor x100      direct  tDirectSun x100    skyamb   tSkyAmb x100
    night      tNight x100         moon    moonContrib x100   aurora   auroraContrib x100
    gates      dayFrac, twilightFrac, terrainShadow                 [raw 0..1]
    factors    sunLit, bounceK*10, mean skyAmbient*100              [mixed]
    skyambraw  skyAmbientTerrain x100 (its hue: blue day .. orange dusk)
    suntint    sunSpecTint (max channel 1.0)                        [hue only]
    aofactors  albedoLum, terrainAO, cloudShadowT                   [raw]
    day        dayColor (albedo)   nightmap nightColor x20          [albedo raw, map x20]
    geodot     geoSunDot, sunDot, tHit/4000                         [raw]
    sunvis     sunDiscVis, geoSunDot + dipSin, dipSin               [raw]

Only pixels the terrain march claimed (tHit > 0) get the override; read `geodot`'s B
(tHit/4000) alongside to tell a terrain pixel from plain sky, and note a beauty-frame
value of exactly 0 with a byte of 0 elsewhere means no-hit.
"""
import argparse
import json
import os
import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.exit("lightscan needs Pillow (tools/harness/requirements.txt)")

VIEWS = {
    "terms":     (100.0, ("totalR", "totalG", "totalB")),
    "direct":    (100.0, ("directR", "directG", "directB")),
    "skyamb":    (100.0, ("skyambR", "skyambG", "skyambB")),
    "night":     (100.0, ("nightR", "nightG", "nightB")),
    "moon":      (100.0, ("moonR", "moonG", "moonB")),
    "aurora":    (100.0, ("auroraR", "auroraG", "auroraB")),
    "gates":     (1.0, ("dayFrac", "twilightFrac", "terrainShadow")),
    "factors":   (1.0, ("sunLit", "bounceKx10", "skyAmbMeanX100")),
    "skyambraw": (100.0, ("skyAmbR", "skyAmbG", "skyAmbB")),
    "suntint":   (1.0, ("sunTintR", "sunTintG", "sunTintB")),
    "aofactors": (1.0, ("albedoLum", "terrainAO", "cloudShadowT")),
    "day":       (1.0, ("dayR", "dayG", "dayB")),
    "albedo":    (1.0, ("dayR", "dayG", "dayB")),
    "nightmap":  (20.0, ("mapR", "mapG", "mapB")),
    "geodot":    (1.0, ("geoSunDot", "sunDot", "tHitX4000")),
    "sunvis":    (1.0, ("sunDiscVis", "marginOverHorizon", "dipSin")),
}


def srgb_to_linear(c):
    c = c / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def views_in(rundir):
    cap = os.path.join(rundir, "captures")
    if not os.path.isdir(cap):
        cap = rundir
    found = {}
    for f in sorted(os.listdir(cap)):
        if not f.endswith(".png"):
            continue
        stem = os.path.splitext(f)[0]
        for name in VIEWS:
            if stem == name or stem.endswith("_" + name):
                found[name] = os.path.join(cap, f)
    return found


def pixels(path, xs, ys):
    im = Image.open(path)
    mode = im.mode
    p = im.convert("RGB").load()
    W, H = im.size
    return mode, W, H, {y: [p[x, y] if 0 <= x < W and 0 <= y < H else None for x in xs] for y in ys}


def decode(path, gain, names, xs, ys):
    mode, W, H, rows = pixels(path, xs, ys)
    out = []
    for y in ys:
        row = []
        for b in rows[y]:
            row.append(None if b is None
                       else {n: round(srgb_to_linear(c) / gain, 6) for n, c in zip(names, b)})
        out.append({"y": y, "px": row})
    return {"mode": mode, "size": [W, H], "rows": out}


def hue_profile(path, xs, ys):
    mode, W, H, rows = pixels(path, xs, ys)
    out = []
    for y in ys:
        vals = []
        for b in rows[y]:
            if b is None:
                vals.append(None)
                continue
            lin = [srgb_to_linear(c) for c in b]
            m = max(lin)
            vals.append({"bytes": list(b), "lin": [round(c, 6) for c in lin],
                         "lum": round(0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2], 6),
                         "sat": 0.0 if m <= 1e-12 else round((m - min(lin)) / m, 3),
                         "chroma": [round(c / m, 3) for c in lin] if m > 1e-12 else None})
        out.append({"y": y, "px": vals})
    return {"mode": mode, "size": [W, H], "rows": out}


def breaks(paths, xs, ys, top=4):
    """Per view: the rows where a channel jumps most, relative to the larger of the two values."""
    res = {}
    for v, p in paths.items():
        gain, names = VIEWS[v]
        mode, W, H, rows = pixels(p, xs, ys)
        series = [None if rows[y][0] is None else [srgb_to_linear(c) / gain for c in rows[y][0]] for y in ys]
        jumps = []
        for i in range(1, len(series)):
            a, b = series[i - 1], series[i]
            if not a or not b:
                continue
            rel = max(abs(b[c] - a[c]) / max(abs(a[c]), abs(b[c]), 1e-9) for c in range(3))
            if rel > 0.35:
                jumps.append({"y": ys[i], "relJump": round(rel, 3),
                              "from": [round(c, 6) for c in a], "to": [round(c, 6) for c in b]})
        jumps.sort(key=lambda j: -j["relJump"])
        res[v] = jumps[:top]
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rundir")
    ap.add_argument("--x", type=int, default=800)
    ap.add_argument("--y", type=int, nargs="+", default=None)
    ap.add_argument("--step", type=int, default=50, help="row spacing when --y is omitted")
    ap.add_argument("--view", default="terms,direct,skyamb,night,gates,factors,skyambraw,suntint,"
                                     "geodot,sunvis,day,nightmap,aofactors")
    ap.add_argument("--split", action="store_true", help="per-view biggest down-column jumps")
    ap.add_argument("--hue", action="store_true", help="beauty/reference frame hue profile")
    ap.add_argument("--ref", default=None, help="capture name (or path) to profile with --hue")
    args = ap.parse_args()

    paths = views_in(args.rundir)
    if not paths and not args.hue:
        sys.exit(f"no debugview captures found in {args.rundir}")
    ys = args.y if args.y else list(range(40, 900, args.step))

    if args.hue:
        ref = args.ref
        if not ref:
            cap = os.path.join(args.rundir, "captures")
            cands = [f for f in sorted(os.listdir(cap))
                     if f.endswith(".png") and not any(f == v + ".png" or f.endswith("_" + v + ".png")
                                                       for v in VIEWS)]
            if not cands:
                sys.exit("no non-debugview capture found; pass --ref")
            ref = os.path.join(cap, cands[0])
        elif not os.path.exists(ref):
            ref = os.path.join(args.rundir, "captures", ref if ref.endswith(".png") else ref + ".png")
        prof = hue_profile(ref, [args.x], ys)
        print(json.dumps({"rundir": args.rundir, "x": args.x, "ref": os.path.basename(ref),
                          "size": prof["size"], "mode": prof["mode"]}))
        for row in prof["rows"]:
            print(json.dumps({"y": row["y"], **(row["px"][0] or {"no_pixel": True})}))
        return

    if args.split:
        want = {v: paths[v] for v in [s.strip() for s in args.view.split(",") if s.strip()] if v in paths}
        print(json.dumps({"rundir": args.rundir, "x": args.x, "jumps": breaks(want, [args.x], ys)}))
        return

    decoded = {}
    for v in [s.strip() for s in args.view.split(",") if s.strip()]:
        if v not in paths:
            continue
        gain, names = VIEWS[v]
        decoded[v] = decode(paths[v], gain, names, [args.x], ys)
    print(json.dumps({"rundir": args.rundir, "x": args.x,
                      "views": {v: {"png": os.path.basename(paths[v]), "size": d["size"], "mode": d["mode"]}
                                for v, d in decoded.items()}}))
    for i, y in enumerate(ys):
        line = {"y": y}
        for v, d in decoded.items():
            px = d["rows"][i]["px"][0]
            if px is not None:
                line[v] = px
        print(json.dumps(line))


if __name__ == "__main__":
    main()
