---
pages: [simulation/weather.md, development/harness.md]
code: [shaders/lightning.frag, shaders/lightning.vert, src/simulations/SatelliteSimLightning.cpp]
---
## Now
A channel piece under the cloud's base, seen by an eye that is also under that base, has only rain and terrain between
it and the eye. It skips the cloud composite's depth (heavy rain at 4.8 /km is opaque within ~150 m), is hidden by
terrain, sea and meshes through the scene depth, and keeps at least 35% of its light (`lightning color rain_floor=`,
BoltColour wet.w); its glow is the soft, neutral "wet" glow. Above 30x time scale every listed flash stays drawn for
0.3 s of wall time (sim time / scale), as at its first return stroke, fading as 0.8 x (1 - (t/0.3)^2). 1x and 10x are
unchanged. Lighting terrain with a flash is not built: the engine lights' ground term (lightning.frag mode 2) or a beam
light-volume box per stroke are the two routes (weather.md "What lighting the terrain would take").

## History
2026-11-24 issue_20261124_192804: shot 2's stroke 18 km out drew 1,937 px through rain, now 40,957 (43,323 with nothing
in front); peak luminance 0.30 -> 0.68 of 0.96. Frames showing a channel within 60 km over 10 s at 60x 21% -> 98%,
at 300x 18% -> 99.7%.
