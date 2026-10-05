---
pages: [rendering/clouds/weather.md, rendering/clouds/temporal.md]
code: [shaders/rain_particles.vert, shaders/rain_particles.frag, src/simulations/SatelliteSimRain.cpp, shaders/cloud_v2_lightning.comp, shaders/include/cv2_optics.glsl]
---
## Now
Rain, snow, sleet and diamond dust at the eye are particles, drawn in the main pass after the sky TAA at full
resolution and blended over the tonemapped frame (premultiplied: a drop dimmer than the sky behind it darkens
it). The drops sit in nested world-fixed boxes around the eye. Box k is 4 m × 2^k and holds N0 × 2^k drops,
and each box covers [L/4, L/2] from the eye. Rain drops take a Marshall-Palmer size (0.5–5 mm) and fall at
that size's terminal velocity (2.3–8.8 m/s). Snowflakes are 2–8 mm, fall at about 1 m/s and flutter. The
rain map around the eye decides which drops exist, so walking toward a shaft its drops appear in the far
boxes first. Each drop is drawn as the streak it sweeps over a shutter time with its real coverage conserved.
Each particle stands for the rain's real cross-section per m³ divided by the particle density, so summed
along a ray the particles reproduce the rain's optical depth. Where one particle would read as a bright dot,
it fades and the rain volume carries that distance.

Drops are lit like the cloud march's rain sample at the eye: the key light through the clouds, sky ambient
through the column above, ground bounce and city light at night. The scattering uses the volume's own phase
function (`cv2_optics.glsl`), so drops at 42° from the antisolar point flash the bow's colours and drops
toward the Sun glow. Diamond dust draws only plate glints.

Settings (Weather tab, Rain & snow):

- "Drops at the eye (visibility)" (`clouds_v2.rain_streaks`, 1 = physical, up to 10).
- "Drop reach (m)" (`clouds_v2.drop_reach_m`, default 32, max 64).
- "Drop particles (x1000, nearest box)" (`clouds_v2.rain_particles_k`, default 16).
- "Drop shutter (ms)" (`clouds_v2.rain_shutter_ms`, default 33).

Motion (2026-10-05) is integrated over sim time on the CPU, so speed and wind change smoothly. Fall speed is
"Rain fall speed (x)" (`rain_fall_speed`, default 1.3) × the terminal velocities, and up to 1.35× faster in heavy
rain. Wind is "Rain wind (x ground wind)" (`rain_wind_gain`, default 1.5) × the ground wind, plus a shower's
outflow away from its core: "Storm wind (m/s)" (`rain_storm_wind_mps`, default 14). "Wind gusts" (`rain_gusts`,
default 1) adds gusts of ±20–60% with ±15° turns. The user's storm snapshot reaches 19 m/s.

Cost is about 0.35 ms at the defaults (RTX 3070 Ti, 1600x900), or +0.45 ms at a 64 m reach.

## History
These particles replaced `cloud_march.comp`'s `rainDrops` (reviews 3–22). That was a per-pixel search of a 3D
lattice inside the half-res cloud composite: a few hundred fat streaks, smeared by the sky TAA, lit by the
cloud colour along the ray, with no bow optics. The old `drop_distance_m` key (lattice layers to 512 m) is no
longer read. Removing the old drops made the cloud march about 1 ms cheaper (the user's Venezuela snapshots,
`profile_log` records 40–41, `harness_runs/rain_rebuild/`). The rain volume still marches the rain within the
particles' reach (τ 0.02–0.05, a slight double count). Lightning does not light the drops.
