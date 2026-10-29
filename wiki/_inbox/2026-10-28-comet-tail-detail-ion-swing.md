---
pages: ["new: simulation/comets.md", rendering/sky.md]
code: [src/simulations/Comets.cpp, src/simulations/SatelliteSimComets.cpp, shaders/comet_splat.vert, shaders/comet_splat.frag, shaders/comet_field.frag, shaders/comet_composite.frag]
---
## Now
The dust tail has detail that resolves as the view zooms in: each grain carries its local (emission time, ln beta) field,
a field pass splats it light-weighted at a quarter of the splat resolution, and the composite modulates the dust light
(mean 1) with synchronic bands in emission time (periods 1.7 d down to 9 min: time-variable jets and outbursts), oblique
striae (emission time + 0.7 ln beta, in dust older than a few days) and size filaments along the syndynes. Each octave
shows once its bands are 4-10 screen pixels apart and the field's own spread there is well under its period
(`comets.tail_detail` 1, 0 = off). The splat target is an atlas of 6 levels, so a particle wider than the quad cap draws
into a coarser level instead of being culled: zoomed in at the closest approach the tail stays. Ion parcels are picked up
on a grid in time AND true anomaly (every 0.01 day and every 0.02 rad of the radial's swing), so the perihelion swing
draws continuous streamers; their brightness follows the gas production (r_eff^-(n-2)) and fluorescence by the sunlight
where each ion is now; the composite's shoulder acts on the brightest channel, so the ion tail stays CO+ deep blue.
Builds run on the frame only after a time jump, never every frame of a fast warp.

## History
Until 2026-10-28 (issues 20261028_221703, 20261028_221813): the tail was smooth at every zoom and vanished below ~20 deg
of field at the closest approach (particles past 4x the quad cap culled); the swing drew the ion tail as separate blue
puffs (a pick-up every 0.01 day, ~0.5 rad of radial swing each at perihelion), pale sky blue (the luminance shoulder let
the blue channel clip), and at 1 d/s through perihelion every frame rebuilt the particles on the main thread (145 frames
over 50 ms in 240, now 0). Comet GPU cost (tails on - off): closest approach 0.9 -> 0.9-1.3 ms, the swing about the same.
