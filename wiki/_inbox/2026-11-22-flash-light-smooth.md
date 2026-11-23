---
pages: [rendering/clouds/weather.md, rendering/clouds/temporal.md]
code: [shaders/cloud_v2_flashvol.comp, shaders/include/flash_volume.glsl, shaders/lightning.frag, shaders/cloud_v2_resolve.comp]
---
## Now
Every slot of the light volume image (16 slots of 32 azimuths x 32 elevations x 16 distances) is baked the same way
for a lightning flash and for an engine light: each distance step is read at 6 places with no per-direction jitter,
and the slot holds the optical depth, the transmittance and where in each step the depth is centred. Both lights read
it with `fvOdFine`, blended over the directions by a cubic B-spline and eased inside each distance step: an engine
light blends the transmittances, a flash the depths (blended as transmittances, a flash inside a tower lit the whole
tower near white). The cloud resolve keeps a history of the cloud's distance only while the eye is still both across and
ALONG each texel's ray (along: under 0.1% of the distance a frame, 60 m/s at 1 km).

## History
2026-11-22 (wave 20261122 c18): a flash's slot had a fixed jitter per direction and 1-2 reads a step, read by a
bilinear blend of the depths (`fvAt`, deleted): a spider lighting a Cb drew the volume's directions as faceted polygon
patches and a starburst of spokes round each emitter (issue_20261122_213848). The distance history was gated on the
parallax alone, which is 0 at the point the eye flies toward: at 40-65 km/s the screen centre kept a distance ~9 km
too far and the flash light there read deep inside the tower, a black disc at the screen centre
(issue_20261122_213657).
