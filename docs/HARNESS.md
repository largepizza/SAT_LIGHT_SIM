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
time), `--timeout <s>` (watchdog), `--stay` (keep running after the script), `--sound` (a real audio
device; without it the engine has none — silent, but `audio record` can still render the mix).

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
  blackbox.csv         GPU/CPU telemetry, fsynced per sample, from ~1 s before launch to exit
                       (blackbox.py; --no-blackbox to skip) — see "Machine-level resets"
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
| `observer lat= lon= [agl=\|alt=]` | move the observer. `alt` = metres above sea level (the shaders' meaning of the height offset: the eye is at max(ground, alt) + 2 m). `agl=0` = on the ground exactly; `agl>0` is exact too: the command takes one extra frame to read back the GPU's own ground (DEM + terrain detail) at the new position. With the depth pass knocked out (bit 1024) it falls back to the CPU's 18 km/px DEM copy, which is off by hundreds of metres on coasts and in valleys. `state` reports `alt_m`, `agl_m`, `ground_m` (+ `ground_source` gpu/cpu), `terrain_cpu_m` and the raw `height_offset_m`. Keeps the camera heading. Ends follow mode |
| `camera az= el= fov=` | azimuth (0 = north, 90 = east), elevation, vertical FOV (0.5-120) in degrees |
| `camera look <sun\|moon\|sel\|planet>` | aim once. `sel` = the selected satellite or planet |
| `camera track <...\|off>` | re-aim every frame (a moving satellite stays centred) |
| `select sat <index>` | by roster index |
| `select const "<name>" [n=<k>]` | the constellation's member highest in the observer's sky, or its k-th member. `const list` shows names |
| `select planet <name>`, `select none` | |
| `follow [sat=<i>] [offset=along,cross,radial]`, `follow off` | fly with a satellite (types with a geometry model). The offset is in metres in the satellite's frame |
| `track [on\|off]` | the selection panel's **Track** button: lock the camera onto the selected satellite and re-aim every frame (the observer stays put, so WASD still walks, and the wheel's `camera fov=` zoom is untouched). No argument = toggle; needs a satellite selection. Released by `select none`, `select planet`, `follow` and any explicit aim (`camera az=`/`el=`, `camera look`, `camera track`, a scripted camera key). `state` reports it as `camera.tracking` and `selection.track`. Not to be confused with `camera track <target>`, which is the harness's own aim-every-frame |
| `viewer [aim=free\|observer\|toward\|sun] [light=live\|studio] [glare=on\|off] [shadows=on\|off] [dist=<radii>]` | the 3D view's own controls, for a `ui open viewer` / `ui open info` capture: `aim=observer` is the Observer chip (the satellite from the ground observer's direction), `aim=sun` (harness only) looks from the Sun's side, where a Sun-facing array glints; `dist` is in model radii. `glare=on` (the default) draws the main view's glare on the glints that make the flare the observer sees (Live light and a tracked satellite only). Reports `flare_per_i` (the observer's effectFlare per unit intensity; 0 = no glare), `glints_last_frame` (the previous frame's glint list: `wait` a frame after a change) and the PHOTOMETRY lines |
| `const list`, `const "<name>"\|all on\|off [highlight=on\|off]` | constellation visibility |
| `get [section[.key]]` | any persisted setting; `get` alone lists them all (the keys are `settings.json`'s) |
| `set <section.key> <value>` (also `key=value`, several per line) | change settings through the same code path `settings.json` loads through. Unknown keys and wrong types are errors. Doesn't change the preset label |
| `preset <Planetarium\|Low\|Medium\|High\|Ultra\|Potato\|Custom>` | apply a graphics preset (it overwrites knockouts and quality sliders) |
| `knockout none\|<mask>\|+key\|-key\|key ...`, `knockout list` | debug knockout bits by stable key (`terrain_march`, `volumetric_cloud_march`, ...; `potato_sky`, `lite_sky`). Sets the preset to Custom, like the Display tab does |
| `capture <name> [ui=on] [crop=x,y,w,h] [scale=s]` | PNG of the frame (no UI unless `ui=on`), cropped then scaled: `scale<1` box-filters down, `scale>1` enlarges with nearest neighbour for pixel-level inspection. Writes `<name>.json` with the state |
| `state [name]` | the full state as the command result (and a file if named) |
| `probe <x> <y>` | what the terrain algorithm does for one pixel's ray (`terrain_probe.comp`, the same functions the renderer uses): the seed from the shared depth, the seeded march and a march from the eye (distance, steps), the DEM/detail/roughness at the hit, and a 64-sample `profile_t_rayalt_dem_H` (t, ray altitude, DEM, DEM + detail) along the ray. Pixel coordinates are the capture PNG's |
| `debugview <off\|normals\|detail\|steps\|albedo\|shadow\|rough\|elevzebra\|distzebra\|erosion>` | replace terrain pixels with a debug channel: normals, detail height / amplitude, march steps (blue few .. red the budget), albedo, sun shadow x Lambert, roughness/rock/snow as R/G/B, zebra stripes every 25 m of elevation / 100 m of hit distance (broken or jagged stripes = height or convergence jitter), and the erosion octaves alone (grey = none, bright ridge / dark gully relative to their bound, blue = too gentle a slope for any). Not persisted |
| `perf [frames=60] [name=]` | average raw GPU timestamp buckets and CPU buckets over N frames, plus the GPU total and wall frame-time distributions. `name=` also appends it to the run's perf log |
| `sweep` | the automated knockout sweep (≈15 s); returns the whole record |
| `ui show\|hide`, `ui scale <0.75-2>` | HUD visibility and UI scale |
| `ui open <settings [tab=Name]\|viewcontrols\|trace\|info\|viewer\|console>`, `ui close <name\|all>` | windows (`info`/`viewer`/`trace` need a selected satellite). An advanced tab turns on "show advanced settings" |
| `ui dump [name]` | every drawn rect/text/image with its box and element id, plus checks: text cut by its scissor, text off the window, and overlapping text under the same scissor |
| `window <W>x<H>` | resize the window and wait for the new swapchain |
| `path clear`, `path key <t> [lat= lon= alt= az= el= fov= sim=<ISO>\|simadd=<s>]` | camera-path keyframes at path time `t` (seconds). Channels you leave out inherit from the previous key (from the current view for the first). See "Camera paths" |
| `path goto <t>` | jump to the path's pose at `t` |
| `path play [fps=30] [record=<name>] [ui=on] [scale=s]` | play the path at a fixed frame rate; with `record` every frame is captured as `captures/<name>_00000.png` ... |
| `overlay text <id> "<text>" [x=0.5 y=0.1 size=28 align=center\|left color=RRGGBB]` | screen text at fractional coordinates, drawn even with the HUD hidden |
| `overlay label <id> "<text>" target=<sel\|sun\|moon\|planet> [size= dx= dy=]` | a label that follows a sky target |
| `overlay clear [id]` | |
| `audio [state [name]]` | the ambience: every context driver (altitude, Sun, ocean, land cover, beam, shells, ...) and every layer's target and current gain, loudest first in `audible`. With a name, also `captures/<name>.audio.json`. Capture sidecars carry the same block as `ambience` |
| `audio record <name> [seconds=8] [bus=ambience\|music\|sfx\|all\|music+ambience] [solo=<layer>]` | renders the mix offline into `captures/<name>.wav` (+ `<name>.json`: state + levels) and returns RMS / peak / per-second RMS. Needs the default muted run (no device). See "Ambient sound" |
| `audio expect <layer,...> [absent=<layer,...>] [min=0.05]` | fails unless each listed layer's gain is at least `min` and each `absent` one below it — a location tour checks itself |
| `audio force <layer> <gain\|off>`, `audio force off` | pin a layer's gain wherever the observer is (calibrating one voice) / release them all |
| `audio music [next\|prev\|pause\|play\|state]` | the music player: returns the track, its name, paused, and the gap countdown. (Offline, a track never ENDS, so the between-track gap is not reachable in a harness run) |
| `log <text>`, `help`, `quit` | |

A command that fails is recorded with its reason and the script continues. The run's status is
then `errors`.

## Camera paths and videos

```
preset High
observer lat=46.62 lon=8.03 agl=0
time sun 6 setting
time pause
ui hide                                   # overlays still draw; `capture ui=on` includes them
overlay text title "SAT LIGHT SIM" y=0.12 size=48
path clear
path key 0 alt=1200 az=150 el=4 fov=55 simadd=0
path key 3 alt=2500 az=170 el=0 fov=50
path key 6 alt=5500 az=200 el=-6 fov=60 simadd=240   # the Sun sets 4 min of sim time over 6 s
path play fps=24 record=alps ui=on
```

then `python tools/harness/frames2video.py harness_runs/<run>/captures/alps -o alps.mp4 --fps 24`
(ffmpeg on PATH). Full example: `tools/harness/scripts/demo_path.satcmd`.

- Interpolation is a cubic Hermite spline with Catmull-Rom tangents per channel (lat, lon, az, el,
  log altitude, log FOV). Azimuth and longitude are unwrapped against the previous key, so the path
  turns the short way. Sim time, when keys set it, is linear between those keys; otherwise it
  advances at the time scale that was active when `path play` started.
- Playback is offline: every frame is exactly 1/fps of path time and of frame time, whatever the
  real frame rate. With `record`, a frame is captured before the next one is shown, so slow PNG
  encoding never drops or repeats a frame (a 1600x900 frame takes ~0.1 s in Release).
- The observer's altitude channel is the same `alt` as `observer alt=` (above sea level, floored
  at the ground). A path in follow mode is not supported: `path play` ends follow mode.

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

**Camera lock.** `select const "Starlink Gen1"; camera look sel; track on; time scale 1x` — the
satellite stays centred while the sky moves; `camera fov=20` zooms in without releasing the lock, and
the reticule answers it with four lock-on bars (`SelReticuleTick` in a `ui dump`). `track.satcmd` is
the worked version.

**UI check.** `ui scale 2.0; ui open settings tab=Controls; wait 3; ui dump controls; capture controls ui=on`,
then read `text_overlaps` in the result before looking at the picture.

## Ambient sound

The ambience (CLAUDE.md, "Subsystem: Ambient sound") is verified at three levels, and only the last
needs ears:

1. **State.** `audio state` / the `ambience` block of every capture sidecar: the drivers and the
   layer gains. `tools/harness/scripts/ambience_tour.satcmd` visits ~19 golden places (beach, open
   sea, plains at night, a Reflect Orbital beam site, forest, dawn, jungle day/night, Sahara, LA at
   night, Alps, Greenland, 11 km, 30 km, over the aurora, beside a Starlink, inside the AI datacenter
   disk, 20,000 km) and `audio expect`s the right layers at each — a wrong layer fails the run.
2. **Signal.** A muted harness run has an audio engine with NO device: nothing plays, and nothing
   is mixed until `audio record` pulls the graph synchronously — deterministic (seeded synths) and
   independent of frame rate. `imgtools.py audio <wav...> -o spec.png` gives, per file, RMS / peak /
   ungated LUFS, energy per band, stereo correlation, transients per second, tonal peaks, and a
   log-frequency spectrogram with an RMS strip: chirps, beeps and clicks show as shapes, a hum as
   lines, wind as a moving band, a loop seam as a vertical edge.
   `tools/harness/scripts/ambience_solos.satcmd` renders every layer alone at gain 1 plus a music
   reference — the calibration run.
3. **Feel.** The user's: play the WAVs, or run with `--sound`.

The mix rule: at every tour stop the ambience totals about 10 dB under the music (the tour's last
recording is the music reference; compare `lufs_ungated`). A solo render mutes the one-shots (gulls)
as well as the other voices.

Motion: a camera path moves the camera, so `path key 0 ... alt=1500; path key 8 lat=+0.05 ...;
path play fps=30; audio state` shows `speed_mps` / `eas` — a jump by `observer` does not (the
teleport guard). No layer uses them since the wind rush was removed (2026-09-25).

Gotcha: ambience.json and the samples reach the exe folder only on a BUILD (the runtime-sync
stamp) — edit, build, then run, or the old table plays.

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
| `audio a.wav [b.wav ...] [-o spec.png] [--width 1000]` | levels, bands, correlation, transients and a stacked spectrogram per `audio record` WAV (see "Ambient sound") |

For multimodal review: models see large images downscaled, so a one-pixel feature in a
1600x900 frame can vanish. Use `capture ... crop=... scale=4` or `imgtools crop` for anything
small, and `sheet` to compare many variants in one image.

`tools/harness/lightscan.py` (Pillow only, no numpy - it still runs where the system Python is too
new for numpy wheels) reads the terrain-lighting NUMBERS back out of a `debugview` capture. The
`terms`, `direct`, `skyamb`, `night`, `moon` and `aurora` views write linear radiance x100 straight
into the frame, bypassing exposure and tonemap, so the byte is `srgb_encode(v * gain)`; the tool
inverts the sRGB curve and divides by the gain, which turns "that patch looks grey" into a value and
a hue per term.

| | |
|---|---|
| `--x 800 --y 400 560 820 --view direct,skyamb` | decode those terms at those pixels |
| `--split` | per view, the biggest down-column jumps (where a column breaks) |
| `--hue [--ref name.png]` | the beauty frame's own RGB down the column |

Gains: the light terms and `skyambraw` x100, `nightmap` x20, and `day`/`albedo`, `gates`, `factors`,
`aofactors`, `geodot`, `sunvis`, `suntint` raw. Only pixels the terrain march claimed (`tHit > 0`) are
overridden, so sky rows show the ordinary sky in every view - check `geodot`'s B (`tHit/4000`) or
`probe` first. A run folder holds one capture per view NAME: a per-time-step `*_sunvis` script such as
`scripts/shadow_trace.satcmd` collides, so pass the frame you mean explicitly.

## Verifying the harness

`python tools/harness/selftest.py` (≈2 minutes) runs real scripts and checks capture sizes and
sidecars, bit-identical determinism, a knockout changing and then restoring the image, settings
round-trips and error reporting, script parse errors, `wait settle` holding time, `time sun`
accuracy, perf and sweep numbers, selection/follow/`ui dump`, `probe` (the seeded march agrees
with a march from the eye) and `debugview`, a recorded camera path (spline midpoint, frame count),
the watchdog, and live mode — 13 tests. Run it after changing `src/Harness.*` or
`src/simulations/SatelliteSimHarness.cpp`.

## Debugging a rendering problem (worked example)

The terrain work that motivated the harness went roughly like this, and the pattern generalizes:

1. `capture` the problem at a fixed view (`observer`, `time sun`, `camera`, `wait settle`), and a
   `sheet` of the same view with each feature toggled (`set ... 0`, `knockout +...`) — which toggle
   makes it disappear?
2. `debugview` the suspect channel. Stripes in `normals` but not `albedo` point at the geometry
   (it was a weak hash); a saturated `detail` view means the noise lost its zero mean.
3. `probe` a pixel on the artifact and one next to it. "seed SKY, but a march from the eye hits at
   2.6 km after 150 steps" located a budget bug that no picture could have.
4. Crash or device loss? Run the same script with the validation layer:
   `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation python tools/harness/run.py ...` — the messages
   land in `app_stdout.txt`. (That is how a mip-size mismatch in a new texture upload was found.)
5. `perf` the view with each feature toggled to attribute cost; `debugview steps` shows where a
   raymarch spends it.

## Gotchas

- **Build Release.** A Debug build encodes each 1600x900 PNG in tens of seconds.
- **`run.py` finds the exe under `build/` only** — `build/<config>/SAT_LIGHT_SIM_V_*.exe`, freshest
  first, and nothing else unless you pass `--exe`. `build-win-release/` is where the `windows-release`
  preset puts the release build, so building *that* tree and then running the harness runs the previous
  binary, silently and completely. Line 1 of `satlight_log.txt` says which one ran: `<commit>, built
  <date>` comes from `<build dir>/generated/version.h`, and that stamp is written at **CMake-configure**
  time (`CMakeLists.txt:42-66`), not per build. Entry 5 of the reset tally is this trap: a run meant to
  check a build fresh out of `build-win-release` reported `f9aa040` — the `build/Release` exe from 75 s
  earlier.
- **A reset can leave the always-on recorder dead, and silently (fixed 2026-09-26).** It starts under
  `pythonw` from the logon task, so a refusal or a crash is invisible unless you look: after any reset,
  read `harness_runs\blackbox\daemon.log` (every start, refusal and stop now goes there) and check the
  newest `<day>.csv`'s mtime. The lock used to name its holder by pid alone — a reset leaves it behind
  and Windows reuses the pid within the minute, so the next boot's recorder refused and exited without
  a word. It now names (pid, creation time) and treats a pre-boot lock as stale. See *The flight
  recorder* in `docs/HARNESS.md`.
- **The window must stay visible (not minimized)**, or the swapchain stops presenting. It opens
  without taking focus.
- Captures are the final 8-bit tonemapped swapchain image, not HDR buffers.
- A capture's sidecar state is taken when the capture is requested, which is the frame it shows.
- `set` changes values without touching the preset label. A later `preset` command overwrites
  the preset's sliders.
- `get` values are floats, so `0.2` reads back as `0.20000000298`.
- **`time sun` is relative to the clock, so the render is *script-position dependent*.** Each
  `time sun` moves the clock to the nearest matching moment (within 12 h), so splitting a script in two,
  reordering views or inserting a view in front silently moves the later views in time — including the
  *date*. That is not cosmetic: the same `v5` view (same observer `lat=39.75 lon=-104.6`, same camera,
  the Sun's az/el agreeing to 0.002° — `87.1866°` vs `87.1879°`, el 35.0000 in both) differed over
  **32.6 %** of the frame, mean 26/255 across the changed pixels, between the full script (which left it
  on 2036-06-20) and the split (2036-06-21). The capture sidecars show the moved day
  (`state.time.utc`) and a different Moon (`state.moon.illum` 0.101 vs 0.176, `moon.az_deg` 22° apart).
  Starting the script with `time set <ISO>` does **not** pin it — the whole preceding `time sun` sequence
  decides. So when an A/B spans two runs, keep the command sequence identical, or at least check that the
  sidecars agree on `state.time.utc` before believing a pixel diff: a moved day looks exactly like a
  regression.

### Machine-level resets (tally)

The platform has hard-reset the whole machine several times — a platform event, not an app crash (4 of
the 9 resets in the last 72 h happened with no app running at all). Windows leaves two witnesses on the
next boot, and **neither is a crash dump**:

- **WHEA-Logger 1** in the System log: the firmware's raw CPER record, hex in `EventData.RawData`,
  severity `fatal`, `notify=BOOT` (UEFI BERT). Its timestamp fields are **binary, not BCD**, and **UTC**.
  Read what it *says* before reading it as a hardware fault: decoding all 46 of them (below) shows the
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

### History: the resets predate the app (2026-09-26)

`crashes.py --history` reads `Microsoft-Windows-Kernel-WHEA/Errors`, which keeps every fatal record
Windows found at boot far longer than the System log does. On this machine: **46 fatal records,
2025-05-29 .. 2026-09-26, 19 of them before this repository's first commit (2026-03-11).** They come
in episodes (2025-06/07, 2025-10, 2026-02, 2026-08-01..21, 2026-09-22..26), often several within
minutes, with long quiet stretches between (none 2026-03..07, the project's busiest months). So the
app is at most a *trigger* — the entries below are when a launch happened to coincide with an episode.
The 2026-09-26 00:11 reset (entry 5) added nothing to this channel (its boot found no firmware record
at all); entry 6 (00:40) added the 46th, and decoding it says what the other 26 PMC records say — which
is why the tally below is kept by hand either way.

### Decoding the CrashLog

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

### The flight recorder (`blackbox.py`)

A reset erases everything in memory and Windows writes no dump, so the only record of what the
hardware was doing is one already on disk. `tools/harness/blackbox.py` samples the GPU (`nvidia-smi`,
100 ms: P-state, power avg + instant, clocks, temperature, utilisation, **PCIe link gen/width**, the
active clock limiter) and the CPU (perf counters, 1 s: % performance, % utility, MHz, the ACPI thermal
zone) and fsyncs every line. `run.py` and `live.py start` record one into every run folder
(`blackbox.csv`, from ~1 s before the launch, so the idle → load step is in it); `crash_witness.txt`
and the `crashes.py` report print its last 20 s — the final P-state / PCIe link transitions, peak
power and temperature, and the marks.

```powershell
python tools/harness/blackbox.py --daemon      # ALWAYS ON: harness_runs/blackbox/<UTC day>.csv (250 ms, 14 days kept)
python tools/harness/blackbox.py --install-task   # same, but it comes back after a reset (logon task, one instance only)
python tools/harness/blackbox.py --out bb.csv -- cmake --build build --config Release --target accuracy-gate
python tools/harness/blackbox.py --summary harness_runs/blackbox --at "2026-09-25 21:57:53"   # local time
```

**Run `--daemon` whenever the machine is in use**, and use `--install-task` to keep that promise across
the resets: half of them happened with no harness run live, and those are exactly the ones nothing
recorded. Entry 5 is what skipping it costs — nothing was recording machine-wide (no `harness_runs\blackbox`
existed), so the only record of that reset is the run's own `blackbox.csv`, which starts one second
before the launch and ends where the machine died: 8 samples of an idle GPU, 15 of a launch, and nothing
at all about the 44 s the machine spent frozen.

**Installed on this machine (2026-09-26):** the logon *task* could not be created — `schtasks /create
/sc onlogon` answers `Access is denied` from an unelevated shell, and `--install-task` says so and
falls back to its no-admin half — so what keeps the recorder alive across a reset here is the Startup
launcher `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\satlight-blackbox.cmd`, pointing at
`C:\Python314\pythonw.exe …\blackbox.py --daemon`. It is there and current. So
`schtasks /query /tn "SatLightSim blackbox"` answering "cannot find the file" is *expected* here, not a
fault; check the `.cmd` and `daemon.log` instead.

**The file's two time columns are meant to be the same instant, and were not.** A row is
`epoch,utc,src,fields`; the epoch column is printed with `%.3f` (rounds to the millisecond) while the
`utc` column was built with `int((t % 1) * 1000)` (truncates), so whenever the fractional second was
≥ .0005 the two disagreed by 1 ms — **3450 of the 7122 rows** in `20260926.csv`, entry 6's freeze block
included, and half of every older `blackbox.csv` as well. Two symptoms: a report can print a stamp no
row in the file has, and two samples ~1 ms apart share an epoch value while their `utc` columns differ.
`_utc` rounds now and both columns come from one rounded instant, verified over 300 freshly written
rows; the always-on recorder was restarted on the fixed code at **07:59:18.960Z** and every row since
agrees (`stored utc − epoch column = 0`). Rows before that — and any older run's `blackbox.csv` — can
read 1 ms early in the `utc` column: when correlating with the event log, the `epoch` column is the
authority.

**The recorder has to be *checked*, because it can be silent.** The logon task starts it under
`pythonw`, where stdout goes nowhere, so a refusal or a crash used to leave no trace at all — and on
2026-09-26 that is what happened. The 00:40 reset left `harness_runs\blackbox\daemon.pid` behind (a
reset cleans nothing up), the pid inside it was reused by an unrelated process within the minute, and
the recorder the 00:41 logon started read a *live* pid, concluded another recorder was running and
exited — silently, so the freeze under investigation had no machine-wide recording at all until it was
noticed by hand. `DaemonLock` now names its holder by **(pid, creation time)** rather than a bare pid
(reuse cannot counterfeit a creation time), treats a lock written before this boot as stale whatever it
says, and logs every start, refusal and stop to `harness_runs\blackbox\daemon.log`. So after a reset:
`Get-Content harness_runs\blackbox\daemon.log -Tail 5` and the newest `<day>.csv`'s mtime are the two
things to look at before trusting either.

At idle the GPU sits at P8 with
the PCIe link at **Gen 1** and a launch retrains it to Gen 4 at the same moment the power steps up —
one of the transitions the recorder is there to catch. First measurement (`harness_runs\viewer_glare_1`,
2026-09-26): a 7 s run went through **five** P-state/link changes (P8 gen1 → P0 gen4 → P5 gen2 → P8
gen1 → P5 gen2 → P0 gen4), ~23 W → ~220 W; the first came at 06:40:41.618Z, **16 ms before `init:
buffers`** — i.e. at the `Logical device created` / `Swapchain created` lines where the first four
in-run resets' logs stopped. Entry 5's own file shows the same step inside a single 100 ms sample
(`07:11:02.244Z` P8/gen1/23.6 W → `07:11:02.355Z` P0/gen4/1800 MHz), 1.4 s before the machine died.
A correlation, not a cause: the decoded CrashLog decides.

The app's side: `satlight_log.txt` stamps are **UTC with milliseconds** since 2026-09-26 (they line
up with `blackbox.csv`), and `SatelliteSim::init` logs `init: <step>` before each step of the launch
window (buffers, the three noise bakes, the cloud/depth targets, the Earth textures, pipelines, mesh
renderer) — the ~2 s every in-run reset has fallen inside, which the log used to show as one silent
gap. Since 2026-09-26 the Earth-texture step is subdivided as well: it logs `init: texture: <file>`
before each of its nine decodes (entry 5 died 1.2 s into that step and nothing in the log said which
file; entry 6 died *inside* one of them and the stamp named it by file). Those stamps price the step:
2.0 s total, of which `earth_elevation.png` is 888 ms (14999×7500, R8 — 112 MB decoded, the launch's
slowest step by 3×; an earlier note here said 21600×10800 / 233 MB, which is not the file) and the two
8K JPEGs 277 and 271 ms, against 7 ms for the noise map. The first texture line is 2 ms after the
step's own line, so the profile is what sits between them.

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

**All six resets that caught a launch did it in its first ~3 s — the launch window** — and
`satlight_log.txt` says where: a healthy launch writes 67 lines and ends at `sky pipeline: FULL
sat_sky.frag`. Entries 1-4 stopped at line 6-7, within one line of each other (1 and 3 at `Logical
device created.` with line 7 committed all-NUL; 2 and 4 at `Swapchain created`) — those four ran builds
from *before* 2026-09-26, which had no `init: <step>` breadcrumbs to stop on (`git log -S 'init:
buffers'` dates them to `c112930`). Entry 5 is the first reset caught by a build that had them, and it
landed on line 13, `init: Earth textures (decode + upload)` — 1.23 s into that step, i.e. *past* the
device/swapchain pair the earlier four died in, inside the launch's largest CPU burst (nine texture
decodes + uploads, 8K JPEGs and the 14999×7500 elevation map among them; `SatelliteSim::init` now logs
one line per texture, see *The flight recorder*). The lines after it in a healthy launch are `env stars: …` and
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

**It is not deterministic, and it is not the app.** 12 run folders finished between entry 2 and
entry 3, and 3 more between entry 3 and entry 4 — and in the same 72 h, **4 of the 9 resets happened
with no app running at all** (09-24 23:51, 09-25 08:29, 08:40, 16:34; `crashes.py` prints who was
live, if anyone), so entry 6's by-hand launch only had the recorder watching it. Treat a reset as a
per-launch risk, not a regression to bisect: one launch at a time, and split scripts rather than
writing one long one, since a reset costs that run (and its unflushed tail) and nothing else.

**A reset is not a crash.** A stale `session.lock` only means "the process never cleaned up", which is
also what an *app abort* looks like — and aborts are common here: `harness_runs\bisect1`, `bisect2` and
`tv1p3` (2026-09-25 02:48-02:49) end in `FATAL: vkQueueSubmit failed.`, and `tw_terms_02` (21:44:38) and
`tw_terms_03` (21:45:24), both launched *after* entry 3's reset, end in `FATAL: SatelliteSim: failed to
create cloud_noise bake pipeline` — the bug fixed at 21:47, after which `tw_terms_fixed` and
`tw_terms_fix1` both finished. All five aborted the process and lost nothing to an all-zero write. The
reset is the one whose log simply *stops*, usually with the next line committed as NUL.

Earlier occurrences predate this table: see *History* above (46 records since 2025-05).

To check a single file by hand:
`python -c "import sys;b=open(sys.argv[1],'rb').read();print('all-zero' if b and not any(b) else 'ok')" <file>`
— `--scan-run` does that (and the zero-tail scan) for a whole folder.

## Extending

Commands live in `SatelliteSim::harnessExec()` (`src/simulations/SatelliteSimHarness.cpp`): match
on `c.name`, read `c.pos` / `c.kv` (`c.num`, `c.flag`, `c.str`), put results in `a.result`
(`"message"` is the one-line summary), `throw` (via `fail()`) on bad input. A command that needs
more frames returns `Status::Pending` and keeps state in `a.scratch`, with `a.frame` counting its
calls. Commands run at the top of `buildUI`, before this frame's camera derivation and
`recordCompute`. Update the table above and `kHelp` together.
