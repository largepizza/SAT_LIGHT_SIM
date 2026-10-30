---
pages: [simulation/kessler.md]
code: [src/simulations/Kessler.cpp, src/simulations/SatelliteSimKessler.cpp, src/simulations/SatelliteSimLaunches.cpp, src/simulations/SatelliteSim.cpp]
---
## Now
A catastrophic pair collision's fragments keep a share beta = 1 - 0.8 u^1.5 (0.2..1, mean 0.68, hashed per piece) of
their parent's velocity relative to the pair's centre of mass, plus the ejecta kick: the debris spreads along the
relative velocity, each parent's pieces running on its own way, momentum kept (kCacheVersion 19; the shipped prebake
is still version 18 until rebuilt).

A pair collision near the eye is drawn as one: the projectile appears on its real approach line (the target's state +
the relative velocity in the target's radial / along / normal frame x (t - te)) for the last 45 s, as a mesh in its own
reserved launch chunk and a point while unresolved, with the projectile's own attitude state; at the impact a flash
(the smaller body's kinetic energy x 1e-3 over ~0.2 s, capped) and an emissive fireball expanding at 0.15 of the
closing speed for 3 s. The world is unchanged (render only). Setting `kessler.collision_closeup` (no UI) is the A/B.

The dust cloud starts at 0.2 s of spread and, resolved, shows its surface brightness (pi x radiance per unit solar
irradiance x "Dust cloud brightness"), not the point-source units: from close up it is a faint veil, not a white
screen.

## History
Issue 20261029_213225 (2026-10-29): "satellites basically just explode and pretend to collide"; the projectile's
record was thousands of km away at the spread event time, and the 10-29 first cut sent every fragment 0.8 of the way
to the centre of mass (one cloud). From 790 m the young dust cloud at x30 filled the view in grey-white.
