---
pages: [simulation/reflectors.md, rendering/clouds/march.md]
code: [src/simulations/SatelliteSim.cpp]
---
## Now
The beam light volume has 4 site boxes. Each lit site is scored by its beams' summed irradiance / (1 + (distance /
150 km)^2). A site keeps its box while it stays among the best 8, unless one of the best 4 has no box and outscores it
by 1.25x. In that case the weakest holder outside the best 4 gives up its box. The site under the eye therefore always
gets a box, and two sites of near-equal score do not trade one back and forth (each hand-over re-bakes the box's whole
density that frame). `beams vol` now names each slot's site (`site`).

## History
issue_20261124_185832: with 8 or fewer lit sites (the usual case), "keep while among the best 8" never gave a box back.
After a detour, four far sites held all the boxes, and the Mississippi Delta site under the eye was left with only
the old ground spot and the merged shafts. The reporter's frame had ground 13 and air 18 levels where a direct load
shows 55 / 54. It was reproduced by standing over the Midwest sites and then loading the report: 13.1 / 17.7 before
the fix, 54.9 / 53.5 after.
