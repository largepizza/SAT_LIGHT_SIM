---
pages: [rendering/clouds/march.md, rendering/clouds/field.md]
code: [shaders/include/clouds_v2.glsl (cv2FarColumn), shaders/cloud_v2_far.comp, src/simulations/SatelliteSim.h]
---
## Now
The far cloud layer (orbit, 600 -> 8900 km cross-fade) draws cloud where the volumetric march does: cv2FarColumn finds
the field margin at which cv2FieldLow's density at the cloud's widest point survives the mean erosion ("Detail" x the
type's erosion strength, the convective profile's reqMin, the small-cumulus lift, the sub-pixel share), by a 10-step
bisection on the same terms, and the pixel's cloud share ramps up from that edge. So the far layer follows "Detail"
as the march does (from 980 km the march covers 94% / 37% / 17% of a nadir frame at Detail 0 / 0.77 / 1.5).
"Far cloud layer coverage bias" (`clouds_v2.far_layer_cover_bias`) defaults to 0. March alone vs far layer alone over
7.9 N 131.8 E: cloud fraction 0.44 / 0.36 at 980 km, 0.31 / 0.28 at 2200, 0.19 / 0.18 at 5000, 0.13 / 0.13 at 9000 km;
3-px-blurred correlation 0.93-0.99.

## History
Issue 20261021_173412 ("2D and volumetric clouds do not match"): the far column thresholded the plain field at
1 - coverage - bias, ignoring the erosion, and the review-24 bias (+0.12, tuned against a march with less erosion)
pushed it further: 90% cloud where the march had 45% (r 0.59 at 980 km, 0.86 at 9000). A plain bias of -0.12 also matched
at the shipped Detail but cannot follow the setting; the eroded-edge model matches the march at Detail 0, 0.77 and 1.5 (diagnostic mask IoU 0.95 / 0.78 / 0.69-0.75).
