#!/usr/bin/env python3
"""Storm review: each weather object from every angle, through its life and in motion (2026-11-06).

Built after a QA round where storms were judged from a few settled stills: square columns, cliffs, rings, hurricane
arms that teleport at 1 h/s and a flicker over a quarter of the sky all passed. This rig makes the same views every time:

    python tools/harness/storm_review.py run [--objects cumulus,supercell,line,hurricane] [--out DIR] [--no-motion]
    python tools/harness/storm_review.py report DIR        (re-make the sheets and numbers from a finished run)

Per object (a centre, a radius S km and a top H km):
  ring     8 bearings x 3 heights (ground 300 m at 1.6 S, 8 km at 2 S, high at 3 S), each looking at the centre
  top      straight down from 2.1 S km;  under: from 300 m under the core looking up toward the centre
  life     the object at several ages (the labs' storms) or hours (a hurricane), from the 8 km south view and the top
  motion   per-frame captures at a fixed sim step (the harness's fixed dt) from the 8 km view and the top:
           the consecutive-frame difference per frame, each frame normalised to its own mean and spread (a sunset is
           not a jump); a spike (max / median) is a jump, a teleport, a pop; sim_s_per_frame from the captures' state
  flicker  30 frames at 1x from the 8 km view, camera still: mean change, and a heatmap of where it changes

Output (DIR, default harness_runs/review/<stamp>): the captures, <obj>_ring.png / <obj>_extra.png / <obj>_motion.png
sheets, <obj>_<view>.mp4 for the motion runs (ffmpeg), numbers.json, and review.md: the numbers, the sheets and the
checklist (storm_review_checklist.md) for a reviewer who does not know what changed. Drives ONE live app
(live.py; started and stopped here if none is running). Needs the harness venv (Pillow + numpy).
"""
import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
LIVE = os.path.join(REPO, "harness_live")
R_KM = 6371.0

# The objects. setup puts the object in place with time paused; centre is (lat, lon) or "observer" (wherever setup left
# the eye); life = (command template, values) re-run per stage; motion = (time scale, frames) per motion view.
OBJECTS = {
    "cumulus": dict(setup=["time set 2036-12-04T17:30:00Z", "time pause", "storms test off"],
                    centre=(-2.79, -97.0), S=15.0, H=3.0,
                    life=None, motion=("5m", 90)),
    "supercell": dict(setup=["time set 2036-11-22T15:00:00Z", "time pause", "storms test lat=23 lon=10 age_min=60", "wait 200"],
                      centre=(23.0, 10.0), S=25.0, H=14.0,
                      life=("storms test lat=23 lon=10 age_min={v}", [10, 30, 60, 120, 180]), motion=("5m", 90)),
    "line": dict(setup=["time set 2036-11-22T15:00:00Z", "time pause", "storms test lat=23 lon=10 age_min=90 kind=line az=90", "wait 200"],
                 centre=(23.0, 10.0), S=110.0, H=14.0,
                 life=("storms test lat=23 lon=10 age_min={v} kind=line az=90", [20, 60, 120, 240]), motion=("5m", 90)),
    "hurricane": dict(setup=["storms test off", "time set 2036-09-10T16:00:00Z", "time pause",
                             "weather cyclone Isaac go=eye", "wait 200"],
                      centre="observer", S=350.0, H=15.0,
                      # (rings by its features: the ground and 8 km views from just outside the eyewall's reach, ~0.25 S)
                      rings=(("g", 0.22, 300.0), ("m", 0.3, 8000.0), ("h", 2.2, 400000.0)),
                      life=("time add {v}", [3600, 3600, 3600]), motion=("1h", 90)),
}


# ── geometry ──────────────────────────────────────────────────────────────────────────────────────────────────────────
def dest(lat, lon, brg, d_km):
    p1, l1, b, a = math.radians(lat), math.radians(lon), math.radians(brg), d_km / R_KM
    p2 = math.asin(math.sin(p1) * math.cos(a) + math.cos(p1) * math.sin(a) * math.cos(b))
    l2 = l1 + math.atan2(math.sin(b) * math.sin(a) * math.cos(p1), math.cos(a) - math.sin(p1) * math.sin(p2))
    return math.degrees(p2), (math.degrees(l2) + 540.0) % 360.0 - 180.0


def bearing(lat1, lon1, lat2, lon2):
    p1, p2, dl = math.radians(lat1), math.radians(lat2), math.radians(lon2 - lon1)
    return (math.degrees(math.atan2(math.sin(dl) * math.cos(p2),
                                    math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl))) + 360.0) % 360.0


def aim(lat, lon, agl_m, clat, clon, d_km, h_m):
    """Camera az / el from the eye to a point h_m over the centre (the Earth's curvature drops it by d^2 / 2R)."""
    az = bearing(lat, lon, clat, clon)
    drop = (d_km * 1000.0) ** 2 / (2.0 * R_KM * 1000.0)
    el = math.degrees(math.atan2(h_m - drop - agl_m, d_km * 1000.0))
    return az, max(-89.0, min(89.0, el))


def views(c, S, H, rings=None):
    """(name, lat, lon, agl_m, az, el, fov) for the ring, the top and the under view. S = the object's radius (km): the
    field of view fits its height and width at each ring's distance."""
    out = []
    for tag, k, agl in rings or (("g", 1.6, 300.0), ("m", 2.0, 8000.0), ("h", 3.0, max(25000.0, S * 1000.0))):
        d = S * k
        for brg in range(0, 360, 45):
            lat, lon = dest(c[0], c[1], brg, d)
            az, el = aim(lat, lon, agl, c[0], c[1], d, H * 500.0)
            hv = max(0.7 * H, 0.65 * S)
            fov = max(15.0, min(80.0, 2.0 * math.degrees(math.atan2(hv, math.hypot(d, (agl / 1000.0 - H * 0.5))))))
            out.append((f"{tag}{brg:03d}", lat, lon, agl, az, el, fov))
    out.append(("top", c[0], c[1], max(S * 2100.0, 20000.0), 0.0, -89.0, 60.0))
    lat, lon = dest(c[0], c[1], 180.0, S * 0.15)
    out.append(("under", lat, lon, 300.0, bearing(lat, lon, c[0], c[1]), 30.0, 90.0))
    return out


def view_cmds(v, name, settle=True, full=True):
    _, lat, lon, agl, az, el, fov = v
    cmds = [f"observer lat={lat:.5f} lon={lon:.5f} agl={agl:.0f}", f"camera az={az:.2f} el={el:.2f} fov={fov:.0f}"]
    if settle:
        cmds.append("wait settle auto max=120")
    cmds.append(f"capture {name}{'' if full else ' scale=0.5'} wait=1")
    return cmds


# ── the live app ──────────────────────────────────────────────────────────────────────────────────────────────────────
def live_alive():
    st = os.path.join(LIVE, "status.json")
    return os.path.exists(st) and time.time() - os.path.getmtime(st) < 5.0


def live_status():
    with open(os.path.join(LIVE, "status.json"), encoding="utf-8") as f:
        return json.load(f)


def send(cmds, timeout=1800.0):
    """One batch into the live inbox; returns the results list (raises on a failed command)."""
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


def observer_now():
    r = send(["capture _where scale=0.1 wait=1"])
    cap = os.path.join(live_status()["run_dir"], "captures", "_where.json")
    with open(cap, encoding="utf-8") as f:
        o = json.load(f)["state"]["observer"]
    return o["lat_deg"], o["lon_deg"]


# ── run ───────────────────────────────────────────────────────────────────────────────────────────────────────────────
QUICK = ("g180", "m000", "m090", "m180", "m270", "h135", "top")


def run_object(key, spec, out, motion, log, quick=False):
    send(spec["setup"])
    c = observer_now() if spec["centre"] == "observer" else spec["centre"]
    S, H = spec["S"], spec["H"]
    vs = views(c, S, H, spec.get("rings"))
    byname = {v[0]: v for v in vs}
    if quick:
        vs = [v for v in vs if v[0] in QUICK]
    info = dict(centre=c, S=S, H=H, views=[v[0] for v in vs])
    log(f"{key}: centre {c[0]:.3f} {c[1]:.3f}, {len(vs)} views")
    for v in vs:
        send(view_cmds(v, f"{key}_{v[0]}"))
    # Life: re-run the stage command, re-locate (a hurricane moves), the 8 km south view and the top.
    if spec["life"] and not quick:
        tmpl, vals = spec["life"]
        stages = []
        for i, val in enumerate(vals):
            send([tmpl.format(v=val), "wait 200"] + (["weather cyclone Isaac go=eye", "wait 30"] if spec["centre"] == "observer" else []))
            cc = observer_now() if spec["centre"] == "observer" else c
            lv = {v[0]: v for v in views(cc, S, H, spec.get("rings"))}
            send(view_cmds(lv["m180"], f"{key}_life{i}_m180") + view_cmds(lv["top"], f"{key}_life{i}_top"))
            stages.append(str(val))
        info["life"] = dict(command=tmpl, values=stages)
        send(spec["setup"])   # back to the reference moment
    # Flicker: 1x, camera still, 30 frames.
    v = byname["m180"]
    if not quick:
        send(view_cmds(v, f"{key}_flk_ref") + ["time scale 1x", "time play"]
         + [f"capture {key}_flk_{i:03d} scale=0.5" for i in range(30)] + ["time pause"])
    # Motion: per-frame captures at the object's time scale.
    if motion and not quick:
        scale, n = spec["motion"]
        info["motion"] = dict(scale=scale, frames=n)
        for vn in ("m180", "top"):
            send(spec["setup"])
            send(view_cmds(byname[vn], f"{key}_mot_{vn}_ref") + [f"time scale {scale}", "time play"]
                 + [f"capture {key}_mot_{vn}_{i:03d} scale=0.5" for i in range(n)] + ["time pause", "time scale 1x"])
    # Collect.
    src = os.path.join(live_status()["run_dir"], "captures")
    times = {}
    for f in os.listdir(src):
        if f.startswith(key + "_") and f.endswith(".png"):
            shutil.move(os.path.join(src, f), os.path.join(out, f))
            j = os.path.join(src, f[:-4] + ".json")
            if os.path.exists(j):
                if "_mot_" in f or "_flk_" in f:
                    try:
                        with open(j, encoding="utf-8") as fj:
                            times[f[:-4]] = json.load(fj)["state"]["time"]["j2000_s"]
                    except (OSError, KeyError, json.JSONDecodeError):
                        pass
                os.remove(j)
    info["times"] = times
    return info


def cmd_run(a):
    out = os.path.abspath(a.out or os.path.join(REPO, "harness_runs", "review", time.strftime("%Y%m%d_%H%M%S")))
    os.makedirs(out, exist_ok=True)
    started = False
    if not live_alive():
        args = [sys.executable, os.path.join(HERE, "live.py"), "start", "--window", "1920x1009"]
        st = a.settings or os.path.join(REPO, "user_data", "settings.json")   # the user's look, when there is one
        if os.path.exists(st):
            args += ["--settings", st]
        subprocess.check_call(args, cwd=REPO)
        started = True
    logf = open(os.path.join(out, "log.txt"), "a", encoding="utf-8")

    def log(s):
        print(s, flush=True)
        logf.write(s + "\n")

    meta = {"build": subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=REPO, capture_output=True, text=True).stdout.strip(),
            "objects": {}}
    mp = os.path.join(out, "meta.json")
    if os.path.exists(mp):   # (an object added to an earlier run: keep the others)
        with open(mp, encoding="utf-8") as f:
            meta["objects"] = json.load(f).get("objects", {})
    try:
        for key in a.objects.split(","):
            t0 = time.time()
            meta["objects"][key] = run_object(key, OBJECTS[key], out, not a.no_motion, log, a.quick)
            log(f"{key}: {time.time() - t0:.0f} s")
            with open(os.path.join(out, "meta.json"), "w", encoding="utf-8") as f:
                json.dump(meta, f, indent=1)
    finally:
        send(["storms test off"])
        if started and not a.keep:
            subprocess.call([sys.executable, os.path.join(HERE, "live.py"), "stop"], cwd=REPO)
    report(out)


# ── report ────────────────────────────────────────────────────────────────────────────────────────────────────────────
def report(out):
    try:
        import numpy as np
        from PIL import Image, ImageDraw
    except ImportError:
        sys.exit("the report needs Pillow + numpy: run with tools/harness/.venv/Scripts/python")
    sys.path.insert(0, HERE)
    from imgtools import font

    with open(os.path.join(out, "meta.json"), encoding="utf-8") as f:
        meta = json.load(f)
    fnt = font(16)

    def grid(cells, cols, path, w=480):
        """cells: [(label, png path or None)] -> a labelled sheet."""
        ims = []
        for lab, p in cells:
            im = Image.open(p).convert("RGB") if p and os.path.exists(p) else Image.new("RGB", (w, w // 2), (40, 40, 40))
            im = im.resize((w, int(im.height * w / im.width)), Image.BOX)
            ims.append((lab, im))
        ch = max(im.height for _, im in ims) + 22
        rows = (len(ims) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * (w + 4), rows * ch), (24, 24, 24))
        d = ImageDraw.Draw(sheet)
        for i, (lab, im) in enumerate(ims):
            x, y = (i % cols) * (w + 4), (i // cols) * ch
            sheet.paste(im, (x, y + 22))
            d.text((x + 4, y + 2), lab, fill=(230, 230, 230), font=fnt)
        sheet.save(path)
        return path

    def L(p):
        a = np.asarray(Image.open(p).convert("RGB"), dtype=np.float32)
        return a @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)

    def Z(p):
        """Luminance normalised to the frame's own mean and spread x 40 (levels): a sunset or the exposure moving over a
        motion run changes every pixel, not the shapes."""
        a = L(p)
        return (a - a.mean()) / max(float(a.std()), 1.0) * 40.0

    nums = {}
    md = [f"# Storm review — build {meta.get('build', '?')}", "",
          "Judge every sheet against `tools/harness/storm_review_checklist.md`. Name the sheet, the cell and the item.", ""]
    for key, info in meta["objects"].items():
        P = lambda n: os.path.join(out, f"{key}_{n}.png")
        ring = [(f"{t}{b:03d}", P(f"{t}{b:03d}")) for t in "gmh" for b in range(0, 360, 45) if os.path.exists(P(f"{t}{b:03d}"))]
        sheets = [grid(ring, 8 if len(ring) > 12 else 4, os.path.join(out, f"{key}_ring.png"), w=360)]
        extra = [("top", P("top")), ("under", P("under"))]
        for i, v in enumerate((info.get("life") or {}).get("values", [])):
            extra += [(f"life {v} m180", P(f"life{i}_m180")), (f"life {v} top", P(f"life{i}_top"))]
        sheets.append(grid(extra, 4, os.path.join(out, f"{key}_extra.png")))
        n = {}
        # Flicker: consecutive-frame change, camera still at 1x; the heatmap is the per-pixel std over the frames.
        flk = [P(f"flk_{i:03d}") for i in range(30) if os.path.exists(P(f"flk_{i:03d}"))]
        if len(flk) > 2:
            st = np.stack([L(p) for p in flk])
            dif = np.abs(np.diff(st, axis=0)).mean(axis=(1, 2))
            n["flicker_mean"] = round(float(dif.mean()), 3)
            n["flicker_p99_px"] = round(float(np.percentile(np.abs(np.diff(st, axis=0)), 99)), 2)
            sd = st.std(axis=0)
            heat = np.clip(sd * 16.0, 0, 255).astype(np.uint8)
            Image.fromarray(np.stack([heat, (heat * 0.5).astype(np.uint8), np.zeros_like(heat)], -1)).save(P("flicker_heat"))
        # Motion: per-frame change; the spike ratio (max / median) flags a jump.
        mot = info.get("motion")
        if mot:
            cells = []
            for vn in ("m180", "top"):
                fr = [P(f"mot_{vn}_{i:03d}") for i in range(mot["frames"]) if os.path.exists(P(f"mot_{vn}_{i:03d}"))]
                if len(fr) < 3:
                    continue
                prev, ds = Z(fr[0]), []
                for p in fr[1:]:
                    cur = Z(p)
                    ds.append(float(np.abs(cur - prev).mean()))
                    prev = cur
                ds = np.array(ds)
                med = float(np.median(ds)) or 1e-6
                worst = int(ds.argmax())
                tm = info.get("times", {})
                tt = [tm.get(f"{key}_mot_{vn}_{i:03d}") for i in range(len(fr))]
                dts = [b - a for a, b in zip(tt, tt[1:]) if a is not None and b is not None]
                n[f"motion_{vn}"] = dict(scale=mot["scale"], sim_s_per_frame=round(float(np.median(dts)), 1) if dts else None,
                                         median=round(med, 3), max=round(float(ds.max()), 3),
                                         spike=round(float(ds.max()) / med, 2), worst_frame=worst + 1,
                                         over_3x=int((ds > 3 * med).sum()))
                k = len(fr)
                cells += [(f"{vn} f{i}", fr[i]) for i in (0, k // 4, k // 2, 3 * k // 4, k - 1)]
                cells += [(f"{vn} jump {worst}->{worst + 1}", fr[worst]), (f"{vn} jump after", fr[worst + 1])]
                if shutil.which("ffmpeg"):
                    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", "30", "-i",
                                    os.path.join(out, f"{key}_mot_{vn}_%03d.png"), "-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2", "-c:v", "libx264", "-pix_fmt", "yuv420p",
                                    "-crf", "20", os.path.join(out, f"{key}_{vn}.mp4")])
            if cells:
                sheets.append(grid(cells, 7, os.path.join(out, f"{key}_motion.png"), w=320))
        nums[key] = n
        md += [f"## {key}", f"centre {info['centre'][0]:.3f}, {info['centre'][1]:.3f}; size {info['S']} km; top {info['H']} km", ""]
        md += [f"- `{os.path.basename(s)}`" for s in sheets]
        if os.path.exists(P("flicker_heat")):
            md.append(f"- `{key}_flicker_heat.png` (where a still 1x view changes: should be dark over cloud)")
        md += ["", "```", json.dumps(n, indent=1), "```", ""]
    with open(os.path.join(out, "numbers.json"), "w", encoding="utf-8") as f:
        json.dump(nums, f, indent=1)
    with open(os.path.join(out, "review.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(md))
    print(json.dumps(nums, indent=1))
    print(f"review: {os.path.join(out, 'review.md')}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    r = sp.add_parser("run")
    r.add_argument("--objects", default="cumulus,supercell,line,hurricane")
    r.add_argument("--out")
    r.add_argument("--no-motion", action="store_true")
    r.add_argument("--quick", action="store_true", help="7 views per object (QUICK), no life / flicker / motion: shape iteration")
    r.add_argument("--settings", help="start the app on these settings (default: user_data/settings.json if present)")
    r.add_argument("--keep", action="store_true", help="leave the live app running")
    p = sp.add_parser("report")
    p.add_argument("dir")
    a = ap.parse_args()
    if a.cmd == "run":
        unknown = [k for k in a.objects.split(",") if k not in OBJECTS]
        if unknown:
            sys.exit(f"unknown objects {unknown}; known: {', '.join(OBJECTS)}")
        cmd_run(a)
    else:
        report(os.path.abspath(a.dir))


if __name__ == "__main__":
    main()
