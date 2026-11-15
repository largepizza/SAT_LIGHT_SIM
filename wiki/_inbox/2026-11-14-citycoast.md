---
pages: [rendering/cities.md]
code: [shaders/sat_sky.frag]
---
## Now
Under the sky TAA the city lights decide land or sea at the pixel's UNJITTERED point, like the rest of their inputs
(the "Unjittered inputs" rule in cities.md, which lists the night map, the terrain contour and the pattern). Past the
terrain march that is the water map at the unjittered uv; inside it, the march's own height/water test (`tdDemAt`) at
the unjittered point, run only where the water map's signed shore distance is within ~2 km + the jitter offset of it.
A pixel whose unjittered point is lit city land at night never takes the sea branch; a pixel whose unjittered point is
water draws no city lights. Cost: +0.05 ms of the sky pass at an orbit shot, +0.07 ms over Norfolk VA from 3 km
(RTX 3070 Ti). Planetarium (SKY_LITE, no TAA) is unchanged.

## History
issue_20261114_162442 (2026-11-14): coastal city lights flickered from orbit ("z-fighting near the coast"). The pixel's
sea/land branch used the jittered sample, so a light within half a pixel of the shore was on in some jitter phases and
replaced by the sea in others; on a sea sample the TAA clips the history (sim time running) and caps a lone bright
sample (moving). Mean |frame-to-frame diff| of lit coast pixels at the shot: 3.6 -> 0.04 (time frozen), 6.2 -> 1.2
(time running), inland 0.16 / 1.05. Running the march test on every city pixel within ~15 km of water cost +0.3 ms
at Norfolk (a launch coast patch is loaded along the US east coast), hence the shore-distance prefilter.
