#!/usr/bin/env python3
"""What the firmware recorded when the machine died (see docs/HARNESS.md, "Machine-level resets").

    python tools/harness/crashes.py                     # the last 72 hours
    python tools/harness/crashes.py --hours 336         # two weeks, for the running tally
    python tools/harness/crashes.py --scan-run harness_runs/20260925_212500_tw_terms
    python tools/harness/crashes.py --json > cper.json  # keep the raw evidence forever
    python tools/harness/crashes.py --decode cper.json  # re-read a saved dump (no machine needed)
    python tools/harness/crashes.py --preflight         # run.py calls this before every launch

A machine-level reset here is not an app crash and leaves no dump: Windows boots, finds a UEFI BERT
record of a *fatal hardware error*, logs WHEA-Logger 1 (the raw CPER record, as hex, in EventData)
plus Kernel-Power 41 with BugcheckCode 0, and the machine is back. The witnesses are therefore that
CPER record and the app's own log; this tool reads both and puts them side by side. Read-only: it
never writes to the event log, the registry, or the machine.

Exit codes: 0 nothing found, 3 a fatal firmware record was found (report and --preflight), 2 the
event log could not be read.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
RUNS = os.path.join(REPO, "harness_runs")

# CPER notification types (UEFI 2.7 N.2.1, cross-checked against Linux include/linux/cper.h and EDK2
# MdePkg/Include/Guid/Cper.h on 2026-09-25). BOOT means the *firmware* recorded the error while
# booting (a BERT entry) rather than Windows reacting to a failure it watched happen.
NOTIFY = {
    "2DCE8BB1-BDD7-450E-B9AD-9CF4EBD4F890": "CMC (corrected machine check)",
    "4E292F96-D843-4A55-A8C2-D481F27EBEEE": "CPE (corrected platform error)",
    "E8F56FFE-919C-4CC5-BA88-65ABE14913BB": "MCE (machine check exception)",
    "CF93C01F-1A16-4DFC-B8BC-9C4DAF67C104": "PCIe error",
    "CC5263E8-9308-454A-89D0-340BD39BC98E": "INIT",
    "5BAD89FF-B7E6-42C9-814A-CF2485D6E98A": "NMI",
    "3D61A466-AB40-409A-A698-F362D464B38F": "BOOT (firmware-recorded, BERT)",
    "667DD791-C6B3-4C27-8A6B-0F8E722DEB41": "DMAR",
}
SEVERITY = {0: "recoverable", 1: "fatal", 2: "corrected", 3: "informational"}

# Section type GUIDs, verified 2026-09-25 against EDK2 MdePkg/Include/Guid/Cper.h (the GUIDs UEFI 2.7
# N.2.5 defines; the three-letter field literals there are the canonical form). The keys must match
# _guid()'s output. A wrong name is worse than a raw GUID, so add names only from a checked source.
SECTION_TYPES = {
    "9876CCAD-47B4-4BDB-B65E-16F193C4F3DB": "processor generic",
    "DC3EA0B0-A144-4797-B95B-53FA242B6E1D": "processor specific (IA32/x64)",
    "E19E3D16-BC11-11E4-9CAA-C2051D5D46B0": "processor specific (ARM)",
    "A5BC1114-6F64-4EDE-B863-3E83ED7C83B1": "platform memory error",
    "61EC04FC-48E6-D813-25C9-8DAA44750B12": "platform memory error 2",
    "D995E954-BBC1-430F-AD91-B44DCB3C6F35": "PCIe error",
    "C5753963-3B84-4095-BF78-EDDAD3F9C9DD": "PCI/PCI-X bus error",
    "EB5E4685-CA66-4769-B6A2-26068B001326": "PCI/PCI-X device error",
    "5B51FEF7-C79D-4434-8F1B-AA62DE3E2C64": "DMAr generic error",
    "71761D37-32B2-45CD-A7D0-B0FEDD93E8CF": "directed I/O DMAr error",
    "036F84E1-7F37-428C-A79E-575FDFAA84EC": "IOMMU DMAr error",
    "81212A96-09ED-4996-9471-8D729C8E69ED": "firmware error record reference",
}
# A firmware error record reference carries no hardware detail of its own: it points at the record the
# platform stored during POST (FRU id + a GUID naming that stored record). Every record on this machine
# is three of these, so the fatal detail is *not* in the event log - only the pointer to it is.
FW_RECORD_GUID = "81212A96-09ED-4996-9471-8D729C8E69ED"

PS_COLLECT = r"""
$ErrorActionPreference = 'SilentlyContinue'
function Get-Ev([string]$Prov, [int[]]$Ids, [datetime]$Start, [int]$Max) {
  $f = @{ LogName = 'System'; StartTime = $Start }
  if ($Prov) { $f['ProviderName'] = $Prov }
  if ($Ids)  { $f['Id'] = $Ids }
  Get-WinEvent -FilterHashtable $f -MaxEvents $Max -ErrorAction SilentlyContinue | ForEach-Object {
    $d = [ordered]@{}
    try { $x = [xml]$_.ToXml() } catch { $x = $null }
    if ($x) { foreach ($n in $x.Event.EventData.Data) { if ($n.Name) { $d[$n.Name] = $n.'#text' } } }
    [pscustomobject]@{ time = $_.TimeCreated.ToString('o'); id = $_.Id; level = $_.LevelDisplayName; data = $d }
  }
}
$o = [ordered]@{}
$o.power = @(Get-Ev 'Microsoft-Windows-Kernel-Power' @(41) $Start 200)
$o.whea  = @(Get-Ev 'Microsoft-Windows-WHEA-Logger' @() $Start 400)
$o.bug   = @(Get-Ev 'Microsoft-Windows-WER-SystemErrorReporting' @(1001) $Start 50)
$o | ConvertTo-Json -Depth 6 -Compress
"""


def ps_collect(start):
    """Read the System log through PowerShell. start is a 'YYYY-MM-DDTHH:MM:SS' string."""
    script = PS_COLLECT
    script = script.replace("$o = [ordered]@{}", "$Start = [datetime]'%s'\n$o = [ordered]@{}" % start)
    exe = "powershell.exe"
    try:
        p = subprocess.run([exe, "-NoProfile", "-NonInteractive", "-Command", script],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace")
    except OSError as e:
        raise RuntimeError("cannot run powershell: %s" % e)
    if p.returncode != 0 or not p.stdout.strip():
        raise RuntimeError("event log read failed (rc=%s): %s" % (p.returncode, (p.stderr or "").strip()[-400:]))
    d = json.loads(p.stdout)
    out = {}
    for k in ("power", "whea", "bug"):
        v = d.get(k) or []
        out[k] = v if isinstance(v, list) else [v]
    return out


def _iso(ts):
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(ts))


def _ts(iso):
    """PowerShell hands back local time with an offset: 2026-09-25T21:25:55.123+02:00."""
    try:
        return time.mktime(time.strptime(iso[:19], "%Y-%m-%dT%H:%M:%S"))
    except ValueError:
        return 0.0


# ── CPER decoding (UEFI 2.7 N.2: 128-byte common header, then 72-byte section descriptors) ──

def _guid(b, off):
    if off + 16 > len(b):
        return ""
    d1, d2, d3 = struct.unpack_from("<IHH", b, off)
    d4 = b[off + 8:off + 16]
    return "%08X-%04X-%04X-%s-%s" % (d1, d2, d3, d4[:2].hex().upper(), d4[2:].hex().upper())


def _ascii(b):
    return b.split(b"\x00")[0].decode("ascii", "replace").strip()


def _bcd(v):
    """EFI_ERROR_TIME_STAMP is 8 BCD bytes: sec, min, hour, flag, day, month, year, century."""
    out = []
    for x in v:
        hi, lo = x >> 4, x & 0xF
        if hi > 9 or lo > 9:
            return None
        out.append(hi * 10 + lo)
    sec, minute, hour, flag, day, month, year, cent = out
    return "%02d%02d-%02d-%02dT%02d:%02d:%02d" % (cent, year, month, day, hour, minute, sec)


def _int(v, default=0):
    """PowerShell hands some of these back as '0' rather than 0; treat anything unparseable as 0."""
    try:
        return int(v)
    except (TypeError, ValueError):
        try:
            return int(str(v).strip(), 0)
        except (TypeError, ValueError):
            return default


def _stamp_bin(v):
    """The same 8 bytes as plain binary fields. This firmware writes them that way (sec, min, hour,
    flag, day, month, year, century) - verified against the Kernel-Power 41 of every record here."""
    if len(v) != 8:
        return None
    sec, minute, hour, flag, day, month, year, cent = v
    if sec > 59 or minute > 59 or hour > 23 or not 1 <= day <= 31 or not 1 <= month <= 12:
        return None
    return "%02d%02d-%02d-%02dT%02d:%02d:%02d" % (cent, year, month, day, hour, minute, sec)


def _utc_ahead():
    """How far a UTC stamp reads ahead of this machine's local event-log time (7 h at UTC-7).
    The CPER stamp and the app's own log lines are both UTC (src/Log.cpp uses gmtime); Windows'
    System log prints local time, so the two need converting before they can be compared."""
    off = time.strftime("%z")
    try:
        h, m = int(off[:3]), int(off[3:5])
    except (ValueError, IndexError):
        return 0.0
    return -(h + m / 60.0 * (1 if h >= 0 else -1))


def decode_cper(raw):
    """raw: bytes or a hex string (what WHEA-Logger puts in EventData). Returns a dict."""
    if isinstance(raw, str):
        try:
            raw = bytes.fromhex("".join(raw.split()))
        except ValueError:
            return {"error": "RawData is not hex"}
    b = raw
    if len(b) < 128 or b[0:4] != b"CPER":
        return {"error": "not a CPER record (%d bytes, signature %r)" % (len(b), b[:4])}
    severity = struct.unpack_from("<I", b, 12)[0]
    validation = struct.unpack_from("<I", b, 16)[0]
    rec_len = struct.unpack_from("<I", b, 20)[0]
    nsec = struct.unpack_from("<H", b, 10)[0]
    ts = b[24:32]
    notif = _guid(b, 0x50)
    r = {
        "length": len(b),
        "revision": struct.unpack_from("<H", b, 4)[0],
        "severity": severity, "severity_name": SEVERITY.get(severity, "?%d" % severity),
        "section_count": nsec,
        "record_length": rec_len, "record_length_matches": rec_len == len(b),
        "timestamp_valid": bool(validation & 2), "timestamp_bcd": _bcd(ts),
        "timestamp_bin": _stamp_bin(ts), "timestamp_hex": ts.hex(" "),
        "platform_id": _guid(b, 0x20), "partition_id": _guid(b, 0x30),
        "creator_id": _guid(b, 0x40),
        "notification": notif, "notification_name": NOTIFY.get(notif, "unknown"),
        "record_id": struct.unpack_from("<Q", b, 0x60)[0],
        "flags": struct.unpack_from("<I", b, 0x68)[0],
        "sections": [],
    }
    for i in range(nsec):
        off = 128 + i * 72
        if off + 72 > len(b):
            break
        s_off, s_len = struct.unpack_from("<II", b, off)
        s_sev = struct.unpack_from("<I", b, off + 48)[0]
        s_guid = _guid(b, off + 16)
        r["sections"].append({
            "offset": s_off, "length": s_len,
            "revision": struct.unpack_from("<H", b, off + 8)[0],
            "flags": struct.unpack_from("<I", b, off + 12)[0],
            "type_guid": s_guid, "type_name": SECTION_TYPES.get(s_guid),
            "fru_id": _guid(b, off + 32), "fru_text": _ascii(b[off + 52:off + 72]),
            "severity": s_sev, "severity_name": SEVERITY.get(s_sev, "?%d" % s_sev),
            "present": s_off + s_len <= len(b),
        })
    return r


# ── reading a harness run folder ──

def newest_mtime(path):
    """Newest mtime in a run folder: when the app last did anything (files, not the folder)."""
    if os.path.isfile(path):
        return os.path.getmtime(path)
    best, n = 0.0, 0
    for root, dirs, files in os.walk(path):
        for f in files:
            try:
                best = max(best, os.path.getmtime(os.path.join(root, f)))
            except OSError:
                pass
            n += 1
            if n > 400:
                return best
    return best or os.path.getmtime(path)


def run_dirs(runs_dir=RUNS):
    if not os.path.isdir(runs_dir):
        return []
    return [os.path.join(runs_dir, d) for d in sorted(os.listdir(runs_dir))
            if os.path.isdir(os.path.join(runs_dir, d))]


def tail(path, n=4):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return [l.rstrip() for l in f.read().splitlines()[-n:]]
    except OSError:
        return []


NUL_MIN = 16  # a shorter all-zero tail is not evidence of an unflushed write
NUL_MAX_FILES = 4000  # a run folder tree can hold thousands of files (satellite_models_debug
                      # dumps ~1000): the old cap of 500 ran out inside that dump and never
                      # reached sea_after - the folder that actually held the 32 all-zero files


def nul_files(path):
    """(all-zero files, files whose tail is zero) - the fingerprint of a write that committed its
    size and never its contents (see docs/HARNESS.md "Machine-level resets")."""
    whole, tailnul, n = [], [], 0
    for root, dirs, files in os.walk(path):
        dirs.sort()
        for f in sorted(files):
            fp = os.path.join(root, f)
            try:
                sz = os.path.getsize(fp)
                if sz == 0 or sz > 64 << 20:
                    continue
                with open(fp, "rb") as fh:
                    data = fh.read() if sz <= 1 << 20 else fh.read(1 << 20)
            except OSError:
                continue
            n += 1
            rel = os.path.relpath(fp, path)
            if sz <= 1 << 20 and data.strip(b"\x00") == b"":
                whole.append(rel)
            elif len(data) >= NUL_MIN and data[-NUL_MIN:].strip(b"\x00") == b"":
                tailnul.append(rel)
            if n > NUL_MAX_FILES:
                return whole, tailnul
    return whole, tailnul


# ── what the reset looks like, and what the run folder looks like ──

def describe(d):
    """Readable lines for one decoded CPER record."""
    if d.get("error"):
        return ["CPER: " + d["error"]]
    L = ["CPER rev 0x%04X  severity=%s  %d section(s), %d bytes  notify=%s"
         % (d["revision"], d["severity_name"], d["section_count"], d["length"], d["notification_name"])]
    if d["timestamp_bcd"]:
        L.append("firmware timestamp %s (BCD)" % d["timestamp_bcd"])
    elif d.get("timestamp_bin"):
        L.append("firmware timestamp %s (UTC; the fields are binary, not BCD, and this machine's "
                 "System-log times are local - %.0f h behind. The app's own log is UTC too)"
                 % (d["timestamp_bin"], _utc_ahead()))
    else:
        L.append("timestamp field unreadable (%s, header says %s)"
                 % (d["timestamp_hex"], "valid" if d["timestamp_valid"] else "invalid"))
    if not d["record_length_matches"]:
        L.append("WARNING: header says %d bytes, the event carried %d" % (d["record_length"], d["length"]))
    if d["creator_id"]:
        L.append("creator id %s" % d["creator_id"])
    for i, s in enumerate(d["sections"]):
        note = []
        if s["type_name"]:
            note.append(s["type_name"])
        if s["fru_text"]:
            note.append("FRU '%s'" % s["fru_text"])
        if not s["present"]:
            note.append("OUT OF RECORD")
        L.append("  section %d  @0x%X  %d B  sev=%s  type=%s%s"
                 % (i, s["offset"], s["length"], s["severity_name"], s["type_guid"],
                    ("  [" + ", ".join(note) + "]") if note else "  (type not identified)"))
    if any(s["type_guid"] == FW_RECORD_GUID for s in d["sections"]):
        L.append("  ^ a firmware error record reference is a pointer, not the error: the platform stored"
                 " the real record during POST, and the hardware detail lives there, not in this event")
    return L


def _count(path):
    try:
        return len(os.listdir(path))
    except OSError:
        return 0


def scan_run(path):
    """What survived in a run folder, and does it look like it died rather than finished."""
    log = os.path.join(path, "satlight_log.txt")
    lines = tail(log, 10 ** 9) if os.path.exists(log) else []
    whole, tailnul = nul_files(path)
    return {
        "dir": path,
        "created": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(os.path.getmtime(path))),
        "last_activity": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(newest_mtime(path))),
        "summary": os.path.exists(os.path.join(path, "summary.json")),
        "lock": os.path.exists(os.path.join(path, "session.lock")),
        "results": os.path.exists(os.path.join(path, "results.jsonl")),
        "captures": _count(os.path.join(path, "captures")),
        "log_lines": len(lines),
        "log_tail": lines[-4:],
        "nul_whole": whole,
        "nul_tail": tailnul,
    }


RUN_MARKERS = ("satlight_log.txt", "session.lock", "results.jsonl")


def _nested_runs(path):
    """Run folders *inside* a run folder: an A/B keeps its two halves together (the folder that
    really holds the 32 all-zero files here is `harness_runs\\ocean_rings_fix\\sea_after`, not
    `ocean_rings_fix` itself). Recognised by the files every run leaves behind."""
    out = []
    try:
        names = sorted(os.listdir(path))
    except OSError:
        return out
    for d in names:
        p = os.path.join(path, d)
        if os.path.isdir(p) and any(os.path.exists(os.path.join(p, m)) for m in RUN_MARKERS):
            out.append(p)
    return out


def scan_lines(path):
    s = scan_run(path)
    L = ["%s  (started %s, last write %s)"
         % (os.path.relpath(s["dir"], REPO), s["created"], s["last_activity"]),
         "  summary.json   %s" % ("present" if s["summary"] else "MISSING - the run never finished"),
         "  session.lock   %s" % ("PRESENT - the app never cleaned up (it did not exit)" if s["lock"]
                                  else "absent - the app exited"),
         "  results.jsonl  %s, captures %d file(s)" % ("present" if s["results"] else "MISSING", s["captures"]),
         "  satlight_log.txt  %d line(s)%s"
         % (s["log_lines"], (", last: " + " | ".join(s["log_tail"])) if s["log_tail"] else "")]
    for p in _nested_runs(path):
        n = scan_run(p)
        L.append("  %s\\  %s, captures %d, log %d line(s)%s"
                 % (os.path.basename(p), "finished" if n["summary"] else "NO summary.json",
                    n["captures"], n["log_lines"],
                    ", last: " + n["log_tail"][-1] if n["log_tail"] else ""))
    if s["nul_whole"]:
        L.append("  ALL-ZERO files (%d) - a write that committed its size, never its contents:"
                 % len(s["nul_whole"]))
        L += ["    " + f for f in sorted(s["nul_whole"])[:12]]
        if len(s["nul_whole"]) > 12:
            L.append("    ... and %d more" % (len(s["nul_whole"]) - 12))
    if s["nul_tail"]:
        L.append("  zero tail (%d file(s)): %s" % (len(s["nul_tail"]), ", ".join(sorted(s["nul_tail"])[:8])))
    if not s["nul_whole"] and not s["nul_tail"]:
        L.append("  no all-zero files - nothing lost to an unflushed write")
    return L


# ── the report ──

def records_in(ev, runs_dir=RUNS, slack=15):
    """Pair every WHEA-Logger record with the Kernel-Power 41 that goes with it, and with the
    harness run folders that were being written just before it."""
    kp = sorted(ev["power"], key=lambda e: _ts(e["time"]))
    dirs = run_dirs(runs_dir)
    out = []
    for e in sorted(ev["whea"], key=lambda e: _ts(e["time"])):
        t = _ts(e["time"])
        rec = {"whea_time": e["time"], "ts": t, "level": e.get("level"),
               "cper": decode_cper((e.get("data") or {}).get("RawData", "")),
               "power": None, "runs": []}
        near = [k for k in kp if abs(_ts(k["time"]) - t) < 600]
        if near:
            k = min(near, key=lambda k: abs(_ts(k["time"]) - t))
            d = k.get("data") or {}
            rec["power"] = {"time": k["time"], "bugcheck": d.get("BugcheckCode"),
                            "power_button": d.get("PowerButtonTimestamp"),
                            "efi_info": d.get("BugcheckInfoFromEFI"), "data": d}
        for p in dirs:
            m = newest_mtime(p)
            if t - slack * 60 <= m <= t + 120:
                rec["runs"].append({"dir": p, "last_write": m,
                                    "finished": os.path.exists(os.path.join(p, "summary.json"))})
        out.append(rec)
    return out


def report(ev, runs_dir, hours, slack, as_json):
    recs = records_in(ev, runs_dir, slack)
    fatal = [r for r in recs if r["cper"].get("severity_name") == "fatal"]
    if as_json:
        json.dump({"generated": _iso(time.time()), "window_hours": hours, "runs_dir": runs_dir,
                   "records": recs, "power_41": ev["power"], "bugchecks": ev["bug"]},
                  sys.stdout, indent=2)
        print()
        return 3 if fatal else 0
    print("fatal hardware errors recorded by the firmware: %d" % len(fatal))
    if not recs:
        print("no WHEA-Logger record in the last %g h" % hours)
    for r in recs:
        c = r["cper"]
        print("\n  > %s  WHEA-Logger 1  %s"
              % (r["whea_time"][:19].replace("T", " "),
                 ("FATAL" if c.get("severity_name") == "fatal" else str(c.get("severity_name"))).upper()))
        for l in describe(c):
            print("      " + l)
        p = r["power"]
        if p:
            print("      Kernel-Power 41 at %s: BugcheckCode=%s PowerButtonTimestamp=%s BugcheckInfoFromEFI=%s"
                  % (p["time"][:19].replace("T", " "), p["bugcheck"], p["power_button"], p["efi_info"]))
            print("      -> bugcheck 0: Windows never saw one, so no dump exists"
                  if not _int(p["bugcheck"]) else "      -> bugcheck 0x%x before the reset" % _int(p["bugcheck"]))
        else:
            print("      no Kernel-Power 41 within 10 min (the boot that logged it is outside the window)")
        if r["runs"]:
            print("      harness run folders live just before it:")
            for x in r["runs"]:
                print("        %s  (last write %s%s)"
                      % (os.path.relpath(x["dir"], REPO),
                         time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(x["last_write"])),
                         ", finished" if x["finished"] else ", NO summary.json"))
        else:
            print("      no harness run folder was being written in that window")
    used = set(r["power"]["time"] for r in recs if r["power"])
    rest = [k for k in ev["power"] if k["time"] not in used]
    if rest:
        print("\n  resets with no firmware error record (Kernel-Power 41 alone): %d" % len(rest))
        for k in sorted(rest, key=lambda x: _ts(x["time"])):
            d = k.get("data") or {}
            t = _ts(k["time"])
            live = [p for p in run_dirs(runs_dir) if t - slack * 60 <= newest_mtime(p) <= t + 120]
            pb = _int(d.get("PowerButtonTimestamp"))
            why = ("the power button was held - a manual power-cycle (stamp %d)" % pb) if pb else \
                  "no BERT/WHEA record: this boot found no firmware error (hung or forced off)"
            print("    %s  BugcheckCode=%s  %s" % (k["time"][:19].replace("T", " "), d.get("BugcheckCode"), why))
            for p in live:
                print("        while %s was running (last write %s)"
                      % (os.path.relpath(p, REPO),
                         time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(newest_mtime(p)))))
    print("\n  bugchecks (WER 1001) in the window: %d" % len(ev["bug"]))
    for b in ev["bug"]:
        d = b.get("data") or {}
        print("    %s  %s" % (b["time"][:19].replace("T", " "), d.get("param1", "")))
    if fatal:
        print("\n  verdict: the firmware recorded a FATAL hardware error and reset the platform.")
        print("           BugcheckCode 0 means no dump will ever be written - the CPER record above and")
        print("           the app's own log (now fsynced per line) are the only evidence that survives.")
    return 3 if fatal else 0


# ── the two entry points run.py uses ──

def preflight_message(runs_dir=RUNS):
    """Warn if the firmware recorded a fatal error since the newest run folder was written, else
    None. One event log query over a short window, so it is cheap enough to call before every
    launch. Only WHEA events are examined: a Kernel-Power 41 with no firmware record (a hang or a
    forced power-off) has no WHEA event behind it, so it cannot be warned about here - those are
    listed in the report and by --witness instead ("resets with no firmware error record")."""
    dirs = run_dirs(runs_dir)
    if not dirs:
        return None
    start = max(newest_mtime(d) for d in dirs) - 60
    ev = ps_collect(_iso(start))
    hits = [e for e in ev["whea"] if _ts(e["time"]) >= start]
    if not hits:
        return None
    e = max(hits, key=lambda x: _ts(x["time"]))
    d = decode_cper((e.get("data") or {}).get("RawData", ""))
    return ("WARNING: the machine hard-reset after the last harness run - WHEA-Logger 1 at %s, %s / %s. "
            "Evidence: python tools/harness/crashes.py --hours 24"
            % (e["time"][:19].replace("T", " "), d.get("severity_name", "?"),
               d.get("notification_name", "?")))


def write_witness(run_dir, hours=6):
    """A run folder with no summary.json did not finish. Write crash_witness.txt into it: what the
    firmware recorded around the last thing the app wrote, plus what survived - so the evidence is
    still readable after the next reboot. Returns the path or None."""
    if not os.path.isdir(run_dir):
        return None
    end = newest_mtime(run_dir)
    ev = ps_collect(_iso(end - hours * 3600))
    L = ["# crash witness: %s" % os.path.basename(run_dir.replace("/", os.sep).rstrip(os.sep)),
         "# written %s by tools/harness/crashes.py (docs/HARNESS.md, Machine-level resets)"
         % time.strftime("%Y-%m-%d %H:%M:%S"),
         "# the app last wrote something at %s" % time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(end)),
         "", "what survived in the run folder:"]
    L += ["  " + x for x in scan_lines(run_dir)]
    hits = [e for e in ev["whea"] if _ts(e["time"]) >= end - hours * 3600]
    if hits:
        for e in sorted(hits, key=lambda x: _ts(x["time"])):
            L += ["", "WHEA-Logger 1 at %s - hardware error recorded by the firmware:"
                  % e["time"][:19].replace("T", " ")]
            L += ["  " + x for x in describe(decode_cper((e.get("data") or {}).get("RawData", "")))]
        for k in ev["power"]:
            d = k.get("data") or {}
            L += ["", "Kernel-Power 41 at %s: BugcheckCode=%s PowerButtonTimestamp=%s"
                  % (k["time"][:19].replace("T", " "), d.get("BugcheckCode"), d.get("PowerButtonTimestamp"))]
    else:
        L += ["", "no WHEA-Logger record in the %g h before this run stopped: either the window is too "
              "short or this was not a firmware reset (an app abort looks like this - check the log "
              "tail above)" % hours]
    path = os.path.join(run_dir, "crash_witness.txt")
    with open(path, "w", encoding="utf-8", errors="replace") as f:
        f.write("\n".join(L) + "\n")
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hours", type=float, default=72, help="how far back to look (default 72)")
    ap.add_argument("--runs-dir", default=RUNS, help="harness run folder root")
    ap.add_argument("--slack", type=float, default=15, help="minutes before a reset that count as live (default 15)")
    ap.add_argument("--json", action="store_true", help="dump events and decoded records as JSON")
    ap.add_argument("--scan-run", action="append", default=[], metavar="DIR", help="what survived in a run folder")
    ap.add_argument("--decode", action="append", default=[], metavar="FILE", help="decode a saved --json dump or raw hex")
    ap.add_argument("--preflight", action="store_true", help="exit 3 if the machine reset since the last run")
    ap.add_argument("--witness", metavar="DIR", help="write crash_witness.txt into a dead run folder")
    a = ap.parse_args()

    if a.witness:
        print(write_witness(os.path.abspath(a.witness)) or "no witness written")
        return 0

    if a.decode:
        for f in a.decode:
            try:
                raw = open(f, "rb").read()
            except OSError as e:
                print("%s: %s" % (f, e))
                continue
            # PowerShell's `>` writes UTF-16LE on 5.1, so a dump saved that way is not UTF-8.
            text = raw.decode("utf-16" if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8-sig", "replace")
            blobs = []
            try:
                d = json.loads(text)
                for r in d.get("records", []):
                    c = r.get("cper")
                    blobs.append((r.get("whea_time", "?"),
                                  c if isinstance(c, dict)          # a --json dump: already decoded
                                  else decode_cper((r.get("data") or {}).get("RawData", ""))))
                for w in d.get("whea", []):
                    blobs.append((w.get("time", "?"), decode_cper((w.get("data") or {}).get("RawData", ""))))
            except ValueError:
                one = "".join(text.split())
                if one and all(c in "0123456789abcdefABCDEF" for c in one):
                    blobs.append(("", decode_cper(one)))
                else:
                    for line in text.splitlines():
                        s = line.strip().rstrip(",")
                        if s.lower().startswith("rawdata"):
                            s = s.split("=", 1)[-1]
                        if s:
                            blobs.append(("", decode_cper(s)))
            for when, c in blobs:
                if when:
                    print("%s  WHEA record" % when[:19].replace("T", " "))
                for l in describe(c):
                    print("  " + l)
                print()
        return 0

    if a.scan_run:
        for p in a.scan_run:
            print("\n".join(scan_lines(os.path.abspath(p))) + "\n")
        return 0

    if a.preflight:
        try:
            msg = preflight_message(a.runs_dir)
        except RuntimeError as e:
            print("cannot read the event log: %s" % e, file=sys.stderr)
            return 2
        if msg:
            print(msg)
            return 3
        return 0

    try:
        ev = ps_collect(_iso(time.time() - a.hours * 3600))
    except RuntimeError as e:
        print("cannot read the event log: %s" % e, file=sys.stderr)
        return 2
    return report(ev, a.runs_dir, a.hours, a.slack, a.json)


if __name__ == "__main__":
    sys.exit(main())
