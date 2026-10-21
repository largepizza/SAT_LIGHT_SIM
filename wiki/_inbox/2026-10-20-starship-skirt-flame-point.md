---
pages: [simulation/orbits.md, modding/satellite-models.md]
code: [tools/make_rocket_models.py, src/simulations/SatelliteSimLaunches.cpp]
---
## Now
Starship's ship model has an open aft skirt (3.3 m, black inside) running down to the track point round its engines;
the RVac exits sit 0.15 m inside the skirt's bottom, the three sea-level bells are recessed (exit 0.8 m up), and the
aft heat shield is 3.3 m up, above every bell's throat. Rule for every rocket model: no surface cuts a bell (each
throat, exit + 0.68 x the bell's length, is at or below the cap or heat shield over it). A burning stage's point
(bloom, glare) sits 0.08 exit diameters behind its exit plane, on the engine cluster; a coasting stage's sunlit point
sits at the middle of the stage.

## History
Issue 20261020_181510: the ship's barrel ended in a cap at the track point with its bells half above it, so the
glowing bell interiors read as crescents; and the flare sat on the flame's core Gaussian, ~33 m behind the exits in
vacuum (~300 px off the engines seen from behind and aside at 66 m).
