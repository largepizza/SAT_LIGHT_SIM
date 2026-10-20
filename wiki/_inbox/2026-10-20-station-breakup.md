---
pages: ["new: simulation/kessler.md", "new: simulation/launches.md", using/features.md, development/harness.md]
code: [src/simulations/SatelliteSimKessler.cpp, src/simulations/Kessler.cpp, src/simulations/SatelliteSimLaunches.cpp, src/simulations/SatelliteSimMissions.cpp, src/simulations/SatelliteSim.cpp]
---
## Now
**A destroyed satellite is gone everywhere.** Each roster satellite has a fate at any sim time, read from the Kessler
event log: working, dead (tumbling), destroyed (broken up) or re-entered. A destroyed or re-entered satellite cannot be
selected, and one that is selected when it goes is dropped. The Missions window's crewed-station row reads
"Destroyed <date>; no more crew flights" (or Dead / Re-entered), with Select and Go to disabled.

**A lost station gets no more crews.** A crew launch is not scheduled when its station is dead or destroyed at or
before lift-off (`launch why` reason "station lost"). A flight already launched shows "rendezvous called off" in the
Ongoing tab. Going back in time before the breakup brings the flights back: the schedule is a function of sim time and
the event log.

**Dust clouds are a tube along the orbit.** The fragments' spread follows the Clohessy-Wiltshire solution for velocity
kicks at the breakup. The cloud is cut into pieces along the orbit. Each piece holds the fragments at that point,
with their radial offset and spread, and a cross-track width that depends on how far those fragments have travelled
since the breakup. So the cloud pinches through the breakup point and the point opposite it, as real debris clouds
do. The pieces are spaced finely enough that each stays inside the tube as the arc curves. The spacing changes
smoothly with age, so the cloud stretches into an arc without jumps. Its lumps move and stretch with the fragments.

**Big parents.** An explosion's fragment count scales as (mass / 2 t)^0.75 above 2 t. The ISS now makes ~13,000
pieces over 10 cm and ~330 over 1 m, where it made 239 and 7. A big cloud's brightness grows as the square root of
its dust past ~5 t, so a station's cloud no longer blows out the view at the default dust brightness.

**Harness.** `kessler state` -> `dust` (Gaussians drawn, summed light, ranges, sizes); `missions state` ->
`stations[].fate`.

## History
2026-10-20 (issue 20261019_231126, "Space Station Explosion"). Before this, an exploded station stayed selectable from
the Missions window, its crew flights kept flying, and its dust cloud was up to 12 straight Gaussians ~2000 km long.
Their count was a step function of the cloud's age, so the cloud's width jumped ~50% when it went from one to two,
and the lumps (hashed per piece) reshuffled at each step. Seen from orbit near the arc these were straight crossing
"beams" that stretched and glitched as time ran.
