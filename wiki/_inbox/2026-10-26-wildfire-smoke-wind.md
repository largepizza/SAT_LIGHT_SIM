---
pages: ["new: simulation/wildfires.md", rendering/atmosphere-and-sky.md]
code: [src/simulations/SatelliteSimWildfire.cpp, src/simulations/Wildfire.cpp, shaders/include/smoke.glsl, shaders/cloud_march.comp, shaders/include/cloud_occlusion.glsl]
---
## Now
Wildfire smoke follows trajectories: every parcel has a fixed emission time on a nested grid (fine near the fire,
geometric down the trail), leaves the fire as it was then (its front's sections, injection height, activity) and is
advected through the wind field (`wx::windAt`) at its own position, height and time on a fixed schedule of ages (steps
0.15 x the age, 20-900 s). Trajectories are cached and only extended, so they are a pure function of the emission time:
a change of wind at the fire bends only the smoke emitted after it, and time warp or reverse play show the same trail.
The pall (the smoke older than the trail, up to ~2 days) is the mass-weighted mean of a few long trajectories. Smoke is
grey: fresh a little darker, aged paler and cooler, lit by a blue sky, its thin parts blue-grey (its scattering is as
spectral as its extinction); brown-orange only toward a low Sun. Settings (Weather window > Controls > WILDFIRES):
"Wildfire smoke" `clouds_v2.wildfire_smoke` 1 (optical depth), "Smoke brightness" `wildfire_smoke_brightness` 1, "Smoke
tint" `wildfire_smoke_tint` 0 (-1 blue-grey .. +1 brown), "Smoke spread" `wildfire_smoke_spread` 1 (width, same mass),
"Smoke spectral" `wildfire_smoke_spectral` 0.6 (x the Angstrom exponent ~1.6; 0 = grey at all wavelengths).

## History
2026-10-26, issues 20261026_204045 / _204239. The trail used to be re-walked from the fire every frame with each stretch's
wind taken at the time of its age: the whole trail rode on the fire's newest wind and swung as one body at 5 min/s, and
the pall was placed by the wind now x up to 18 h. The smoke was beige: warm tints, a brown-grey sky light and a reddening
of the light leaving the smoke. Far clouds behind smoke showed as light-blue shapes: the air split in front of them was
pulled toward the smoke's mean distance (for a wide pall, past the cloud) and split linearly in distance; it is now
capped at 0.6 of the cloud's distance and solved in air-brightness space.
