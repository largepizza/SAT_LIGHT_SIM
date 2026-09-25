# Known photometric residuals

The inaccuracies we know about and have accepted, as of commit `e61f67f` (2026-09-23, end of
lighting-overhaul Phase 3). Every item passes or is outside the accuracy gate
(`cmake --build <dir> --target accuracy-gate`, see `cmake/AccuracyGate.cmake`). They are logged so
nobody re-discovers them as bugs, and so a future change that moves one can be judged against where
it started. This is a satellite-lighting simulator, not a model optimizer: each item below is small
next to the features built on top of it.

Numbers come from the gate's configuration (5000 samples, seed 1) unless noted. They are identical
on MSVC and GCC to the printed precision. The full history is on the "Satellite Brightness
Benchmarking" design page (Results log).

## Against published measurements

| Item | Model | Published | Gap | Why / notes |
|---|---|---|---|---|
| VisorSat mean m1000 (Mallama 2021) | 6.993 | 7.218 | −0.224 (tol 0.3) | Plain mean samples our random geometry; the observers' geometry selection is unpublished. Phase-matched mean 7.263 (+0.045) is the like-for-like comparison. Was 6.967 / 7.220 until 2026-09-24, when occlusion started sampling each component of a merged lobe (the visor was fitted under the old sampling). |
| VisorSat median m1000 | 7.095 | 7.365 | −0.270 (tol 0.3) | Narrowest gated margin (0.030; 0.004 before 2026-09-24). Same cause as the mean. |
| VisorSat 110–120° phase bin | 7.084 | 7.433 (25 obs) | −0.349 | Model too bright at high phase. The only scored bin outside 0.3; curve RMS 0.159 passes (0.134 before 2026-09-24). Bins 120°+ have ≤ 5 observations (unscored). |
| VisorSat 40–60° / 60–100° bins | — | — | −0.11 / +0.10..0.21 | Mild S-shaped residual around the observed curve. |
| VisorSat scatter (sd m1000) | 0.740 | 0.854 | −0.11 | Attitude is fixed; real satellites jitter and fly varying roll. No attitude-noise model yet. |
| VisorSat phase slope | 0.004 mag/deg | 0.005 | −0.001 | Informational. |
| VisorSat − V1.0 differential | +1.069 | +1.29 | −0.221 (tol 0.3) | The visor's real shape is unpublished (derived from Cole's 23° full-shade constraint). |
| V1.0 mean m1000 (Mallama 2020a) | 5.924 | 5.93 | −0.006 | Held out of all fitting. sd 0.763 vs 0.67 published (informational). |
| V1.0 app bulk export (32.7°S, sim 2036-11-21) | 5.934 | 5.93 | +0.004 | Out-of-sample site and season; not a paper's sampling. |
| ISS (2023-2026 configuration) | mean m1000 −0.59 (≈ −2.5 overhead at 415 km) | −2 to −4 on favourable passes (satobs.org) | — | **No benchmark yet.** Dimensions sourced, layout derived, every surface material an estimate (`iss.json` sources). The m1000 values in this and the next rows are a V1.0-campaign copy (twilight, ≥ 20°, fully sunlit, 3000 samples, seed 1) on the model's own shell - a sanity check, not a comparison. |
| Tiangong | mean m1000 0.73 (≈ −1.3 overhead at 386 km) | — | — | No benchmark. Attitude and materials estimates. |
| Starship HLS depot | mean m1000 2.20, sd 1.3 | — | — | No benchmark and no published depot hardware: length, solar band, attitude and the bare-steel skin (the real depot has in-space insulating tiles, unpublished) are estimates. |
| SpaceX AI satellite (Starmind AI1) | mean m1000 4.58, sd 2.9 (glints; 1000 km SSO shell) | — | — | Rebuilt 2026-09-24 to the project owner's layout from the AI1 spec sheet: flat 10 m bus, 70 m span, two 10 x 9.65 m radiators edge-on to the Sun (193 m² a face - the sheet's 110 m² would be a 2.75 m strip). Was 5.51 / 3.6 with the earlier estimate. No benchmark. |
| Reflect Orbital mirror | — | — | — | No benchmark; the operational mirror size is the legacy type's 2376 m² in Earendil-1's square shape. |
| V2 Mini app bulk export | 5.81 (median 5.31, sd 1.57) | 7.87 mitigated; ~5.2 unmitigated (Mallama et al. 2023) | — | **Uncalibrated model**, flown without SpaceX's brightness-mitigation attitude. Next benchmark candidate. |
| V2 Mini, mitigated (Mallama et al. 2023) | 7.90 | 7.87 | +0.03 | Film distribution chosen with this benchmark in view (Beckmann; GGX read 7.43). Phase-curve shape: ~1 mag fainter than the paper's fit below 40 deg and 0.5-0.9 fainter at 80-140 deg; sd 1.3 vs 0.79. A second dataset (Jul-Dec 2024, 550 km, arXiv:2502.03651) gives 7.22 for the same satellites. |
| V2 Mini DTC, mitigated (Mallama et al. 2025) | 6.42 | 6.47 | −0.05 | HELD OUT (no fitting): corroborates the V2 Mini materials. sd 1.02 vs 1.32. |
| Starlink V1.5 (arXiv:2507.00107 Table 1) | 6.06 | 6.34 | −0.28 (tol 0.3) | Narrow margin. Backsheet transmission (0.03) fitted to Mallama & Respler 2022's Post-VisorSat phase function; its low-phase bins stay 0.6-0.8 too bright (weighted RMS 0.63 vs the fit). Observers/period assumed. |
| OneWeb (Mallama 2020b) | 6.96 | 7.18 | −0.22 | The bus MLI (albedo 0.1, F0 0.25) was FITTED to this mean (gold-foil MLI read 6.49). sd 1.1 vs 0.68. |
| Amazon Leo, operational (Mallama et al. 2026) | 6.60 | 6.81 | −0.21 | Held out; layout from the authors' interpretation of Amazon imagery. sd 0.94 vs 0.64. |
| Guowang, orbit-raising (arXiv:2507.00107) | 5.78 | 4.21 | **+1.57, NOT GATED** | Unpublished hardware and attitude during orbit raising; the estimated 10 m-span model is far too faint. The benchmark file is `"gated": false`. |
| Starlink V3, V2 Mini DTC dims, stations, debris | — | — | — | No benchmarks. Stations are render-level estimates. |

## Fitted, estimated or stood-in values

- **Calibrated (fitted to VisorSat, M8):** `solar_cell` diffuse albedo 0.06 → 0.02; the VisorSat
  visor size (inset 0.55 m, gap 0.233 m) at Cole's 23° cutoff. V1.0 stayed held out.
- **Estimates (model `sources` blocks, status `estimate`):** antenna-region size and albedo (0.8) on
  both 2020 Starlink models; V2 Mini wing hinge placement and tracking; most material presets
  outside `solar_cell`.
- **Hubble** flies an anti-sun stand-in attitude (no inertial-pointing law) and has no benchmark.
- **GGX, not Beckmann,** for the polished bus (Beckmann made 70–110° 0.3–0.5 mag too faint once
  earthshine was fixed), even though that material's α came from a Beckmann↔Phong equivalence.

## Model approximations (measured against exact references)

| Item | Error | Where checked |
|---|---|---|
| Occlusion sampling (16 samples per component of a lobe) vs ray-cast reference (25 points per triangle, ~0.5 m² each on large ones) | VisorSat p95 0.14, max 0.23; ISS p95 0.17, max 0.33; V2 Mini p95 0.07, max 0.36; others ≤ 0.02 | `--selftest` (gate p95 < 0.25; exact value varies per platform with the selftest's configurations) |
| GPU occlusion: lobes under 1e-4 of the total left unoccluded; skipped below mag 10 | ≤ 0.005 mag | design choice (`kOccMinLobeFrac`, `kOcclusionMagFloor`) |
| Cone occluder uses its larger radius | conservative (over-shadows) | `buildSatOcclusion` |
| Earthshine table vs exact integral | p95 0.004, max 0.014 mag | `--selftest` |
| Earthshine on a tilted plane: order-4 SH fit (floored at the vector value) vs brute-force cap integral | ≤ 0.029 of the vector irradiance (exact coefficients and table) | `--selftest`. Until 2026-09-24 the diffuse term used the vector irradiance alone: 0 for a face edge-on to it, where the cap really gives ~30% of a nadir plate's. Moved the benchmarks by ≤ 0.05 mag (OneWeb −0.03, Guowang −0.04, differential +0.002). |
| Earth is a uniform Lambertian sphere, albedo 0.3 | no clouds, oceans, glint or seasonal albedo | model limit |
| Extinction: Chapman column (erfcx fit) vs brute-force ray integral | max 0.32% of the column | `--selftest` |
| Extinction constants: 8 km / 1.2 km scale heights, 60/40 split | fixed, no weather or site haze | model limit |
| Sun disk folded into every lobe as α = 0.0023 | flat-mirror peak +2% (1.50e4 vs 1.47e4 m²) | M0 |
| Merged lobes beyond the budget (48, or 256 for rosters ≤ 10k) | exact for the shipped models within budget except the ISS (514 exact lobes): p95 0.08, max 5.6 at the app's 256 (merged module-cylinder facets in rare geometries); Hubble at a forced 24: p95 0.20 | lobe validator |

## Simulation frame and precision

- **Sim Earth rotation omits GMST at J2000** (≈ 280.46°): the Sun's hour angle is off real UTC by
  ≈ +5.25 h. Self-consistent everywhere; replaying real timestamped observations must correct it.
- **GPU orbit math is float32:** fixed for the orbital phase on 2026-09-23 (Phase 4, `orbitPhase()`:
  median 60 m / max ~770 m along-track → 1 m / 9 m). The remaining float32 floor (final angle and
  position, a few metres) still makes the GPU–CPU parity gap exceed 0.02 mag (and motion choppy)
  only with the observer very close to a satellite. Phase 4 follow mode needs the double-precision,
  camera-relative path.
- **Benchmarks exclude penumbra** (fully sunlit only, as the papers do); the app draws penumbra.
- **Legacy two-surface types are not physical magnitudes** (tuned display units); they cannot be
  traced, exported or benchmarked until they get geometry models.
