---
pages: [rendering/launch-visuals.md]
code: [src/simulations/SatelliteSimLaunches.cpp, src/simulations/Launches.cpp, shaders/cloud_march.comp, data/launches.json]
---
## Now
The upper stage's exhaust above ~100 km is the twilight "jellyfish": a bright nucleus at the stage and streaks fanning
out of it. Its parcels spread at 0.3 x the exhaust speed (the plume's divergence) and condense ~3x faster above
100-160 km; at night their sunlight is x5 above 70-130 km (the sim's night exposure is not physical); above 90-140 km
every exhaust Gaussian has a limb-brightened rim (a freely expanding parcel is a shell). Streaks: K polylines
(launches.json `upper_engines`, Starship 6; 4 filaments for a single nozzle) carrying 40% of the young vacuum exhaust
(to 240 s of age), knots at fixed ages behind the stage, offset by 0.45-0.8 of the plume's radius, 0.06 of it wide:
the interaction sheets between neighbouring nozzles' plumes. In the Gaussian budget vacuum Gaussians are weighted by
tau / (tau + 1e-3) and the young vacuum plume is always kept. Hot staging (`hot_staging`: Starship, Soyuz-2, Long
March 2F): 18 turbulent lobes round the interstage at separation, 0.4-1.4 km/s outward, 250-550 m wide growing to a few
km in ~30 s, orange-tinted (1, 0.78, 0.5), drawn with ~2x the billow contrast and a second octave (plume seeds 192-255).

## History
Issue 20261030_134020: parcels expanded isotropically at 2 km/s (~300 km wide after 3 min, optical depth ~2e-5) and the
young ones lost the 384-Gaussian budget to wide old bells, so the stage had no visible plume; there was no staging
cloud. Report view, RTX 3070 Ti 1920x1009: cloud_march 13.5 -> ~10 ms, launches CPU 2.24 -> 1.87 ms, composite at 128
registers.
