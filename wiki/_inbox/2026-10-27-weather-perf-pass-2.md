---
pages: [rendering/clouds/weather.md, development/profiling.md]
code: [src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSimWeather.cpp, shaders/cloud_v2_weather.comp]
---
## Now
In time warp (the all-six bake path) a weather-cube face in view bakes its levels of 128+ texels a side (0-4) only over
the rectangle the eye can see: its cap of the eye's horizon + 12 deg projected onto the face (padded; coarser levels two
texels wider). Levels under 128 texels, and faces whose rectangle covers over half of them, bake whole. The rest of an
in-view face is due at the out-of-view rate and shares the frame's single full-face bake with the faces out of view.
Outside time warp (slice bakes) nothing changes. Settings without UI: `clouds_v2.weather_rect_bake` (true),
`weather_rect_levels` (5), `weather_rect_max_share` (0.5), `weather_rect_margin_deg` (12).
In time warp `updateStorms` also runs at the top of the frame, as soon as sim time is final. The bake's envelope storm
index is then built on its own worker threads and joined before the frame's uploads (`clouds_v2.storm_env_async`, true).
Harness `state` -> `weather.cube` reports `rect_bake`, `rect_bakes` and `env_job_ms`.

## History
Measured on an RTX 3070 Ti at 1600x900, at 1 h/s near a hurricane. Bake GPU time per frame went from 3.38 to 1.70 ms
from 12 km, 2.79 to 1.89 ms from sea level and 5.78 to 4.26 ms from 420 km. The look stayed within the noise floor of
A/A pairs. The main thread's `record_compute` went from ~5.3 to ~4.6 ms.
Tried and dropped:
- Starting the index job where the bake is decided: it overlapped too little of the frame.
- Writing the storm index tables slot by slot: no gain.
- Prewarming the next year's cyclone seasons: no measured effect.
