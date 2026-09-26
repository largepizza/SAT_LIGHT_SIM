#!/usr/bin/env python3
"""Flight recorder for machine-level resets (docs/HARNESS.md, "Machine-level resets").

    python tools/harness/blackbox.py --out bb.csv -- cmake --build build --target accuracy-gate
    python tools/harness/blackbox.py --daemon            # always on: harness_runs/blackbox/<day>.csv
    python tools/harness/blackbox.py --summary harness_runs/<run>/blackbox.csv
    python tools/harness/blackbox.py --summary harness_runs/blackbox --at "2026-09-25 21:57:53"

When this machine resets, nothing that was in memory survives and Windows writes no dump, so the
only record of what the hardware was doing is one that was already on disk. This samples the GPU
(nvidia-smi, every --gpu-ms) and the CPU (Windows perf counters, every second) and appends one line
per sample, fsynced, so the file ends at the last sample before the power went. run.py records one
into every harness run folder, starting before the launch so the idle -> load step is in it.

Lines are CSV: epoch seconds, UTC ISO time, source, then the source's fields (a '#' line names them).
  gpu   pstate, power W (avg), power W (instant), graphics MHz, memory MHz, temp C, util %,
        PCIe gen, PCIe width, clock event reasons (bitmask), fan %, VRAM MiB
  cpu   % processor performance (turbo > 100), % processor utility, MHz, ACPI thermal zone K
  mark  free text (launch, exit code, ...)
Standard library only. Read-only towards the machine: it queries, it never sets anything.
"""
import argparse
import csv
import glob
import io
import os
import subprocess
import sys
import threading
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DAEMON_DIR = os.path.join(REPO, "harness_runs", "blackbox")

GPU_FIELDS = ["pstate", "power.draw", "power.draw.instant", "clocks.gr", "clocks.mem", "temperature.gpu",
              "utilization.gpu", "pcie.link.gen.current", "pcie.link.width.current",
              "clocks_event_reasons.active", "fan.speed", "memory.used"]
GPU_NAMES = ["pstate", "gpu_w", "gpu_w_inst", "gr_mhz", "mem_mhz", "gpu_c", "gpu_util", "pcie_gen",
             "pcie_width", "clk_reasons", "fan", "vram_mib"]
CPU_COUNTERS = [r"\Processor Information(_Total)\% Processor Performance",
                r"\Processor Information(_Total)\% Processor Utility",
                r"\Processor Information(_Total)\Processor Frequency",
                r"\Thermal Zone Information(*)\Temperature"]
CPU_NAMES = ["cpu_perf", "cpu_util", "cpu_mhz", "tz_k"]

# nvidia-smi clock event reasons (nvml.h, nvmlClocksEventReason*): which limiter is active.
CLK_REASONS = {0x1: "idle", 0x2: "app-clocks", 0x4: "sw-power-cap", 0x8: "hw-slowdown",
               0x10: "sync-boost", 0x20: "sw-thermal", 0x40: "hw-thermal", 0x80: "hw-power-brake",
               0x100: "display-clocks"}


def _utc(t):
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(t)) + ".%03dZ" % int((t % 1) * 1000)


class BlackBox:
    """Samples until stop(). Every line is flushed and fsynced before the next is taken: a sample
    that only reached the page cache is lost with the machine, which is the one case this is for."""

    def __init__(self, path, gpu_ms=100, cpu=True):
        self.path, self.gpu_ms, self.cpu = path, gpu_ms, cpu
        self.lock = threading.Lock()
        self.procs, self.threads = [], []
        self.f = None
        self.day = None

    def _open(self, path):
        os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
        new = not os.path.exists(path) or os.path.getsize(path) == 0
        f = open(path, "a", encoding="utf-8", newline="")
        if new:
            f.write("# blackbox.py: fsynced per line; epoch,utc,src,fields\n")
            f.write("# gpu," + ",".join(GPU_NAMES) + "\n# cpu," + ",".join(CPU_NAMES) + "\n")
        return f

    def write(self, src, fields):
        t = time.time()
        line = "%.3f,%s,%s,%s\n" % (t, _utc(t), src, ",".join(str(x).strip() for x in fields))
        with self.lock:
            if self.f is None:
                return
            if self.path is None:  # daemon: one file per UTC day
                day = time.strftime("%Y%m%d", time.gmtime(t))
                if day != self.day:
                    self.f.close()
                    self.day = day
                    self.f = self._open(os.path.join(DAEMON_DIR, day + ".csv"))
            self.f.write(line)
            self.f.flush()
            try:
                os.fsync(self.f.fileno())
            except OSError:
                pass

    def mark(self, text):
        self.write("mark", [str(text).replace(",", ";").replace("\n", " ")])

    def _pump(self, proc, src, parse):
        for raw in proc.stdout:
            vals = parse(raw)
            if vals:
                self.write(src, vals)

    def start(self):
        if self.path is None:
            self.day = time.strftime("%Y%m%d", time.gmtime())
            self.f = self._open(os.path.join(DAEMON_DIR, self.day + ".csv"))
        else:
            self.f = self._open(self.path)
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        try:
            p = subprocess.Popen(["nvidia-smi", "--query-gpu=" + ",".join(GPU_FIELDS),
                                  "--format=csv,noheader,nounits", "-lms", str(self.gpu_ms)],
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                                 errors="replace", creationflags=flags)
            self.procs.append(p)
            self._thread(p, "gpu", lambda l: [x.strip() for x in l.split(",")] if l.count(",") >= 5 else None)
        except OSError as e:
            self.mark("no nvidia-smi: %s" % e)
        if self.cpu:
            try:
                p = subprocess.Popen(["typeperf"] + CPU_COUNTERS + ["-si", "1"], stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, errors="replace", creationflags=flags)
                self.procs.append(p)
                self._thread(p, "cpu", _parse_typeperf)
            except OSError as e:
                self.mark("no typeperf: %s" % e)
        self.mark("blackbox start pid %d gpu_ms %d" % (os.getpid(), self.gpu_ms))
        return self

    def _thread(self, p, src, parse):
        t = threading.Thread(target=self._pump, args=(p, src, parse), daemon=True)
        t.start()
        self.threads.append(t)

    def stop(self):
        self.mark("blackbox stop")
        for p in self.procs:
            try:
                p.terminate()
            except OSError:
                pass
        for p in self.procs:
            try:
                p.wait(timeout=3)
            except Exception:
                pass
        with self.lock:
            if self.f:
                self.f.close()
                self.f = None


def _parse_typeperf(raw):
    raw = raw.strip()
    if not raw.startswith('"') or "PDH-CSV" in raw or "\\\\" in raw:
        return None
    row = next(csv.reader(io.StringIO(raw)), [])
    if len(row) < 2:
        return None
    vals = row[1:]
    return vals if all(v.strip() for v in vals) else None


# ── reading it back ──

def read(path):
    """[(epoch, src, [fields])] from one file or every .csv in a folder, in time order."""
    files = sorted(glob.glob(os.path.join(path, "*.csv"))) if os.path.isdir(path) else [path]
    out = []
    for fp in files:
        try:
            with open(fp, "rb") as fh:
                data = fh.read()
        except OSError:
            continue
        for line in data.decode("utf-8", "replace").splitlines():
            line = line.strip("\x00").strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",")
            try:
                out.append((float(parts[0]), parts[2], parts[3:]))
            except (ValueError, IndexError):
                pass
    out.sort(key=lambda r: r[0])
    return out


def _f(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def _reasons(v):
    try:
        m = int(v, 16)
    except (TypeError, ValueError):
        return v
    names = [n for b, n in sorted(CLK_REASONS.items()) if m & b]
    return "+".join(names) if names else "none"


def summarize(path, at=None, seconds=20):
    """Readable lines: the last `seconds` of samples before `at` (epoch; default: the file's end)."""
    rows = read(path)
    if at is not None:
        rows = [r for r in rows if r[0] <= at + 1]
    if not rows:
        return ["blackbox: no samples%s in %s" % (" before that time" if at else "", path)]
    end = rows[-1][0]
    win = [r for r in rows if r[0] >= end - seconds]
    gpu = [(t, dict(zip(GPU_NAMES, f))) for t, s, f in win if s == "gpu"]
    cpu = [(t, dict(zip(CPU_NAMES, f))) for t, s, f in win if s == "cpu"]
    marks = [(t, f[0] if f else "") for t, s, f in win if s == "mark"]
    L = ["blackbox: last sample %s UTC%s" % (_utc(end), "" if at is None else
                                             " (%.1f s before the given time)" % (at - end)),
         "  window: the final %g s, %d GPU + %d CPU samples" % (seconds, len(gpu), len(cpu))]
    if gpu:
        g = gpu[-1][1]
        L.append("  GPU at the end: %s %s W (inst %s) gr %s MHz mem %s MHz %s C util %s%% PCIe gen%s x%s "
                 "limiter %s" % (g.get("pstate"), g.get("gpu_w"), g.get("gpu_w_inst"), g.get("gr_mhz"),
                                 g.get("mem_mhz"), g.get("gpu_c"), g.get("gpu_util"), g.get("pcie_gen"),
                                 g.get("pcie_width"), _reasons(g.get("clk_reasons"))))
        pw = [(_f(d.get("gpu_w_inst")) or _f(d.get("gpu_w")) or 0, t) for t, d in gpu]
        mx = max(pw)
        L.append("  GPU peak power in window: %.1f W at %s (%.1f s before the end)"
                 % (mx[0], _utc(mx[1]), end - mx[1]))
        L.append("  GPU peak temperature: %s C" % max((_f(d.get("gpu_c")) or 0) for t, d in gpu))
        prev = None
        for t, d in gpu:  # every P-state / PCIe link change: the transitions the resets sit on
            k = (d.get("pstate"), d.get("pcie_gen"), d.get("pcie_width"))
            if prev is not None and k != prev:
                L.append("  %s  (-%.2f s)  %s gen%s x%s -> %s gen%s x%s"
                         % (_utc(t)[11:], end - t, prev[0], prev[1], prev[2], k[0], k[1], k[2]))
            prev = k
    if cpu:
        c = cpu[-1][1]
        L.append("  CPU at the end: perf %.0f%% util %.0f%% %.0f MHz, thermal zone %.0f K"
                 % tuple((_f(c.get(k)) or 0) for k in CPU_NAMES))
        L.append("  CPU peak utility in window: %.0f%%" % max((_f(d.get("cpu_util")) or 0) for t, d in cpu))
    for t, m in marks:
        L.append("  mark %s  (-%.2f s)  %s" % (_utc(t)[11:], end - t, m))
    return L


def prune(days=14):
    """Daemon files older than `days` go, so an always-on recorder stays bounded (~0.1 GB/day)."""
    for fp in glob.glob(os.path.join(DAEMON_DIR, "*.csv")):
        try:
            if time.time() - os.path.getmtime(fp) > days * 86400:
                os.remove(fp)
        except OSError:
            pass


def _wait_pid(pid):
    """Block until process `pid` exits (Windows: a SYNCHRONIZE handle; elsewhere: poll)."""
    if os.name == "nt":
        import ctypes
        k32 = ctypes.windll.kernel32
        h = k32.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
        if not h:
            return
        try:
            k32.WaitForSingleObject(h, 0xFFFFFFFF)
        finally:
            k32.CloseHandle(h)
        return
    while True:
        try:
            os.kill(pid, 0)
        except OSError:
            return
        time.sleep(1.0)


def _local_epoch(s):
    """'2026-09-25 21:57:53' in this machine's local time (the Windows event list's clock)."""
    return time.mktime(time.strptime(s.replace("T", " ")[:19], "%Y-%m-%d %H:%M:%S"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", help="file to append to (default with a command: blackbox_<stamp>.csv)")
    ap.add_argument("--daemon", action="store_true", help="run until stopped, one file per day in harness_runs/blackbox")
    ap.add_argument("--gpu-ms", type=int, default=None, help="GPU sample period (default 100; daemon 250)")
    ap.add_argument("--summary", metavar="PATH", help="summarize a recording (file or folder)")
    ap.add_argument("--at", help="with --summary: local time of a reset, 'YYYY-MM-DD HH:MM:SS'")
    ap.add_argument("--seconds", type=float, default=20, help="with --summary: window length (default 20)")
    ap.add_argument("--until-pid", type=int, help="with --out: record until this process exits (live.py)")
    ap.add_argument("command", nargs=argparse.REMAINDER, help="-- command to run while recording")
    a = ap.parse_args()

    if a.summary:
        print("\n".join(summarize(a.summary, _local_epoch(a.at) if a.at else None, a.seconds)))
        return 0

    if a.until_pid and a.out:
        bb = BlackBox(a.out, a.gpu_ms or 100).start()
        bb.mark("watching pid %d" % a.until_pid)
        try:
            _wait_pid(a.until_pid)
        except KeyboardInterrupt:
            pass
        bb.mark("pid %d exited" % a.until_pid)
        bb.stop()
        return 0

    cmd = a.command[1:] if a.command[:1] == ["--"] else a.command
    if a.daemon:
        prune()
        bb = BlackBox(None, a.gpu_ms or 250).start()
        print("recording to %s (Ctrl-C to stop)" % DAEMON_DIR, flush=True)
        try:
            while True:
                time.sleep(3600)
                prune()
        except KeyboardInterrupt:
            pass
        bb.stop()
        return 0
    if not cmd:
        ap.error("give --daemon, --summary, or -- <command>")
    out = a.out or os.path.join(REPO, "harness_runs", "blackbox_%s.csv" % time.strftime("%Y%m%d_%H%M%S"))
    bb = BlackBox(out, a.gpu_ms or 100).start()
    time.sleep(1.0)  # an idle baseline before the load arrives
    bb.mark("launch " + " ".join(cmd))
    rc = 1
    try:
        rc = subprocess.call(cmd)
    except KeyboardInterrupt:
        rc = 130
    finally:
        bb.mark("exit %s" % rc)
        time.sleep(0.5)
        bb.stop()
    print("blackbox: " + out)
    return rc


if __name__ == "__main__":
    sys.exit(main())
