#!/usr/bin/env python3
"""Flight recorder for machine-level resets (docs/HARNESS.md, "Machine-level resets").

    python tools/harness/blackbox.py --out bb.csv -- cmake --build build --target accuracy-gate
    python tools/harness/blackbox.py --daemon            # always on: harness_runs/blackbox/<day>.csv
    python tools/harness/blackbox.py --install-task       # ... and come back by itself after a reset
    python tools/harness/blackbox.py --uninstall-task     # stop it doing that again
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
Standard library only. Read-only towards the machine: it queries, it never sets anything - the one
exception is `--install-task` / `--uninstall-task`, which are opt-in and touch only this user's logon
task (a scheduled task needs an elevated shell) or, when that is refused, a launcher in the Startup
folder. `--daemon` takes a pid lock in `harness_runs/blackbox/daemon.pid`, so the two can never
double-record into one day file.
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
DAEMON_LOCK = os.path.join(DAEMON_DIR, "daemon.pid")
DAEMON_LOG = os.path.join(DAEMON_DIR, "daemon.log")


def _log(msg):
    """The recorder is started under pythonw by the logon task, where stdout goes nowhere: a refusal or
    a crash used to be invisible, and was (see DaemonLock). This is where it says what happened."""
    try:
        os.makedirs(DAEMON_DIR, exist_ok=True)
        with open(DAEMON_LOG, "a", encoding="utf-8") as f:
            f.write("%s pid %d %s\n" % (_utc(time.time()), os.getpid(), msg))
    except OSError:
        pass


TASK_NAME = "SatLightSim blackbox"
STARTUP_DIR = os.path.join(os.environ.get("APPDATA", ""), "Microsoft", "Windows", "Start Menu",
                           "Programs", "Startup")
STARTUP_LAUNCHER = os.path.join(STARTUP_DIR, "satlight-blackbox.cmd")

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
    """ISO-8601 UTC to milliseconds, *rounded* (not truncated) so it agrees with the epoch column, which
    is printed with %.3f. Truncating let a row disagree with itself by 1 ms whenever the raw fractional
    second was >= .0005 - 3450 of the 7122 rows in 20260926.csv - in a file whose whole purpose is to be
    read back as a timeline."""
    ms = int(round((t % 1) * 1000))
    if ms >= 1000:  # .9996 rounds to the next second; strftime alone would print :00.1000
        ms -= 1000
        t += 1.0
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(t)) + ".%03dZ" % ms


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
        # Print both time columns from one rounded instant: %.3f rounds while _utc used to truncate, so
        # a cpu and a gpu row 1 ms apart could share an epoch value with different ISO stamps (and one
        # row could disagree with itself). The epoch column is the key everything is correlated on.
        t = float("%.3f" % time.time())
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


def _pid_alive(pid):
    """Is process `pid` still running? (Windows: open a SYNCHRONIZE handle and ask; elsewhere: signal 0.)"""
    if pid <= 0:
        return False
    if os.name == "nt":
        import ctypes
        k32 = ctypes.windll.kernel32
        h = k32.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
        if not h:
            return False
        try:
            return k32.WaitForSingleObject(h, 0) == 0x00000102  # WAIT_TIMEOUT: still running
        finally:
            k32.CloseHandle(h)
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def _boot_time():
    """Wall-clock time this machine booted, or None if it will not say. A lock written before it cannot
    have a live holder: a reset leaves no process behind, and nothing survives the boot."""
    if os.name == "nt":
        import ctypes
        return time.time() - ctypes.windll.kernel32.GetTickCount64() / 1000.0
    try:
        with open("/proc/uptime") as f:
            return time.time() - float(f.read().split()[0])
    except (OSError, ValueError, IndexError):
        return None


def _proc_created(pid):
    """Wall-clock time `pid` was created, or None. A pid is not an identity: Windows reuses the numbers,
    within seconds of a boot on this machine, so the lock records (pid, created) and both must match."""
    if pid <= 0:
        return None
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        k32 = ctypes.windll.kernel32
        h = 0
        for access in (0x1000, 0x0400):  # PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_QUERY_INFORMATION
            h = k32.OpenProcess(access, False, pid)
            if h:
                break
        if not h:
            return None
        try:
            made, exited, kern, user = (wintypes.FILETIME() for _ in range(4))
            if not k32.GetProcessTimes(h, ctypes.byref(made), ctypes.byref(exited),
                                       ctypes.byref(kern), ctypes.byref(user)):
                return None
            ticks = (made.dwHighDateTime << 32) | made.dwLowDateTime
            return ticks / 1e7 - 11644473600.0  # 1601-01-01 in FILETIME ticks -> Unix epoch
        finally:
            k32.CloseHandle(h)
    try:  # Linux: field 22 of /proc/<pid>/stat counts clock ticks since boot
        with open("/proc/uptime") as f:
            uptime = float(f.read().split()[0])
        with open("/proc/%d/stat" % pid) as f:
            return time.time() - uptime + int(f.read().rsplit(")", 1)[1].split()[19]) / 100.0
    except (OSError, ValueError, IndexError):
        return None


def _to_float(s):
    try:
        return float(s)
    except ValueError:
        return None


def _parse_iso(s):
    """The lock file is read by hand too, so it carries a human timestamp: '...T07:27:50.897Z' in UTC."""
    if not s.endswith("Z"):
        return None
    try:
        return time.mktime(time.strptime(s[:19], "%Y-%m-%dT%H:%M:%S")) - time.timezone
    except ValueError:
        return None


class DaemonLock:
    """One recorder per machine. Two --daemon processes (the logon task plus one started by hand) would
    interleave into the same day file, and this file is evidence, so it has to be one writer.

    The reset case is what it has to survive: a reset cleans nothing up, so the lock stays behind and
    its pid gets reused - within seconds of a boot here. A bare pid check therefore reads a live
    *stranger* as the holder and locks the next boot's recorder out, silently, under pythonw (observed
    2026-09-26: the 00:40 reset left pid 26576, which was live again by the 00:41 logon, so the
    recorder never came back). So a holder is named by (pid, creation time) - which reuse cannot
    counterfeit - and a lock written before this boot is stale whatever it says."""

    def __init__(self, path=DAEMON_LOCK):
        self.path = path
        self.mine = False

    def _read(self):
        """(pid, created, written) from the lock; created/written are None when unknown (a two-field file
        from before this format, or a platform that would not say)."""
        try:
            with open(self.path) as f:
                tok = f.read().split()
        except OSError:
            return None
        if not tok:
            return None
        try:
            pid = int(tok[0])
        except ValueError:
            return None
        if len(tok) >= 3:
            created = None if tok[1] == "-" else _to_float(tok[1])
            written = _to_float(tok[2])
        else:  # legacy: pid + ISO time of the write
            created, written = None, _parse_iso(tok[1]) if len(tok) > 1 else None
        return pid, created, written

    def holder(self):
        """The pid of the live daemon that holds the lock, or 0 if the lock is free or stale."""
        rec = self._read()
        if not rec:
            return 0
        pid, created, written = rec
        boot = _boot_time()
        if boot and written and written < boot - 5.0:
            return 0  # written before this machine booted: nothing from back then is running
        if not _pid_alive(pid):
            return 0
        now = _proc_created(pid)
        if created is not None and now is not None and abs(now - created) > 1.0:
            return 0  # same number, different process: the pid was reused
        if created is None and now is not None and written and now > written + 2.0:
            return 0  # legacy file, and it cannot have written the lock before it existed
        return pid

    def acquire(self):
        """Return 0, or the pid of the daemon that already holds it."""
        other = self.holder()
        if other:
            return other
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        now = time.time()
        mine = _proc_created(os.getpid())
        with open(self.path, "w") as f:
            f.write("%d %s %.3f %s\n" % (os.getpid(), "-" if mine is None else "%.6f" % mine,
                                         now, _utc(now)))
            f.flush()
            os.fsync(f.fileno())
        self.mine = True
        return 0

    def release(self):
        if self.mine and self.holder() == os.getpid():
            try:
                os.remove(self.path)
            except OSError:
                pass


def _task_cmd():
    """The logon task's command line: pythonw (no console window at every logon) + this script +
    --daemon, all absolute - a task has no shell, no PATH of its own and no working directory."""
    exe = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
    if not os.path.exists(exe):
        exe = sys.executable
    return '"%s" "%s" --daemon' % (exe, os.path.abspath(__file__))


def _schtasks(args):
    return subprocess.run(["schtasks"] + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, errors="replace")


def _startup_write():
    """The no-admin half of --install-task: a .cmd in Startup, which Windows runs at logon for this
    user. Creating a *scheduled task* needs an elevated shell; dropping a file there does not."""
    os.makedirs(STARTUP_DIR, exist_ok=True)
    with open(STARTUP_LAUNCHER, "w", newline="\r\n") as f:
        f.write("@echo off\n")
        f.write("rem SatLightSim flight recorder - tools/harness/blackbox.py --daemon\n")
        f.write("rem installed by 'blackbox.py --install-task', removed by '--uninstall-task'\n")
        f.write("start \"\" %s\n" % _task_cmd())
    return STARTUP_LAUNCHER


def _startup_remove():
    try:
        os.remove(STARTUP_LAUNCHER)
        return True
    except OSError:
        return False


def task_install(remove=False):
    """Install (or remove) whatever keeps a recorder running across a reset: a logon task, or - when
    creating one is denied, which an unelevated shell always is on this machine - a Startup launcher."""
    if os.name != "nt":
        print("--install-task is Windows-only (a schtasks logon task, or a Startup-folder launcher)")
        return 2
    if remove:
        did = []
        if _schtasks(["/delete", "/tn", TASK_NAME, "/f"]).returncode == 0:
            did.append("removed the logon task %r" % TASK_NAME)
        if _startup_remove():
            did.append("removed %s" % STARTUP_LAUNCHER)
        print("\n".join(did) if did else "nothing to remove: no logon task and no Startup launcher")
        return 0
    p = _schtasks(["/create", "/tn", TASK_NAME, "/tr", _task_cmd(), "/sc", "onlogon",
                   "/delay", "0000:30", "/f"])
    if p.returncode == 0:
        print((p.stdout or "").strip())
        q = _schtasks(["/query", "/tn", TASK_NAME, "/v", "/fo", "LIST"])
        for line in (q.stdout or "").splitlines():
            if line.split(":", 1)[0].strip() in ("TaskName", "Task To Run", "Schedule Type",
                                                "Run As User", "Status"):
                print("  " + line.strip())
        print("\nthe recorder now starts by itself 30 s after every logon (task %r)." % TASK_NAME)
    else:
        lines = [l for l in (p.stdout or "").splitlines() if l.strip()]
        print("  schtasks refused: %s" % (lines[0] if lines else "no output"))
        print("  (creating a task needs an elevated shell - using the Startup folder instead)")
        _startup_write()
        print("\nthe recorder now starts by itself at every logon (%s)." % STARTUP_LAUNCHER)
    print("it writes %s, one file per UTC day, 14 kept - and it is the only thing that can record a"
          % os.path.relpath(DAEMON_DIR, REPO))
    print("freeze that happens with no harness run live.")
    print("check it:  python tools/harness/blackbox.py --summary harness_runs/blackbox")
    print("remove it: python tools/harness/blackbox.py --uninstall-task")
    return 0


def _local_epoch(s):
    """'2026-09-25 21:57:53' in this machine's local time (the Windows event list's clock)."""
    return time.mktime(time.strptime(s.replace("T", " ")[:19], "%Y-%m-%d %H:%M:%S"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", help="file to append to (default with a command: blackbox_<stamp>.csv)")
    ap.add_argument("--daemon", action="store_true", help="run until stopped, one file per day in harness_runs/blackbox")
    ap.add_argument("--gpu-ms", type=int, default=None, help="GPU sample period (default 100; daemon 250)")
    ap.add_argument("--install-task", action="store_true",
                    help="keep the recorder running across a reset: install a logon task (Windows)")
    ap.add_argument("--uninstall-task", action="store_true", help="remove it again (task or Startup launcher)")
    ap.add_argument("--summary", metavar="PATH", help="summarize a recording (file or folder)")
    ap.add_argument("--at", help="with --summary: local time of a reset, 'YYYY-MM-DD HH:MM:SS'")
    ap.add_argument("--seconds", type=float, default=20, help="with --summary: window length (default 20)")
    ap.add_argument("--until-pid", type=int, help="with --out: record until this process exits (live.py)")
    ap.add_argument("command", nargs=argparse.REMAINDER, help="-- command to run while recording")
    a = ap.parse_args()

    if a.install_task or a.uninstall_task:
        return task_install(remove=a.uninstall_task)

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
        lock = DaemonLock()
        other = lock.acquire()
        if other:
            _log("refused: pid %d is the holder (%s)" % (other, DAEMON_LOCK))
            print("blackbox: already recording (pid %d, %s) - one recorder per machine, not two"
                  % (other, DAEMON_LOCK))
            return 0
        prune()
        try:
            bb = BlackBox(None, a.gpu_ms or 250).start()
        except Exception as e:  # pythonw hides this: without the log the recorder just never appears
            _log("died starting: %r" % (e,))
            raise
        _log("recording to %s (gpu_ms %d, lock %s)" % (DAEMON_DIR, a.gpu_ms or 250, DAEMON_LOCK))
        print("recording to %s (Ctrl-C to stop)" % DAEMON_DIR, flush=True)
        try:
            while True:
                time.sleep(3600)
                prune()
        except KeyboardInterrupt:
            pass
        finally:
            bb.mark("daemon stopped")
            bb.stop()
            _log("stopped")
            lock.release()
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
