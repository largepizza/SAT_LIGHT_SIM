---
pages: ["new: using/missions.md", simulation/launches.md, using/controls.md, using/settings.md, modding/index.md]
code: [src/simulations/SatelliteSimMissions.cpp, src/simulations/SatelliteSimLaunches.cpp, src/simulations/Launches.cpp, data/launches.json, tools/make_rocket_models.py, tools/make_icons.py]
---
## Now
A Missions window (the rocket button on the right HUD panel, next to Bookmarks; key M) holds everything dynamic in the
sky. **Upcoming**: the next five launches, each with vehicle, program and payload, pad, date (UTC), the Sun's
elevation at the pad and a countdown, tagged CREWED or TWILIGHT; "Go to" puts the observer on the ground about 12 km
from the pad (on land), looking at it, 45 s before lift-off at 1x; "Watch" only moves the clock. Below them, whatever
their order: the next twilight launch (the "jellyfish": the Sun 5-16 degrees below the pad, so the exhaust high up is
sunlit over a dark ground) and the next crewed launch; and last, always, THE SUN'S FURY: a dark red card counting
down to the coronal mass ejection's arrival (2037-07-15 03:00 UTC; the flare 17.5 h earlier), then "DAY n" with the
cascade's counts, with Go to the arrival (2 min before, dusk over Monument Valley) and the storm night. **Ongoing**:
crew transfers in flight (ascent, then "docking in ..."), the crewed stations (altitude, next crew flight; Select,
Go to), satellite launches in flight (phase: first stage, upper stage, parking orbit, deployment), and per program
the trains raising (count, progress, next arrival) and the retirees deorbiting (next re-entry). **Controls**: the
LAUNCHES and THE SUN'S FURY settings, which moved here from Settings > Constellations (that tab keeps a button that
opens the window).
Crew rotations fly: every station in the roster gets a new crew twice a year (ISS: Crew Dragon on Falcon 9 from
Kennedy 39A / SLC-40; Tiangong: Shenzhou on Long March 2F from Jiuquan; Haven-1/2, Axiom, Starlab, Orbital Reef: Crew
Dragon; the Russian station: Soyuz from Baikonur; Bharatiya: Gaganyaan on LVM3), about 18 launches a year, launched
when the station's orbital plane passes over the pad. The rocket and its upper stage fly and are drawn (three new
vehicle models); the capsule's rendezvous with the station is not drawn: the window reports the docking time
(Shenzhou and Soyuz ~3-7 h, Dragon and Gaganyaan ~1-1.5 days). Crew flights stand down after the Sun's Fury like every
launch. Modding: a program in `launches.json` with a `"crew": {"spacecraft": ..., "people": ...}` block matches
station constellations (a single satellite) and flies every `life_years` (a cadence, not scaled by "Satellite design
life"); `raise_days` is lift-off to docking.
Launch Go to now puts the observer on the ground (it used to keep the eye's height, so a jump from orbit stayed in
orbit); the same fix applies to "Watch a retired satellite re-enter".

## History
Until 2026-10-19 the launch and Sun's Fury controls lived in Settings > Constellations with "Watch the next launch /
a twilight launch" buttons and a one-line "Next launch" readout; there were no crewed flights.
