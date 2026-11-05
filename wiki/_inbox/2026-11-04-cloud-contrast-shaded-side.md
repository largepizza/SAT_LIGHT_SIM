---
pages: [rendering/clouds/march.md]
code: [shaders/cloud_v2_march.comp, src/simulations/SatelliteSimCloudsV2.cpp, tools/cloud_micro_reference.py]
---
## Now
The diffuse field's side-escape factor e^(-k t) (t the delta-scaled optical depth toward the Sun) is a setting:
"Multi-scatter side escape" (`clouds_v2.ms_side_escape`, default 0.11, range 0-0.4). 0.11 is fitted with
`tools/cloud_micro_reference.py --contrast` (Sun at right angles to the view) so a lobed cloud's shaded half matches
the path tracer (0.90-1.22 reference / model; at 0.05 it was 1.5-2.2x too bright). "Sky light keeps its colour"
(`clouds_v2.sky_light_hue`, default 1) keeps the sky light's hue inside cloud (cloud scattering is grey); 0 greys it
by 1 / (1 + 0.11 tau_above) as before. UBO `cv2.shade` (x hue, y side escape). No cost (two uniforms, 128 registers).

## History
2026-11-04 (cloud contrast): the shaded side of towers read light grey against photos of deep blue-grey. In a squall
line from 14 km (Sun 21 deg) display p95/p10 of opaque cloud 3.8 -> 5.4, sunlit tops -2%; fair-weather fields
unchanged (their flatness is shape and haze). The tonemap compresses ~28:1 scene to ~4:1 display ("Tonemap contrast"
1.15); 1.35 would read 8.3 but darkens lit tops 7% - left to the user. Tried and dropped: splitting the sky dome into
Sun / anti-Sun halves (this sky model's halves differ by only ~5%; it pushed the march to 249 registers).
