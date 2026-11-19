---
pages: [rendering/clouds.md, rendering/clouds/field.md, rendering/clouds/layers.md]
code: [shaders/include/clouds_v2.glsl, shaders/cloud_v2_march.comp, shaders/cloud_v2_cdc.comp, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
There is no fractus (scud) under raining cloud bases. Below a raining base the clouds v2 field draws only the
rain shafts (curtains squeezed 0.85 along the wind everywhere). The "Fractus (ragged scud)" slider and the
`clouds_v2.fractus` settings key are gone (an old settings file's key is ignored on load); the UBO's `vert.w` is
spare and slider slot 531 is free. The cloud shell no longer widens 500 m downward for it.

## History
Fractus was added 2026-11-05 (night wave "vertical") and patched twice (2026-11-05 integration fix, 2026-11-15
"seen from below only", note 2026-11-15-fractus-from-below.md: that note and the fractus parts of
2026-11-05-varied-bases-lenticulars.md are superseded by this one). Removed 2026-11-18 at the user's request
(issue_20261118_210125 "Cloud wisps in rain": the small clouds at the top of rain columns were not up to the
look of the sim): from under a raining base they drew a grainy, speckled ragged layer.
