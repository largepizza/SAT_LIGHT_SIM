---
pages: [rendering/clouds/march.md, rendering/clouds/storms.md]
code: [shaders/cloud_v2_march.comp, shaders/lightning.frag, shaders/include/lightning_draw.glsl, src/simulations/SatelliteSimLightning.cpp]
---
## Now
Reflect-Orbital beams and lightning (flashes and engine lights) light cloud with the same droplet phase as the Sun and
the Moon. Beams (`beamFinish`): the Mie table at the sample's droplet size, the low scattering orders and the diffuse
field at the droplets' mean cosine, forward peak capped at 40 like the Moon's. Flashes and engine lights
(`lightning.frag` `boltDropPhase`): the analytic HG + Draine droplet fit at r_eff 10 um, the ice lobe above the
glaciation band at the lit point's height; their diffuse field is unchanged. The two-lobe HG phase remains only with
the droplet phase off.

## History
Until 2026-11-23 beams and flashes used the older two-lobe HG phase. Beam-lit decks gained lobe relief and became
brighter (display levels 14 -> 50 and 15 -> 31 at issue_20261113_183043); +0.8-1.0 ms GPU where beams light cloud
(RTX 3070 Ti, 1600x900). The user kept the new brightness.
