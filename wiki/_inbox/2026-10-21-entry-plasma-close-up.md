---
pages: [rendering/launch-visuals.md, simulation/crew-flights.md, simulation/rockets.md, development/harness.md]
code: [src/simulations/SatelliteSimLaunches.cpp, src/simulations/SatelliteSim.cpp, data/launches.json]
---
## Now
Entry plasma close up is one emitter for Starship's ship, the capsules and Dream Chaser: a thin shock layer on the
windward face (a capsule's heat shield, radius `heat_shield_r_m`; the ship's and Dream Chaser's belly), a short hot
wake behind the body (0.12 of the layer's brightness) and the path wake (0.15). Unresolved, each Gaussian carries the
point's on-screen energy; resolved (1.5 -> 8 px) it is a surface brightness, ~4x a sunlit white face at peak heating,
held at its face-on value from any view and capped at 2.0 of displayed light (x the sky exposure, so night needs no
extra factor). The plasma lights its own hull through physical mesh local lights (not lifted at night, not adapted):
three under the ship's belly, two under Dream Chaser's, one ahead of a capsule's shield. Dream Chaser flies its entry
40 deg nose-high (eased in over the 10 min before the entry interface, out over the 3 min before the glide); its
glow sits on the belly (`belly_half_len_m` 4.5, `heat_shield_m` 9.65, `heat_shield_r_m` 1.5). Go to / Return place the
default offset (-4r, 0, 1.5r) behind the object's motion over the ground (or a slow rocket's engine end). Harness:
`launch ship day=1`, `launch state` -> `entry_plasma[]`, `observer.follow.cam_along_motion_m` / `default_side_dot`.

## History
The capsule sheath was a 3.5-m round Gaussian in the point's units (~10 mag over a surface): from 40 m a saturated
disc several capsules wide; the ship's belly sheath the same as a white ellipse, its hull a dark silhouette (its only
mesh light had a 160-m soft radius). Dream Chaser's glow sat on its body axis with zero angle of attack. After a
booster's boostback the carried follow heading points against the motion, and Return landed the camera 150 m in
front (now 150 m behind). mission_ops: 1286 PASS, 0 FAIL / WARN.
