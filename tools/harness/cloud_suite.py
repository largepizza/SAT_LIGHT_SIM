#!/usr/bin/env python3
"""Cloud regression suite: daytime scenes at ordinary angles, in motion and with time running.

Built 2026-10-22 because the cloud work had been judged mostly on sunset anvils. Each scene is a fixed
place, time and view (no snapshot file needed); for each one the suite measures:

  - the settled image (still_*.png) and its cost (perf),
  - motion: tstab's player-speed paths (walk, strafe, pan90, pitch, rise) vs the settled view at
    sampled poses (scored like tools/harness/tstab.py),
  - still view, time PAUSED: captures after 40 / 300 / 1200 frames (does anything drift or vanish
    while nothing moves? e.g. cloud shadows fading in and out),
  - still view, time RUNNING at 1x: a capture every 30 frames for 600 frames (flicker and how the
    clouds' and shadows' light changes frame to frame).

    python tools/harness/cloud_suite.py gen -o harness_runs/suite/suite.satcmd [--only a,b] [--no-motion]
    python tools/harness/run.py harness_runs/suite/suite.satcmd --window 1920x1009 --out harness_runs/suite/run1
    python tools/harness/cloud_suite.py score harness_runs/suite/run1 [--sheet]
    python tools/harness/cloud_suite.py compare run1 run2
"""
import argparse
import glob
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tstab  # noqa: E402

import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

TIME = "2036-11-22T02:06:22Z"     # local noon over eastern Australia / the western Pacific
# name: (lat, lon, height, az, el, kind, sun)
#   height: "agl=N" (on the ground) or "alt=N" (m above sea level); kind: ground | air
#   sun: None = TIME (Sun 40-80 deg), or "30 setting" -> `time sun 30 setting` (mid-afternoon)
SCENES = {
    "cu_ground":    (-33.9, 151.2, "alt=30",  0.0, 14.0, "ground", None),   # Sydney: scattered cumulus over land
    "cu_air":       (-37.8, 145.0, "alt=3000", 60.0, -18.0, "air", None),   # Melbourne: broken cumulus, shadows on land
    "broken_air":   (-27.5, 153.0, "alt=3000", 60.0, -18.0, "air", None),   # Brisbane: near-overcast with gaps
    "sea_air":      (-12.5, 130.8, "alt=3000", 60.0, -18.0, "air", None),   # Darwin: cumulus over sea and coast
    "towers":       (-5.0, 163.0, "alt=30", 120.0, 14.0, "ground", None),   # tropical towers and rain, from the sea
    "trade_cu":     (7.0, -170.0, "alt=30",  0.0, 14.0, "ground", None),    # trade cumulus, Sun 38 deg
    "in_layer":     (-33.9, 151.2, "alt=1500", 120.0, 2.0, "air", None),    # level flight at cloud height
    "shadow_air":   (31.0, 121.0, "alt=3000", 330.0, -30.0, "air", None),  # Shanghai, Sun 34 deg: cumulus shadows on flat land
    "shadow_low":   (31.0, 121.0, "alt=800", 330.0, -15.0, "air", None),   # the same from under the cloud base
    "shadow_ground": (31.0, 121.0, "alt=12", 0.0, 6.0, "ground", None),    # the same at ground level
    "ovc_ground":   (36.0, 138.0, "alt=1360", 0.0, 12.0, "ground", None),     # central Japan: overcast, Sun 33 deg
}
MOTION = ["walk", "strafe", "pan90", "pitch", "rise"]
PAUSED_T = [40, 300, 1200]
RUN_EVERY, RUN_N = 30, 20


def place(name, sc):
    lat, lon, h, az, el, kind, sun = sc
    L = [f"# ===== {name}", f"time set {TIME}", "time pause", f"observer lat={lat} lon={lon} {h}"]
    if sun:
        L.append(f"time sun {sun}")
        L.append("time pause")
    L += [f"camera az={az} el={el} fov=60"]
    return L


def cmd_gen(a):
    only = set(a.only.split(",")) if a.only else None
    L = ["# cloud_suite.py gen", "set display.dynamic_resolution false", "set clouds_v2.auto_exposure 0"]
    L += [f"set {kv}" for kv in (a.set or [])]
    for name, sc in SCENES.items():
        if only and name not in only:
            continue
        lat, lon, h, az, el, kind, sun = sc
        L += place(name, sc)
        L += ["wait settle 160", f"capture {name}_still scale=0.5", f"perf frames=60 name={name}"]
        # Time paused: does anything drift while nothing moves?
        L += place(name, sc) + ["wait settle 40", f"capture {name}_p{PAUSED_T[0]:05d} scale=0.5"]
        prev = PAUSED_T[0]
        for t in PAUSED_T[1:]:
            L += [f"wait {t - prev}", f"capture {name}_p{t:05d} scale=0.5"]
            prev = t
        # Time running at 1x.
        L += place(name, sc) + ["wait settle 160", "time play", "time scale 1x"]
        for i in range(RUN_N):
            L += [f"wait {RUN_EVERY}", f"capture {name}_r{i:03d} scale=0.5"]
        L += ["time pause"]
        if a.no_motion:
            continue
        # Motion: tstab's paths at the player's speeds, from this place.
        alt = float(h.split("=")[1])            # m above sea level (ovc_ground stands on 1345 m)
        agl = alt if kind == "air" else 15.0
        for sname, keys in tstab.scenarios(lat, lon, alt, az, el, agl).items():
            if sname not in MOTION:
                continue
            L += place(name, sc) + ["path clear"]
            for (t, la, lo, al, z, e) in keys:
                altp = f" alt={al:.1f}" if sname == "rise" else ""
                L.append(f"path key {t:.3f} lat={la:.7f} lon={lo:.7f}{altp} az={z:.3f} el={e:.3f}")
            L += ["path goto 0", "wait settle 160", f"path play fps={tstab.FPS} record={name}-{sname} scale=0.5"]
            for t in tstab.REF_T:
                L += [f"path goto {t}", "wait settle 160",
                      f"capture {name}-{sname}_ref_{int(round(t * tstab.FPS)):05d} scale=0.5"]
    L.append("quit")
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    open(a.out, "w", encoding="utf-8").write("\n".join(L) + "\n")
    print(a.out, len(L), "lines")


def lum(p):
    a = np.asarray(Image.open(p).convert("RGB")).astype(np.float32)
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def hp_std(l):
    # High-pass grain: |l - 5x5 box| std (the box by cumulative sums).
    k = 5
    p = np.pad(l, k // 2, mode="edge")
    c = p.cumsum(0).cumsum(1)
    c = np.pad(c, ((1, 0), (1, 0)))
    box = (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)
    return float((l - box).std())


def cmd_score(a):
    cap = os.path.join(a.run, "captures")
    out = {"run": a.run, "scenes": {}}
    for name in SCENES:
        st = os.path.join(cap, f"{name}_still.png")
        if not os.path.exists(st):
            continue
        s = out["scenes"].setdefault(name, {})
        ls = lum(st)
        s["still_mean"] = float(ls.mean())
        s["still_grain"] = hp_std(ls)
        h = ls.shape[0]
        # Paused drift: each later capture vs the first.
        ps = sorted(glob.glob(os.path.join(cap, f"{name}_p*.png")))
        if len(ps) > 1:
            l0 = lum(ps[0])
            s["paused"] = []
            for p in ps[1:]:
                l1 = lum(p)
                d = l1 - l0
                s["paused"].append({"frame": int(os.path.basename(p)[len(name) + 2:-4]),
                                    "mean_abs": float(np.abs(d).mean()), "mean_signed": float(d.mean()),
                                    "lower_half_signed": float(d[h // 2:].mean()),
                                    "gt8": float((np.abs(d) > 8).mean())})
        # Time running: frame-to-frame change and the trend of the lower half (ground / shadows).
        rs = sorted(glob.glob(os.path.join(cap, f"{name}_r*.png")))
        if len(rs) > 2:
            L = [lum(p) for p in rs]
            steps = [np.abs(L[i + 1] - L[i]) for i in range(len(L) - 1)]
            s["run_step_mean"] = float(np.mean([d.mean() for d in steps]))
            s["run_step_gt8"] = float(np.mean([(d > 8).mean() for d in steps]))
            s["run_lower_trend"] = [round(float(l[h // 2:].mean()), 2) for l in L]
            stk = np.stack(L)
            s["run_temporal_std"] = float(stk.std(0).mean())
        # Motion.
        mo = {}
        for ref in sorted(glob.glob(os.path.join(cap, f"{name}-*_ref_*.png"))):
            base = os.path.basename(ref)[:-4]
            sn, idx = base.split("_ref_")
            mov = os.path.join(cap, f"{sn}_{idx}.png")
            if not os.path.exists(mov):
                continue
            d = np.abs(lum(mov) - lum(ref))
            mo.setdefault(sn.split("-", 1)[1], []).append((float(d.mean()), float((d > 16).mean())))
        if mo:
            s["motion"] = {k: {"mean": float(np.mean([m for m, _ in v])), "gt16": float(np.mean([g for _, g in v]))}
                           for k, v in mo.items()}
            s["motion_score"] = float(np.mean([m["mean"] for m in s["motion"].values()]))
    # perf
    res = os.path.join(a.run, "results.jsonl")
    if os.path.exists(res):
        for line in open(res, encoding="utf-8"):
            try:
                r = json.loads(line)
            except ValueError:
                continue
            cmd = r.get("cmd", "")
            if cmd.startswith("perf") and "name=" in cmd:
                nm = cmd.split("name=")[1].split()[0]
                g = (r.get("result") or {}).get("gpu_ms") or {}
                tot = (r.get("result") or {}).get("gpu_total_ms")
                if nm in out["scenes"]:
                    out["scenes"][nm]["gpu_total_ms"] = (tot.get("mean") if isinstance(tot, dict) else tot) if tot is not None else sum(g.values())
                    out["scenes"][nm]["cloud_march_ms"] = g.get("cloud_march")
    json.dump(out, open(os.path.join(a.run, "cloud_suite.json"), "w"), indent=1)
    print(f"{'scene':12s} {'gpu':>6s} {'cm':>6s} {'grain':>6s} {'paus300':>8s} {'paus1200':>9s} "
          f"{'runstep':>8s} {'run>8':>6s} {'runstd':>7s} {'motion':>7s}   per-path")
    for n, s in out["scenes"].items():
        pa = s.get("paused", [])
        p3 = next((p for p in pa if p["frame"] == 300), None)
        p12 = next((p for p in pa if p["frame"] == 1200), None)
        fmt = lambda p: f"{p['mean_signed']:+.2f}" if p else "   -"
        mot = " ".join(f"{k}={v['mean']:.2f}" for k, v in s.get("motion", {}).items())
        print(f"{n:12s} {s.get('gpu_total_ms') or 0:6.1f} {s.get('cloud_march_ms') or 0:6.1f} {s['still_grain']:6.2f} "
              f"{fmt(p3):>8s} {fmt(p12):>9s} {s.get('run_step_mean', 0):8.2f} {s.get('run_step_gt8', 0) * 100:5.1f}% "
              f"{s.get('run_temporal_std', 0):7.2f} {s.get('motion_score', 0):7.2f}   {mot}")


def cmd_compare(a):
    A = json.load(open(os.path.join(a.a, "cloud_suite.json")))["scenes"]
    B = json.load(open(os.path.join(a.b, "cloud_suite.json")))["scenes"]
    keys = ["gpu_total_ms", "cloud_march_ms", "still_grain", "run_step_mean", "run_temporal_std", "motion_score"]
    print(f"{'scene':12s} " + " ".join(f"{k[:12]:>16s}" for k in keys))
    for n in A:
        if n not in B:
            continue
        print(f"{n:12s} " + " ".join(f"{A[n].get(k) or 0:7.2f}->{B[n].get(k) or 0:7.2f}" for k in keys))


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    g = sp.add_parser("gen")
    g.add_argument("-o", "--out", required=True)
    g.add_argument("--only")
    g.add_argument("--no-motion", action="store_true")
    g.add_argument("--set", action="append", help="'section.key value' applied at the start")
    s = sp.add_parser("score")
    s.add_argument("run")
    s.add_argument("--sheet", action="store_true")
    c = sp.add_parser("compare")
    c.add_argument("a")
    c.add_argument("b")
    a = ap.parse_args()
    {"gen": cmd_gen, "score": cmd_score, "compare": cmd_compare}[a.cmd](a)


if __name__ == "__main__":
    main()
