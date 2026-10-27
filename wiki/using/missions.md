# Missions

Everything in the sky that comes and goes is gathered in one place: the **Missions** window lists the next
launches, what is flying now and the Sun's Fury, and tracks the debris cascade; a launch object's **mission
milestones** let you skip from one event of its flight to the next; and Settings → **Launches** holds the settings
for launches and the Sun's Fury. This page explains all three. How launches are scheduled and how rockets fly is on
[Launches](../simulation/launches.md), [Rocket flight](../simulation/rockets.md) and
[Crew flights](../simulation/crew-flights.md); the storm and the debris cascade are on
[The Sun's Fury](../simulation/kessler.md). Storms, hurricanes and wildfires have a window of their own, described
on [Weather](weather.md).

## Opening the window

The rocket button, the first of the menu buttons on the bottom-right panel, or **M** ("Missions" in the key
bindings). The window opens at the right edge of the screen and can be moved and resized. Its top row holds three
tabs, **Upcoming**, **Ongoing** and **Kessler**, and the current sim time in UTC. A line under the tabs (amber)
reports the result of the last button you pressed, for example what a Watch has put in view or why nothing was found;
it disappears after 12 s. A docking port is named as words (*IDA-2 Harmony forward*, *node port nadir*) here, in the
follow bar and in the milestones.

The rows are refreshed twice a second, and at once after a jump in time; only the countdowns change every frame.

## Watch: one button for every row

Every row of the window has a single button, **Watch**. It lands on the event at a moment when there is something to
see, sets time to 1x and puts the subject in view; the line under the tabs says what you are looking at.

| Watch on | Where it puts you |
|---|---|
| A launch (Upcoming) | 5 s before lift-off, on the ground 5 km from the pad (on land), the camera on the rocket with a 40 degree view. The rocket is selected but the camera is not locked to it: you watch it climb away |
| A twilight launch | The same, 5 s before lift-off, but 12 km from the pad with a 60 degree view, so the exhaust can spread over the sky as it climbs into sunlight |
| The Sun's Fury | The storm's first night over Monument Valley, three and a half hours after it arrives, the aurora overhead |
| A crew flight or a rocket in flight (Ongoing) | Flies out to it and rides along ([follow mode](controls.md#follow-mode-go-to)) |
| A crewed station | Selects it and flies alongside |
| A train of new satellites | Rides along with the nearest of them |
| Retired satellites coming down | The next retired satellite of any program burning up over a dark sky, seen from the ground beside its track (on the side with the clearer sky), in mid-glow about 35 s before it breaks up. The fireball is selected and tracked |
| A breakup (Kessler tab) | 3 s after it, 80 km beside the orbit and 25 km below it, so the debris is seen against space; a piece of it is selected and tracked |

## Upcoming

### The next launches

**NEXT LAUNCHES** lists the next five lift-offs over the coming two days. Each card shows:

- tags: **CREWED** (a crew flight), **CARGO** (a supply flight to a station), **TWILIGHT** (lift-off with the Sun
  5 to 10 degrees below the pad's horizon), **BUILD-OUT** (a first fill of an empty plane during the
  [deployment era](#the-deployment-era));
- the vehicle, the program and the number of satellites (for a crew flight: the spacecraft, the station and the
  number of crew);
- the pad, the date and time in UTC and the Sun's elevation at the pad;
- the countdown (`T- 3d 04h`, `T- 12m 03s`, `T- 42 s`) and **Watch**.

When there is nothing to list, the card says why: launches are off (Settings → Launches), none fall in the next two
weeks, or launches have stood down at the Sun's Fury.

### Pinned launches

Under the list, whatever their order, two launches are always shown:

| Section | What it holds |
|---|---|
| NEXT TWILIGHT LAUNCH (JELLYFISH) | The next lift-off within a week with the Sun 5 to 10 degrees below the pad. The exhaust high up is then sunlit while the ground below is already dark, and spreads into a glowing bell. With the Sun lower than that, the bell stays in the Earth's shadow |
| NEXT CREWED LAUNCH | The next crew flight to any station |

### The Sun's Fury card

A card pinned under the list, always visible on the Upcoming tab, follows the [Sun's Fury](../simulation/kessler.md),
the scripted solar storm of 15 July 2037 <!-- history-ok -->. Before the storm it counts down to the arrival of the
coronal mass ejection and gives the times of the flare and of the arrival. Once the storm has arrived it reads
**DAY n** and the cascade's counts: dead satellites tumbling, satellites destroyed or re-entered, collisions and how
many were catastrophic. On a short window the card keeps only its title, countdown and button.

Its **Watch** goes to the storm night (the same view as the bookmark *The Sun's Fury*). When the scenario is off, a
**Settings** button opens Settings → Launches, where it can be switched on.

## Ongoing

The Ongoing tab lists what is happening now, in sections, in this order. Each row has a **Watch** (see
[above](#watch-one-button-for-every-row)).

| Section | Rows |
|---|---|
| CREW TRANSFERS | Crew and cargo flights between lift-off and docking: their phase (ascent, phasing, approach, docking in ...) |
| CREWED STATIONS | Every station: altitude, and when the next crew flies (or that a new crew is on its way) |
| SATELLITE LAUNCHES | Rockets in flight: phase (first stage, upper stage, parking orbit, deployment) |
| SATELLITE OPERATIONS | Per program, the trains of new satellites climbing to their slots (count, progress, next arrival) and the retired satellites coming down (next re-entry) |
| BUILD-OUT (DEPLOYMENT ERA) | Deployment era only, last. A summary line, *n of N constellations complete*, with **Show** / **Hide**, then one row per constellation: satellites in place out of the target, the share, launches a day, the date it completes (or completed), with a progress bar. The rows start hidden once every constellation is complete |

A station that has been destroyed or has re-entered (after a breakup in the [Sun's Fury](#the-suns-fury-card) or
one you caused) reads *Destroyed* or *Re-entered* with the date and "no more crew flights", and its Watch is
disabled. A station that has not been launched yet in the deployment era reads *Not launched yet*.

To step through a flight's events, use the [mission milestones](#mission-milestones) of the object you are riding
along with.

## Kessler: the debris tracker

The third tab follows the debris cascade of the Sun's Fury.

- **THE SUN'S FURY**: before the storm, its countdown and three tiles (working satellites, the arrival time, the
  current geomagnetic Kp index); after it, **DAY n**, and how far the cascade has been computed.
- **Counts**, as large tiles: collisions, catastrophic collisions, pieces of a metre and more, fragments of 10 cm and
  more made, dead satellites tumbling, satellites destroyed, satellites re-entered, and satellites still working.
- **DEBRIS SHELLS**: a small globe seen from the equator, with the orbital shells drawn round it like a polar plot:
  distance from the Earth is altitude (exaggerated 1.6 times), the angle is latitude. Blue marks where the working
  satellites fly; the heat colours show the chance a year that a 10 m² satellite there is hit by a tracked object (a
  dead satellite or a piece of a metre or more). Numbered markers show the eight latest significant breakups, your
  position is a cyan dot, and you can drag the globe to turn it. A line names the worst shell.
- **COLLISIONS PER DAY SINCE THE CME**: a plot of all collisions (orange) and the catastrophic ones (red), with the
  peak. Over a long time the bins widen to several days.
- **LATEST BREAKUPS**: the same numbered breakups listed with their kind (yours are marked), the objects involved,
  when and where, the impact speed for a collision, and **Watch**.

With the scenario off, the tab says so and offers **Open the settings**.

## Settings: the Launches tab

Settings → **Launches** (under SKY, after Constellations) holds every setting for launches and the Sun's Fury, in four
sections, graphics first. A button at its top opens the Missions window, and the Constellations tab keeps a
**Launches** button that leads here.

### GRAPHICS: ROCKETS AND EXHAUST

| UI label | `settings.json` key | Default | Effect |
|---|---|---|---|
| Launch site graphics | `launches.pad_graphics` | on | The pads on the ground: graded concrete, flame trenches, towers, landing zones, their lights at night; no city lights within about 3 km |
| Plume opacity (x) | `launches.plume_gain` | 1 (0 to 5) | How thick the exhaust trails and the high bells are |
| Plume brightness (x) | `launches.plume_brightness` | 1 (0 to 5) | How much light the exhaust scatters |
| Plume width (x) | `launches.plume_width` | 1 (0.25 to 4) | The width of the exhaust; the same exhaust, so a wider plume is thinner |
| Plume billows (x) | `launches.plume_billows` | 1 (0 to 3) | The contrast of the exhaust's billows |
| Plume growth (x) | `launches.plume_growth` | 1 (0 to 5) | How fast the exhaust spreads as it mixes with the air. Changing it rebuilds the plumes in view (a short pause) |
| Ground clouds (x) | `launches.ground_cloud_gain` | 1 (0 to 5) | The steam cloud at lift-off and the dust or spray at a landing |
| Flame brightness (x) | `launches.rocket_gain` | 1 (0 to 10) | The rockets' flames |
| Rocket light on clouds (x) | `launches.engine_light_gain` | 1 (0 to 10) | At night the engines light the clouds, haze, exhaust and ground around them |

Every slider at 1 is the tuned look; **Reset exhaust to defaults** puts them all back.

### GRAPHICS: DEBRIS AND RE-ENTRIES

**Dust cloud brightness**, **Fireball material colours** and **Small re-entry brightness**: see
[The Sun's Fury](features.md#the-suns-fury) on the features page.

### LAUNCHES

| UI label | `settings.json` key | Default | Effect |
|---|---|---|---|
| Launches and retirements | `launches.enabled` | on | Every constellation is kept flying by launches; the satellites they replace deorbit and re-enter |
| Deployment era (2019-2036) | `launches.deployment_era` | on | The constellations are built up by launches up to 2036. Off: the full 2036 roster at every date |
| Satellite design life | `launches.life_scale` | x1 | x0.5, x1, x2, x4 of each program's design life (5 to 7 years). Shorter means more launches and re-entries. Changing it reshuffles the schedule |
| After the Sun's Fury | `launches.after_storm` | Stand down | Stand down: no launches from the storm on. Resume: launches return three weeks after it |

Four buttons follow the same Watch rule as the Missions window:

| Button | Where it takes you |
|---|---|
| Watch the next launch | The next lift-off, 5 s before, from beside the pad |
| Watch a twilight launch | The next lift-off within a week with the Sun 5 to 10 degrees below the pad, 5 s before, from 12 km |
| Watch a Starship catch | The next ship coming home to its tower, in its flip and landing burn 25 s before the catch, 2.5 km from the pad; the ship is selected and tracked |
| Watch a retired satellite re-enter | The next retiree burning up over a dark sky within 48 h, in mid-glow, from beside its track |

Under them, live counts: the next launch, launches in the next 24 h, satellites in trains climbing, and retired
satellites deorbiting.

### THE SUN'S FURY

The scenario, its collision settings, **Watch the next re-entry**, **Open the Kessler tracker** and **Undo my
breakups**; see [The Sun's Fury](features.md#the-suns-fury) on the features page.

### The deployment era

With **Deployment era** on, each constellation is filled by launches from its program's start date (Starlink from
2019) to its size in 2036. Before that a constellation has only the satellites launched so far: the others are not
drawn, cannot be selected or broken up, and take no part in the debris cascade. Stations appear on their launch
dates and are built module by module, launch sites come online on their own dates, and the pads show the towers they
had at the time. The 2020 benchmark shells are left out of this sky; **In the deployment era** under BENCHMARK SHELLS
(2020) in the Constellations tab (`launches.era_benchmark_shells`, default off) brings them back.

## Mission milestones

Every launch object carries a timeline of the events of its flight, and you can jump between them. A **launch
object** is anything a launch made: a rocket on its pad or in flight, its booster, a satellite in a train climbing to
its slot, a retired satellite deorbiting, and a satellite in operations (through the launch that put it there).

The milestones are, depending on the object:

- **the rocket**: lift-off, max-Q, MECO and stage separation, fairing separation, SECO (the parking orbit),
  deployment begins and last satellite released, the upper stage's deorbit burn and its re-entry; for Starship's
  ship the entry interface, the belly-flop descent, the flip and landing burn, and the catch by the tower (or a
  splashdown); for a crew or cargo flight the phasing burns, the approach and docking, and later undocking, the
  deorbit burn, entry and splashdown or landing;
- **the booster**: its burns after separation (boostback, entry, landing) and its catch, landing or arrival on a
  droneship;
- **a train member**: its release, the start of raising and its arrival in its slot;
- **a retiree**: the start of its deorbit and its re-entry;
- **a satellite in operations**: the launch that put it in its slot, and when its replacement arrives.

### Skipping

A skip moves the clock to a little before the milestone (10 s, less where milestones crowd together), sets time to
1x and keeps a paused clock paused. The selection and Go to stay on the same object at the new time: the rocket, its
booster, a train member, a retiree. A satellite still in its train before the jump is followed into its slot after
it, and the reverse. While following, the camera keeps its offset and view relative to the object across the skip.

| Control | Action |
|---|---|
| `]` | Next mission milestone (key binding "Next Mission Milestone") |
| `[` | Previous mission milestone ("Previous Mission Milestone") |
| Follow bar: `<` and **Next: ... >** | While following a launch object, the previous and next milestone; the button names the next one |
| Info window, MISSION section | **< Previous**, **Next: ... >**, and a **Skip** (ahead) or **Back** button on every milestone row |

The keys act on the object you are following, or else on the selection. With neither, a notice says to select a
rocket, a train or a satellite first. Both keys can be rebound in Settings → Controls.

### The MISSION section in the Info window

For a launch object, the [satellite window](features.md#the-satellite-window-and-the-3d-view) has a **MISSION**
section right after STATUS, open by default:

- the mission's name, on its own line, and the object (for example the booster, or a member of the train);
- facts: what it is doing now, the climb's progress and arrival, its re-entry; for a satellite in operations, its
  time in its slot, the share of its design life used and when its replacement arrives (or that none is scheduled,
  because launches stand down at the Sun's Fury);
- a progress bar through the milestone list, with the count of milestones passed: each milestone is an equal step,
  and between two milestones the bar moves on in proportion to the time, so the bar and the count agree;
- **< Previous** and **Next: ... >**;
- the milestones, each with its time as T+ from lift-off (for a satellite in operations: "in ..." or "... ago") and
  in UTC. The last one passed is highlighted and marked `>`.

## Where in the code

- `src/simulations/SatelliteSimMissions.cpp`: the window (`buildMissionsWindow()`), the cached rows
  (`missionsRefresh()`), the Sun's Fury card and its Watch (`missionsGoToFury()`), the Ongoing Watches
  (`missionsOngoingAct()`).
- `src/simulations/SatelliteSimKesslerTracker.cpp`: the Kessler tab (`buildKesslerTrackerTab()`), its globe and the
  breakups' Watch (`kesslerWatchEvent()`).
- `src/simulations/SatelliteSimLaunches.cpp`: the schedule queries behind the rows (`launchUpcoming()`), the launch
  Watch (`launchWatchKey()`), the retiree Watch (`launchGoToNextReentry()`), the catch (`launchWatchCatch()`), the
  Launches tab's sections (`buildLaunchGraphicsSection()`, `buildLaunchSettingsSection()`), the milestones
  (`missionBatchMilestones()`, `missionSubjectOfIndex()`) and the skips (`missionJump()`, `missionJumpNow()`,
  `missionAttachAt()`).
- `src/simulations/SatelliteSimUI.cpp`: the Launches tab (`buildSettingsLaunchesTab()`), the MISSION section
  (`buildMissionTimeline()`) and the follow bar (`buildFollowHud()`).
- `src/simulations/SatelliteSimKessler.cpp`: THE SUN'S FURY settings and its graphics (`buildKesslerSettingsSection()`,
  `buildKesslerGraphicsSection()`).
