---
pages: [sound/ambience.md]
code: [src/AmbientSynth.cpp, src/simulations/SatelliteSimAmbience.cpp, src/simulations/SatelliteSimLaunches.cpp, assets/sound/ambience/ambience.json]
---
## Now
A lightning channel within 8 km sends one strike as its thunder arrives: a snap (bright noise, under 1 ms up), a
crackle (sharp ticks thinning over half a second) and a boom (a sine falling 75 -> 34 Hz with low noise); the snap
fades fastest with distance, the boom carries to ~8 km, beyond that the roll alone is the thunder. A loud roll is
scaled by its own peak instead of being clamped, so a close strike is a bang that falls away rather than seconds of
flat noise. Rockets play on their own layer (`rocket`), heard from any height: each burning stage has its roar
through the air (delayed by the distance, at most 3 s by default; louder and bassier up close) and a far rumble of
the stage as it burns now (the collisions' distance law, to 2000 km). Ignition, staging and the booster's relights
are thumps on the collisions' synth: a bang through the air nearby, a collision-like low boom from far off.

## History
Until 2026-11-21 a strike within ~3 km was 6-10 s of noise held at the limiter (issue_20261121_163317: "does not have
the low frequency and snap crackle"), and rockets shared the thunder layer: silent above 25 km and beyond ~100 km,
with one "punch" for ignition and nothing at staging (issue_20261121_164722). Measured for a Starship lift-off (master
0.4): the roar at 300 m -21.8 -> -15.0 dBFS RMS, under 80 Hz -22.1 -> -15.3 dBFS; at 100 km -55.2 -> -36.6 dBFS (centroid
54 Hz).
