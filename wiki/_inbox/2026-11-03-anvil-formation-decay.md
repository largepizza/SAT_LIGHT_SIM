---
pages: [rendering/clouds/storms.md, simulation/weather.md]
code: [src/simulations/Weather.cpp, src/simulations/SatelliteSimWeather.cpp, shaders/include/cv2_storms.glsl, shaders/include/clouds_v2.glsl]
---
## Now
A storm's anvil spreads over the "Anvil formation time" (`clouds_v2.storm_anvil_form_min`, default 60 min) from the
moment its dominant tower reaches the anvil (strength 0.42), and lingers after the last tower falls back for 1-2x the
"Anvil decay time" (`clouds_v2.storm_anvil_decay_min`, default 70 min, an air-mass storm's; multicells x 45/35,
supercells x 60/35). Both the shield's strength and the tower's flared head ("spread") follow the ramp g (0.6 + 0.4 g),
which is near-linear over the strengths the shader shows (presence = smoothstep(0.05, 0.6, strength)): presence goes
10 -> 90% over 0.385 of the formation time. The spread widens the head from the tower's own width (anvilK 1 / flare)
and scales the anvil plate under it; it rides in the GPU tower's c.z (anvilK + 8 x round((1 - spread) x 1000)).
Squall lines / MCSs keep their own hours-long ramps; tower strength, life and lightning are unchanged.

## History
Until 2026-11-03 (issue 20261030_210137, "anvils pop in and out" at 5 min/s) the shield rose over a fixed 15-min
smoothstep and lingered 35-70 min (air mass): presence 10 -> 90% in 3.8 sim min, 95% of it inside one 5-min step. The
larger pop was the tower head's plate switching on with the tower's last 0.42 -> 0.6 of strength (~3 sim min). Measured
at the report's view after the change: presence rise 21.7 min, decay 43 min (medians), largest 5-min step 22% of peak.
Slowing the constants alone (45 / 70 min, smoothstep) left the plate's pop unchanged.
