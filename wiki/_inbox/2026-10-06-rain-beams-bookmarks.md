---
pages: [rendering/clouds/weather.md, rendering/clouds/march.md, simulation/reflectors.md, using/features.md]
code: [src/simulations/SatelliteSim.h, src/simulations/SatelliteSimRain.cpp, src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSim.cpp, shaders/rain_particles.vert, shaders/cloud_v2_march.comp, data/bookmarks/bookmarks.json, tools/harness/scripts/default_bookmarks.satcmd]
---
## Now
- Rain defaults: "Rain" (`clouds_v2.rain_amount`) 3.0; "Rain fall speed (x)" (`clouds_v2.rain_fall_speed`) 5, range
  0.3-10. Ice particles (snowflakes, diamond dust) fall on their own phase (`cv2.snowMotion`) at that factor capped at
  1.5, so snow still drifts down at ~1-2 m/s.
- The Reflect beams' light on precipitation shafts follows the shafts' temperature (the air 500 m over the ground
  under the eye, `cv2.precip.w` = its liquid share): liquid rain shows the bow opposite the satellites, snow takes the
  snow phase (broad lobes, a faint 22-degree halo and sundogs around the satellites, no bow).
- Standing in a beam, the rain/snow particles at the eye are lit by it too (CPU evaluates the beams' irradiance at the
  eye from the cloud-light list: `cv2.beamEye`, `beamEyeDir`), through the particles' own phase: drops show the bow,
  flakes and ice crystals glint around the satellites. Harness `state` -> `clouds_v2.rain.eye_temp_c`,
  `shaft_temp_c`, `beam_eye_irr_suns`, `beam_eye_dir_enu`.
- Six default bookmarks: Great Australian Eclipse, Blood Moon from Anchorage (2037-01-31 14:10 UTC, Moon ~29 deg up
  in the WSW over the city), Solar Storm (Kp 6.8 aurora from 420 km), Beam-lit Snow over Antarctica (McMurdo, mid-April
  twilight, -23 C, standing in the converging Reflect beams), Rainbow over Venezuela, Starmind Formation Rings.

## History
- The user's "Satbow over Antartica" bookmark (McMurdo, 2037-04-15) showed a vivid rainbow opposite the Reflect
  satellites: `beamFinish` lit every precipitation shaft with the rain phase whatever the temperature (the Sun/Moon
  path already switched to snow below freezing, review 5/6). The sim's temperature there is -22.5 C at the eye and
  -23.3 C in the shafts' air, so it is snow and the bow is gone; the bookmark became "Beam-lit Snow over Antarctica".
  Light pillars from the beams' ground spots (plates reflecting the lit snow below) are not modelled.
- Removed defaults: Reflect Orbital over Anchorage, AI Satellites at Twilight, Los Angeles by Night, Sunset from
  Orbit, Blood Moon over Alice Springs. Renamed: Aurora from Orbit -> Solar Storm, Venezuela Rainbow -> Rainbow over
  Venezuela.
