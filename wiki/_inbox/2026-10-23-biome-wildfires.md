---
pages: [new: simulation/wildfires.md, simulation/weather.md]
code: [src/simulations/Biome.cpp, src/simulations/Wildfire.cpp, src/simulations/SatelliteSimWildfire.cpp, src/simulations/SatelliteSimHarness.cpp, shaders/include/smoke.glsl, shaders/cloud_march.comp, shaders/sat_sky.frag]
---
## Now
A land-cover map (`wx::BiomeMap`, 2048 x 1024, built at load from the day map's colour, latitude, elevation and the
ocean mask) classifies water, ice, tundra, three forests, grassland, savanna, shrubland and desert, with a fuel and a dust
value per texel. Wildfires are hashed per 1-degree cell and day from fuel x the season's dryness, each with its whole
life fixed at birth (ignition, life, size, the direction it spreads), so time warp and reverse show the same fires
(~85-285 burning worldwide depending on the season).

A coherent wind field (`wx::windAt`: trades, westerlies and polar easterlies + the curl of travelling waves at synoptic,
mesoscale and ~30-km scales; slower and backed near the ground, turning with height) blows every fire's smoke, so
neighbouring plumes agree. A fire's smoke is a streak: parcels rise from just downwind of the head to the injection
height and are carried by the wind of their height at the moment they travelled, so plumes bend over, wiggle, curve as
the wind changes over hours and fan out where a lower layer goes another way; each stretch carries the smoke the fire
made when it was emitted. Older smoke is a flat, mass-conserving pall per fire; a fire season's palls overlap into
regional haze. Smoke is grey (fresh, sooty smoke darker; aged paler), lit by the Sun's colour where it is and shadowed
by the Earth per pixel; seen through, it is spectral: the Sun behind thick smoke and the land under it turn yellow to
red.

The burning front is a field of flickering point sprites like the city lights: the burnt ellipse with fingers, broken
into runs of flame and gaps (the back burning out as the fire ages), with embers behind it and spot fires ahead. The
points stay ~5 px apart on screen at any distance (levels cross-faded, the light kept), from a chain of flames a few km
off to a thin orange arc from orbit; their brightness comes from the flames' luminance and size, compressed to the city
sprites' range, flickering less as a point stands for more front, dimmer against a sunlit ground. Within ~40 km the
flames are also drawn as glowing tongues along the front (by day too). At night the nearest fires light their smoke, the
cloud and the ground, fading out by ~300 km. Settings: "Wildfires (x)" `clouds_v2.wildfires` 1, "Wildfire smoke"
`wildfire_smoke` 1, "Wildfire light" `wildfire_light` 1 (Weather window > Controls). Harness `biome`, `fires`,
`fires go` (side / under / orbit views), `fires look [k] [what=head|plume]`. Cost at Medium (1920x1009, RTX 3070 Ti): a
big fire a few km off at night about +0.6 ms GPU and +0.4 ms CPU; from orbit over the August fire season about +0.8-1.1 ms
GPU and +0.35 ms CPU.

## History
First cut set the pall's optical depth directly (0.1-0.8 from above): one fire whitewashed 200 km. It now follows the
smoke the plume has emitted (~0.01-0.05). Same day: the old rain keys (rain_wind_gain, rain_storm_wind_mps, rain_gusts,
snow_wind) carry their ratio to the default onto the new keys when a settings file has only the old ones.

2026-10-24, from the user's issue 20261023_225322 ("needs tremendous work"): the front's point sprites had never been
drawn (the fireball pass read kFireBuf's count before the wildfires added theirs), so a fire showed as one glow; its glow
light stopped dead 200 km from the eye; each fire's wind was a random hashed direction and the burnt ellipse turned with
a slow wander; smoke was orange-brown and lit as at its fire's centre even where a pall reached past the terminator, so
the night side and dusk views glowed. Now: points at every range, the glow ~4x dimmer and fading out, the wind field,
grey smoke with a per-pixel Earth shadow.
