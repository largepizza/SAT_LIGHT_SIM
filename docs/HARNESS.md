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

`run.py` adds `--exe`, `--config`, `--quiet`, `--boot-screen on|off`, `--boot-capture` (see "The
loading screen") and `--forensics` (see "Freeze forensics").

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
  captures/boot_NN.png          `run.py --boot-capture`: the loading screen's frames
  blackbox.csv         only with `run.py --forensics` (docs/FREEZES.md)
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
- **`run.py` runs the freshest `build/<config>/SAT_LIGHT_SIM_V_*.exe` and nothing else.** A build
  in another tree (`build-win-release/`, the `windows-release` preset) is invisible to it; pass
  `--exe`. Line 1 of `satlight_log.txt` names the build that ran, but its commit stamp is written at
  **CMake-configure** time, not per build.
- **The first launch after a shader rebuild is slow** (~10 s extra): the driver compiles every
  pipeline from scratch. Don't read launch or perf timings from that run.
- **The window must stay visible (not minimized)**, or the swapchain stops presenting. It opens
  without taking focus.
- Captures are the final 8-bit tonemapped swapchain image, not HDR buffers. A capture's sidecar
  state is taken when the capture is requested, which is the frame it shows.
- `set` changes values without touching the preset label; a later `preset` overwrites its sliders.
  `get` values are floats (`0.2` reads back as `0.20000000298`).
- **`time sun` is relative to the current clock**, so each one moves the clock to the nearest
  matching moment (within 12 h). Splitting, reordering or inserting views silently moves the later
  ones in time, sometimes to another *date* (a different Moon, a 30 % pixel diff). `time set` first
  does not pin it. For an A/B across two runs keep the command sequence identical, and check that
  the sidecars agree on `state.time.utc` before believing a diff.

## The loading screen

Every launch, harness runs included, shows the loading screen (black, title, version, then the init
steps scrolling down and fading). Its frames all come before `init()` returns, i.e. before the
script's first command, so scripts are unaffected.

- `run.py --boot-capture` writes every loading frame to `captures/boot_NN.png`.
- `satlight_log.txt` has one `boot: <step> (<ms since launch>)` line per step: the launch's cost
  per step.
- `run.py --boot-screen off` is the old white-window launch, for comparison.

## Launch spacing (read this before scripting many runs)

Launching the app has frozen the whole development machine, most recently when one run was relaunched
right after another exited (docs/FREEZES.md). So:

- `run.py` and `live.py start` wait **30 s after the last app exit** before launching
  (`tools/harness/launchgate.py`; `run.py --cooldown S` overrides it, never set it to 0 by habit).
- For many small batches, start **one** app with `live.py start` and `send` to it; don't loop `run.py`.
- Batch your checks into one script where you can. Every launch is a risk; plan them.

## Freeze forensics

`docs/FREEZES.md` holds the freeze history and the tools built for it (`blackbox.py`, `crashes.py`,
`overnight.py`). None run unless asked: `run.py --forensics` records GPU/CPU telemetry into the run
folder and checks the event log before launching.

## Extending

Commands live in `SatelliteSim::harnessExec()` (`src/simulations/SatelliteSimHarness.cpp`): match
on `c.name`, read `c.pos` / `c.kv` (`c.num`, `c.flag`, `c.str`), put results in `a.result`
(`"message"` is the one-line summary), `throw` (via `fail()`) on bad input. A command that needs
more frames returns `Status::Pending` and keeps state in `a.scratch`, with `a.frame` counting its
calls. Commands run at the top of `buildUI`, before this frame's camera derivation and
`recordCompute`. Update the table above and `kHelp` together.
