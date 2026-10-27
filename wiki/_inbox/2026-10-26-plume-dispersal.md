---
pages: [rendering/launch-visuals.md]
code: [src/simulations/Launches.cpp, src/simulations/Launches.h, src/simulations/SatelliteSimLaunches.cpp]
---
## Now
Rocket exhaust parcels fade out over the last 60% of their life (2400 s below 40 km, 3600 s above): their condensed
share is multiplied by 1 - smoothstep(0.4, 1, age / max age), so nothing is left when a parcel ends. Old exhaust keeps
widening with age by a Richardson term, d(sigma)/dt += 0.18 sigma^(1/3) (m/s, half that above 12-20 km, none in the
bells above 60-90 km), x "Plume growth" x `launches.plume_dispersal` (settings key only, default 1, 0 = off): a low
trail is ~1 km wide after 10 min and ~5 km at the end, thinner as it widens (same cross-section). A burn's knots are
thinned continuously by width (every 2nd .. 64th, a knot sliding onto its neighbours' midpoint before it is dropped);
ground-cloud puffs are never dropped. Each launch's reserved share of the 384-Gaussian budget is eased and fades at its
edge, so another launch starting to draw does not cut older plumes.

## History
Issue 20261026_210053 (2026-10-26): a launch's bell seen from 53.6 km over the Gulf went out piece by piece between
T+62 and T+65 min. The budget was not binding (310 of 384); the parcels hit their fixed lifetime with 0.2-0.5 of their
optical depth left. Frame-to-frame change in the plume crop at 10x: max 1.29 -> 0.47 (now only at video keyframes).
The bells' widening was tried and dropped: a grey veil over the sky, +2.7 ms of cloud composite. Ground puffs were
drawn x8-16 from 20 s of age by accident (the step decimation's factor without the drop); kept, now continuous.
Cost: twilight Starship bookmark cloud_march 7.9 -> 8.3-9.2 ms at dispersal 1 (7.8 at 0).
