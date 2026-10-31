---
pages: [development/clouds.md, development/terrain.md]
code: [shaders/sat_sky.frag]
---
## Now

A ridge silhouetted against a cloud deck no longer draws a one-to-three-pixel bright line along its crest.
`scene_depth.comp` marches the half-res grid with `kTdCoarseOctaves`, so its silhouette stands several
texels proud of the full-resolution one; `cloud_v2_march.comp` clamps the volumetric deck to that surface,
and the deck behind the ridge is cut. `sat_sky.frag`'s composite multiplied the WHOLE open-sky column by the
cut cloud's remaining transmittance (0.05..0.6 on the band), so that share showed 27 luminance where the
cloud is 14 and the ridge 7 (report view, issue_20261022_180225, 60.0081 N 159.3117 W, 1060 m,
2036-11-22T02:17:11Z): overshoot 1.81x / +12 levels, on 304 of 306 columns of the silhouette. The composite
now takes `airFront` (the atmosphere loop's sums to the cloud's split) as the background for a sky pixel
that sits within one depth texel of a surface its own ray missed, gated on `cloudBlock < 0.98` so a bright
rim against a clear sky is untouched: overshoot 1.05x / +0.7 levels, 25/306 columns left (the right-hand end
against the lit horizon), and everything outside the silhouette band is bit-identical. A 10-90% width across
the whole cloud-to-ridge range goes 1.14 -> 7.36 px because the band that used to be brighter than both
neighbours now sits between them; the terrain step itself is one row, at the same row, before and after.

## History

2026-10-22 (same issue) fixed the half-res cloud's stair-steps along a ridge with a 5x5 joint-bilateral
depth weighting in `sat_sky.frag`. That made the composite read the cloud at the pixel's own surface depth,
which fixed the clear horizon showing through the missing deck - but the depth's silhouette is wider than a
5x5 (the march's coarse octaves stand up to ~3.5 texels proud), so the cut share kept leaking the open-sky
airlight, now white instead of orange. The 2026-10-31 change only replaces that leaked background; the cloud
data itself is not touched (a 9x9 depth-matched re-read was tried first: the resolved targets next to a
silhouette are contaminated by the resolve's 3x3, so it changes nothing).
