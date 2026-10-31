---
pages: [rendering/sea.md, rendering/clouds/march.md, history/known-issues.md]
code: [shaders/sat_sky.frag, shaders/cloud_v2_march.comp]
---
## Now
The concentric bands that a 30 km eye sees over the Coral Sea (issue_20261025_190008's frame) are **cloud**, not
sea: they are the known open "concentric rings looking down from inside the anvil" (pass 16, 2026-09-28), the
march-step contour rings a thin flat high deck draws when it is crossed at a grazing angle. Measured at that view
(1920x1009, exposure pinned at EV -0.88888): turning the far-sea ripple off (`clouds.ocean_far_ripple 0`) moves the
frame 0.425 display levels mean and leaves the band energy at 28.23 against 28.24 milli-levels, and at that view the
ripple's own weight is 0.13 with a 64 m cell (a sub-2-pixel grain, and its octave index spans 1.85..3.3 across the
whole frame, so it can cross two boundaries, not the ten arcs on screen). A probe of the cloud composite put the
bands in `cloudA`, the march's additive radiance (band energy 36.8 against the beauty frame's 24.6; `airFront` 5.1;
the aerial perspective's attenuation 0.85); they survive the cloud shadow with both paths off (35.3), the occupancy
clipmap off, `sky_ap 0` (the exact per-pixel atmosphere loop), `history_weight 1`, `full_res 0`, the light march's
LOD at 58 and 232 m, the erosion LOD and the self-shadow cone, and they go away only with the volumetric march
knocked out (3.3). They are anchored to neither the screen nor the eye: rotating the camera 40 degrees at the same
eye removes them and rotating back restores them, and moving the clock 10 or 60 minutes (a wholly different cloud
field at the same place and camera) removes them - so they are one patch of thin cloud at one moment of the weather,
which is why the reporter's session shows them too (the weather map is a function of place and sim time).

## History
2026-10-31 (c35, follow-up to issue_20261025_190008). The report's own resolution text says the bands are "the
far-sea ripple's distance-octave banding, filed as its own follow-up"; that is wrong and is in front of the reporter,
and the sea page should not repeat it. The far-ripple octave selection is real (`want = log2(seaTune.w * footA *
kSeaFreq)` makes the ripple's cell a function of distance) but it is latent at this view and was measured to move the
frame by 0.4 levels. No renderer change was made for c35; evidence is `qa/waves/20261030b/evidence/c35/`.
