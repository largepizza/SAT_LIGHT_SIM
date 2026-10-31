---
pages: [rendering/clouds/technique.md, rendering/clouds/field.md]
code: [shaders/include/cloud_lightning.glsl]
---
## Now
The Sun transmittance volume's four levels fade in on a RADIAL norm about the point under the eye: `length(cc)` in
cells, `smoothstep(52.0, 62.0, ...)` of it, instead of the Chebyshev `max(|cc.x|, |cc.y|)` over `48 .. 56`. The
radii keep the reach's area (`pi 62^2 = 12 076` cells^2 against the old square's `(2 x 56)^2 = 12 544`, 3.7% less)
and 62 plus the four-corner read stays inside the 64-cell half-window, so no slot is read past its wrap. The read
itself is unchanged: four cell corners bilinearly, the two height knots linearly, the levels cross-faded.

Why it matters: the level's reach was a SQUARE in the lattice plane anchored to the eye, so the shadow's
resolution (level 0's 250 m cells inside, level 1's 1 km outside) stepped across two straight lines 12-14 km from
the nadir, one of them close to screen-horizontal because the lattice's `a` axis is horizontal and its `b` axis
lies along the Sun. An observer flying at 30 km saw a box of shading travel with them. Measured on the level-0
region at the report's view: the handover contour's longest straight run 327 px -> 94 px, the region's area
389 413 -> 380 558 px, and the whole frame's mean absolute change 0.089 display levels (p99 0.86, 154 pixels over
10 levels, all inside the moved contour). The shadow's own strength is untouched: the sea's p5/p50 0.903 -> 0.902,
the cloud deck's mean 0.5901 -> 0.5899 and its std 0.0239 -> 0.0238; no measurable cost.

## History
2026-10-31, issue_20261025_190008 ("Intense ripple on clouds shadows"). The report's own words are about
animation at 1x; the "hard axis-aligned edges" and the "square from orbit" that the triage brief carried come from
the two siblings, and neither is this volume: issue_20261022_175215's block edge was the cloud detail cache's
trilinear blend (fixed in 9b070ca), and issue_20261029_174506's square is the cloud-over-ground map `cv2Vt`
(fixed in 77e2cec). Instrumenting the read (the share of the four corner reads that stamped valid, packed into the
ground shadow and read back through debug view 50) showed 99.5% of sea pixels reading the full four-corner weight,
so the "one constant per cell" premise was wrong: the defect left in this path was the SHAPE of the handover, not
filtering. The sea's concentric bands that the brief attributed to the level steps survive `sun_volume 0` (the
whole volume off) and `godrays 0`; they are the far-sea ripple's distance-octave banding in `sat_sky.frag`
(review 24/27), a separate subsystem, and the reporter's 1x "odd animation" also reads as the sea's wave/glint
flicker rather than the shadow.
