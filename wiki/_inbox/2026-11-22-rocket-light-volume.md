---
pages: [rendering/launch-visuals.md]
code: [shaders/lightning.frag, shaders/include/flash_volume.glsl, shaders/cloud_v2_flashvol.comp, src/simulations/SatelliteSimLightning.cpp]
---
## Now
A burning stage, a pad's floodlights, a ship's and a wildfire's glow (the engine lights) light the clouds through the
cloud around them, by the formula a lightning flash uses: each of up to six lights holds a slot of the flashes' light
volume (the cloud's optical depth outward from the light, 32 x 32 directions x 16 distances to 24 km; 16 slots, shared,
a flash first), and the cloud a pixel shows is lit by I / (r^2 + r0^2) x (the cloud's phase x e^-depth + the diffuse
field of the slab between). An engine light stands outside the lobes it lights, so its volume answers only for the
cloud that stands between (read 200 m short of the lit point, the depth inside a distance step rebuilt from where the
step's depth is centred, the transmittances of the directions about the point blended by a cubic B-spline), the lit
point's own cloud comes from the pixel's opacity (flat over the composite's last few per cent of transmittance, whose
level lines a steady light would draw as contours), and the diffuse field's side escape is the cloud look's own
(`clouds_v2.ms_side_escape`). Cloud of a few optical depths passes most of the light; it is dark from about 10. The level is set so a thick face with the light behind the eye and nothing between reads
as it did by distance alone. A moving light's next volume is baked ahead of it, one re-bake distance (100-600 m) along
its way, and the two volumes are mixed by how far along the light is; a standing light bakes again when the cloud map
has drifted that far and fades over 0.3 s. The lit haze along the ray is cut by the same volume. Setting (no UI):
`launches.engine_light_volume`, default 1; 0 = every light by distance alone. A light without a slot (more than six,
more than 24 km from the cloud layer) is lit by distance alone, as are Potato and Planetarium, which draw no cloud.
Harness: `launch state` lists `engine_light_volumes` and the bakes; `lightning light e_side= e_single= e_skin_m=
e_level=` changes the tunables, `e_od=` shows where the cloud depth between is at least a value.

## History
Until 2026-11-22 an engine light lit every cloud in line of sight by 0.85 x opacity^1.5 / (r^2 + r0^2), x 0.35 from
the far side by height: no cloud between, a rocket in a deck drew an even blown-out blob (issue_20261122_105107).
Measured numbers and what was tried and dropped (the volume read at the lit face, the flashes' side escape 0.03, a
fade over a fixed time after each bake) are in docs/subsystems/launches.md "Rocket light through the light volume".
