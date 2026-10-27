---
pages: [rendering/clouds/weather.md, development/profiling.md]
code: [src/simulations/SatelliteSimWeather.cpp, shaders/include/cv2_storms.glsl, shaders/include/terrain_detail.glsl]
---
## Now
The storm objects' spatial index lists each tower only in the cube-face bins its reach can actually touch, and each
anvil shield only in the bins its capsule (1.6 radii along the spine) can touch: every row of the bounding rectangle is cut
from both ends by a test on the bin's centre and chord circumradius (setting `clouds_v2.storm_index_tight`, default on;
exact). Tower bins stay 256 per face side (~39 km); the march's shield bins are 128 per face side (~78 km, the count in
`cv2ShieldHead.y`; tables 32768 / 131072 entries), while the weather bake's envelope index keeps 64. The per-frame listing
runs on the storm catalog's thread pool. The shield loop stops once no remaining listed shield can be stronger than the
best found, and the anvil's noise fetches are skipped past the outline's largest reach. In the Maritime Continent storm cluster the cloud march is ~0.8-1.1 ms cheaper
(15.6-16.0 -> 14.8-14.9 ms, RTX 3070 Ti, 1600x900, the user's High settings); other storm views are unchanged. The storm surge test
is skipped over the open sea past the water map's reach, where it has no effect: ~0.35 ms of the sky pass at sea level
under a hurricane. `clouds_v2.storm_shield_bins_ab 64` (no UI) lists the old shield bins for an A/B.

## History
Measured first with loop-doubling probes: the tower and shield loops cost ~0 in the hurricane, squall-line, supercell
and haboob views (0.6 ms at most) and ~1 / ~2.5 ms in the Maritime cluster; what storms cost elsewhere is their cloud
(Cb columns off: -6.7 ms from 12 km in a hurricane's eye; anvils off: -3.3..-4 ms). Tried and dropped: a per-tower
"far body" skip (exact, but +0.3 ms: it diverges within a ray group), runtime tower bin counts, 512 tower bins and 256
shield bins (no gain past the tight index; the 256-bin shield table overflowed). The weather bake at 1 h/s is 1.35-1.4 ms
a face (stamp 0.25, cyclone supersample 0.12, coverage fraction 0.15); nothing there was changed.
