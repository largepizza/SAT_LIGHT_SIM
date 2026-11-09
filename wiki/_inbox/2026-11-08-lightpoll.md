---
pages: [rendering/atmosphere-and-sky.md, simulation/visibility.md]
code: [shaders/include/darksky.glsl, shaders/sat_sky.frag]
---
## Now
The Milky Way (direct view and its sea reflection) and the zodiacal light are scaled by two light-pollution terms:
the dark-sky gate `darkSkyVis` (the detection threshold: faint parts go first) and `darkSkyArtContrast(dir)` =
1 / (1 + ratio), ratio = the skyglow model's artificial-to-natural sky brightness in that direction. The second is the
contrast an eye adapted to the brighter sky sees: the rendered night background is not brightened by the skyglow, so
without it a feature kept its dark-sky contrast. Only the artificial glow counts (moonlight and twilight are rendered
into the background). Not applied to the aurora or comets. Core contrast kept vs a pristine sky: ~0.75 at 21.7
mag/arcsec^2, 0.31 at 21.0, 0.15 at 20.5, 0.025 at 19.5, 0.003 at 18.5.

## History
2026-11-08, issue 20261108_174856 ("Light pollution?", 19.76N 98.60W, 3745 m, ~55 km NE of Mexico City): the model
put that sky at 20.52 mag/arcsec^2 (D. Lorenz's atlas, 2016 tiles: zone 5b, 20.0-19.5 at the ground), yet the band's
core rendered +286% over the sky; now +57% (physical: a 21.0 core on a 20.4 sky, +58%). Dark sites unchanged.
