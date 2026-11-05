---
pages: [rendering/clouds/field.md, rendering/clouds/layers.md]
code: [shaders/include/clouds_v2.glsl, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
"Cumulus core" (Settings > Clouds, key `clouds_v2.cumulus_core`, default 0.5, 0-1) protects a cumulus mass's interior from the
bubbles, turrets and lobes: the margin blends toward max(margin, envelope margin) by k x smoothstep(0, 0.25, envelope margin),
so carving is full at the cloud's skin and fades with depth. Big masses stay whole, their surfaces keep the bubbles, and the
towers (the positive side) are unchanged.

The mid-level wisps read the shape volume on their own anchor: the drifted frame's z (the Earth's axis) is squeezed x0.4 as a
fixed linear map of world position, reduced in double on the CPU. They are ~2.5x longer east-west than north-south at the
equator and stay fixed to the ground as the eye moves.

Orographic lenses: the cap reads the relief 800 m upwind (it sits over the summit), half-size min(0.8 (ex - dry) + 100, 800) m,
extinction 0.05 /m, lens moisture default 0.75.

## History
Issue 20261105_084815 ("Cloud whisps evolve with movement"): the wisps' stretch was applied to the eye-relative offset, so it
was centred on the eye and the pattern slid at 0.6x the eye's across-wind speed. A hard core floor max(m, envelope - D) was
tried first; it bared the envelope's smooth inner shell (paper cut-outs). The lens was a thin grey sliver 2 km off Fuji's
summit (0.025 /m, 0.4 t); widening its footprint with the mip-2 dome laid one plate over the whole Alps and was dropped.

## Now (second round, same day)
"Cumulus bubble warp" (`clouds_v2.cumulus_bubble_warp`, default 150 m, 0-400) displaces the bubble/turret read by the cumulus
lobes, so the bubbles are lumpy blobs rather than spheres. "Cumulus head width" (`clouds_v2.cumulus_head_width`, default 0.5)
flattens each cell's top toward a plateau, so the vertical profile (widest at ~0.55 of the height) gives towers heads wider than
their stems. Lenticular lenses need prominence as well as lift (footprint min(ex - dry, 3 (ex - exB) + 150)), split into 1-4
stacked plates ~200 m apart (a pile of plates), with sharper rims.
