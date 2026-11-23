---
pages: [rendering/clouds/storms.md, rendering/clouds/field.md]
code: [shaders/include/clouds_v2.glsl]
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
16 km). Other storm sites: up to 32% of raining columns had a clear stretch over 50 m, 0 after. Open: rain in that slab
carries a height fraction > 0 and is marched as cloud (fine steps, full light march): 24.5 -> 33.6 ms GPU for an eye in it.
