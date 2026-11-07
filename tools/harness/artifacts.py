#!/usr/bin/env python3
"""Temporal ARTIFACT gate: flicker, blockiness and stair-stepping as the user sees them (2026-11-07).

    tools/harness/.venv/Scripts/python tools/harness/artifacts.py run [--scenarios tower,anvil,...] [--scales 1x,1m,5m]
                                                                       [--frames 16] [--out DIR] [--keep] [--quick]
    tools/harness/.venv/Scripts/python tools/harness/artifacts.py score DIR
    tools/harness/.venv/Scripts/python tools/harness/artifacts.py compare DIR_A DIR_B

Why it exists: the user watches storms at 1 min/s at ~30 fps. Every earlier check ran at the harness's fixed 1/60 s step,
at 1x or settled, and judged motion from lossy videos, so the time-lapse's flicker (15x the 1x view's), the sparse grid's
blocks and the stair-stepped billow edges all passed (issue_20261107_124812 and its siblings). This tool:

- runs ONE live app at a 1/30 s fixed step (`live.py start --fixed-dt`), on user_data/settings.json unless a scenario's
  report brings its own;
- per scenario (a report's view, `SCENARIOS` below) and time scale: settles paused, plays at the scale, then captures
  `--frames` CONSECUTIVE frames as full-resolution PNGs (a crop: no codec, no downscale -- both hide 2-px blocks);
- scores each run (`numbers.json`, `review.md`, a heat map and a worst-tile crop per run):
    rev12   share of pixel-frames whose change REVERSES sign with both steps > 12 levels (smooth motion is monotonic:
            this is flicker / sparkle, not movement)
    rev6    the same over 6 levels
    blkT2 / blkT4   block structure of the frame-to-frame CHANGE on a 2- / 4-px grid: mean |gradient of |dI|| across
            grid lines over its mean off them (1.0 = none; the sparse march's 2x2 and smooth tiles' 4x4 show > 1.1)
    blkS2   the same on the image itself (stair-stepped edges)
    blkT2_lv / blkS2_lv   the same as an EXCESS in levels (on-grid minus off-grid |gradient|): a ratio rises when the
            rest of the change gets quieter, this does not
    lag / lag16   mean |last running frame - the same moment settled| (levels) and the share off by > 16: the history's
            DRAG -- ghosts and smears. A fix that trades flicker for drag shows here (2026-11-07: a stickier history
            took the 5 min/s tower's flicker 20% -> 1.5% and drew it as a smeared ghost). It also holds the weather
            cube's own lag in time-lapse (faces re-bake up to ~35 sim s behind while time runs and catch up once paused:
            a cumulus present while running, gone in the reference): compare lag between builds, never alone.
    d1      mean |frame-to-frame change| (context: motion and evolution, not an error)
- `compare` prints both runs side by side with the ratio, for a fix's before / after.

A run at 1x is the floor: a scenario's 1m / 5m numbers far above it are the time-lapse's own artifacts.
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import time

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
LIVE = os.path.join(REPO, "harness_live")
sys.path.insert(0, HERE)

REP = "C:/Code/Shader_Fun/user_data/reports/"
# name: (setup commands, crop x,y,w,h at 1920x1009). The setups pause time; the run plays from there.
SCENARIOS = {
    # A tall air-mass tower from 4.3 km, 14 km away, midday (issue_20261107_124812: blocks, the flat "wall").
    "tower":  ([f'report load "{REP}issue_20261107_124812" shot=1 trail=off', "time pause",
                "time set 2036-11-22T08:02:30Z"], (300, 0, 960, 640)),
    # A tower under its anvil with cirrus overhead (issue_20261107_124930: grainy anvil and cirrus, smeared tower).
    "anvil":  ([f'report load "{REP}issue_20261107_124930" shot=1 trail=off', "time pause"], (480, 0, 960, 640)),
    # A sunset anvil from the sea (issue_20261107_125130: sparkling grain all over it).
    "sunset": ([f'report load "{REP}issue_20261107_125130" shot=1 trail=off', "time pause"], (480, 250, 960, 560)),
    # The ITCZ's tower and anvil from 6.7 km (issue_20261106_184711).
    "itcz":   ([f'report load "{REP}issue_20261106_184711" shot=1 trail=off', "time pause"], (480, 150, 960, 640)),
    # The cumulus field from 650 km (issue_20261107_125245: "noise repetition").
    "orbit":  ([f'report load "{REP}issue_20261107_125245" shot=1 trail=off', "time pause"], (480, 200, 960, 600)),
}


def send(cmds, timeout=1800.0):
    name = f"{int(time.time() * 1000)}_{os.getpid()}"
    inbox = os.path.join(LIVE, "inbox")
    tmp = os.path.join(inbox, name + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("\n".join(cmds) + "\n")
    os.replace(tmp, os.path.join(inbox, name + ".satcmd"))
    out = os.path.join(LIVE, "outbox", name + ".json")
    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.exists(out):
            try:
                with open(out, encoding="utf-8") as f:
                    res = json.load(f)["results"]
            except (OSError, json.JSONDecodeError, KeyError):
                time.sleep(0.2)
                continue
            bad = [r for r in res if not r.get("ok")]
            if bad:
                raise RuntimeError(f"{bad[0].get('cmd')}: {bad[0].get('error')}")
            return res
        time.sleep(0.2)
    raise RuntimeError(f"no answer within {timeout} s")


def live_dir():
    with open(os.path.join(LIVE, "status.json"), encoding="utf-8") as f:
        return json.load(f)["run_dir"]


def lum(path):
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def grid_ratio(img, b):
    """Mean |gradient| across b-px grid lines over its mean off them, both axes (1.0 = no grid structure)."""
    gx = np.abs(np.diff(img, axis=1))
    gy = np.abs(np.diff(img, axis=0))
    ix = (np.arange(gx.shape[1]) % b) == (b - 1)
    iy = (np.arange(gy.shape[0]) % b) == (b - 1)
    on = gx[:, ix].mean() + gy[iy, :].mean()
    off = gx[:, ~ix].mean() + gy[~iy, :].mean()
    return float(on / max(off, 1e-6))


def grid_excess(img, b):
    """The same, ABSOLUTE: the mean |gradient| on b-px grid lines minus off them, in levels (a ratio rises when everything
    else gets quieter; this does not)."""
    gx = np.abs(np.diff(img, axis=1))
    gy = np.abs(np.diff(img, axis=0))
    ix = (np.arange(gx.shape[1]) % b) == (b - 1)
    iy = (np.arange(gy.shape[0]) % b) == (b - 1)
    return float(0.5 * ((gx[:, ix].mean() - gx[:, ~ix].mean()) + (gy[iy, :].mean() - gy[~iy, :].mean())))


def score_seq(paths):
    f = np.stack([lum(p) for p in paths])
    d = np.diff(f, axis=0)
    a, b = d[:-1], d[1:]
    rev12 = (a * b < 0) & (np.abs(a) > 12) & (np.abs(b) > 12)
    rev6 = (a * b < 0) & (np.abs(a) > 6) & (np.abs(b) > 6)
    ad = np.abs(d)
    return {
        "frames": len(paths),
        "rev12_pct": round(100.0 * float(rev12.mean()), 3),
        "rev6_pct": round(100.0 * float(rev6.mean()), 3),
        "blkT2": round(float(np.mean([grid_ratio(x, 2) for x in ad])), 3),
        "blkT4": round(float(np.mean([grid_ratio(x, 4) for x in ad])), 3),
        "blkS2": round(float(np.mean([grid_ratio(x, 2) for x in f])), 3),
        "blkT2_lv": round(float(np.mean([grid_excess(x, 2) for x in ad])), 4),
        "blkS2_lv": round(float(np.mean([grid_excess(x, 2) for x in f])), 4),
        "d1": round(float(ad.mean()), 2),
    }, rev6.sum(axis=0)


def heat_and_worst(paths, revmap, out_png, out_crop, tile=128):
    base = Image.open(paths[len(paths) // 2]).convert("RGB")
    h = np.clip(revmap * (255.0 / max(1.0, float(np.percentile(revmap, 99.9)))), 0, 255).astype(np.uint8)
    heat = Image.fromarray(np.stack([h, h // 4, h // 4], -1))
    sheet = Image.new("RGB", (base.width * 2 + 4, base.height), (20, 20, 20))
    sheet.paste(base, (0, 0))
    sheet.paste(heat, (base.width + 4, 0))
    sheet.save(out_png)
    # the tile with the most reversals, at 3x
    H, W = revmap.shape
    best, bx, by = -1, 0, 0
    for y in range(0, max(H - tile, 1), tile // 2):
        for x in range(0, max(W - tile, 1), tile // 2):
            s = float(revmap[y:y + tile, x:x + tile].sum())
            if s > best:
                best, bx, by = s, x, y
    crops = [Image.open(p).convert("RGB").crop((bx, by, bx + tile, by + tile)).resize((tile * 3, tile * 3), Image.NEAREST)
             for p in paths[len(paths) // 2 - 1: len(paths) // 2 + 2]]
    cs = Image.new("RGB", (len(crops) * (tile * 3 + 4), tile * 3), (20, 20, 20))
    for k, c in enumerate(crops):
        cs.paste(c, (k * (tile * 3 + 4), 0))
    ImageDraw.Draw(cs).text((4, 4), f"worst tile x={bx} y={by} (3 consecutive frames, 3x)", fill=(255, 255, 0))
    cs.save(out_crop)


def cmd_run(a):
    scen = [s for s in a.scenarios.split(",") if s]
    scales = [s for s in a.scales.split(",") if s]
    out = a.out or os.path.join(REPO, "harness_runs", "artifacts", time.strftime("%Y%m%d_%H%M%S"))
    os.makedirs(out, exist_ok=True)
    started = False
    st = {}
    try:
        st = json.load(open(os.path.join(LIVE, "status.json"), encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        pass
    if not st.get("alive"):
        sett = os.path.join(REPO, "user_data", "settings.json")
        args = [sys.executable, os.path.join(HERE, "live.py"), "start", "--window", "1920x1009", "--fixed-dt", str(a.dt)]
        if os.path.exists(sett):
            args += ["--settings", sett]
        subprocess.run(args, check=True)
        started = True
    else:
        print("note: using the live app already running; its fixed step is whatever it was started with "
              f"(this tool's runs assume --fixed-dt {a.dt})")
    meta = {"dt": a.dt, "frames": a.frames, "runs": {}}
    if os.path.exists(os.path.join(out, "meta.json")):   # (a second run into the same folder adds to it)
        meta["runs"] = json.load(open(os.path.join(out, "meta.json"), encoding="utf-8")).get("runs", {})
    try:
        for s in scen:
            setup, crop = SCENARIOS[s]
            cr = ",".join(str(v) for v in crop)
            for sc in scales:
                tag = f"{s}_{sc}"
                cmds = list(setup) + ["time scale 1x", "wait settle auto max=250", f"time scale {sc}", "time play",
                                      f"wait {a.warm}"]
                cmds += [f"capture af_{tag}_{k:02d} crop={cr}" for k in range(a.frames)]
                # LAG: the same moment settled (paused right after the last frame): how far the running image trails it.
                cmds += ["time pause", "time scale 1x", "wait settle auto max=250", f"capture af_{tag}_ref crop={cr}"]
                print(f"[{tag}]", flush=True)
                send(cmds, timeout=1800)
                rd = live_dir()
                dst = os.path.join(out, tag)
                os.makedirs(dst, exist_ok=True)
                for k in list(range(a.frames)) + ["ref"]:
                    src = os.path.join(rd, "captures", f"af_{tag}_{k:02d}.png" if k != "ref" else f"af_{tag}_ref.png")
                    t0 = time.time()
                    while not os.path.exists(src) and time.time() - t0 < 30.0:   # (the PNG encode trails the result)
                        time.sleep(0.2)
                    time.sleep(0.3)
                    shutil.copy(src, dst)
                meta["runs"][tag] = {"scenario": s, "scale": sc}
    finally:
        with open(os.path.join(out, "meta.json"), "w", encoding="utf-8") as f:
            json.dump(meta, f, indent=1)
        if started and not a.keep:
            subprocess.run([sys.executable, os.path.join(HERE, "live.py"), "stop"])
    cmd_score(argparse.Namespace(dir=out))


def cmd_score(a):
    meta = json.load(open(os.path.join(a.dir, "meta.json"), encoding="utf-8"))
    nums = {}
    lines = ["# Artifact gate", "", f"fixed step {meta['dt']} s, {meta['frames']} consecutive frames a run", "",
             "| run | rev12 % | rev6 % | blkT2 | blkT4 | blkS2 | blkT2 lv | blkS2 lv | lag | lag16 % | d1 |", "|---|---|---|---|---|---|---|---|---|---|---|"]
    for tag in meta["runs"]:
        paths = sorted(p for p in glob.glob(os.path.join(a.dir, tag, f"af_{tag}_*.png")) if not p.endswith("_ref.png"))
        ref = os.path.join(a.dir, tag, f"af_{tag}_ref.png")
        if len(paths) < 4:
            continue
        n, revmap = score_seq(paths)
        if os.path.exists(ref):
            dl = np.abs(lum(paths[-1]) - lum(ref))
            n["lag"] = round(float(dl.mean()), 2)
            n["lag16_pct"] = round(100.0 * float((dl > 16).mean()), 2)
        nums[tag] = n
        heat_and_worst(paths, revmap, os.path.join(a.dir, f"{tag}_heat.png"), os.path.join(a.dir, f"{tag}_worst.png"))
        lines.append(f"| {tag} | {n['rev12_pct']} | {n['rev6_pct']} | {n['blkT2']} | {n['blkT4']} | {n['blkS2']} | {n['blkT2_lv']} | {n['blkS2_lv']} | {n.get('lag', '-')} | {n.get('lag16_pct', '-')} | {n['d1']} |")
    with open(os.path.join(a.dir, "numbers.json"), "w", encoding="utf-8") as f:
        json.dump(nums, f, indent=1)
    lines += ["", "Heat maps: `<run>_heat.png` (a frame | its rev6 count per pixel). Worst tile: `<run>_worst.png`."]
    with open(os.path.join(a.dir, "review.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines[4:4 + 2 + len(nums)]))


def cmd_compare(a):
    A = json.load(open(os.path.join(a.a, "numbers.json"), encoding="utf-8"))
    B = json.load(open(os.path.join(a.b, "numbers.json"), encoding="utf-8"))
    keys = ["rev12_pct", "blkT2_lv", "blkS2_lv", "lag", "lag16_pct", "d1"]
    print("run".ljust(16) + "".join(k.rjust(22) for k in keys))
    for tag in A:
        if tag not in B:
            continue
        row = tag.ljust(16)
        for k in keys:
            va, vb = A[tag].get(k, "-"), B[tag].get(k, "-")
            row += f"{va:>8} -> {vb:<8}".rjust(22)
        print(row)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--scenarios", default=",".join(SCENARIOS))
    r.add_argument("--scales", default="1x,1m,5m")
    r.add_argument("--frames", type=int, default=16)
    r.add_argument("--warm", type=int, default=30, help="frames played before the captures")
    r.add_argument("--dt", type=float, default=1.0 / 30.0)
    r.add_argument("--out")
    r.add_argument("--keep", action="store_true", help="leave the live app running")
    s = sub.add_parser("score")
    s.add_argument("dir")
    c = sub.add_parser("compare")
    c.add_argument("a")
    c.add_argument("b")
    a = ap.parse_args()
    {"run": cmd_run, "score": cmd_score, "compare": cmd_compare}[a.cmd](a)


if __name__ == "__main__":
    main()
