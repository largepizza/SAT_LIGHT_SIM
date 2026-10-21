#!/usr/bin/env python3
"""Mission-operations test suite (docs/HARNESS.md "Mission operations"): every launch vehicle and deployment style,
traced through its phases, with PASS / FAIL per vehicle x phase.

    python tools/harness/mission_ops.py run                      # numeric traces at the default epochs (~2-4 min)
    python tools/harness/mission_ops.py run --captures           # + per-phase captures and contact sheets (~20 min)
    python tools/harness/mission_ops.py run --vehicle falcon9 --epochs 2036-11-22
    python tools/harness/mission_ops.py report <run dir>         # re-evaluate a run's stored traces (no app)

One app (tools/harness/live.py, its own live folder) serves the whole run. For each epoch it asks
`launch trace from=<epoch>`: the next launch of every (vehicle, program) pair in data/launches.json, traced from the
launch model itself (SatelliteSim::launchTraceBatchJson): pad, climb, max-Q, staging, booster recovery, fairing,
insertion, coast, deployment, raising, arrival, retirees, upper-stage deorbit / entry / a ship's catch, docking. The
thresholds are THRESHOLDS below (one table: what each check is and why), applied here, so a run can be re-judged
without the app. Plus `launch formation` for a formation program (Starmind): the train's arrangement from release
to its rings of 8, now and with the old per-member schedule (before=1).

Output (harness_runs/mission_ops/<stamp>/): traces/<epoch>.json (raw), formation*.json, report.json (machine:
every check with its value, limit, status), report.md and report.html (the matrix + per-combo detail), and with
--captures captures/<combo>/*.png + sheets/<combo>.png (contact sheets: one tile per phase, follow + wide views).

Statuses: PASS, FAIL, WARN (outside the limit in a part another agent owns, e.g. the booster's landing burn
physics: reported, not failed), SKIP (a crew flight whose spacecraft has no model), N/A (the phase does not exist for this launch),
NOT FOUND (no launch of the pair in the search window).
"""
import argparse
import glob
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT_ROOT = os.path.join(REPO, "harness_runs", "mission_ops")

DEFAULT_EPOCHS = ["2036-11-22", "2031-06-01", "2026-11-05"]

PHASES = ["pad_liftoff", "ascent", "max_q", "meco_staging", "booster_recovery", "upper_ascent", "fairing_sep",
          "insertion", "coast", "deployment", "raising", "arrival", "retiree_deorbit", "docking", "upper_deorbit",
          "upper_entry", "ship_catch"]

# Each check: (phase, metric path, op, limit, owner, why). op: "<", ">", "abs<" (|value| < limit), "range" (limit =
# [lo, hi]), "absdiff<" (limit = [other metric, tol]: |value - other| < tol), "true", "false". owner: "" = a FAIL,
# "landing" = a WARN (the booster landing-burn physics, owned by another agent), "info" = recorded only.
THRESHOLDS = [
    ("pad_liftoff", "pad_offset_m", "<", 60.0, "", "the stack stands on its pad (sites within 150 m share one pad)"),
    ("pad_liftoff", "height_above_sea_m", "absdiff<", ["pad_base_m", 3.0], "", "on the pad's plateau + mount"),
    ("pad_liftoff", "rise_10s_m", ">", 5.0, "", "it rises off the pad"),
    ("ascent", "max_proper_accel_m_s2", "<", 60.0, "", "no jumps on the climb (proper acceleration < 6 g)"),
    ("ascent", "nose_max_deg_s", "<", 10.0, "", "the nose turns smoothly on the climb"),
    ("ascent", "max_alt_drop_m", "<", 1.0, "", "the climb never loses height"),
    ("max_q", "q_kpa", "range", [10.0, 70.0], "", "a plausible max-Q"),
    ("max_q", "alt_km", "range", [5.0, 22.0], "", "max-Q at 5-22 km"),
    ("meco_staging", "alt_km", "absdiff<", ["alt_spec_km", 15.0], "", "MECO near the vehicle's figure"),
    ("meco_staging", "stage_gap_m", "<", 50.0, "", "the upper stage starts where the stack was"),
    ("meco_staging", "velocity_jump_m_s", "<", 30.0, "", "no velocity jump at separation"),
    ("booster_recovery", "nose_max_deg_s", "<", 25.0, "", "the booster's flips are slews"),
    ("booster_recovery", "max_proper_accel_m_s2", "<", 80.0, "landing", "no jumps after staging (burns < 8 g)"),
    ("booster_recovery", "touchdown_off_m", "<", 10.0, "", "lands on its target (tower / zone / deck / ground)"),
    ("booster_recovery", "touchdown_above_m", "abs<", 2.0, "", "comes to rest at its target's height"),
    ("booster_recovery", "touchdown_speed_m_s", "<", 35.0, "landing", "last-second mean speed (a 50 m/s^2 burn reads ~25)"),
    ("booster_recovery", "landing_tilt_deg_max_last10s", "<", 15.0, "landing", "upright in the last 10 s"),
    ("booster_recovery", "ride_home_off_deck_m", "<", 1.0, "", "rides home on the ship's deck"),
    ("upper_ascent", "max_proper_accel_m_s2", "<", 60.0, "", "no jumps to orbit"),
    ("upper_ascent", "nose_max_deg_s", "<", 6.0, "", "the upper stage's nose turns smoothly"),
    ("fairing_sep", "alt_km", "absdiff<", ["spec_km", 5.0], "", "fairing goes at the vehicle's altitude"),
    ("fairing_sep", "before_seco", "true", None, "", "before SECO"),
    ("fairing_sep", "half_gap_after_10s_m", "range", [5.0, 3000.0], "", "the halves fall away (1.8 m/s apart; the stage keeps thrusting: ~0.5 a t^2)"),
    ("fairing_sep", "halves_fall_s", ">", 60.0, "", "the halves fall for minutes"),
    ("insertion", "alt_km", "absdiff<", ["park_km", 5.0], "", "SECO at the parking altitude"),
    ("insertion", "speed_m_s", "absdiff<", ["circular_speed_m_s", 40.0], "", "circular speed"),
    ("insertion", "flight_path_angle_deg", "abs<", 0.5, "", "horizontal at insertion"),
    ("insertion", "plane_angle_deg", "<", 2.0, "", "launched into the target plane (members drift the rest)"),
    ("insertion", "seco_max_proper_accel_m_s2", "<", 60.0, "", "no jump at SECO"),
    ("coast", "alt_min_km", "absdiff<", ["alt_max_km", 2.0], "", "a circular coast"),
    ("deployment", "release_err_m_max", "<", 5.0, "", "each member appears at the dispenser / its stack slot"),
    ("deployment", "recede_60s_m_min", ">", 0.0, "", "each member moves away from the stage"),
    ("deployment", "min_member_gap_1_10min_m", ">", 2.0, "", "the train leaves cleanly: no two members within 2 m from 1 to 10 min after the later one leaves"),
    ("deployment", "min_member_gap_after_60s_m", ">", 2.0, "info", "closest pair over the first 2 h (late close passes come from the CW offsets about each member's own plane)"),
    ("deployment", "member_gap_ratio_min", ">", 0.9, "info", "closest approach of a pair / its gap when the later one left"),
    ("deployment", "min_stage_gap_2h_m", ">", 1.0, "", "nor through the stage"),
    ("raising", "max_proper_accel_after_2_5d_m_s2", "<", 0.3, "", "a gentle climb once the release offset is gone (the radius ramp's corners: 0.1-0.25 m/s per s)"),
    ("raising", "descend_after_2_5d_m_max", "<", 200.0, "", "the climb does not sink (raising is monotonic)"),
    ("raising", "max_1s_step_over_circular_m", "<", 100.0, "", "no position jumps while raising"),
    ("arrival", "pos_err_m_max", "<", 50.0, "", "arrives on its slot"),
    ("arrival", "vel_err_m_s_max", "<", 1.0, "", "at the slot's velocity"),
    ("retiree_deorbit", "start_pos_err_m_max", "<", 50.0, "", "the retiree starts at its slot"),
    ("retiree_deorbit", "start_vel_err_m_s_max", "<", 1.0, "", "at the slot's velocity"),
    ("retiree_deorbit", "end_alt_km_max", "range", [190.0, 215.0], "", "lowered to ~200 km before re-entry"),
    ("retiree_deorbit", "max_rise_m", "<", 100.0, "", "the deorbit never climbs"),
    # Crew spacecraft (2026-10-20, CLAUDE.md Launches > "Crew spacecraft fly as their own objects").
    ("docking", "sep_offset_m", "<", 1.0, "", "the spacecraft separates from its place on the upper stage"),
    ("docking", "hours_after_launch", "range", [2.0, 72.0], "", "docks hours to a few days after lift-off (phasing)"),
    ("docking", "arrival_range_to_station_km", "range", [3.0, 8.0], "", "the phasing ends ~5 km behind the station"),
    ("docking", "hold400_range_m", "range", [390.0, 410.0], "", "holds 400 m out on the port's axis"),
    ("docking", "hold200_range_m", "range", [190.0, 210.0], "", "holds 200 m out"),
    ("docking", "hold20_range_m", "range", [15.0, 25.0], "", "holds 20 m out"),
    ("docking", "dock_contact_m", "<", 0.5, "", "contact at the station model's docking port"),
    ("docking", "docked_after_60s", "true", None, "", "docked after contact"),
    ("docking", "docked_mid_stay", "true", None, "", "still docked months later (rides with the station)"),
    ("docking", "undocked_after", "true", None, "", "undocks and backs out at its departure"),
    ("docking", "docked_days", "range", [1.0, 400.0], "", "stays docked for the rotation"),
    ("docking", "accel_max_mps2", "<", 200.0, "", "no position jumps at any seam (burns and the entry: tens to ~120 m/s^2)"),
    ("docking", "turn_max_deg_s", "<", 15.0, "", "turns smoothly (no half-turn flips)"),
    ("upper_deorbit", "nose_from_retrograde_deg", "<", 15.0, "", "burns retrograde"),
    ("upper_deorbit", "nose_max_deg_s", "<", 6.0, "", "turns smoothly before and after the burn"),
    ("upper_deorbit", "burn_max_proper_accel_m_s2", "<", 100.0, "", "no jump in the deorbit burn (the smoothstep blend peaks at ~4x dv / burn)"),
    ("upper_entry", "alt_km", "range", [95.0, 105.0], "", "the entry interface at ~100 km"),
    ("upper_entry", "max_1s_step_m", "<", 8500.0, "", "continuous through the entry interface"),
    ("ship_catch", "off_centre_m", "<", 5.0, "", "the ship is caught at the tower"),
    ("ship_catch", "above_ground_m", "range", [50.0, 60.0], "", "under its flaps, ~55 m up"),
    ("ship_catch", "handover_m", "<", 50.0, "", "entry to descent hand-over is continuous"),
]

# The formation (Starmind rings of 8): the train converges into its rings once the release offsets ease out.
FORMATION_THRESHOLDS = [
    ("ring_shape_err_after_2d_m_max", "<", 50.0, "from day 2 the eight hold their ring's shape (every pairwise distance within 50 m of the slots')"),
    ("shape_err_growth_after_2d_m_max", "<", 25.0, "and it only converges after that"),
    ("spread_after_2d_m_max", "<", 2000.0, "the group stays together (no member thousands of km away)"),
]


def get(d, path):
    for k in path.split("."):
        if not isinstance(d, dict) or k not in d:
            return None
        d = d[k]
    return d


def check_one(phase_obj, metric, op, limit):
    v = get(phase_obj, metric)
    if v is None:
        return None, None
    if op == "<":
        return v, v < limit
    if op == ">":
        return v, v > limit
    if op == "abs<":
        return v, abs(v) < limit
    if op == "range":
        return v, limit[0] <= v <= limit[1]
    if op == "absdiff<":
        o = get(phase_obj, limit[0])
        if o is None:
            return v, None
        return v, abs(v - o) < limit[1]
    if op == "true":
        return v, bool(v)
    if op == "false":
        return v, not v
    raise ValueError(op)


def evaluate_trace(tr):
    """{phase: {"status": ..., "checks": [...]}} for one traced launch."""
    out = {}
    phases = tr.get("phases", {})
    for ph in PHASES:
        po = phases.get(ph)
        res = {"status": "N/A", "checks": []}
        if po is None:
            if ph == "docking" and tr.get("crew"):
                res["status"] = "FAIL"
                res["note"] = "crew flight without a docking phase"
            out[ph] = res
            continue
        res["t_plus_s"] = po.get("t_plus_s")
        res["utc"] = po.get("utc")
        if po.get("not_applicable"):
            out[ph] = res
            continue
        if ph == "docking" and not po.get("modelled", False):
            res["status"] = "SKIP"
            res["note"] = "this crew flight has no spacecraft model (launches.json crew \"model\")"
            res["metrics"] = {k: v for k, v in po.items() if k not in ("t", "utc")}
            out[ph] = res
            continue
        worst = "PASS"
        for (p, metric, op, limit, owner, why) in THRESHOLDS:
            if p != ph:
                continue
            v, ok = check_one(po, metric, op, limit)
            if v is None or ok is None:
                continue
            st = "PASS" if ok else ("WARN" if owner == "landing" else "INFO" if owner == "info" else "FAIL")
            res["checks"].append({"metric": metric, "value": v, "op": op, "limit": limit, "status": st, "why": why})
            if st == "FAIL":
                worst = "FAIL"
            elif st == "WARN" and worst == "PASS":
                worst = "WARN"
        # Ground landing that is really at sea (a droneship vehicle without a ship for this site).
        if ph == "booster_recovery" and po.get("kind") == "ground" and (po.get("end_shore_dist_m") or 0) > 3000:
            res["checks"].append({"metric": "kind", "value": "ground", "op": "at sea",
                                  "limit": po.get("end_shore_dist_m"), "status": "FAIL",
                                  "why": "a 'ground' landing over open sea (no landing ship serves this site)"})
            worst = "FAIL"
        if ph == "booster_recovery" and po.get("kind") == "expendable" and (po.get("end_shore_dist_m") or 0) < -3000:
            res["checks"].append({"metric": "end_shore_dist_m", "value": po.get("end_shore_dist_m"), "op": "> -3000",
                                  "limit": -3000, "status": "INFO",
                                  "why": "an expendable booster comes down over land (inland sites do this for real)"})
        res["status"] = worst
        res["metrics"] = {k: v for k, v in po.items() if k not in ("t", "utc")}
        out[ph] = res
    return out


def evaluate_formation(f):
    res = {"status": "PASS", "checks": []}
    for (metric, op, limit, why) in FORMATION_THRESHOLDS:
        v, ok = check_one(f, metric, op, limit)
        if v is None:
            continue
        st = "PASS" if ok else "FAIL"
        res["checks"].append({"metric": metric, "value": v, "op": op, "limit": limit, "status": st, "why": why})
        if st == "FAIL":
            res["status"] = "FAIL"
    return res


# ── The live app ─────────────────────────────────────────────────────────────────────────────────────────
class Live:
    def __init__(self, d, window="1280x720"):
        self.dir = d
        self.window = window

    def start(self):
        p = subprocess.run([sys.executable, os.path.join(HERE, "live.py"), "--dir", self.dir, "start", "--window", self.window,
                            "--timeout", "180"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        print(p.stdout.strip())
        if p.returncode != 0:
            sys.exit("could not start the app")
        st = json.load(open(os.path.join(self.dir, "status.json")))
        self.run_dir = st.get("run_dir")

    def send(self, text, timeout=900):
        name = f"{int(time.time() * 1000)}_{os.getpid()}"
        inbox = os.path.join(self.dir, "inbox")
        os.makedirs(inbox, exist_ok=True)
        tmp = os.path.join(inbox, name + ".tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(text.strip() + "\n")
        os.replace(tmp, os.path.join(inbox, name + ".satcmd"))
        out = os.path.join(self.dir, "outbox", name + ".json")
        t0 = time.time()
        while time.time() - t0 < timeout:
            if os.path.exists(out):
                try:
                    with open(out, encoding="utf-8") as f:
                        return json.load(f)["results"]
                except (OSError, json.JSONDecodeError):
                    pass
            time.sleep(0.2)
        raise TimeoutError(f"no answer within {timeout}s for: {text[:200]}")

    def stop(self):
        subprocess.run([sys.executable, os.path.join(HERE, "live.py"), "--dir", self.dir, "stop"],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def combo_id(c, epoch):
    return f"{epoch}_{c['vehicle']}_{c['program'].replace(' ', '_').replace('/', '_')}"


# ── Captures ──────────────────────────────────────────────────────────────────────────────────────────
def follow_dist(vehicle):
    return 450.0 if vehicle == "starship" else 160.0


def capture_combo(live, cid, c, cap_dir):
    """Per-phase captures of one traced launch: a ground view at the pad, then follow views (close + wide) at every
    milestone of the rocket (payload role), the booster's after staging, and the train's (a member)."""
    tr = c["trace"]
    key = c["key"]
    ph = tr.get("phases", {})
    L = follow_dist(c["vehicle"])
    shots = []

    def cap(name, label):
        full = f"{cid}__{name}"
        shots.append({"name": name, "label": label, "png": os.path.join(cap_dir, full + ".png")})
        return f"capture {full} scale=0.5"

    def run(cmds, timeout=600):
        res = live.send("\n".join(cmds), timeout=timeout)
        for r in res:
            if not r.get("ok"):
                print(f"    ERR {r.get('cmd')}: {r.get('error')}")
        return res

    close = f"follow offset={-L:.0f},{0.8 * L:.0f},{0.3 * L:.0f} aim=sat"
    wide = "follow offset=-1500,5000,1500 aim=sat"
    # Pad: on the ground beside it, 15 s before lift-off, looking at the stack.
    run([f"launch goto key={key} t=-15 ground=1", "time play", "wait 40", "launch select kind=rocket",
         "wait 10", "camera look sel", "camera fov=5", cap("00_pad_ground", "pad (ground view ~12 km, T-15 s)")])
    # The rocket's milestones (payload role), following it.
    run([f"launch goto key={key} t=-5", "wait 30", "launch select kind=rocket", "follow " + close.split(" ", 1)[1], "wait 5"])
    res = run(["missions milestone list"])
    ms = (res[-1].get("result") or {}).get("mission", {}).get("milestones", []) if res else []
    for m in ms:
        i = m.get("i")
        lab = m.get("label", "")
        if "re-enters" in lab and "Upper" not in lab and "Ship" not in lab:
            continue
        run([f"missions milestone {i}", "wait 25", close, "wait 3", cap(f"p{i:02d}_close", f"{lab} (close)"),
             wide, "wait 3", cap(f"p{i:02d}_wide", f"{lab} (wide)")])
    # The booster after staging.
    meco = (ph.get("meco_staging") or {}).get("t_plus_s")
    if meco:
        run(["follow off", f"launch goto key={key} t={meco + 3:.0f}", "wait 30", "launch select kind=booster nearest=1",
             "follow " + close.split(" ", 1)[1], "wait 5"])
        res = run(["missions milestone list"])
        bms = (res[-1].get("result") or {}).get("mission", {}).get("milestones", []) if res else []
        for m in bms:
            if m.get("who", 0) & 2 == 0 or (m.get("in_s", 0) or 0) < -5:
                pass
            lab = m.get("label", "")
            if "Booster" not in lab and "burn" not in lab.lower():
                continue
            i = m.get("i")
            run([f"missions milestone {i}", "wait 25", close, "wait 3", cap(f"b{i:02d}_close", f"booster: {lab} (close)"),
                 wide, "wait 3", cap(f"b{i:02d}_wide", f"booster: {lab} (wide)")])
    # The train: a member after its release, at its milestones.
    dep = ph.get("deployment")
    if dep and tr.get("members", 0) > 0:
        t = dep["t_plus_s"] + (dep.get("interval_s") or 2.0) * 2 + 20
        run(["follow off", f"launch goto key={key} t={t:.0f}", "wait 30", "launch select kind=upper nearest=1",
             f"follow offset=-200,150,60 aim=sat", "wait 10", "launch select kind=train member=0",
             "follow offset=-60,40,15 aim=sat", "wait 10", cap("t00_release", "member 0 just released (close)")])
        res = run(["missions milestone list"])
        tms = (res[-1].get("result") or {}).get("mission", {}).get("milestones", []) if res else []
        for m in tms:
            lab = m.get("label", "")
            if not any(s in lab for s in ("raising", "Arrival", "Train", "unfold", "Unfold")):
                continue
            i = m.get("i")
            run([f"missions milestone {i}", "wait 25", "follow offset=-60,40,15 aim=sat", "wait 3",
                 cap(f"t{i:02d}_close", f"train: {lab}"), "follow offset=-3000,2000,600 aim=sat", "wait 3",
                 cap(f"t{i:02d}_wide", f"train: {lab} (wide)")])
    run(["follow off"])
    return shots


def contact_sheet(cid, shots, path, status_line):
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        return False
    have = [s for s in shots if os.path.exists(s["png"])]
    if not have:
        return False
    tw = 400
    ims = []
    for s in have:
        im = Image.open(s["png"]).convert("RGB")
        im = im.resize((tw, round(im.height * tw / im.width)))
        ims.append((s, im))
    th = max(im.height for _, im in ims)
    cols = 4
    rows = (len(ims) + cols - 1) // cols
    try:
        fnt = ImageFont.truetype("arial.ttf", 14)
    except OSError:
        fnt = ImageFont.load_default()
    head = 40
    sheet = Image.new("RGB", (cols * tw, head + rows * (th + 22)), (18, 18, 22))
    d = ImageDraw.Draw(sheet)
    d.text((8, 6), cid, fill=(240, 240, 240), font=fnt)
    d.text((8, 22), status_line, fill=(200, 200, 120), font=fnt)
    for k, (s, im) in enumerate(ims):
        x, y = (k % cols) * tw, head + (k // cols) * (th + 22)
        sheet.paste(im, (x, y + 20))
        d.text((x + 4, y + 3), s["label"][:60], fill=(220, 220, 220), font=fnt)
    sheet.save(path)
    return True


# ── Report ────────────────────────────────────────────────────────────────────────────────────────────
STATUS_COLOR = {"PASS": "#2e7d32", "FAIL": "#c62828", "WARN": "#ef6c00", "SKIP": "#6d6d6d", "N/A": "#3a3a3a",
                "NOT FOUND": "#8e24aa"}


def build_report(run_dir):
    traces = {}
    for p in sorted(glob.glob(os.path.join(run_dir, "traces", "*.json"))):
        traces[os.path.splitext(os.path.basename(p))[0]] = json.load(open(p, encoding="utf-8"))
    rows = []
    for epoch, t in traces.items():
        for c in t.get("combos", []):
            row = {"epoch": epoch, "vehicle": c.get("vehicle"), "program": c.get("program"), "layout": c.get("layout"),
                   "found": c.get("found", False), "why": c.get("why"), "key": c.get("key"), "utc": c.get("utc_launch"),
                   "id": combo_id(c, epoch)}
            if c.get("found") and c.get("trace"):
                tr = c["trace"]
                row.update({"site": tr.get("site"), "recovery": tr.get("recovery"), "members": tr.get("members"),
                            "first_fill": tr.get("first_fill"), "early_vehicle": tr.get("early_vehicle")})
                row["phases"] = evaluate_trace(tr)
            rows.append(row)
    formation = {}
    for p in sorted(glob.glob(os.path.join(run_dir, "formation*.json"))):
        f = json.load(open(p, encoding="utf-8"))
        formation[os.path.splitext(os.path.basename(p))[0]] = {"eval": evaluate_formation(f), "data": f}
    summary = {"PASS": 0, "FAIL": 0, "WARN": 0, "SKIP": 0}
    for r in rows:
        for ph in (r.get("phases") or {}).values():
            if ph["status"] in summary:
                summary[ph["status"]] += 1
    rep = {"run_dir": run_dir, "generated": time.strftime("%Y-%m-%d %H:%M:%S"), "summary": summary, "rows": rows,
           "formation": {k: v["eval"] for k, v in formation.items()},
           "thresholds": [dict(zip(("phase", "metric", "op", "limit", "owner", "why"), t)) for t in THRESHOLDS]}
    json.dump(rep, open(os.path.join(run_dir, "report.json"), "w", encoding="utf-8"), indent=1)
    write_markdown(rep, os.path.join(run_dir, "report.md"))
    write_html(rep, formation, os.path.join(run_dir, "report.html"))
    return rep


def short(st):
    return {"PASS": "ok", "FAIL": "FAIL", "WARN": "warn", "SKIP": "skip", "N/A": "-"}.get(st, st)


def write_markdown(rep, path):
    L = [f"# Mission operations report\n", f"{rep['generated']}  |  {rep['run_dir']}\n",
         f"Phase checks: {rep['summary']}\n", "| epoch | vehicle | program | layout | recovery | " + " | ".join(PHASES) + " |",
         "|" + "---|" * (5 + len(PHASES))]
    for r in rep["rows"]:
        if not r.get("found"):
            L.append(f"| {r['epoch']} | {r['vehicle']} | {r['program']} | {r.get('layout') or ''} | NOT FOUND: {r.get('why')} |" +
                     " |" * len(PHASES))
            continue
        cells = [short(r["phases"][p]["status"]) for p in PHASES]
        L.append(f"| {r['epoch']} | {r['vehicle']} | {r['program']} | {r['layout']} | {r.get('recovery')} | " + " | ".join(cells) + " |")
    L.append("\n## Failures and warnings\n")
    for r in rep["rows"]:
        for p, ph in (r.get("phases") or {}).items():
            for ck in ph.get("checks", []):
                if ck["status"] in ("FAIL", "WARN"):
                    L.append(f"- **{ck['status']}** {r['epoch']} {r['vehicle']} / {r['program']} `{p}.{ck['metric']}` = "
                             f"{fmt(ck['value'])} (limit {ck['op']} {ck['limit']}): {ck['why']}")
            if ph.get("note") and ph["status"] == "FAIL":
                L.append(f"- **FAIL** {r['epoch']} {r['vehicle']} / {r['program']} `{p}`: {ph['note']}")
    L.append("\n## Formation (rings of 8)\n")
    for k, f in rep["formation"].items():
        L.append(f"- {k}: **{f['status']}** " + "; ".join(f"{c['metric']} = {fmt(c['value'])} ({c['status']})" for c in f["checks"]))
    open(path, "w", encoding="utf-8").write("\n".join(L) + "\n")


def fmt(v):
    if isinstance(v, float):
        return f"{v:.4g}"
    return str(v)


def write_html(rep, formation, path):
    import html
    h = ["<!doctype html><html><head><meta charset='utf-8'><title>Mission operations</title><style>",
         "body{font:13px system-ui,Segoe UI,sans-serif;background:#111;color:#ddd;margin:16px}",
         "table{border-collapse:collapse}td,th{border:1px solid #333;padding:3px 6px;text-align:center}",
         "th{background:#222;position:sticky;top:0}td.l{text-align:left}a{color:#8cf}",
         "details{margin:6px 0}summary{cursor:pointer}.m{font:12px monospace;color:#aaa}</style></head><body>",
         f"<h1>Mission operations</h1><p>{html.escape(rep['generated'])} &middot; {html.escape(rep['run_dir'])}</p>",
         f"<p>Phase checks: {html.escape(json.dumps(rep['summary']))}</p><table><tr><th>epoch</th><th>vehicle</th>"
         "<th>program</th><th>layout</th><th>site</th><th>recovery</th>" +
         "".join(f"<th>{p.replace('_', ' ')}</th>" for p in PHASES) + "</tr>"]
    for r in rep["rows"]:
        if not r.get("found"):
            h.append(f"<tr><td>{r['epoch']}</td><td>{r['vehicle']}</td><td class=l>{html.escape(r['program'] or '')}</td>"
                     f"<td>{r.get('layout') or ''}</td><td colspan={2 + len(PHASES)} style='background:{STATUS_COLOR['NOT FOUND']}'>"
                     f"NOT FOUND: {html.escape(str(r.get('why')))}</td></tr>")
            continue
        h.append(f"<tr><td>{r['epoch']}</td><td>{r['vehicle']}</td><td class=l><a href='#{r['id']}'>{html.escape(r['program'])}</a></td>"
                 f"<td>{r['layout']}</td><td>{r.get('site')}</td><td>{r.get('recovery')}</td>")
        for p in PHASES:
            st = r["phases"][p]["status"]
            h.append(f"<td style='background:{STATUS_COLOR.get(st, '#333')}'>{short(st)}</td>")
        h.append("</tr>")
    h.append("</table>")
    if formation:
        h.append("<h2>Formation (rings of 8)</h2>")
        for k, f in formation.items():
            e = f["eval"]
            h.append(f"<h3>{html.escape(k)}: <span style='color:{STATUS_COLOR[e['status']]}'>{e['status']}</span></h3><ul>")
            for c in e["checks"]:
                h.append(f"<li>{c['metric']} = {fmt(c['value'])} ({c['op']} {c['limit']}): {c['status']} &mdash; {html.escape(c['why'])}</li>")
            h.append("</ul><table><tr><th>t after release (h)</th><th>ring shape err (m)</th><th>offset err vs slot ring (m)</th><th>spread (m)</th><th>ring radius (m)</th>"
                     "<th>centroid to slot centroid (km)</th></tr>")
            for s in f["data"].get("samples", []):
                h.append(f"<tr><td>{s['t_after_release_h']:.2f}</td><td>{s.get('ring_shape_err_m_max', 0):.1f}</td><td>{s['ring_err_m_max']:.1f}</td>"
                         f"<td>{s['spread_from_centroid_m_max']:.1f}</td><td>{s['ring_radius_m']:.1f}</td>"
                         f"<td>{(s.get('cluster0') or {}).get('centroid_to_slot_km', 0):.1f}</td></tr>")
            h.append("</table>")
    h.append("<h2>Per launch</h2>")
    run_dir = rep["run_dir"]
    for r in rep["rows"]:
        if not r.get("found"):
            continue
        h.append(f"<h3 id='{r['id']}'>{r['epoch']} &middot; {r['vehicle']} &middot; {html.escape(r['program'])} "
                 f"<span class=m>key {r['key']}, lift-off {html.escape(str(r['utc']))}, {r.get('site')}, {r.get('members')} members"
                 f"{', first fill' if r.get('first_fill') else ''}{', early vehicle' if r.get('early_vehicle') else ''}</span></h3>")
        sheet = os.path.join(run_dir, "sheets", r["id"] + ".png")
        if os.path.exists(sheet):
            h.append(f"<p><a href='sheets/{r['id']}.png'><img src='sheets/{r['id']}.png' style='max-width:100%'></a></p>")
        for p in PHASES:
            ph = r["phases"][p]
            if ph["status"] == "N/A":
                continue
            h.append(f"<details><summary><b style='color:{STATUS_COLOR.get(ph['status'])}'>{ph['status']}</b> {p}"
                     f" <span class=m>T+{fmt(ph.get('t_plus_s'))} s {html.escape(str(ph.get('utc')))}</span></summary><ul>")
            for ck in ph.get("checks", []):
                h.append(f"<li><b style='color:{STATUS_COLOR.get(ck['status'], '#aaa')}'>{ck['status']}</b> {ck['metric']} = "
                         f"{html.escape(fmt(ck['value']))} ({ck['op']} {html.escape(json.dumps(ck['limit']))}) &mdash; {html.escape(ck['why'])}</li>")
            if ph.get("note"):
                h.append(f"<li>{html.escape(ph['note'])}</li>")
            h.append(f"<li class=m>{html.escape(json.dumps(ph.get('metrics', {}))[:3000])}</li></ul></details>")
    h.append("</body></html>")
    open(path, "w", encoding="utf-8").write("\n".join(h))


# ── Main ──────────────────────────────────────────────────────────────────────────────────────────────
def cmd_run(a):
    stamp = time.strftime("%Y%m%d_%H%M%S")
    run_dir = os.path.abspath(a.out or os.path.join(OUT_ROOT, stamp))
    os.makedirs(os.path.join(run_dir, "traces"), exist_ok=True)
    live = Live(os.path.join(run_dir, "live"), window=a.window)
    live.start()
    try:
        filt = (f" vehicle={a.vehicle}" if a.vehicle else "") + (f' program="{a.program}"' if a.program else "")
        formation_key = None
        for ep in a.epochs:
            t0 = time.time()
            res = live.send(f"time set {ep}T00:00:00Z\ntime pause\nlaunch trace from={ep} days={a.days}{filt}", timeout=1800)
            tr = (res[-1].get("result") or {}).get("trace") if res else None
            if not res[-1].get("ok") or tr is None:
                print(f"  {ep}: ERR {res[-1].get('error')}")
                continue
            json.dump(tr, open(os.path.join(run_dir, "traces", ep + ".json"), "w", encoding="utf-8"), indent=1)
            nf = sum(1 for c in tr["combos"] if c.get("found"))
            print(f"  {ep}: {nf} / {len(tr['combos'])} pairs traced ({time.time() - t0:.0f} s)")
            for c in tr["combos"]:
                if c.get("found") and (c.get("trace") or {}).get("cluster_size", 1) > 1 and formation_key is None:
                    formation_key = (ep, c["key"])
        if formation_key and not a.no_formation:
            ep, key = formation_key
            for before in (0, 1):
                res = live.send(f"launch formation key={key} samples=10 before={before}", timeout=600)
                f = (res[-1].get("result") or {}).get("formation")
                if f:
                    json.dump(f, open(os.path.join(run_dir, f"formation_{ep}{'_before' if before else ''}.json"), "w",
                                      encoding="utf-8"), indent=1)
        # Performance side effect: launch check over whatever the traces built.
        res = live.send("launch check", timeout=600)
        if res and res[-1].get("ok"):
            json.dump(res[-1]["result"]["check"], open(os.path.join(run_dir, "launch_check.json"), "w"), indent=1)
        if a.captures:
            cap_dir = os.path.join(live.run_dir, "captures") if live.run_dir else os.path.join(run_dir, "captures")
            os.makedirs(os.path.join(run_dir, "sheets"), exist_ok=True)
            rep = build_report(run_dir)
            status_of = {r["id"]: r for r in rep["rows"]}
            captured = set()
            for ep in a.epochs:
                p = os.path.join(run_dir, "traces", ep + ".json")
                if not os.path.exists(p):
                    continue
                for c in json.load(open(p, encoding="utf-8"))["combos"]:
                    if not c.get("found"):
                        continue
                    cid = combo_id(c, ep)
                    if a.capture_epochs and ep not in a.capture_epochs:
                        continue
                    pair = (c["vehicle"], c["program"])
                    if pair in captured:  # each vehicle / program pair once (its first epoch)
                        continue
                    captured.add(pair)
                    print(f"  captures: {cid}")
                    t0 = time.time()
                    try:
                        shots = capture_combo(live, cid, c, cap_dir)
                    except TimeoutError as e:
                        print(f"    timeout: {e}")
                        continue
                    r = status_of.get(cid, {})
                    line = " ".join(f"{k}:{short(v['status'])}" for k, v in (r.get("phases") or {}).items() if v["status"] != "N/A")
                    # (the PNGs are in the live run folder; copy them next to the report)
                    dst = os.path.join(run_dir, "captures", cid)
                    os.makedirs(dst, exist_ok=True)
                    for s in shots:
                        if os.path.exists(s["png"]):
                            import shutil
                            shutil.copy2(s["png"], os.path.join(dst, os.path.basename(s["png"])))
                            s["png"] = os.path.join(dst, os.path.basename(s["png"]))
                    json.dump(shots, open(os.path.join(dst, "shots.json"), "w"), indent=1)
                    contact_sheet(cid, shots, os.path.join(run_dir, "sheets", cid + ".png"), line)
                    print(f"    {len(shots)} shots ({time.time() - t0:.0f} s)")
    finally:
        live.stop()
    rep = build_report(run_dir)
    print(f"report: {os.path.join(run_dir, 'report.html')}")
    print(f"phase checks: {rep['summary']}; formation: " + ", ".join(f"{k} {v['status']}" for k, v in rep["formation"].items()))
    return 1 if rep["summary"]["FAIL"] else 0


def cmd_report(a):
    rep = build_report(os.path.abspath(a.run_dir))
    print(f"phase checks: {rep['summary']}")
    print(os.path.join(a.run_dir, "report.html"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("run")
    s.add_argument("--epochs", type=lambda x: x.split(","), default=DEFAULT_EPOCHS)
    s.add_argument("--days", type=float, default=400.0, help="search window per pair (days)")
    s.add_argument("--vehicle", default="")
    s.add_argument("--program", default="")
    s.add_argument("--captures", action="store_true")
    s.add_argument("--capture-epochs", type=lambda x: x.split(","), default=None,
                   help="capture only these epochs (default: all traced)")
    s.add_argument("--no-formation", action="store_true")
    s.add_argument("--window", default="1280x720")
    s.add_argument("--out")
    s = sp.add_parser("report")
    s.add_argument("run_dir")
    a = ap.parse_args()
    sys.exit({"run": cmd_run, "report": cmd_report}[a.cmd](a) or 0)


if __name__ == "__main__":
    main()
