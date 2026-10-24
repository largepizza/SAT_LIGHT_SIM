---
pages: [rendering/atmosphere-and-sky.md, rendering/clouds/weather.md, rendering/clouds/field.md, sound/ambience.md, development/harness.md]
code: [shaders/sat_sky.frag, shaders/cloud_v2_march.comp, src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSimWeather.cpp, shaders/cloud_v2_lightning.comp, shaders/include/clouds_v2.glsl, src/simulations/SatelliteSimAmbience.cpp, assets/sound/ambience/ambience.json]
---
## Now
The sky's single-scattering loop integrates each step's Rayleigh, Mie and "Haze near clouds" density exactly for a
height linear over each half step (start, middle, end), so the sky has no step-count structure: the step count still
grows with the path's length ("View samples (min)" 6.5 ~ 15-km steps looking up), but the result no longer changes when
it does.

The weather cube is re-baked a face per frame at ordinary speeds; in time warp (once six frames of sim time pass a face's
due time, 45 s with storms stamped) all six faces are baked together every frame, at one sim time.

A tornado's funnel hangs from the cloud base actually drawn over it and is lit through the storm's column: the lightning
pass probes up to four funnel columns (the first height with cloud > 1e-3 /m, and the optical depth above it); the funnel
runs 250 m into that base with the wall cloud just under it, and takes the light the cloud march gives that base (the
sky's light through the column's diffuse transmission 1 / (1 + 0.11 tau), plus 0.35 x (0.3 + 0.7 x it) from below), grey
under a storm. Its roar is the ambience layer `tornado_roar` (a low, churning freight-train rumble, broader and brighter
close, debris hiss within ~1 km) on the driver `tornado` = per funnel sqrt(width / 150 m) x lifecycle / (1 + (d / 2 km)^2).
Offline: -36 dBFS RMS at 1 km (the whole ambience -35), -39 at 3 km, -48 at 12 km.

An anvil's underside is lit from below: a high sheet (4-9 km up and above) takes 0.6 of the unshadowed Sun's light
reflected by what is under it (albedo 0.15 -> 0.3, cloud tops), since it sees the ground and clouds far out round it,
mostly outside its own shadow; an anvil-reaching tower's flared head measures its height share from the overhang
(zeta 0.7), so its underside is lit from below like a cloud base. Seen from below an anvil reads mid grey, about as
bright as the shadowed tower walls beside it.

Harness: `state` -> `weather.tornado_list` (base_agl_m, base_probed, tau_above, light_k) and `weather.cube` (rebakes,
all_six_bakes, face_lag_s); `clouds_v2.weather_one_face` (no UI) restores the one-face schedule as an A/B.

## History
Issues 20261023_230542 ("Rings in the sky"), 20261023_224032 ("Weather system updates choppy"), 20261023_223907
("Tornado oddities"). The sky took each density at the step's midpoint: looking up, the Mie (1.2 km scale height) and the
haze (2.5 km) were off by up to ~50x and jumped each time the step count changed, every few degrees from the zenith:
concentric rings, gone with 32 view samples (the A/B that found it); the zenith is now a little hazier, the horizon
unchanged. The cube baked one face a frame: at 1 h/s each face was up to ~13 min behind, each at its own moment, and past
a 15-min step face 0 alone was re-baked every frame while the other five froze. The funnel hung from the Cb type's base
(hundreds of metres under the drawn cloud) and was lit as under a clear sky (white under a dark base). Issue
20261023_230636 ("Anvil darkness"): the bounce used the key light shadowed AT the sample (the Sun transmittance
volume), 0 under any anvil, and the flared head took the tower's whole zeta as its height share: an anvil seen from
below was near black (0.02 linear against 0.10 for the shadowed tower walls).
