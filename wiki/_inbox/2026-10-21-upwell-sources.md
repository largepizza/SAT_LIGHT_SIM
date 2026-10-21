---
pages: [sound/music.md, sound/ambience.md]
code: [src/simulations/SatelliteSimAmbience.cpp, src/simulations/Ambience.cpp, assets/sound/ambience/ambience.json]
---
## Now
The track's upwell stem answers more than glaring satellites: burning rocket engines, sunlit rocket stages close by,
re-entry fireballs and Kessler collision dust clouds raise it too. ambience.json's `music_upwell` lists `sources`
(one piecewise map per driver) combined by `max` (or `soft_sum`). The new drivers `rocket_mag`, `stage_mag`,
`fireball_mag`, `dust_mag` are equivalent apparent magnitudes at the camera of each kind, counting a source when it
is on screen or near (8 / 1.5 / 15 / 20 km full, none past 6x), so a launch watched from the pad or followed lifts the
stem fully whichever way the camera looks, and a faint distant dot does nothing. Slews (2 s up, 6 s down) and the
intro rule unchanged.

## History
Issue 20261021_002542 "Upwells". Measured: Starship from 12 km 0.97 at ignition, 1.0 by T+40; following a Falcon 9
1.0; a re-entry from 120 km 0.4 -> 0.94; a Starlink breakup overhead 0.84; a day-21 cascade night sky 0.
