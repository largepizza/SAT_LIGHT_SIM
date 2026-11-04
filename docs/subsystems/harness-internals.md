# Automation harness internals

How the harness is wired into the app (the user manual is docs/HARNESS.md).

Part of the subsystem notes (`docs/subsystems/`, map in `AGENTS.md` §4). Moved verbatim from
`CLAUDE.md` on 2026-11-03. Do not read this file whole: `python tools/outline.py docs/subsystems/harness-internals.md`
lists its sections and topics with line numbers. New notes go next to the section they belong to.

## Subsystem: Automation harness (2026-09-25)

User guide and command reference: **docs/HARNESS.md** (keep its table in step with
`harnessExec()` and `kHelp`). Architecture, for editing it:

- **Two halves.** `src/Harness.h/.cpp` is app-independent: argv options, the statement parser, the
  `Runner` (one command at a time across frames, `results.jsonl`, `summary.json`, the live inbox,
  the watchdog). `src/simulations/SatelliteSimHarness.cpp` is what each command does to the sim,
  plus `harnessStateJson()` (the capture sidecar) and the console's key handling.
- **Where it runs:** `harnessTick()` is the second statement of `buildUI` (after
  `beginCpuFrameTiming`), so a command's effect is in the frame it ran in and a `capture` after it
  records that frame. Time/observer/follow commands also re-run `updatePositions()` immediately so
  a `state` or sidecar in the same frame is not one frame stale.
- **Hooks it owns:** `Simulation::frameDt()` (fixed step; 0.5 s during `wait settle`, which also
  holds sim time), `wantsQuit()`, `onChar()`/`capturesKeyboard()` (the console; App leaves Esc to
  it), `Paths::setUserDataDirOverride()` (main.cpp points it at the run folder before `Log::init`),
  `wantsCleanScreenshot()` honouring `screenshotIncludeUI`, the crop/scale step in
  `finalizeScreenshot()`, `UIRenderer::requestLayoutDump()`.
- **Settings go through the settings.json path:** `loadSettings()` is now file read +
  `applySettingsJson(j, isPatch=false)`; `saveSettings()` is `buildSettingsJson()` + write. `set`
  applies a one-key patch (`isPatch=true`: no schema check, no intro re-arm, and no preset
  re-derivation unless the patch names the preset); `get` reads `buildSettingsJson()`. So every
  persisted setting is scriptable with no per-setting code — add a setting to those two functions
  and the harness has it.
- Camera paths (`path key/play`) and `overlay` text are harness state too (`harnessPath_`,
  `harnessOverlays_`); `path play` owns the clock (fixed 1/fps via `harnessFixedDtOverride_`, sim
  time set per frame) so a recording is uniform in time however slowly frames encode.
- **The player's camera lock is scriptable:** `track on|off` drives the same `startTrack()` /
  `stopTrack()` the selection panel's Track button does (nothing to do with `camera track <target>`,
  which is this harness's own aim-every-frame), and `state` reports it twice: `camera.tracking` and
  `selection.track`. `tools/harness/scripts/track.satcmd` is the worked example and `selftest.py`'s
  `track_toggle` is the regression test — the drawn evidence comes straight out of a `ui dump`
  (four `SelActBtn`, four `SelReticuleTick` only while locked).
- **`observer agl=` reads the GPU's ground back** (`terrainFrameBuf.y` -> host-mapped
  `terrainFrameReadBuf`, copied every frame after the depth pass): the CPU's `cpuTerrainHeightM` is
  an 18 km/px DEM copy, and until 2026-09-25 `agl` used it — the Big Sur golden view (agl=30) stood
  at 758 m over ground that is really at 342 m. `state`'s observer block used to add the terrain to
  the height offset a second time as well.
- `harnessRunner_` is null outside a harness run (and before the console's first use), and every
  hook is then a no-op. The first-run preset seed, intro, first-run notices, music and toasts are
  all suppressed in a harness run.
- **Launch spacing: the harness waits 30 s after an app exit before the next launch**
  (`tools/harness/launchgate.py`, used by `run.py` and `live.py start`). Back-to-back relaunches are
  the leading suspect for the machine freezes (docs/FREEZES.md, still unresolved; BIOS Gen3 did not
  fix them). For many batches use one `live.py` app, not a `run.py` loop. Freeze forensics
  (`blackbox.py`, `crashes.py`, `overnight.py`) are opt-in: `run.py --forensics`.
- **The loading screen runs in harness launches too** (its frames precede the script).
  `run.py --boot-capture` writes them to `captures/boot_NN.png`; see *Loading screen* below.
