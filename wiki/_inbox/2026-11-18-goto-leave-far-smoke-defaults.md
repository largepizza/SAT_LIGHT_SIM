---
pages: [using/controls.md, using/features.md, development/controls-and-settings.md]
code: [src/simulations/SatelliteSim.cpp (updateFollow), src/simulations/SatelliteSim.h (kFollowLeaveM, wildfireSmoke*), src/text/ui_text_en.inc (ToastLeftGoToFar)]
---
## Now
Go to ends on its own when the player flies more than 100 km (`kFollowLeaveM`) from the object it rides: the same
as pressing G (`stopFollowHere`), a free observer where the camera is, toast "Left Go to: over 100 km from it, you
stay here". Only the player's own WASD / Q-E / pad movement triggers it; a cinematic, Go to's flight in or out and
a harness `follow offset=` never do.

Wildfire smoke defaults (the user's look, 2026-11-18): `clouds_v2.wildfire_smoke` 1.1, `wildfire_smoke_brightness`
0.70, `wildfire_smoke_tint` -0.52, `wildfire_smoke_spectral` 0.72 (spread stays 1.0).

## History
Before 2026-11-18 Go to followed at any distance up to `kMaxObsHeightM`, and the smoke defaulted to 1.0 / 1.0 / 0.0 / 0.6.
