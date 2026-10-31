---
pages: [rendering/clouds/field.md, rendering/clouds/storms.md]
code: [shaders/include/clouds_v2.glsl]
---
## Now
`cv2MidSigmaS(q, fpM, stormK, ...)` takes the storm strength (`CV2Col.storm`) and *lifts* the mid layer with it;
`cv2MidSigma(q, fpM, ...)` is the `stormK = 0` wrapper the far shader (over 16 km eye height) still calls. In the
storm's inner region (`smoothstep(0.35, 0.85, col.storm)`) the mid-level moisture regime is driven to the settings'
own maximum (`max(regimeMap, stormK x "Mid layer amount" x smoothstep(0, 0.35, covM))`), the coverage the layer
sees is floored at 0.6 x stormK and the stratiform weight at 0.25 x stormK, so the storm's air carries both
altocumulus and altostratus. The mid layer gives way only over the towers' own footprint (`col.prox` normalised by
its storm-strength factor), not over the whole storm region.

Measured at a 40-km storm view from the report's 6666 m: the mid layer's own alpha in the band beside the tower
0.005 -> 0.36, the share of that band with any mid cloud 5.7% -> 45%, the tower's silhouette contrast 26.7 -> 25.1
levels (17.3% -> 21.3% relative) and the GPU +1.38 ms at 1920x1009. The report's own view (no storm in range) is
unchanged to 0.07 levels. The storm view's auto-exposure drops 1.1 EV because a sunlit mid deck now fills half the
frame.

Cost shape, worth knowing before retuning: the mid layer's march cost follows the AREA it covers and whether it is
OPAQUE. A translucent veil costs more than a deck, because the march goes on through it and lights the whole low
column behind it while a deck ends the march (`T < 0.01`): the same view at 0.35 of the lift measured +6.2 ms
against the deck's +1.1 ms, and `mid_amount` 0.75 / 0.4 / 0.15 measured 11.26 / 11.36 / 10.09 ms of cloud march.

## History
2026-10-31, issue_20261022_180823 ("Storm Coverage": stormy areas should have higher densities of mid layer
clouds). A storm stood in air with *less* mid cloud than the calm sky beside it: `cv2Field` multiplied the whole
mid layer by `1 - smoothstep(0, 0.15, col.storm)`, an erasure wherever a tower can stand (it was there to keep a
thin lens from slicing each tower with a flat plate at ~6 km), and the storm stamp adds only low-layer coverage, so
a storm could lose mid cloud and never gain any. The issue had already been fixed once, in 9b070ca (2026-10-22),
whose constants were too weak at these settings (+0.07 on the regime, ~0.06 of movement) and whose whole hunk was
reverted by the next commit, 1f25e9b ("Clouds flicker fix", a full-res sky-TAA change with no mention of the mid
layer). A first attempt at the full storm region cost +6.3 ms because the layer's decaying tail (col.storm -> 0 at
the tower's reach) is what a grazing ray runs along for a hundred km; the inner region is the cheap end of it.
