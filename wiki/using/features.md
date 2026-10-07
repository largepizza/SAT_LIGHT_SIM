# Features tour

A walk through what you can do in SAT LIGHT SIM: find your way around the screen, pick out satellites and planets,
inspect one in 3D, plot how bright it gets over a pass, control time, save places as bookmarks, take pictures and
films, and find your way around the settings. The keys are on [Controls](controls.md); how each thing is computed
is linked from each section.

## The screen

Two panels sit along the bottom of the screen; **Tab** hides them for a clean view.

| Panel | Contents |
|---|---|
| Bottom left | The UTC clock and speed, the time buttons (slower, pause, faster, reverse) and the picture buttons (Screenshot, HQ photo, Star trails) |
| Bottom right | Latitude, longitude, altitude with an **MSL / AGL** switch (above sea level or above the ground), the frame rate, the version, and the menu buttons: **Bookmarks**, **Cinematics** and **Settings**. A menu button is lit while its window is open |

The icons are solid white silhouettes; hover any button to see its name.

**Typing a position or a time.** Click the clock, the latitude, the longitude or the altitude and it becomes a
text box (Enter applies, Esc cancels):

| Field | Accepts |
|---|---|
| Clock | `2036-11-22 02:06` or `2036-11-22 02:06:30` (UTC), the ISO form with `T` and `Z`, a date alone, or `HH:MM[:SS]` for that time on the current date. The year must be 1800 to 2200 and the day must exist in its month <!-- history-ok --> |
| Latitude, longitude | Decimal degrees with a sign or a letter (`34.05 N`, `-118.25`, `118.25W`); `lat, lon` typed into either field sets both |
| Altitude | A number in the panel's unit (km or mi), or with a unit (`350 m`, `12 km`, `30000 ft`, `5 mi`); it is above sea level or above the ground according to the MSL / AGL switch |

Text that does not parse shows an amber notice and changes nothing. Moving the observer this way ends follow mode.

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
Under the picture four tiles show its **magnitude** as you see it, its **elevation**, its **range** and the
**phase** angle. Below them are collapsible sections, each a list of labels and values:

| Section | Contents |
|---|---|
| SATELLITE *(open)* | Type, model, size, mesh (triangle and part counts) |
| ORBIT *(open)* | Altitude, inclination, RAAN, period, and the power lost to the flare-mitigation tilt |
| BRIGHTNESS | Magnitude as seen, above the air, the extinction, the magnitude at 1000 km, the phase angle; a **Trace pass** button |
| SKY POSITION | Visibility, elevation, azimuth and range from your position |
| VIEW | Camera (Free, From you, Toward you), light (Live, Studio), pose (Sunlit, Rest), and switches for Spin, Shadows, Reflections, Surface detail, Glare and Markers (a cyan dot and line toward you; for a mirror, an orange line to the ground site it is aiming at); **Reset view** |
| CHECK | A photometric check: the rendered picture integrated into a magnitude (Render), the brightness model's value (Model) and the difference; **Run check** |

The readouts always refer to your parked position on the ground, even while you are following the satellite.
How the brightness is computed is on [Satellite photometry](../simulation/photometry.md).

## Magnitude traces

**Trace pass** opens a plot of the selected satellite's brightness over its current pass, or the next one within
two orbits. The window opens at the bottom left, above the time controls.

- Two rows of tiles summarise it. **Pass**: the peak magnitude, its time, the length of the pass and the highest
  elevation. **Now**: the clock, and the current magnitude, elevation and phase.
- The plot shows the apparent magnitude (after the atmosphere), the magnitude above the atmosphere and the phase
  angle, with a marker at the present moment. The legend and the axis titles share one line above it.
- **Live** (on by default) retraces up to ten times a second when anything that would change the curve changes:
  you move, the selection changes, the pass ends, a relevant setting changes. With Live off, a line says when you
  have moved, and **Retrace** updates by hand (its tooltip shows what one retrace costs).
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

The panel at the bottom left shows the simulated UTC time and the speed. Its buttons are slower, pause, faster
and **R** (reverse). Speeds run from 1x through 10x, 1 minute, 5 minutes, 1 hour, 1 day, 1 week and 1 month up to
1 year per second. Time can run backwards at any speed, and every satellite, beam and mirror is a pure function of
time, so going back and forth through the same moment gives the same sky.

The program always starts at the same moment, 22 November 2036, 02:06 UTC <!-- history-ok -->, and the clock is
real UTC: the Sun, Moon and planets are where they will really be. To go to another moment, click the clock and
type a date and time (see [The screen](#the-screen)), or use the faster speeds.

### Eclipses

The Moon follows a real lunar theory and the Sun's and Moon's sizes are their true ones, so eclipses happen at
their real times and places. Two fall within a year of the start: the total lunar eclipse of 31 January 2037
(greatest near 14:00 UTC) <!-- history-ok --> and the total solar eclipse of 13 July 2037 (totality across
central Australia, greatest near 02:43 UTC). <!-- history-ok --> During a solar eclipse the sky darkens, the
horizon turns orange and the corona appears; during a lunar eclipse the Moon goes red. See
[Moon and eclipses](../rendering/moon-and-eclipses.md).

## Bookmarks

The **Bookmarks** button (bottom right) opens a window that saves places and moments. A bookmark keeps the
observer's position and height, the camera's direction and zoom, the sim time and the cloud map's position (the
clouds drift, so the place and time alone would show different weather).

- Type a name (optional) and press **Add current view**. The next frame without the interface becomes the
  bookmark's 256 × 144 thumbnail.
- Each bookmark is a card: the thumbnail (click it to go there), its name (click to rename), when and where it
  was taken, and **Go**, **Update** (replace it with the current view and a new thumbnail) and **Delete** (click
  twice to confirm).
- **Go** ends follow mode and Track and jumps straight there. A bookmark taken while following a satellite comes
  back as a free camera at the camera's place.
- **Restore defaults** adds back any of the bookmarks that ship with the sim that are missing from the list. It
  leaves your own bookmarks alone.

Up to 112 bookmarks fit. They are stored in `bookmarks/bookmarks.json` in the user data folder, with the
thumbnails as PNG files beside it.

### The bookmarks that ship with the sim

A first run, with no `bookmarks.json` yet, starts with nine bookmarks. Deleting all of them leaves an empty list,
which stays empty; **Restore defaults** brings them back.

| Bookmark | What it shows |
|---|---|
| Venezuela Rainbow | Sunrise behind a storm over northern Venezuela (9.97 N 68.27 W). Starts paused, since the storm changes within minutes |
| Great Australian Eclipse | The total solar eclipse of 13 July 2037 at greatest eclipse (21.75 S 138.31 E): the corona in a 15° field <!-- history-ok --> |
| Blood Moon over Alice Springs | The total lunar eclipse of 31 January 2037 <!-- history-ok --> |
| Aurora from Orbit | A strong geomagnetic storm (Kp about 6.8) seen from 420 km over Siberia |
| Reflect Orbital over Anchorage | The line of Reflect Orbital mirrors above the city |
| AI Satellites at Twilight | The intro's vantage on the Big Sur coast |
| Los Angeles by Night | The city lights from 10 km |
| Starmind Formation Rings | Beside one of Starmind's eight-satellite formation rings at 565 km. Starts paused |
| Sunset from Orbit | The Earth's limb at sunset from 420 km |

A bookmark may also set the clock when you go to it: the shipped ones run at 1x, and the two marked above start
paused. The shipped files live in `default_bookmarks/` next to the program.

## Pictures

| Tool | How | Result |
|---|---|---|
| Screenshot | F12, or the camera button in the bottom-left panel | `screenshots/satlight_<date>_<time>.png` at window size, without the interface |
| HQ photo | F8, or the sparkle-camera button | `screenshots/satlight_hq_<date>_<time>.png` |
| Star trails | `F`, or the trails button | A long exposure: stars, planets and satellites leave streaks until you switch it off |

**HQ photo** pauses time, renders the scene offscreen at a multiple of the window size and lets it settle for a
number of frames before saving, so the clouds and other temporally accumulated effects are fully converged. All
the detail that depends on pixel size (terrain, city lights) follows the finer pixels, so a photo shows more
detail than the window, not just more pixels. At 2× a 1600 × 900 window (a 3200 × 1800 photo) it takes a
couple of seconds.

| Setting (Display tab, *HQ photo*) | `settings.json` key | Default |
|---|---|---|
| Resolution (x window) | `display.photo_scale` | 1 (1 to 4) |
| Settle frames | `display.photo_frames` | 48 |

Star trails' decay and brightness are the **Trail decay (s)** and **Trail gain** sliders on the Photometry tab.

## Cinematics

The **Cinematics** button (a clapperboard, bottom right) opens the **Cinematics** window, an editor for camera moves made of keyframes.

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

The **Settings** button (a gear, bottom right) opens the settings window. Its tabs are listed down the left side
under four headings, in a list that scrolls on its own. Six tabs are always there; eight more appear when **Show
advanced settings** (Display tab) is on.

| Heading | Tab | What it holds |
|---|---|---|
| GENERAL | Display | *Graphics*: preset, render scale, automatic render scale, clouds following the render scale, temporal anti-aliasing, frame limiter, beam pointing rays. *Window*: window mode, text scale, units. *HQ photo*. *Startup*: play the intro on startup, ask for the graphics mode on startup, replay the intro or the tutorial. *Settings*: show advanced settings, reset to defaults |
| | Controls | *Movement* (move speed), *Bindings* (each action's key and gamepad button, with Rebind and Bind), *Mouse* (look, zoom, select), *Invert look*, *Help* (the controls overlay); see [Controls](controls.md) |
| | Sound | Volume, music, ambience and the ambience mix; see [Sound](../sound/index.md) |
| SKY | Constellations | Every shell with ON/OFF, **HLT** (highlight: draw every satellite of the shell at least faintly, to see its pattern) and **VIEW** (select its highest satellite in your sky and open its 3D view); the planets |
| | Photometry | *Brightness*, *Sky dimming*, *Point sources*, *Bloom and glare*, *Dark sky* (Milky Way and zodiacal light), *Star trails*, *Satellite models* (part occlusion, reflections), *Bulk export* |
| RENDERING *(advanced)* | Clouds | The volumetric clouds: shape, lighting, quality |
| | Weather | Storms, lightning, rain and snow, weather evolution, fog, dust, ice fog |
| | Atmosphere | Scattering, airglow, zodiacal light, aurora (with a live readout of the current space weather) |
| | Terrain | Relief detail, materials, shadows, close-up textures |
| | Night lights | City lights, roads, light sprites, night sky light |
| | Ocean | Waves, sea state, reflections, satellite glints on the water |
| | Beams | Orbital-mirror beams and their light in the clouds |
| | Performance | The GPU and CPU cost of each part of the frame, *Save snapshot*, the knockout sweep and the knockout switches; see [Profiling](../development/profiling.md) |
| ABOUT | Attributions | Credits for data, imagery, textures and sounds |

Every window and tab is built from the same few parts: sections headed in small capitals over a thin rule,
readouts as a label column and a value column, and switches, choices and buttons as small pills. Every slider's
value can be clicked and typed ([Controls](controls.md#typing-values)).

Changing any slider on a rendering tab sets the graphics preset to **Custom**. See
[Graphics settings](graphics-settings.md).

## Where in the code

- `src/simulations/SatelliteSimUI.cpp`: the HUD (`buildUI()`, `buildRightHudPanel()`), the typed fields
  (`setSimTimeFromText()`, `setLatLonFromText()`, `setAltitudeFromText()`), the selection panel
  (`buildSelectedSatPanel()`), the info window, trace window, settings tabs and the UI kit (`uiSection()`,
  `uiKV()`, `uiStatTile()`, `uiButton()`, `uiToggleRow()`, `uiChoiceRow()`).
- `src/simulations/SatelliteSimBookmarks.cpp`: bookmarks.
- `src/simulations/SatelliteSim.cpp`: screenshots (`requestScreenshot()`), HQ photos (`requestPhoto()`), follow
  mode (`updateFollow()`), traces and bulk export.
- `src/simulations/SatelliteSimCinematic.cpp`, `src/simulations/Cinematic.cpp`: cinematics.
- `src/simulations/SatelliteSimTutorial.cpp`: the tutorial.
