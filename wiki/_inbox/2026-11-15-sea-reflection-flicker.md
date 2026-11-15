---
pages: [rendering/sea.md]
code: [shaders/sat_sky.frag, shaders/cloud_v2_searefl.comp, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
Reflected clouds (rendering/sea.md, "Reflected clouds"): the sea no longer reads the reflection pass's image where each
pixel's own wave facet points. It reads it at the facets' MEAN direction: the flat sea's mirror direction, lifted three
times as far as the reflected sky's roughness lift (at a grazing angle the facets that face the eye tilt toward it, so the
sea just below the horizon mirrors cloud well above it), blurred to the facets' spread of reflected directions (0.1 rad)
with one trilinear read of the pass's mip chain. The pass now builds that chain: levels 1-3 (2, 4, 8 texels) per 8x8
workgroup in shared memory, levels 4-6 (16, 32, 64) in a second, tiny dispatch of the same shader (specialization
constant 2), stored in a row under each region of the far layer's image. The wave facets still shape the sea through the
Fresnel term and the clear sky's reflection. Cost: ~0.03 ms (the reduction), the sky pass unchanged within noise
(RTX 3070 Ti, 1600x900).

## History
2026-11-15 (issue 20261115_014814, "Rapid flicker with ocean reflections"): the first version of the pass was read where
each facet's reflected ray points (mapped back to the flat sea, its depression at least 0.8x the view's own, bilinear, with
a vertical spread by distance and roughness). The near sea's facets point at directions tens of texels apart and, from 43 m
in motion, change from frame to frame, so every pixel drew a new sample of cloud or gap each frame: the near sea flickered
bright and dark. Frame-to-frame change on the reporter's trail (3 frames, the report's crop) 17.5 -> 12.4 (11.7 with the
pass off); the far band's hard outline count 262 -> 0; its reflected-cloud weight 0.80 -> 0.82.
