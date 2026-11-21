---
pages: [rendering/launch-visuals.md]
code: [src/simulations/SatelliteSimLaunches.cpp]
---
## Now
The jellyfish's streaks (launchFillVisuals) share out the exhaust's cross-section between their knots by age. A parcel
stands for the exhaust of its emission interval (ages [age, age + dt]); the exhaust emitted since the youngest parcel
(ages 0 .. tau - tY) is the part of the NEXT parcel's interval already emitted (its history is built ahead), so the
streaks always reach the engines while the stage burns. Each overlap of a parcel's interval with a streak segment reads
the parcel's history at those ages, its condensed share averaged over the emission phase (the parcel standing for age a
was born 0 .. dt before it), so a segment's light does not depend on where in the ~1 s parcel cycle the frame falls.

## History
Until 2026-11-21 (issue_20261121_120035 "Blinking rocket exhaust", a Starship upper stage at 349 km seen from ~0.5 km
beside it): the young segments had no cross-section once the youngest parcel was older than them, so the streaks left
the nozzle for ~0.5 s after each birth and started ~2.5 km behind it for the rest; up close their near part spans most
of the frame, and the chevron flashed at 1 Hz (the parcel rate). Knockouts singled them out (plume coverage budget,
plume bricks and engine light off: no change; streaks off: the 1-Hz steps gone). Measured (RTX 3070 Ti, 1600x900, the
reporter's path): plume-box largest one-frame change 10.7% -> 2.3%, on camera-steady frames 6.6% -> 0.4% and the range
within a window 20.6% -> 1.7%; 1-Hz power of the frame change 1.19 -> 0.53, 2-Hz harmonic 4.4x -> 1.0x its local floor.
Light at the old cycle's mean (shot-1 view: 204 vs the old 189-218 cycle, mean 201). GPU and launches CPU within noise.
A first cut that read the condensed share at the overlap's own age (no phase average) held the chevron but +8 levels
brighter than any phase of the old one (the old read lagged the gas's age by 0 .. dt).
