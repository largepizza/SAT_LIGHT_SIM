---
pages: [simulation/wildfires.md]
code: [shaders/smoke_layer.comp]
---
## Now
The wildfire smoke layer's pass (smoke_layer.comp) moves the sky pass's air split toward the haze by the haze's
share of the texel's opacity. The haze past the texel's cloud distance counts toward that split's distance and toward
the haze's opacity only as far as it is seen: each step's weight behind the cloud is multiplied by the cloud's
transmittance and the haze's in front, and the haze opacity is 1 - Tf (1 - aB (1 - Tb)) (1 - Tf Tb with no cloud, so
a clear texel is unchanged). An opaque cloud's split stays with the haze in front of it.

## History
2026-11-18 (issue_20261118_210334, "odd horizon haze, clipping on clouds"): the hidden haze behind a cloud counted
whole. Behind a cloud the ray ends at the sea below the horizon (~110 km from 1 km up) but at the layer's top above it
(hundreds of km), so the split and the air in front of the cloud jumped at the horizon: a hard-edged pale band across
any cloud over the horizon. Step across the horizon row at the report's view: -14 to -24 levels -> -1.5 to +4.0 (the
layer off: -1.3 to +4.9, the cloud's own). Cloud texels with haze behind them read slightly darker (+0.4-0.9 levels
mean past the floor at the 2026-11-17 smoke views); pass cost unchanged.
