---
pages: [simulation/photometry.md, modding/satellite-models.md, accuracy/results.md, accuracy/benchmarks.md]
code: [src/simulations/SatModel.cpp, src/simulations/SatModel.h, shaders/sat_orbit.comp, shaders/sat_mesh.frag, src/simulations/SatMesh.cpp, data/satellite_models/starlink_v1_0.json, data/satellite_models/starlink_visorsat.json, data/benchmarks/KNOWN_RESIDUALS.md]
---
## Now
A material may set `"minnaert": k` (0.2-1, default 1 = Lambert): its diffuse intensity is ρ(k+1)/(2π)·A·(cos i·cos e)^k
instead of ρ/π·A·cos i·cos e, the same power with light and view overhead and brighter than Lambert with either grazing.
It is in every evaluator (CPU photometry and brute-force references, `sat_orbit.comp`, the mesh renderer for the Sun,
Moon, local lights and earthshine; under the broad earthshine source the face's irradiance acts at cos i = En/E).
k is stored at 1/127 resolution; materials without the key are unchanged bit for bit. GPU parity in the app: dmag
~1e-6; the 3D view's render-vs-lobes check within 0.02 mag with shadows off.

The 2020 Starlink reference models (V1.0, VisorSat) are calibrated to Mini-MegaTORTORA (MMT) light curves: a joint fit
of both designs to 42 MMT tracks (1.5-s medians, SatModelTool --pass), 39 held out. bus_metal Beckmann α 0.08, F0 0.9,
diffuse 0.11; antenna_white 0.31; visor_black 0.019; visor inset 0.39 m (gap 0.166 m); Minnaert k 0.65 on those three
diffuse terms. Held-out MMT medians (MMT − model): V1.0 −0.02, VisorSat +0.21. Benchmarks at the measured windows: VisorSat
7.428 vs 7.218 (median 7.500 vs 7.365), V1.0 5.904 vs 5.93, differential 1.523 vs 1.29; VisorSat phase-curve RMS 0.349
is reported, not gated.

## History
The M8 calibration (GGX α 0.23, preset albedos, inset 0.55 m) was fitted under SatBench's assumed Sun −18..−6° window;
replayed against MMT's real geometry (Sun −24..−11°, high passes) both models were ~1 mag too bright (median residual
+0.88..+1.08). A Lambert-only refit left a Sun-altitude trend of −0.29 (V1.0) / −0.36 (VisorSat) mag per 10°; Minnaert
k 0.65 takes it to −0.07 / −0.22 (Mallama 2020a measured k 0.578 ± 0.084 on 830 V1.0 passes; the free fit gives 0.80,
the loss is flat in k). The elevation trend is unchanged. A population of 115 tiny polished facets (flare study)
reproduced MMT's flare rate but not the flares' timing, and moved the benchmarks by ≤ 0.05 (curve RMS 0.297-0.302 by
realization): tested, not shipped.
