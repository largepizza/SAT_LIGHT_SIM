---
pages: [rendering/clouds/weather.md, rendering/clouds/field.md]
code: [src/simulations/SatelliteSimHaboob.cpp, shaders/include/clouds_v2.glsl, src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSimWeatherFeatured.cpp]
---
## Now
A haboob's dust wall is 0.5-1.8 km tall head by head (the far-field Gaussians 0.7-1.9 km), as real walls are (~1-2 km,
rarely ~3 km) under a dry-desert cumulonimbus base 2.5-4 km up. Its gust front leads a squall line / MCS by 8 km + 22 km
x the storm's age ramp (~30 km once mature: in the subsidence ahead of the line's shelf band) and a multicell or
supercell by 4 + 11 km. The clear air ahead of the front is a base lift: the low cloud is removed only below ~1.35 x the
tallest head (~3 km AGL), from 8 km behind the front (over its pall) to 12 km ahead, with lumpy edges (1200 m x the coarse
lobes); the storm's cumulus above stays whole. The cloud-density clipmap starts a new epoch whenever the dust field changes
(a wall enters or leaves, `clouds_v2.haboobs`, `haboob_field`). The harness's aloft haboob view stands 2 km up.

## History
Issue 20261026_204749 (Namibia, 18.1 S 17.3 E): walls were 0.9-2.9 km (heads to 3.4 km) and the clear air cut all low
cloud below 1.35 x the tallest head (~5 km AGL) from 2.5 km behind to 12 km ahead of a front only 5-19 km ahead of the
towers: a 5-km tunnel through the squall line's own cumulus ("giant chunk missing", "cavities"). The clipmap kept cells
baked empty under the clear air after the march swapped to its dust-free specialisation (rectangles of missing cloud,
never expiring with time paused) — the likely "lighting snaps near haboobs" of issue 20261026_191842. The same report's
"eroded top layer" is the anvil shields; its pop-in after a turn did not change with the Sun transmittance volume, the
clipmap, S2 light or the faint-sample skip switched off.
