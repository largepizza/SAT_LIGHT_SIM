#!/usr/bin/env python3
"""Matrix regression suite: ALTITUDE x MOTION, plus CONTEXT rows (docs/HARNESS.md, "Matrix suite").

Built 2026-10-22 from the user's observation that most issues arise with movement (regular, fast, camera),
altitude (0 m .. 5000 km) or context (Reflect beams, aurora, 3D models, launches, the sea), and a bug found
moving in orbit within a minute of QA that no biome-oriented suite (cloud_suite.py) would have hit.

    python tools/harness/matrix_suite.py gen harness_runs/mx/mx.satcmd [--quick] [--only altitude=1km+3km,context=beams]
    python tools/harness/run.py harness_runs/mx/mx.satcmd --window 1920x1009 --out harness_runs/mx/run1
    python tools/harness/matrix_suite.py score harness_runs/mx/run1
    python tools/harness/matrix_suite.py compare harness_runs/mx/run1 harness_runs/mx/run2

`gen --fragment` writes the commands only (no `quit`, no header), for pasting into a larger batch; every capture
name starts with `mx_`. Every cell starts from an ABSOLUTE state (time set + paused, cloud drift reset, observer,
camera, follow / selection / aurora reset), never from the previous cell. The settings the suite changes are set
back at the end from --restore (default user_data/settings.json when it exists).

The matrix
  rows ALTITUDE (Shanghai's flat delta, 31.0 N 121.0 E, ground ~4 m, broken cumulus, Sun 34 deg at
  2036-11-22T02:06:22Z, cloud_suite's shadow_* scenes): 0m 100m 1km 3km 15km 100km 500km 1000km 5000km
  cols MOTION, at the PLAYER's speeds for that height (CLAUDE.md "Controls"):
        still  settled reference + `perf`; noise = 16 consecutive frames with time PAUSED (the temporal passes'
               own sampling noise, flicker.py's metric); live = the same with time running at 1x
        walk   WASD: max(agl, 3) m/s over the ground, capped at 0.08 rad/s (~510 km/s)
        strafe the same sideways
        fast   boost: 6.25x walk, capped at 0.5 rad/s (~3200 km/s)
        look   pan 90 deg/s + pitch +-20 deg
        rise / descend  Q / E: dh/dt = +-(10 m/s + 0.5 h), i.e. (h + 20) e^(+-t/2)
  rows CONTEXT: storm (towers from the sea, anvils from 15 km), sea (trade cumulus from 20 m), night (Shanghai
  from 3 km at 22 h local), beams (the intro's vantage at twilight + `beams go`), aurora (Kp 8 from Coldfoot,
  Kp ~7 from 420 km), iss / starmind (3D models in follow mode: frame sequences with time running), launch (the
  default twilight Starship bookmark's moment: plume noise + live), eclipse (the 2037-07-13 totality).
Moving frames are scored against settled references at the same pose (tstab.py's metric: mean |diff| of
luminance, % pixels > 16 levels), still frames by per-pixel temporal std (flicker.py's metric).
"""
import argparse
import glob
import json
import math
import os
import sys

R_EARTH = 6371000.0
FPS = 60
DUR = 2.5
TIME = "2036-11-22T02:06:22Z"
DRIFT_RATE = "0.0000065"
SETTLE_REF = 120          # a reference is within the noise floor by ~120 frames (cloud_suite.py)
SETTLE_PRE = 60
ORBIT_WAIT = 90           # from orbit: 90 ordinary frames after a settle clear the 5-km cloud-edge blocks (HARNESS.md)
NOISE_FRAMES = list(range(40, 56))
SCALE = 0.5
GROUND_SH = 4.0           # Shanghai's ground, m above sea level (the delta: 2-5 m)

# Settings the suite changes (restored at the end).
SETS = [("display.dynamic_resolution", "false"), ("clouds_v2.auto_exposure", "0"), ("clouds.drift_rate", DRIFT_RATE)]

MOTIONS_FULL = ["walk", "strafe", "fast", "look", "rise", "descend"]
MOTIONS_QUICK = ["walk", "fast", "look", "rise"]
REFS_FULL = {"walk": [1.25, 2.5], "fast": [1.25, 2.5], "look": [2.5], "strafe": [2.5], "rise": [2.5], "descend": [2.5]}


def label_alt(m):
    return f"{int(m // 1000)}km" if m >= 1000 else f"{int(m)}m"


# (agl m, az, el) per altitude row; camera views chosen so each shows cloud: horizon from low, the deck from
# above, the Earth with its limb from orbit, the disc from 5000 km.
ALT_ROWS = [(0, 0.0, 6.0), (100, 330.0, 3.0), (1000, 330.0, 2.0), (3000, 330.0, -30.0), (15000, 330.0, -15.0),
            (100000, 330.0, -25.0), (500000, 330.0, -40.0), (1000000, 330.0, -50.0), (5000000, 330.0, -80.0)]


class Cell:
    """One matrix row. pose = (lat, lon, alt_asl or None for agl placement, az, el, fov), agl = height above the
    ground for the player's speeds; setup = commands that reach the absolute state (before `observer`);
    pre_observer / post = extra commands; motions = paths; seq = follow-mode frame sequences [(label, scale, n)]."""

    def __init__(self, name, row, context, time, lat=None, lon=None, alt=None, agl=0.0, az=None, el=None, fov=60.0,
                 ground=0.0, motions=(), quick_motions=None, extra_setup=(), post_setup=(), stills=True,
                 noise=True, live=True, seq=(), extra_stills=(), teardown=(), app_pose=False, orbit=False,
                 cam_after=False):
        self.__dict__.update(locals())
        del self.__dict__["self"]


def build_cells():
    cells = []
    for agl, az, el in ALT_ROWS:
        nm = "alt" + label_alt(agl)
        mot = list(MOTIONS_FULL)
        if agl == 0:
            mot.remove("descend")
        if agl > 15000:
            mot.remove("strafe")      # from orbit a strafe is a walk in another direction
        cells.append(Cell(nm, label_alt(agl), "clouds", TIME, 31.0, 121.0, None if agl == 0 else GROUND_SH + agl,
                          agl=agl, az=az, el=el, ground=GROUND_SH, motions=mot,
                          quick_motions=[m for m in MOTIONS_QUICK if m in mot], orbit=agl >= 100000))
    C = []
    # Storm: tropical towers and rain from the sea (cloud_suite "towers"), and the anvils from above.
    C.append(Cell("storm_gnd", "0m", "storm", TIME, -5.0, 163.0, 30.0, agl=30, az=120.0, el=14.0,
                  motions=["walk", "fast", "look"], quick_motions=["look"]))
    C.append(Cell("storm_15km", "15km", "storm", TIME, -5.0, 163.0, 15000.0, agl=15000, az=120.0, el=-10.0,
                  motions=["walk", "fast", "look"], quick_motions=["fast"]))
    # The sea: trade cumulus over open Pacific from 20 m (waves, glitter, reflected clouds).
    C.append(Cell("sea_20m", "20m", "sea", TIME, 7.0, -170.0, 20.0, agl=20, az=0.0, el=6.0,
                  motions=["walk", "fast", "look"], quick_motions=["walk"]))
    # Night: Shanghai from 3 km at 22:06 local (city light under cloud, moonlight).
    C.append(Cell("night_3km", "3km", "night", "2036-11-22T14:06:22Z", 31.0, 121.0, GROUND_SH + 3000, agl=3000,
                  az=330.0, el=-20.0, motions=["walk", "fast", "look"], quick_motions=["fast"]))
    # Reflect beams: the intro's vantage at the intro's moment (twilight, the mirrors aimed at Topaz), then
    # `beams go` (the busiest converged site now: app-posed, stills only).
    # Reflect beams at the intro's moment (twilight, the mirrors aimed at Topaz). run 1 (2026-10-22) found the
    # intro vantage shows the mirrors but no beam on screen, so the cells stand where `beams go` put the observer
    # then (site 18 = Topaz, 62 converged beams; deterministic at this paused moment): 20 km west of the site
    # looking at its glow with the beams rising into the frame, and under the site looking up the beams.
    C.append(Cell("beams_site", "0m", "beams", TIME, 35.3798, -120.2706, 731.0, agl=2, az=89.9, el=8.0, fov=60.0,
                  motions=["look", "walk", "fast"], quick_motions=["look"]))
    C.append(Cell("beams_up", "0m", "beams", TIME, 35.38, -120.05, 800.0, agl=2, az=-72.6, el=19.4, fov=90.0,
                  motions=["look"], quick_motions=["look"]))
    # Aurora: Kp 8 from a dark site (Coldfoot), and the Solar Storm bookmark's G2 storm from 420 km.
    C.append(Cell("aur_gnd", "0m", "aurora", "2036-12-21T06:00:00Z", 67.25, -150.18, None, agl=2, az=0.0, el=30.0,
                  fov=100.0, motions=["look", "walk"], quick_motions=["look"],
                  extra_setup=["aurora kp 8"], post_setup=["time sun midnight", "time pause"]))
    C.append(Cell("aur_orbit", "420km", "aurora", "2037-01-12T18:00:00Z", 62.0, 90.0, 420000.0, agl=420000,
                  az=0.0, el=-18.0, fov=80.0, motions=["walk", "fast"], quick_motions=["walk"], orbit=True))
    # 3D models (follow mode: `path play` would end it, so frame sequences with time running, + `perf` while
    # running). The ISS at the moment of broll_earth.satcmd's NYC pass (a visible pass: sunlit over a twilight
    # Earth), framed as iss_radiators.satcmd; run 1's TIME put it in the Earth's shadow (a black model).
    C.append(Cell("iss", "orbit", "model", "j2000:1164233557.6", 40.7, -74.0, None, agl=100, app_pose=True,
                  noise=False, live=False,
                  post_setup=['select const "International Space Station" n=0', "follow offset=10,-120,15",
                              "follow aim=sat", "camera fov=60"],
                  seq=[("1x", "1x", 8), ("10x", "10x", 8)], teardown=["follow off", "select none"], orbit=True))
    # The default "Starmind Deployment" bookmark: a sunlit Starship releasing Starmind satellites (run 1's
    # formation-ring view from below showed only points against the stars).
    C.append(Cell("starmind", "orbit", "model", TIME, None, None, None, app_pose=True, noise=False, live=False,
                  post_setup=["wait 200", "launch deploy filter=Starmind", "wait 5", "follow offset=-140,300,55",
                              "follow aimat=-70,0,25", "camera fov=55", "time pause", "time add 110"],
                  seq=[("1x", "1x", 8), ("10x", "10x", 8)], teardown=["follow off", "select none"], orbit=True))
    # A launch plume: the default "Twilight Starship Launch" bookmark's moment. The camera is set absolutely after
    # `launch look` (az -158.2 there in run 1) so the look path can pan from it.
    C.append(Cell("launch", "plume", "launch", "2036-11-22T11:50:00Z", az=-158.2, el=24.0, fov=75.0, cam_after=True,
                  post_setup=["wait 200", "launch next twilight=1", "time add 330", "launch look stage=1",
                              "time add -40", "time pause"], motions=["look"], quick_motions=["look"]))
    # The 2037-07-13 total solar eclipse at greatest eclipse on the shadow axis (run 1: Sun seen 0, the sky light
    # 3.7%, the exposure adapted to it), at the bookmark's narrow field so the corona is resolved.
    C.append(Cell("eclipse", "0m", "eclipse", "2037-07-13T02:42:56Z", -21.75, 138.31, None, agl=2, az=3.248,
                  el=46.405, fov=20.0, motions=["look", "walk"], quick_motions=["look"]))
    return cells + C


# ---------------------------------------------------------------- path geometry
def dest(lat, lon, bearing_deg, dist_m):
    la, lo, b, d = math.radians(lat), math.radians(lon), math.radians(bearing_deg), dist_m / R_EARTH
    la2 = math.asin(math.sin(la) * math.cos(d) + math.cos(la) * math.sin(d) * math.cos(b))
    lo2 = lo + math.atan2(math.sin(b) * math.sin(d) * math.cos(la), math.cos(d) - math.sin(la) * math.sin(la2))
    return math.degrees(la2), math.degrees(lo2)


def speeds(agl):
    walk = min(max(agl, 3.0), 0.08 * R_EARTH)
    fast = min(6.25 * max(agl, 3.0), 0.5 * R_EARTH)
    return walk, fast


def path_keys(c, motion):
    """[(t, {channel: value})] for one motion from the cell's pose. Channels left out inherit (az / el for an
    app-aimed camera, alt for a cell placed by agl)."""
    walk, fast = speeds(c.agl)
    n = 6 if motion not in ("look",) else 11
    ks = []
    for i in range(n):
        t = DUR * i / (n - 1)
        k = {}
        if motion in ("walk", "strafe", "fast"):
            v = fast if motion == "fast" else walk
            brg = (c.az if c.az is not None else 0.0) + (90.0 if motion == "strafe" else 0.0)
            k["lat"], k["lon"] = dest(c.lat, c.lon, brg, v * t)
        elif motion == "look":
            k["az"] = c.az + 90.0 * t
            k["el"] = max(-89.0, min(89.0, c.el + 20.0 * math.sin(2.0 * t)))
        elif motion in ("rise", "descend"):
            s = 1.0 if motion == "rise" else -1.0
            h = (c.agl + 20.0) * math.exp(0.5 * s * t) - 20.0
            k["alt"] = c.ground + max(h, 2.0)
        if c.alt is not None and "alt" not in k:
            k["alt"] = c.alt
        if c.az is not None and "az" not in k:
            k["az"], k["el"] = c.az, c.el
        ks.append((t, k))
    return ks


def fmt_key(t, k):
    parts = [f"path key {t:.3f}"]
    for ch, f in (("lat", "{:.7f}"), ("lon", "{:.7f}"), ("alt", "{:.1f}"), ("az", "{:.3f}"), ("el", "{:.3f}")):
        if ch in k:
            parts.append(f"{ch}=" + f.format(k[ch]))
    return " ".join(parts)


# ---------------------------------------------------------------- generation
def setup(c):
    L = [f"# ===== {c.name} ({c.context}, {c.row})", "follow off", "select none", "aurora auto", "debugview off",
         "path clear", "time pause", "time scale 1x",
         f"time j2000 {c.time[6:]}" if c.time.startswith("j2000:") else f"time set {c.time}",
         f"drift default rate={DRIFT_RATE}"]
    L += list(c.extra_setup)
    if c.lat is not None:
        h = f"alt={c.alt:.1f}" if c.alt is not None else f"agl={c.agl:g}"
        L.append(f"observer lat={c.lat} lon={c.lon} {h}")
    cam = [f"camera az={c.az} el={c.el} fov={c.fov}"] if c.az is not None else []
    L += ([] if c.cam_after else cam) + list(c.post_setup) + (cam if c.cam_after else [])
    return L


def settle(c, n=SETTLE_REF):
    return [f"wait settle {n}"] + ([f"wait {ORBIT_WAIT}"] if c.orbit else [])


def inherited_path(dur=1.0):
    return ["path clear", "path key 0", f"path key {dur}"]


def gen_cell(c, quick):
    p = f"mx_{c.name}"
    L = setup(c) + settle(c) + [f"capture {p}_still scale={SCALE}", f"perf frames=60 name={p}"]
    fr = ",".join(str(f) for f in NOISE_FRAMES)
    if c.noise:
        L += inherited_path() + [f"path play fps={FPS} record={p}_n frames={fr} scale={SCALE}"]
    if c.live and not quick:
        L += ["time play", "time scale 1x"] + inherited_path() + \
             [f"path play fps={FPS} record={p}_l frames={fr} scale={SCALE}", "time pause"]
    for tag, cmds in c.extra_stills:
        L += setup(c) + ["wait 10"] + cmds + settle(c) + [f"capture {p}_{tag} scale={SCALE}"]
        if c.noise:
            L += inherited_path() + [f"path play fps={FPS} record={p}_{tag}_n frames={fr} scale={SCALE}"]
    for tag, rate, n in c.seq if not quick else c.seq[:1]:
        L += setup(c) + settle(c) + ["time play", f"time scale {rate}", "wait 5"]
        for i in range(n):
            L += [f"capture {p}_s{tag}_{i:03d} scale={SCALE}", "wait 2"]
        L += [f"perf frames=30 name={p}_s{tag}", "time pause", "time scale 1x"]
    motions = (c.quick_motions if c.quick_motions is not None else c.motions) if quick else c.motions
    for m in motions:
        refs = [DUR] if quick else REFS_FULL[m]
        rec = f"{p}-{m}"
        L += [f"# --- {c.name} {m}"] + setup(c) + ["path clear"]
        L += [fmt_key(t, k) for t, k in path_keys(c, m)]
        L += ["path goto 0", f"wait settle {SETTLE_PRE}",
              f"path play fps={FPS} record={rec} frames={','.join(str(int(round(t * FPS))) for t in refs)} scale={SCALE}"]
        for t in refs:
            L += [f"path goto {t}"] + settle(c) + [f"capture {rec}_ref_{int(round(t * FPS)):05d} scale={SCALE}"]
    L += list(c.teardown)
    return L


def parse_only(s):
    if not s:
        return None
    flt = {}
    for tok in s.split(","):
        k, _, v = tok.partition("=")
        if not v:
            k, v = "cell", k
        flt.setdefault(k.strip().lower(), set()).update(x.strip() for x in v.split("+"))
    return flt


def selected(c, flt):
    if not flt:
        return True
    ok = []
    if "cell" in flt:
        ok.append(c.name in flt["cell"])
    if "altitude" in flt:
        ok.append(c.context == "clouds" and c.row in flt["altitude"])
    if "context" in flt:
        ok.append(c.context in flt["context"])
    return any(ok)


def restore_lines(path):
    L = ["# ===== restore"]
    vals = {}
    if path and os.path.exists(path):
        j = json.load(open(path, encoding="utf-8"))
        for k, _ in SETS:
            sec, key = k.split(".", 1)
            if sec in j and key in j[sec]:
                vals[k] = j[sec][key]
    for k, v in SETS:
        if k in vals:
            x = vals[k]
            L.append(f"set {k} {str(x).lower() if isinstance(x, bool) else repr(x)}")
        else:
            L.append(f"# (no {k} in {path}: left at the suite's value)")
    L += ["follow off", "select none", "aurora auto", "debugview off", "path clear", "time scale 1x", "time pause",
          "drift default"]
    return L


def estimate(lines):
    # Calibrated on harness_runs/suite/fast1 (1920x1009, cloud_suite): a settle frame ~30 ms, a path frame
    # ~30 ms, a capture ~150 ms on top, perf ~1.9 s for 60 frames, other commands ~20 ms.
    s = 0.0
    fr = 0
    for l in lines:
        w = l.split()
        if not w or w[0].startswith("#"):
            continue
        if w[0] == "wait" and len(w) > 1 and w[1] == "settle":
            n = int(w[2]) if len(w) > 2 else 40
            s += n * 0.0302; fr += n
        elif w[0] == "wait" and len(w) > 1 and w[1].isdigit():
            s += int(w[1]) * 0.03; fr += int(w[1])
        elif w[0] == "path" and w[1] == "play":
            n = int(DUR * FPS + 1) if "-" in l.split("record=")[1].split()[0] else int(1.0 * FPS + 1)
            caps = len(l.split("frames=")[1].split()[0].split(",")) if "frames=" in l else 0
            s += n * 0.0305 + caps * 0.15; fr += n
        elif w[0] == "capture":
            s += 0.15 + 5 * 0.03; fr += 5
        elif w[0] == "perf":
            s += 1.9; fr += 64
        else:
            s += 0.02
    return s, fr


def cmd_gen(a):
    flt = parse_only(a.only)
    cells = [c for c in build_cells() if selected(c, flt)]
    body = [f"set {k} {v}" for k, v in SETS]
    for c in cells:
        body += gen_cell(c, a.quick)
    body += restore_lines(a.restore)
    sec, frames = estimate(body)
    head = [f"# matrix_suite.py gen {'--quick ' if a.quick else ''}{'--only ' + a.only if a.only else ''}".rstrip(),
            f"# {len(cells)} cells, ~{frames} frames, estimated {sec / 60:.1f} min at 1920x1009",
            f"# python tools/harness/run.py {a.out} --window 1920x1009 --out <run dir>"]
    L = (body if a.fragment else head + body + ["quit"])
    if a.fragment:
        L = [f"# matrix_suite fragment: {len(cells)} cells, ~{frames} frames, ~{sec / 60:.1f} min"] + L
    os.makedirs(os.path.dirname(os.path.abspath(a.out)) or ".", exist_ok=True)
    open(a.out, "w", encoding="utf-8").write("\n".join(L) + "\n")
    print(json.dumps({"script": a.out, "cells": [c.name for c in cells], "lines": len(L), "frames": frames,
                      "est_min": round(sec / 60, 1)}, indent=1))


# ---------------------------------------------------------------- scoring
def _np():
    try:
        import numpy as np
        from PIL import Image
        return np, Image
    except ImportError:
        sys.exit("matrix_suite score needs Pillow + numpy (tools/harness/requirements.txt)")


def lum(p):
    np, Image = _np()
    a = np.asarray(Image.open(p).convert("RGB")).astype(np.float32)
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def hp_std(l):
    np, _ = _np()
    k = 5
    p = np.pad(l, k // 2, mode="edge")
    c = p.cumsum(0).cumsum(1)
    c = np.pad(c, ((1, 0), (1, 0)))
    box = (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)
    return float((l - box).std())


def median3(l):
    """3x3 median: removes isolated point sources (satellites, stars, city glitter, sprite twinkle) so a motion
    score measures the clouds / ground / sea, not points that a settle re-draws a pixel over."""
    np, _ = _np()
    try:
        from scipy.ndimage import median_filter
        return median_filter(l, size=3, mode="nearest")
    except ImportError:
        p = np.pad(l, 1, mode="edge")
        h, w = l.shape
        st = np.stack([p[y:y + h, x:x + w] for y in range(3) for x in range(3)])
        return np.median(st, axis=0)


def motion_metrics(mov, ref):
    """Moving frame vs settled reference. Primary (`mean`, `gt16`, `p95`): point sources removed (median3).
    `raw_mean` / `raw_gt16`: as captured. `signed`: mean(moving - ref) over content pixels (ref > 12 after the
    median): < 0 = content lost in motion (clouds fading, history thinning), > 0 = blur / bloat / glow lag.
    `bands`: |diff| per eighth of the height, top to bottom (the limb / horizon is where most bugs live)."""
    np, _ = _np()
    lm, lr = lum(mov), lum(ref)
    d0 = np.abs(lm - lr)
    fm, fr = median3(lm), median3(lr)
    d = np.abs(fm - fr)
    m = fr > 12
    h = d.shape[0]
    return {"mean": float(d.mean()), "gt16": float((d > 16).mean()), "p95": float(np.percentile(d, 95)),
            "raw_mean": float(d0.mean()), "raw_gt16": float((d0 > 16).mean()),
            "signed": float((fm - fr)[m].mean()) if m.any() else 0.0,
            "bands": [float(d[h * i // 8:h * (i + 1) // 8].mean()) for i in range(8)]}


def write_sheet(path, pairs):
    """moving | settled | |diff| x4 rows, one per (label, moving png, ref png)."""
    np, Image = _np()
    from PIL import ImageDraw
    rows = []
    for label, mov, ref in pairs:
        a = np.asarray(Image.open(mov).convert("RGB")).astype(np.float32)
        b = np.asarray(Image.open(ref).convert("RGB")).astype(np.float32)
        d = np.clip(np.abs(a - b).mean(2) * 4, 0, 255).astype(np.uint8)
        h, w = a.shape[:2]
        row = Image.new("RGB", (w * 3, h))
        row.paste(Image.fromarray(a.astype(np.uint8)), (0, 0))
        row.paste(Image.fromarray(b.astype(np.uint8)), (w, 0))
        row.paste(Image.fromarray(d).convert("RGB"), (2 * w, 0))
        ImageDraw.Draw(row).text((6, 4), label + "   moving | settled | |diff| x4", fill=(255, 255, 0))
        rows.append(row)
    if rows:
        sh = Image.new("RGB", (rows[0].width, sum(r.height for r in rows)))
        y = 0
        for r in rows:
            sh.paste(r, (0, y))
            y += r.height
        sh.save(path)


def temporal(files):
    """flicker.py's statistics over a frame sequence: per-pixel std (pixels brighter than 6 on average)."""
    np, _ = _np()
    if len(files) < 4:
        return None
    s = np.stack([lum(f) for f in files])
    m = s.mean(0) > 6
    if not m.any():
        m = np.ones_like(m)
    sd = s.std(0)
    d1 = np.abs(np.diff(s, axis=0)).mean(0)
    return {"std": float(sd[m].mean()), "d1": float(d1[m].mean()), "p99": float(np.percentile(sd[m], 99)),
            "gt8": float((sd[m] > 8).mean())}


def cmd_score(a):
    np, _ = _np()
    cap = os.path.join(a.run, "captures")
    cells = {c.name: c for c in build_cells()}
    out = {"run": a.run, "cells": {}}
    sheets = []
    for name, c in cells.items():
        p = os.path.join(cap, f"mx_{name}")
        st = p + "_still.png"
        if not os.path.exists(st):
            continue
        s = out["cells"].setdefault(name, {"row": c.row, "context": c.context})
        ls = lum(st)
        s["still_mean"], s["still_grain"] = float(ls.mean()), hp_std(ls)
        for tag in ("n", "l"):
            t = temporal(sorted(glob.glob(p + f"_{tag}_[0-9]*.png")))
            if t:
                s["noise" if tag == "n" else "live"] = t
        for tag, _ in c.extra_stills:
            t = temporal(sorted(glob.glob(p + f"_{tag}_n_[0-9]*.png")))
            if t:
                s[f"noise_{tag}"] = t
        for tag, _, _ in c.seq:
            fs = sorted(glob.glob(p + f"_s{tag}_*.png"))
            if len(fs) > 2:
                L = [lum(f) for f in fs]
                h, w = L[0].shape
                ctr = (slice(h // 4, 3 * h // 4), slice(w // 4, 3 * w // 4))
                steps = [np.abs(L[i + 1] - L[i]) for i in range(len(L) - 1)]
                s[f"seq_{tag}"] = {"step_mean": float(np.mean([d.mean() for d in steps])),
                                   "step_gt16": float(np.mean([(d > 16).mean() for d in steps])),
                                   "centre_step_mean": float(np.mean([d[ctr].mean() for d in steps]))}
        mo = {}
        for ref in sorted(glob.glob(p + "-*_ref_*.png")):
            base = os.path.basename(ref)[:-4]
            rec, idx = base.split("_ref_")
            mov = os.path.join(cap, f"{rec}_{idx}.png")
            if not os.path.exists(mov):
                continue
            mm = motion_metrics(mov, ref)
            mm["frame"] = int(idx)
            mo.setdefault(rec.split("-", 1)[1], []).append(mm)
            sheets.append((mm["mean"], f"{name} {rec.split('-', 1)[1]} {idx}", mov, ref))
        if mo:
            s["motion"] = {}
            for k, v in mo.items():
                agg = {q: float(np.mean([x[q] for x in v])) for q in ("mean", "gt16", "p95", "raw_mean", "raw_gt16", "signed")}
                agg["bands"] = [float(np.mean([x["bands"][i] for x in v])) for i in range(8)]
                agg["frames"] = v
                s["motion"][k] = agg
    res = os.path.join(a.run, "results.jsonl")
    if os.path.exists(res):
        for line in open(res, encoding="utf-8"):
            try:
                r = json.loads(line)
            except ValueError:
                continue
            cmd, rr = r.get("cmd", ""), (r.get("result") or {})
            if cmd.startswith("perf") and "name=mx_" in cmd:
                nm = cmd.split("name=mx_")[1].split()[0]
                base, _, tag = nm.rpartition("_s")
                if nm not in out["cells"] and base in out["cells"] and tag:
                    tot = rr.get("gpu_total_ms")
                    out["cells"][base].setdefault("path_gpu", {})["seq" + tag] = {
                        "mean": tot.get("mean") if isinstance(tot, dict) else tot,
                        "max": tot.get("max") if isinstance(tot, dict) else None,
                        "cloud_march": (rr.get("gpu_ms") or {}).get("cloud_march")}
                if nm in out["cells"]:
                    tot = rr.get("gpu_total_ms")
                    g = rr.get("gpu_ms") or {}
                    out["cells"][nm]["gpu_ms"] = tot.get("mean") if isinstance(tot, dict) else tot
                    out["cells"][nm]["cloud_march_ms"] = g.get("cloud_march")
                    out["cells"][nm]["sky_ms"] = g.get("sky_background_draw")
            if cmd.startswith("path play") and "record=mx_" in cmd:
                rec = cmd.split("record=mx_")[1].split()[0]
                nm, _, m = rec.partition("-")
                if m and nm in out["cells"]:
                    gm = rr.get("gpu_ms_mean") or {}
                    out["cells"][nm].setdefault("path_gpu", {})[m] = {
                        "mean": gm.get("total"), "cloud_march": gm.get("cloud_march"),
                        "max": rr.get("gpu_total_ms_max"), "p90": rr.get("gpu_total_ms_p90")}
    # every failed command in an mx cell (a cell whose `beams go` found no site, a missing satellite, ...)
    if os.path.exists(res):
        errors = []
        for line in open(res, encoding="utf-8"):
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if not r.get("ok", True):
                errors.append({"src": r.get("src"), "cmd": r.get("cmd"), "error": r.get("error")})
        out["errors"] = errors
    json.dump(out, open(os.path.join(a.run, "matrix_suite.json"), "w"), indent=1)
    if a.sheet:
        sheets.sort(key=lambda x: -x[0])
        sp = os.path.join(a.run, "matrix_worst.png")
        write_sheet(sp, [(lab, m, r) for _, lab, m, r in sheets[:a.sheet]])
        print("worst motion frames:", sp)
    print_table(out)


def print_table(out):
    cols = ["walk", "strafe", "fast", "look", "rise", "descend"]
    print(f"{'cell':12s} {'row':>6s} {'gpu':>5s} {'cm':>5s} {'noise':>6s} {'n>8%':>5s} {'live':>5s}  "
          + " ".join(f"{c:>15s}" for c in cols) + "   gpu moving max / extra")
    for n, s in out["cells"].items():
        no, li = s.get("noise"), s.get("live")
        mo = s.get("motion", {})
        cells = []
        for c in cols:
            v = mo.get(c)
            cells.append(f"{v['mean']:4.2f}/{v['gt16'] * 100:4.1f}/{v['signed']:+4.1f}" if v else f"{'-':>15s}")
        pg = s.get("path_gpu", {})
        mx = max([v["max"] or 0 for v in pg.values()], default=0)
        extra = " ".join(f"{k}={v['step_mean']:.2f}" for k, v in s.items() if k.startswith("seq_"))
        extra += " ".join(f" {k}={v['std']:.2f}" for k, v in s.items() if k.startswith("noise_"))
        print(f"{n:12s} {s['row']:>6s} {s.get('gpu_ms') or 0:5.1f} {s.get('cloud_march_ms') or 0:5.1f} "
              f"{(no or {}).get('std', 0):6.2f} {(no or {}).get('gt8', 0) * 100:5.2f} {(li or {}).get('std', 0):5.2f}  "
              + " ".join(cells) + f"   {mx:5.1f} {extra}")
    print("motion cells: mean |diff| vs the settled reference (0-255, point sources median-filtered out) / % pixels "
          "> 16 / signed mean over content (<0 content lost in motion, >0 blur/bloat/lag); noise = per-pixel temporal "
          "std with time paused, live = at 1x")
    if out.get("errors"):
        print(f"{len(out['errors'])} failed commands (see matrix_suite.json 'errors'):")
        for e in out["errors"][:12]:
            print("  ", e.get("src"), e.get("cmd"), "->", e.get("error"))


def cmd_compare(a):
    def load(r):
        p = os.path.join(r, "matrix_suite.json")
        if not os.path.exists(p):
            sys.exit(f"{p} missing: run `score {r}` first")
        return json.load(open(p))["cells"]
    A, B = load(a.a), load(a.b)
    flags = []

    def chk(cell, what, va, vb, rel, absd):
        if va is None or vb is None:
            return
        bad = vb > va * (1.0 + rel) + absd
        good = vb < va * (1.0 - rel) - absd
        if bad or good:
            flags.append((cell, what, va, vb, "REGRESSION" if bad else "better"))

    for n in A:
        if n not in B:
            continue
        x, y = A[n], B[n]
        chk(n, "gpu_ms", x.get("gpu_ms"), y.get("gpu_ms"), 0.10, 1.0)
        chk(n, "cloud_march_ms", x.get("cloud_march_ms"), y.get("cloud_march_ms"), 0.10, 0.5)
        chk(n, "still_grain", x.get("still_grain"), y.get("still_grain"), 0.15, 0.3)
        for k in [k for k in x if k.startswith("noise") or k == "live"]:
            if k in y:
                chk(n, f"{k}.std", x[k]["std"], y[k]["std"], 0.20, 0.3)
                chk(n, f"{k}.gt8", x[k]["gt8"] * 100, y[k]["gt8"] * 100, 0.25, 0.3)
        for k in [k for k in x if k.startswith("seq_")]:
            if k in y:
                chk(n, f"{k}.step", x[k]["step_mean"], y[k]["step_mean"], 0.20, 0.5)
        for m, v in x.get("motion", {}).items():
            w = y.get("motion", {}).get(m)
            if w:
                chk(n, f"{m}.mean", v["mean"], w["mean"], 0.20, 0.4)
                chk(n, f"{m}.gt16%", v["gt16"] * 100, w["gt16"] * 100, 0.25, 0.5)
                if "signed" in v and "signed" in w:
                    chk(n, f"{m}.|signed|", abs(v["signed"]), abs(w["signed"]), 0.3, 0.5)
        for m, v in x.get("path_gpu", {}).items():
            w = y.get("path_gpu", {}).get(m)
            if w and v.get("max") and w.get("max"):
                chk(n, f"{m}.gpu_max", v["max"], w["max"], 0.20, 3.0)
    for cell in sorted(set(A) ^ set(B)):
        print(f"{cell}: only in {'A' if cell in A else 'B'}")
    nreg = 0
    for cell, what, va, vb, kind in flags:
        nreg += kind == "REGRESSION"
        if kind == "REGRESSION" or a.all:
            print(f"{kind:10s} {cell:12s} {what:22s} {va:8.3f} -> {vb:8.3f}")
    print(f"{nreg} regressions, {len(flags) - nreg} improvements (--all lists them)")
    sys.exit(1 if nreg else 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    g = sp.add_parser("gen")
    g.add_argument("out")
    g.add_argument("--quick", action="store_true", help="4 motions per altitude, 1 reference each, no live window")
    g.add_argument("--only", help="altitude=0m+1km,context=beams+aurora,cell=alt3km (OR of the parts)")
    g.add_argument("--fragment", action="store_true", help="commands only (no header, no quit), for a larger batch")
    g.add_argument("--restore", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                                                     "user_data", "settings.json"),
                   help="settings.json whose values the changed settings are set back to")
    s = sp.add_parser("score")
    s.add_argument("run")
    s.add_argument("--sheet", type=int, default=0, metavar="N", help="write the N worst motion frames as <run>/matrix_worst.png")
    c = sp.add_parser("compare")
    c.add_argument("a")
    c.add_argument("b")
    c.add_argument("--all", action="store_true")
    a = ap.parse_args()
    {"gen": cmd_gen, "score": cmd_score, "compare": cmd_compare}[a.cmd](a)


if __name__ == "__main__":
    main()
