---
pages: [simulation/clouds.md]
code: [shaders/include/clouds_v2.glsl]
---
## Now
Ice fog / diamond dust now varies by REGION as well as by patch. `cv2FogDust`'s ice branch multiplies the ice
mask by a two-octave `cv2DirNoise` field of its own: `0.78 * noise(dirE * 6.64 - 8.47) + 0.22 * noise(dirE * 10.86
- 6.04)`, mapped to `0.12 + 0.88 * smoothstep(0.32, 0.57, reg)` — regions of roughly 960 km and 590 km, amplitude
0.12..1.0 (mean ~0.65). The frequencies and offsets are its own, not the dust's (5.0 / 15.0) and not the cluster
patches (`patchI`, ~30 km), so one stretch of the ice sheet hazes and the next is nearly clear. From the
reporter's 751 km view (issue_20261030_204305, ice fog amount 1.19 / density 0.000719) the film's footprint splits
25.8% gone / 15.5% partial / 58.7% kept, luminance std over the dark-water film 10.2 -> 14.7 (+44%), and the film
over the open Southern Ocean drops from 19.6 to 14.6 levels of brightening; 1300 km further out (Weddell Sea) the
same film falls from 21.2 to 6.7 levels. At 1.5 km AGL at the same amount the layer is still there (adds 5.5
levels over 91% of the frame, 8.4 over its lower 65%), so the low-altitude look is unchanged.

The ice mask itself (latitude / land, with the `1 - smoothstep(-80, -72, lat)` fallback that covers the ice
shelves, whose DEM reads 0 m like the sea) is untouched: the field only scales its extinction. A true sea-ice or
shelf mask would let the Southern Ocean's open water be excluded outright rather than by the region field.

## History
The ice fog was a single uniform veil since 2026-09-29 (`kIceFogH = 250`, `iceS = sunE.w * fog2.w * ice * clear *
(0.2 + 0.8 * patchI) * exp(-agl / 250)`). The dust path had had large-scale regions since the 2026-09-29 review
("the variation is GENTLE — regions 0.3-1 at ~500-1500 km"); the ice branch had no equivalent, so at high amounts
it read as one white film from orbit. No eye-altitude fade was added: the dust fades to 8% by 60 km because its
plumes are terrestrial, but the reporter wants the ice fog at these high settings, and a global altitude fade would
dim exactly that. That knob is still available if a future report asks for it.
