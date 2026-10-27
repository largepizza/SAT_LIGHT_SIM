---
pages: [simulation/weather.md, rendering/clouds.md]
code: [shaders/cloud_v2_weather.comp, shaders/cloud_march.comp, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
Under a tropical cyclone's central dense overcast the low cloud is an overcast deck whatever the source map has there
(the weather bake floors its coverage at the full threshold, fading out with the CDO's edge at 0.75-1.1 of its radius),
and it rains from it: the CDO's precipitation is heaviest near the radius of maximum wind and thins to light
stratiform rain at its edge (0.6 -> 0.25 of the stamp's rain), so the radar shows a core, a fall-off and the bands.
The volumetric clouds' "smooth tiles" (full-res clouds only) wait until the eye has been still for 64 frames. The
plume / smoke billows in the dust-list composite are filtered to the pixel footprint (each noise octave fades to its
mean as its texels per pixel pass 0.5 -> 2), so a wildfire pall seen from orbit is smooth instead of a pixel-scale
honeycomb.

## History
2026-10-27, issues 20261027_003307 / _004019. The CDO's coverage was an additive brightness of mix(0.55, 1, intensity):
over a clear map (Typhoon Sanba weakening over Laos, intensity 0.29) the deck under the shield was scattered cloud and
cv2FieldLow's rain shafts (which need a formed column) fell only within ~2 rmw of the eye: 77 km out, under an optical
depth of ~230, no rain. Smooth tiles were classified from a history that a fast flight had just replaced with the
quarter grid's tent (smooth everywhere): 64-px squares of the quarter lattice's dots, greyer and missing the cirrus,
for ~1.5 s after stopping. The smoke billows' noise coordinate is in units of a Gaussian's SMALLEST sigma (a pall's
thickness, ~1 km): from 85 km a half-res pixel spanned ~25 texels of an unmipped volume.
