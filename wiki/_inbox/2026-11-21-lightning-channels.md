---
pages: [rendering/clouds/weather.md, development/harness.md, development/tools.md]
code: [src/simulations/LightningBolt.cpp, src/simulations/LightningBolt.h, src/simulations/SatelliteSimLightning.cpp, shaders/bolt_ground.comp, shaders/cloud_v2_lightning.comp, tools/bolt_tool/main.cpp]
---
## Now
A lightning flash's channel is a pure function of the flash's own record: its seed and id, its kind, the cloud's
base and top and the strike point, in the flash's Earth-fixed frame (the flash list carries the origin and the strike
point as an Earth-fixed direction + altitude, `CV2Flash.e` / `.f`, 96 B a record). Nothing about the observer enters
it, so the same flash at the same sim instant is the same tree from every viewpoint and in every run (harness
`lightning` -> `trees[].hash`). The tree is built once at full detail (pieces of 5-20 m, at most ~9,000 vertices) as
paths of midpoint displacement whose vertices carry their subdivision level; the draw picks, per path and per frame,
the level whose pieces are ~2.5 px at that path's distance from the eye, so level of detail never changes the shape.
The channel is fitted to the drawn ground (DEM + procedural detail, evaluated on the GPU by `bolt_ground.comp` and
read back two frames later): a ground stroke ends on the ground, branches that reach it end there, streamers are
rooted on it, and near-level sheets keep 120 m over relief. `BoltTool` (`cmake --build build --target BoltTool`)
runs the generator without the app: `stats`, `dump`, and `tools/bolt_tool/sheet.py` for a contact sheet.

## History
Until 2026-11-21 the tree was built for the distance and field of view the flash's origin was first seen at (pieces
of 3 px there, one random stream) and cached, so another viewpoint, zoom or session gave another bolt for the same
flash, and a spider first seen with its origin 23 km away drew an arm passing 1 km from the eye in ~140-m straight
pieces (issue_20261121_163317). Sheets' paths were displaced freely in 3D: a 30-km arm's midpoint moved kilometres
up or down, through the ground. The strike point sat on the weather cube's ground (smoothed over ~20 km).
Measured: the report's spider has one hash and identical Earth-fixed vertices from 1.1, 23, 33 and 298 km.
