---
pages: [simulation/sun-moon-planets.md, rendering/refraction.md, development/controls-and-settings.md]
code: [shaders/sat_sky.frag, src/simulations/SatelliteSimRefraction.cpp, src/simulations/SatelliteSimHudPopups.cpp]
---
## Now
**Eclipse glasses** (View popup and Settings > Photometry > Exposure, under Coronagraph; `hud.right_panel.eclipse_glasses`,
default off) draw the Sun's disc at its true radius as through a solar filter: limb darkening, sunspots, the Moon's bite
in black during a partial eclipse, with no glare, aureole, lens flare or bloom. It is a display choice, not physics: real
glasses darken everything else to black, while here only the Sun is filtered and the rest of the frame keeps its exposure.
The disc keeps each pixel's own bent-path transmittance, so a setting Sun stays flattened and orange-red (the air's
loss at the disc centre is divided out, so a horizon Sun is as bright as a high one and shows its spots); at totality
the Moon is black and the corona is drawn as without glasses. Glasses and the coronagraph are exclusive: switching one
on turns the other off. Full sky variants only (the Potato / SKY_LITE tier draws its usual disc).
Measured: with auto exposure off, the frame away from the Sun is unchanged (0.01 levels mean in the corners); the sky
pass costs the same with the glasses off (2.20 vs 2.30 ms high Sun, 9.18 vs 9.64 ms at sunset, RTX 3070 Ti 1600x900).

**The Sun's Fury's sunspot group** is abnormally large: `refraction.fury_spot_msh` (default 12000 millionths of the
solar hemisphere, about twice April 1947's record ~6100; Carrington's 1859 group ~2300), drawn as a complex of five umbrae
in one merged penumbra with large followers, 0.54 x 0.25 solar radii. At +14 deg latitude it crosses the central
meridian at the flare (2037-07-14 09:30 UTC): on the disc from 2037-07-07 ~18 UTC (east limb) to 2037-07-21 ~01 UTC
(west limb), 17 deg east of the central meridian during the 2037-07-13 eclipse.

## History
Until 2026-11-17 the Fury group was a delta group of the ordinary look at half-length 0.24 R: ~3970 MSH (measured with a
numpy port of the shader's spot function). The ordinary hashed groups are unchanged.
