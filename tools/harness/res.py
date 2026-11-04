#!/usr/bin/env python3
"""Read harness results without dumping them (2026-11-03). Standard library only.

A `perf` result carries a full `state` (tens of KB), a `state` result more: never print a result whole.
This prints one line per command, or only the fields asked for.

    python tools/harness/res.py                         # newest harness_runs/*: one line per command
    python tools/harness/res.py live                    # newest live.py answer (harness_live/outbox)
    python tools/harness/res.py <run dir | results.jsonl | outbox.json | name part>
    python tools/harness/res.py -c perf -c state        # only commands starting with these
    python tools/harness/res.py -c state -g clouds_v2.pass_ms -g weather.cpu_ms
    python tools/harness/res.py -c state -g 'clouds_v2.rain.*'     # * = every key / index at that level
    python tools/harness/res.py -c state --keys clouds_v2           # the shape there: key, type, size
    python tools/harness/res.py --perf [-b cloud_march,sky_background_draw]
    python tools/harness/res.py --ab                    # perf rows grouped by name minus a _rN suffix
    python tools/harness/res.py after --perf --base before   # + the GPU change per matching perf name
    python tools/harness/res.py --errors
    python tools/harness/res.py --summary

Paths address the command's `result`; `@field` addresses the record itself (`@wall_ms`, `@frames`).
`perf name=<view>_<variant>_r<k>` in a script makes --ab group the reps of each view/variant.
"""
import argparse
import fnmatch
import glob
import json
import os
import re
import statistics
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def newest(paths):
    paths = [p for p in paths if os.path.exists(p)]
    return max(paths, key=os.path.getmtime) if paths else None


def resolve(spec):
    """-> (path, records). spec: latest | live | dir | results.jsonl | outbox .json | part of a run name."""
    if spec in (None, "latest"):
        p = newest(glob.glob(os.path.join(REPO, "harness_runs", "*", "results.jsonl")))
    elif spec == "live":
        p = newest(glob.glob(os.path.join(REPO, "harness_live", "outbox", "*.json")))
    elif os.path.isdir(spec):
        p = os.path.join(spec, "results.jsonl")
    elif os.path.isfile(spec):
        p = spec
    else:
        p = newest(glob.glob(os.path.join(REPO, "harness_runs", f"*{spec}*", "results.jsonl")))
    if not p or not os.path.exists(p):
        sys.exit(f"res: no results for '{spec or 'latest'}'")
    if p.endswith(".jsonl"):
        recs = []
        with open(p, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        recs.append(json.loads(line))
                    except json.JSONDecodeError:
                        pass
    else:
        with open(p, encoding="utf-8") as f:
            d = json.load(f)
        recs = d.get("results", d) if isinstance(d, dict) else d
    return p, recs


def get_paths(obj, path):
    """Yield (concrete path, value) for a dotted path with * wildcards."""
    parts = [x for x in path.split(".") if x != ""]

    def walk(o, i, trail):
        if i == len(parts):
            yield ".".join(trail), o
            return
        k = parts[i]
        if "*" in k or "?" in k:
            if isinstance(o, dict):
                for kk, v in o.items():
                    if fnmatch.fnmatchcase(str(kk), k):
                        yield from walk(v, i + 1, trail + [str(kk)])
            elif isinstance(o, list) and k == "*":
                for j, v in enumerate(o):
                    yield from walk(v, i + 1, trail + [str(j)])
            return
        if isinstance(o, dict) and k in o:
            yield from walk(o[k], i + 1, trail + [k])
        elif isinstance(o, list) and re.fullmatch(r"-?\d+", k) and -len(o) <= int(k) < len(o):
            yield from walk(o[int(k)], i + 1, trail + [k])
        else:
            yield ".".join(trail + [k]), "<missing>"

    yield from walk(obj, 0, [])


def short(v, width):
    if isinstance(v, float):
        s = f"{v:.4g}"
    elif isinstance(v, str):
        s = v
    else:
        s = json.dumps(v, separators=(",", ":"), default=str)
    return s if width <= 0 or len(s) <= width else s[:width - 3] + "..."


def shape(v):
    if isinstance(v, dict):
        return f"dict[{len(v)}]"
    if isinstance(v, list):
        return f"list[{len(v)}]"
    return type(v).__name__


def perf_rows(recs):
    rows = []
    for r in recs:
        c = r.get("cmd", "")
        if not c.startswith("perf") or not isinstance(r.get("result"), dict):
            continue
        res = r["result"]
        m = re.search(r"name=(\S+)", c)
        fm = re.search(r"frame ([\d.]+) ms", res.get("message", ""))
        g = res.get("gpu_total_ms") or {}
        rows.append({"name": m.group(1) if m else f"#{r.get('i', len(rows))}",
                     "frame": float(fm.group(1)) if fm else float("nan"),
                     "gpu": g.get("mean", float("nan")), "p90": g.get("p90", float("nan")),
                     "b": res.get("gpu_ms") or {}, "cpu": sum((res.get("cpu_ms") or {}).values())})
    return rows


def pick_buckets(rows, spec, n=6):
    if spec:
        return [b.strip() for b in spec.split(",") if b.strip()]
    tot = {}
    for r in rows:
        for k, v in r["b"].items():
            tot[k] = tot.get(k, 0.0) + v
    return [k for k, _ in sorted(tot.items(), key=lambda kv: -kv[1])[:n]]


ABBR = {"sky_background_draw": "sky", "cloud_march": "cloud", "scene_depth": "depth", "orbit_compute": "orbit",
        "flare_compute": "flare", "satellite_star_draw": "points", "ui_overlay": "ui", "beam_cloud_block": "beam"}


def print_table(header, rows):
    w = [max(len(str(x)) for x in col) for col in zip(header, *rows)]
    for row in [header] + rows:
        print("  ".join(str(x).rjust(w[i]) if i else str(x).ljust(w[i]) for i, x in enumerate(row)))


def main():
    for st in (sys.stdout, sys.stderr):
        try:
            st.reconfigure(encoding="utf-8", errors="replace")
        except AttributeError:
            pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", nargs="?", help="latest (default) | live | run dir | results.jsonl | outbox json | name part")
    ap.add_argument("-c", "--cmd", action="append", help="only commands starting with this (regex, repeatable)")
    ap.add_argument("-g", "--get", action="append", help="dotted path into the result (repeatable; * / alt* = fnmatch on keys)")
    ap.add_argument("--keys", nargs="?", const="", help="list the keys / types / sizes at this path")
    ap.add_argument("--perf", action="store_true", help="table of perf commands")
    ap.add_argument("--ab", action="store_true", help="perf rows grouped by name with --strip removed")
    ap.add_argument("--strip", default=r"[_-]r(ep)?\d+$", help="regex removed from perf names to group (--ab)")
    ap.add_argument("--base", help="--perf / --ab: another run (same spec forms); adds dGPU per matching name")
    ap.add_argument("-b", "--buckets", help="GPU buckets for --perf / --ab (comma separated; default the 6 largest)")
    ap.add_argument("--errors", action="store_true", help="only failed commands")
    ap.add_argument("--summary", action="store_true", help="print summary.json (compact)")
    ap.add_argument("-w", "--width", type=int, default=200, help="truncate values to this many chars (0 = no limit)")
    a = ap.parse_args()

    path, recs = resolve(a.run)
    print(f"# {os.path.relpath(path, REPO)}  ({len(recs)} commands)")
    if a.summary:
        sp = os.path.join(os.path.dirname(path), "summary.json")
        if os.path.exists(sp):
            with open(sp, encoding="utf-8") as f:
                print(short(json.load(f), a.width or 2000))
        else:
            print("(no summary.json)")
        return
    if a.cmd:
        rx = [re.compile(c) for c in a.cmd]
        recs = [r for r in recs if any(x.match(r.get("cmd", "")) for x in rx)]
    if a.errors:
        recs = [r for r in recs if not r.get("ok", True) or r.get("error")]

    if a.perf or a.ab:
        rows = perf_rows(recs)
        if not rows:
            sys.exit("res: no perf results")
        bks = pick_buckets(rows, a.buckets)
        head = ["name", "n", "frame", "gpu", "p90"] + [ABBR.get(b, b) for b in bks] + ["cpu"]
        base = {}
        if a.base:
            _, brecs = resolve(a.base)
            for r in perf_rows(brecs):
                k = re.sub(a.strip, "", r["name"]) if a.ab else r["name"]
                base.setdefault(k, []).append(r["gpu"])
            head.append("dGPU")
        if a.ab:
            groups = {}
            for r in rows:
                groups.setdefault(re.sub(a.strip, "", r["name"]), []).append(r)
            out = []
            for name, g in groups.items():
                gm = [x["gpu"] for x in g]
                out.append([name, len(g), f"{statistics.mean(x['frame'] for x in g):.2f}",
                            f"{statistics.mean(gm):.2f}" + (f" ±{(max(gm) - min(gm)) / 2:.2f}" if len(g) > 1 else ""),
                            f"{statistics.mean(x['p90'] for x in g):.2f}"]
                           + [f"{statistics.mean(x['b'].get(b, 0.0) for x in g):.2f}" for b in bks]
                           + [f"{statistics.mean(x['cpu'] for x in g):.2f}"])
        else:
            out = [[r["name"], 1, f"{r['frame']:.2f}", f"{r['gpu']:.2f}", f"{r['p90']:.2f}"]
                   + [f"{r['b'].get(b, 0.0):.2f}" for b in bks] + [f"{r['cpu']:.2f}"] for r in rows]
        if a.base:
            for row in out:
                b = base.get(row[0])
                g = float(row[3].split(" ")[0])
                row.append(f"{g - statistics.mean(b):+.2f}" if b else "-")
        print_table(head, out)
        return

    for r in recs:
        c = r.get("cmd", "")
        ok = r.get("ok", True) and not r.get("error")
        res = r.get("result")
        if a.keys is not None:
            print(f"[{r.get('i', '?')}] {c[:90]}")
            for p, v in get_paths(res, a.keys):
                if isinstance(v, dict):
                    for k, vv in v.items():
                        print(f"    {k:34s} {shape(vv):12s} {len(json.dumps(vv, default=str)):>8,} B")
                else:
                    print(f"    {p or '(result)'} = {shape(v)} {short(v, a.width)}")
            continue
        if a.get:
            print(f"[{r.get('i', '?')}] {c[:90]}")
            for g in a.get:
                if g.startswith("@"):
                    pairs = get_paths(r, g[1:])
                else:
                    pairs = get_paths(res if isinstance(res, (dict, list)) else {}, g)
                for p, v in pairs:
                    print(f"    {p} = {short(v, a.width)}")
            continue
        if ok:
            msg = res.get("message", "") if isinstance(res, dict) else (res if isinstance(res, str) else "")
            print(f"[{r.get('i', '?')}] ok   {short(c, 70):70s} {short(msg, max(a.width - 80, 40) if a.width else 0)}")
        else:
            print(f"[{r.get('i', '?')}] ERR  {short(c, 70)}\n      -> {short(r.get('error', ''), a.width)}")


if __name__ == "__main__":
    main()
