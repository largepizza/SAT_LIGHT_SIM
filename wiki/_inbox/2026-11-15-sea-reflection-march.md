---
pages: [rendering/sea.md, rendering/clouds.md]
code: [shaders/cloud_v2_searefl.comp, shaders/sat_sky.frag, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
Reflected clouds (rendering/sea.md, "Reflected clouds"): a compute pass, `cloud_v2_searefl.comp`, marches the cloud field
along the MIRRORED ray of the flat sea at quarter resolution (the frame plus 25% past each edge): 14 jittered u^2 steps from
the cloud shell's entry to 160 km on the coarse field, lit from the Sun transmittance volume, the sample's own column, the
diffuse-field model, the Sun / sky table and the cloud-over-ground map (no light march), weighted e^(-t / 40 km) for the air
in front, accumulated over frames (weight 0.2) with the sea point's exact reprojection. It stores the clouds' light and the
share of the clear reflected sky still seen (gaps + the air in front, shadowed under a deck) in two regions of the far cloud
layer's image (never used together). The sky pass maps each wave's reflected ray back to the flat sea's view ray whose
mirror it is (depression at least 0.8x the view's own), reads that bilinearly with a vertical spread by distance and
roughness, and composes clear x share + light. Setting: `clouds_v2.sea_refl_march` (1 = on; 0 = the clear reflection dimmed
by the cloud shadow). Not run above 30 km or with clouds v2 off. Cost ~0.5 ms (RTX 3070 Ti, 1080p, storm over the sea), the
sky pass ~0.2-3 ms cheaper than the screen-space lookup it replaces.

## History
2026-11-12 to 2026-11-15 the reflected clouds were looked up on screen: the reflected ray's crossings of five altitudes
(0.3-5 km) were projected into the camera and tested against the cloud target's distance (2026-11-14: a soft test). At a
grazing angle a sea row maps to ~1/4 of a sky row, so reflected storms drew as a compressed, banded strip with hard wavy
outlines that reshaped with camera motion (issue 20261114_233558), and clouds off screen or behind nearer ones were missing.
Measured at that issue: flat cuts along the band's outline 4171 -> 257 (5x5 median), trail 2-frame change (blur16) 6.92 ->
6.08. The flat-cut rectangular patches on the far "water" in issue 20261114_234423 shot 2 are not the sea (debug view 50
colours no pixels there): they are far land, terraced in the albedo debug view, with the half-res cloud alpha stepping the
same way; unchanged.
