---
pages: [simulation/reflectors.md, using/controls.md, development/harness.md]
code: [src/simulations/SatelliteSimReflectorSites.cpp, shaders/sat_orbit.comp, CMakeLists.txt]
---
## Now
The Missions window has a fourth tab, **Reflectors**: every site the Reflect Orbital mirrors are beaming sunlight at
right now, anywhere on Earth, busiest first. A row shows the site's kind (SOLAR / FARMLAND / POLAR TOWN), its name,
the number of sunlit mirrors aimed at it, the compass direction and elevation the beams arrive from (their mean, seen
from the site) and the Sun's elevation there; the header says how many of the live mirrors are beaming and at how
many sites. Each row has one Watch: time at 1x, the observer on the ground 15 km from the site, side-on to its mean
beam, looking at the air 1.5 km above it.

The counts come from a census on the GPU: `sat_orbit.comp` compiled a second time with `-DSAT_CENSUS`, which runs
the mirrors' own target choice (the lock-window winner) for every live mirror, not only those above the observer's
horizon, and counts the sunlit ones per site. It runs only while the tab is open (or the harness asks) and costs
nothing measurable (~5000 mirrors x 201 targets). It agrees exactly with the beams drawn: 126 mirrors at the
Mississippi Delta site at the opening's moment, 163 at Cerro Dominador. Harness: `missions reflectors [row=<i>]`,
`missions tab reflectors`.

## History
Before 2026-11-24 the app only knew the beams of satellites above the observer's horizon (`beams` in the harness), so
finding a lit site meant searching the night side by hand.
