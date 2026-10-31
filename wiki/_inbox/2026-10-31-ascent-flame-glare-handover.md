---
pages: [rendering/launches.md, rendering/glare.md]
code: [src/simulations/SatelliteSimLaunches.cpp (launchFillVisuals, the fP block; kFlameGlareHandoverM / kFlameGlareHandoverFullM)]
---
## Now
A burning stack's POINT (its bloom seed, its glare sprite, its point sprite) hands its light to the launch's
own emissive Gaussians **by range** as well as by resolved size: inside 12 km its flare is capped to 14 (just
past the glare threshold, the value the size-based hand-over already targets) and releases smoothly to its
unedited value by 28 km. Beyond 28 km the point is bit-for-bit what it was. The flame's Gaussians, the plume
parcels, the lit exhaust, the engine light, the sprite's position and the selection readout are untouched.

Why both hand-overs are needed: the pre-existing one keys on `Lpx`, the flame CORE's on-screen length, in a
4-30 px window. A flame is resolved by its PLUME, not its core - at 2.3 km (fov 120) `Lpx` is 5.6 px while the
plume image is already 60 px of saturated white - so `smoothstep(4, 30, 5.6) = 0.011` moved the flare by 1%.

## Measured (2026-10-31, Release, 1920x1009)
Reporter camera: lat 29.5775 lon -92.3337, eye 3737 m, az 240 el 0 fov 120, night, Starship batch
780103506198728 at T+31 s, clock paused, flame 2.3 km away. The base frame: 3820 clipped-white px, saturated
radius 34 px, a 1024 px glare sprite of 10 rays over a flame of half-peak radius 69 px; the vehicle's 1-2 px
silhouette counted 9 px.

Cause: the point's flare is ~4.7e5 at 2.3 km (Starship's `flame1_mag_100km` -6.0 gives mag -14.2) against a
threshold of ~11.5, so the glare response is pinned at its ceiling `b = 8` (s = 6.24) and
`glareNearScale` (the proximity term, up to 8x below 21.86 km, added 2026-09-26 for a mesh the camera flies up
to) multiplies `radius = 27.83 * (0.6 + s)` by 7.8: 1480 px, clamped to the 1024 px point size. The same
arithmetic gives 1117 px at 8 km (also clamped), 152 at 23 km, 122 at 48 km, 80 at 136 km. A rocket lifting off
2 km away thus inherits a close-up glare although nothing about it is resolved: the launch paints the flame
twice.

After: 613 clipped-white px, saturated radius 16, half-peak 60; the annulus means at 40-85 / 85-170 / 170-300 /
300-450 px go 0.5793 -> 0.4583, 0.1726 -> 0.1014, 0.0772 -> 0.0724, 0.0536 -> 0.0535 and now equal the same
frame's glare-off reference within 0.0005 (no measurable ray light at any radius); the vehicle's silhouette
9 -> 118 px (peak contrast 0.051 -> 0.149, all of it darker than the flame). Controls: on the pad and at 48 /
136 / 459 km the flame's own measures are identical and the whole-frame difference is at the renderer's repeat
noise (mean 0.005-0.021 display levels, 0.01-0.07% of pixels against 0.026% for a repeated capture); 12 km
off-pad 678 -> 164 clipped px, 8 km 873 -> 288.

## History
- 2026-10-31 (issue_20261019_222829 "Plumes Again"): the ascent flame was a saturated white disc with 10-12
  long straight rays and the vehicle unresolvable inside it. Established by A/B on the base: `glare_gain 0`
  removes the disc and the rays (3820 -> 625 clipped px); `flare_streak_gain 0` and `knockout +sat_meshes`
  change nothing (20 px), so it was the rocket point's glare sprite, not the flame's emission and not a hull
  glint. Range-keyed hand-over added.
- 2026-10-20 (issue_20261019_222938): the size-based hand-over (`Lpx`, 4-30 px) and `pointK`, for a Super
  Heavy seen from 200 m; unchanged here.
- 2026-09-26: the proximity size gain in `include/glare.glsl` (`glareNearGain` 8x at zero range, released by
  `glareNearRangeKm` 21.86); unchanged - the flame's cap is what neutralises it, so the slider still does what
  it says for a resolved satellite.
