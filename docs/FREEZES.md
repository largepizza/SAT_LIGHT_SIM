# Freezes and machine-level resets

## Status (2026-09-26, evening): NOT resolved; mitigated by launch spacing

- **BIOS PCI_E1 `Auto` → `Gen3`** (kept): an overnight soak then ran 55 launches clean, but the machine
  froze twice more the same afternoon (13:01, 13:52 local). Gen3 is not the fix.
- **The 13:52 freeze was a back-to-back relaunch:** `selftest.py`'s `det_a` exited at 20:50:51Z, `det_b`
  launched immediately and stopped at 20:50:54Z on `init: texture: city_day_detail.png`, the same step
  entry 9 died on. Working hypothesis (the user's): **rapid exit → relaunch is the trigger**, i.e. the
  GPU tearing down one device and being driven back up to full load within a second or two.
- **Mitigation:** the harness spaces launches. `tools/harness/launchgate.py` makes `run.py` and
  `live.py start` wait 30 s after the last app exit (`--cooldown S`); for many batches, use one
  `live.py` app instead of many `run.py` launches. Not a proof. If freezes continue with spacing in
  place, the next candidates are a BIOS/chipset/GPU-driver update and an overnight memtest86.
- The loading screen (`.plans/BOOT_LOADER_PLAN.md`) is a feature, not the fix. The forensics tools below
  run only when asked (`run.py --forensics`, `overnight.py`); nothing starts a recorder at logon.

## The original problem statement

**The problem, in one line: launching this app has a real chance of hanging this machine.**

That sentence is the whole reason this file exists, and it is the 2026-09-26 development period that
put it there: **8 resets in ~14 h**, 2 of the 16 launches the soak drove, every one that caught a
launch caught **inside the first ~4 s of init** - the window where `SatelliteSim::init()` decodes nine
textures (1.83 s of it, ~1.1 GB of decode buffers), builds a 1.38 M-satellite constellation and
creates ~30 pipelines, with nothing on screen and no frame presented to say so. No other app on this
machine does this to it. Whatever the platform is doing underneath, **the launch is the part we
control, and it is where the work goes.**

- **The tally** (below) - one row per reset, newest last: what was live, what the app was doing when it
  stopped, what survived. Eight entries so far, and **2 of the 16 launches the loop has driven died**.
- **The witnesses**: WHEA-Logger 1 (the firmware's CPER record, decoded with Intel's `iclg`), Kernel-Power
  41 (present for every reset, with `BugcheckCode=0` when no dump exists), and `satlight_log.txt` /
  `blackbox.py`'s day file, which are fsynced per line and simply *stop*.
- **The platform's own fragility, kept for scale and not for blame**: 4 of the 12 resets in the last 48 h
  ran with nothing at all, and the firmware's log goes back past this repository (*Out of scope*, below).
  Nothing here can fix that. It is why a reset is a **per-launch risk to be engineered around**, not a
  regression to bisect - and why "it also happens with no app running" is not a reason to stop.
- **The work**: `.plans/BOOT_LOADER_PLAN.md` - a boot/loading screen that runs the init as named,
  per-frame steps, so the burst above stops existing and the next freeze lands on a step number instead
  of a white window. *The launch window we control*, below, is the measurement it is built from.

The harness itself - scripts, tools, the flight recorder, the loop that soaks it - is documented in
`docs/HARNESS.md`; this file holds the freezes and the machine.

## Assessment after entry 9 (2026-09-26)

Written *before* any further launch, because entry 9 took the previous session's context with it.
It changes the direction of this file: **the boot screen is worth finishing as a feature, but it is
not the fix, and the next step is a platform test, not more app code.**

**What the evidence says.**
1. **A user-mode program cannot legitimately do this.** The hang is below the OS (entry 6: the armed
   Ctrl+Scroll Lock bugcheck, which fires inside the keyboard ISR, produced nothing), and the platform
   logs only a forced power-off with every fault bit clear. A Vulkan app making valid API calls can
   crash itself, or TDR the GPU (which Windows recovers from and logs); it cannot stop CPUs from taking
   interrupts. Only hardware, firmware or a kernel driver can. The app is a **trigger**, not a cause.
2. **It predates the app's heavy launch, and happens without it.** 19 of the firmware records are older
   than the repository, and 4 of the 12 resets in the last 48 h had no app running.
3. **It is not one step, and not the DEM.** Entry 9 ran the Phase A boot-screen build and died on
   `city_day_detail.png` (a 2048² PNG, 85 ms). The spread across entries: 1-4 somewhere after
   `Swapchain created` (those builds had no breadcrumbs, so the next line, `env stars:`, came *after*
   the texture step, and they could have died anywhere in it), 5-8 on the DEM, and 9 on a small PNG. The
   DEM collects the most because it is the longest step in the window (834 ms of ~2 s), i.e. **it is
   where the clock usually is when it happens.** What every entry shares is **the first 0.2-2.5 s after
   the GPU leaves idle**: P8 → P0, the PCIe link retrains **gen1 → gen4**, and the launch's first
   hundreds of MB of DMA run over it.
4. **Why it feels new: the harness multiplied the launches.** A few by-hand launches a day became
   dozens (16 in one soak). At ~1 in 8 per launch (2 of 16 in the soak; 13 Kernel-Power 41s from 2026-09-24 to 09-26),
   a per-launch risk that was an occasional mystery became a wall. Nothing requires a code change in
   the 3D-sats / erosion work to explain that.

**Leading hypothesis: the PCIe Gen4 link (CPU lanes → RTX 3070 Ti).** i9-11900K (Rocket Lake, the
first Intel desktop CPU with Gen4 lanes), MSI Z590 PRO WIFI on **BIOS A.30 from 2021-09** (five years
of board updates skipped), and a link that retrains gen1 → gen4 at the start of every launch and then
immediately carries the launch's uploads. A link that drops during retraining or under that load leaves
cores stalled on transactions that never complete, which is exactly the "no interrupts, no dump"
signature. Marginal Gen4 signalling (worst with a riser cable) is the textbook cause of "hard freeze
under GPU load, only the power button helps". **Not proven**: consumer boards seldom report PCIe AER to
Windows, so the absence of WHEA 17/18/19 corrected-error events is no alibi, and the GPU's replay counter
(`nvidia-smi -q`, *Replays Since Reset*) read 0 after the 11:36 boot, before any load. Checked
2026-09-26 and ruled out as the path: Resizable BAR is **off** (BAR1 = 256 MiB), so uploads are GPU
DMA from system RAM, not CPU writes into VRAM; Windows ASPM is already **Off** (High performance plan).
Secondary suspect: memory. 4 × 16 GB from what look like **two kits** (two report part `DDR4 3600`, two
report none) at JEDEC 2667 — less likely at JEDEC speeds, but the launch is also a ~1 GB allocation burst.

**The test (no code, one variable at a time).**
1. **BIOS: force the GPU slot to Gen3** (MSI Click BIOS: *Settings → Advanced → PCI Subsystem Settings →
   PEG0 / PCI_E1 Max Link Speed = Gen3*). An RTX 3070 Ti loses ~1% at Gen3. Also note whether the card
   sits on a riser cable.
2. **Soak it**: `overnight.py` with the recorder. At ~1/8 per launch the old platform survives 25 clean
   launches ~3.5% of the time, and 40 clean launches ~0.5%, so 40 clean launches is a real answer.
3. Still freezing at Gen3: update the BIOS (and the ME firmware + chipset driver), then an overnight
   memtest86, then a GPU driver rollback — each followed by a soak.
4. Blackbox checkpoint for any soak: the `pcie_gen` column should now top out at **3**.

**What this means for the app.** Every game uploads gigabytes over the same link at load, so a machine
that freezes on this app's ~500 MB would freeze on those too, and players on healthy machines are not
at risk from the load itself. The boot screen still earns its place (no 4 s white window, and a frozen
screen names its step), and the asset repack (`.plans/BOOT_LOADER_PLAN.md` Phase D) shortens the
launch outright. The new order is in that plan's *Status*.

## The launch window we control (2026-09-26)

A healthy launch, from `harness_runs/overnight/runs/013_20260926_012236_launch/satlight_log.txt`
(one fsynced line per step, so the ms stamps are real). `t` is seconds after `Vulkan instance created`:

| t | step | cost |
|---|---|---|
| 0.000 | Vulkan instance created | |
| 0.066 | Logical device created | 66 ms |
| 0.237 | Swapchain created (1600x900) | 171 ms |
| 0.241 | `init: buffers` -> noise bakes -> cloud march + scene depth targets (5 steps) | 26 ms |
| 0.267 | `init: Earth textures (decode + upload)` | **1.832 s** |
| 1.829 | `init: Earth textures done` -> `constellation: building...` | |
| 2.100 | `constellation: 1381359 satellites, 24 types` (24 model builds) | 1.22 s |
| 3.548 | `satellite GPU buffers: 132 MB device-local`, `satellite orbits uploaded` | 267 ms |
| 3.589 | `init: pipelines` -> `init: mesh renderer + environment probes` | 15 ms |
| 3.65+ | mesh renderer models, UI icons, first frame | |

**~3.7 s from instance to first present, with the window white and uncomposited for all of it** -
`App::run()` calls `ctx.init()`, `sim->init()` and `ui.init()` before the first `drawFrame()`, so
nothing is ever presented until init has returned. That is the launch window the entries below died
in. Inside the texture step, one file is 45 % of it:

| texture | pixels (real header) | file | `req_comp` | decode peak | ms |
|---|---|---|---|---|---|
| `earth_elevation.png` | 14999x7500 **RGB8** | 19.2 MB | 1 | 322 -> 107 MB = **429 MB** | **834** |
| `8k_earth_daymap.jpg` | 8192x4096 | 6.5 MB | 4 | 96 -> 128 MB | 255 |
| `8k_earth_nightmap.jpg` | 8192x4096 | 2.5 MB | 4 | 96 -> 128 MB | 242 |
| `8k_earth_clouds.jpg` | 8192x4096 | 11.1 MB | 1 | 96 -> 32 MB | 219 |
| `city_day_detail.png` | 2048x2048 | 6.6 MB | 4 | 12 -> 16 MB | 85 |
| `city_night_detail.png` | 2048x2048 | 4.0 MB | 4 | 16 MB | 73 |
| `8k_earth_specular_map.png` | 8192x4096 **grey8** | 0.8 MB | 1 | 32 MB | 64 |
| `8k_stars_milky_way.jpg` | 4096x2048 | 0.4 MB | 4 | 24 -> 32 MB | 49 |
| `full_moon.png`, `rgba_noise.png` | small | 0.1 MB | 4 | < 1 MB | < 10 each |

The DEM's code comment says `21600x10800`; the header says **14999x7500** (that is the "112 MB
elevation map" these entries mean), and it is **greyscale stored as RGB** - checked row 5000 of the
PNG: r==g==b on every sampled pixel, sea level = 15, the value `oceanMaskCpu` tests for. So
`req_comp=1` still decodes 322 MB of RGB before making the 107 MB single-channel copy: ~430 MB of
live CPU memory, then a 107 MB staging `memcpy`, a 2.3 Mpixel downsample, a 112 Mpixel ocean-mask
scan, one copy-to-image and a 14-level mip chain, all in one call that presents nothing in between.
Summed over the nine textures, ~1.1 GB of decode buffers in 1.83 s. Full design, phases and the
per-texture fixes: `.plans/BOOT_LOADER_PLAN.md`.

## Machine-level resets (tally)


The machine has frozen and been hard-reset at least eight times, all of them while this project was
under active development (a platform event, not an app crash - 4 of the 12 resets in the last 48 h
happened with no app running at all, and shutting the app down does not stop them). Windows leaves two
witnesses on the next boot, and **neither is a crash dump**:

- **WHEA-Logger 1** in the System log: the firmware's raw CPER record, hex in `EventData.RawData`,
  severity `fatal`, `notify=BOOT` (UEFI BERT). Its timestamp fields are **binary, not BCD**, and **UTC**.
  Read what it *says* before reading it as a hardware fault: decoding all 47 of them (below) shows the
  record is the platform reporting **how it was reset** — the power button held 4 s — not something it
  found broken. The word "fatal" is the record's severity class, not a diagnosis.
- **Kernel-Power 41** with `BugcheckCode=0`: Windows never saw a bugcheck, so **no dump exists**
  (no `MEMORY.DMP`, nothing in `Minidump`, no WER 1001). Do not go looking for one.
- **The app's own log.** `satlight_log.txt` is fsynced per line, so where it stops is real evidence;
  a line that was mid-write when the power went instead commits its size and reads all-NUL. (An app
  *abort* leaves a stale `session.lock` too, but it ends in a `FATAL:` line with nothing all-NUL —
  the difference matters, see below.)

`tools/harness/crashes.py` reads all of it, read-only (it runs `wevtutil` on the System log and stats
the run folders; it never writes to the event log, the registry or the machine):

```powershell
python tools/harness/crashes.py                      # the last 72 h: records, sections, live runs
python tools/harness/crashes.py --hours 336          # two weeks, for the tally below
python tools/harness/crashes.py --json > cper.json   # keep the raw evidence forever (UTF-16-safe)
python tools/harness/crashes.py --decode cper.json   # re-read a saved dump, no machine needed
python tools/harness/crashes.py --scan-run harness_runs/<dir>   # what survived in a run folder
python tools/harness/crashes.py --witness harness_runs/<dir>    # write crash_witness.txt into it
python tools/harness/crashes.py --preflight          # exit 3 if the firmware recorded a fatal error
python tools/harness/crashes.py --history            # every fatal record Windows kept, by month
python tools/harness/crashes.py --dump-status        # is the manual-crash key armed, has it ever fired
python tools/harness/crashes.py --export-crashlog D  # one CPER (Intel CrashLog inside) per record
python tools/harness/crashes.py --decode-crashlog D  # Intel's iclg over them (see Decoding the CrashLog)
```

Exit codes: `0` nothing found, `3` a fatal firmware record (`--preflight` too), `2` the event log
could not be read. `tools/harness/run.py` wires this in: `--preflight` before every launch (one
warning line if the *firmware* recorded a fatal error since the newest run folder — a Kernel-Power
41-only reset has no WHEA event behind it, so entry 3's kind of reset is **invisible to `--preflight`**;
the report's "resets with no firmware error record" list is where those appear) and `--witness`
automatically whenever a run ends without `summary.json`, so a dead run carries its own
`crash_witness.txt` (log tail, live Kernel-Power events, the decoded record) across the next reboot.
Both are best-effort — an unreadable event log never fails a run.

**UTC vs local.** The app logs UTC (`gmtime` in `src/Log.cpp`), the firmware stamp is UTC, but the
Windows event *list* prints local time (UTC−7 on this box). So `[04:44:38]` in a run folder and
`21:44:38` in the event log are the same moment — compare in UTC only. `--decode` prints the UTC
instant and shows the binary reading next to the (wrong) BCD one.

**Every section in every record here is a *firmware error record reference***
(`81212A96-09ED-4996-9471-8D729C8E69ED`, UEFI 2.7 §N.2.5; that name is verified against EDK2
`MdePkg/Include/Guid/Cper.h`) — **of type 2, which embeds the record itself.** Until 2026-09-26 this
section said the 7608 bytes were "three pointers" with no device detail; that was wrong. Each section
is a 32-byte header (type 2 = SoC firmware record, a record id, and the format GUID
`8F87F311-C998-4D9E-A0C4-6065518C4F6D` = **Intel CrashLog**) followed by the raw CrashLog: 2560 B from
the PCH's PMC (product `TGP/H`, the Z590's chipset), 512 B of PMC trace, and 4096 B from the CPU's
Punit. Some records carry only the last two. Intel publishes the decoder: **`iclg`**
([github.com/intel/crashlog](https://github.com/intel/crashlog), `iclg-windows.zip`), whose built-in
collateral includes TGP. See *Decoding the CrashLog* below.

## Out of scope: the records before this repository (2026-09-26)

`crashes.py --history` reads `Microsoft-Windows-Kernel-WHEA/Errors`, which keeps fatal records far
longer than the System log does. On this machine that channel reaches back past the project's first
commit, and digging through it stopped being worth the time: **the app cannot be blamed for, or fixed
by, a reset from last year**, and the tally below starts where the freezes started costing development
hours. What the channel is still good for is *shape* - this box has always restarted in bursts (several
within minutes, then quiet for months), which is context for "why did it freeze twice tonight", not
evidence about a build. `--history` is still the way to see it; the per-episode archaeology is not kept
here.

## Decoding the CrashLog

```powershell
python tools/harness/crashes.py --export-crashlog harness_runs/crashlog   # one CPER file per record
# unzip iclg-windows.zip (github.com/intel/crashlog/releases) to tools/harness/iclg/ (gitignored)
python tools/harness/crashes.py --decode-crashlog harness_runs/crashlog   # <time>.json + <time>.info.txt
```

Use `--export-crashlog`, not `iclg extract`: every record here has record id 0 and `iclg extract`
keeps one per id (it kept 2 of 46). `iclg info` on the 2026-09-25 21:57 record lists `PMC rev 3
TGP/H`, `PMC_TRACE rev 1 TGP/H`, `Punit` (product 0x028, no collateral) and two CPU regions. `--decode-crashlog` ends with a
table of the PMC's reset-cause bits per record (`reset_cause_table`).

**Decoded 2026-09-26 — all 46 records, and the answer is FREEZES, forced off with the power button.**
27 of the 46 carry a PMC record; the other 19 carry no PMC record at all (PMC trace + a Punit header +
a CPU region iclg marks invalid — the smaller CPERs are those), so the counts below are over the 27.
**The union of every reset-cause bit that is set anywhere in all 46 records is exactly two fields.**

| n | PMC reset cause | meaning |
|---|---|---|
| 24 | `gblrst_cause_0.pb_ovr` (+ `host_pr_cause_0.cf9`) | **power-button override** — the button held 4 s |
| 3 | `host_pr_cause_0.cf9` only | a software-initiated restart |
| 19 | no PMC record (see above) | — |

`cf9` is the host's software-reset bit, set in all 27; on a forced power-off the platform sets it too,
so it is not a second event. Nothing else has ever been set: every hardware cause the PMC tracks is 0
in **every** record — `cpu_trip` (thermal trip), `syspwr_flr` /
`pchpwr_flr` (power failure), `pmc_3strike` (a core stopped responding), `cpu_thrm_wdt`, the
PMC/ME/TCO watchdogs, `ich_cat_tmp`. So the machine did **not** reset itself: it **hung**, was forced
off, and the firmware filed a "fatal" CrashLog about that reset, which Windows then reports as a fatal
hardware error. The WHEA event is the *consequence* of the freeze, never its cause, and everything above
that reads "the firmware reset the platform" should be read that way. (It also explains entry 3's
Kernel-Power 41 with no record, and the bursts minutes apart: a machine that froze again on the way
back up.)

What that changes: the thing to catch is the **hang**. A hard hang leaves no dump because Windows never
bugchecks — unless it is told to on a key press. As administrator, once: `reg add
HKLM\SYSTEM\CurrentControlSet\Services\kbdhid\Parameters /v CrashOnCtrlScroll /t REG_DWORD /d 1 /f`
(USB keyboards; `i8042prt\Parameters` for PS/2), a *Kernel* or *Automatic* memory dump in System →
Advanced → Startup and Recovery, and a reboot. Then, when it freezes: hold **right Ctrl** and press
**Scroll Lock twice** before reaching for the power button. If the kernel still takes interrupts, the
machine bugchecks with 0xE2 (MANUALLY_INITIATED_CRASH) and writes `C:\Windows\MEMORY.DMP`, showing what
every CPU was doing. If even that does nothing, the hang is below the OS (a bus or device lockup), which
is itself the diagnosis. `blackbox.py`'s last line then says when the machine froze (its samples stop),
and the power-button reset time (Kernel-Power 41) how long it stayed frozen.

**Armed since 2026-09-25 23:58 (checked 2026-09-26).** `crashes.py --dump-status` reads the whole path
back, and it is set up on this machine: `kbdhid\Parameters\CrashOnCtrlScroll = 1` (the USB driver; the
PS/2 one is absent), written 2026-09-25 23:58:02 local — before the 00:11:48 boot, so it was itself
*loaded*: kbdhid reads the key when the driver starts, so one written during a boot catches nothing
until the next one, and a freeze in between would be missed while the key still reads back as armed
(`--dump-status` now prints the write time next to this boot's start and says which side of that line
you are on). `CrashDumpEnabled = 2` = kernel dump, the right size for a hung kernel, with 80 GB free on
`C:`. Nothing has come of it yet: no `C:\Windows\MEMORY.DMP`, an empty `Minidump\`, and the only
bugcheck Windows still holds is from 2026-08-31 (`0x0000010d`, WDF_VIOLATION — a real driver BSOD, 25
days before these freezes, not one of them). **Entry 6 is the first freeze it was armed for, and it was
pressed: the answer is the interesting part (entry 6, below).**

Every row below was confirmed with `--scan-run` (that is where "all-zero" and "zero tail" come from).
A run that is not named in a row finished normally — a reset cost the named folder, and nothing else.
(Entry 6 is not a run folder at all: it was launched by hand, so nothing but the app's own log and the
always-on recorder covers it.)

| # | Reset (local) | Firmware record | Run that was live | What it was doing | Damage |
|---|---------------|-----------------|-------------------|-------------------|--------|
| 1 | 2026-09-25 20:25:34 (Kernel-Power 41 at 20:25:27) | WHEA-Logger 1, fatal CPER, 3 × firmware error record reference | `terrain_views.satcmd` (right after `ocean_rings_sea.satcmd`, which finished at 20:23:28) | app had just launched; the *previous* run's final ~3 s of writes were still unflushed | `ocean_rings_fix\sea_after` (the previous run): **29 all-zero files** — `settings.json`, `summary.json`, `app_stdout.txt`, `perf_profiles\profile_log.jsonl`, the 3 `sea60_e*.json` sidecars and 22 `satellite_models_debug\*.mtl` — plus **4 zero tails** (`sea60_e30/e60/e60_normals.png`, `results.jsonl`), and `land_after\satlight_log.txt` is a fifth. The alt=1200 captures written earlier were fine. Treat every all-zero capture as absent (it will not decode as a PNG either) |
| 2 | 2026-09-25 20:44:33 (KP41 20:44:26) | WHEA-Logger 1, fatal CPER, same 3 sections | `ocean_rings_fix\land_v5_d20` (a *nested* run folder) | app had just launched — `satlight_log.txt` stops at line 7 of 67, right after `Swapchain created`, before `env stars: …` (the next line of a healthy launch) and before the first command ran | that run only: the folder holds `session.lock` + a 7-line log, no captures, no `summary.json`. The other 10 nested runs under `ocean_rings_fix` all have `summary.json` and were untouched, and no new all-zero file exists anywhere. It did kill that experiment (a v5 date A/B), which never captured |
| 3 | 2026-09-25 21:25:42 | **none** — Kernel-Power 41 alone (`BugcheckCode=0`, no BERT/WHEA record: the boot found no firmware error, i.e. it hung or was forced off) | `harness_runs\tw_terms_01` (started 21:25:02, 40 s before the reset; the Release exe had been written 21:24:58) | app had just launched — log stops at line 6, `Logical device created.`, with line 7 committed **all-NUL** (entry 1's exact fingerprint) | that run only: `session.lock` + a 7-line log, no `captures\`, no `summary.json` — none of its 15 per-term captures happened. Its script survives at `build\tw_terms.satcmd` |
| 4 | 2026-09-25 21:58:01 (KP41 21:57:53) | WHEA-Logger 1, fatal CPER, same 3 sections | `harness_runs\tw_day2` (started 21:55:05) | app had just launched — log stops at line 7, `Swapchain created`, with nothing all-NUL (no partial write survived) | that run only (a date A/B): `session.lock` + a 7-line log. The three runs in between (`tw_terms_fixed` 21:47, `tw_terms_fix1` 21:53, `tw_day` 21:54) finished, and the next launch (`20260925_221520_smoke`, 22:15) was clean |
| 5 | 2026-09-26 00:11:48 | **none** — Kernel-Power 41 alone (`BugcheckCode=0`, no BERT/WHEA record: the boot found no firmware error, like entry 3) | `harness_runs\nearcheck` (started 00:11:02; its own `blackbox.csv` ends at 00:11:03.771 local — the freeze — and the boot was 44 s later, so 00:11:48 is the reboot, not the freeze) | app had just launched — log stops at line 13 of 67, `init: Earth textures (decode + upload)`: **the deepest any reset has reached**, 1.23 s into that step and 1.79 s after the launch mark. The per-texture stamps a healthy launch of the same tree wrote 80 min later (`harness_runs\nearcheck2`) put that instant **~430 ms into the `earth_elevation.png` decode** — 14999×7500, 112 MB decoded, 888 ms, the single slowest step of the whole launch. Its own `blackbox.csv` (100 ms) ends at `07:11:03.771Z`, right after the idle→load step: P8/gen1 23.6 W → P0/gen4 1800 MHz (mem 9501), 75.60 W avg (81.31 W instant, peak *0.5 s before the end* — the ramp was still going up), 44 °C, util 14 %, 2153 MiB used (1587 MiB before the launch), CPU 138 % of 3504 MHz — no rail, clock or temperature anywhere near a limit, and nothing after it: the machine was dead for the next 44 s (boot at 00:11:48) | that run only: `session.lock` + a 13-line log, no captures, no `summary.json` — the capture it was launched for never happened. **It also was not running the build it was meant to test**: line 1's stamp is `f9aa040` (`build/generated/version.h`, exe written 2026-09-25 23:43:35), not the `c112930` `build-win-release\Release` exe written 75 s earlier — `run.py` searches only under `build/`, so the release-preset tree is invisible to it (see Gotchas). So this reset is *not* evidence about the change that run was checking |
| 6 | 2026-09-26 00:40:32 (WHEA event at boot; Kernel-Power 41 and the record's own firmware stamp both say 00:40:26 = **the forced power-off, 93 s after the freeze**) | WHEA-Logger 1, fatal CPER, same 3 sections — decoded: `gblrst_cause_0.pb_ovr` + `host_pr_cause_0.cf9`, every fault bit 0 (the 46th record) | **no run folder** — a *by-hand* launch of `build-win-release\Release` (a by-hand launch writes its log beside the exe; the `satlight_log.prev.txt` next to it is `nearcheck2` from 00:30) | app had just launched — log stops at line 21 of 67, `init: texture: assets/textures/earth_elevation.png`, 232 ms before the recorder's last sample. That is the step entry 5 was only inferred to be inside, now printed by name | the app's log, preserved at `harness_runs\crashlog2\20260926T073853_launch_death_app_log.txt` — **the next launch renames it to `satlight_log.prev.txt`**, so copy it out before relaunching. No captures and no `summary.json` (there was no run). The always-on recorder covered it: `harness_runs\blackbox\20260926.csv` ends mid-upload (the block below) |
| 7 | 2026-09-26 01:23:28 (KP41 09:52:34 = the forced power-off **8.5 h** later, so 01:23:28 is the freeze) | WHEA-Logger 1, fatal CPER, same 3 sections — decoded: `gblrst_cause_0.pb_ovr` + `host_pr_cause_0.cf9`, every fault bit 0 (the **47th** record) | `harness_runs\overnight\runs\015_20260926_012323_launch` — iteration 15 of a 10 h soak (`overnight.py --hours 10`, scripts `launch, smoke, launch, viewer_glare`, release exe `c112930` built 2026-09-26 00:38:50) | app had just launched — log stops at line 21 of 67, `init: texture: assets/textures/earth_elevation.png`, **511 ms** before the machine-wide recorder's last sample. Its own `blackbox.csv` (100 ms) ends at 08:23:28.161Z: P0/gen4, 1800/9501, 85.49 W avg (82.67 W inst), 51 °C, util 0 %, **1744 MiB** used (1143 MiB at its first sample: +601 MiB over 2.9 s in ~150-215 MiB steps) — nothing near a limit, and nothing after it: the recorder only came back at 09:57:46Z, **8.5 h** later | that run only: `session.lock` + a 21-line log + a 34-line run csv, no captures, no `summary.json`. The machine-wide file covered it — `harness_runs\blackbox\20260926.csv` ends at 08:23:28.130Z — and `crashes.py`'s report for it is kept at `harness_runs\overnight\reset_20260926_0123.json` |
| 8 | 2026-09-26 10:42:02 (KP41 10:42:46; boot 10:42:42, 40 s later, so 10:42:02 is the freeze) | **none** — Kernel-Power 41 alone (`BugcheckCode=0`, no BERT/WHEA record: that boot found no firmware error at all, like entries 3 and 5; the channel still holds 47) | `harness_runs\overnight\runs\001_20260926_104158_smoke` — iteration 1 of a loop started as a recorder start/stop test (`--hours 0.0005`), which launched once because the deadline was measured *after* the 50 s recorder settle (fixed the same day) | app had just launched — log stops at line 21 of 67, `init: texture: assets/textures/earth_elevation.png`, **265 ms** before the day file's last sample. The day file (100 ms) ends at 17:42:02.689Z: P0/gen4, 1800/9501, 82.01 W avg (96.03 W inst), 44 °C, util 3 %, **1595 MiB** used (1029 MiB 1.5 s earlier, +566 MiB in ~150-210 MiB steps) — the block below | that run only: `session.lock` + a 21-line log + a 27-sample run csv, no captures, no `summary.json`. Build: the A/B exe (`ca755ee`, built 10:38:12) on the **single-shot arm** (`SATLIGHTSIM_ELEV_UPLOAD` unset), so this reset says nothing about the chunked path |
| 9 | 2026-09-26 ~11:34:43 (log's last line 18:34:42.648Z; boot 11:36:54, KP41 11:37:05, WHEA 11:37:25) | WHEA-Logger 1, fatal CPER — **not decoded yet** (run `--export-crashlog` / `--decode-crashlog`) | `harness_runs\boot_a_on` — the **boot-screen Phase A build** (working tree on `f9aa040` + uncommitted Phase A; the stamp says `f9aa040`), `--boot-screen` on. **No recorder**: no `blackbox.csv` in the run, and the day file ends at 10:53:56 | app had just launched and **the boot frame was on screen** (`boot: starting... (67 ms)`). Log stops at line 20, `init: texture: assets/textures/city_day_detail.png` — a **2048x2048 PNG (85 ms)**, *not* the DEM, 1.18 s after `Vulkan instance created`, right after the three 8K JPEG decodes + uploads | that run only: `session.lock` + a 20-line log. It also cost the previous agent's whole session context — see *Assessment after entry 9* below |

**All eight resets that caught a launch did it in its first ~4 s — the launch window** — and
`satlight_log.txt` says where: a healthy launch writes 67 lines and ends at `sky pipeline: FULL
sat_sky.frag`. Entries 1-4 stopped at line 6-7, within one line of each other (1 and 3 at `Logical
device created.` with line 7 committed all-NUL; 2 and 4 at `Swapchain created`) — those four ran builds
from *before* 2026-09-26, which had no `init: <step>` breadcrumbs to stop on (`git log -S 'init:
buffers'` dates them to `c112930`). Entry 5 is the first reset caught by a build that had them, and it
landed on line 13, `init: Earth textures (decode + upload)` — 1.23 s into that step, i.e. *past* the
device/swapchain pair the earlier four died in, inside the launch's largest CPU burst (nine texture
decodes + uploads, 8K JPEGs and the 14999×7500 elevation map among them; `SatelliteSim::init` now logs
one line per texture, see *The flight recorder* in `docs/HARNESS.md`). The lines after it in a healthy launch are `env stars: …` and
the constellation build (27 model lobe validations, 1.38 M satellites, the 132 MB device-local buffer
upload). So the window is: Vulkan logical device up → swapchain / first GPU work → the noise bakes →
the Earth texture decode + upload → star and constellation build, and nothing past that has ever been
hit.

**Entry 6: the manual-crash key was armed and pressed, and it answered nothing — so the freeze is
below the OS.** The key had been armed since 23:58 the evening before, i.e. *before* entry 6's boot
(00:11:48), so kbdhid had it loaded and live (`crashes.py --dump-status` prints exactly that
reasoning, because a key written during a boot catches nothing until the next one), and it was pressed
— right Ctrl + Scroll Lock twice, several times — while the machine was frozen. Still no
`C:\Windows\MEMORY.DMP`, `Minidump\` still empty, no WER 1001, and entry 6's Kernel-Power 41 carries
`BugcheckCode=0`. That matters: with the key armed, 0xE2 is raised *inside* the keyboard ISR, so
Windows needs nothing but an interrupt to write a kernel dump. Nothing came out, so **the frozen
platform was not delivering keyboard interrupts to Windows at all** — it stopped where the OS does not
run, and no recorder that lives in the OS can see that far. It is a finding, not a failed attempt. (The
one way to lose it: the *left* Ctrl, which kbdhid ignores by design.)

Entry 6 also came with a name on it. The per-texture breadcrumb printed `init: texture:
assets/textures/earth_elevation.png` at `07:38:53.491Z`, 232 ms before the flight recorder's last
sample — the same file and same step entry 5's timing was measured into. The recorder's last second:

| UTC | state | pwr avg / inst | clocks gr / mem | °C | util | VRAM used |
|---|---|---|---|---|---|---|
| 07:38:52.691 | P8, gen1 | 23.4 / 23.5 W | 210 / 405 | 39 | 7 % | 1012 MiB |
| 07:38:52.954 | **P0, gen4** | 34.4 / 81.3 W | 1800 / 9501 | 43 | 10 % | 1142 MiB |
| 07:38:53.219 | P0, gen4 | 34.4 / 81.3 W | 1800 / 9501 | 43 | 10 % | 1355 MiB |
| 07:38:53.470 | P0, gen4 | **61.6 / 79.7 W** | 1800 / 9501 | 43 | 5 % | 1547 MiB |
| 07:38:53.723 | P0, gen4 | 61.6 / 79.7 W | 1800 / 9501 | 43 | 5 % | **1568 MiB — last sample** |

Nothing is near a limit — 43 °C, 62 W of a ~115 W budget, 5 % utilisation, CPU at its usual 138 % of
3504 MHz — and the machine dies *while VRAM is being filled*: +556 MiB in the last 800 ms, in steps of
~112-213 MiB, which is what one 112 MB texture costs (a 112 MiB staging buffer plus a 112 MiB device
image), on the gen4 link it had switched to 0.7 s earlier. Same profile as entry 5, same file, 26
minutes and one reset apart, with clean launches of the same tree in between (00:30's `nearcheck2`
wrote the stamps that price the step). The launch does that upload every time — so the thing to chase
is the *coincidence*, not a hardware limit that was reached. The last sample is 00:38:53.723 local and
the platform says it was forced off at 00:40:26, so this machine sat frozen — answering nothing, no
interrupts, no dump key — for another **93 s** before the power button ended it (those are the 4 s the
`pb_ovr` bit reports, started ~00:40:22).

**Entry 7 is that upload, caught end to end (01:23:28).** The 01:17:53Z soak (`overnight.py --hours 10`,
release exe `c112930` built 00:38:50, iterating `launch, smoke, launch, viewer_glare`) was 15 iterations
in when iteration 15 died. Its mark is in `harness_runs\overnight\loop.csv` at 08:23:23.990Z, the app's
own first fsynced line (`Vulkan instance created.`) is **2.6 s** later — that gap is process start, not
work — and the last sample either recorder wrote is **4.1 s** after the mark. The app's log stops at line
21 of 67, on `init: texture: assets/textures/earth_elevation.png`, 511 ms before the machine-wide file's
last sample: the same line entries 6 and 8 stopped on, and the same step entry 5 died inside. VRAM ran
1143 → 1744 MiB across the run's own 34-sample csv, P0/gen4 1800/9501, 85.49 W avg / 82.67 W inst, 51 °C,
util 0 % — nothing near a limit — and then nothing: the day file ends at 08:23:28.130Z and does not
resume until 09:57:46Z, 8.5 h later, after the power button (~09:52:26; KP41 at 09:52:34). That boot found
the **47th** firmware record, and its decode is the same `gblrst_cause_0.pb_ovr` + `host_pr_cause_0.cf9`
with every fault bit 0 — the platform reporting *how it was forced off*, not something it found broken.

**Entry 8 is that upload again, 9 h later, and it is a loop's own first launch (10:42:02).** The soak was
started with `--hours 0.0005` as a recorder start/stop test — no launches intended — but the deadline was
measured from *after* the 50 s recorder settle, so iteration 1 ran anyway: mark `iter 1 start smoke` at
17:41:58.657Z, app's first fsynced line 2.6 s later, last sample **4.0 s** after the mark. Its log stops
at line 21 of 67, on `init: texture: assets/textures/earth_elevation.png`, **265 ms** before the day
file's last sample. The recorder's last second and a half:

| UTC | state | pwr avg / inst | clocks gr / mem | °C | util | VRAM used |
|---|---|---|---|---|---|---|
| 17:42:01.151 | P8, gen1 | 23.8 / 23.7 W | 210 / 405 | 39 | 14 % | 1029 MiB |
| 17:42:01.708 | **P0, gen4** | 39.8 / **96.0 W** | 1800 / 9501 | 43 | 21 % | 1175 MiB |
| 17:42:02.035 | P0, gen4 | 39.8 / 96.0 W | 1800 / 9501 | 43 | 21 % | 1381 MiB |
| 17:42:02.251 | P0, gen4 | 65.7 / 80.4 W | 1800 / 9501 | 44 | 7 % | 1552 MiB |
| 17:42:02.471 | P0, gen4 | 65.7 / 80.4 W | 1800 / 9501 | 44 | 7 % | 1595 MiB |
| 17:42:02.689 | P0, gen4 | **82.0 / 80.6 W — last sample** | 1800 / 9501 | 44 | 3 % | **1595 MiB** |

Nothing is near a limit — 44 °C, 82 W of a ~115 W budget, 3 % utilisation, VRAM +566 MiB in 1.5 s in steps
of ~150-210 MiB (the 112 MB elevation map: a staging buffer plus a device image, as in entries 6-7) — on
the gen4 link the launch had retrained 1.5 s earlier. The machine was back 40 s later (boot 10:42:42,
KP41 10:42:46), and that boot found **no firmware record at all**: this reset, like entries 3 and 5, has
only Kernel-Power 41 to witness it, and the channel still holds 47 records. The app was the **A/B build**
(`ca755ee`, built 10:38:12) on the **single-shot arm** — `SATLIGHTSIM_ELEV_UPLOAD` unset — so nothing here
measures the chunked path.

**It is not deterministic, and it is a development problem.** 12 run folders finished between entry 2
and entry 3, and 3 more between entry 3 and entry 4, so there is no code change to point at and no
bisect to run — and in the last 48 h, **4 of the 12 resets happened with no app running at all**
(09-24 23:51, 09-25 08:29, 08:40, 16:34; `crashes.py` prints who was live, if anyone), so entry 6's
by-hand launch only had the recorder watching it. That is the platform's own fragility, and it is why
the working assumption here is **per-launch risk, engineered around**, not a culprit to convict: one
launch at a time, split scripts rather than writing one long one, since a reset costs that run (and its
unflushed tail) and nothing else. What the app can do about the part it owns is in the next paragraph
and in `.plans/BOOT_LOADER_PLAN.md`.

**The odds, as of entry 8.** `overnight.py` has now driven 16 launches — 15 through the 2026-09-25→26
night, 1 this morning — and **2 died**: the night's last (entry 7) and the morning's first (entry 8), both
within ~4 s of the launch, both with the app stopped on `init: texture: …earth_elevation.png` (entries 5
and 6 stopped inside and on the same step). **Four of the eight resets landed on the launch, and three of
those on one line of it** — a line that is 834 ms of decoding a greyscale map stored as RGB into a 429 MB
peak buffer, inside a 1.83 s texture step, inside a ~3.7 s init that presents no frame at all (the
measurement is at the top of this file). That is not something to leave alone: the answer is to make the
launch small and interruptible rather than to keep re-rolling it. `.plans/BOOT_LOADER_PLAN.md` is that
work — a boot/loading screen running the init as named per-frame steps, the DEM split decode/derive/
upload-N, and the assets repacked so the decode buffers stop being three times what the shaders read.
`SATLIGHTSIM_ELEV_UPLOAD` (`SatelliteSim::elevUploadChunks`) is the one carried-over experiment: it
splits the 112 MB upload into `N` `vkCmdCopyBufferToImage` slices recorded one per frame, and it is
**unmeasured so far** — every reset on a build that has it (entry 8) ran the single-shot arm — so the
plan promotes the chunked arm to the default instead of asking a soak to remember to carry it. Two
practical notes that do not change: the risk is per *launch*, so a loop's first iteration is as exposed
as its hundredth and what a run *contains* changes nothing; and the loop logs `--exe`/`--scripts` with
every run for exactly this reason — `4 of the 12` resets happened with nothing running at all, so a
reset is not a verdict on the build or the script in the run folder next to it.

**A reset is not a crash.** A stale `session.lock` only means "the process never cleaned up", which is
also what an *app abort* looks like — and aborts are common here: `harness_runs\bisect1`, `bisect2` and
`tv1p3` (2026-09-25 02:48-02:49) end in `FATAL: vkQueueSubmit failed.`, and `tw_terms_02` (21:44:38) and
`tw_terms_03` (21:45:24), both launched *after* entry 3's reset, end in `FATAL: SatelliteSim: failed to
create cloud_noise bake pipeline` — the bug fixed at 21:47, after which `tw_terms_fixed` and
`tw_terms_fix1` both finished. All five aborted the process and lost nothing to an all-zero write. The
reset is the one whose log simply *stops*, usually with the next line committed as NUL.

Earlier occurrences are out of scope - see *Out of scope: the records before this repository* above.

To check a single file by hand:
`python -c "import sys;b=open(sys.argv[1],'rb').read();print('all-zero' if b and not any(b) else 'ok')" <file>`
— `--scan-run` does that (and the zero-tail scan) for a whole folder.

*Every number in this file comes from a file that was fsynced before the machine stopped: the recorder
(`docs/HARNESS.md`, *The flight recorder*), the app's own log, or the firmware's record of how it was
reset. Keep the tally in step with reality - a reset that catches a launch is evidence about the platform,
and the recorder is what turns it into evidence about the app.*


## Appendix: the flight recorder (`blackbox.py`) and the soak (`overnight.py`)

Moved here from `docs/HARNESS.md` when the freezes were resolved. A freeze erases memory and Windows
writes no dump, so the only record of what the hardware did is one already on disk.
`tools/harness/blackbox.py` samples the GPU through `nvidia-smi` (P-state, power, clocks,
temperature, utilisation, **PCIe link gen/width**, clock limiter) and the CPU (perf counters), and
fsyncs every line.

```powershell
python tools/harness/run.py <script> --forensics      # blackbox.csv in the run folder + event-log preflight
python tools/harness/blackbox.py --daemon --gpu-ms 100 # machine-wide: harness_runs/blackbox/<UTC day>.csv
python tools/harness/blackbox.py --gaps harness_runs/blackbox/<day>.csv   # every discontinuity, classified
python tools/harness/blackbox.py --summary harness_runs/blackbox --at "2026-09-25 21:57:53"   # local time
pythonw tools/harness/overnight.py --hours 10 --fix-power   # launch in a loop until the machine dies
```

- **`--gaps` is the morning-after read.** A day file simply *ending* (nothing resumed it) means its
  last sample is the freeze; telemetry resuming under a different pid means the recorder died with
  the machine; a `blackbox stop` / `daemon stopped` mark means a clean stop.
- **Opt-in.** Nothing starts a recorder at logon. `overnight.py` starts one for its soak (and stops the
  one it started), passes `--forensics` to every `run.py`, waits 50 s for a fresh recorder's P0 hold to
  settle before the first launch, and writes `harness_runs/overnight/overnight.log`, `loop.csv` and
  `last_launch.json` (the run folder live *now*, written before each launch).
- **Rate.** 100 ms (~95 MB/night) is what resolves an upload's VRAM steps; 250 ms does not.
- **After a reset, check the recorder was really running**: `harness_runs/blackbox/daemon.log` and the
  newest day file's mtime. The lock names its holder by (pid, creation time) and treats a pre-boot lock
  as stale (a bare pid, reused within the minute after a reset, used to lock the next recorder out
  silently).
- Its `utc` and `epoch` columns agree since 2026-09-26 07:59Z; before that the `utc` column can read
  1 ms early, so the `epoch` column is the authority.
