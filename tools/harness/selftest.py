#!/usr/bin/env python3
"""The harness's own test suite (docs/HARNESS.md "Verifying the harness"). Standard library only.

    python tools/harness/selftest.py            # ~2 min, launches the Release app ~12 times
    python tools/harness/selftest.py -k live    # only tests whose name contains "live"

Each test runs real scripts through run.py / live.py and checks the run folder: results, summary,
capture sizes, sidecars, determinism (bit-identical PNG bytes), error reporting, the watchdog.
Run it after changing src/Harness.* or SatelliteSimHarness.cpp.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(REPO, "harness_runs", "selftest")
PY = sys.executable


def run(name, commands, extra=(), expect_ok=True):
    out = os.path.join(OUT, name)
    shutil.rmtree(out, ignore_errors=True)
    script = os.path.join(OUT, name + ".satcmd")
    os.makedirs(OUT, exist_ok=True)
    with open(script, "w") as f:
        f.write(commands.strip() + "\n")
    p = subprocess.run([PY, os.path.join(HERE, "run.py"), script, "--out", out, "--quiet", *extra],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    res = []
    rp = os.path.join(out, "results.jsonl")
    if os.path.exists(rp):
        res = [json.loads(l) for l in open(rp, encoding="utf-8") if l.strip()]
    summ = json.load(open(os.path.join(out, "summary.json"))) if os.path.exists(os.path.join(out, "summary.json")) else {}
    if expect_ok and summ.get("status") != "ok":
        raise AssertionError(f"run {name} status {summ.get('status')}:\n{p.stdout[-2000:]}")
    return out, res, summ


def png_size(p):
    with open(p, "rb") as f:
        b = f.read(24)
    return int.from_bytes(b[16:20], "big"), int.from_bytes(b[20:24], "big")


def png_pixels_crc(p):
    """CRC of the decompressed IDAT stream: identical pixels <=> identical value (no Pillow needed)."""
    data = open(p, "rb").read()
    i, idat = 8, b""
    while i < len(data):
        n = int.from_bytes(data[i:i + 4], "big")
        if data[i + 4:i + 8] == b"IDAT":
            idat += data[i + 8:i + 8 + n]
        i += 12 + n
    return zlib.crc32(zlib.decompress(idat))


def result(res, cmd_prefix):
    for r in res:
        if r["cmd"].startswith(cmd_prefix):
            return r
    raise AssertionError(f"no result for '{cmd_prefix}'")


SCENE = """
preset Medium
observer lat=46.55 lon=7.98 agl=50
time sun 12 rising
time pause
camera az=200 el=5 fov=60
wait settle 20
"""

TESTS = {}


def test(fn):
    TESTS[fn.__name__] = fn
    return fn


@test
def capture_sizes_and_sidecar():
    out, res, _ = run("capture", SCENE + """
capture full
capture half scale=0.5
capture zoom crop=100,200,50,40 scale=4
capture withui ui=on scale=0.25
""")
    cap = os.path.join(out, "captures")
    assert png_size(os.path.join(cap, "full.png")) == (1600, 900)
    assert png_size(os.path.join(cap, "half.png")) == (800, 450)
    assert png_size(os.path.join(cap, "zoom.png")) == (200, 160)
    assert png_size(os.path.join(cap, "withui.png")) == (400, 225)
    side = json.load(open(os.path.join(cap, "full.json")))
    assert abs(side["state"]["observer"]["lat_deg"] - 46.55) < 1e-3, side["state"]["observer"]
    assert side["state"]["render"]["preset"] == "Medium"


@test
def determinism():
    a, _, _ = run("det_a", SCENE + "capture x")
    b, _, _ = run("det_b", SCENE + "capture x")
    ca, cb = png_pixels_crc(os.path.join(a, "captures", "x.png")), png_pixels_crc(os.path.join(b, "captures", "x.png"))
    assert ca == cb, "two runs of the same script gave different pixels"


@test
def knockout_changes_image():
    out, res, _ = run("knockout", SCENE + """
capture base
knockout +terrain_march
wait settle 5
capture noterrain
knockout none
wait settle 5
capture back
""")
    cap = os.path.join(out, "captures")
    base, nt, back = (png_pixels_crc(os.path.join(cap, n + ".png")) for n in ("base", "noterrain", "back"))
    assert base != nt, "terrain knockout changed nothing"
    assert base == back, "clearing the knockout did not restore the image"
    assert result(res, "knockout +terrain")["result"]["knockout_mask"] == 1


@test
def settings_roundtrip_and_errors():
    out, res, summ = run("settings", """
set clouds.coverage 0.2
get clouds.coverage
set photometry.extinction_coeff=0.25 clouds.density=0.5
set clouds.not_a_key 3
set clouds.coverage banana
get nope.nope
frobnicate
""", expect_ok=False)
    assert summ["status"] == "errors" and summ["errors"] == 4, summ
    assert abs(result(res, "get clouds.coverage")["result"]["value"] - 0.2) < 1e-6
    r = result(res, "set photometry.extinction_coeff")["result"]
    assert abs(r["photometry.extinction_coeff"] - 0.25) < 1e-6 and abs(r["clouds.density"] - 0.5) < 1e-6
    assert "no setting" in result(res, "set clouds.not_a_key")["error"]
    assert "is a number" in result(res, "set clouds.coverage banana")["error"]
    assert "unknown command" in result(res, "frobnicate")["error"]


@test
def parse_error_is_reported():
    out, res, summ = run("parse", 'log "unterminated\ncapture x', expect_ok=False)
    assert summ.get("status") == "script_error" and "unterminated" in summ.get("message", ""), summ


@test
def settle_holds_time():
    out, res, _ = run("settle", """
time play
time set 2036-06-21T12:00:00Z
wait settle 30
state
time pause
""")
    t = result(res, "state")["result"]["time"]["j2000_s"]
    t0 = result(res, "time set")["result"]["j2000_s"]
    assert abs(t - t0) < 0.05, f"settle moved time by {t - t0} s"


@test
def time_sun_finds_elevation():
    out, res, _ = run("sun", """
observer lat=-33.9 lon=18.4
time sun -6 setting
state a
time sun 30 rising
state b
""")
    a = result(res, "state a")["result"]["sun"]["el_deg"]
    b = result(res, "state b")["result"]["sun"]["el_deg"]
    assert abs(a + 6) < 0.1 and abs(b - 30) < 0.1, (a, b)


@test
def perf_and_sweep_numbers():
    out, res, _ = run("perf", SCENE + "perf frames=20 name=selftest\nsweep", extra=("--timeout", "240"))
    p = result(res, "perf")["result"]
    assert p["gpu_total_ms"]["mean"] > 0 and p["frames"] == 20, p
    s = result(res, "sweep")["result"]["knockout_sweep"]
    assert len(s["steps"]) >= 10 and s["baseline"]["total"] > 0
    log = os.path.join(out, "perf_profiles", "profile_log.jsonl")
    kinds = [json.loads(l)["record_kind"] for l in open(log)]
    assert kinds == ["harness_sample", "knockout_sweep"], kinds


@test
def selection_follow_ui_dump():
    out, res, _ = run("select", SCENE + """
select const "Starlink Gen1"
camera look sel
follow
wait settle 10
capture follow
follow off
select planet Jupiter
ui open settings tab=Photometry
wait 3
ui dump settings
""")
    assert result(res, "select const")["result"]["constellation"] == "Starlink Gen1"
    d = json.load(open(os.path.join(out, "captures", "settings.layout.json")))
    assert len(d["items"]) > 50 and any(i.get("text") == "Photometry" for i in d["items"])


@test
def watchdog_timeout():
    out, res, summ = run("timeout", "wait seconds 120", extra=("--timeout", "8"), expect_ok=False)
    assert summ.get("status") == "timeout", summ


@test
def live_mode():
    d = os.path.join(OUT, "live")
    shutil.rmtree(d, ignore_errors=True)
    live = os.path.join(HERE, "live.py")
    subprocess.run([PY, live, "--dir", d, "start"], check=True, stdout=subprocess.PIPE)
    try:
        p = subprocess.run([PY, live, "--dir", d, "send", "observer lat=10 lon=20; state", "--json"],
                           stdout=subprocess.PIPE, text=True)
        assert p.returncode == 0, p.stdout
        p2 = subprocess.run([PY, live, "--dir", d, "send", "bogus"], stdout=subprocess.PIPE, text=True)
        assert p2.returncode == 1 and "unknown command" in p2.stdout, p2.stdout
    finally:
        subprocess.run([PY, live, "--dir", d, "stop"], stdout=subprocess.PIPE)
    time.sleep(2)
    assert not os.path.exists(os.path.join(d, "status.json")), "status.json left behind after quit"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-k", default="")
    a = ap.parse_args()
    passed = failed = 0
    for name, fn in TESTS.items():
        if a.k not in name:
            continue
        t0 = time.time()
        try:
            fn()
            passed += 1
            print(f"PASS {name} ({time.time() - t0:.1f}s)", flush=True)
        except Exception as e:  # noqa: BLE001 - report every failure, keep going
            failed += 1
            print(f"FAIL {name} ({time.time() - t0:.1f}s): {e}", flush=True)
    print(f"{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
