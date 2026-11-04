---
pages: [simulation/kessler.md, sound/ambience.md]
code: [src/simulations/Kessler.cpp, src/simulations/SatelliteSimKessler.cpp, src/simulations/SatelliteSimAmbience.cpp, src/AmbientSynth.cpp, assets/sound/ambience/ambience.json]
---
## Now
A catastrophic pair collision keeps momentum exactly: each large piece keeps a share beta = 1 - 0.8 u^1.5 of its parent's
velocity relative to the pair's centre of mass (two streams along the relative velocity), then all records are shifted
together so their mass-weighted mean velocity is the centre of mass's; the small population (the density grid's
statistical orbits) does the same, and the dust cloud / the event's orbit moves at the centre-of-mass velocity
(kCacheVersion 20). Measured on a head-on Starmind pair (2500 kg each, 15.0 km/s): debris centroid 7502 m/s from the
target's orbit vs 7502 predicted.
Up close, the satellites a collision kills (its two bodies, ring neighbours within 200 km dying in that second) stay
whole in their own attitude until the exact impact time (the roster's death words are whole seconds; kesslerRosterHold).
The frame whose sim interval holds the impact, and the next, show the flash at its peak at any time scale. The
projectile's stand-in is selectable (a transient: "<name> - on a collision course, impact in x s").
Each collision drawn up close plays one hit on the new "impact" ambience layer (group space; synth "impact"): gain from
the CoM-frame kinetic energy and the distance ((1 + d / 2 km)^-0.75 in orbit), bass (a falling thump 30-80 Hz) from
energy and mass, a crack and a short FDN reverb up close; with the eye under 100 km a slow low-passed boom (~250 Hz)
with a long dark tail, distance scale 300 km.

## History
issue_20261030_180204 (2026-11-03). Before: the target and its neighbours went dead (tumbling, arrays swung white into
the Sun) up to a second before the impact; the dust and small fragments kept the target's velocity so a head-on hit's
cloud stayed with the formation; the 3-s flash was skipped at warps above ~x200; the stand-in was not selectable;
the flash used 500 kg for types without a mass (Starmind is 2500) and rode a relative-velocity vector from the
engine's sample time (up to 6 h of orbit off). Collisions had no sound.
