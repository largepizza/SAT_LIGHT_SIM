---
pages: [simulation/kessler.md, development/performance.md]
code: [src/simulations/Kessler.h, src/simulations/Kessler.cpp, src/simulations/SatelliteSimKessler.cpp, src/simulations/SatelliteSim.h]
---
## Now
The cascade's event log keeps a 20-byte core per event. An engine collision's time, seed and spread are rebuilt from its
6-hour step and the pair it names, a hit by a small fragment's mass from the step and its target, and its speed is kept
in the core. Scripted events, the player's breakups, catastrophic collisions between two tracked objects (the tracker's
significant breakups) and anything that would not rebuild exactly are kept whole. The rest of an event (where it
happened, its dust and fragment counts) is kept for 31 days before the player's time and then dropped; going back
further than that already replays the cascade, which rebuilds it.

The debris pieces' entries in the satellite table are made from the pool record when they are read, instead of being
stored a third time.

For the default history (no player breakups) the app keeps, in the user cache, an archive of the small-fragment
re-entry streams and up to four world snapshots (the world's whole state at a 30-day mark, about 0.9-1.4 GB each). One
is written while the player waits for a catch-up. A later jump past a snapshot, or going back far enough to need a
replay, restarts the cascade from the latest snapshot at or before the player's time: the jump from day 100 to day 535
takes ~7 s instead of ~35-50 s. A jump to a date never visited before still replays from the CME (the supernova's day
170: ~24 s). Snapshots are not shipped: one is ~1 GB against the prebake's 45 MB. Setting `kessler.snapshots`.

Working set at day 1100 of the cascade: ~4.1 GB (was ~6.4 GB).

## History
2026-10-29 (workstream A, branch memcompact): the event log took 80 bytes an event (2.4 GB at day 1100), the pool was
held three times (the world, the mirror and the satellite table), and every jump or deep rewind replayed from the CME.
A 9-day detail window was tried first: it forced replays on 9-30-day rewinds the old build did not need.
