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

Shapes. A path's roughness follows the scale of its pieces (long sweeps gentle, 30-400 m steps sharp, the last metres
soft). A ground stroke's stepped-leader branches are placed stratified along the descent below the cloud (4-13 x the
square root of "Lightning tendrils", `clouds_v2.lightning_tendrils`, default 1.23: 9 on average), leave the channel
downward 14-48 degrees off a direction between the channel's trend and the vertical, run up to 3 km, end in the air,
fork again and carry fine tendrils, tapering and dimming by order. Spider lightning is a forking sheet: 3-6 arms that
shed side branches and fork at their ends into two or three shorter arms; an in-cloud flash joins two such sheets
1.2-4.5 km apart by a channel. A tree holds at most ~9,000 vertices; six bolts at their first stroke draw in 0.11 ms.

## History
Until 2026-11-21 the tree was built for the distance and field of view the flash's origin was first seen at (pieces
of 3 px there, one random stream) and cached, so another viewpoint, zoom or session gave another bolt for the same
flash, and a spider first seen with its origin 23 km away drew an arm passing 1 km from the eye in ~140-m straight
pieces (issue_20261121_163317). Sheets' paths were displaced freely in 3D: a 30-km arm's midpoint moved kilometres
up or down, through the ground. The strike point sat on the weather cube's ground (smoothed over ~20 km).
Measured: the report's spider has one hash and identical Earth-fixed vertices from 1.1, 23, 33 and 298 km (8,656 /
2,044 / 2,042 / 352 drawn pieces). The shapes of 2026-10-05 drew branches per metre of parent from the top with one
vertex budget (bushy clumps near the cloud, a bare lower channel) and sheets as 3-6 unforked arms; the draw's pieces
at 6 and 25 km fell from 10.4-12.0k to 2.4-6.2k a ground stroke (0.12 -> 0.11 ms for six bolts).
