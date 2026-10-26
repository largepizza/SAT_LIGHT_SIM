---
pages: [rendering/clouds/weather.md, rendering/sky.md]
code: [src/simulations/SatelliteSimWeather.cpp, shaders/sat_sky.frag]
---
## Now
A tornado's funnel, wall cloud and debris cloud stand on the full DEM under the funnel (read once per ~500 m of its
track), within the procedural terrain detail's tens of metres of the drawn ground. Over the aerial-perspective volume's
horizon band cross-fade, the air in front of the clouds takes both the volume's and the per-pixel loop's share of the
shadowed air's sky light, as the sky colour does.

## History
Issue 20261025_184550. The funnel stood on the CPU's 18-km DEM copy: 105 m under the drawn ground in the report's
view, so its foot and half its debris cloud were buried and the debris' top ended at the eye's height, and the funnel
read as cut off along the horizon. The air split dropped the volume's share of the shadowed-air light over the band's
cross-fade: under a storm, two hard dark lines along the horizon at the band's edges, one through the funnel. Cost
within noise.
