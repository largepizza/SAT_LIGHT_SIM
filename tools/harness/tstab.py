#!/usr/bin/env python3
"""Temporal-stability benchmark (docs/HARNESS.md, "Temporal stability").

The question it answers: how far is the image the player sees WHILE MOVING from the image the same
view settles to when the camera stops? Every temporal shortcut (the clouds' sparse march + history,
the sky TAA, adaptive rate, fast-flight mode) trades that distance for speed, and ghosting, blocky
horizons, smears and "the screen fuzzes out" are all that distance. One number per scenario, at the
speeds a player really moves at (walk = 1x the height above the ground per second, boost = 6.25x
it; pans of 90-240 deg/s), not the gentle paths earlier tests used.

    python tools/harness/tstab.py gen <profile_log.jsonl> [--index -1] -o harness_runs/tstab/t.satcmd
    python tools/harness/run.py harness_runs/tstab/t.satcmd --window 1920x1009 --out harness_runs/tstab/run1
    python tools/harness/tstab.py score harness_runs/tstab/run1 [--sheet]
    python tools/harness/tstab.py compare harness_runs/tstab/run1 harness_runs/tstab/run2

`gen` restores the user's snapshot (settings, view, cloud drift), pauses time (the clouds are then a
fixed field, so a reference taken later at the same pose is the same scene), and for each scenario
plays a camera path at 60 fps recording every frame, then returns to sampled poses with
`path goto` and settles 160 frames for the reference. `score` compares each sampled moving frame
with its reference: mean |diff| of luminance (0-255), the 95th / 99th percentile, the share of
pixels off by more than 16 levels, and the same per horizontal band (the horizon band is where the
user's complaint lives). The settled reference of the first pose is also captured twice, as the
floor (what two settles of one view differ by).
"""
import argparse
import glob
import json
import math
import os
import sys

try:
    import numpy as np
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("tstab needs Pillow + numpy (tools/harness/requirements.txt)")

R_EARTH = 6371000.0
FPS = 60
DUR = 2.5
REF_T = [0.5, 1.0, 1.5, 2.0, 2.5]
SETTLE = 160


def dest(lat, lon, bearing_deg, dist_m):
    la, lo, b, d = math.radians(lat), math.radians(lon), math.radians(bearing_deg), dist_m / R_EARTH
    la2 = math.asin(math.sin(la) * math.cos(d) + math.cos(la) * math.sin(d) * math.cos(b))
    lo2 = lo + math.atan2(math.sin(b) * math.sin(d) * math.cos(la), math.cos(d) - math.sin(la) * math.sin(la2))
    return math.degrees(la2), math.degrees(lo2)


def scenarios(lat, lon, alt, az, el, agl):
    walk = max(agl, 2.0)          # m/s: "Move speed" 1 x the height above the ground (SatelliteSim WASD)
    boost = walk * 6.25           # Shift: speed 0.5 vs 0.08 rad/s
    sc = {}

    def line(name, speed, bearing_off, pan_dps, keys=6, climb=0.0, pitch=None):
        ks = []
        for i in range(keys):
            t = DUR * i / (keys - 1)
            la, lo = dest(lat, lon, az + bearing_off, speed * t) if speed > 0 else (lat, lon)
            e = el + (pitch(t) if pitch else 0.0)
            ks.append((t, la, lo, alt + climb * t, az + pan_dps * t, max(-89.0, min(89.0, e))))
        sc[name] = ks

    line("walk", walk, 0.0, 0.0)
    line("boost", boost, 0.0, 0.0)
    line("strafe", walk, 90.0, 0.0)
    line("pan90", 0.0, 0.0, 90.0)
    line("pan240", 0.0, 0.0, 240.0)
    line("walkpan", walk, 0.0, 60.0)
    rise = 10.0 + 0.5 * walk          # Q/E: "Move speed" x (10 m/s + 0.5 x the height above the ground)
    line("rise", 0.0, 0.0, 0.0, climb=rise)
    line("pitch", 0.0, 0.0, 0.0, keys=11, pitch=lambda t: 25.0 * math.sin(t * 2.0))
    line("boostpan", boost, 0.0, 240.0)
    line("flick", 0.0, 0.0, 0.0, keys=11, pitch=None)
    sc["flick"] = [(t, lat, lon, alt, az + 720.0 * min(t, 0.5) + 720.0 * max(0.0, min(t - 1.25, 0.5)) * -1.0, el)
                   for t in [DUR * i / 25 for i in range(26)]]
    line("boostrise", 0.0, 0.0, 0.0, climb=rise * 4.0)
    line("chaos", boost, 0.0, 120.0, keys=11, climb=rise, pitch=lambda t: 12.0 * math.sin(t * 3.0))
    return sc


def cmd_gen(a):
    recs = [json.loads(l) for l in open(a.log, encoding="utf-8") if l.strip()]
    recs = [r for r in recs if r.get("record_kind", "snapshot") == "snapshot"]
    rec = recs[a.index]
    ob, cam = rec["observer"], rec["camera"]
    if "view" in rec:
        d = rec["view"]["obs_dir"]
        lat = math.degrees(math.asin(max(-1.0, min(1.0, d[2]))))
        lon = math.degrees(math.atan2(d[1], d[0]))
    else:
        lat, lon = ob["lat_deg"], ob["lon_deg"]
    alt = ob["height_offset_m"]
    agl = alt - ob.get("terrain_h_m", 0.0)
    az, el = cam["az_deg"], cam["el_deg"]
    log = os.path.abspath(a.log).replace("\\", "/")
    only = set(a.only.split(",")) if a.only else None
    L = [f"# tstab: {os.path.basename(a.log)} index={a.index}  lat={lat:.4f} lon={lon:.4f} alt={alt:.0f} agl={agl:.0f}"]
    # Variants share one launch: "tag" or "tag:key value;key value" (settings applied after the snapshot).
    variants = a.variant or [""]
    for vi, v in enumerate(variants):
        tag, _, sets = v.partition(":")
        pre = (tag + "-") if tag else ""
        L += [f"# ===== variant {tag or '(base)'}", f"snapshot {log} index={a.index}",
              "set clouds_v2.auto_exposure 0", "time pause"]
        L += [f"set {kv}" for kv in (a.set or [])]
        L += [f"set {kv.strip()}" for kv in sets.split(";") if kv.strip()]
        if vi == 0:
            L += [f"wait settle {SETTLE}", "capture floor_a", f"wait settle {SETTLE}", "capture floor_b"]
        for name, keys in scenarios(lat, lon, alt, az, el, agl).items():
            if only and name not in only:
                continue
            L.append(f"# --- {pre}{name}")
            L.append("path clear")
            for (t, la, lo, al, z, e) in keys:
                L.append(f"path key {t:.3f} lat={la:.7f} lon={lo:.7f} alt={al:.1f} az={z:.3f} el={e:.3f}"
                         + (f" simadd={t:.3f}" if a.live_time else ""))
            L += ["path goto 0", f"wait settle {SETTLE}", f"path play fps={FPS} record={pre}{name}"]
            for t in REF_T:
                L += [f"path goto {t}", f"wait settle {SETTLE}", f"capture {pre}{name}_ref_{int(round(t * FPS)):05d}"]
    L.append("quit")
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    open(a.out, "w", encoding="utf-8").write("\n".join(L) + "\n")
    print(json.dumps({"script": a.out, "lat": lat, "lon": lon, "alt": alt, "agl": agl,
                      "walk_mps": max(agl, 2.0), "boost_mps": max(agl, 2.0) * 6.25}, indent=1))


def lum(p):
    a = np.asarray(Image.open(p).convert("RGB")).astype(np.float32)
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2], a


def metrics(d):
    return {"mean": float(d.mean()), "p95": float(np.percentile(d, 95)), "p99": float(np.percentile(d, 99)),
            "gt16": float((d > 16).mean())}


def cmd_score(a):
    cap = os.path.join(a.run, "captures")
    out = {"run": a.run, "scenarios": {}}
    fa, fb = os.path.join(cap, "floor_a.png"), os.path.join(cap, "floor_b.png")
    if os.path.exists(fa) and os.path.exists(fb):
        out["floor"] = metrics(np.abs(lum(fa)[0] - lum(fb)[0]))
    sheet_rows = []
    for ref in sorted(glob.glob(os.path.join(cap, "*_ref_*.png"))):
        base = os.path.basename(ref)[:-4]
        name, idx = base.split("_ref_")
        mov = os.path.join(cap, f"{name}_{idx}.png")
        if not os.path.exists(mov):
            continue
        lm, am = lum(mov)
        lr, ar = lum(ref)
        d = np.abs(lm - lr)
        h = d.shape[0]
        nb = 8
        bands = [float(d[h * i // nb:h * (i + 1) // nb].mean()) for i in range(nb)]
        s = out["scenarios"].setdefault(name, {"frames": []})
        s["frames"].append(dict(metrics(d), frame=int(idx), bands=bands))
        if a.sheet:
            sheet_rows.append((f"{name} {idx}", am, ar, d))
    for name, s in out["scenarios"].items():
        fr = s["frames"]
        for k in ("mean", "p95", "p99", "gt16"):
            s[k] = float(np.mean([f[k] for f in fr]))
        s["bands"] = [float(np.mean([f["bands"][i] for f in fr])) for i in range(len(fr[0]["bands"]))]
    allm = [s["mean"] for s in out["scenarios"].values()]
    out["score_mean"] = float(np.mean(allm)) if allm else None
    json.dump(out, open(os.path.join(a.run, "tstab.json"), "w"), indent=1)
    print(f"{'scenario':10s} {'mean':>6s} {'p95':>6s} {'p99':>6s} {'>16':>6s}   bands (top..bottom)")
    if "floor" in out:
        f = out["floor"]
        print(f"{'floor':10s} {f['mean']:6.2f} {f['p95']:6.1f} {f['p99']:6.1f} {f['gt16']*100:5.1f}%")
    for name, s in out["scenarios"].items():
        print(f"{name:10s} {s['mean']:6.2f} {s['p95']:6.1f} {s['p99']:6.1f} {s['gt16']*100:5.1f}%   "
              + " ".join(f"{b:5.1f}" for b in s["bands"]))
    print(f"score (mean of scenario means): {out['score_mean']:.2f}")
    if a.sheet and sheet_rows:
        w = 640
        ims = []
        for label, am, ar, d in sheet_rows:
            hh = int(am.shape[0] * w / am.shape[1])
            tiles = [Image.fromarray(am.astype(np.uint8)).resize((w, hh)),
                     Image.fromarray(ar.astype(np.uint8)).resize((w, hh)),
                     Image.fromarray(np.clip(d * 4, 0, 255).astype(np.uint8)).convert("RGB").resize((w, hh))]
            row = Image.new("RGB", (w * 3, hh))
            for i, t in enumerate(tiles):
                row.paste(t, (i * w, 0))
            ImageDraw.Draw(row).text((6, 4), label + "   moving | settled | |diff| x4", fill=(255, 255, 0))
            ims.append(row)
        for n0 in range(0, len(ims), 10):
            part = ims[n0:n0 + 10]
            sh = Image.new("RGB", (w * 3, sum(i.height for i in part)))
            y = 0
            for i in part:
                sh.paste(i, (0, y))
                y += i.height
            p = os.path.join(a.run, f"tstab_sheet_{n0 // 10}.png")
            sh.save(p)
            print("sheet:", p)


def cmd_compare(a):
    rs = [json.load(open(os.path.join(r, "tstab.json"))) for r in a.runs]
    names = sorted(set().union(*[r["scenarios"].keys() for r in rs]))
    print(f"{'scenario':10s} " + " ".join(f"{os.path.basename(os.path.normpath(r)):>18s}" for r in a.runs))
    for n in names:
        print(f"{n:10s} " + " ".join(
            (f"{r['scenarios'][n]['mean']:7.2f} ({r['scenarios'][n]['gt16']*100:4.1f}%)  " if n in r["scenarios"] else " " * 18)
            for r in rs))
    print(f"{'score':10s} " + " ".join(f"{r['score_mean']:18.2f}" for r in rs))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    g = sp.add_parser("gen")
    g.add_argument("log")
    g.add_argument("--index", type=int, default=-1)
    g.add_argument("-o", "--out", required=True)
    g.add_argument("--only", help="comma-separated scenario names")
    g.add_argument("--set", action="append", help="extra 'key value' settings applied after the snapshot")
    g.add_argument("--live-time", action="store_true", help="sim time runs at 1x along the path (the references hold it)")
    g.add_argument("--variant", action="append", help="tag[:key value;key value] - several share one launch")
    g.set_defaults(fn=cmd_gen)
    s = sp.add_parser("score")
    s.add_argument("run")
    s.add_argument("--sheet", action="store_true")
    s.set_defaults(fn=cmd_score)
    c = sp.add_parser("compare")
    c.add_argument("runs", nargs="+")
    c.set_defaults(fn=cmd_compare)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
