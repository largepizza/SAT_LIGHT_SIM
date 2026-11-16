---
pages: [rendering/launches.md, "new: rendering/noctilucent-clouds.md"]
code: [src/simulations/Nlc.h, src/simulations/SatelliteSimNLC.cpp, shaders/nlc_shell.comp, src/simulations/SatelliteSimLaunches.cpp, tools/nlc_model.py]
---
## Now
Noctilucent (polar mesospheric) clouds form at 83 km wherever the air is colder than the frost point (Murphy & Koop 2005)
of its water vapour: a 5 ppmv background plus every launch's exhaust water of the last 9 days (the upper stage's burn
at 80-115 km x flow x the propellant's water share; ~60 t for a Starship), drifting toward the summer pole at
35 m/s, spreading (K 4e5 m^2/s) and photolysed (3 days). The temperature is a climatology by latitude and season
(summer pole ~140 K, 41 N summer ~169 K, equator 190 K) plus tides and 8 K rms gravity waves. Ice = the vapour above
saturation, freeze-dried from 8 km of air into a ~2.5 km layer, 100 particles/cm^3 (20-120 nm), lit with the plume ice
Mie table. A 512x256 map is rebuilt each sim minute on a worker; shaders/nlc_shell.comp adds it onto the cloud composite.
The layer is fibrous: bands, billows and ~1.6-km fibres from the clouds' baked Perlin volume, domain-warped, read in
a transverse chart (no pole pinch), as temperature waves and a corrugation of the ice (launches.nlc_fibres, 1).
Old high trails (72-106 km, 2-10 min) flatten to 1.2 km deep. Setting launches.nlc (true). Harness `state` -> nlc reports
the model's ice chance at the launch sites and downwind (e.g. 5 Jul 2036, 70 N 95 W: 74% of the day vs 53% natural;
Starbase: never). Cost ~0 from the ground, +0.1 ms from orbit.

## History
Requested as "a layer that drifts slowly away from launch sites" (issue_20261115_165612); the user chose the physics:
the launch sites' mesopause (176-206 K) is far above the loaded frost point (~150-160 K), so the clouds appear where
the water goes (poleward into the summer mesopause), as the shuttle's plumes did (Stevens et al. 2003, 2005).
