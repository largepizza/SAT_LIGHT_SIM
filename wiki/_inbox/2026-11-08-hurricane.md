---
pages: [simulation/tropical-cyclones.md, rendering/clouds/weather.md]
code: [shaders/include/clouds_v2.glsl, src/simulations/SatelliteSimWeather.cpp, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
A hurricane's central dense overcast is one storm mass in the low cloud field reaching up to its tropopause top, under
the ice sheet of the high layer. Its eyewall is that mass too: within about one radius of maximum wind of the eye the
mass rises 4 km higher, every cell is a tower, and the eye is cut out as a stadium that widens upward (0.85 to 1.2 eye
radii) with fluted, bulging walls. The ring of eyewall tower objects still exists for rain, lightning and the weather
bake but is no longer drawn. The sheet's outline follows the spiral arms out (to ~1.45 CDO radii), is notched between
them and frays into thin spiral streaks at its edge; its top carries a low (+-140 m) spiral ripple. Settings:
"Hurricane sheet swirl" (`clouds_v2.hurricane_sheet_swirl`, 1) and "Hurricane eyewall" (`clouds_v2.hurricane_eyewall`, 1).

## History
2026-11-08 (issue_20261108_173906): the CDO's interior was flat mesas at 4-7 km with ~7 km of clear air below the
sheet, because the storm-cumulus targets and an early-out height bound written before the storm mass existed capped
the mass at the cumulus / storm-cumulus top. The eyewall was 18 tower objects standing in the hole as separate
cylinders; the sheet was a round disc with an even texture.
