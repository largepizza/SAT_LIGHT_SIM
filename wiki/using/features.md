# Features tour

A walk through what you can do in SAT LIGHT SIM: pick out satellites and planets, inspect one in 3D, plot how
bright it gets over a pass, control time, take pictures and films, and find your way around the settings. The
keys are on [Controls](controls.md); how each thing is computed is linked from each section.

## Selecting satellites and planets

Left-click a point of light to select it, or press `T` (A on a gamepad) to select whatever is nearest the centre
of the screen. Planets (Mercury, Venus, Mars, Jupiter, Saturn, Uranus) win over satellites when both are under the
cursor. Clicking empty sky clears the selection.

A satellite that is large enough on screen is drawn as a 3D model instead of a point, with no selection needed.
Those are picked by their outline. See [Satellite meshes](../rendering/satellite-meshes.md).

### The selection panel

A small panel follows the selected object, with a reticle around it. For a satellite it shows its constellation
and number, its type and its current brightness as an apparent magnitude. For a planet it shows its magnitude,
distance and phase.

A satellite's panel has a row of icon buttons. Hover one to see its name.

| Button | What it does |
|---|---|
| **Info** (an italic *i*) | Opens the satellite window: a 3D view and the satellite's orbit, brightness and geometry. Satellites with a geometry model only |
| **Go to** (an eye) | Flies out to the satellite and rides along with it ([follow mode](controls.md#follow-mode-go-to)). Satellites with a geometry model only |
| **Trace pass** | Plots the satellite's brightness over its current or next pass |
| **Track** (a sight) | Keeps the camera pointed at the satellite while you stay where you are. Works for every satellite |

A lit button means the action is active (the eye while following, the reticle while tracking). When the selected
satellite is off screen, a chip at the edge of the screen points to it and carries the same buttons.

## The satellite window and the 3D view

**Info** opens a window with a rendered view of the real satellite, where it is right now, lit by the real Sun
and the light of the Earth below it. Drag inside the picture to orbit the camera around it, scroll to zoom.

Four icons sit over the picture:

| Chip | Effect |
|---|---|
| **Spin** | The camera slowly circles the satellite |
| **Observer** | View it from your direction on the ground: this is the side, and the glint, that you see from below |
| **Studio** | A fixed studio light instead of the live Sun, to see the shape clearly |
| **Maximize** | Pop the view out into its own large, resizable window (the small view keeps running) |

The window title shows the satellite's name and, in its title bar, **Select** (select it in the sky) and **Go to**.
Below the picture are collapsible sections:

| Section | Contents |
|---|---|
| SATELLITE | Type, model, triangle and part counts, size |
| ORBIT | Altitude, inclination, RAAN, period, and the power lost to the flare-mitigation tilt |
| PHOTOMETRY | Phase angle, magnitude above the atmosphere, magnitude as seen through it, magnitude at 1000 km |
| OBSERVER | Range and geometry from your position, and a switch for the markers (a cyan dot and dashed line toward you; for a mirror, an orange line to the ground site it is aiming at) |
| CAMERA | Camera presets (free, from you, toward you) and reset |
| RENDER | Light (live or studio), pose, shadows, reflections, surface detail, glare |
| CHECK | A photometric check: the rendered picture integrated into a magnitude, compared with the brightness model |

The readouts always refer to your parked position on the ground, even while you are following the satellite.
How the brightness is computed is on [Satellite photometry](../simulation/photometry.md).

## Magnitude traces

**Trace pass** opens a plot of the selected satellite's brightness over its current pass, or the next one within
two orbits. It plots the apparent magnitude (after the atmosphere), the magnitude above the atmosphere and the
phase angle, with a marker at the present moment and a *Now* readout of the current magnitude, elevation and phase.

- **Live: ON** (the default) retraces up to ten times a second when anything that would change the curve
  changes: you move, the selection changes, the pass ends, a relevant setting changes.
- **Export CSV** writes the trace to `traces/trace_<model>_<satellite>_<time>.csv` in the user data folder. The
  file header records every input (model and its hash, orbit, observer, settings), so the curve can be reproduced
  exactly with `SatModelTool --replay-trace`; see [SatBench and the accuracy gate](../accuracy/satbench.md).

When a satellite cannot be traced (an old-style type with no geometry model, a mirror whose ground sites did not
load, or no pass within two orbits) the window says why.

## Bulk brightness export

Settings → **Photometry** → *Bulk export* samples brightness over time for a whole constellation or for the
selected satellite, for statistics rather than single passes.

| Row | Choices |
|---|---|
| Source | The selected satellite, or any constellation (click to cycle) |
| Window | Next 24 h or next 7 days from the current sim time |
| Cadence | Every 10, 30 or 60 s |

It keeps every instant at which a satellite is at least 20 degrees up, fully sunlit, and the Sun is in the
twilight window used by the benchmarks. It runs in the background (the button shows progress and cancels) and
writes `exports/samples_<source>_<time>.csv` in the user data folder, in the same format the benchmark tool
writes. See [SatBench and the accuracy gate](../accuracy/satbench.md).

## Time

The time bar at the bottom left shows the simulated UTC time and the speed. Its buttons are slower, pause, faster
and **R** (reverse). Speeds run from 1x through 10x, 1 minute, 5 minutes, 1 hour, 1 day, 1 week and 1 month up to
1 year per second. Time can run backwards at any speed, and every satellite, beam and mirror is a pure function of
time, so going back and forth through the same moment gives the same sky.

The program always starts at the same moment, 22 November 2036, 02:06 UTC <!-- history-ok -->, and the clock is
real UTC: the Sun, Moon and planets are where they will really be. There is no date entry; use the faster speeds
to travel in time.

### Eclipses

The Moon follows a real lunar theory and the Sun's and Moon's sizes are their true ones, so eclipses happen at
their real times and places. Two fall within a year of the start: the total lunar eclipse of 31 January 2037
(greatest near 14:00 UTC) <!-- history-ok --> and the total solar eclipse of 13 July 2037 (totality across
central Australia, greatest near 02:43 UTC). <!-- history-ok --> During a solar eclipse the sky darkens, the
horizon turns orange and the corona appears; during a lunar eclipse the Moon goes red. See
[Moon and eclipses](../rendering/moon-and-eclipses.md).

## Pictures

| Tool | How | Result |
|---|---|---|
| Screenshot | F12, or the camera button in the time bar | `screenshots/satlight_<date>_<time>.png` at window size, without the interface |
| HQ photo | F8, or the sparkle-camera button | `screenshots/satlight_hq_<date>_<time>.png` |
| Star trails | `F`, or the trails button | A long exposure: stars, planets and satellites leave streaks until you switch it off |

**HQ photo** pauses time, renders the scene offscreen at a multiple of the window size and lets it settle for a
number of frames before saving, so the clouds and other temporally accumulated effects are fully converged. All
the detail that depends on pixel size (terrain, city lights) follows the finer pixels, so a photo shows more
detail than the window, not just more pixels. A 3200 × 1800 photo takes a couple of seconds.

| Setting (Display tab) | `settings.json` key | Default |
|---|---|---|
| HQ photo resolution (x window) | `display.photo_scale` | 2 (1 to 4) |
| HQ photo settle frames | `display.photo_frames` | 48 |

Star trails' decay and brightness are the **Trail decay (s)** and **Trail gain** sliders on the Photometry tab.

## Cinematics

The film button in the time bar opens the **Cinematics** window, an editor for camera moves made of keyframes.

- A **cinematic** has a name and one or more **shots**, played in order. Each shot is a list of **keys**: a camera
  position, view direction and zoom at a time, joined by a smooth spline. **+ Key at view** adds the current
  camera as a key after the last one; every key's time can be typed, and dragging a key on the **timeline**
  retimes it. Clicking or dragging on the timeline scrubs.
- **Sim time**: a shot runs the clock from a start time at a chosen rate (0 freezes it), unless its keys set the
  time themselves.
- **Ease in / out** makes a shot start and stop gently (0 = constant speed).
- **Store look** saves the shot's graphics and cloud settings (and the cloud map's position) with it, so a cut to
  that shot restores them.
- **Follow satellite** turns a shot into a follow shot: the camera rides with the selected satellite, and every
  key stores its position and view in the satellite's own frame, so the framing is kept as the satellite moves.
- **Play shot** / **Play all**, with **Loop**. Esc or **Stop** ends playback. Movement keys are off while a
  cinematic plays.
- **Save** writes `<name>.json` into `cinematics/` in the user data folder; a new name saves a copy. The saved
  list underneath has **Load** and a two-click **Delete**.

**Exporting** writes one PNG per frame to `screenshots/cinematics/<name>_<stamp>/`:

| Export | What it does |
|---|---|
| **Export preview** | Renders each frame at window size, at the chosen frames per second |
| **Export HQ** | Renders each frame as an HQ photo (resolution and settle frames set in the window). The window shows the expected time before it starts |

**Motion blur (subframes)** renders each exported frame as several subframes across a half-frame shutter and
averages them in linear light. While an export runs, a progress bar with the frame count, the time left and
**Stop** stays on screen.

## Settings

The gear at the bottom right opens Settings. Seven tabs are always there; seven more appear when **Show
advanced settings** (Display tab) is on.

| Tab | What it holds |
|---|---|
| Constellations | Every shell with ON/OFF, **HLT** (highlight: draw every satellite of the shell at least faintly, to see its pattern) and **VIEW** (select its highest satellite in your sky and open its 3D view); the planets list |
| Sound | Music and ambience volumes, the track player, ambience details; see [Sound](../sound/index.md) |
| Controls | Bindings, movement speed, axis inversion; see [Controls](controls.md) |
| Camera | A reminder of the mouse controls |
| Display | Graphics preset, render scale, temporal AA, frame limiter, window mode, text scale, units, HQ photo, intro and tutorial replay, performance readouts, reset to defaults |
| Photometry | Brightness, sky suppression, extinction, light pollution, the point, bloom and glare styling, flare-mitigation tilt, trails, mesh reflections, part occlusion, bulk export |
| Clouds *(advanced)* | The volumetric clouds: shape, lighting, quality |
| Weather *(advanced)* | Storms, lightning, rain and snow, weather evolution, fog, dust, ice fog |
| Atmosphere *(advanced)* | Scattering, airglow, zodiacal light, aurora |
| Terrain *(advanced)* | Relief detail, materials, shadows, close-up textures |
| Night lights *(advanced)* | City lights, roads, light sprites, night sky light |
| Ocean *(advanced)* | Waves, sea state, reflections, satellite glints on the water |
| Beams *(advanced)* | Orbital-mirror beams and their light in the clouds |
| Attributions | Credits for data, imagery, textures and sounds |

Changing any slider on an advanced tab sets the graphics preset to **Custom**. See
[Graphics settings](graphics-settings.md).

## Where in the code

- `src/simulations/SatelliteSimUI.cpp`: the HUD (`buildUI()`), the selection panel (`buildSelectedSatPanel()`),
  the info window, trace window and settings tabs.
- `src/simulations/SatelliteSim.cpp`: screenshots (`requestScreenshot()`), HQ photos (`requestPhoto()`), follow
  mode (`updateFollow()`), traces and bulk export.
- `src/simulations/SatelliteSimCinematic.cpp`, `src/simulations/Cinematic.cpp`: cinematics.
- `src/simulations/SatelliteSimTutorial.cpp`: the tutorial.
