---
pages: [rendering/clouds/temporal.md, rendering/clouds/march.md, using/settings.md]
code: [shaders/cloud_v2_resolve.comp, shaders/cloud_v2_march.comp, src/simulations/SatelliteSim.h]
---
## Now
The cloud resolve gives a clear pixel the parallax of the nearest cloud in its 7x7 new samples when its own 3x3
holds none. While the eye moves, such a pixel is in motion (tight clamp, moving weight), so the cloud it showed a
frame ago is rejected.

"Silver lining" (`clouds_v2.silver_lining`, default 0.5, 0-4) is sunlight scattered forward through thin water
cloud: a narrow lobe (g 0.92, weight 0.12) plus a wider one (g 0.8, weight 0.5), attenuated at 0.3 of the optical
depth toward the Sun. The rim of a cloud in front of the Sun glows and light leaks through its thin parts; views
away from the Sun do not change. Cost within noise, 128 registers.

Defaults: "Light march footprint (x length)" (`clouds_v2.light_sample_footprint`) 0, "Fast-flight cloud LOD"
(`clouds_v2.fast_flight_lod`) off.

## History
2026-10-13, issues 20261013_184710 and 20261013_181252. Flying 2.5 km/s low over cumulus, every cloud dragged a
granulated copy of its trailing edge over the sea, in blocks of the sparse grid: pixels 2-6 texels behind the edge
were reprojected at infinity with the parallax floored at 50 km ("still"), and the still view's 7x7 clamp box,
which held the cloud, passed the history. Present before the batched march (the pre-batching shader shows it);
visible at render scale 0.65, where a sparse block is 6 screen pixels.

The light march footprint at 0.08 (640 m of an 8 km march) removed a cumulus's self-shadow: a backlit cloud was a
flat glowing shape cut by a straight far-shadow line. At 0 the same cloud has a shaded body and lit rims. The old
silver lining (the narrow lobe alone, at full extinction) moved a backlit rim by about 6 levels between 0 and 3.

## Now (addendum, issue 20261013_192258)
The near Sun-shadow lattices (the two shaft rings and the fine grid) carry the shadow of the low cloud, towers and
anvils only: the high layer and the mid layer's sheets cast none there. The cascade and each sample's own light
march still count the mid layer at "Mid layer shadow". Before, clouds within a ring's reach of the eye were grey
under altocumulus and white beyond it, a boundary that moved with the observer.
