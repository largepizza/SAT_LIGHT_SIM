---
pages: [rendering/points-bloom-glare.md]
code: [shaders/glare_cluster.comp, shaders/glare.vert, shaders/include/glare.glsl]
---
## Now
The glare cluster grid (`glare_cluster.comp`) weighs a source with `glareClusterU(s) = 2^(4 clamp(s, 0, 3))`
and `glare.vert` reads its OWN weight back out of that grid to keep `w = q_self / (q_self + others)` of its
spikes. The cancellation is exact only if the grid computes the same `s` the sprite does, and it did not: the
grid clamped the response `b = 0.5 log2(flare)` at **4.0** while `glare.vert` clamps at **8.0**. `u`'s own
`s` cap of 3 is reached at `b = 3 + threshold = 4.765`, just above the grid's clamp, so for every source
brighter than `flare 2^(2*4.765) = 739` the grid stored up to **8.3x less** weight than the sprite read back:
`others = sum - q_self` went negative, clamped to 0, `w = 1`, and **the brightest flares kept every spike in
any crowd** - the artifact the grid exists to remove. The grid's clamp is now 8.0, matching `glare.vert`
(commit in the c21 cluster of wave 20261030b, issue_20261019_222938).

Why it hides: a lone source has `others = 0` and `w = 1` either way, and everything a pad draws is capped at
flare 14 (`launchFillPadLights`) and every mesh glint path already clamped at 4.0 on both sides, so the
reported "Tower lights" view is byte-identical before and after. The class that changes is a crowd of
genuinely bright things - sun glints, burns, re-entries: two equal `flare 1e6` sources 20 px apart now keep
25% of their spikes each where they kept 100%; six keep 2.8%.

Measured at the reported view (1920x1009, `report load` shot 1): the six pad flood lights of the "Tower
lights" frame are merged (spike field 2021 px removed by the merge, isolated control 0 px), the tower box's
mean luminance 92.68 (reporter, build d9c4e08) -> 50.22 with cores still saturated, and the isolated pad
flood 322 px away keeps all ten rays.

## History
2026-10-20 (`6f75b9d`): the cluster grid, `glareSpikeWeight`, `photometry.glare_cluster_merge`; pad floods
-4 -> -3 mag and flare capped at 14 so a mast's cluster reads as one glow. That commit is what fixed the
reported shot; the grid's weight clamp stayed behind at 4.0.
2026-09-24: the spike pattern (`include/glare.glsl`) replaced the sun corona's angular noise.
