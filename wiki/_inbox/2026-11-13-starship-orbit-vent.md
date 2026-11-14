---
pages: [rendering/launch-visuals.md]
code: [src/simulations/SatelliteSimLaunches.cpp, src/simulations/Launches.h, data/launches.json]
---
## Now
Starship's upper stage vents the whole time it is in orbit: from SECO to its deorbit burn, and again from the burn's end
until just before the entry interface. From the ground at twilight it reads as a fan of sunlit ice opening from the
ship, as in photos of real Starlink deployments. The vent is drawn each frame, not built as exhaust parcels: 17 knots
at ages up to 360 s behind the ship, each the gas vented then out of the aft end and coasted ballistically to now,
joined by one Gaussian per chord and lit like every plume. launches.json per vehicle: `orbit_vent_kg_s` (Starship 5),
`orbit_vent_m_s` (250), `orbit_vent_spread_m_s` (80, fan half-angle ~ atan(spread / speed)); 0 = no vent.
Settings > Launches "Starship orbital vent (x)" (settings.json `launches.orbit_vent`, 0-4, default 0.29, 0 = off) scales
its density.

## History
Before 2026-11-13 a Starship vented only once, a 3-minute controlled dump 20 min before its deorbit burn, so a
deployment seen from the ground was a bare moving dot. 1.5 kg/s was invisible from the ground; 15 kg/s matches the
user's photo but was a white wall from a few km away, so the default became 5 kg/s on 2026-11-14 (the slider's 3x is
the photo). The user then tuned the slider to 0.29 (with plume opacity 1.0), the default since 2026-11-14. Gas older than 360 s is not drawn: vented retrograde it falls ~200 km and curls back within 15 min, a
lobe the photos do not show, and the widest Gaussians cost the most. Cost with the fan filling a 40-degree view from
440 km (at 15 kg/s): GPU +0.7 ms (cloud composite), CPU +0.06 ms (RTX 3070 Ti, 1600x900).
