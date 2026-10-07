# Automation harness

A small command language that drives the running app: put the observer anywhere, set the time
by Sun elevation, change any setting or knockout bit, select or fly to satellites, then capture
screenshots (each with a JSON record of the exact state), dump the UI layout, sample GPU/CPU
timings, or run the knockout sweep. Scripts are deterministic: running one twice gives
bit-identical images.

Use it for anything that needs the real renderer: shader and terrain changes, image A/Bs,
performance, UI layout, camera shots. Things that don't need the renderer (photometry, orbits,
benchmarks) stay in `SatModelTool`, which is faster and runs in CI.

Be efficient with your sim launches. There is currently a bug (9/27/2026) on the host machine where the app entirely freezes and halts the machine. Only use the harness when strictly necessary, if something seems like a "one-liner" (ie: settings change, constant modification, small-medium tweak) or if is a matter of taste (ie: adjusted sim sounds), do not use the harness and defer judgement to the user.

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

`run.py` adds `--exe`, `--config`, `--quiet`, `--boot-screen on|off`, `--boot-capture`, `--boot-chooser N`, `--boot-safety-ms MS` (see "The
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
| `wait settle [frames]` (or `settle`) | hold sim time and run 40 (or N) frames at a 0.5 s step, so eased values converge: sky glare, the dark-sky dome, mesh fades, beam-light fades, environment probes. The sky TAA restarts with it. **From orbit, add `wait 90` after it before a capture:** a settled capture from 400 km draws the clouds' edges in ~5-km blocks, which 90 ordinary frames clear (review 11; not the settle length — 120 settle frames or holding the march at full rate left them) Use before every capture that follows a change |
| `time set <ISO>` | e.g. `2036-06-21T04:00:00Z`. Real UTC (the Earth rotation angle is GMST since 2026-10-03; an older script's time + 5 h 15 min 01 s gives its old view). `time sun` is still the easy way to a local time of day |
| `time sun <el> [rising\|setting]` | the time nearest now (within 12 h) when the Sun is at `<el>` degrees for this observer. `time sun noon` / `time sun midnight` |
| `time add <s>`, `time j2000 <s>` | relative / absolute (seconds since J2000) |
| `time pause`, `time play`, `time scale <1x\|10x\|1m\|5m\|1h\|1d\|1w\|1mo\|1yr>`, `time reverse on\|off` | |
| `observer lat= lon= [agl=\|alt=]` | move the observer. `alt` = metres above sea level (the shaders' meaning of the height offset: the eye is at max(ground, alt) + 2 m). `agl=0` = on the ground exactly; `agl>0` is exact too: the command takes one extra frame to read back the GPU's own ground (DEM + terrain detail) at the new position. With the depth pass knocked out (bit 1024) it falls back to the CPU's 18 km/px DEM copy, which is off by hundreds of metres on coasts and in valleys. `state` reports `alt_m`, `agl_m`, `ground_m` (+ `ground_source` gpu/cpu), `terrain_cpu_m` and the raw `height_offset_m`. Keeps the camera heading. Ends follow mode |
| `beams [list]`, `beams go [rank=1] [dist_km=0] [bearing=270] [agl=2\|alt=] [look=site\|up\|none]` | where the Reflect-Orbital beams land NOW: converged beams per target site, from the last beam readback — only satellites above the OBSERVER's horizon make beams, so stand in the region first (`observer ...`; an empty list is usually that). `go` puts the observer `dist_km` from the rank-th busiest site along `bearing` (measured from the site; `agl` uses the CPU's approximate ground) and aims at the site's ground point (`site`) or up the beams toward their satellites (`up`). Frame beams with this, never a guessed time and place; force `clouds_v2.coverage` 0 / ~1.6 for a clear / cloudy night (harness_runs/cloud_v2_p10b.satcmd) |
| `snapshot <profile_log.jsonl> [index=-1] [settings=on\|off] [drift=intro\|default]` | reproduce a perf snapshot the user saved (Settings → Display → Save Snapshot; `perf_profiles/profile_log.jsonl` next to their exe) — **the way a location is handed over**. Applies the record's settings, sim time (paused), the exact observer direction and height, the camera and the cloud map's drift PHASE (session state the intro sets: lat/lon + time alone framed different clouds). `index` counts snapshot records only (knockout sweeps are skipped), from the end when negative. The intro is kept off (the record's settings name `play_intro_on_startup`, and a patch that does re-arms it — it flew the camera away). Records from before 2026-09-28 have no settings or phase: run with `--settings` = the user's settings.json, and the phase is reconstructed as a session that played the intro (`drift=intro`, default; ~300 m of error per 10 min between intro and snapshot) or the compiled default offset (`drift=default`) |
| `camera az= el= fov=` | azimuth (0 = north, 90 = east), elevation, vertical FOV (0.5-120) in degrees |
| `camera look <sun\|moon\|sel\|planet>` | aim once. `sel` = the selected satellite or planet |
| `camera track <...\|off>` | re-aim every frame (a moving satellite stays centred) |
| `select sat <index>` | by roster index |
| `select const "<name>" [n=<k>]` | the constellation's member highest in the observer's sky, or its k-th member. `const list` shows names |
| `select planet <name>`, `select none` | |
| `follow [sat=<i>] [offset=along,cross,radial] [fly=on]`, `follow off [fly=on]` | fly with a satellite (types with a geometry model). The offset is in metres in the satellite's frame. `fly=on` (review 22): the UI's Go to / Exit FLIGHT (0.8-2.5 s by the log of the distance; both ways along the parked observer's line of sight to the satellite when clear, review 24; the aim is free on arrival) instead of the jump — `wait` frames to watch it; `state`'s `observer.follow_flight` is 1 (out), 2 (home) or 0 |
| `track [on\|off]` | the selection panel's **Track** button: lock the camera onto the selected satellite and re-aim every frame (the observer stays put, so WASD still walks, and the wheel's `camera fov=` zoom is untouched). No argument = toggle; needs a satellite selection. Released by `select none`, `select planet`, `follow` and any explicit aim (`camera az=`/`el=`, `camera look`, `camera track`, a scripted camera key). `state` reports it as `camera.tracking` and `selection.track`. Not to be confused with `camera track <target>`, which is the harness's own aim-every-frame |
| `viewer [aim=free\|observer\|toward\|sun] [light=live\|studio] [glare=on\|off] [shadows=on\|off] [dist=<radii>]` | the 3D view's own controls, for a `ui open viewer` / `ui open info` capture: `aim=observer` is the Observer chip (the satellite from the ground observer's direction), `aim=sun` (harness only) looks from the Sun's side, where a Sun-facing array glints; `dist` is in model radii. `glare=on` (the default) draws the main view's glare on the glints that make the flare the observer sees (Live light and a tracked satellite only). Reports `flare_per_i` (the observer's effectFlare per unit intensity; 0 = no glare), `glints_last_frame` (the previous frame's glint list: `wait` a frame after a change) and the PHOTOMETRY lines |
| `const list`, `const "<name>"\|all on\|off [highlight=on\|off]` | constellation visibility |
| `expect <key\|cine.<path>\|state.<path>> <value> [tol=1e-4]` | fails the run unless the setting (or the open cinematic's JSON, `cine.name`, `cine.shots.0.keys.1.t`, or the `state` JSON, `state.observer.lat_deg`, `state.time.utc`) equals the value (numbers within tol, relative past 1) |
| `get [section[.key]]` | any persisted setting; `get` alone lists them all (the keys are `settings.json`'s) |
| `set <section.key> <value>` (also `key=value`, several per line) | change settings through the same code path `settings.json` loads through. Unknown keys and wrong types are errors. Doesn't change the preset label |
| `preset <Planetarium\|Low\|Medium\|High\|Ultra\|Potato\|Custom>` | apply a graphics preset (it overwrites knockouts and quality sliders) |
| `knockout none\|<mask>\|+key\|-key\|key ...`, `knockout list` | debug knockout bits by stable key (`terrain_march`, `volumetric_cloud_march`, ...; `potato_sky`, `lite_sky`). Sets the preset to Custom, like the Display tab does |
| `capture <name> [ui=on] [crop=x,y,w,h] [scale=s]` | PNG of the frame (no UI unless `ui=on`), cropped then scaled: `scale<1` box-filters down, `scale>1` enlarges with nearest neighbour for pixel-level inspection. Writes `<name>.json` with the state |
| `photo <name> [scale=1-4] [frames=N]` | the HQ photo (F8 / the hotbar's sparkle camera): renders `scale` x the window offscreen with the clouds at full rate and time paused, accumulates `frames` frames, saves it as `<name>.png` (no UI, no sidecar). Sets `display.photo_scale` / `photo_frames` when given |
| `state [name]` | the full state as the command result (and a file if named); `clouds_v2.rate` is the march's rate this frame (sparse / full / half / adaptive); `satellites.visible_count` the compact visible list's length (satellites + city light sprites); `clouds_v2.sun_cloud_t` the Sun's cloud transmittance from the eye (review 22), `clouds_v2.fast_lod` the fast-flight cloud LOD this frame; `clouds_v2.rain` the rain particles (mode, instances, levels) and the rain map / light at the eye (`rate_eye`, `rate_near_max`, `max_at_en_m` = where the map peaks, metres east/north: walk there with time paused, the clouds drift ~40 m/s) |
| `probe <x> <y>` | what the terrain algorithm does for one pixel's ray (`terrain_probe.comp`, the same functions the renderer uses): the seed from the shared depth, the seeded march and a march from the eye (distance, steps), the DEM/detail/roughness at the hit, and a 64-sample `profile_t_rayalt_dem_H` (t, ray altitude, DEM, DEM + detail) along the ray. Pixel coordinates are the capture PNG's |
| `debugview <off\|normals\|detail\|steps\|albedo\|shadow\|rough\|elevzebra\|distzebra\|erosion>` | replace terrain pixels with a debug channel: normals, detail height / amplitude, march steps (blue few .. red the budget), albedo, sun shadow x Lambert, roughness/rock/snow as R/G/B, zebra stripes every 25 m of elevation / 100 m of hit distance (broken or jagged stripes = height or convergence jitter), and the erosion octaves alone (grey = none, bright ridge / dark gully relative to their bound, blue = too gentle a slope for any). Not persisted |
| `debugview solar` (27) | solar PV parks (reflector targets of kind `solar`): R panel coverage as seen, G the park's ground, B the panel normal's east component (trackers) |
| `debugview <oceanrefl\|oceanfresnel\|oceanstate\|oceansurf\|oceannormal\|oceanshore>` (40-45) | the SEA's channels (2026-09-30): the sky reflection x0.05, fresnel / below-horizon reflection factor / reflection strength, sea state /2 / crest / detail blend, the surface before the Moon and glints, the wave normal, the shore distance (r land side, g water side, /100 m). Written raw into the swapchain: a capture's sRGB value^2.2 reads the number |
| `debugview oceanshadow` (50) | the sea's cloud shadow (r, 1 = lit) and the reflected clouds' weight (g, 1 = clear-sky march) — review 17 found the "blocky reflections" in r |
| `debugview <cloudairsplit\|cloudtrans\|cloudrad\|cloudalpha>` (46-49) | the cloud COMPOSITE in sat_sky.frag: the air split distance (/100 km; the haze in front of a cloud is split there), the cloud transmittance, its radiance x20 (saturates on lit cloud), the raw alpha (r opaque distance, g translucent mean distance, /100 km). Specks and stair-steps along cloud edges show up in 46 when they are the air split's |
| `eclipse <solar\|lunar>` | the next eclipse after the current time (the real ones: Meeus Moon, GMST Earth; solar = a partial seen anywhere, lunar = umbral): sets the time to greatest eclipse, puts the observer under the Moon (lunar) or where the shadow axis meets the Earth (solar), aims at the Moon; reports the Sun fraction the observer sees. `state`'s `moon` block has `sep_from_sun_deg`, `sun_seen_frac`, `drawn_radius_deg`, `sun_radius_deg`. `scripts/eclipses_2037.satcmd`, `scripts/moon.satcmd` |
| `aurora [next <kp> [days=]\|substorm [min=]\|kp <v>\|auto]` | the aurora's activity (`SpaceWeather.cpp`, a pure function of sim time). Bare: reports it. `next <kp>` (default 7) jumps sim time to the PEAK of the next storm reaching that Kp (auto mode, searched in 15-min steps up to 1100 days); `substorm` jumps to 10 min after the next substorm onset whose intensity passes `min=` (0.4); `kp <v>` fixes the activity (manual mode, substorms still run); `auto` returns to the space-weather history. `state`'s `aurora` block: `kp`, `g_scale`, the drivers (`quiet`, `hss`, `cme`), `oval_mlat_deg`, `substorm`, and the observer's `mlat_deg` / `mlt_h` / `oval_overhead`. Test from DARK sites (Coldfoot 67.25 N 150.18 W; eastern Montana 46.9 N 106 W): a city's skyglow hides the aurora. `scripts/aurora_oval.satcmd`, `scripts/aurora_ground.satcmd` |
| `perf [frames=60] [name=]` | average raw GPU timestamp buckets and CPU buckets over N frames, plus the GPU total and wall frame-time distributions. `name=` also appends it to the run's perf log |
| `shaders reload [march=<spv>] [wg=<X>x<Y>]` | rebuild the clouds v2 march and resolve pipelines from the SPVs on disk (after `cmake --build build --config Release --target CompileShaders` and a copy of the two SPVs into `build/Release/shaders/` — the full build's post-build copy fails while the app holds files open) and drop the cloud history. A shader iteration without an app launch: use it with one `live.py` app. `march=` loads another SPV (path relative to the exe) so variants interleave in one batch; `wg=` sets the march's workgroup size (specialization constants, default 16x16), which bounds the registers the driver may give a thread. Reports the driver's statistics (`VK_KHR_pipeline_executable_properties`: Register Count, Binary Size, Local Memory Size (spills; the high 32 bits are junk)). **Check Register Count before timing a variant**: NVIDIA gives this shader either 128 or ~227 registers and the second is 20-60% slower (the 2026-09-29 perf sprint: `harness_runs/perf_sprint/`, the scripts `ab.sh`, `attr.sh`, `mk.sh` there) |
| `lightning` | the lightning flashes in progress (the previous frame's list, `cloud_v2_lightning.comp`): each one's id, intensity, cloud-to-ground or not, `sprite` (a red sprite, kind 2), age, distance and observer-relative position, plus the thunder queue (`thunder_pending`, `thunder_rolls`). Flashes follow sim time, so with time paused one stays frozen: `time add 0.25` steps through a flash |
| `lightning spawn kind=cg\|ic\|spider\|sprite dist_km= az= [seed=] [base_m=1500] [top_m=10000] [ground_km=4] [ground_az=] [dur=]` | place a flash by hand (2026-10-05): it runs through the same channels, glow and thunder as the storms' own (heights above the eye's ground). Step through it with time paused and `time add` (leader 0-50 ms, then the strokes); for thunder, `wait` a few frames, `time add` the sound's arrival (distance / 343 m/s), then `audio record`. `lightning` also lists the built channel `trees` (segments by order) and `drawn_segments` |
| `sweep` | the automated knockout sweep (≈15 s); returns the whole record |
| `ui show\|hide`, `ui scale <0.75-2>` | HUD visibility and UI scale |
| `ui open <settings [tab=Name]\|viewcontrols\|trace\|info\|viewer\|cine\|bookmarks\|console>`, `ui close <name\|all>` | windows (`info`/`viewer`/`trace` need a selected satellite). An advanced tab (incl. Performance) turns on "show advanced settings". A UI kit row is clicked as `Key:index` (`ui click ReplayIntroBtn:0`, `ui click InfoSectHdr:2`). A `ui click` presses on the frame the pointer arrives, which the sky picker can take for a click on empty sky: re-`select` before a window that needs the selection |
| `bookmark add [name]\|go <n>\|update <n>\|rename <n> <name>\|delete <n>\|list` | the Bookmarks window's actions (n from 1). The thumbnail is captured a few frames later (`wait 3`); results list each bookmark (`thumbnail` true once it has one). `scripts/ui_windows.satcmd` |
| `tutorial start [step] [pad=1]`, `tutorial step <n\|name>`, `tutorial next\|back\|skip\|state` | the first-run tutorial (never started on its own in a harness run). Steps 1-9 or look/move/altitude/boost/select/actions/time/capture/settings; its action steps finish on real input, so `next` moves past them. `pad=1` shows the gamepad graphics. Results carry `step`, `progress`, `done_showing`, `tutorial_done`. `scripts/tutorial.satcmd` |
| `pad <a\|b\|x\|y\|lb\|rb\|start\|view\|ls\|rs\|up\|down\|left\|right>` | a gamepad button press as `pollGamepad` handles it, no controller needed: its context meaning first (the tutorial card's Start/View, the selection's D-pad focus / A / B), else its binding. Marks the pad as the last input. Results carry `action`, `pad_focus`. `scripts/gamepad_nav.satcmd` |
| `ui open cine` | the Cinematics window (see "Cinematics") |
| `ui click <ElementId>[:index] [fx=0.5] [hold=N]` | the scripted pointer moves onto the element's centre (last frame's layout; ids as in `ui dump`), presses for a frame and releases — the real hover/click path. Fails if the element is off screen (scrolled out of its window) |
| `ui type <text>`, `ui key <enter\|esc\|tab\|backspace\|delete\|left\|right\|home\|end\|w>` | keyboard input as typed (`onChar` / `onKey`): into the focused text field, else the game. Results carry `text_focus` and the field's `buffer` |
| `ui dump [name]` | every drawn rect/text/image with its box and element id, plus checks: text cut by its scissor, text off the window, and overlapping text under the same scissor |
| `window <W>x<H>` | resize the window and wait for the new swapchain |
| `path clear`, `path key <t> [lat= lon= alt= az= el= fov= sim=<ISO>\|simadd=<s>]` | camera-path keyframes at path time `t` (seconds). Channels you leave out inherit from the previous key (from the current view for the first). See "Camera paths" |
| `path goto <t>` | jump to the path's pose at `t` |
| `path play [fps=30] [record=<name>] [ui=on] [scale=s]` | play the path at a fixed frame rate; with `record` every frame is captured as `captures/<name>_00000.png` ...; the result carries `gpu_ms_mean` (cloud_march and total, the GPU cost while moving — `perf` measures a still view), and (review 22) `gpu_total_ms_p90` / `gpu_total_ms_max` and `gpu_worst_frame_ms` (the worst frame's buckets): spikes hide in a mean |
| `overlay text <id> "<text>" [x=0.5 y=0.1 size=28 align=center\|left color=RRGGBB]` | screen text at fractional coordinates, drawn even with the HUD hidden |
| `overlay label <id> "<text>" target=<sel\|sun\|moon\|planet> [size= dx= dy=]` | a label that follows a sky target |
| `overlay clear [id]` | |
| `audio [state [name]]` | the ambience: every context driver (altitude, Sun, ocean, land cover, beam, shells, ...) and every layer's target and current gain, loudest first in `audible`. With a name, also `captures/<name>.audio.json`. Capture sidecars carry the same block as `ambience` |
| `audio record <name> [seconds=8] [bus=ambience\|music\|sfx\|all\|music+ambience] [solo=<layer>]` | renders the mix offline into `captures/<name>.wav` (+ `<name>.json`: state + levels) and returns RMS / peak / per-second RMS. Needs the default muted run (no device). See "Ambient sound" |
| `audio expect <layer,...> [absent=<layer,...>] [min=0.05]` | fails unless each listed layer's gain is at least `min` and each `absent` one below it — a location tour checks itself |
| `audio force <layer> <gain\|off>`, `audio force off` | pin a layer's gain wherever the observer is (calibrating one voice) / release them all |
| `audio music [next\|prev\|pause\|play\|end\|state]` | the music player: returns the track, its name, paused, the gap countdown, `has_upwell` and `upwell_gain`. Offline a track never ends by itself; `end` ends it now, as if it had played out, so the between-track gap (and the key's glide through it) is reachable |
| `audio tonality [wait]` | the key the ambience is in: root (Hz, as played and its target), source (track / gap / manual), key, chord, scale and chord intervals, the tuning-curve cents. `wait` holds the script until every track is analysed (a harness run analyses synchronously, so it rarely waits). Also the `tonality` block of `audio state` and every sidecar's `ambience` |
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
- **A recording renders exactly one app frame per path frame** (the result's `frames` field is the
  app frames the command took: path frames + 1). Until review 15 (2026-09-30) it waited for each
  PNG's ENCODE while the app kept rendering at the held pose — ~12 app frames per recorded frame —
  so the clouds' history and the sky TAA converged between recorded frames, and every motion test
  showed far less ghosting than a player sees. It now waits for the copy only; the next frame's
  `finalizeScreenshot` joins the previous encode on the main thread (a stall, not a frame).
- The observer's altitude channel is the same `alt` as `observer alt=` (above sea level, floored
  at the ground). A path in follow mode is not supported: `path play` ends follow mode.

## Cinematics (review 17)

The camera paths are one object shared by the harness and the in-app **Cinematics** window (the film
button on the right HUD panel): a cinematic is a list of SHOTS, each a spline through keys (`Cinematic.h`).
`path key` / `path clear` / `path play` edit and play the CURRENT shot; `cine` manages the rest:

| Command | |
|---|---|
| `cine new [name]`, `cine name <n>` | start a cinematic / rename it (the save file and export folder names) |
| `cine shot add [name]` / `del` / `<n>` | add a shot (made current; its sim time starts now) / delete / select (1-based) |
| `cine shot look [clear]` | store the current look with the shot (clouds, clouds_v2, photometry, constellations, planets, render settings) — applied at its cut |
| `cine shot follow <sel\|index\|off>` | the shot rides with a satellite: its keys are the camera's OFFSET and VIEW in the satellite's frame — frame each with `follow offset=along,cross,radial` (the view stays put in the satellite's frame) and `camera`, then `cine key`. `path goto <t>` on a follow shot goes through the cinematic's evaluation (follow mode stays on) |
| `cine shot ease <0-1>` | ease in/out over the shot (0.5 default: the camera starts and stops gently; sim time is not eased) |
| `cine shot simrate <x>`, `cine shot simnow` | sim time rate along the shot (0 = frozen) / its start = now |
| `cine key [t=]` | the current view as a key (2 s after the last by default) |
| `cine play [shot]` | real-time playback (the whole cinematic, or the current shot) |
| `cine export preview\|hq [fps=] [scale=] [frames=] [blur=] [shot]` | frames to `captures/cine_<name>[_hq]/frame_NNNNN.png` (in-app: `screenshots/cinematics/<name>_<stamp>/`) + `cinematic.json` + a README with the frames2video command. **preview** = rendered in motion at a fixed 1/fps; **hq** = every frame a settled HQ photo (`scale` x the window, `frames` settle frames per frame). `blur=N`: motion blur, each frame the linear-light mean of N subframes over a 180-degree shutter (HQ: the subframes share the settle budget, at least 4 each) |
| `cine save [file]`, `cine load <file>` | JSON in `<user data>/cinematics/` (`sat-light-sim-cinematic/1`) |
| `cine state`, `cine stop` | the cinematic as JSON (shots, keys) / stop playback or an export |

Text entry (2026-10-03): the window's name fields, each key's time and every slider value are text boxes
(SatelliteSimUI.cpp "Text fields"); `tools/harness/scripts/text_fields.satcmd` checks them through `ui click` /
`ui type` / `ui key` / `expect`, and `cine_follow.satcmd` checks that a follow key plays back with its framing and
that HQ and preview exports finish (run it with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`: App presents
progress frames during the HQ export).

A cut (the next shot) resets the sky TAA and the clouds' history, so nothing of the last shot smears into the
first frames. The worked example, the application tour, is `tools/harness/scripts/tour.satcmd`.

Between keys the camera follows a Catmull-Rom spline per channel (log altitude and FOV); sim time is the
shot's start + its rate x the shot time unless keys set `sim=`. While a cinematic plays or exports it owns
the camera and the clock (WASD and look are off). Video: `python tools/harness/frames2video.py <dir>/frame -o out.mp4 --fps 30`.

## Temporal stability (`tools/harness/tstab.py`)

How far the image seen WHILE MOVING is from the image the same view settles to: ghosting, smears,
blocks and grain are all that distance. Built in review 15 from the user's snapshot.

```
python tools/harness/tstab.py gen <profile_log.jsonl> [--index -1] -o harness_runs/tstab/t.satcmd        [--live-time] [--only walk,pitch,...] [--variant "tag:key value;key value"]...
python tools/harness/run.py harness_runs/tstab/t.satcmd --window 1920x1009 --out harness_runs/tstab/run1
python tools/harness/tstab.py score harness_runs/tstab/run1 [--sheet]
python tools/harness/tstab.py compare run1 run2
```

- Scenarios run at the PLAYER's speeds from the snapshot's height above the ground: `walk` (WASD,
  1 x AGL per second), `boost` (Shift, 6.25x), `strafe`, `pan90` / `pan240` (deg/s yaw), `walkpan`,
  `rise` (Q), `boostrise`, `pitch` (+-25 deg sweeps), `boostpan`, `flick` (720 deg/s turn, hold,
  turn back), `chaos` (boost + yaw + pitch + climb). 60 fps, 2.5 s each.
- Each path is recorded every frame, then 5 sampled poses are revisited (`path goto`) and settled
  160 frames for the reference. `--live-time` runs sim time at 1x along the path (the reference holds
  that instant), as a player sees it. `floor` = two settles of one view (the noise floor, ~0.4).
- `score`: mean / p95 / p99 |luminance diff| (0-255) and the share > 16 levels, per scenario and per
  horizontal band (top..bottom); `--sheet` writes moving | settled | |diff| x4 rows. Variants share a
  launch (their scenarios are prefixed `tag-`); `path play`'s `gpu_ms_mean` in results.jsonl is the
  cost while moving.
- **Yaw-only pans hide row-aligned errors** (the horizon's structure is horizontal): the pitch
  scenario is what found review 15's depth-history bug (mean 5.4 -> 1.3).
- **Run it over CLOUDS, and only the scenarios that isolate the question** (the user, 2026-10-04): motion bugs live
  mostly in the clouds' temporal passes, and the clear-night Reflect-site snapshot (profile_log_1004e 24) measures only
  the sky TAA. The full 12-scenario suite on three scenes cost ~15 min and was too bright to show anything; the climb
  trails the user reported were measured with `--only rise,boostrise` on two of their snapshots in ~2 min a build:
  `harness_runs/trails/r37|r38.satcmd` (profile_log_1004j 37 = Pacific cumulus from 2 km, 38 = Edmonton from 4.3 km).
- **Sample before you commit to a benchmark.** One launch that loads each candidate snapshot, captures it still and
  records a few seconds of the motion in question (no settled references: `harness_runs/trails/look.satcmd`), then
  look at the frames: keep only the candidates where the problem is on screen (clouds, city lights, beams, a
  horizon), then run the scored benchmark on those, on both builds. The harness is bit-deterministic run to run, so
  one run per build is enough.

## Still-view flicker benchmark (`harness_runs/rscale/bench4.satcmd` + `bench4.py <run>`)

Four of the user's still views with city lights (Irvine horizon from 330 m, SF Bay from 1.2 km, SoCal from 1805 km,
Dallas from 3.5 km), 61 frames each, scored with `flicker.py` (below). Any change to the sky TAA or the city lights
runs all four: a fix measured on one view (the orbit one) doubled the flicker at the low horizon (2026-10-04).
Local files (harness_runs/ is gitignored; the snapshots are profile_log_1004i 35-36, 1004h 33, 1004f 30).

## Still-view flicker (`tools/harness/flicker.py`)

Per-pixel temporal statistics of a recorded sequence with the camera still (review 18, the MEO cloud flicker):
`python tools/harness/flicker.py <run>/captures/<name>_0 [--skip 10] [--crop x,y,w,h] [--heat out.png]` prints
the mean per-pixel std of luminance, the mean frame-to-frame change, the p99 std and the share of pixels whose
std passes 8 levels (visible flicker); `--heat` writes the std map x8. Record with two identical path keys
(`path key 0; path key 1.5; path play fps=30 record=x`), time running or paused (paused = the march's own
sampling noise). Crop to the Earth from orbit: the AI disk's million satellites are a noisy ring in space.
`harness_runs/meo/flick.satcmd` is the 2000 / 8000 / 20000 km set.

## Climb consistency (`tools/harness/climb.py`)

Are the clouds the SAME clouds as the eye climbs (review 19)? Every level of detail must be a filtered version of the
one below it, never a different placement.

```
python tools/harness/climb.py gen <profile_log.jsonl> [--index -1] [--fov 40] -o harness_runs/climb/c.satcmd [--variant "tag:key value;key value"]...
python tools/harness/run.py harness_runs/climb/c.satcmd --window 1600x900 --out harness_runs/climb/run1
python tools/harness/climb.py score harness_runs/climb/run1 [--sheet] [--ref 8 --ref 40 --ref 200]
python tools/harness/climb.py match run1 tagA tagB      # two variants at the same altitudes (march vs far layer)
python tools/harness/climb.py compare runA runB
```

- `gen` restores the snapshot, pauses time and climbs straight up (camera el -89.9) through 15 altitudes, 8 to 2500 km.
- `score` reprojects each frame onto the next (exact nadir geometry on a sphere 1.5 km up), filters it to that frame's
  footprint and correlates the CLOUD detail (red channel above its local background: cloud shadows on the sea are real
  at 40 km and sub-pixel from orbit, so they are not counted). Per step: r, the masks' IoU, the cloud fractions; then r
  against fixed references (`--ref`), where drift that each step hides adds up. A reference far below a frame covers
  only a few dozen of its pixels: trust the 40 / 200 km references and the steps over the 8 km one.
- Use fov 40: a wide FOV mixes footprints in one frame. Variants share a launch.

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
   `tools/harness/scripts/ambience_beams.satcmd` does the beam sound at the tour's beam site facing the
   Reflect ring (`glare_n` ~440): with the music playing it reads the track's upwell stem up
   (`audio music state`: `upwell_gain` 1.0) and the beam layers absent, looks away (upwell back to 0),
   then ends the track (`audio music end`) and reads the beam layers riding `music_gap` through the
   gap (0.15 early, 0.95 halfway, 0.25 late), recording music + upwell, the full mix and each beam
   voice. `ambience_tonality.satcmd` reads the key per
   track and through a between-track gap (`audio music end`). Read the numbers before believing a
   recording.
2. **Signal.** A muted harness run has an audio engine with NO device: nothing plays, and nothing
   is mixed until `audio record` pulls the graph synchronously — deterministic (seeded synths) and
   independent of frame rate. `imgtools.py audio <wav...> -o spec.png` gives, per file, RMS / peak /
   ungated LUFS, energy per band, stereo correlation, transients per second, tonal peaks, and a
   log-frequency spectrogram with an RMS strip: chirps, beeps and clicks show as shapes, a hum as
   lines, wind as a moving band, a loop seam as a vertical edge.
   `tools/harness/scripts/ambience_solos.satcmd` renders every layer alone at gain 1 plus a music
   reference — the calibration run. `beam_glare` and `beam_site_hum` are soloed at the beam site with
   the camera on the Reflect ring: anywhere else they would measure silence. For timbre and level work
   without a launch, `SoundTool --render <synth> --out x.wav voices=0/0,1/2,6/8` renders one voice with
   keyframed params (tools/sound_tool/main.cpp).
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

Gains: the light terms (incl. `nightsky`, the night sky + moonlit sky on the albedo) and `skyambraw`
x100, `nightmap` and `citylights` (the night map with its blue base removed) x20, and `day`/`albedo`, `gates`, `factors`,
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
- **The startup graphics chooser** (CLAUDE.md "Startup graphics chooser") is skipped in a harness run
  — a run never waits for input. `run.py --boot-chooser N` shows it scripted (env
  `SATLIGHTSIM_BOOT_CHOOSER=pick=N`): two frames on the default, two on option N (0 Full, 1
  Planetarium, 2 Potato), then it starts; with `--boot-capture` those frames are `boot_NN.png` too.
  `state` reports `render.boot_choice`, `full_sky_deferred`, `full_sky_pipeline`, `ask_graphics_mode`.
  `tools/harness/scripts/boot_chooser.satcmd` checks the pick and the deferred pipelines.
- The boot safety net (> 250 ms frames right after loading step one tier down) is off in a harness
  run; `run.py --boot-safety-ms MS` arms it at that threshold (e.g. 1, to watch it fire).

## Cleaning up run folders

`python tools/harness/gc.py` (dry run by default; `--apply` deletes) removes run folders from `harness_runs/`
and `harness_live/run_*` older than `--keep-days` (7) unless they are PINNED (`gc.py --keep <run>` writes a
`KEEP` file), REFERENCED by name from CLAUDE.md, docs/, .plans/ or the agent memory folder (a note citing
`harness_runs/cloud_v2_p10b` keeps that evidence), or the live app's current folder. `--slim` also thins the
`path play record=` frame sequences of kept runs older than `--slim-days` (1) to every 30th frame — most of
the space. Pin a run you will want to compare against later before it ages out.

## Launch spacing (read this before scripting many runs)

Launching the app has frozen the whole development machine, most recently when one run was relaunched
right after another exited (docs/FREEZES.md). So:

- `run.py` and `live.py start` wait **30 s after the last app exit and after the last launch**, and until
  fewer than **2 app processes** are running on the machine (`tools/harness/launchgate.py`; `run.py
  --cooldown S` overrides the spacing, never set it to 0 by habit; `SATLIGHT_HARNESS_MAX_APPS` the cap).
  The gate's state is machine-wide (`%LOCALAPPDATA%/SatLightSimHarness`), so agents in separate git
  worktrees share it, and the running count comes from the process list (the user's own app counts).
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
