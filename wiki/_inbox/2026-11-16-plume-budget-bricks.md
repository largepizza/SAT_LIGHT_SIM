---
pages: [rendering/launches.md]
code: [src/simulations/SatelliteSimLaunches.cpp, shaders/plume_brick.comp, src/simulations/SatelliteSim.h]
---
## Now
The plume Gaussian budget is topped up by screen coverage: the best-ranked candidates are drawn until their cones
cover `launches.plume_cover_max` screens (key only, default 16; ~70 us of dustCloudsCS per screen on an RTX 3070 Ti
at 1600x900), never fewer than 384, leaving 256 list slots for the weather that fills after the launches; the room is
eased 3% a frame. Near a wide bell the old 384 already exceed it, so nothing changes there; far plumes gain room
(five launches in view: 382 -> 413 Gaussians). Plume bricks (the lobed staging-cloud volumes, 4 slots) go to the
hot-staging lobes and compact groups first, slot holders keep theirs, and each group's brick weight is eased (~1/3 s)
so a slot changing hands cross-fades. The hot-staging brick's envelope is read at a smoothly warped position (cells 0.4
of the brick, displacement 0.05 of it), so its outline bulges instead of being a sphere.

## History
Phase 4 of the plume overhaul (2026-11-16). Before: a fixed 384-Gaussian budget, bricks to the nearest groups (old
200-km exhaust took the slots: issue_20261116_175048, the staging cloud popped as the view orbited), and a round
billow outline (issue_20261116_175808). A pure coverage budget (no 384 floor) was rejected: it would cut near bells
the user had accepted. Log-density big lobes could not move the outline (the envelope falls too steeply at the rim).
