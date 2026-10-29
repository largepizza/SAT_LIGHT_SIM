"""Bake the shipped Sun's Fury: data/kessler/suns_fury_<hash>.pak (CLAUDE.md "Kessler syndrome" > The prebake).

Runs the app through the harness with the built-in default settings (the shipped roster and the default Sun's Fury
settings: the world hash the app computes at launch), lets the engine simulate DAYS days past the CME and writes the
compact event log with `kessler savecache out=data/kessler`. Any other suns_fury_*.pak in data/kessler is removed (a
roster, a model mass, a Kessler setting or kCacheVersion changed the hash: the old file can never match again).

    python tools/make_kessler_prebake.py [--days 1100] [--exe build/Release/SAT_LIGHT_SIM_V_*.exe]

Re-run it after any change that moves the world hash (the app logs "kessler: no cache suns_fury_<hash>.pak - simulating
from the storm" when the shipped file does not match). One run is ~20-40 min (the engine sampling the cascade); the
harness's launch spacing applies (tools/harness/launchgate.py).
"""
import argparse
import glob
import json
import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--days", type=float, default=1100.0, help="days past the CME to bake (default 1100: ~3 years)")
    ap.add_argument("--exe", help="the app to run (default: run.py's choice, build/Release)")
    ap.add_argument("--out", help="harness run folder (default harness_runs/<stamp>_kessler_prebake)")
    a = ap.parse_args()

    dest = os.path.join(REPO, "data", "kessler")
    os.makedirs(dest, exist_ok=True)
    before = set(glob.glob(os.path.join(dest, "suns_fury_*.pak")))
    # Sim time at the bake's end (held there until the cascade arrives): the engine then takes every core, and the
    # mirror keeps no history ahead of "now" (at the default start it would keep every generation of every slot).
    cme = 1184511600.0  # 2037-07-15 03:00 UTC, J2000 s (SatelliteSimKessler.cpp kFuryArriveT)
    script = "\n".join([
        "# made by tools/make_kessler_prebake.py",
        "wait 30",
        "time pause",
        f"time j2000 {cme + a.days * 86400.0:.0f}",
        f"kessler wait days={a.days:g}",
        "kessler savecache out=" + dest.replace("\\", "/"),
        "",
    ])
    fd, path = tempfile.mkstemp(suffix=".satcmd", prefix="kessler_prebake_")
    with os.fdopen(fd, "w") as f:
        f.write(script)
    cmd = [sys.executable, os.path.join(REPO, "tools", "harness", "run.py"), path,
           "--timeout", str(int(6 * 3600)), "--quiet"]
    if a.exe:
        cmd += ["--exe", a.exe]
    out = a.out or os.path.join(REPO, "harness_runs", "kessler_prebake")
    cmd += ["--out", out]
    r = subprocess.run(cmd)
    os.unlink(path)
    if r.returncode != 0:
        print("make_kessler_prebake: the harness run failed", file=sys.stderr)
        return 1
    written = None
    with open(os.path.join(out, "results.jsonl")) as f:
        for line in f:
            j = json.loads(line)
            if j["cmd"].startswith("kessler savecache"):
                if not j.get("ok"):
                    print("make_kessler_prebake: savecache failed: " + j.get("error", "?"), file=sys.stderr)
                    return 1
                written = j["result"]["path"]
    if not written:
        print("make_kessler_prebake: no savecache result", file=sys.stderr)
        return 1
    written = os.path.abspath(written)
    for old in before:
        if os.path.abspath(old) != written:
            os.remove(old)
            print("removed " + os.path.relpath(old, REPO))
    print("wrote %s (%.1f MB)" % (os.path.relpath(written, REPO), os.path.getsize(written) / 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main())
