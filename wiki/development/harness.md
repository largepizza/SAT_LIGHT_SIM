# Automation harness

The harness is a small command language that drives the running app: put the observer anywhere, set the time
by the Sun's elevation, change any setting or knockout bit, select or fly to satellites, then capture
screenshots (each with a JSON record of the exact state), dump the UI layout, sample GPU and CPU timings,
render the audio, or play camera paths. Scripts are deterministic: running one twice gives bit-identical
images.

It is **the way to launch the app** during development, for people and agents alike. A harness run exits on its
own, runs muted in its own user data folder (it never reads or writes your `settings.json` and cannot trigger
crash recovery on your next launch), and leaves captures, state, layout dumps and timings to check. Use it for
anything that needs the real renderer: shader and terrain changes, image A/B comparisons, performance, UI
layout, camera shots. Work that does not need the renderer (photometry, orbits, benchmarks) belongs in
[SatModelTool](tools.md#satmodeltool), which is faster and runs in CI.

How a change *feels* (camera motion, input, audio, a menu's ergonomics) is still for a person to judge.

## Launch spacing

!!! warning "Invariant"
    Never relaunch the app back-to-back. `run.py` and `live.py start` wait **30 s after the last app exit** before
    launching (`tools/harness/launchgate.py`; `run.py --cooldown S` overrides it and should not be set to 0 by
    habit). For many small batches, start **one** app with `live.py start` and `send` commands to it rather
    than looping `run.py`. Batch checks into one script where possible: every launch costs startup time and
    carries some risk to the development machine.

## Quick start

```bash
cmake --build build --config Release --target SatLightSim     # Release: PNG encoding in Debug is very slow
python tools/harness/run.py tools/harness/scripts/smoke.satcmd
python tools/harness/run.py -c "observer lat=46.55 lon=7.98 agl=50; time sun 10 rising; camera az=200 el=5; wait settle; capture alps"
```

`run.py` prints the run folder, one line per command (`ok` and a message, or `ERR` and the reason), the
captures written and a status line. It exits non-zero if the app crashed, timed out, or any command failed.

The Python tools that analyse images and audio need a virtual environment once:

```bash
py -3 -m venv tools/harness/.venv
tools/harness/.venv/Scripts/python -m pip install -r tools/harness/requirements.txt
```

`run.py` and `live.py` themselves use only the standard library.

## Ways in

| Front end | Use | How |
|---|---|---|
| `tools/harness/run.py <script>` | batch: one script, one process, exits when done | `-c "cmd; cmd"` for inline commands |
| `tools/harness/live.py` | many small batches against one running app (no startup per batch) | `start`, `send "cmds"`, `send -f file`, `status`, `stop` |
| the console in the app | a person typing the same commands | the backtick key toggles it; Enter runs, Up/Down history, Esc closes |
| executable flags | anything else | `SAT_LIGHT_SIM_V_*.exe --script f.satcmd --out dir` |

Executable flags: `--script <file>`, `--live <dir>`, `--out <dir>`, `--window WxH` (default 1600x900),
`--settings <settings.json>` (start from these settings, copied into the run folder), `--user-data` (use the
normal user data folder instead of the run folder), `--fixed-dt <s>` (default 1/60; 0 = real time),
`--timeout <s>` (watchdog), `--stay` (keep running after the script), `--sound` (a real audio device). Unknown
flags are an error, so a typo never silently runs the normal app.

`run.py` adds `--exe`, `--config` (default Release), `--quiet`, `--cooldown`, `--boot-screen on|off` and
`--boot-capture`. It runs the **freshest** `build/<config>/SAT_LIGHT_SIM_V_*.exe`; a build in another tree
(`build-win-release/`) is invisible to it unless passed with `--exe`.

Live mode: `live.py start` launches the app with `--live harness_live/`. Each `send` writes one file to
`harness_live/inbox/`; the app polls the inbox and writes `outbox/<same name>.json` with one result per command.
Captures land in the live run folder that `status` prints.

The console, used outside a harness run, writes to `harness_runs/console_<time>/` next to the executable, runs
in real time and never exits by itself.

## The run folder

```
harness_runs/<stamp>_<script>/
  results.jsonl                    one line per command: src (file:line), cmd, ok, error, frames, wall_ms, result{}
  summary.json                     status: ok | errors | timeout | script_error | closed; counts; commit; log path
  captures/<name>.png              + <name>.json sidecar: the command, size and the full state at capture time
  captures/<name>.layout.json      `ui dump`
  captures/<name>.state.json       `state <name>`
  captures/<name>.wav              `audio record`
  perf_profiles/profile_log.jsonl  `perf name=...` samples and `sweep` records
  satlight_log.txt, settings.json, app_stdout.txt, <script>.satcmd (a copy of what ran)
  captures/boot_NN.png             `run.py --boot-capture`: the loading screen's frames
```

Each run starts from the built-in defaults unless `--settings` is given. A capture's sidecar state is taken in
the frame the capture shows. Captures are the final 8-bit tonemapped image, not HDR buffers.

`python tools/harness/gc.py` (a dry run unless `--apply`) deletes run folders older than `--keep-days` (7)
unless they are pinned (`gc.py --keep <run>` writes a `KEEP` file), named in `CLAUDE.md`, `docs/`, `.plans/` or
the agent memory folder, or the live app's current folder. `--slim` thins old recorded frame sequences to every
30th frame.

## Script syntax

One command per line or separated by `;`. `#` starts a comment. Arguments are positional words or
`key=value`. Double quotes group words with spaces (`select const "Starlink Gen1"`).

```
preset High
observer lat=46.55 lon=7.98 agl=50      # Interlaken, 50 m above the ground
time sun -4 setting                      # dusk: the Sun 4 degrees below the horizon, going down
time pause
camera az=280 el=3 fov=60
wait settle                              # let every eased quantity converge
capture dusk
knockout +terrain_march ; wait settle 10 ; capture dusk_noterrain
```

A failing command is recorded with its reason and the script continues; the run's status is then `errors`.

## Command reference

### Flow and time

| Command | What it does |
|---|---|
| `wait <frames>`, `wait seconds <s>` | wait N frames, or wall-clock seconds |
| `wait settle [frames]` (or `settle`) | hold sim time and run 40 (or N) frames at a 0.5 s step, so eased values converge (sky glare, the dark-sky dome, mesh fades, beam-light fades, environment probes); the sky's temporal anti-aliasing restarts with it. Use before every capture that follows a change. From orbit, add `wait 90` after it before a capture: a settled view from 400 km draws cloud edges blocky until about 90 ordinary frames have passed |
| `time set <ISO>` | e.g. `2036-06-21T04:00:00Z` (UTC) <!-- history-ok --> |
| `time sun <el> [rising\|setting]`, `time sun noon\|midnight` | the time nearest now (within 12 h) when the Sun is at `<el>` degrees for this observer |
| `time add <s>`, `time j2000 <s>` | relative, or absolute in seconds since J2000 |
| `time pause`, `time play`, `time scale <1x\|10x\|1m\|5m\|1h\|1d\|1w\|1mo\|1yr>`, `time reverse on\|off` | |
| `log <text>`, `help`, `quit` | `help` prints the command summary (`kHelp`) |

### Observer, camera and targets

| Command | What it does |
|---|---|
| `observer lat= lon= [agl=\|alt=]` | move the observer, keeping the camera heading; ends follow mode. `alt` is metres above sea level (the eye sits at max(ground, alt) + 2 m); `agl` is above the ground, read back from the GPU's detailed terrain (one extra frame). With the depth pass knocked out it falls back to the CPU's coarse DEM, which is off by hundreds of metres on coasts and in valleys. `state` reports `alt_m`, `agl_m`, `ground_m`, `ground_source` and the raw height offset |
| `camera az= el= fov=` | azimuth (0 north, 90 east), elevation, vertical field of view (0.5 to 120) in degrees |
| `camera look <sun\|moon\|sel\|planet>` | aim once; `sel` is the selected satellite or planet |
| `camera track <...\|off>` | re-aim every frame |
| `select sat <index>`, `select const "<name>" [n=<k>]`, `select planet <name>`, `select none` | by roster index; a constellation's member highest in the sky (or its k-th member) |
| `const list`, `const "<name>"\|all on\|off [highlight=on\|off]` | constellation visibility |
| `follow [sat=<i>] [offset=along,cross,radial] [fly=on]`, `follow off [fly=on]` | ride with a satellite (types with a geometry model); offset in metres in its frame. `fly=on` uses the UI's animated Go to / return instead of a jump |
| `track [on\|off]` | the selection panel's Track button: the camera stays locked on the selected satellite while the observer and zoom stay free. Released by `select none`, `select planet`, `follow` and any explicit aim |
| `beams [list]`, `beams go [rank=1] [dist_km=0] [bearing=270] [agl=2\|alt=] [look=site\|up\|none]` | where the Reflect Orbital beams land now (from the last readback; only satellites above the observer's horizon make beams, so stand in the region first). `go` puts the observer near the rank-th busiest site and aims at it or up the beams. Frame beams this way rather than by a guessed time and place |
| `eclipse <solar\|lunar>` | the next eclipse after the current time: sets greatest eclipse, puts the observer under the Moon or where the shadow axis meets the Earth, aims at the Moon |
| `aurora`, `aurora next [<kp>] [days=1100]`, `aurora substorm [min=0.4]`, `aurora kp <v>`, `aurora auto` | the aurora's activity, which is a pure function of sim time. Bare: report it. `next` jumps sim time to the peak of the next storm reaching that Kp (default 7; automatic activity). `substorm` jumps to 10 minutes after the next substorm onset stronger than `min`. `kp <v>` fixes the activity by hand (substorms still run); `auto` returns to the computed space weather. Test from a dark site: a city's skyglow hides the aurora |
| `bookmark add [name]`, `bookmark go\|update\|delete <n>`, `bookmark rename <n> <name>`, `bookmark list` | the Bookmarks window's actions, numbered from 1 as listed. The thumbnail is captured a few frames later, so `wait 3` after `add` or `update` before looking at the window |
| `snapshot <profile_log.jsonl> [index=-1] [settings=on\|off] [drift=intro\|default]` | reproduce a perf snapshot a user saved ([Snapshots](#snapshots)) |

### Settings and rendering

| Command | What it does |
|---|---|
| `get [section[.key]]` | any persisted setting; `get` alone lists them all (the keys are `settings.json`'s) |
| `set <section.key> <value>` (or several `key=value`) | change settings through the same code path `settings.json` loads through ([Controls and settings](controls-and-settings.md#one-read-path-one-write-path)); unknown keys and wrong types are errors; the preset label is unchanged |
| `expect <key\|cine.<path>\|state.<path>> <value> [tol=1e-4]` | fail unless the value matches: a setting (`clouds_v2.coverage`), the open cinematic's JSON (`cine.shots.0.keys.1.t`) or the `state` JSON (`state.observer.lat_deg`, `state.time.utc`). Numbers compare within `tol` (relative above 1), booleans accept `true`/`1`/`on`, anything else compares as text |
| `preset <Planetarium\|Low\|Medium\|High\|Ultra\|Potato\|Custom>` | apply a graphics preset (overwrites knockouts and quality sliders) |
| `knockout none\|<mask>\|+key\|-key\|key ...`, `knockout list` | knockout bits by stable key ([Profiling](profiling.md#knockout-bits)), plus `potato_sky` and `lite_sky`; sets the preset to Custom |
| `debugview <name\|off>` | replace terrain, sea or cloud-composite pixels with a debug channel (below) |
| `probe <x> <y>` | what the terrain march does for one pixel's ray, in the capture's pixel coordinates: the seed from the shared depth, the seeded march and a march from the eye (distance, steps), the heights at the hit, and a 64-sample profile along the ray |
| `viewer [aim=free\|observer\|toward\|sun] [light=live\|studio] [glare=on\|off] [shadows=on\|off] [dist=<radii>]` | the satellite 3D view's controls, for a `ui open viewer` or `ui open info` capture; reports the observer's flare per unit intensity, the glints and the photometry lines |
| `lightning` | the lightning flashes in progress (id, kind, intensity, age, distance, cloud base and top, segment count), the built channel trees, the drawn flash and segment counts, and the thunder queue. With time paused a flash stays frozen, so `time add 0.25` steps through one |
| `lightning spawn kind=cg\|ic\|spider\|sprite dist_km=10 az=<camera az> [seed=] [base_m=1500] [top_m=10000] [ground_km=4] [ground_az=] [dur=]` | place a flash by hand: cloud-to-ground (default), in-cloud, spider or red sprite, at that distance and azimuth, with the cloud's base and top above the eye's ground and, for a ground stroke, its strike point `ground_km` aside. It runs through the same channel, glow and thunder code as the storms' own flashes. For thunder, `wait` a few frames, `time add` the sound's travel time (distance / 343 m/s), then `audio record` |
| `shaders reload [march=<spv>] [wg=<X>x<Y>]` | rebuild the cloud pipelines from the SPIR-V on disk and report the driver's register count, binary size and spill memory ([Profiling](profiling.md#ab-testing-a-shader-change)) |

Debug views (`debugview`): `normals`, `detail`, `steps` (march steps, blue few to red the budget), `albedo`,
`shadow`, `rough` (roughness, rock, snow as RGB), `elevzebra` and `distzebra` (stripes every 25 m of elevation
and 100 m of distance: broken stripes mean height or convergence jitter), `erosion`; the terrain lighting term
by term (`terms`, `direct`, `skyamb`, `night`, `moon`, `aurora`, `gates`, `factors`, `skyambraw`, `suntint`,
`aofactors`, `day`, `nightmap`, `geodot`, `sunvis`, `nightsky`, `citylights`), `solar` (solar parks); the sea
(`oceanrefl`, `oceanfresnel`, `oceanstate`, `oceansurf`, `oceannormal`, `oceanshore`, `oceanshadow`); and the
cloud composite (`cloudairsplit`, `cloudtrans`, `cloudrad`, `cloudalpha`). They are not persisted. The lighting
term views write linear radiance (x100) straight into the frame, bypassing exposure and tonemapping, so
`tools/harness/lightscan.py` can decode a pixel back to a number and a hue.

### Captures, state and timing

| Command | What it does |
|---|---|
| `capture <name> [ui=on] [crop=x,y,w,h] [scale=s]` | PNG of the frame (no UI unless `ui=on`), cropped then scaled: `scale<1` box-filters down, `scale>1` enlarges with nearest neighbour for pixel inspection. Writes the state sidecar |
| `photo <name> [scale=1-4] [frames=N]` | the HQ photo: rendered at `scale` x the window offscreen, clouds at full rate, time paused, settled over `frames` frames; no UI, no sidecar |
| `state [name]` | the full state (time, observer, camera, selection, clouds, satellites, Moon, aurora, exposure, ambience...) as the result, and a file if named. The `aurora` block has `kp`, `g_scale`, the drivers (`quiet`, `hss`, `cme`), the oval's edges (`oval_mlat_deg`), any `substorm`, and the observer's magnetic latitude, magnetic local time and the oval's brightness overhead |
| `perf [frames=60] [name=]` | average raw GPU timestamp buckets and CPU buckets over N frames, with GPU total and wall frame-time distributions; `name=` also appends to the run's perf log |
| `sweep` | the automated knockout sweep (about 15 s); returns the whole record |

### UI and input

| Command | What it does |
|---|---|
| `ui show\|hide`, `ui scale <0.75-2>` | HUD visibility and UI scale |
| `ui open <settings [tab=Name]\|viewcontrols\|trace\|info\|viewer\|bookmarks\|cine\|console>`, `ui close <name\|all>` | windows (`info`, `viewer` and `trace` need a selected satellite); an advanced tab (Performance included) turns on advanced settings |
| `ui click <ElementId>[:index] [fx=0.5] [hold=N]` | move a scripted pointer onto the element's centre (last frame's layout) and click through the real hover and click path; fails if the element is scrolled out of view. A UI-kit element is addressed as `Key:index` (`ui click ReplayIntroBtn:0`, `ui click InfoSectHdr:2`). The click lands on the frame the pointer arrives, which the sky picker can also take as a click on empty sky: re-`select` before opening a window that needs the selection |
| `ui type <text>`, `ui key <enter\|esc\|tab\|backspace\|delete\|left\|right\|home\|end\|w>` | keyboard input into the focused text field, else the game |
| `ui dump [name]` | every drawn rectangle, text and image with its box and element id, plus checks for clipped, off-window and overlapping text |
| `pad <a\|b\|x\|y\|lb\|rb\|start\|view\|ls\|rs\|up\|down\|left\|right>` | a gamepad press without a controller, through the context meaning first, else the binding |
| `tutorial start [step] [pad=1]`, `tutorial step <n\|name>`, `tutorial next\|back\|skip\|state` | the first-run tutorial (never started on its own in a harness run) |
| `window <W>x<H>` | resize the window and wait for the new swapchain |
| `overlay text <id> "<text>" [x=0.5 y=0.1 size=28 align=center\|left color=RRGGBB]`, `overlay label <id> "<text>" target=<sel\|sun\|moon\|planet>`, `overlay clear [id]` | screen text, drawn even with the HUD hidden |

### Camera paths and cinematics

| Command | What it does |
|---|---|
| `path clear`, `path key <t> [lat= lon= alt= az= el= fov= sim=<ISO>\|simadd=<s>]` | keyframes at path time `t` (seconds); channels left out inherit from the previous key |
| `path goto <t>` | jump to the path's pose at `t` |
| `path play [fps=30] [record=<name>] [ui=on] [scale=s]` | play the path at a fixed frame rate; `record` captures every frame as `captures/<name>_00000.png` ...; the result carries `gpu_ms_mean` (the cost while moving), `gpu_total_ms_p90`, `gpu_total_ms_max` and the worst frame's buckets |
| `cine new\|name\|shot\|key\|play\|export\|save\|load\|state\|stop ...` | the cinematic: shots, keys, looks, follow shots, playback and preview or HQ export |

The harness's camera path **is** the current cinematic shot, so a path authored in a script appears in the
in-app Cinematics window and the reverse. Between keys each channel (latitude, longitude, azimuth, elevation,
log altitude, log field of view) follows a cubic Hermite spline with Catmull-Rom tangents; azimuth and longitude
are unwrapped so the path turns the short way. Playback is offline: every frame is exactly 1/fps of path time
and of frame time whatever the real frame rate, and a recording renders exactly one app frame per recorded
frame (it waits for the copy, not the PNG encode), so a recording shows the same temporal behaviour a player
sees. `tools/harness/frames2video.py <dir>/<name> -o out.mp4 --fps 30` assembles frames with ffmpeg.

`cine` subcommands: `new [name]`, `name <n>`; `shot add [name]`, `shot del`, `shot <n>`, `shot look [clear]`
(store the current look with the shot), `shot follow <sel|index|off>`, `shot ease <0-1>`,
`shot simrate <x>`, `shot simnow`; `key [t=]`; `play [shot]`;
`export preview|hq [fps=] [scale=] [frames=] [blur=] [shot]` (frames to `captures/cine_<name>[_hq]/`, with a
`cinematic.json` and a README; `blur=N` averages N subframes over a 180-degree shutter in linear light);
`save [file]`, `load <file>` (`<user data>/cinematics/`); `state`, `stop`. A cut to the next shot resets the
sky's and the clouds' temporal history. `tools/harness/scripts/tour.satcmd` is the worked example.

### Sound

| Command | What it does |
|---|---|
| `audio [state [name]]` | every ambience driver and layer gain, the audible layers, the tonality |
| `audio record <name> [seconds=8] [bus=ambience\|music\|sfx\|all\|music+ambience] [solo=<layer>]` | render the mix offline to a WAV; needs the default muted run |
| `audio expect <layer,...> [absent=<layer,...>] [min=0.05]` | fail unless the listed layers are audible and the absent ones are not |
| `audio force <layer> <gain\|off>`, `audio force off` | pin a layer's gain |
| `audio music [next\|prev\|pause\|play\|end\|state]` | the music player; `end` ends the track so the gap is reachable offline |
| `audio tonality [wait]` | the key the ambience is in; `wait` holds until every track is analysed |

A muted run has an audio engine with no device; see [Ambience](../sound/ambience.md#offline-rendering-for-the-harness).

## Determinism

What makes two runs identical:

- **A fixed frame step.** The sim receives `--fixed-dt` (1/60 s) each frame, not wall time.
- **`wait settle` before a capture.** Many values ease toward their targets over seconds; a settle holds sim time
  and steps 0.5 s per frame. The HUD's frame rate during a settle is meaningless.
- **An explicit preset.** A fresh run folder seeds the preset from the device type; put `preset <name>` first.
- **`time pause`** unless the script is about motion.
- The same window size and the same build.

`time sun` is relative to the current clock: each one moves to the nearest matching moment within 12 hours, so
splitting, reordering or inserting views silently moves the later ones in time, sometimes to another date (a
different Moon). For an A/B across two runs keep the command sequence identical, and check that the sidecars
agree on `state.time.utc`.

## Snapshots

A user hands over a location by pressing *Save snapshot* (F9, or Settings → Performance) in the app, which appends
a record to `perf_profiles/profile_log.jsonl` in their user data folder. `snapshot <file> [index=-1]` restores
it: the record's settings, sim time (paused), the exact observer direction and height, the camera and the cloud
map's drift phase (session state that latitude, longitude and time alone do not reproduce: the same place and
time would frame other clouds). `index` counts snapshot records only, from the end when negative. Records
without settings or a drift phase need `--settings` set to the user's `settings.json`; `drift=intro` (the
default) reconstructs the phase as a session that played the intro, `drift=default` uses the compiled default.

## Analysis tools

`tools/harness/imgtools.py` (Pillow and numpy) prints JSON first, so numbers can be checked before looking at
pictures:

| Subcommand | |
|---|---|
| `sheet <pngs or run dir> -o out.png [--cols 3] [--width 480]` | a labelled contact sheet of many variants |
| `diff a.png b.png [-o heat.png]` | mean, max and RMS difference, PSNR, changed fraction and its bounding box, an amplified heat map |
| `stats a.png` | luminance percentiles, clipped fractions, a 4 x 4 grid of mean luminance |
| `crop a.png x y w h --scale 4 -o out.png` | pixel-exact enlargement |
| `seams a.png` | rows and columns whose mean luminance jumps against their neighbours (bands, tile seams) |
| `audio a.wav [b.wav ...] [-o spec.png]` | levels, ungated LUFS, bands, correlation, transients and a spectrogram per WAV |

A model looking at a large image sees it downscaled, so a one-pixel feature can vanish: use
`capture ... crop=... scale=4` or `imgtools crop` for small things, and `sheet` to compare variants.

Motion and consistency tools, each with `gen` (write a script), a run with `run.py`, then `score` or `compare`:

| Tool | Measures |
|---|---|
| `tools/harness/tstab.py` | **temporal stability**: how far the image seen while moving is from the image the same view settles to (ghosting, smears, blocks, grain). It generates player-speed scenarios from a snapshot's height above the ground (`walk`, `boost`, `strafe`, `pan90`, `pan240`, `walkpan`, `rise`, `boostrise`, `pitch`, `boostpan`, `flick`, `chaos`, 2.5 s each at 60 fps), records each, revisits five poses and settles them for reference, and reports mean, p95 and p99 luminance difference and the share past 16 levels, per scenario and per horizontal band. A `floor` scenario (two settles of one view) gives the noise floor. Yaw-only pans hide row-aligned errors; the `pitch` scenario catches them |
| `tools/harness/flicker.py` | **still-view flicker**: per-pixel temporal statistics of a recorded still sequence (mean and p99 per-pixel standard deviation, the share of pixels past 8 levels, an optional heat map) |
| `tools/harness/climb.py` | **climb consistency**: climbs straight up from a snapshot through 15 altitudes (8 to 2500 km) and correlates the cloud detail of each frame, reprojected and filtered, with the next and with fixed references. Every level of detail should be a filtered version of the one below it, not a different placement |

`tools/harness/find_cloud_spots.py [ISO time]` lists cloudy benchmark locations for a fixed sim time (the cloud
map drifts, so a spot is tied to a time). `tools/harness/lightscan.py` decodes the terrain lighting debug views.
`tools/perf_analysis/live_perf_table.py` tabulates `perf name=` results.

## Worked recipes

**Performance, no images:**

```
preset Medium
observer lat=61.2 lon=-149.9 agl=10      # Anchorage, a beam-heavy view
time sun -12
camera az=0 el=20 fov=80
wait settle
perf frames=120 name=anchorage_medium
sweep
```

**Shader A/B at fixed views:** capture a baseline, change the code or a setting, capture again, then
`imgtools.py diff before.png after.png -o heat.png`. Within one run, toggle with `set` or `knockout` and
`wait settle` between captures.

**Debugging a rendering artefact:** capture it at a fixed view; make a `sheet` with each feature toggled to see
which removes it; `debugview` the suspect channel; `probe` a pixel on the artefact and one beside it; rerun with
the validation layer (`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`, messages in `app_stdout.txt`) for a crash
or device loss; `perf` with features toggled to attribute cost.

**UI check:** `ui scale 2.0; ui open settings tab=Controls; wait 3; ui dump controls; capture controls ui=on`,
then read the dump's text-overlap checks before the picture. `tools/harness/scripts/ui_windows.satcmd` walks the
info, trace and Bookmarks windows and the HUD's typed fields this way, checking each result with `expect state.`.

## Gotchas

- **Build Release.** A Debug build takes tens of seconds to encode each 1600 x 900 PNG.
- **The first launch after a shader change is slower** while the driver compiles pipelines; do not read timings
  from it.
- **The window must stay visible** (not minimised), or the swapchain stops presenting. It opens without taking
  focus.
- `get` returns floats (`0.2` reads back as `0.20000000298`); a later `preset` overwrites `set` sliders.
- `ambience.json`, samples and other runtime files reach the executable only through a build.
- The commit stamp in the log's first line is written at configure time, not per build.

## The loading screen in a run

Every launch, harness runs included, shows the loading screen. Its frames all come before `init()` returns,
before the script's first command, so scripts are unaffected. `run.py --boot-capture` writes each loading frame
to `captures/boot_NN.png`, `--boot-screen off` launches without it, and `satlight_log.txt` has a
`boot: <step> (<ms since launch>)` line per step.

## Architecture

The harness has two halves:

| Half | Files | Role |
|---|---|---|
| app-independent | `src/Harness.h/.cpp` | command-line options, the statement parser, the `Runner` (one command at a time across frames, `results.jsonl`, `summary.json`, the live inbox, the status file), the watchdog |
| the commands | `src/simulations/SatelliteSimHarness.cpp` | `harnessExec()` (what each command does), `harnessStateJson()` (the capture sidecar), the console's key handling |

**Where it runs.** `harnessTick()` is called near the top of `buildUI()`, after the CPU frame timing starts and
before the camera and `recordCompute()`. A command's effect is therefore in the frame it ran in, and a
`capture` after it records that frame. Time, observer and follow commands also re-run `updatePositions()` at
once, so a `state` or sidecar in the same frame is not one frame stale.

**Commands across frames.** `harnessExec()` returns `Status::Done`, `Status::Error` (through `fail()`) or
`Status::Pending`; a pending command is called again next frame, with `a.frame` counting its calls and
`a.scratch` holding its state. Results go into `a.result`, whose `"message"` is the one-line summary.

**Hooks it uses:**

- `Simulation::frameDt()`: the fixed step (0.5 s during `wait settle`, which also holds sim time; a cinematic
  export or `path play` sets its own).
- `Simulation::wantsQuit()`: leave the main loop when the script ends.
- `onChar()` and `capturesKeyboard()`: the console.
- `Paths::setUserDataDirOverride()`: `main.cpp` points the user data folder at the run folder before the log
  starts.
- `wantsCleanScreenshot()` honouring `ui=on`, the crop and scale in `finalizeScreenshot()`, and
  `UIRenderer::requestLayoutDump()`.
- Settings go through `applySettingsJson()` and `buildSettingsJson()`, so every persisted setting is scriptable
  with no per-setting code.

Outside a harness run (and before the console's first use) the runner is null and every hook is a no-op. In a
run, the intro, the first-run tutorial, first-run notices and the preset seed notice are suppressed, the music
is started on a device-less engine, and the soundtrack analysis runs synchronously.

**Adding a command:** match on `c.name` in `harnessExec()`, read `c.pos` and `c.kv` (`c.num()`, `c.flag()`,
`c.str()`), put results in `a.result`, `fail()` on bad input. Update `kHelp` and this page's tables together.

**Testing the harness:** `python tools/harness/selftest.py` runs real scripts and checks capture sizes and
sidecars, bit-identical determinism, knockouts changing and restoring the image, settings round trips, error
reporting, parse errors, `wait settle` holding time, `time sun` accuracy, perf and sweep numbers, selection,
follow and `ui dump`, `probe`, `debugview`, a recorded camera path, Track, the watchdog and live mode. Run it
after changing `src/Harness.*` or `SatelliteSimHarness.cpp`; it launches the app many times, so it takes several
minutes with launch spacing.

## Where in the code

| File | What |
|---|---|
| `src/Harness.h/.cpp` | options, parser, `Runner`, live inbox, watchdog |
| `src/simulations/SatelliteSimHarness.cpp` | `harnessInit()`, `harnessTick()`, `harnessExec()`, `harnessStateJson()`, `kHelp` |
| `tools/harness/run.py`, `live.py`, `launchgate.py` | drivers and launch spacing |
| `tools/harness/imgtools.py`, `tstab.py`, `flicker.py`, `climb.py`, `lightscan.py`, `frames2video.py`, `gc.py`, `selftest.py` | analysis and maintenance |
| `tools/harness/scripts/` | worked scripts (`smoke`, `determinism`, `terrain_views`, `ambience_tour`, `tour`, `track`, `text_fields`, `ui_windows`, `tutorial`, ...) |
| `docs/HARNESS.md` | the older reference this page restructures |
