#!/usr/bin/env python3
"""What the firmware recorded when the machine died (see docs/FREEZES.md, "Machine-level resets").

    python tools/harness/crashes.py                     # the last 72 hours
    python tools/harness/crashes.py --hours 336         # two weeks, for the running tally
    python tools/harness/crashes.py --scan-run harness_runs/20260925_212500_tw_terms
    python tools/harness/crashes.py --json > cper.json  # keep the raw evidence forever
    python tools/harness/crashes.py --decode cper.json  # re-read a saved dump (no machine needed)
    python tools/harness/crashes.py --preflight         # run.py calls this before every launch
    python tools/harness/crashes.py --history           # every fatal record Windows kept, by month
    python tools/harness/crashes.py --export-crashlog harness_runs/crashlog   # the Intel CrashLogs
    python tools/harness/crashes.py --decode-crashlog harness_runs/crashlog   # needs Intel iclg
    python tools/harness/crashes.py --dump-status       # is the manual-crash key armed, has it ever fired

A machine-level reset here is not an app crash and leaves no dump: Windows boots, finds a UEFI BERT
record of a *fatal hardware error*, logs WHEA-Logger 1 (the raw CPER record, as hex, in EventData)
plus Kernel-Power 41 with BugcheckCode 0, and the machine is back. The witnesses are therefore that
CPER record and the app's own log; this tool reads both and puts them side by side. Read-only: it
never writes to the event log, the registry, or the machine.

Exit codes: 0 nothing found, 3 a fatal firmware record was found (report and --preflight), 2 the
event log could not be read.
"""
import argparse
import glob
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
# A "firmware error record reference" section (UEFI 2.7 N.2.10) starts with a 32-byte header: record
# type (0 IPF SAL, 1 SoC type 1, 2 SoC type 2), revision, 6 reserved, a 64-bit record id and (type 2) a
# GUID naming the record's format. Types 1/2 carry the record ITSELF after the header. Every record on
# this machine is three type-2 sections with the Intel CrashLog GUID, i.e. 2560 + 512 + 4096 bytes of
# raw Intel CrashLog: the PCH's PMC record, a PMC trace, and the CPU's Punit record. Until 2026-09-26
# this file and docs/FREEZES.md called them "pointers with no hardware detail" - wrong: the detail is
# here, and Intel's decoder (github.com/intel/crashlog, `iclg`) reads it; see --export-crashlog.
FW_RECORD_GUID = "81212A96-09ED-4996-9471-8D729C8E69ED"
FW_RECORD_TYPES = {0: "IPF SAL (pointer only)", 1: "SoC firmware record type 1", 2: "SoC firmware record type 2"}
FW_RECORD_FORMATS = {"8F87F311-C998-4D9E-A0C4-6065518C4F6D": "Intel CrashLog"}
# The Kernel-WHEA/Errors channel keeps every fatal record Windows found at boot, for far longer than
# the System log (which rolls over): on this machine it reaches back to 2025-05, ten months before the
# project's first commit.
WHEA_ERRORS_LOG = "Microsoft-Windows-Kernel-WHEA/Errors"

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
        fw = None
        if s_guid == FW_RECORD_GUID and s_off + 32 <= len(b):
            fw_type = b[s_off]
            fw_fmt = _guid(b, s_off + 16) if fw_type == 2 else ""
            fw = {"type": fw_type, "type_name": FW_RECORD_TYPES.get(fw_type, "?%d" % fw_type),
                  "revision": b[s_off + 1], "record_id": struct.unpack_from("<Q", b, s_off + 8)[0],
                  "format_guid": fw_fmt, "format_name": FW_RECORD_FORMATS.get(fw_fmt),
                  "payload_bytes": max(0, s_len - 32) if fw_type in (1, 2) else 0}
        r["sections"].append({
            "fw_record": fw,
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
    size and never its contents (see docs/FREEZES.md "Machine-level resets")."""
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
        fw = s.get("fw_record")
        if fw:
            L.append("      %s rev %d, %s%s"
                     % (fw["type_name"], fw["revision"], fw["format_name"] or fw["format_guid"] or "no format GUID",
                        (", %d bytes of it embedded" % fw["payload_bytes"]) if fw["payload_bytes"] else ""))
    if any((s.get("fw_record") or {}).get("format_name") == "Intel CrashLog" for s in d["sections"]):
        L.append("  ^ the Intel CrashLog itself is in this event: decode it with Intel's iclg"
                 " (crashes.py --export-crashlog DIR, then --decode-crashlog DIR)")
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
        live = [x for x in r["runs"] if not x["finished"]]
        bb = blackbox_lines(live[-1]["dir"] if live else None,
                            _ts(r["power"]["time"]) if r["power"] else r["ts"])
        for l in bb:
            print("      " + l)
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
        print("\n  verdict: the firmware filed a 'fatal' CrashLog record at boot. All 46 records decoded so")
        print("           far are the chipset noting HOW it was reset: 24 name the power button held 4 s")
        print("           (a forced power-off of a FROZEN machine), 3 a software restart, 19 carry no PMC")
        print("           record - and every hardware cause bit is 0 in every one, so the record is the")
        print("           consequence of the freeze, never its cause (--decode-crashlog prints the bits).")
        print("           BugcheckCode 0: Windows never bugchecked, so no dump exists.")
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
         "# written %s by tools/harness/crashes.py (docs/FREEZES.md, Machine-level resets)"
         % time.strftime("%Y-%m-%d %H:%M:%S"),
         "# the app last wrote something at %s" % time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(end)),
         "", "what survived in the run folder:"]
    L += ["  " + x for x in scan_lines(run_dir)]
    bb = blackbox_lines(run_dir)
    if bb:
        L += ["", "the flight recorder (blackbox.csv) - the hardware's last seconds:"] + ["  " + x for x in bb]
    hits =[e for e in ev["whea"] if _ts(e["time"]) >= end - hours * 3600]
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


# ── the long history, and the CrashLog payloads ──

PS_WHEA_ERRORS = r"""
$ErrorActionPreference = 'SilentlyContinue'
@(Get-WinEvent -LogName '%s' -MaxEvents 5000 -ErrorAction SilentlyContinue | ForEach-Object {
  $r = ''
  try { $x = [xml]$_.ToXml(); foreach ($n in $x.Event.EventData.Data) { if ($n.Name -eq 'RawData') { $r = $n.'#text' } } } catch {}
  [pscustomobject]@{ time = $_.TimeCreated.ToString('o'); id = $_.Id; raw = $r }
}) | ConvertTo-Json -Depth 3 -Compress
""" % WHEA_ERRORS_LOG


def whea_history():
    """Every fatal record in the Kernel-WHEA/Errors channel, oldest first: [{time, ts, raw}]."""
    p = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", PS_WHEA_ERRORS],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace")
    if p.returncode != 0:
        raise RuntimeError("event log read failed (rc=%s): %s" % (p.returncode, (p.stderr or "").strip()[-400:]))
    txt = p.stdout.strip()
    d = json.loads(txt) if txt else []
    d = d if isinstance(d, list) else [d]
    out = [{"time": e["time"], "ts": _ts(e["time"]), "raw": e.get("raw") or ""} for e in d if e.get("raw")]
    return sorted(out, key=lambda e: e["ts"])


def first_commit_ts():
    try:
        p = subprocess.run(["git", "log", "--reverse", "--format=%ct"], cwd=REPO, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, text=True)
        return float(p.stdout.split()[0])
    except (OSError, ValueError, IndexError):
        return None


def history_report():
    ev = whea_history()
    if not ev:
        print("no records in %s" % WHEA_ERRORS_LOG)
        return 0
    fc = first_commit_ts()
    months = {}
    for e in ev:
        months.setdefault(e["time"][:7], []).append(e)
    print("%s: %d fatal firmware records, %s .. %s"
          % (WHEA_ERRORS_LOG, len(ev), ev[0]["time"][:10], ev[-1]["time"][:10]))
    if fc:
        pre = [e for e in ev if e["ts"] < fc]
        print("  %d of them BEFORE this repository's first commit (%s) - out of scope for the tally here "
              "(docs/FREEZES.md)"
              % (len(pre), time.strftime("%Y-%m-%d", time.localtime(fc))))
    sig = {}
    for e in ev:  # same firmware record shape each time? (section count and sizes)
        c = decode_cper(e["raw"])
        k = tuple(s["length"] for s in c.get("sections", []))
        sig[k] = sig.get(k, 0) + 1
    print("  record shapes (section sizes): " + ", ".join("%s x%d" % (list(k), n) for k, n in sig.items()))
    print("\n  by month:")
    for m in sorted(months):
        days = sorted(set(e["time"][8:10] for e in months[m]))
        print("    %s  %2d  %s" % (m, len(months[m]), "#" * len(months[m])) + "   days " + " ".join(days))
    return 3


# Dump kinds (Control\CrashControl\CrashDumpEnabled). A hang is caught with CrashOnCtrlScroll, which
# needs the kernel alive *and* a dump big enough to hold kernel stacks: a small dump (3) is written by
# the crash path but is not what shows what a hung driver's threads were doing.
DUMP_KINDS = {0: "none - a hang can never leave a dump",
              1: "complete (the whole RAM)",
              2: "kernel (enough for a hang: kernel stacks, no user pages)",
              3: "small/minidump - too small to show what a hung driver was doing",
              7: "automatic"}


def _reg(name, value):
    """One value out of HKLM via reg.exe (read-only). None when the key or the value is absent."""
    p = subprocess.run(["reg", "query", name, "/v", value],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace")
    if p.returncode != 0:
        return None
    for line in p.stdout.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[0].lower() == value.lower():
            return parts[2].strip()
    return None


def _reg_path(name, value, default):
    """A REG_EXPAND_SZ path out of the registry, expanded, or `default` when it is not set."""
    raw = _reg(name, value)
    return os.path.expandvars(raw.strip().strip('"')) if raw else os.path.expandvars(default)


def _reg_written(path):
    """When the HKLM subkey `path` was last written (local time), or None. reg.exe will not say, and it
    matters: kbdhid reads CrashOnCtrlScroll when the driver loads, so a key set *during* the current
    boot catches nothing until the next one. A freeze in between would be missed while the key still
    reads back as armed."""
    import ctypes
    adv = ctypes.windll.advapi32
    h = ctypes.c_void_p()
    if adv.RegOpenKeyExW(ctypes.c_void_p(0x80000002), ctypes.c_wchar_p(path), 0, 0x20019,
                         ctypes.byref(h)) != 0:
        return None
    try:
        ft = ctypes.c_longlong()
        if adv.RegQueryInfoKeyW(h, None, None, None, None, None, None, None, None, None, None,
                                ctypes.byref(ft)) != 0:
            return None
        return time.localtime(ft.value / 1e7 - 11644473600.0)  # FILETIME ticks -> Unix epoch
    finally:
        adv.RegCloseKey(h)


def _boot_local():
    """When this machine booted (local time), or None."""
    import ctypes
    try:
        return time.localtime(time.time() - ctypes.windll.kernel32.GetTickCount64() / 1000.0)
    except (AttributeError, OSError):
        return None


def dump_status():
    """Is the manual-crash key armed (hold right Ctrl, press Scroll Lock twice) - and has it ever
    fired? docs/FREEZES.md tells the reader to set it up once; nothing says whether it worked, and a
    freeze leaves no other trace, so this reads the keys, the dump paths and the crash history back."""
    ctl = r"HKLM\SYSTEM\CurrentControlSet\Control\CrashControl"
    L = ["manual crash key (hold right Ctrl, press Scroll Lock twice while the machine is frozen):"]
    for svc, kb in (("kbdhid", "USB"), ("i8042prt", "PS/2")):
        v = _reg(r"HKLM\SYSTEM\CurrentControlSet\Services\%s\Parameters" % svc, "CrashOnCtrlScroll")
        state = ("ARMED" if _int(v) == 1 else "NOT SET") if v is not None else "absent (driver not present)"
        L.append("  %-4s %-9s CrashOnCtrlScroll = %s" % (kb, svc, state))
    when = _reg_written(r"SYSTEM\CurrentControlSet\Services\kbdhid\Parameters")
    boot = _boot_local()
    if when:
        live = "this boot's start is unknown"
        if boot:
            live = ("in effect for this boot" if time.mktime(when) < time.mktime(boot) else
                    "NOT in effect until the next boot - kbdhid reads it when the driver loads")
        L.append("       %-9s last written %s local%s -> %s"
                 % ("", time.strftime("%Y-%m-%d %H:%M:%S", when),
                    ", this boot began " + time.strftime("%H:%M:%S", boot) if boot else "", live))
    kind = _int(_reg(ctl, "CrashDumpEnabled"), -1)
    L.append("  dump %-10s CrashDumpEnabled = %s -> %s"
             % ("", "?" if kind < 0 else kind, DUMP_KINDS.get(kind, "unknown kind")))
    dump = _reg_path(ctl, "DumpFile", r"%SystemRoot%\MEMORY.DMP")
    mini = _reg_path(ctl, "MinidumpDir", r"%SystemRoot%\Minidump")
    for label, path in (("dump file", dump), ("minidumps", mini)):
        if os.path.isfile(path):
            L.append("  %-9s %s  %.1f MB, written %s" % (label, path, os.path.getsize(path) / 1e6,
                       time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(os.path.getmtime(path)))))
        elif os.path.isdir(path):
            L.append("  %-9s %s  no dump in it" % (label, path))
        else:
            L.append("  %-9s %s  does not exist" % (label, path))
    import shutil
    drive = os.path.splitdrive(os.path.abspath(dump))[0] + "\\"
    try:
        L.append("  free      %s  %.1f GB (a kernel dump needs room for the kernel's pages)"
                 % (drive, shutil.disk_usage(drive).free / 1e9))
    except OSError:
        pass
    try:
        bugs = ps_collect(_iso(time.time() - 30 * 86400))["bug"]
        L.append("  bugchecks %d WER 1001 in the last 30 days%s" % (
            len(bugs), " - 0x000000e2 would be this key (MANUALLY_INITIATED_CRASH)" if not bugs
            else ":"))
        for b in bugs[-3:]:
            L.append("    %s  %s" % (b["time"][:19].replace("T", " "),
                                     (b.get("data") or {}).get("param1", "")))
    except (RuntimeError, KeyError, TypeError) as e:
        L.append("  bugchecks cannot read the event log: %s" % e)
    armed = any(_int(_reg(r"HKLM\SYSTEM\CurrentControlSet\Services\%s\Parameters" % s, "CrashOnCtrlScroll")) == 1
                for s in ("kbdhid", "i8042prt"))
    has = os.path.isfile(dump) or (os.path.isdir(mini) and glob.glob(os.path.join(mini, "*.dmp")))
    if not armed:
        L.append("\n  verdict: a freeze will keep leaving NO dump - that is what 'Kernel-Power 41 with")
        L.append("           BugcheckCode=0' means. Arm it (docs/FREEZES.md, 'What that changes').")
    elif kind not in (1, 2, 7):
        L.append("\n  verdict: the key is armed but CrashDumpEnabled=%d won't capture a hung kernel." % kind)
    elif not has:
        L.append("\n  verdict: armed and a dump is configured, but none has ever been written here -")
        L.append("           so press the key *while* the machine is frozen, before reaching for the")
        L.append("           power button (right Ctrl; the left one is ignored by design). If it was")
        L.append("           pressed during a freeze and still nothing appeared, that freeze answered")
        L.append("           no keyboard interrupt at all, which puts it below the OS: see entry 6 of")
        L.append("           the tally in docs/FREEZES.md.")
    else:
        L.append("\n  verdict: armed, and dump(s) exist - compare their timestamps with the tally in")
        L.append("           docs/FREEZES.md before assuming this freeze left one.")
    print("\n".join(L))
    return 0


def export_crashlogs(dest):
    """One CPER file per fatal record (<local time>.crashlog). `iclg extract` keeps only one record per
    record id, and every record here has id 0, so it drops 44 of 46; this keeps them all."""
    os.makedirs(dest, exist_ok=True)
    n = 0
    for e in whea_history():
        name = e["time"][:19].replace(":", "").replace("-", "") + ".crashlog"
        try:
            data = bytes.fromhex("".join(e["raw"].split()))
        except ValueError:
            continue
        with open(os.path.join(dest, name), "wb") as f:
            f.write(data)
        n += 1
    print("wrote %d CPER record(s) to %s" % (n, dest))
    return 0


def find_iclg():
    """Intel's CrashLog decoder (github.com/intel/crashlog releases, iclg-windows.zip). Looked for in
    $ICLG, tools/harness/iclg/, then PATH. Not vendored: it is a 3 MB third-party binary."""
    cands = [os.environ.get("ICLG", ""), os.path.join(os.path.dirname(os.path.abspath(__file__)), "iclg", "iclg.exe")]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    from shutil import which
    return which("iclg") or which("iclg.exe")


def decode_crashlogs(src):
    """iclg decode each <src>/*.crashlog into <name>.json, and iclg info into <name>.info.txt."""
    exe = find_iclg()
    if not exe:
        print("iclg not found: download iclg-windows.zip from https://github.com/intel/crashlog/releases,"
              " unzip it to tools/harness/iclg/ (or set ICLG=<path to iclg.exe>)")
        return 2
    files = sorted(glob.glob(os.path.join(src, "*.crashlog")))
    for fp in files:
        base = os.path.splitext(fp)[0]
        for args, suffix in ((["info", fp], ".info.txt"), (["decode", fp], ".json")):
            p = subprocess.run([exe] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            with open(base + suffix, "wb") as f:
                f.write(p.stdout)
            if p.returncode != 0:
                print("%s: iclg %s failed (%d): %s" % (os.path.basename(fp), args[0], p.returncode,
                                                        p.stderr.decode(errors="replace").strip()[-300:]))
    print("decoded %d record(s) in %s (<time>.json + <time>.info.txt)" % (len(files), src))
    print("\n" + "\n".join(reset_cause_table(src)))
    return 0


# PMC reset-cause flags worth naming (TGP collateral, iclg's field names). pb_ovr is the one that has
# been set on this machine: the power button held for 4 s. The rest would mean the hardware itself pulled
# the reset: a thermal trip, a power-rail failure, a watchdog, a CPU three-strike.
RESET_CAUSES = {
    "pb_ovr": "power button held 4 s (a forced power-off)",
    "cf9": "software restart (a write to port CF9)",
    "cpu_trip": "CPU THERMAL TRIP", "syspwr_flr": "SYSTEM POWER FAILURE", "pchpwr_flr": "PCH POWER FAILURE",
    "pmc_3strike": "CPU THREE-STRIKE (a core stopped responding)", "cpu_thrm_wdt": "CPU thermal watchdog",
    "pmc_wdt": "PMC watchdog", "me_wdt": "ME watchdog", "tco_wdt": "TCO watchdog", "ich_cat_tmp": "PCH catastrophic temperature",
}


def reset_cause_table(src):
    """Per decoded record (<src>/*.json from iclg): the PMC's non-zero reset-cause bits, and a tally."""
    def walk(o, p=""):
        if isinstance(o, dict):
            for k, v in o.items():
                yield from walk(v, p + "." + k)
        elif isinstance(o, list):
            for i, v in enumerate(o):
                yield from walk(v, p + "[%d]" % i)
        else:
            yield p, o
    L, tally = ["reset causes the chipset (PMC) recorded:"], {}
    for fp in sorted(glob.glob(os.path.join(src, "*.json"))):
        try:
            with open(fp, encoding="utf-8") as f:
                d = json.load(f)
        except (OSError, ValueError):
            continue
        rows = list(walk(d))
        has_pmc = any(".pmc.pmu." in p for p, _ in rows)
        causes = sorted(set(p.rsplit(".", 1)[1] for p, v in rows
                            if ".pmu." in p and not p.endswith("_value") and _int(v)))
        key = ", ".join(causes) if has_pmc else "(no PMC record in this one)"
        tally[key] = tally.get(key, 0) + 1
        L.append("  %s  %s" % (os.path.basename(fp)[:15], key))
    L.append("tally:")
    for k, n in sorted(tally.items(), key=lambda x: -x[1]):
        named = "; ".join(RESET_CAUSES.get(c.strip(), c.strip()) for c in k.split(",")) if not k.startswith("(") else k
        L.append("  %3d  %s" % (n, named))
    return L


def blackbox_lines(run_dir=None, at_ts=None):
    """The flight recorder's last seconds (blackbox.py): the run's own blackbox.csv, else the
    always-on recorder's day files around `at_ts`. [] when nothing was recording."""
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import blackbox
    except Exception:
        return []
    if run_dir and os.path.exists(os.path.join(run_dir, "blackbox.csv")):
        return blackbox.summarize(os.path.join(run_dir, "blackbox.csv"))
    if at_ts is not None and os.path.isdir(blackbox.DAEMON_DIR):
        rows = [r for r in blackbox.read(blackbox.DAEMON_DIR) if at_ts - 600 <= r[0] <= at_ts + 1]
        if rows:
            return blackbox.summarize(blackbox.DAEMON_DIR, at_ts)
    return []


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
    ap.add_argument("--history", action="store_true", help="every fatal record Windows kept (Kernel-WHEA/Errors), by month")
    ap.add_argument("--dump-status", action="store_true",
                    help="is the manual-crash key (Ctrl+ScrollLock) armed, and has it ever written a dump")
    ap.add_argument("--export-crashlog", metavar="DIR", help="write each fatal record's CPER (Intel CrashLog inside) to DIR")
    ap.add_argument("--decode-crashlog", metavar="DIR", help="run Intel's iclg over DIR/*.crashlog (see find_iclg)")
    a = ap.parse_args()

    if a.dump_status:
        return dump_status()

    try:
        if a.history:
            return history_report()
        if a.export_crashlog:
            return export_crashlogs(os.path.abspath(a.export_crashlog))
    except RuntimeError as e:
        print("cannot read the event log: %s" % e, file=sys.stderr)
        return 2
    if a.decode_crashlog:
        return decode_crashlogs(os.path.abspath(a.decode_crashlog))

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
