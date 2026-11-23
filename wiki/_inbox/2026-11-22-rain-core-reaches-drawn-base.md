---
pages: [rendering/clouds/storms.md, rendering/clouds/field.md]
code: [shaders/include/clouds_v2.glsl, shaders/cloud_v2_march.comp]
---
## Now
The heavy rain under a storm object's dominant towers (`gCv2RainCore`, set by `cv2StormGeom`) is set up to
`kCv2RainCoreUpM` (6000 m) over the cumulonimbus type's base; the low field's rain branch (`cv2LowA`) ends it at its rain
floor, 400 m over the field's own base, which is the base the tower is drawn on (`gCv2BaseLow`). Rain under a tower
therefore starts at the drawn cloud bottom: no clear slab between the two.

## History
Until 2026-11-22 the core was set only below the type's base (900 m + 0.9 x the ground). Since 2026-11-08 a tower stands
on the field's base (condensation level, regional spread, cell offset), which is higher over land by day: at
issue_20261122_154502's column the rain ended at 2572 m and the cloud began near 2960 m (387 m of clear air, median over
16 km). Other storm sites: up to 32% of raining columns had a clear stretch over 50 m, 0 after. With the core alone the
rain in that slab carried the storm's height fraction (> 0, counted from the type's base) and was marched as cloud (fine
steps, full light march): 24.5 -> 33.6 ms GPU for an eye in it. The storm's height fraction now counts from the drawn
base and is carried negated below it (rain alone there, <= 0); the cloud body takes it back (abs). Zeroed instead, the
body's bumps hanging below the unbumped base (with the rain floor's share) were lit as rain and the base seen from
outside turned bright cream. Cost with the fix: +0.4 ms GPU at the reporter's view, +0.1 under the rain looking out.
