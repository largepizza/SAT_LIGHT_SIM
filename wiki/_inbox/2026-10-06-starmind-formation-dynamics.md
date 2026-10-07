---
pages: [simulation/orbits.md, modding/constellations.md]
code: [src/simulations/SatelliteSim.cpp (buildOrbits), shaders/sat_orbit.comp (formationOffset), src/simulations/SatPhotometry.cpp (satOrbitStateAt), data/constellations.json, data/constellations.schema.json]
---
## Now
`cluster_shape: "ring"` is a passive formation: every member flies its cluster centre's circular orbit plus the
bounded, drift-free Clohessy-Wiltshire (Hill) relative orbit about it, in the centre's radial / along-track /
cross-track frame with theta = u + delta: x = (rho/2) cos theta, y = -rho sin theta, z = k x. Members are evenly
spaced in delta from a per-cluster start. A member below the centre moves faster and runs ahead, one above falls
behind, and the whole formation turns once per orbit. `cluster_radius_km` = rho (the along-track semi-axis;
radial amplitude rho/2). `cluster_relative_orbit`: `circular` (default; k = sqrt 3, a circle of radius rho tilted
60 deg out of the orbit plane about the along-track axis, every member rho from the centre), `projected_circular`
(k = 2, a circle seen from above) or `in_plane` (k = 0, the 2:1 ellipse in the orbit plane).
Shells constellations take a deterministic per-cluster scatter: `cluster_alt_jitter_km`, `cluster_phase_jitter`
(fraction of the gap between clusters), `cluster_raan_jitter_deg`, `cluster_incl_jitter_deg` (all default 0).
Starmind (both entries): rings of 8, rho 0.35 km (700 m across, neighbours 268 m apart; the formations are
reportedly under 750 m), circular; jitter 0.25 km / 0.25 / 0.15 deg / 0.03 deg.
GPU: the coefficients ride in GpuSatOrbit's tumble slots with a flag bit in constIdx (still 64 bytes); the CPU
evaluator mirrors it exactly, and magnitude traces carry `orbit_formation` / `orbit_formation_pqk`.

## History
The first cut (same day, f48804e) drew each ring as a rigid polygon in the orbit plane, 2 km radius, holding its
shape because every member was given its centre's mean motion at its own radius ("held together with magic",
the user). At 2 km each cluster resolved into a small ring of points from the ground. The user asked for the
real dynamics (lower members advancing, the ring rotating once per orbit), sub-750 m formations, and some
large-scale irregularity so the constellation stops drawing perfect lattices.
