#!/usr/bin/env python3
"""The clouds-and-sky performance matrix (2026-11-09): matrix_suite.py's generator and scorer over the cells of the
cloud / sky perf pass. Stdlib only for `gen`; `score` / `compare` / `jumpscore` need numpy + Pillow (the harness venv).

    python tools/harness/cloudsky_matrix.py gen harness_runs/cloudsky/mx/mx.satcmd [--issue] [--jumps] [--quick] [--only cell=brk_300km,context=storm]
    python tools/harness/cloudsky_matrix.py gen harness_runs/cloudsky/mx/core.satcmd --core --quick --issue   # ~6 min: after each kept change
    python tools/harness/run.py harness_runs/cloudsky/mx/mx.satcmd --window 1920x1009 --out harness_runs/cloudsky/mx/base
    python tools/harness/cloudsky_matrix.py score harness_runs/cloudsky/mx/base
    python tools/harness/cloudsky_matrix.py compare harness_runs/cloudsky/mx/base harness_runs/cloudsky/mx/after
    python tools/harness/cloudsky_matrix.py jumpscore harness_runs/cloudsky/mx/base
    python tools/harness/res.py harness_runs/cloudsky/mx/base --perf -b cloud_march,sky_background_draw

Axes (the goal: High stays under 25 ms across all of them):
  ALTITUDE  2 m, 1.5 km (in the low layer), 6.5 km (between layers), 11 km (airliner), 300 km, 850 km
  WEATHER   clear (Sahara), broken cumulus (Indian Ocean), dense cumulus (Pacific 7 N), overcast deck (Antarctic
            plateau), storms (two storm objects alive at the suite's moment, each seen from 40 km off)
  LIGHTING  day (the weather sites), the terminator (Siberia, Sun 0.4 deg up; and the dense site at a 3-deg setting
            Sun), night (the dense site and Shanghai), Reflect beams (matrix_suite's two beam cells)
  MOTION    still + perf, 16 paused frames (noise), 16 frames at 1x (live), walk / fast / look / rise paths scored
            against settled references (matrix_suite), time warp at 1 min/s (perf + frames), and JUMPS between cells
            (frames after the jump against the settled view; `jumpscore`)
plus the five shots of issue_20261109_184851 as loaded (the reporter's settings), paused and running (`--issue`).

SETTINGS: run it on the USER'S settings.json (run.py --settings <main checkout>/user_data/settings.json), never on the
built-in defaults. The compiled-in defaults are an older cloud look (2026-11-09: 98 clouds_v2 keys differ by more than
2% -- coverage 0.96 vs 0.39, tower_top 0.85 vs 0.43, cb_cumulus_top_km 12 vs 7, anvil 8 vs 0.35): towers amplified and
cut off at the shell's top, another cost profile, and not the look the user judges. The user's file carries High's tier
values (430 iterations, 7 light steps, full-res clouds, render scale 1): it IS High with the current look. The drift
rate is the user's too.

Exposure: the auto exposure is ON and metered once per cell (`exposure fast` .. `exposure hold`), not switched off.

Names: mx_<site>_<altitude> (perf name and capture prefix), jump_<k>_*, is_s<N>_P / _R.
"""
import glob
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import matrix_suite as ms  # noqa: E402

T0 = ms.TIME
# (agl m, el): the horizon from the ground, level inside / between the layers, down from above, the limb from orbit.
ALTS = [(2, 10.0), (1500, 0.0), (6500, -3.0), (11000, -8.0), (300000, -25.0), (850000, -55.0)]
# site: (context, time, lat, lon, az, post_setup)
SITES = [
    ("clear", "clear", "2036-11-22T09:23:34Z", 23.0, 10.0, 120.0, []),
    ("brk", "broken", T0, -30.0, 80.0, 210.0, []),      # (the Sun is low in the east there: 120 looks into its glint)
    ("dense", "dense", T0, 7.0, -170.0, 120.0, []),
    ("ovc", "overcast", T0, -75.0, 90.0, 120.0, []),
    # Storm objects alive at T0 on the user's settings (harness `weather`): a multicell at 2.5 S 135.5 E seen from 40 km
    # east of it with the Sun behind the eye, another at 12.4 N 159.7 W seen from 40 km south (side-lit).
    ("stA", "storm", T0, -2.5, 135.86, 270.0, []),
    ("stB", "storm", T0, 12.04, -159.7, 0.0, []),
    ("term", "terminator", T0, 62.0, 100.0, 30.0, []),      # (side-lit: long shadows; `sunset` looks into the Sun)
    ("sunset", "terminator", T0, 7.0, -170.0, 270.0, ["time sun 3 setting", "time pause"]),
    ("night", "night", "2036-11-22T10:00:00Z", 7.0, -170.0, 120.0, []),
]
MOTION_ALTS = (2, 6500, 300000)
ISSUE = "C:/Code/Shader_Fun/user_data/reports/issue_20261109_184851"
# Jumps: stand settled in the first cell, jump to the second.
JUMPS = [("brk_300km", "stA_2m"), ("stA_2m", "dense_6km"), ("dense_6km", "ovc_850km"), ("ovc_850km", "brk_2m")]
# The quick subset for a kept change (`--core`): the heavy cells of each altitude and lighting.
CORE = ["dense_2m", "stA_2m", "ovc_1km", "stB_6km", "term_6km", "ovc_11km", "dense_300km", "term_300km", "sunset_300km",
        "ovc_850km", "night_6km"]


def alt_label(a):
    return f"{a // 1000}km" if a >= 1000 else f"{a}m"


def build_cells():
    C = []
    for site, ctx, t, lat, lon, az, post in SITES:
        for agl, el in ALTS:
            mot = []
            if agl in MOTION_ALTS:
                mot = ["look"] if site == "clear" else ["fast", "look"]
                if agl == 2 and site in ("brk", "dense", "stA"):
                    mot = ["walk"] + mot
            elif site in ("brk", "stA"):
                mot = ["fast"]
            if site == "brk" and agl in (2, 11000):
                mot = mot + ["rise"]
            seq = [("1m", "1m", 8)] if (site in ("brk", "dense", "stA") and agl in (6500, 300000)) else []
            C.append(ms.Cell(f"{site}_{alt_label(agl)}", alt_label(agl), ctx, t, lat, lon, None, agl=agl, az=az, el=el,
                             fov=60.0, ground=0.0, motions=mot, quick_motions=(["fast"] if "fast" in mot else mot[:1]),
                             post_setup=list(post), seq=seq, orbit=agl >= 100000))
    keep = {"night_3km", "beams_site", "beams_up"}
    C += [c for c in _orig_build() if c.name in keep]
    return C


_orig_build = ms.build_cells
ms.build_cells = build_cells

# EXPOSURE: the auto exposure stays ON (matrix_suite switches it off for determinism, which blows out every daytime
# frame: nothing to score in a clipped cloud). Each cell is metered once, deterministically, where it stands -- the
# harness's `exposure fast` settles the metered offset by a fixed share per frame -- and held for the cell's stills,
# paths and references. (The first 100 frames let the clouds and the Sun volume refill after the move.)
ms.DRIFT_RATE = "3.710407327162102e-06"   # the user's clouds.drift_rate (matrix_suite's 6.5e-6 is the compiled default)
ms.SETS = [("display.dynamic_resolution", "false"), ("display.fps_cap_mode", "0"), ("clouds_v2.auto_exposure", "1"),
           ("clouds.drift_rate", ms.DRIFT_RATE)]
METER = ["wait 100", "exposure fast", "wait 45", "exposure hold"]
_orig_setup = ms.setup
_metered = [None]


def setup(c):
    L = _orig_setup(c)
    if _metered[0] != c.name:
        _metered[0] = c.name
        L = L + METER
    return L


ms.setup = setup


def jump_lines():
    cells = {c.name: c for c in build_cells()}
    L = []
    for k, (a, b) in enumerate(JUMPS):
        ca, cb = cells[a], cells[b]
        for part in ("cap", "perf"):
            # (metered at the destination first: the frames after the jump and their reference share one exposure)
            L += ([f"# ===== jump {k} {a} -> {b} ({part})"] + _orig_setup(cb) + METER + _orig_setup(ca) + ms.settle(ca)
                  + _orig_setup(cb))
            if part == "cap":
                L += [f"capture jump_{k}_f00 scale={ms.SCALE}", "wait 12", f"capture jump_{k}_f20 scale={ms.SCALE}", "wait 36",
                      f"capture jump_{k}_f60 scale={ms.SCALE}"] + ms.settle(cb, 160) + [f"capture jump_{k}_ref scale={ms.SCALE}"]
            else:
                L += [f"perf frames=30 name=jump_{k}_first30", f"perf frames=60 name=jump_{k}_next60"]
    return L


def issue_lines():
    L = []
    for n in range(1, 6):
        L += [f"# ===== issue shot {n}", "exposure auto", "follow off", "select none", "path clear",
              f'report load "{ISSUE}" shot={n} trail=off', "set display.fps_cap_mode 0",
              "set display.dynamic_resolution false", "time pause", "wait 260",
              f"capture is_s{n}_still scale={ms.SCALE}", f"perf frames=60 name=is_s{n}_P",
              "time play", "wait 200", f"perf frames=60 name=is_s{n}_R", "time pause",
              # the reporter's last seconds of camera motion, replayed (the cost while moving)
              f'report load "{ISSUE}" shot={n}', "set display.fps_cap_mode 0", "set display.dynamic_resolution false",
              "path goto 0", "wait settle 60", "path play fps=60"]
    L += ["# ===== end of the issue shots", "path clear", "time scale 1x", "time pause"]
    return L


def cmd_jumpscore(run):
    cap = os.path.join(run, "captures")
    out = {}
    for k, (a, b) in enumerate(JUMPS):
        ref = os.path.join(cap, f"jump_{k}_ref.png")
        if not os.path.exists(ref):
            continue
        lr = ms.lum(ref)
        row = {}
        for f in ("f00", "f20", "f60"):
            p = os.path.join(cap, f"jump_{k}_{f}.png")
            if os.path.exists(p):
                d = abs(ms.lum(p) - lr)
                row[f] = {"mean": round(float(d.mean()), 3), "pct_gt16": round(float((d > 16).mean() * 100), 3)}
        out[f"{a}->{b}"] = row
    json.dump(out, open(os.path.join(run, "jumpscore.json"), "w"), indent=1)
    for k, v in out.items():
        print(k, " ".join(f"{f}: {r['mean']:.2f} / {r['pct_gt16']:.2f}%" for f, r in v.items()))


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "jumpscore":
        return cmd_jumpscore(sys.argv[2])
    issue = "--issue" in sys.argv
    jumps = "--jumps" in sys.argv
    if "--core" in sys.argv:
        sys.argv += ["--only", "cell=" + "+".join(CORE)]
    sys.argv = [a for a in sys.argv if a not in ("--issue", "--jumps", "--core")]
    ms.main()
    if len(sys.argv) > 2 and sys.argv[1] == "gen" and (issue or jumps):
        path = sys.argv[2]
        L = open(path, encoding="utf-8").read().split("\n")
        i = next(k for k, l in enumerate(L) if l.startswith("# ===== restore"))
        extra = (jump_lines() if jumps else []) + (issue_lines() if issue else [])
        open(path, "w", encoding="utf-8").write("\n".join(L[:i] + extra + ["exposure auto"] + L[i:]))
        print(json.dumps({"added_lines": len(extra), "jumps": jumps, "issue": issue}))


if __name__ == "__main__":
    main()
