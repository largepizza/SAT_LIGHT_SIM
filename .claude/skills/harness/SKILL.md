---
name: harness
description: Run, script and look at the Sat Light Sim app through the automation harness (tools/harness/run.py, live.py) — captures, perf numbers, state, UI dumps, debug views, issue-report replays. Use whenever you need to see or measure a change in the real app, reproduce a report, or check a view. Carries the launch-safety rules and the cheap way to read results and images.
---

# harness — see the app without burning tokens

The full manual is `docs/HARNESS.md` (130 KB). **Do not read it.** Look up what you need:
`python tools/harness/hdoc.py <command>` (reference rows), `-s "<section>"` (one section), `--list` (all names).

## 1. Before launching

- **Build Release** (`cmake --build build --config Release --target SatLightSim`). Shader-only change:
  `--target CompileShaders`, then in a running app `shaders reload` (no relaunch). `run.py` takes the freshest
  `build/Release/SAT_LIGHT_SIM_V_*.exe`; another tree needs `--exe`. In a worktree, build that worktree's `build/`.
- **Launches can freeze the machine.** Never launch by hand, never loop `run.py`; the launch gate spaces launches
  30 s and caps app processes (`SATLIGHT_HARNESS_MAX_APPS`, default 2). Several rounds of checks → **one `live.py`
  app**: `python tools/harness/live.py status` first (another agent's app may be up: never stop it), else
  `live.py start [--settings user_data/settings.json]`, `live.py send "..."` / `send -f x.satcmd`, `live.py stop` at the end.
- One self-contained check → `python tools/harness/run.py x.satcmd --quiet --out harness_runs/<name>` (or `-c "cmd; cmd"`).
  `--quiet` prints only errors and the status line. The first launch after a shader rebuild is ~10 s slower
  (pipeline compile): never time that run.

## 2. Writing a script

- Pin the moment: `time set <ISO>` then `time pause`. Chains of `time sun` move the clock relative to now — an A/B
  across two runs must use the identical command sequence.
- Settle before any capture or measurement: `wait settle` (or `wait settle auto`). After `report load`, a time
  jump or a cloud-field setting change, the clipmap / Sun volume refill for ~150 frames: `wait 160` before timing.
- A user's view: `report load "<issue dir with forward slashes>" shot=N [trail=off]` or `snapshot <log> index=`.
  Their settings come with it; everything later in that script runs on them.
- Settings: `set <key> <value>` (any settings.json key; `get <key>` to read). Presets: `preset Medium`.
- Debug views and probes before theories: `debugview <name>` (`hdoc.py debugview` lists them), `probe x y`.

## 3. Reading results (never dump them)

`python tools/harness/res.py [run dir | name part | live]` — the newest run by default.

| Want | Command |
|---|---|
| one line per command | `res.py <run>` (`--errors` for failures only) |
| a few fields | `res.py <run> -c state -g clouds_v2.pass_ms -g 'weather.cpu_*'` |
| what a result holds | `res.py <run> -c state --keys clouds_v2` (key, type, size) |
| timing table | `res.py <run> --perf -b cloud_march,sky_background_draw` |
| grouped A/B reps | `res.py live --ab` (name perfs `perf name=<view>_<variant>_r<k>`) |

A `perf` result embeds a ~100 KB `state`; a `state` result is bigger. Printing either whole, `cat results.jsonl`
or writing a one-off parser is the most common waste in this repo's transcripts.

## 4. Looking at images (each full frame costs ~1.5k tokens to view)

- Numbers first: `tools/harness/.venv/Scripts/python tools/harness/imgtools.py stats a.png` (luminance grid,
  clipping), `diff a.png b.png -o heat.png` (mean / max / changed fraction / bbox), `seams a.png`.
- Then look at the part that matters: `capture name crop=x,y,w,h scale=4` in the script, or
  `imgtools.py crop a.png x y w h --scale 3 -o c.png`; many variants → `imgtools.py sheet dir -o sheet.png`.
- `capture name scale=0.5` when the whole frame matters but detail does not.
- Motion problems (ghosting, smearing, flicker) do not show in a settled `capture`: `path play record=` /
  `tools/harness/tstab.py`, `flicker.py`; matrix-wide checks: `matrix_suite.py --quick` (hdoc.py -s "Matrix").

## 5. Recipes

```
# view + perf (no images)
time set 2036-11-22T02:06:22Z; time pause; observer lat=61.2 lon=-149.9 agl=10; camera az=0 el=20 fov=80
wait settle; perf frames=120 name=anchorage
# satellite close-up
select const "Starlink Gen1"; follow offset=-30,0,10; wait settle; capture sat
# UI layout
ui open settings tab=Controls; wait 3; ui dump controls; capture controls ui=on   # read text_overlaps first
```

Timing A/Bs (shader or setting variants interleaved in one app): the `perf-ab` skill.
