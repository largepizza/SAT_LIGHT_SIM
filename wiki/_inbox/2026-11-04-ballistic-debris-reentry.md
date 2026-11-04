---
pages: [simulation/index.md]
code: [src/simulations/Kessler.cpp, src/simulations/SatelliteSimKessler.cpp]
---
## Now
A Kessler debris record whose orbit's perigee is at or below 120 km (a ballistic piece, typical of a head-on pair
collision's slower fragments) dies where its Kepler arc next crosses 100 km on the way down
(`World::deathTime` -> `ballisticEntryDelay`, closed form), and its re-entry fireball starts there and then. From a
980-km breakup a piece with 0.5 / 2 / 4 / 6.9 km/s of horizontal speed re-enters 8.0 / 8.3 / 9.5 / 25 min later,
230 / 940 / 2160 / 9900 km downrange. kCacheVersion 21.

## History
Until 2026-11-04 (issue_20261104_015233 "Odd mirrored debris?") `decayTime` returned 0 for these orbits, so every
ballistic piece died at the breakup second and its fireball was drawn at 100 km directly under the breakup along its
own heading: a head-on collision's two sides burst out of one point in opposite directions (58 simultaneous
re-entries from one 980-km collision, heads within 8 km of each other).
