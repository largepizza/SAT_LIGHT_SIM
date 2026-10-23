---
pages: [new: simulation/wildfires.md, simulation/weather.md]
code: [src/simulations/Biome.cpp, src/simulations/Wildfire.cpp, src/simulations/SatelliteSimWildfire.cpp, shaders/include/smoke.glsl, shaders/cloud_march.comp, shaders/sat_sky.frag]
---
## Now
A land-cover map (`wx::BiomeMap`, 2048 x 1024, built at load from the day map's colour, latitude, elevation and the
ocean mask) classifies water, ice, tundra, three forests, grassland, savanna, shrubland and desert, with a fuel and a dust
value per texel. Wildfires are hashed per 1-degree cell and day from fuel x the season's dryness, each with its whole
life fixed at birth (ignition, life, size, wind), so time warp and reverse show the same fires (~85-285 burning worldwide
depending on the season). Each fire near the eye draws a smoke column that rises, bends downwind and flattens at its
injection height, plus a mass-conserving pall of the days' smoke; many fires' palls overlap into regional haze. Smoke is
spectral (blue extinguished more than red): the Sun through it and the land under it turn yellow-orange, thick cores
orange-brown. The front shows as flickering orange points and, at night, lights its smoke, cloud and ground. Settings:
"Wildfires (x)" `clouds_v2.wildfires` 1, "Wildfire smoke" `wildfire_smoke` 1, "Wildfire light" `wildfire_light` 1
(Weather window > Controls). Harness `biome`, `fires`, `fires go`.

## History
First cut set the pall's optical depth directly (0.1-0.8 from above): one fire whitewashed 200 km. It now follows the
smoke the plume has emitted (~0.01-0.05). Same day: the old rain keys (rain_wind_gain, rain_storm_wind_mps, rain_gusts,
snow_wind) carry their ratio to the default onto the new keys when a settings file has only the old ones.
