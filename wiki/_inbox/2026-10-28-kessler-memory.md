---
pages: [simulation/kessler.md, development/profiling.md, development/harness.md]
code: [src/simulations/Kessler.h, src/simulations/Kessler.cpp, src/simulations/SatelliteSimKessler.cpp]
---
## Now
The Sun's Fury keeps one copy of its event log: the world's `EventLog` (fixed-table segments that never move), which the
main thread reads through a view up to the count it has taken in. The rollback journal is capped at every step (1M
entries) and a checkpoint left more than a day behind the player is dropped. Generations of a pool slot that ended more
than 8 days before the player's time are dropped when the worker publishes (the mirror would only prune them), the
worker waits while more than 3M published pool changes are still to be mirrored, and the mirror drops small-fragment
streams and re-entry hours more than 30 days before now. Harness `kessler mem` reports the memory by structure. A jump
to the Betelgeuse supernova from the start replays ~170 days of the shipped prebake in ~32 s with a peak working set of
~4 GB; at three years the app holds ~6.4 GB (event log 2.4 GB, streams 0.7 GB, the pool three times ~1.5 GB).

## History
Until 2026-10-28 the journal grew without bound whenever the player's time jumped past the frontier (10.7 GB during the
supernova jump), the main thread held a second copy of every event (2.4 GB at three years), and a replay's publish left
~600 MB of change capacity behind: peaks of 17-23 GB, 10 GB at three years. Before / after: supernova Watch 79 -> 32 s,
peak 17.6 -> 4.1 GB; 0 -> 1100 days 720 -> 260 s, peak 22.7 -> 6.4 GB. Fingerprints identical at seven dates.
