---
pages: [rendering/clouds/wildfires.md, rendering/clouds/weather.md]
code: [shaders/sat_sky.frag]
---
## Now
A wildfire's burn scar (`fireScarAt` in `sat_sky.frag`, filled from `SatelliteSimWildfire.cpp`
`weatherFillFireParams`) is no longer one smooth ellipse with a footprint-thin rim. Its perimeter wanders with
the fire's own unburnt fingers (2.6 km and 0.8 km value-noise cells, amplitude up to ~0.15 of the local radius,
scaled with the radius so it holds at any scar size), the last ~1 km inside the perimeter is a partially burnt
mosaic instead of a step, and the interior carries km-scale unburnt patches on top of the old fine ash mottling.
The char colour, the scar's centre and its covered area are unchanged; the smoke path (`smoke.glsl`,
`cloud_march.comp`) is not touched.

Why: from 26 km a 5-day-to-2-week-old fire's scar (this one ~1130 km2 of ground, 27-63 km away) drew as a
single hard-edged black oval - at the report's view 634 x 355 px with a 0.2-km rim - and the reporter read it as
"smoke effects still spawn oversized oval particles" (issue_20261028_192133). Measured at that view, probing the
reported oval's own rim at 720 angles (the luminance step 5 px inside vs 5 px outside): mean 5.42 -> 2.71 levels
(-50%), max 19.03 -> 13.85, p95 14.19 -> 8.43, the share of the rim above 10 levels 16.8% -> 2.6%, above 15
2.9% -> 0%. The perimeter's relative deviation from a circle 0.45 -> 0.63. Over the same 59,282 px core, the
scar's darkening varies by 8.9 levels (p5-p95) at the 2-km scale, up from 4.3 (+108%), while the 2-px speckle
falls 4.19 -> 1.19 (-72%); mean darkening 11.39 -> 11.84 levels and 89% of the old oval's core is still dark.
The scene with `clouds_v2.wildfire_scars false` is identical before and after (mean |diff| 0.05/255), so the
change is confined to this function.

Open: a burnt edge is still an edge - at the scar's *new* perimeter the 5-px luminance step is as large as
before (mean 6.2, max 17.5, 16.5% of angles over 10). It is now the edge of a ragged mosaic of charred and
unburnt ground rather than the rim of an ellipse, and the 2-km unburnt islands are smooth value-noise blobs
(no fractal detail below 0.8 km beyond the ash octave). The thin smoke trail that crosses the scar's upper-left
rim is unchanged, including where it brightens the rim in the report's shot.

## History
2026-10-24: burn scars were added (dark charred ellipse with grey-white ash and a few unburnt islands, the
active arc burning at its edge). 2026-10-31: the ellipse became a ragged mosaic (issue_20261028_192133).
