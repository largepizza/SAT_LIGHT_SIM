# Missions

Everything in the sky that comes and goes is gathered in one place: the **Missions** window lists the next
launches, what is flying now and the Sun's Fury, and a launch object's **mission milestones** let you skip from
one event of its flight to the next. This page explains both. How launches are scheduled and how rockets fly is on
[Launches](../simulation/launches.md), [Rocket flight](../simulation/rockets.md) and
[Crew flights](../simulation/crew-flights.md); the storm and the debris cascade are on
[The Sun's Fury](../simulation/kessler.md).

## Opening the window

The rocket button on the right of the bottom-right panel (between the Issue reports flag and Bookmarks), or **M**
("Missions" in the key bindings). The window opens at the right edge of the screen and can be moved and resized.
Its top row holds three tabs, **Upcoming**, **Ongoing** and **Controls**, and the current sim time in UTC. A line
under the tabs (amber) reports the result of the last button you pressed, for example where a Go to took you or why
nothing was found; it disappears after 12 s. A docking port is named as words (*IDA-2 Harmony forward*,
*node port nadir*) here, in the follow bar and in the milestones.

The rows are refreshed twice a second, and at once after a jump in time; only the countdowns change every frame.

## Upcoming

### The next launches

**NEXT LAUNCHES** lists the next five lift-offs over the coming two days. Each card shows:

- tags: **CREWED** (a crew flight), **CARGO** (a supply flight to a station), **TWILIGHT** (lift-off with the Sun
  5 to 16 degrees below the pad's horizon), **BUILD-OUT** (a first fill of an empty plane during the
  [deployment era](#the-deployment-era));
- the vehicle, the program and the number of satellites (for a crew flight: the spacecraft, the station and the
  number of crew);
- the pad, the date and time in UTC and the Sun's elevation at the pad;
- the countdown (`T- 3d 04h`, `T- 12m 03s`, `T- 42 s`).

Two buttons act on the launch:

| Button | What it does |
|---|---|
| **Go to** | Puts you on the ground about 12 km from the pad, on land, looking at it, 45 s before lift-off, with time at 1x |
| **Watch** | Only moves the clock to 45 s before lift-off; you stay where you are |

When there is nothing to list, the card says why: launches are off (Controls), none fall in the next two weeks, or
launches have stood down at the Sun's Fury.

### Pinned launches

Under the list, whatever their order, two launches are always shown:

| Section | What it holds |
|---|---|
| NEXT TWILIGHT LAUNCH (JELLYFISH) | The next lift-off within a week with the Sun 5 to 16 degrees below the pad. The exhaust high up is then sunlit while the ground below is already dark, and spreads into a glowing bell |
| NEXT CREWED LAUNCH | The next crew flight to any station |

Both have the same Go to and Watch buttons as the list.

### The Sun's Fury card

A card pinned under the list, always visible on the Upcoming tab, follows the [Sun's Fury](../simulation/kessler.md),
the scripted solar storm of 15 July 2037 <!-- history-ok -->. Before the storm it counts down to the arrival of the
coronal mass ejection and gives the times of the flare and of the arrival. Once the storm has arrived it reads
**DAY n** and the cascade's counts: dead satellites tumbling, satellites destroyed or re-entered, collisions and how
many were catastrophic.

| Button | What it does |
|---|---|
| **Go to the arrival** | Two minutes before the storm arrives: dusk over Monument Valley, looking north |
| **The storm night** | Three and a half hours in, the aurora overhead (the same view as the bookmark *The Sun's Fury*) |
| **Controls** | Shown instead when the scenario is off: opens the Controls tab, where it can be switched on |

## Ongoing

The Ongoing tab lists what is happening now, in sections, in this order.

| Section | Rows | Buttons |
|---|---|---|
| CREW TRANSFERS | Crew and cargo flights between lift-off and docking: their phase (ascent, phasing, approach, docking in ...) | **Select**, **Go to**, **Next >** |
| CREWED STATIONS | Every station: altitude, and when the next crew flies (or that a new crew is on its way) | **Select**, **Go to** |
| SATELLITE LAUNCHES | Rockets in flight: phase (first stage, upper stage, parking orbit, deployment) | **Select**, **Go to**, **Next >** |
| SATELLITE OPERATIONS | Per program, the trains of new satellites climbing to their slots (count, progress, next arrival) and the retired satellites coming down (next re-entry) | **Select** and **Go to** the nearest one; for retirees also **Watch** |
| BUILD-OUT (DEPLOYMENT ERA) | Deployment era only. A summary line, *n of N constellations complete*, with **Show** / **Hide**, then one row per constellation: satellites in place out of the target, the share, launches a day, the date it completes (or completed), with a progress bar. The rows start hidden once every constellation is complete | **Show** / **Hide** |

- **Select** selects the object even when it is not drawn from where you are (below your horizon, or too faint):
  the selection panel then points to it from the edge of the screen.
- **Go to** flies the camera to it and rides along ([follow mode](controls.md#follow-mode-go-to)). For a rocket that
  is still on the pad, it is placed where its launch puts it.
- **Next >** skips to just before that launch's next milestone (see [below](#mission-milestones)). A stage of it that
  is selected or followed stays selected or followed.
- **Watch** on a retiree row jumps to the next retired satellite of any program coming down over a dark sky, 25 s
  before it breaks up.

A station that has been destroyed or has re-entered (after a breakup in the [Sun's Fury](#the-suns-fury-card) or
one you caused) reads *Destroyed* or *Re-entered* with the date and "no more crew flights", and its Select and
Go to are disabled. A station that has not been launched yet in the deployment era reads *Not launched yet*.

## Controls

The Controls tab holds the settings for launches and for the Sun's Fury. The Constellations tab of the settings
window keeps a button, **Open the Missions window**, that leads here.

### LAUNCHES

| UI label | `settings.json` key | Default | Effect |
|---|---|---|---|
| Launches and retirements | `launches.enabled` | on | Every constellation is kept flying by launches; the satellites they replace deorbit and re-enter |
| Launch site graphics | `launches.pad_graphics` | on | The pads on the ground: graded concrete, flame trenches, towers, landing zones, their lights at night; no city lights within about 3 km |
| Deployment era (2019-2036) | `launches.deployment_era` | on | The constellations are built up by launches up to 2036. Off: the full 2036 roster at every date |
| Satellite design life | `launches.life_scale` | x1 | x0.5, x1, x2, x4 of each program's design life (5 to 7 years). Shorter means more launches and re-entries. Changing it reshuffles the schedule |
| Exhaust plumes | `launches.plume_gain` | x1 | Off, x0.3, x1, x3: the exhaust trails and the high bells |
| Rocket light on clouds | `launches.engine_light_gain` | x1 | Off, x0.3, x1, x3: at night the engines light the clouds, haze, exhaust and ground around them |
| After the Sun's Fury | `launches.after_storm` | Stand down | Stand down: no launches from the storm on. Resume: launches return three weeks after it |

Four buttons jump to an event:

| Button | Where it takes you |
|---|---|
| Watch the next launch | 30 s before the next lift-off, beside the pad |
| Watch a twilight launch | The next lift-off within a week with the Sun 5 to 16 degrees below the pad |
| Watch a Starship catch | 90 s before the next ship comes home to a tower, 2.5 km from the pad: its descent, flip and landing burn |
| Watch a retired satellite re-enter | The next retiree coming down over a dark sky within 48 h, 25 s before it breaks up |

Under them, live counts: the next launch, launches in the next 24 h, satellites in trains climbing, and retired
satellites deorbiting. The rockets' brightness has no button; it is `launches.rocket_gain` (default 1) in
`settings.json`.

### THE SUN'S FURY

The settings of the storm and the debris cascade; see [The Sun's Fury](features.md#the-suns-fury) on the features
page for the table and what they do.

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
| Missions window, Ongoing: **Next >** | That launch's next milestone |

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
  (`missionsRefresh()`), the Sun's Fury card and its jumps (`missionsGoToFury()`).
- `src/simulations/SatelliteSimLaunches.cpp`: the schedule queries behind the rows (`launchUpcoming()`), the
  LAUNCHES settings (`buildLaunchSettingsSection()`), the milestones (`missionBatchMilestones()`,
  `missionSubjectOfIndex()`) and the skips (`missionJump()`, `missionJumpNow()`, `missionAttachAt()`).
- `src/simulations/SatelliteSimUI.cpp`: the MISSION section (`buildMissionTimeline()`) and the follow bar
  (`buildFollowHud()`).
- `src/simulations/SatelliteSimKessler.cpp`: THE SUN'S FURY settings (`buildKesslerSettingsSection()`).
