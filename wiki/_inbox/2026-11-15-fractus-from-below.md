---
pages: [rendering/clouds.md]
code: [shaders/include/clouds_v2.glsl, shaders/cloud_v2_march.comp, shaders/cloud_v2_cdc.comp]
---
## Now
Fractus (the ragged scud band 60 m to 250-550 m under a raining base, setting "Fractus", `clouds_v2.fractus`, default 1)
is drawn only where it is seen from below: on top of its 3-7 km distance fade it fades in between 0 and ~4.6 deg of
elevation from the eye (tan 0 .. 0.08). From level with the band or above it, there is no scud; the rain curtains under
the base keep their vertical streaks and reach the ground. Ground views are unchanged.

## History
2026-11-15 (issue_20261115_132950 "Odd rain clumps"): from 2.2 km beside a storm whose base was at the eye's height,
the band cut off by the 3-7 km ring drew detached, flat-topped pale lenses under the base, lit brighter than the rain.
No measurable cost change (cloud march stays at 128 registers).
