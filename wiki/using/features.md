# Features tour

A walk through what you can do in SAT LIGHT SIM: find your way around the screen, pick out satellites, rockets and
planets, inspect one in 3D, plot how bright it gets over a pass, control time, watch launches and the Sun's Fury,
save places as bookmarks, take pictures and films, report a problem, and find your way around the settings. The
keys are on [Controls](controls.md), the Missions window on [Missions](missions.md); how each thing is computed is
linked from each section.

## The screen

Two panels sit along the bottom of the screen; **Tab** hides them for a clean view.

| Panel | Contents |
|---|---|
| Bottom left | The UTC clock and speed, the time buttons (slower, pause, faster, reverse) and the picture buttons (Screenshot, HQ photo, Star trails) |
| Bottom right | Latitude, longitude, altitude with an **MSL / AGL** switch (above sea level or above the ground), the frame rate, the version, and the menu buttons: **Issue reports**, **Missions**, **Bookmarks**, **Cinematics** and **Settings**. A menu button is lit while its window is open |

The icons are solid white silhouettes; hover any button to see its name.

**Typing a position or a time.** Click the clock, the latitude, the longitude or the altitude and it becomes a
text box (Enter applies, Esc cancels):

| Field | Accepts |
|---|---|
| Clock | `2036-11-22 02:06` or `2036-11-22 02:06:30` (UTC), the ISO form with `T` and `Z`, a date alone, or `HH:MM[:SS]` for that time on the current date. The year must be 1800 to 2200 and the day must exist in its month <!-- history-ok --> |
| Latitude, longitude | Decimal degrees with a sign or a letter (`34.05 N`, `-118.25`, `118.25W`); `lat, lon` typed into either field sets both |
| Altitude | A number in the panel's unit (km or mi), or with a unit (`350 m`, `12 km`, `30000 ft`, `5 mi`); it is above sea level or above the ground according to the MSL / AGL switch |

Text that does not parse shows an amber notice and changes nothing. Moving the observer this way ends follow mode.

## Selecting satellites, rockets and planets

Left-click a point of light to select it, or press `T` (A on a gamepad) to select whatever is nearest the centre
of the screen. Planets (Mercury, Venus, Mars, Jupiter, Saturn, Uranus) win over satellites when both are under the
cursor. Clicking empty sky clears the selection.

Besides satellites, these can be selected:

- **rocket stages**, from the pad to their re-entry, and Starship's ship on its way home;
- **re-entry fireballs**: a retired satellite or a spent stage burning up, and the re-entries of the
  [Sun's Fury](#the-suns-fury);
- **debris pieces** of the Sun's Fury, which are drawn as 3D pieces close up.

A click picks from what is drawn: an object below your horizon or too faint to show cannot be clicked. Anything
reached by name instead (a row of the [Missions](missions.md) window, a station in the Constellations tab, a
member of the GEO belt's list) is selected whether it is drawn or not, and a rocket stays selected while it is out
of view. A satellite that does not exist at the current date (one not yet launched in the deployment era, or one
destroyed or re-entered in the Sun's Fury) cannot be selected; if the selected satellite is destroyed or re-enters,
the selection is dropped.

A satellite or rocket that is large enough on screen is drawn as a 3D model instead of a point, with no selection
needed. Those are picked by their outline. See [Satellite meshes](../rendering/satellite-meshes.md).

### The selection panel

A small panel follows the selected object, with a reticle around it. Its first line is the object's name: the
constellation and number (`Starlink Gen2 #1234`), a station's or a GEO satellite's own name, or for a rocket its
vehicle, stage and what it is doing (`on the pad, T-9 s`, its burn, T+, altitude and speed). The second line says
what it is:

| Object | Second line |
|---|---|
| A roster satellite | Its satellite type |
| A GEO belt satellite | Its operator and slot (`Intelsat - GEO 27.5 W`) |
| A rocket | `Rocket stage (<model>)` |
| Starship's ship coming home | `Starship ship, entry` |
| A fireball | `Re-entry fireball` |
| A satellite in a train or deorbiting | Its type and phase with its height: `raising to slot, 412/565 km`, `parking orbit`, `in the dispenser`, `deorbiting` |

Then its brightness as an apparent magnitude (for a planet: magnitude, distance and phase). The magnitude is always
what you see from your position on the ground, also while you are following the object: *below your horizon*,
*dark* (in the Earth's shadow, or a rocket that is not burning), or the magnitude after the atmosphere's
extinction. For a burning rocket it is its flame's.

A satellite's panel has a row of icon buttons. Hover one to see its name.

| Button | What it does |
|---|---|
| **Info** (an italic *i*) | Opens the satellite window: a 3D view and the satellite's status, brightness, orbit and mission. Objects with a geometry model only |
| **Go to** (an eye) | Flies out to the object and rides along with it ([follow mode](controls.md#follow-mode-go-to)). Objects with a geometry model only (every modelled satellite, rocket and debris piece) |
| **Trace pass** | Plots the satellite's brightness over its current or next pass |
| **Track** (a sight) | Keeps the camera pointed at the object while you stay where you are. Works for everything selectable |

A lit button means the action is active (the eye while following, the reticle while tracking). When the selected
object is off screen, a chip at the top left of the screen, *Selected: name (out of view)*, carries the same
buttons. It shows the name alone (up to its first comma, colon or bracket, so a rocket's T+ or a spacecraft's phase
is left off), and while you are following something it sits below the follow bar.

## The satellite window and the 3D view

**Info** opens a window on the right side of the screen, as tall as the space above the bottom-right panel. At its
top is a rendered view of the real object, where it is right now, lit by the real Sun and the light of the Earth
below it. Drag inside the picture to orbit the camera around it, scroll to zoom. A burning rocket stage is drawn
with its flame and glowing nozzles. A satellite that has been destroyed or has re-entered shows no model: the view
shows the sky where it was, with *Destroyed* or *Re-entered* and the date.

Four icons sit over the picture:

| Chip | Effect |
|---|---|
| **Spin** | The camera slowly circles the object |
| **Observer** | View it from your direction on the ground: this is the side, and the glint, that you see from below |
| **Studio** | A fixed studio light instead of the live Sun, to see the shape clearly |
| **Maximize** | Pop the view out into its own large, resizable window (the small view keeps running) |

The window title shows the object's name and, in its title bar, **Select** (select it in the sky) and **Go to**.
A title too long for the title bar is cut with "..." before those icons.
Under the picture four tiles show its **magnitude** as you see it, its **elevation**, its **range** and the
**phase** angle. For a burning rocket stage the magnitude is its flame's, as you see it from the ground.

**ACTIONS**, always visible, is one row of four buttons. Trace pass, Go to and Track select the viewed object first.

| Button | What it does |
|---|---|
| Trace pass | Plots its brightness over its pass (not for a rocket or a re-entry) |
| Go to | Flies to it and rides along; reads **Leave** while you are following it |
| Track | Keeps the camera on it as it crosses the sky |
| **Explode** (danger colour) | Breaks it up with one click; see [Breaking a satellite up](#breaking-a-satellite-up) |

Below them are collapsible sections, each a list of labels and values; a long value wraps onto more lines. Only
the sections scroll.

| Section | Contents |
|---|---|
| STATUS *(open)* | What it is: constellation, satellite type, and its state (operational, dead and tumbling, destroyed, re-entered, in a launch, a debris piece). For a rocket, a visiting spacecraft, a fairing half or a re-entry: what it is doing, altitude, speed (over the ground below 80 km, inertial from 150 km) and a time row (below) |
| MISSION *(open)* | For a launch object: its timeline and milestones, with buttons to skip between them; see [Missions](missions.md#the-mission-section-in-the-info-window) |
| BRIGHTNESS *(open)* | Magnitude as seen, above the air, the extinction, the magnitude at 1000 km, the phase angle, and for a formation satellite the formation's combined magnitude |
| SKY POSITION *(open)* | Visibility, elevation, azimuth and range from your position |
| ORBIT *(open)* | Altitude, inclination, RAAN, period, and the power lost to the flare-mitigation tilt. Not shown for a rocket or a re-entry |
| MODEL (TECHNICAL) | Type, model id, size, mesh (triangle and part counts) |
| 3D VIEW | Camera (Free, From you, Toward you), light (Live, Studio), pose (Sunlit, Rest), and switches for Spin, Shadows, Reflections, Surface detail, Glare and Markers (a cyan dot and line toward you; for a mirror, an orange line to the ground site it is aiming at); **Reset view** |
| PHOTOMETRY CHECK | A photometric check: the rendered picture integrated into a magnitude (Render), the brightness model's value (Model) and the difference; **Run check** |

The STATUS time row depends on the object: *Lift-off in* / *Since lift-off* for a rocket, *Glowing for* for a
re-entry, and for a visiting spacecraft its phase: *Since separation*, *Docked since* (or *Berthed since*) with the
UTC time and the number of days, *Since undocking* (or *Since unberthing*), *Since entry interface*. Durations read
`42 s`, `12 m 03 s`, `4 h 12 m` or `116 d 17 h`.

The readouts always refer to your parked position on the ground, even while you are following the object. An
object that the Earth hides from you reads *Below horizon* in SKY POSITION and BRIGHTNESS.
When the object changes into what continues it (a rocket stack into its upper stage at staging, a stage into its
re-entry fireball, a satellite in a train into its slot on arrival), the window and its 3D view follow it, keeping
the camera's direction. How the brightness is computed is on [Satellite photometry](../simulation/photometry.md).

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
1 year per second. Time can run backwards at any speed, and every satellite, beam, mirror, launch and debris piece
is a function of time, so going back and forth through the same moment gives the same sky.

The program always starts at the same moment, 22 November 2036, 02:06 UTC <!-- history-ok -->, and the clock is
real UTC: the Sun, Moon and planets are where they will really be. To go to another moment, click the clock and
type a date and time (see [The screen](#the-screen)), or use the faster speeds.

With the deployment era on (the default; see [Missions](missions.md#the-deployment-era)), going back in time also
goes back in the constellations' history: at a date before 2036 only the satellites launched by then are in the sky.

### Eclipses

The Moon follows a real lunar theory and the Sun's and Moon's sizes are their true ones, so eclipses happen at
their real times and places. Two fall within a year of the start: the total lunar eclipse of 31 January 2037
(greatest near 14:00 UTC) <!-- history-ok --> and the total solar eclipse of 13 July 2037 (totality across
central Australia, greatest near 02:43 UTC). <!-- history-ok --> During a solar eclipse the sky darkens, the
horizon turns orange and the corona appears; during a lunar eclipse the Moon goes red. See
[Moon and eclipses](../rendering/moon-and-eclipses.md).

## Launches and rockets

The constellations are kept flying by launches. Each orbital plane is refreshed once per design life: a rocket lifts
off from a pad the plane passes over, deploys a train of satellites low, the train climbs over weeks to its slots,
and the satellites it replaces deorbit and re-enter as fireballs. Crews fly to the stations twice a year. Rockets are
bright points, then 3D models as you come close; their exhaust trails across the sky, and at twilight it spreads
into a sunlit bell high above a dark ground. Boosters fly back and land, Starship's ships come home to be caught by
their towers, and every upper stage deorbits. The roar arrives at the speed of sound.

The launch sites are drawn on the ground: graded concrete, flame trenches, towers, tank farms and landing zones,
with floodlights and beacons at night. Everything about launches is in the [Missions](missions.md) window: the
schedule, what is flying, the settings, and the milestones of each flight. The physics is on
[Launches](../simulation/launches.md), [Rocket flight](../simulation/rockets.md) and
[Crew flights](../simulation/crew-flights.md).

## The Sun's Fury

On 15 July 2037 at 03:00 UTC <!-- history-ok -->, two days after the Great Australian Eclipse, a Carrington-class
solar storm arrives. Its aurora reaches the mid-latitudes, it kills a share of the satellites in orbit, and the dead
tumble and collide. Every collision is a cloud of debris that feeds the next: a Kessler cascade, computed at
physical rates. Before the storm nothing differs from the plain sky. There is no mode to switch on: the storm
happens when sim time gets there. The [Missions](missions.md#the-suns-fury-card) window counts down to it and has
buttons to go there.

What you can see after it:

- dead satellites tumbling, glinting as they turn;
- debris pieces of a metre and more, drawn close up as torn solar arrays, radiator sections and pieces of bus;
- a glowing dust cloud along each breakup's orbit for about three hours;
- re-entry fireballs as drag brings things down: a glow from about 100 km, a breakup flash, then a train of
  fragments, some tinted by what burns; and, once the cascade runs, many small meteor-like streaks.

The cascade is computed in the background, a little ahead of sim time. When time runs faster than it can be
computed, time holds and a banner says "Computing the debris cascade". The results are cached in the user data
folder (`kessler/`), so a second visit is quick. How it all works is on [The Sun's Fury](../simulation/kessler.md).

### Breaking a satellite up

**Explode** in the [satellite window](#the-satellite-window-and-the-3d-view) breaks the viewed satellite or debris
piece up with one click, at any date. The debris is usually there the same frame and time keeps running; the result
shows as a notice. When the cascade's history has to be rebuilt instead (always before the storm, or after time has
been played back a long way), it is rebuilt in the background while the clock holds for a moment under the banner
"Computing the debris cascade: the breakup". A breakup before the storm starts the cascade at that moment (and switches the scenario on if it was
off). Until the storm arrives, working satellites dodge everything, so only the debris, the dead satellites and the
passive junk collide; after it, the **Collision avoidance** setting applies. Going back in time before the breakup
undoes it. A destroyed satellite is gone everywhere: it cannot be selected, and a destroyed station gets no more crew
flights. Explode is disabled for rockets, re-entries and satellites already destroyed. **Undo my breakups** in the
settings below removes all of yours.

### Settings

In the Missions window's **Controls** tab, section THE SUN'S FURY:

| UI label | `settings.json` key | Default | Effect |
|---|---|---|---|
| Scenario (a storm on 2037-07-15) | `kessler.enabled` | on | The storm and the cascade <!-- history-ok --> |
| Collision rate | `kessler.rate_multiplier` | x1 | x0.1, x1, x10, x100. x1 is physical; higher is faster, not realistic. Re-simulates from the storm |
| Collision avoidance | `kessler.avoidance` | 90% | 0%, 50%, 90%, 99%: the share of encounters a working satellite dodges. Re-simulates |
| Simulate up to | `kessler.horizon_years` | 20 y | 5, 20 or 50 years after the storm |
| Dust cloud brightness | `kessler.dust_gain` | x100 | x1, x10, x30, x100, x1000. At x1 (the physical estimate) a cloud fades below the dark sky within minutes |
| Fireball material colours | `kessler.fireball_colors` | Vivid | Off, Subtle, Vivid: aluminium white, copper green, titanium blue-white, sodium yellow, lithium crimson |
| Small re-entry brightness | `kessler.small_reentry_gain` | x30 | Off, x1, x3, x10, x30, x100 for the small fragments' streaks. x1 is physical, about magnitude +1 from 100 km |

**Watch the next re-entry** jumps to the next satellite or large piece coming down over a dark sky, 25 s before it
breaks up, from beside its track. Live counts follow: days since the storm, how far the cascade is computed, dead
satellites, destroyed or re-entered satellites, large pieces, small fragments made, collisions, and your breakups.

The **Collision debris** row of the Constellations tab shows or hides the debris; its **HLT** tints the pieces
orange and draws each as a visible dot, to see the cloud's pattern.

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
| Great Australian Eclipse | The total solar eclipse of 13 July 2037 at greatest eclipse (21.75 S 138.31 E): the corona in a 15° field <!-- history-ok --> |
| Blood Moon from Anchorage | The total lunar eclipse of 31 January 2037, 14:10 UTC, with the reddened Moon about 29° up in the west-southwest over the city <!-- history-ok --> |
| Solar Storm | A strong geomagnetic storm seen from 420 km over Siberia |
| Beam-lit Snow over Antarctica | McMurdo in mid-April twilight at the edge of a snow shower, the camera on the line of Reflect Orbital mirrors: the flakes glitter and halo in the beams. Starts paused |
| Rainbow over Venezuela | Sunrise at the edge of a raining storm on the Falcón coast (11.02 N 68.58 W), looking at the point opposite the Sun: a full double bow, with the drops falling around you. Starts paused |
| Starmind Formation Rings | Beside one of Starmind's eight-satellite formation rings at 565 km. Starts paused |
| The Sun's Fury | Monument Valley on the storm's first night, three and a half hours after it arrives: the aurora overhead |
| Twilight Starship Launch | A Starship lift-off from Starbase Louisiana at twilight, from the ground |
| Starmind Deployment | A sunlit Starship releasing Starmind satellites, seen from 350 km. Starts paused |

A bookmark may also set the clock when you go to it: the shipped ones run at 1x, and those marked above start
paused. The shipped files live in `default_bookmarks/` next to the program.

## Pictures

| Tool | How | Result |
|---|---|---|
| Screenshot | F12, or the camera button in the bottom-left panel | `screenshots/satlight_<date>_<time>.png` at window size, without the interface (unless the setting below is on) |
| Screenshot with UI | F10 | The same, always with the interface |
| HQ photo | F8, or the sparkle-camera button | `screenshots/satlight_hq_<date>_<time>.png` |
| Star trails | `F`, or the trails button | A long exposure: stars, planets and satellites leave streaks until you switch it off |

A screenshot with the interface keeps every window you had open, but never the things that exist only because of
the shot: its own notice, the issue note bar and the Issue reports window. The picture is the frame shown right
after the key press.

**HQ photo** pauses time, renders the scene offscreen at a multiple of the window size and lets it settle for a
number of frames before saving, so the clouds and other temporally accumulated effects are fully converged. All
the detail that depends on pixel size (terrain, city lights) follows the finer pixels, so a photo shows more
detail than the window, not just more pixels. At 2× a 1600 × 900 window (a 3200 × 1800 photo) it takes a
couple of seconds.

| Setting (Display tab) | `settings.json` key | Default |
|---|---|---|
| Screenshots (F12) include the UI (*Screenshots*) | `display.screenshot_ui` | off |
| Issue report shots include the UI (*Screenshots*) | `display.report_shot_ui` | off |
| Resolution (x window) (*HQ photo*) | `display.photo_scale` | 1 (1 to 4) |
| Settle frames (*HQ photo*) | `display.photo_frames` | 48 |

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

In an HQ export, each frame's settle frames are averaged evenly, and the previous frame's result counts as one more
frame's worth, so each exported frame is the mean of about twice the settle frames. Fine cloud edges therefore do not
flicker from frame to frame; after a cut between shots the average starts afresh.

**Motion blur (subframes)** renders each exported frame as several subframes across a half-frame shutter and
averages them in linear light. While an export runs, a progress bar with the frame count, the time left and
**Stop** stays on screen.

## Reporting a problem

When something looks wrong, an **issue report** records it with everything needed to reproduce it.

- **F9** (*Save Snapshot*) adds a **shot** to the active issue, and **F7** (*New Issue Snapshot*) starts a new issue.
  A shot is the frame shown right after the key press (with or without the interface, as set above), together with
  the place, camera, time, cloud drift, every setting, recent frame times and the camera's last 12 seconds of
  movement. Nothing is paused or settled for it, so the shot shows exactly what you saw.
- After a shot, a bar at the top asks for a note on it: Enter saves, Esc closes, Shift+Enter is a new line. It never
  takes the keyboard while you are moving. A new issue opens the **Issue reports** window on it instead, for a
  title, a category and a description.
- The **Issue reports** window (the flag button, bottom right) lists the issues as cards. In an issue, a click on a
  shot's frame pins the spot that matters and a drag marks an area. **Go** returns to a shot's place and moment
  (paused); **Go + settings** also switches to its settings, which replace yours.
- **Export zip** writes `report_exports/<id>.zip` in the user data folder: the issue's folder and the end of the
  log, with your folder paths removed. That file is what you send.

When a fix comes back, the issue shows its resolution and is listed in green; set it to Closed after checking, or
to Open again. The format and the developer's side are on [Issue reports](../development/issue-reports.md).

## Settings

The **Settings** button (a gear, bottom right) opens the settings window. Its tabs are listed down the left side
under four headings, in a list that scrolls on its own. Six tabs are always there; eight more appear when **Show
advanced settings** (Display tab) is on.

| Heading | Tab | What it holds |
|---|---|---|
| GENERAL | Display | *Graphics*: preset, render scale, automatic render scale, clouds following the render scale, temporal anti-aliasing, frame limiter, beam pointing rays. *Window*: window mode, text scale, units, UI theme, window opacity. *Screenshots*. *HQ photo*. *Startup*: play the intro on startup, ask for the graphics mode on startup, replay the intro or the tutorial. *Settings*: show advanced settings, dev mode, reset to defaults |
| | Controls | *Movement* (move speed), *Bindings* (each action's key and gamepad button, with Rebind and Bind), *Mouse* (look, zoom, select), *Invert look*, *Help* (the controls overlay); see [Controls](controls.md) |
| | Sound | Volume, music, ambience and the ambience mix; see [Sound](../sound/index.md) |
| SKY | Constellations | The constellations, grouped (below); the planets; a button that opens the Missions window |
| | Photometry | *Exposure and Moon* (exposure, auto exposure, highlight roll-off, white balance, the Moon's drawn size), *Brightness*, *Sky dimming*, *Point sources*, *Bloom and glare*, *Dark sky* (Milky Way and zodiacal light), *Star trails*, *Satellite models* (part occlusion, reflections), *Bulk export* |
| RENDERING *(advanced)* | Clouds | The volumetric clouds: shape, lighting, quality |
| | Weather | Storms, lightning, rain and snow, weather evolution, fog, dust, ice fog, cloud shadows and god rays |
| | Atmosphere | Scattering, airglow, zodiacal light, aurora (with a live readout of the current space weather) |
| | Terrain | Relief detail, materials, shadows, close-up textures |
| | Night lights | City lights, roads, light sprites, night sky light |
| | Ocean | Waves, sea state, reflections, satellite glints on the water |
| | Beams | Orbital-mirror beams and their light in the clouds |
| | Performance | The GPU and CPU cost of each part of the frame, *Save snapshot*, the knockout sweep and the knockout switches; see [Profiling](../development/profiling.md) |
| ABOUT | Attributions | Credits for data, imagery, textures and sounds |

Every window and tab is built from the same few parts: sections headed in small capitals over a thin rule,
readouts as a label column and a value column, and switches, choices and buttons as small pills. Every slider's
value can be clicked and typed ([Controls](controls.md#typing-values)). Changing any slider on a rendering tab sets
the graphics preset to **Custom**. See [Graphics settings](graphics-settings.md).

### The Constellations tab

The constellations are grouped by kind: broadband constellations, stations and depots, orbital data centers,
reflectors, the geostationary belt, science, the 2020 benchmark shells, and debris. A group starts folded; click its
name to open it. Its **ON** and **HLT** set every entry in it (**MIX** when they differ). Each entry has:

| Column | Meaning |
|---|---|
| Show (ON / OFF) | Whether its satellites are in the sky |
| HLT | Highlight: draw every satellite of it at least faintly, to see its pattern |
| Actions | **VIEW** for a constellation: select its satellite highest in your sky and open its 3D view. **SEL** and **GO** for a station: select it, or fly to it. **LIST** for the geostationary belt: its satellites by name, searchable and in pages, each with SEL and GO |
| Count | How many satellites it has |

The geostationary belt holds about 400 real geostationary satellites by name at their slots (positions from the
mid-2020s, approximate for 2036). Which groups are open and the list's search are saved.

### Brightness of satellites on screen

Four sliders on the Photometry tab change how satellites are drawn, not how bright they are physically. The first
three are under *Point sources*, the last under *Bloom and glare*.

| UI label | `settings.json` key | Default | Effect |
|---|---|---|---|
| Resolved satellites | `photometry.sat_resolved` | 1 (0 to 1) | A modelled satellite a few pixels across fades from a point into its model's real surface brightness, so the point does not blaze beside a dim model |
| Satellite adaptation | `photometry.sat_adaptation` | 1 (0 to 2) | Bright satellite models filling the view, or a bright Earth seen from orbit, dim the faint points, as an eye adapts (up to 8 magnitudes) |
| Merge formations | `photometry.formation_merge` | 1 (0 to 1) | A formation too small to resolve is drawn as one point with the members' combined light |
| Glare cluster merge | `photometry.glare_cluster_merge` | 1 (0 to 1) | Lights glaring close together give up their spikes to a soft shared glow: the brightest keeps most of its star. 0 gives every light its own star |

## Where in the code

- `src/simulations/SatelliteSimUI.cpp`: the HUD (`buildUI()`, `buildRightHudPanel()`), the typed fields
  (`setSimTimeFromText()`, `setLatLonFromText()`, `setAltitudeFromText()`), the selection panel
  (`buildSelectedSatPanel()`), the info window (`buildInfoWindow()`), trace window, settings tabs
  (`buildSettingsConstellationsTab()`) and the UI kit (`uiSection()`, `uiKV()`, `uiStatTile()`, `uiButton()`,
  `uiToggleRow()`, `uiChoiceRow()`).
- `src/simulations/SatelliteSim.cpp`: the selection's text (`formatSelectedSatInfo()`), screenshots
  (`requestScreenshot()`), HQ photos (`requestPhoto()`), follow mode (`updateFollow()`), traces and bulk export.
- `src/simulations/SatelliteSimKessler.cpp`: breakups from the window (`kesslerBreakupFromUi()`) and the Sun's Fury
  settings.
- `src/simulations/SatelliteSimBookmarks.cpp`: bookmarks.
- `src/simulations/SatelliteSimReports.cpp`, `src/simulations/SatelliteSimReportsWindow.cpp`: issue reports.
- `src/simulations/SatelliteSimCinematic.cpp`, `src/simulations/Cinematic.cpp`: cinematics.
- `src/simulations/SatelliteSimTutorial.cpp`: the tutorial.
