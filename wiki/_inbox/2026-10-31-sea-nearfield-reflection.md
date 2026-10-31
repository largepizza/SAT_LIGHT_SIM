# The sea from just above it: the reflected cloud deck is smeared by the unresolved ripple slope at every range

Date: 2026-10-31. Cluster c31 (issue_20261022_172435). Files: `shaders/sat_sky.frag` (ocean branch,
the reflected-cloud smear). No settings, no defaults, no parameters changed.

## What it was

With the eye 2 m over the water (the report's view: 39.1080 N 161.6340 E, alt 0, camera el -6.59,
fovY 64.66) the sea is a perfect mirror of the cloud deck: the reflected clouds came out as a
hard-edged contour map - white rims round flat patches, each cloud outlined - which the reporter
read as "white-rimmed contour terraces and flat dark pools".

Attribution (one capture per knockout, metric = |row-to-row luma| > 0.04 over the lower third,
largest 4-connected rim):

| capture | hard-step px | largest rim |
|---|---|---|
| untouched | 53988 | 11366 |
| `knockout +ocean_sky_reflection` | 17890 | 942 |
| `knockout +volumetric_cloud_march` | 21013 | 1030 |
| `knockout +cloud_shadow_per_pixel` | 41194 | 6872 |
| `set clouds.ocean_whitecaps 0` | identical | identical |
| `set clouds.ocean_far_ripple 0` | identical | identical |

It is not the wave amplitude quantisation, not the normal quantisation, not foam, and not the
far-ripple LOD switch. The dark pools are the cloud shadow on the water (correct); the rims are
the reflected cloud silhouettes.

Why the reflection was sharp at that eye height: `oceanWaveFootK` is 0.1 while the eye is still
("Sea wave sharpness" deliberately leaves the wave octaves 10x finer than a pixel for the sky TAA
to average), so the footprint filter's lost slope (`seaRough`) is ~0 over the near sea - and the
reflected-cloud smear (review 22b) was gated on `smoothstep(1500, 25000, dist)`, a far-sea
softening. With a 2-5 m eye the whole sea is 2-40 m out, so the smear never ran.

## What changed

The smear is now at every range, and its width is the old footprint-slope width plus the slope a
pixel cannot resolve (Cox & Munk `sqrt(0.003 + 0.00512 W + 0.06 seaRoughFoot)`, the same `s2` the
Sun glint's Beckmann lobe uses). The blur is an ANGLE, not a distance: the same screen pixels at
6 m as at 6 km. The five taps and the rest of the reflection (the clear march, the cloud-distance
weight `rKf`, the Fresnel, the foam, the glint, the aerial perspective) are untouched.

## Numbers (before -> after, pinned exposure, 1920x1009)

- Largest hard rim in the lower third: 11366 -> 4800 px (-58%). Hard-step pixels -19%, mean
  |row-to-row luma| -18%.
- Lower-third high-pass std 9.29 -> 7.89 levels (the fine rims are what goes); luma std
  44.9 -> 46.8, so the wave-shaped structure is kept.
- The 2 km view over the same sea: mean |diff| 0.31 levels, against a 1.01-level noise floor
  between two settled captures of the same view - unchanged.
- The horizon seam is NOT fixed: 35.9 -> 35.0 levels at its worst row. That one is the grazing
  Fresnel (`min((1-nv)^3, 0.5)` on a dark water colour under a bright hazy sky), not the
  far-ripple LOD switch - with a 2 m eye the far-ripple band (3.3-8 km) sits inside one pixel of
  the horizon.

Evidence: `qa/waves/20261030b/evidence/c31/` (README.md, metrics.json, before/after captures and
crops, the knockout captures).
