---
pages: [simulation/tropical-cyclones.md, rendering/clouds/storms.md]
code: [src/simulations/SatelliteSimWeather.cpp, src/simulations/Weather.cpp, shaders/include/clouds_v2.glsl]
---
## Now
A hurricane's 18 eyewall tower objects (Weather.cpp cycloneTowersAt, role 1) give way to the storm mass's own eyewall by
the share "Hurricane eyewall" x "Storm mass" x the eye's grown share. The march draws what is left of them at reduced
strength, and caps them at 0.8 of the Cb's height (`kCycWallCap`, the march copy's height cap). Their tops stand ~3 km under
the central dense overcast's lid, with no flared head and no overshooting swell. From orbit the overcast is unbroken round the eye.
From inside the eye or below, the towers are still in the wall. The weather bake's copy of the towers is unchanged, so their
rain, radar stamp and lightning stay.

## History
Until 2026-11-15 (issue_20261115_154546 "Hurricane eyewall cloud poofs"), only the strength was faded, and the height
saturates at strength 0.6. At a partial share (the reporter's eyewall 0.56 x storm mass 0.62) the ring's heads reached the
lid and swelled through it: four bright 20-40-km mounds round the eye from 69 km. At the shot, GPU total went from 27.2 to 24.6 ms.
