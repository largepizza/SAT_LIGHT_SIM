---
pages: [rendering/atmosphere-and-sky.md, rendering/clouds/weather.md]
code: [shaders/cloud_march.comp, shaders/sat_sky.frag]
---
## Now
- **The air in front of a cloud is split at the plumes in front of it, for every plume** (cloud_march.comp `smokeAir`):
  a tornado, a haboob's far-field Gaussians, smoke and any launch exhaust no engine light reaches (`dustCloudsCS`
  `tauAir`) pull the alpha's distance (sat_sky.frag's air split) toward their own distance by their opacity in front of
  the cloud. Engine-lit exhaust is left out (the engine-light pass would light it a second time as cloud).
- **The piece of a sky step in front of the split reaches the eye through the air to its own middle** (sat_sky.frag,
  `aFK`), not to the step's middle. Not in SKY_LITE.
- **A smoke / dust sheet follows the Earth's curve by being moved down** by the ground's drop below its tangent plane
  at the ray's flat closest approach (d^2 / 2R) and evaluated again; from above a pall shows its full column at any
  distance, at the limb its far part lies on the ground.

## History
- Issue 20261027_001835 (tornadoes / haboobs "still transparent", a dark horizon line through them). Two causes, both
  in the composite, neither in the plumes themselves: (1) a tornado in front of rain, fog or the desert dust haze (any
  resolved v2 opacity) took the air split at that cloud's mean distance, tens of km out, so all the horizon haze in
  front of it was added over the funnel x (1 - T) with T ~ 0; only smoke had been pulled. Funnel on/off (displayed
  luminance, core) 1.13 -> 0.75; the front haze over the funnel 0.98 -> 0.53 of its funnel-off value. (2) the air in
  front of a near split (1-3 km) took the camera attenuation to the middle of a 16-24 km step: darker toward the
  horizon, a step at it (the ray's step shortens where it meets the ground) — the dark line, drawn over T ~ 0 walls.
  Haboob shot: dip of 13 levels + 11-level step -> flat; it now matches a 256-sample, no-AP-volume reference within
  1 level. A storm-cloud bookmark (Venezuela rainbow) moved 10 levels mean toward the reference (old vs ref 11.9,
  new vs ref 2.6): every near cloud's front haze was too dark; cloud bases read hazier, as the reference does.
- Issue 20261027_002826 ("yellow circles around storms", 534 km): wildfire palls (sigma ~350-490 km) cut off at
  ~150-200 km by the 2026-10-25 thinning (hS^2 - hP^2 used the whole ground drop for a near-vertical ray). Now a
  smooth veil; orbit cloud bucket +0.7 ms (more of the palls evaluated), elsewhere within noise. A thin brown smoke
  line now hugs the limb where a pall reaches it (physical: the sheet seen edge-on).
