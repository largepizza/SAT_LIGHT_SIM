---
pages: [development/ui.md, using/settings.md]
code: [src/simulations/SatelliteSim.cpp (introBenchDecision), docs/subsystems/ui.md "UC1 benchmark"]
---
## Now
The first run's graphics benchmark (the mean GPU frame time over the opening's caption period) steps the preset
down one tier (Medium -> Low -> Planetarium -> Potato) only above 33 ms, i.e. under about 30 FPS. Under 4.8 ms it
steps up one tier (Potato -> Planetarium, Low -> Medium -> High). High, Ultra and Custom are never moved, and the
"graphics set to X" notice shows either way.

## History
Until 2026-11-18 the step-down line was 18 ms (1.5x a 12 ms target). Medium measured 17.3 ms at 1600x900 on the
development machine, so machines a little slower were moved to Low; the user's aim for 1.2 is "put most users in
Medium", and they chose 33 ms.
