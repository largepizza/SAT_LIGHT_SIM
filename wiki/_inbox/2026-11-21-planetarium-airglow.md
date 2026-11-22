---
pages: [rendering/aurora-airglow.md, rendering/hardware-tiers.md]
code: [shaders/sat_sky.frag, shaders/include/sky_atmos.glsl]
---
## Now
Planetarium draws the green and sodium airglow bands in closed form, once a pixel, at the full sky's mean level: each
band's column along the ray (a Gaussian shell: grazing, or per crossing at its zenith angle), x 0.6 (the patches'
mean) x (1 - 0.727 x Airglow coverage), the green x 0.73 (the part of its layer under the 100-km top of the full
sky's march), x the evening decay and the sodium's winter maximum, x the Sun's glare gate from orbit. It has no
patches or coverage noise. The red band and the aurora stay off on that preset. It follows the same settings
(`clouds.airglow_gain`, the per-band gains, `airglow_coverage_gain`).

## History
Until 2026-11-21 it drew the green band alone at a level set for the old tonemap and the old default gain, and gave
every upward ray from the ground the horizon's slant path: a thick green limb band from orbit (3.31 x
Medium's radiance) and a flat green night sky from the ground (7.10 x) (issue_20261121_160405 shots 2-3).
Now 0.83 x and 1.03 x.
