---
pages: [rendering/atmosphere-and-sky.md, rendering/launch-visuals.md, rendering/meteors.md, using/graphics-settings.md, using/missions.md]
code: [src/simulations/SatelliteSim.h, src/simulations/SatelliteSimLaunches.cpp, src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimUI.cpp]
---
## Now
A first run (and the fresh build) starts from the author's tuned look: the compiled-in defaults of the Milky Way,
the launch exhaust and the meteors are the numbers that used to live only in a settings.json.

**Milky Way** (Sky tab, its Milky Way and "zoomed in" star clouds folds): gain (ground) 0.47, gain (space) 0.80,
night vision 0.50, star clouds 1.0, zoom detail 0.99 (contrast 1.25 unchanged). The zoomed star clouds: "Zoom
from (px/texel)" 8 — the slider now runs 0.1-8 and the runtime clamp is 0.05-8, so the point lattice is in at
nearly every field of view — "Zoom full" 1.03, star fill 0.89, brightness spread 0.56, faint sky from 0, faint sky
full 0.002, nebula exclusion 0.93 (star size 0.9 px and spacing 4 px unchanged).

**Rockets and exhaust** (Settings > Launches): plume opacity 0.3, plume brightness 1.06, plume width 0.88, plume
billows 0.99, plume dispersal 3.94, jellyfish streak spread 0.91, streak length 55 s, streak diffusion 0.59,
nucleus brightness 1.04, hot-staging cloud 1.90, rocket light on clouds 0.905 (plume growth, ground clouds and
flame brightness stay at 1). "Reset exhaust to defaults" restores exactly these, and its tooltip reads "Every
slider above back to its default".

**Meteors** (Sky tab): fireball rate 0.1 (a tenth of the showers' fireballs), train duration 0.07 (7% of the
decay time; 1 is the physical one).

Every literal is a float32-exact copy of the settings.json number, so a saved file round-trips unchanged and an
existing settings.json keeps whatever it holds — only a first run or a reset sees the new look.
`tools/settings_defaults_diff.py` reports no launches, meteors or milky_way key differing from the defaults.

## History
Promoted 2026-10-30 from the author's tuned user_data/settings.json. The previous defaults were the
2026-10-21 / 2026-10-28 constants: Milky Way 1.0 / 2.5 / 0.5 / 0.7 / 0.6 and star clouds 0.6 / 0.3 / 0.9 / 0.65 /
0.003 / 0.02 / 1.0 (the star-cloud zoom started only as the pixel passed 0.6 texel, i.e. a narrow field), every
exhaust slider 1.0 (1 = the 2026-10-21 look), jellyfish streaks 30 s, fireball rate 1 and train duration 0.3.
