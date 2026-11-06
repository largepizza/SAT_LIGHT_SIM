---
pages: [rendering/clouds/storms.md, simulation/weather.md]
code: [shaders/include/clouds_v2.glsl, src/simulations/SatelliteSimWeather.cpp]
---
## Now
An overshooting top is a swell of the tower's own head, not a separate dome. About a third of the anvil-reaching towers
(overshoot hash > 0.6) raise their head's top over the core by up to "Cb overshoot" (`clouds_v2.cb_overshoot_km`) + 600 m
above the anvil lid, as a broad, low mound about 0.55 tower radii across. Above the lid it is the tower's cloud; it merges
into the anvil plate through the smooth max. The overshoot pulses: each tower's top pushes up and settles back on its own
10-20 sim-minute cycle, continuously in the tower's age. Over every tower's top the cloud margin falls 10 m per metre of
height, so nothing of a tower stands above its top.

## History
Until 2026-11-06 an overshoot was a dome primitive (0.35 tower radii, 1 km high, its lobes damped). Above a tower's top
the margin was the horizontal distance from its axis at any height, so the lobes and "Storm bubbles" stood a pillar ~2 km
wide over every anvil-reaching tower, up to lid + overshoot + 800 m: with the dome it read as a mushroom (a thin stem
under a smooth round cap) growing out of the anvil (issue 20261105_201719). With `cb_overshoot_km` 0 the pillar was
still there as a flat-topped spire.

## Now (material, same day)
A storm's towers and anvil are the cumulus layer's cloud: the storm objects give only the shape (tower, flared head,
lid at the tropopause, plate spreading downwind, overshoot swell); density (the congestus type's, the plate thinning
with age and toward its edge), billow erosion, multiple-scattering and ambient multipliers and the thermal bubbles are
the cumulus'. The fine bubbles are off on storms (on a 20-km wall they read as a sponge of pits from afar). The plate's
own thin-ice density and the hurricane CDO top density setting (`storm_cyc_top_density`) no longer apply.
