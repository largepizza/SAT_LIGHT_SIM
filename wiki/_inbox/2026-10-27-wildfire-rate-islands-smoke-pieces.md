---
pages: ["new: simulation/wildfires.md", rendering/atmosphere-and-sky.md]
code: [src/simulations/Wildfire.cpp, src/simulations/Biome.cpp, src/simulations/SatelliteSimWildfire.cpp, shaders/cloud_march.comp]
---
## Now
- **How often fires start.** The birth rate per 1-degree cell and day is kBirthRate (0.045) x the biome's ignition
  propensity x its fuel x dryness^2 x cos(lat) x "Wildfires (x)". Propensity: savanna 1, shrubland 0.5, grassland 0.45,
  tropical forest 0.4, temperate forest 0.2, boreal 0.18, tundra 0.03 (savanna carries most of the world's burnt area;
  boreal and humid forests burn rarely, in larger fires). Burning worldwide (harness `fires world=1`): 15 Jan 43, 15 Apr 63,
  15 Jul 27, 12 Aug 137, 15 Oct 66 (12 Aug was ~340 before), mostly savanna.
- **The fire season narrows poleward.** BiomeMap::seasonalDryness's extratropical part is smoothstep(c0, 1, cos) of the
  summer cosine, c0 rising from -0.45 at 30 deg to 0.35 by 62 deg: May-October at mid-latitudes, June-August in the boreal
  belt, nothing in the winter half (it was the cosine itself: 0.29 at 53 N in late November).
- **A fire needs a land mass.** An ignition is kept only if at least 6 of 8 points ~30 km round it are burnable land:
  no fires on small islands (the Aleutians) or the last texel of a coast.
- **Smoke only from burning ground.** A fire's smoke production is x the share of its active front on burnable ground
  (a front that ran into the sea or onto rock made full smoke from a stand-in source).
- **Smoke trail pieces.** Each streak segment between two trajectory knots (the nested time grid doubles its spacing
  down the trail, so a segment was 15-30x as long as the smoke is wide) is cut into up to 12 round pieces along a
  Catmull-Rom curve through the neighbouring knots, spaced at most ~2.2x the smoke's width and never closer than 6 px on
  screen, sharing its mass; the smoke budget is 640 Gaussians (was 384), at least 96 left for the flames. Every smoke
  Gaussian is at least ~1.5 full-res pixels thick as seen (the pall 3), its column kept (anti-aliasing of thin, edge-on
  sheets in the half-res composite). Env `SATLIGHTSIM_SMOKE_PIECES=0` restores single segments (A/B).
- **Flames close up.** A flame Gaussian's width is 0.35 x its height + 0.05 x min(spacing, 6 x height), its depth across
  the front at most half its height + 1 m.
- cloud_march.comp: a fire glow's light on smoke fades over the outer part of its reach (was cut at the reach); smoke is
  not thinned when the eye is inside it; smoke billows fade in over tau 0.01-0.05.

## History
Issues 20261027_000126 (a fire on an Aleutian island in November, its smoke over the sea), _000414 (smoke as huge
stretched ovals from 165 km), _002954 (a flame Gaussian ~50 m wide x 2 m tall right under the eye at night: its width took
0.05 x a spacing set by the range to the fire's CENTRE, ~1 km; seen from 150 m up an orange oval filling half the view),
_003452 (fires too common; a pall edge along the limb drawn as a staircase). Tried and dropped: an air-split distance at
the smoke's front instead of its light-weighted distance (no measurable effect on 001835 shot 3), a fire-glow core radius
from the arc's extent / no ground term (the oval was the flame, not the glow).
