---
pages: [rendering/aurora-airglow.md, rendering/points-bloom-glare.md, rendering/satellite-meshes.md]
code: [shaders/include/sky_atmos.glsl, shaders/sat_sky.frag, shaders/sky_ap.comp, shaders/cloud_march.comp, src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSim.h]
---
## Now
Airglow evolves with sim time. Its noise moves on a closed loop through noise space once a UTC day (radius 0.6,
~70 m/s of morphing), and the patterns turn about the pole at 2 x the clouds' drift phase (~47 m/s zonal), so
nothing jumps at 00:00 UTC. Each sample's brightness follows the hours since its own sunset: green
0.85 + 0.4 e^(-0.4 t) plus a post-midnight bump; sodium x a winter maximum (about 2-3x summer at 45 deg); red
0.66 + 2.06 e^(-t/2), x up to 2.5 in the equatorial-anomaly arcs (|geomagnetic lat| ~15 deg) until 3-7 h after
sunset. Each curve averages ~1 over a night. Above 40-100 km the bands are x the Sun's glare gate
(`skyGlareVisibility`, as the Milky Way), so the limb line disappears with the Sun on screen; this is not in
Planetarium (SKY_LITE). Defaults: `clouds.airglow_gain` 0.77, `airglow_red_gain` 0.005, `airglow_sodium_gain`
0.095, `airglow_coverage_gain` 0.47; `photometry.milky_way_night_vision` 0.33.
A satellite mesh's dominant lobe keeps the point sprite's (full unresolved flux) glare at every on-screen size,
weighted by its share of the sun-like light; the mesh's own glints give up that share.

## History
The airglow noise drifted at 0.015 units/s of sim-seconds-in-day: ~24 km/s, with a jump at midnight UTC. Gain
0.066 left the bands under one level from the ground. Sodium 0.095 is ~1.3x the green column, which makes the limb
line olive; ~0.02 matches the green of the ISS photographs (not adopted, user's call). Mirror glare: past 12-36 px
the glare came only from mesh texels, each holding its share of the flux, so an all-white Reflect mirror tracked
at 5 km lost its starburst from ~80 px on (before/after: `harness_runs/glare_before`, `glare_after`). Sky pass
cost of the airglow changes: within noise (8.68 vs 8.84 ms).
