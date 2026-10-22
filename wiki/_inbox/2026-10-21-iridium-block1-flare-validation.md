---
pages: [accuracy/results.md, simulation/photometry.md, accuracy/benchmarks.md]
code: [data/satellite_models/iridium_block1.json, src/simulations/SatModel.cpp, tools/sat_model_tool/pass_eval.cpp, tools/video/vidkit/passes.py]
---
## Now
`data/satellite_models/iridium_block1.json` is a validation model of the first-generation Iridium satellite. It is not in the
roster, but it runs in the accuracy gate's model selftest. Each of its three Main Mission Antennas is 1.88 x 0.86 m, with its
normal 50 deg from nadir. The front MMA faces the inertial velocity and the other two sit at +-120 deg. The MMA reflectance has
two parts: a near-mirror core (F0 0.88, GGX alpha 0.0005) and a haze holding 0.3 of the specular reflectance (alpha 0.04,
`calibrated`), a material haze (`"haze"`, see 2026-10-21-photometry-sun-disc-haze-extinction.md). Until that existed the
panel was split by area into a 70% core strip and a 30% haze strip.

The haze was fitted to Ron Lee's and Randy John's 1997 visual magnitude-vs-mirror-angle data (SKYSAT iridrpt.txt). Without it,
the tail at 0.6-12 deg reads about 3 mag too faint. An MMT-9 light curve held out of the fit confirms it: the wing of an
IRIDIUM 13 flare, 2015-06-05, at mirror angles 7-14 deg, matches within 0.5 mag. The mirror-only lobe is 1.5-1.7 mag too faint
there.

The model was compared with 12 Heavens-Above predictions from 2000, using TLEs of the time. Geometry: elevation and azimuth
agree within 0.5 deg and the distance to the centre line within ~3 km rms. How fast the magnitude drops away from the centre
agrees within 0.4 mag rms. The model's centre magnitude is ~0.9 mag fainter than Heavens-Above's, whose -9 at ~830 km is
brighter than any 1.62 m^2 flat mirror can make.

The occlusion selftest p95 is 0.239, close to the 0.25 gate.

## History
Two pipeline limits were found.
1. (Fixed 2026-10-21: the Sun's disc is now an anisotropic source term.) Folding the Sun's disc into the GGX half-vector
   lobe (SUN_ALPHA) conserves the total energy, but it does not give the disc's shape:
   - the peak is 1/cos(theta_i) too bright (+0.26 mag at 50 deg incidence, 0.2 mag faint at normal incidence against the
     limb-darkened disc centre);
   - the beam is cos(theta_i) too narrow across the plane of incidence;
   - the core's half-maximum radius is 0.17 deg against the real disc's 0.26 deg, with long wings.
2. (Fixed 2026-10-21.) `vidkit.passes.pass_spec` places the observer on WGS84, but the pass tool's extinction takes |r| - 6371 km as the
   observer's height. At 54 N that put the observer 7 km below the sphere and gave 35 mag of extinction; at the MMT site it
   added ~0.2 mag. This affects the earlier Starlink MMT replays.

The predictors' attitude frame (x exactly along the velocity) differs from our TRIAD (z exactly nadir) by up to 0.08 deg. That
moves the flare peak by 0.3-0.5 s.
