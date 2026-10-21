---
pages: [rendering/sea.md, simulation/launches.md]
code: [shaders/sat_sky.frag, src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
The sea mirrors every engine, floodlight and ship light (the engine-light list that also lights clouds and haze) from where it is: sat_sky.frag, after the Sun glint, evaluates the same Beckmann microfacet lobe with the light's direction taken from the sea point and its irradiance I / (d^2 + r0^2) there, dimmed by the clouds the eye sees in front of the light. A light near the eye therefore mirrors at its true specular point: for a source at height H, horizontal distance D, seen from an eye at height h, the mirror point lies D h / (h + H) from the eye, i.e. at a depression of atan((H + h) / D), always deeper below the horizon than the source is above it. The reflected cloud composite (screen-space, as if at infinity) drops emissive-only texels (flames, lit exhaust) whose light misses the sea point's reflected ray by more than 2-6 degrees; real clouds are unchanged. Satellite ocean glints keep the infinite-distance geometry (under 1 degree off at hundreds of km). Cost within run-to-run noise.

## History
Issue 20261020_181339 "Rocket Ocean Flare": from 4.9 km up, 3 km from a Starship at T+46 s (0.9 km above the eye), a glitter path ran from the horizon down under the flame. It was the cloud composite's flame Gaussians reflected at the mirrored direction; the true mirror point was 74 degrees below the horizon (out of frame), not 17. The bloom ellipse under the flame and the faint column to the pad are the lit exhaust, not reflections.
