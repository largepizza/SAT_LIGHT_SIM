---
pages: [rendering/clouds/march.md, rendering/atmosphere-and-sky.md]
code: [shaders/cloud_v2_march.comp, shaders/include/atmosphere.glsl]
---
## Now
The cloud march attenuates each lit sample's light by the air between the eye and that sample (`cv2AirOD`), and
that air is the curved path's column: `atmColumn` (the Chapman column of `atmosphere.glsl`, Rayleigh 8 km and
aerosol 1.2 km scale heights), the same function the photometry uses. The air split the march hands to the sky
pass (the distance at which the sky pass cuts "the air in front of the cloud", `tAirFrontM`) is the distance
where that same column's transmittance equals the samples' light-weighted mean transmittance. It is solved by
bisection on `cv2AirOD` between the eye and the samples' mean distance (12 halvings: 0.6 km at the limb from
428 km). One cloud keeps its own distance; samples spread along the ray get the haze of the point that has their
mean air in front of it. The march stays at 128 registers. Its GPU cost: none measurable at a 50-degree orbit
view, about +0.25 ms (18.6 -> 18.9 ms GPU, RTX 3070 Ti, inside the reps' +-0.5 ms scatter) at a 1.2-degree zoom
on the limb where every pixel is cloud.

## History
Until 2026-11-21 `cv2AirOD` took the air on the straight line between the eye's height and the sample's
(`beamColumn`), and the split was `mean distance x odSamples / odAtMeanDistance`, the optical depth taken linear
in distance. Both are right for a level ray near the ground (the 2026-11-08 rain-curtain fix they came from), and
wrong from orbit: nearly all of a limb ray's air is in its last ~200 km before the tangent point. From 428 km the
straight-line column gave a sample at the tangent point 0.63 of optical depth where the path has 4.3, less air to
a sample beyond the tangent point than to one at it, and the linear solve put the split hundreds of km short of
the nearest cloud sample, at 4-20 km of altitude. Clouds overlapping in depth along a limb ray lost half to all of
the haze in front of them and most of their light's attenuation: a band of dark purple and brown streaks just
under the Earth's limb, in the march texels' blocks (issue_20261121_154506, "Odd horizon darking and
pixelation"; shot 1 at a 1.2-degree field of view: the deepest dip of the row-mean luminance in the band 7.5 ->
1.0 levels). Bisection on the straight-line column alone removed the dark streaks but left the band darker than
the clear limb beside it in green and blue ((177, 179, 186) against (167, 181, 193) with the clouds knocked out;
(185, 190, 199) with the curved column).
