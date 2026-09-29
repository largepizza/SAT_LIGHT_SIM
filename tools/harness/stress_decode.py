"""Freeze reproducer without the app: decode the launch's textures in a loop (docs/FREEZES.md).

Every boot freeze on record stops in the texture step, most often on
`init: texture: assets/textures/earth_elevation.png`, and pinning the GPU at P0 (NVIDIA "Prefer
maximum performance") did not stop them. That step starts with a pure CPU + RAM burst: a 19 MB
PNG inflated into a ~322 MB RGB buffer, reduced to a 107 MB single-channel copy, then copied again
into a staging buffer. This script repeats that burst with no Vulkan, no GPU work and no app:

  * it freezes the machine  -> the platform (RAM / CPU / firmware / a kernel driver) is the cause,
                               and the app is only one way to trigger it;
  * it survives many cycles -> the GPU upload side of the launch is back in play.

It is also a fast re-test after each hardware change (a kit pulled, BIOS updated), instead of
spending harness launches.

Differences from the app, stated so a clean run is read correctly: the app decodes with stb_image
(`stbi_load(..., 1)`, single-threaded); PIL uses libpng + zlib. The buffers are the same size and
the same shape of allocate -> fill -> copy -> free. Nothing here touches the GPU.

Each cycle appends one fsynced line to harness_runs/stress_decode/<start>.log, so after a freeze
the last line says which cycle and which phase it died in (compare with the Kernel-Power 41 time).
Run the recorder alongside for machine-wide telemetry:
    python tools/harness/blackbox.py --daemon --gpu-ms 100

Usage:
    python tools/harness/stress_decode.py                  # the DEM only, 2 s idle between cycles
    python tools/harness/stress_decode.py --set launch     # all nine textures in the app's order
    python tools/harness/stress_decode.py --gap 0          # back-to-back, no idle between cycles
    python tools/harness/stress_decode.py --threads 4      # four decoders at once (harder than the app)
    python tools/harness/stress_decode.py --minutes 60     # stop after an hour (default: 30 min)
"""
import argparse
import os
import sys
import threading
import time

try:
    from PIL import Image
except ImportError:
    sys.exit("stress_decode.py needs Pillow: python -m pip install pillow")

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TEX = os.path.join(REPO, "assets", "textures")
OUT_DIR = os.path.join(REPO, "harness_runs", "stress_decode")

# (file, channels the app asks stb_image for) in SatelliteSim::init's order.
LAUNCH_SET = [
    ("../noise/rgba_noise.png", 4),
    ("full_moon.png", 4),
    ("8k_earth_daymap.jpg", 4),
    ("8k_stars_milky_way.jpg", 4),
    ("8k_earth_nightmap.jpg", 4),
    ("city_day_detail.png", 4),
    ("city_night_detail.png", 4),
    ("earth_elevation.png", 1),
    ("8k_earth_specular_map.png", 1),
    ("8k_earth_clouds.jpg", 1),
]
DEM_SET = [("earth_elevation.png", 1)]

Image.MAX_IMAGE_PIXELS = None  # the DEM is 112 Mpx, past PIL's decompression-bomb guard


class Log:
    """One fsynced line per event, so a line that reached the file survives a hard reset."""

    def __init__(self, path):
        self.f = open(path, "a", encoding="utf-8")
        self.lock = threading.Lock()

    def line(self, text):
        stamp = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime()) + f".{int(time.time() * 1000) % 1000:03d}Z"
        with self.lock:
            self.f.write(f"[{stamp}] {text}\n")
            self.f.flush()
            os.fsync(self.f.fileno())
        print(f"[{stamp}] {text}", flush=True)


def burst(path, comp):
    """The app's per-texture CPU work: decode, reduce to `comp` channels, copy into a staging
    buffer, and for the DEM the two CPU scans that follow (downsample + full-resolution pass)."""
    with Image.open(path) as im:
        im.load()                                   # inflate: the big allocation
        im = im.convert("L" if comp == 1 else "RGBA")
    pixels = im.tobytes()                           # stbi_load's returned buffer
    staging = bytearray(pixels)                     # memcpy into the mapped staging buffer
    w, h = im.size
    if comp == 1 and w * h > 50_000_000:
        # earthElevCpu (2160x1080 point sample) and a full pass like the ocean-mask scan
        _ = bytes(pixels[(y * h // 1080) * w + (x * w // 2160)] for y in range(0, 1080, 8) for x in range(2160))
        _ = pixels.count(15)
    n = len(staging)
    del staging, pixels, im
    return n


def run_set(tex_set, log, cycle, worker):
    for rel, comp in tex_set:
        path = os.path.normpath(os.path.join(TEX, rel))
        name = os.path.basename(path)
        log.line(f"cycle {cycle} w{worker} start {name}")
        t0 = time.perf_counter()
        n = burst(path, comp)
        log.line(f"cycle {cycle} w{worker} done  {name} {n / 1e6:.0f} MB {1000 * (time.perf_counter() - t0):.0f} ms")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--set", choices=["dem", "launch"], default="dem",
                    help="dem = earth_elevation.png only; launch = all textures in init order")
    ap.add_argument("--gap", type=float, default=2.0, help="idle seconds between cycles (default 2)")
    ap.add_argument("--threads", type=int, default=1, help="decoders running at once (the app uses 1)")
    ap.add_argument("--minutes", type=float, default=30.0, help="stop after this long (default 30)")
    ap.add_argument("--cycles", type=int, default=0, help="stop after this many cycles (0 = no limit)")
    args = ap.parse_args()

    tex_set = DEM_SET if args.set == "dem" else LAUNCH_SET
    for rel, _ in tex_set:
        p = os.path.normpath(os.path.join(TEX, rel))
        if not os.path.isfile(p):
            sys.exit(f"missing texture: {p}")

    os.makedirs(OUT_DIR, exist_ok=True)
    log_path = os.path.join(OUT_DIR, time.strftime("%Y%m%d_%H%M%S") + ".log")
    log = Log(log_path)
    log.line(f"stress_decode start: set={args.set} gap={args.gap}s threads={args.threads} "
             f"minutes={args.minutes} cycles={args.cycles or 'unlimited'} pid={os.getpid()}")
    print(f"log: {log_path}", flush=True)

    deadline = time.time() + args.minutes * 60.0
    cycle = 0
    try:
        while time.time() < deadline and (args.cycles == 0 or cycle < args.cycles):
            cycle += 1
            t0 = time.perf_counter()
            if args.threads == 1:
                run_set(tex_set, log, cycle, 0)
            else:
                ts = [threading.Thread(target=run_set, args=(tex_set, log, cycle, i)) for i in range(args.threads)]
                for t in ts:
                    t.start()
                for t in ts:
                    t.join()
            log.line(f"cycle {cycle} complete {1000 * (time.perf_counter() - t0):.0f} ms; idle {args.gap:g} s")
            if args.gap > 0:
                time.sleep(args.gap)
    except KeyboardInterrupt:
        log.line(f"stopped by user after {cycle} cycles")
        return
    log.line(f"stress_decode finished cleanly: {cycle} cycles")


if __name__ == "__main__":
    main()
