---
pages: [rendering/satellite-meshes.md, rendering/sea.md]
code: [shaders/sat_mesh.frag, shaders/lightning.frag, shaders/cloud_march.comp, shaders/sat_sky.frag, src/simulations/SatelliteSimLaunches.cpp, tools/make_ship_models.py, tools/make_rocket_models.py]
---
## Now
Meshes take local lights: a burning engine (from just behind its nozzles) and, at night, each floodlight of a launch pad
(~2e4 cd, a 40-degree beam at the pad's centre) and of a landing ship (45 degrees at the deck), diffuse and specular, up
to 8 per frame (the most important to the camera), lifted at night and adapted like the engine lights on clouds. A
landing booster, a pad's tower and a droneship's deck are lit by them; the engine lights' screen-space ground term no
longer lights mesh pixels.

A booster landing on a ship's deck raises a thin exhaust skirt (not a dome of sea spray); a landing burn's trail is at
half the ascent trail's cross-section. The lit haze round a point light softens its core radius in quadrature, with no
flat disc.

Landing ships ride a swell: heave, pitch and roll from three sinusoids of 6-14 s, scaled by the sea state at the ship
(the drawn waves' rule with the cloud map's storms). The deck, the booster on it and the ship's lights move together.
The securing robot (`ship_robot`) drives out 75 s after touchdown and swings its clamp arms under the booster.

Grid fins are a square grid of 0.14-m cells, drawn solid at the grid's mean once a cell is under 3-6 pixels.

High plume billows are read in the parcel's own frame (Earth-fixed axes, in units of its width), so they no longer
rescale with the view. The sea's reflection drops texels near a flame only where the flame dominates them.

## History
2026-10-21, issue 20261020_213739: the white halo round a night droneship landing was the sea-spray plume (6x the
exhaust's mass) lit by the burn and the floods; earlier the deck glows were emissive Gaussians standing in for the
missing mesh lighting, and grid fins were a dithered speckle.
