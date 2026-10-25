---
pages: [simulation/kessler.md]
code: [src/simulations/Kessler.cpp, src/simulations/SatelliteSimKesslerTracker.cpp]
---
## Now
An engine collision between two objects happens where the engine found them together, carried to the event's own time:
the engine samples each 6-h step once (its middle) and spreads each event's time through the step; at the event's time
the projectile is placed on the target with the relative velocity it had at the sample time, turned into the target's
local frame at the event time. Every fragment of a breakup starts at the breakup's recorded place, where its dust cloud
is drawn and where the Missions window's breakup Watch looks.

## History
Until 2026-10-24 the projectile's fragments were built from where the projectile was at the spread time, 350-13000 km
from the target (measured over 16 engine collisions); half a breakup's debris was far from its dust cloud and from the
Watch view, which had worked round it by moving the eye to a piece. kCacheVersion 17.
