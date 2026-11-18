---
pages: [simulation/wildfires.md, rendering/launch-visuals.md, rendering/atmosphere-and-sky.md]
code: [src/simulations/SatelliteSimSmokeLayer.cpp, shaders/smoke_layer.comp, src/simulations/SatelliteSimWildfire.cpp, src/simulations/SatelliteSimLaunches.cpp, shaders/sat_sky.frag]
---
## Now
Wildfire smoke has two parts. The fresh column over the fronts stays as Gaussians in the dust list. Everything older
(the trail from ~0.5-1.5 h of age, handed over smoothly, and each fire's mass-conserving pall) is a regional haze
LAYER: its mass (x 3 for aged smoke's higher extinction: secondary aerosol and grown particles) is spread (the pall a
further 60 km, `clouds_v2.wildfire_smoke_layer_spread_km`; the trail by 25 km x sqrt(age in h)) and summed into a
128 x 128 map of column optical depth, mid height and thickness on the eye's tangent plane, rebuilt each frame.
`shaders/smoke_layer.comp` integrates each pixel's ray through it in curved coordinates after the cloud composite (as
the noctilucent layer is): exact vertical integration per ~25-km step, extinction at the physical Angstrom exponent
(~1.6), light = albedo 0.9 x (the Sun through air and the smoke above, HG g 0.6 + two multiple-scattering octaves) +
a grey sky share. It forms a deep haze band toward the horizon, greys the sky, dims and reddens the Sun, its glare and
the sunset seen through it, and shades the ground's sunlight (the layer's pieces go to sat_sky.frag smokeSunTransmit
as undrawn Gaussians). `clouds_v2.wildfire_smoke_layer` (1, no UI) scales it; 0 = the old trail and pall Gaussians.

The dust list is shared out: the launches (first) leave what the weather asked for last frame (tornadoes, haboobs,
the smoke's demand, the flame band's 256 floor; at least 256, never below the plumes' 384), and the smoke may use the
list up to its end less the flames' floor. `clouds_v2.wildfire_dust_share` 0 = the old split. Harness `fires` ->
`smoke_demand`, `weather_dust_want`, `smoke_layer`; `shaders reload smoke_layer=<spv>`.

## History
issue_20261117_195951 "Smoke trails or bust" (2026-11-17): smoke popped in and out between near-identical views near
the start vantage. Since the plumes' coverage budget (2026-11-16) their room reached ~730 of the 1024 entries, and the
smoke, held to the first 512 with the flame band, got none (report shots 4 / 5: plume_room 724 / 629, smoke range
empty, 14 / 54 segments in view lost). The reporter asked for a greyer, broadly diffused layer that reddens sunsets
and hazes the horizon, citing the launch noctilucent-cloud layer as the model.
