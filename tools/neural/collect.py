"""Collect the neural-light training set from the real renderer (2026-11-11, branch neural-light).

Views come from the user's own saved places: issue-report shots (user_data/reports, the cloud-ish ones) and perf
snapshots (user_data/perf_profiles/profile_log.jsonl), each also perturbed in time of day, camera and altitude. One
live app (tools/harness/live.py) walks the plan: load the view on the user's current settings, let the bakes refill,
then `ndump write` (the -DCV2_NDUMP march variant: tools/neural/ndump.py for the record) over `frames` full-rate
frames, whose jitter average is the converged target. Each dump is packed to a fp16 .npz and the raw deleted.

    python tools/neural/collect.py plan -o tools/neural/data/plan.jsonl --n 1800
    python tools/neural/collect.py run tools/neural/data/plan.jsonl --out tools/neural/data/views   # app already live
    python tools/neural/collect.py run ... --start --window 1600x900 --settings user_data/settings.json

`run` is resumable: views whose .npz exists are skipped. Stdlib + numpy (through ndump.py).
"""
import argparse, json, os, random, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
LIVE = os.path.join(ROOT, "tools", "harness", "live.py")
PROFILE_LOG = "user_data/perf_profiles/profile_log.jsonl"
CLOUDY = re.compile(r"cloud|storm|hurric|anvil|cumul|cirrus|haze|fog|rain|snow|tornado|weather|shadow|sunset|twilight|horizon|sky", re.I)

def py():
    return sys.executable

def abs_path(rel):
    return os.path.join(ROOT, rel).replace("\\", "/")

# ───────────────────────── plan ─────────────────────────
def sources():
    """-> list of dicts {kind, ref, lat, lon, alt, az, el, utc}."""
    out = []
    rep = os.path.join(ROOT, "user_data", "reports")
    for d in sorted(os.listdir(rep)):
        rj = os.path.join(rep, d, "report.json")
        if not os.path.exists(rj):
            continue
        with open(rj, encoding="utf-8") as f:
            r = json.load(f)
        if not (r.get("category") in ("clouds", "weather", "sky") or CLOUDY.search(r.get("title", "") or "")):
            continue
        for s in r.get("shots", []):
            st = s.get("state") or {}
            ob, cam = st.get("observer", {}), st.get("camera", {})
            if "lat_deg" not in ob:
                continue
            out.append({"kind": "report", "ref": f"user_data/reports/{d}", "shot": int(s.get("n", 1)),
                        "lat": ob["lat_deg"], "lon": ob["lon_deg"], "alt": ob.get("alt_m", ob.get("agl_m", 2.0)),
                        "az": cam.get("az_deg", 0.0), "el": cam.get("el_deg", 10.0), "utc": st.get("time", {}).get("utc"),
                        "sun_el": st.get("sun", {}).get("el_deg")})
    pl = os.path.join(ROOT, PROFILE_LOG)
    if os.path.exists(pl):
        with open(pl, encoding="utf-8") as f:
            for i, line in enumerate(f):
                if not line.strip():
                    continue
                r = json.loads(line)
                ob, v = r.get("observer", {}), r.get("view", {})
                out.append({"kind": "snapshot", "ref": PROFILE_LOG, "index": i,
                            "lat": ob.get("lat_deg"), "lon": ob.get("lon_deg"), "alt": ob.get("height_offset_m", 2.0),
                            "az": v.get("az_deg", 0.0), "el": v.get("el_deg", 10.0), "utc": (r.get("sim_time") or {}).get("utc")})
    return out

def variants(src, rng, per_source):
    """The base view plus perturbations: time of day, camera, altitude. Each is a list of harness commands."""
    vs = [{"tag": "base", "cmds": []}]
    for j in range(per_source - 1):
        cmds, tag = [], []
        r = rng.random()
        if r < 0.75:                     # time: move the Sun (hours, both ways; weather evolves with it)
            dh = rng.choice([-4, -3, -2, -1.5, -1, 1, 1.5, 2, 3, 4, 6, 8, 12])
            cmds.append(f"time add {int(dh * 3600)}"); tag.append(f"t{dh:+g}h")
        if rng.random() < 0.6:           # camera: a turn and a nod
            daz = rng.uniform(-90, 90); de = rng.uniform(-15, 15)
            el = max(-89.0, min(89.0, float(src["el"]) + de))
            cmds.append(f"camera az={float(src['az']) + daz:.1f} el={el:.1f}"); tag.append("cam")
        if rng.random() < 0.35 and src.get("lat") is not None:   # altitude: x0.3 .. x4, inside the shell or above it
            alt = max(2.0, float(src["alt"])) * rng.choice([0.3, 0.5, 2.0, 4.0])
            alt = max(2.0, min(alt, 900e3))
            cmds.append(f"observer lat={src['lat']:.5f} lon={src['lon']:.5f} alt={alt:.0f}"); tag.append(f"alt{alt / 1000:.1f}k")
        if not cmds:
            dh = rng.choice([-2, 2, 6]); cmds.append(f"time add {int(dh * 3600)}"); tag.append(f"t{dh:+g}h")
        vs.append({"tag": "_".join(tag), "cmds": cmds})
    return vs

def cmd_plan(a):
    rng = random.Random(a.seed)
    srcs = sources()
    plan = []
    for k, s in enumerate(srcs):
        base = f"{'r' if s['kind'] == 'report' else 's'}{k:04d}"
        for j, v in enumerate(variants(s, rng, a.per_source)):
            if s["kind"] == "report":
                load = f'report load "{abs_path(s["ref"])}" shot={s["shot"]} settings=off trail=off'   # (the app's cwd is build/Release)
            else:
                load = f"snapshot {abs_path(s['ref'])} index={s['index']} settings=off"
            plan.append({"name": f"{base}_{j}_{v['tag']}", "source": s, "load": load, "cmds": v["cmds"]})
    rng.shuffle(plan)
    if a.n and len(plan) > a.n:
        plan = plan[:a.n]
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as f:
        for p in plan:
            f.write(json.dumps(p) + "\n")
    nr = sum(1 for p in plan if p["source"]["kind"] == "report")
    print(f"{len(srcs)} sources ({sum(1 for s in srcs if s['kind'] == 'report')} report shots), {len(plan)} views planned "
          f"({nr} from reports, {len(plan) - nr} from snapshots) -> {a.out}")

# ───────────────────────── run ─────────────────────────
def live(args, timeout=900):
    r = subprocess.run([py(), LIVE] + args, cwd=ROOT, capture_output=True, text=True, timeout=timeout + 60)
    return r.returncode, r.stdout + r.stderr

def live_status():
    rc, out = live(["status"])
    try:
        return json.loads(out[out.index("{"):])
    except Exception:
        return {"alive": False, "raw": out}

def send_batch(text, out_dir, timeout):
    """Send one inbox file; returns the outbox results list."""
    name = f"nl_{int(time.time() * 1000)}"
    inbox = os.path.join(out_dir, "inbox")
    os.makedirs(inbox, exist_ok=True)
    tmp = os.path.join(inbox, name + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text + "\n")
    os.replace(tmp, os.path.join(inbox, name + ".satcmd"))
    out = os.path.join(out_dir, "outbox", name + ".json")
    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.exists(out):
            try:
                with open(out, encoding="utf-8") as f:
                    return json.load(f)["results"]
            except Exception:
                time.sleep(0.2)
                continue
        time.sleep(0.2)
    return None

def cmd_run(a):
    plan = [json.loads(l) for l in open(a.plan, encoding="utf-8") if l.strip()]
    os.makedirs(a.out, exist_ok=True)
    manifest = os.path.join(a.out, "manifest.jsonl")
    done = set()
    if os.path.exists(manifest):
        for l in open(manifest, encoding="utf-8"):
            if l.strip():
                done.add(json.loads(l)["name"])
    todo = [p for p in plan if p["name"] not in done and not os.path.exists(os.path.join(a.out, p["name"] + ".npz"))]
    print(f"{len(plan)} planned, {len(done)} done, {len(todo)} to go")
    if a.start:
        st = live_status()
        if not st.get("alive"):
            args = ["start"]
            if a.window: args += ["--window", a.window]
            if a.settings: args += ["--settings", a.settings]
            rc, out = live(args, timeout=300)
            print(out.strip())
            if rc != 0:
                sys.exit("live start failed")
    st = live_status()
    if not st.get("alive"):
        sys.exit("no live app (pass --start, or start it: python tools/harness/live.py start --settings user_data/settings.json)")
    live_dir = os.path.join(ROOT, a.live_dir)
    # The variant march + the dump on (idempotent).
    # Half-res clouds for the dumps (a full-res grid is 4x the record), the buffer BEFORE the variant SPV.
    pre = chr(10).join(["set clouds_v2.full_res 0", "wait 5", "ndump on", "shaders reload march=" + a.spv,
                        "set clouds_v2.full_rate_above_km 0"])
    res = send_batch(pre, live_dir, 120)
    for r in res or []:
        print("  ", "ok " if r.get("ok") else "ERR", r.get("cmd"), (r.get("result") or {}).get("message", r.get("error", "")))
    if not res or not all(r.get("ok") for r in res):
        sys.exit("the preamble failed (is the ndump variant SPV built? python tools/harness/shader_variant.py march --tag ndump -D CV2_NDUMP)")
    # Validation sources (train.py's split: the source name's hash) also get `--frames-each` SINGLE-frame dumps, so a
    # net can be judged on its output averaged over frames (what the sky TAA would show) against the converged target.
    import hashlib
    def is_val(name):
        src = name.split("_")[0]
        return int(hashlib.md5(f"{a.val_seed}:{src}".encode()).hexdigest()[:8], 16) / 2**32 < a.val_frac
    frames_dir = os.path.join(a.out, "frames")
    if a.frames_each: os.makedirs(frames_dir, exist_ok=True)
    def pack(path, out):
        pk = subprocess.run([py(), "-I", os.path.join(HERE, "ndump.py"), "pack", path, "-o", out, "--delete"],
                            capture_output=True, text=True)
        if pk.returncode == 0:
            try: os.remove(os.path.splitext(path)[0] + ".json")
            except OSError: pass
        return pk
    t_start = time.time(); n_ok = 0; n_err = 0
    for b in range(0, len(todo), a.batch):
        batch = todo[b:b + a.batch]
        lines = []
        for p in batch:
            lines.append(p["load"])
            lines.append("time pause")
            lines += p["cmds"]
            lines.append(f"wait {a.settle} maxs=60")
            lines.append(f"ndump write {p['name']} frames={a.frames}")
            if a.frames_each and is_val(p["name"]):
                for k in range(a.frames_each):
                    lines.append(f"ndump write {p['name']}_f{k} frames=1")
        res = send_batch(chr(10).join(lines), live_dir, a.timeout)
        if res is None:
            print("no answer from the app; stopping"); break
        # Pair ndump results back to views by name.
        nd = {}
        for r in res:
            c = r.get("cmd") or ""
            if c.startswith("ndump write"):
                nd[c.split()[2]] = r
        errs = [r for r in res if not r.get("ok")]
        for r in errs:
            print("  ERR", r.get("cmd"), "->", r.get("error"))
        for p in batch:
            r = nd.get(p["name"], {})
            rec = {"name": p["name"], "ok": bool(r.get("ok")), "load": p["load"], "cmds": p["cmds"], "source": p["source"]}
            if r.get("ok"):
                path = r["result"]["path"]
                rec["mean_visits"] = r["result"].get("mean_visits")
                pk = pack(path, os.path.join(a.out, p["name"] + ".npz"))
                if pk.returncode != 0:
                    rec["ok"] = False; rec["error"] = "pack: " + pk.stderr.strip()[-300:]
                nf = 0
                for k in range(a.frames_each if is_val(p["name"]) else 0):
                    rf = nd.get(f"{p['name']}_f{k}", {})
                    if rf.get("ok") and pack(rf["result"]["path"], os.path.join(frames_dir, f"{p['name']}_f{k}.npz")).returncode == 0:
                        nf += 1
                if nf: rec["frames"] = nf
            else:
                rec["error"] = r.get("error", "no result")
            n_ok += rec["ok"]; n_err += (not rec["ok"])
            with open(manifest, "a", encoding="utf-8") as f:
                f.write(json.dumps(rec) + "\n")
        el = time.time() - t_start
        print(f"[{b + len(batch)}/{len(todo)}] ok {n_ok} err {n_err}  {el / 60:.1f} min, {el / max(n_ok + n_err, 1):.1f} s/view", flush=True)
        if a.max_minutes and el > a.max_minutes * 60:
            print("time budget reached"); break
    if a.stop:
        live(["stop"])

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = p.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("plan"); s.add_argument("-o", "--out", default="tools/neural/data/plan.jsonl")
    s.add_argument("--n", type=int, default=1800); s.add_argument("--per-source", type=int, default=3); s.add_argument("--seed", type=int, default=1)
    s.set_defaults(fn=cmd_plan)
    s = sp.add_parser("run"); s.add_argument("plan"); s.add_argument("--out", default="tools/neural/data/views")
    s.add_argument("--live-dir", default="harness_live"); s.add_argument("--spv", default="shaders/v_ndump_march.spv")
    s.add_argument("--frames", type=int, default=32); s.add_argument("--settle", type=int, default=100)
    s.add_argument("--batch", type=int, default=5); s.add_argument("--timeout", type=int, default=900)
    s.add_argument("--start", action="store_true"); s.add_argument("--window", default="1280x720"); s.add_argument("--settings", default="user_data/settings.json")
    s.add_argument("--stop", action="store_true"); s.add_argument("--max-minutes", type=float, default=0)
    s.add_argument("--frames-each", type=int, default=0, help="extra single-frame dumps per VALIDATION view (-> <out>/frames/)")
    s.add_argument("--val-frac", type=float, default=0.1); s.add_argument("--val-seed", type=int, default=0)
    s.set_defaults(fn=cmd_run)
    a = p.parse_args()
    a.fn(a)

if __name__ == "__main__":
    main()
