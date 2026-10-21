#!/usr/bin/env python3
"""Performance benchmark of the launch / mission system (docs/HARNESS.md "Launch benchmark"): CPU and GPU at fixed
scenes, each with launches ON and OFF (the system's whole cost is the difference), and the time warps.

    python tools/harness/launch_bench.py run [--out harness_runs/launch_bench/<stamp>.json] [--frames 120]
    python tools/harness/launch_bench.py compare <baseline.json> <new.json>     # per scene: before -> after (delta)

Scenes (all at 1600x900 unless --window):
  bookmark_2036      the default "Twilight Starship Launch" bookmark (2036-11-22, Louisiana, T+290 s), 1x
  bookmark_2036_1h   the same at 1 h/s     bookmark_2036_1d   at 1 d/s   (fixed 1/60 s steps: sim time per frame)
  era_2032_busy      2032-08-27 (the busiest Starship day of the deployment era), the Gulf coast at dusk, 1x
  night_close        the next night Starship launch, T+60 s, following the stack from 300 m (flames, plumes, lights)
  droneship          the next booster landing on a ship, 30 s before touchdown, 1.5 km from the ship
Per scene: `perf` (GPU buckets: plumes / flames / engine lights land in cloud_march and the draw buckets, meshes in
scene_depth; CPU buckets incl. `launches`), `launch state` (cpu_split_ms, counts), then the same with
`launches.enabled false`. Writes one JSON (commit, window, scenes). Store a baseline under
harness_runs/launch_bench/ and compare later runs against it.
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
from mission_ops import Live  # noqa: E402

SCENES = [
    ("bookmark_2036", ["bookmark go {bm}", "time scale 1x", "wait 120"]),
    ("bookmark_2036_1h", ["bookmark go {bm}", "time scale 1h", "wait 120"]),
    ("bookmark_2036_1d", ["bookmark go {bm}", "time scale 1d", "wait 120"]),
    ("era_2032_busy", ["time set 2032-08-27T23:40:00Z", "observer lat=29.70 lon=-92.40 agl=2", "camera az=170 el=25 fov=70",
                       "time play", "time scale 1x", "wait 200"]),
    ("night_close", ["time set 2036-11-22T02:06:22Z", "wait 30", "launch next when=night vehicle=starship lead=0",
                     "time add 60", "wait 30", "launch select kind=rocket", "follow offset=-300,200,60 aim=sat", "wait 90"]),
    ("droneship", ["time set 2036-11-22T02:06:22Z", "wait 30", "launch droneship lead=30", "wait 90"]),
]

KEYS = ["gpu_total_mean", "gpu_cloud_march", "gpu_scene_depth", "gpu_sky_draw", "gpu_sat_draw", "cpu_launches",
        "cpu_kessler", "wall_p50"]


def summarize(perf, lst):
    g = perf.get("gpu_ms", {})
    c = perf.get("cpu_ms", {})
    s = {"gpu_total_mean": perf.get("gpu_total_ms", {}).get("mean"), "gpu_total_p90": perf.get("gpu_total_ms", {}).get("p90"),
         "gpu_cloud_march": g.get("cloud_march"), "gpu_scene_depth": g.get("scene_depth"),
         "gpu_sky_draw": g.get("sky_background_draw"), "gpu_sat_draw": g.get("satellite_star_draw"),
         "cpu_launches": c.get("launches"), "cpu_kessler": c.get("kessler"), "cpu_build_ui": c.get("build_ui"),
         "wall_p50": perf.get("wall_frame_ms", {}).get("p50")}
    if lst:
        s["launch_cpu_split_ms"] = lst.get("cpu_split_ms")
        for k, src in (("batches", "batches_now"), ("rockets", "rockets_drawn"), ("plume_gaussians", "plume_gaussians"),
                       ("train", "train_satellites"), ("retiring", "retiring_satellites"), ("engine_lights", "engine_lights"),
                       ("stack_meshes", "stack_meshes"), ("ship_meshes", "ship_meshes"), ("records_written", "records_written")):
            if src in lst:
                s["n_" + k] = lst[src] if not isinstance(lst[src], list) else len(lst[src])
    return s


def git_commit():
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=REPO, stdout=subprocess.PIPE, text=True).stdout.strip()
    except OSError:
        return "?"


def cmd_run(a):
    stamp = time.strftime("%Y%m%d_%H%M%S")
    out = os.path.abspath(a.out or os.path.join(REPO, "harness_runs", "launch_bench", f"{stamp}_{git_commit()}.json"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    live = Live(os.path.join(os.path.dirname(out), "live_" + stamp), window=a.window)
    live.start()
    res = {"commit": git_commit(), "when": stamp, "window": a.window, "frames": a.frames, "scenes": {}}
    try:
        r = live.send("bookmark list")
        bms = (r[-1].get("result") or {}).get("bookmarks", [])
        bm = next((i + 1 for i, b in enumerate(bms) if "Twilight Starship" in (b.get("name") or "")), 1)
        for name, cmds in SCENES:
            if a.scene and a.scene not in name:
                continue
            entry = {}
            # The scene with launches on, then the SAME view with them off (a followed object is left in place first:
            # `follow stay`; the clock keeps its rate): the difference is the system's cost.
            for on in (True, False):
                if on:
                    setup = ["set launches.enabled true", "follow off"] + [c.format(bm=bm) for c in cmds]
                else:
                    setup = ["follow stay", "set launches.enabled false", "wait 60"]
                r = live.send("\n".join(setup + [f"perf frames={a.frames} name={name}{'' if on else '_off'}", "launch state"]),
                              timeout=900)
                errs = [x for x in r if not x.get("ok") and not x.get("cmd", "").startswith("follow stay")]
                perf = next((x["result"] for x in r if x.get("cmd", "").startswith("perf") and x.get("ok")), {})
                lst = (r[-1].get("result") or {}).get("launches") if r and r[-1].get("ok") else None
                entry["on" if on else "off"] = summarize(perf, lst)
                if errs:
                    entry.setdefault("errors", []).extend(f"{x.get('cmd')}: {x.get('error')}" for x in errs)
            on, off = entry["on"], entry["off"]
            entry["delta"] = {k: (on.get(k) or 0) - (off.get(k) or 0) for k in KEYS if on.get(k) is not None}
            res["scenes"][name] = entry
            d = entry["delta"]
            print(f"  {name:18} gpu {on.get('gpu_total_mean', 0):6.2f} ms (launches +{d.get('gpu_total_mean', 0):5.2f})  "
                  f"cpu launches {on.get('cpu_launches', 0):5.2f} ms  batches {on.get('n_batches')}")
        r = live.send("set launches.enabled true")
        # The launch bookmarks still frame their launch (a change to the launch model moves the stages): captures.
        for i, b in enumerate(bms):
            if any(s in (b.get("name") or "") for s in ("Starship", "Starmind Deployment")):
                tag = "bm_" + "".join(ch for ch in b["name"] if ch.isalnum())
                r = live.send(f"follow off\nbookmark go {i + 1}\nwait 60\ncapture {tag} scale=0.5\nlaunch state", timeout=600)
                st = (r[-1].get("result") or {}).get("launches", {}) if r and r[-1].get("ok") else {}
                res.setdefault("bookmarks", {})[b["name"]] = {"rockets_drawn": st.get("rockets_drawn"),
                                                             "stack_meshes": st.get("stack_meshes"),
                                                             "capture": os.path.join(live.run_dir or "", "captures", tag + ".png")}
    finally:
        live.stop()
    json.dump(res, open(out, "w", encoding="utf-8"), indent=1)
    print(out)


def cmd_compare(a):
    A = json.load(open(a.base, encoding="utf-8"))
    B = json.load(open(a.new, encoding="utf-8"))
    print(f"baseline {A.get('commit')} ({A.get('when')})  ->  {B.get('commit')} ({B.get('when')})")
    for name in B["scenes"]:
        if name not in A["scenes"]:
            continue
        print(f"\n{name}")
        for part in ("on", "delta"):
            for k in KEYS:
                x, y = A["scenes"][name].get(part, {}).get(k), B["scenes"][name].get(part, {}).get(k)
                if x is None or y is None:
                    continue
                flag = "  <-- slower" if y - x > max(0.25, 0.1 * abs(x)) else ""
                print(f"  {part:5} {k:16} {x:8.3f} -> {y:8.3f}  ({y - x:+.3f}){flag}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("run")
    s.add_argument("--out")
    s.add_argument("--frames", type=int, default=120)
    s.add_argument("--window", default="1600x900")
    s.add_argument("--scene", default="")
    s = sp.add_parser("compare")
    s.add_argument("base")
    s.add_argument("new")
    a = ap.parse_args()
    {"run": cmd_run, "compare": cmd_compare}[a.cmd](a)


if __name__ == "__main__":
    main()
