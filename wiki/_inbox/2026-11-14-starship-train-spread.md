---
pages: [rendering/launch-visuals.md]
code: [src/simulations/SatelliteSimLaunches.cpp, src/simulations/Launches.h, data/launches.json]
---
## Now
A Starship train disperses after deployment. Each member leaves the door with the common push (0.7 m/s, mostly up)
plus a retrograde along-track push graded by release order: launches.json `train_spread_mps` (Starship 0.7, other
vehicles 0) for the first out, falling linearly to 0 for the last. An earlier member is always drifting ahead faster,
so the train moves ahead of the ship in release order and its gaps grow with time (~3 x the per-member step x age),
with no member overtaking another. A Starmind train (8) is ~10 km long with 1.3-1.6 km gaps an hour after deployment.
From the ground it is an evenly spaced string of dots leading the venting ship.

## History
Before 2026-11-14 every member left with the same push, so all flew one trajectory shifted in time and stayed
30-90 m apart for their whole free flight: 8 Starminds were ~250 m end to end, one blob from the ground, unlike the
user's photo of a well spread train ahead of the ship. Closest approach of two members (`launch trace`
`min_member_gap_after_60s_m`): 36 m (was 32; gate 20 m). The ship never comes within 1.4 km after an orbit (unchanged).
