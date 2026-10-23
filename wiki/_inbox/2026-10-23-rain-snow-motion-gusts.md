---
pages: [rendering/clouds/weather.md, sound/ambience.md]
code: [src/simulations/SatelliteSimRain.cpp, shaders/rain_particles.vert, shaders/cloud_v2_lightning.comp, src/simulations/SatelliteSimAmbience.cpp, assets/sound/ambience/ambience.json]
---
## Now
Rain particles fall at their physical terminal speed: each particle carries a share of the rain's cross-section, so its
size is drawn from the AREA-weighted Marshall-Palmer distribution Gamma(3, Lambda), Lambda from the rate round the eye
(2.5 /mm at 10 mm/h, 1.7 at 60): visible drops fall ~4-7 m/s. A rain particle is redrawn elsewhere every "Drop
persistence (ms)" (`clouds_v2.drop_persistence_ms`, 60; 0 = never), each at its own phase with a fade, so every frame
holds unrelated streaks as a camera sees rain; flakes and diamond dust are followed for good and take 0.35 of the shutter.
The wind is the SURFACE wind at the eye (`surfaceWindAt`, a pure function of place and sim time): a mean wind at eye
height (median ~1.4 m/s, up to ~6; "Surface wind (x)" `surface_wind_gain` 1), the storm towers' outflow within 60 km
("Storm outflow (m/s)" `storm_outflow_mps` 8), the shower's outflow from the rain map, gust fronts ("Gust fronts"
`gust_fronts` 1: 1-2 s rise to +50-100% (at most +10 m/s), 15-40 deg turn, 5-20 s decay, ~every 2.5 min in calm air to
~every 30 s in a downpour by a storm) and turbulence ("Wind turbulence" `wind_turbulence` 1). Snow drifts at "Snow drift
(x surface wind)" (`snow_drift` 0.7). "Drop fall speed (x physical)" `drop_fall_speed` 1. The drops' sky light keeps the
zenith's hue only by the cloud column's diffuse transmission (at most 60%). Ambience: drivers `gust`, `surface_wind`,
`rain_driven`; layer `wind_gust` (wind synth, gain 0.25) swells with the gust; the rain layer's intensity reads
`rain_driven` (up to +60% in a gust).

## History
2026-10-23 (user: "too blue sometimes, extremely floaty and blowing straight sideways", with fall speed at 10x). Causes:
sizes from the NUMBER distribution above 0.5 mm at a fixed 5 mm/h (median 0.74 mm, 2.9 m/s: drizzle speed); each
particle stands for ~12 (near) to ~3000 (far) drops and moved as one lump, so the far levels (most particles) crept at
2-3 px a frame — raising fall speed only made faint long lines; the wind was 0.4 x the cloud wind aloft toward the east
plus a 9.5-14 m/s outflow from ANY shower (snow too, x1.2 for flakes): ~5 m/s on flakes falling ~1.5 m/s. Under a Cb the
drops' ambient was 0.35 x the zenith's blue (B/R 1.58 at tau 275; now 0.97, same luminance). Old keys `rain_fall_speed`,
`rain_wind_gain`, `rain_storm_wind_mps`, `rain_gusts`, `snow_wind` are not read. Cost unchanged (~0.35 ms rain, ~0.2 ms snow).
