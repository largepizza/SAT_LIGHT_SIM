---
pages: [rendering/atmosphere-and-sky.md, rendering/sea.md]
code: [src/simulations/SatelliteSimRefraction.cpp, src/simulations/SatelliteSim.h, shaders/sat_sky.frag]
---
## Now
**Sunspots** are bipolar active-region GROUPS on the solar sphere (SatelliteSimRefraction.cpp `fillSunspots`, a pure
function of sim time; sat_sky.frag `sunspotFactor`). Up to 16 groups on the visible side: their number follows the
11-year cycle's activity, each lives 4-60 days (bigger longer), grows fast and decays slowly, sits 6-32 degrees from the
solar equator, is tilted by Joy's law (the leader nearer the equator) and rides the Sun's differential rotation across
the disc east to west. A group is a leader spot, one follower or a cluster of them, and 8-22 PORES (pinprick dots, no
penumbra); only spots larger than ~2500 km have an umbra inside a ragged, filamented penumbra, and complex (delta)
groups have several umbrae sharing one. Faculae brighten groups toward the limb. The Sun's Fury's flare region (a large
delta group, 14 N) crosses the central meridian at the flare, 2037-07-14 09:30 UTC, and is on the disc for the weeks
round it. Spots show where the disc is not saturated (a low or hazy Sun, or a zoomed view through haze). The groups go
to the GPU in the refraction block of GroundBeamsBuf (`GpuRefraction::spotInfo / spots`); harness `refraction` lists
them (`sunspot_groups`, `solar_cycle`).

**The Sun's glare and corona follow the light of its visible disc**: the air columns they use are the mean over the
disc's visible rows (weighted by chord x transmittance), not the ray through the disc's centre; above the refraction
table (the disc over ~4 deg) the exact Sun path is used.

**The sea's glitter** is drawn with an integer hash (it was a sin hash, which drew repeating diagonal streaks from orbit),
and its sparkle contrast falls with the number of facets a pixel covers ((footprint / 0.3 m)^2, at least 150): from
orbit the glint is its smooth mean. The glint's shadowing and masking are the whole wave field's, taken against the
mean sea surface (Saunders / Cox-Munk), so a resolved wave's back slope under a low Sun still glints.

**The Moon's face** is Lommel-Seeliger, 2 mu0 / (mu0 + mu): a flat full disc, a bright face up to the terminator; its
total light follows the phase law (Allen's, the same brightness everything the Moon lights uses) over the
Lommel-Seeliger sphere's integrated phase function, scaled so the full Moon's total is as before. SKY_LITE keeps the old
Lambert shading (size budget).

## History
2026-10-27, issues 20261027_000948 / _001448 / _001706 / _003747. Sunspots were 2-8 identical round spots at fixed places
on the disc, their number from the geomagnetic Kp. The glare used the columns of the ray through the disc's centre:
while it grazed the sea it crossed hundreds of km of air (no glare over a bright half-set disc), and when the apparent
centre went below the horizon the lookup gave the short column of a ray ending on the sea, so the whole veil came on at
once (the frame's mean 0.23 -> 0.73 within 4 s of sim time); above ~4 deg the glare took the 4-deg columns' tint. The
glitter's sparkle hash was a sin hash (diagonal streaks across the glint from orbit), and its masking used the resolved
wave normal (the back slopes of every wave black under a low Sun). The Moon was Lambert x the lit fraction x a strong limb darkening: a shaded ball, 1.8-2x the phase law's light at quarter.
