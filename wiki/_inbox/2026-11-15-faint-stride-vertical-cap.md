---
pages: [rendering/clouds/march.md]
code: [shaders/cloud_v2_march.comp]
---
## Now
A faint fine sample (optical depth over the light march's length under `CV2_FAINT_OD` 0.2) stands for two steps
(`CV2_FAINT_STRIDE` 2) only while the doubled step spans at most 200 m of altitude (`kMaxVertStep`, the cap every step
already has): `CV2_FAINT_STRIDE x stepF x |dz| <= kMaxVertStep`. Grazing rays through haze (inside a mid layer, along
an overcast) keep the stride and its saving; rays looking steeply down through a cumulus field take single steps. The
"Cheap samples" list on the march page does not mention the faint stride at all: add it, with this limit and why.

Why: a stride skips the position after the faint sample. Where that position is a thin, dense sheet (a cumulus skirt
under its faint margin), the sheet is met at half the rate, and an opaque sheet that is missed is not made up for by the
haze sample's doubled weight (hit rate, not weight). Measured at issue_20261115_015822 (7.7 km, el -71, density 0.22):
mean cloud transmittance 174 levels with the unlimited stride, 156 without, 156 with the limit; at density 0.5, 28 / 15 / 15.
Cloud march cost of the limit (RTX 3070 Ti, 1920x1009): +0.24-0.44 ms looking down at scattered cumulus, +0.1-0.35 ms
from inside the mid layer at Brisbane, 0 from orbit (issue_20261109_184851 shot 5).

## History
Until 2026-11-15 the faint stride had no vertical limit. At the low densities of the 2026-11-14/15 looks (0.22, 0.5) the
thin skirts of scattered cumulus seen from above were thinned and see-through along their outlines: dark contour lines
on the sea round every cloud, visible once the view had settled (issue_20261115_015822, cluster c03 "faintcontour").
The faint lighting skip was suspected first and cleared by A/B.
