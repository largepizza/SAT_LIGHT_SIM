---
pages: [using/sky-events.md, simulation/meteors.md, using/features.md]
code: [src/simulations/SatelliteSimSkyEvents.cpp, src/simulations/SatelliteSimSpaceWeather.cpp, src/simulations/Meteors.cpp]
---
## Now
The Sky events window has two tabs. Events: ONE-TIME EVENTS on top at any date (the Great Comet, its meteor storm, the Sun's
Fury, the Betelgeuse supernova), each in its own colour; ONGOING (events under way, each with Start / End buttons that jump the
clock to its start / just before its end, beside the one Watch); NEXT OF EACH KIND (folded); UPCOMING. "Details" adds a second
line to each row. Space weather: both hemispheres' auroral oval as polar plots (the renderer's own oval), Kp, storm scale,
oval edge, solar cycle, drivers, the substorm, "Watch the aurora" (the best dark place under the oval) and the next storms.
The CME kind lists every Earth-directed storm of G3+ besides the Sun's Fury.

The Great Comet of 2037 sheds a meteor storm: peak 2037-03-17 03:30 UTC (the closest approach), ZHR 8400 (a spike of 8000,
half-width ~0.4 d, over a 400 residual), the residual decaying with an e-fold of 75 d (265 in mid April, ~50 in August, under
10 on 2037-12-22). The radiant follows the comet's orbit geometry (RA 322, Dec -17.5 at the peak, 38 deg from the Sun, entry
~53 km/s), fireball-rich.

## History
The Sun's Fury moved from Missions to Sky events (round 1). All numbers for the storm are our assumptions (no real comet).
`kSporadic` moved to row 13; its generator seed is pinned so no sporadic meteor changed.
