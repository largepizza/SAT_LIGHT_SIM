---
pages: [rendering/clouds/march.md, development/tools.md]
code: [shaders/cloud_v2_march.comp, src/simulations/SatelliteSimCloudsV2.cpp, tools/cloud_micro_reference.py]
---
## Now
Water and ice cloud with a light march take a per-sample "droplet phase" ("Droplet phase", `clouds_v2.droplet_phase`,
default 1). The droplets' effective radius grows with height above the cloud base (r_eff = 1.15 um x (dz + 40 m)^(1/3) x
"Droplet size", `droplet_size` 1, clamped 4-30 um: ~5 um at a cumulus base, ~20 um high in a tower); above the
glaciation height (-10 to -35 C from the sea-level temperature under the eye, 6.5 C/km) the cloud is ice (flatter phase,
mean cosine 0.75, no bow or glory). Single scattering is the Lorenz-Mie table at that radius; light scattered 1-3 times
on the way in is lit at the droplet phase composed k + 1 times (Legendre moments ^ n: the diffraction peak stays peaky
for the low orders, then isotropizes), using the Jendersie & d'Eon 2023 HG + Draine fit per diameter; the higher orders
are the delta-Eddington diffuse field at the droplets' own g, x T/(T + 2) for light escaping a small cloud's sides, x
"Droplet multi-scatter gain" (`droplet_ms_gain` 1.8). Thin wisps keep the peaky phase (dark outlines from the side,
bright toward the Sun); thick cores are lit by the diffuse field. The silver lining is now the low orders, not a tuned lobe.

## History
Replaced (as the default) the tuned two-lobe HG single scattering with an 11% Mie share, the "Silver lining" HG pair and
a fixed g 0.85 diffuse field. `tools/cloud_micro_reference.py` path-traces the cloud_ms_reference blobs with the droplet
phase: the old model against droplet cloud was 0.69x on average, backlit cloud 0.38-0.42x, thick tops 0.66-0.72x; the new
one 1.01x, backlit 0.68-0.78x, thick 0.99-1.08x. Perf and in-app images: see docs/subsystems/clouds.md "The droplet phase".
