---
pages: [simulation/wildfires.md, rendering/launch-visuals.md, development/harness.md]
code: [shaders/cloud_march.comp, src/simulations/SatelliteSimWildfire.cpp, src/simulations/SatelliteSim.h]
---
## Now
Wildfire smoke Gaussians wide enough for the Earth's curve to matter (drop over 3 horizontal sigma > 0.15 of their
thickness: palls, long trail pieces) are evaluated on the curved Earth: height above the sphere through the centre plus
the tangent-plane offset, integrated by 8-point Gauss-Legendre over the exact ray intervals inside the sheet's shell and
horizontal footprint (`smokeCurvedInt`). Only rays within 1-2x the sheet's tilt of grazing, or an eye between the flat and
curved sheet, take it (blended with the flat closed form); the per-Gaussian data is computed on the CPU in double and read
once per 16x16 tile. Billow contrast falls as 1/sqrt of the path through a Gaussian over 1.5x its smallest sigma. Each
front section is drawn as a chain of 1, 2 or 4 sub-pieces along the actual front (192 front samples; 2x2 covariances
interpolated between knots), sharing the segment's piece budget with the nodes along the trail. Pixel-size floors use the
Gaussian's nearest range. Smoke and rocket plumes take the Moon's shadow in a solar eclipse (sunlight and sky light).
Debug: `clouds_v2.wildfire_debug` 32 = no pall, 64 = no section chains; harness `fires` -> `smoke_gaussians`.

## History
Issue 20261029_084627 ("Haze during eclipse"): at the 2037 totality from 11.7 km, a pall drawn as one flat 400x290 km
Gaussian tangent 528 km away stood ~20 km up near the eye (tau 11 along the view); the sky was streaked white haze and the
smoke was lit by the un-eclipsed Sun. From 79 km a 22-day fire's front was five straight 50-60 km bars. It replaced the
per-ray "move the flat Gaussian down by the drop" patch (faded out with the eye near the sheet, one crossing only), one
straight Gaussian per section (sigma 1.7x the section rms) and floors at the centre's range (pall 0.9 -> 3.3 km thick).
Composite stays at 128 registers only if the curved branch leaves q, sq and o flat and keeps no second moment (249
otherwise). Measured (RTX 3070 Ti, 1920x1009, alternating HEAD/fix runs): report shots 3.5/5.4 -> 3.7/5.5 ms, over a big
fire from 26 km 21.9 -> 22.3 ms, beside a fire on the ground 6.0 -> 6.4-6.9 ms.
