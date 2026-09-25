# Automation harness

A small command language that drives the running app: put the observer anywhere, set the time
by Sun elevation, change any setting or knockout bit, select or fly to satellites, then capture
screenshots (each with a JSON record of the exact state), dump the UI layout, sample GPU/CPU
timings, or run the knockout sweep. Scripts are deterministic: running one twice gives
bit-identical images.

Use it for anything that needs the real renderer: shader and terrain changes, image A/Bs,
performance, UI layout, camera shots. Things that don't need the renderer (photometry, orbits,
benchmarks) stay in `SatModelTool`, which is faster and runs in CI.

> **Agents:** this is the one sanctioned way to launch the app yourself. A harness run exits on
> its own, never touches the user's settings, and is muted. Interactive "feel" is still for the
> user to judge (see CLAUDE.md).

## Quick start

```bash
cmake --build build --config Release --target SatLightSim     # use Release: PNG encoding in Debug is very slow
python tools/harness/run.py tools/harness/scripts/smoke.satcmd  # ~10 s, prints each command's result
python tools/harness/run.py -c "observer lat=46.55 lon=7.98 agl=50; time sun 10 rising; camera az=200 el=5; wait settle; capture alps"
```

`run.py` prints the run folder, one line per command (`ok` + message, or `ERR` + reason), the
captures written, and a status line. It exits non-zero if the app crashed, timed out, or any
command failed.

## Four ways in, one language

| Front end | Use | How |
|---|---|---|
| `tools/harness/run.py <script>` | batch: one script, one process, exits when done | `-c "cmd; cmd"` for inline commands |
| `tools/harness/live.py` | many small batches against one running app (no ~10 s startup each time) | `start`, `send "cmds"`, `send -f file`, `status`, `stop` |
| the **`~` console** in the app | a person typing the same commands | `` ` `` toggles; Enter runs, Up/Down history, Esc closes |
| exe flags | anything else | `SAT_LIGHT_SIM_V_*.exe --script f.satcmd --out dir` (see below) |

Exe flags: `--script <file>`, `--live <dir>`, `--out <dir>`, `--window WxH` (default 1600x900),
`--settings <settings.json>` (start from these settings instead of the defaults), `--user-data`
(use the normal user data folder, not the run folder), `--fixed-dt <s>` (default 1/60, 0 = real
time), `--timeout <s>` (watchdog), `--stay` (keep running after the script), `--sound`.

The console, used outside a harness run, writes to `harness_runs/console_<time>/` next to the exe,
runs in real time, and never exits by itself.

## The run folder

```
harness_runs/<stamp>_<script>/
  results.jsonl        one line per command: src (file:line), cmd, ok, error, frames, wall_ms, result{}
  summary.json         status: ok | errors | timeout | script_error | closed; counts; commit; log path
  captures/<name>.png  + <name>.json sidecar: the command, size, and the full state at capture time
  captures/<name>.layout.json   `ui dump`
  captures/<name>.state.json    `state <name>`
  perf_profiles/profile_log.jsonl   `perf name=...` samples and `sweep` records
  satlight_log.txt, settings.json, app_stdout.txt, <script>.satcmd (a copy of what ran)
```

Each run gets its own user-data folder, so it starts from the built-in defaults, never reads or
writes your `settings.json`, and a killed run can't trigger crash recovery on your next launch.
Use `--settings path/to/settings.json` to start from particular settings.

## Script syntax

One command per line or separated by `;`. `#` starts a comment. Arguments are positional words
or `key=value`. Double quotes group words with spaces (`select const "Starlink Gen1"`).

```
preset High
observer lat=46.55 lon=7.98 agl=50      # Interlaken, 50 m above the ground
time sun -4 setting                      # dusk: the Sun 4 degrees below the horizon, going down
time pause
camera az=280 el=3 fov=60
wait settle                              # let every eased quantity converge (see Determinism)
capture dusk
knockout +terrain_march ; wait settle 10 ; capture dusk_noterrain
```

## Command reference

| Command | What it does |
|---|---|
| `wait <frames>` | wait N frames |
| `wait seconds <s>` | wait wall-clock seconds |
| `wait settle [frames]` (or `settle`) | hold sim time and run 40 (or N) frames at a 0.5 s step, so eased values converge: sky glare, the dark-sky dome, mesh fades, beam-light fades, environment probes. Use before every capture that follows a change |
| `time set <ISO>` | e.g. `2036-06-21T04:00:00Z`. **The sim clock is not real UTC** (no GMST term), so prefer `time sun` |
| `time sun <el> [rising\|setting]` | the time nearest now (within 12 h) when the Sun is at `<el>` degrees for this observer. `time sun noon` / `time sun midnight` |
| `time add <s>`, `time j2000 <s>` | relative / absolute (seconds since J2000) |
| `time pause`, `time play`, `time scale <1x\|10x\|1m\|5m\|1h\|1d\|1w\|1mo\|1yr>`, `time reverse on\|off` | |
| `observer lat= lon= [agl=\|alt=]` | move the observer; `agl` = metres above the terrain, `alt` = above sea level. Keeps the camera heading. Ends follow mode |
| `camera az= el= fov=` | azimuth (0 = north, 90 = east), elevation, vertical FOV (0.5-120) in degrees |
| `camera look <sun\|moon\|sel\|planet>` | aim once. `sel` = the selected satellite or planet |
| `camera track <...\|off>` | re-aim every frame (a moving satellite stays centred) |
| `select sat <index>` | by roster index |
| `select const "<name>" [n=<k>]` | the constellation's member highest in the observer's sky, or its k-th member. `const list` shows names |
| `select planet <name>`, `select none` | |
| `follow [sat=<i>] [offset=along,cross,radial]`, `follow off` | fly with a satellite (types with a geometry model). The offset is in metres in the satellite's frame |
| `const list`, `const "<name>"\|all on\|off [highlight=on\|off]` | constellation visibility |
| `get [section[.key]]` | any persisted setting; `get` alone lists them all (the keys are `settings.json`'s) |
| `set <section.key> <value>` (also `key=value`, several per line) | change settings through the same code path `settings.json` loads through. Unknown keys and wrong types are errors. Doesn't change the preset label |
| `preset <Planetarium\|Low\|Medium\|High\|Ultra\|Potato\|Custom>` | apply a graphics preset (it overwrites knockouts and quality sliders) |
| `knockout none\|<mask>\|+key\|-key\|key ...`, `knockout list` | debug knockout bits by stable key (`terrain_march`, `volumetric_cloud_march`, ...; `potato_sky`, `lite_sky`). Sets the preset to Custom, like the Display tab does |
| `capture <name> [ui=on] [crop=x,y,w,h] [scale=s]` | PNG of the frame (no UI unless `ui=on`), cropped then scaled: `scale<1` box-filters down, `scale>1` enlarges with nearest neighbour for pixel-level inspection. Writes `<name>.json` with the state |
| `state [name]` | the full state as the command result (and a file if named) |
| `perf [frames=60] [name=]` | average raw GPU timestamp buckets and CPU buckets over N frames, plus the GPU total and wall frame-time distributions. `name=` also appends it to the run's perf log |
| `sweep` | the automated knockout sweep (≈15 s); returns the whole record |
| `ui show\|hide`, `ui scale <0.75-2>` | HUD visibility and UI scale |
| `ui open <settings [tab=Name]\|viewcontrols\|trace\|info\|viewer\|console>`, `ui close <name\|all>` | windows (`info`/`viewer`/`trace` need a selected satellite). An advanced tab turns on "show advanced settings" |
| `ui dump [name]` | every drawn rect/text/image with its box and element id, plus checks: text cut by its scissor, text off the window, and overlapping text under the same scissor |
| `window <W>x<H>` | resize the window and wait for the new swapchain |
| `log <text>`, `help`, `quit` | |

A command that fails is recorded with its reason and the script continues. The run's status is
then `errors`.

## Determinism

What makes two runs identical, and what breaks it:

- **Fixed frame step.** The sim receives `--fixed-dt` (1/60 s) each frame, not wall time, so time
  advances by exactly the same amount per frame regardless of frame rate.
- **`wait settle` before a capture.** Many values ease toward their target over seconds of real
  time. `wait settle` holds sim time and runs frames at a 0.5 s step. The HUD's fps readout during
  a settle is meaningless.
- **Set the preset explicitly.** A fresh run folder seeds the preset from the device type
  (Medium on a discrete GPU, Low otherwise). Put `preset <name>` first.
- **`time pause`** unless the script is about motion.
- Same window size and build. Verified: two runs of `determinism.satcmd` are bit-identical.

## Recipes

**Performance, no images.** `perf` returns numbers only:

```
preset Medium
observer lat=61.2 lon=-149.9 agl=10      # Anchorage, the beam-heavy worst case
time sun -12
camera az=0 el=20 fov=80
wait settle
perf frames=120 name=anchorage_medium
sweep
```

`results.jsonl` holds `gpu_ms` per bucket, `gpu_total_ms` {mean, p50, p90, max}, `cpu_ms`, and
`wall_frame_ms`. The sweep record is the same format `analyze_profile.py` already reads (it is
also in `perf_profiles/profile_log.jsonl`). Frame time is capped by VSync; GPU timestamps are not.

**Shader A/B at fixed views.** Capture a baseline, change a knockout or setting, capture again,
then diff:

```bash
python tools/harness/run.py views.satcmd --out harness_runs/before
# ...edit the shader, rebuild...
python tools/harness/run.py views.satcmd --out harness_runs/after
tools/harness/.venv/Scripts/python tools/harness/imgtools.py diff harness_runs/before/captures/v1.png harness_runs/after/captures/v1.png -o heat.png
```

**Satellite close-up.** `select const "Starlink Gen1"; follow offset=-30,0,10; wait settle; capture sat`.

**UI check.** `ui scale 2.0; ui open settings tab=Controls; wait 3; ui dump controls; capture controls ui=on`,
then read `text_overlaps` in the result before looking at the picture.

## Image tools

`tools/harness/imgtools.py` (needs Pillow + numpy; set up once with
`py -3 -m venv tools/harness/.venv` and
`tools/harness/.venv/Scripts/python -m pip install -r tools/harness/requirements.txt`). Every
subcommand prints JSON first, so you can check numbers before spending tokens on pictures:

| | |
|---|---|
| `sheet <pngs or run dir> -o out.png [--cols 3] [--width 480]` | labelled contact sheet: many variants in one image |
| `diff a.png b.png [-o heat.png]` | mean/max/RMS difference, PSNR, changed fraction and its bounding box, an amplified heatmap |
| `stats a.png` | luminance percentiles, clipped black/white fractions, a 4x4 grid of mean luminance |
| `crop a.png x y w h --scale 4 -o out.png` | pixel-exact enlargement |
| `seams a.png` | rows/columns whose mean luminance jumps against their neighbours (bands, tile seams) |

For multimodal review: models see large images downscaled, so a one-pixel feature in a
1600x900 frame can vanish. Use `capture ... crop=... scale=4` or `imgtools crop` for anything
small, and `sheet` to compare many variants in one image.

## Verifying the harness

`python tools/harness/selftest.py` (≈2 minutes) runs real scripts and checks capture sizes and
sidecars, bit-identical determinism, a knockout changing and then restoring the image, settings
round-trips and error reporting, script parse errors, `wait settle` holding time, `time sun`
accuracy, perf and sweep numbers, selection/follow/`ui dump`, the watchdog, and live mode. Run it
after changing `src/Harness.*` or `src/simulations/SatelliteSimHarness.cpp`.

## Gotchas

- **Build Release.** A Debug build encodes each 1600x900 PNG in tens of seconds.
- **The window must stay visible (not minimized)**, or the swapchain stops presenting. It opens
  without taking focus.
- Captures are the final 8-bit tonemapped swapchain image, not HDR buffers.
- A capture's sidecar state is taken when the capture is requested, which is the frame it shows.
- `set` changes values without touching the preset label. A later `preset` command overwrites
  the preset's sliders.
- `get` values are floats, so `0.2` reads back as `0.20000000298`.

## Extending

Commands live in `SatelliteSim::harnessExec()` (`src/simulations/SatelliteSimHarness.cpp`): match
on `c.name`, read `c.pos` / `c.kv` (`c.num`, `c.flag`, `c.str`), put results in `a.result`
(`"message"` is the one-line summary), `throw` (via `fail()`) on bad input. A command that needs
more frames returns `Status::Pending` and keeps state in `a.scratch`, with `a.frame` counting its
calls. Commands run at the top of `buildUI`, before this frame's camera derivation and
`recordCompute`. Update the table above and `kHelp` together.
