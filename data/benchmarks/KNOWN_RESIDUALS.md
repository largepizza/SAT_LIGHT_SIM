# Known photometric residuals

The inaccuracies we know about and have accepted, as of commit `e61f67f` (2026-09-23, end of
lighting-overhaul Phase 3). Every item passes or is outside the accuracy gate
(`cmake --build <dir> --target accuracy-gate`, see `cmake/AccuracyGate.cmake`). They are logged so
nobody re-discovers them as bugs, and so a future change that moves one can be judged against where
it started. This is a satellite-lighting simulator, not a model optimizer: each item below is small
next to the features built on top of it.

Numbers come from the gate's configuration (5000 samples, seed 1) unless noted, re-run 2026-10-03 after the Earth
rotation angle became GMST (sites now see their real twilight instants; every mean moved by ≤ 0.02 mag), and 2026-10-21
after the Sun's disc became an anisotropic source term (every gated value moved by ≤ 0.001 mag), and 2026-10-22 after
SatBench started drawing a sample's site ONCE and keeping it through rejected draws (each site's share of the samples is
now its weight; redrawn after every rejection, a site with a narrower window or a shorter twilight was under-sampled:
Amazon Leo +0.07, V2 Mini +0.085, every other ≤ 0.012). They are identical
on MSVC and GCC to the printed precision. The full history is on the "Satellite Brightness
Benchmarking" design page (Results log).

## Against published measurements

| Item | Model | Published | Gap | Why / notes |
|---|---|---|---|---|
| VisorSat mean m1000 (Mallama 2021) | 7.428 | 7.218 | +0.210 (tol 0.3) | Re-fitted 2026-10-22 (below) and sampled at MEASURED windows: visual sites Sun −24..−10°, elevation ≥ 30°; MMT Sun −24..−11°, elevation ≥ 45° (the file's `sampling` notes say how each was measured). By site: Mallama 7.42 vs 7.26 observed, Respler 7.44 vs 7.41, MMT 7.43 vs 7.06 (median 7.45 vs 7.37: the table's MMT rows carry bright-attitude passes). Phase-matched mean 7.533 (+0.315). Until 2026-10-22: 7.011 under SatBench's assumed −18..−6° / ≥ 20° window, which the M8 fit had been tuned to. |
| VisorSat median m1000 | 7.500 | 7.365 | +0.135 (tol 0.3) | |
| VisorSat phase-curve RMS | 0.349 | — | **NOT GATED (metric only)**, tol 0.3 | `ungated_metrics` in the file: mean and median stay gated. Mostly an offset: bins 40–100° are 0.33–0.45 too faint, 100–110° +0.18, 110–120° −0.09. The bin MEANS carry the bright-attitude passes (MMT rows mean 7.06, median 7.37) that a nominal attitude cannot make, and the same model reads ~0.2 too BRIGHT against MMT's own held-out tracks of these satellites (below): the table and the tracks disagree by ~0.4 mag at the fitted geometry. With the flare-facet population (a test copy, not shipped) 0.297–0.302. Was 0.158 (passing) under the assumed window. |
| VisorSat / V1.0 MMT replay (94 tracks 2020-25, SatModelTool --pass, 1.5-s medians, MMT − model) | V1.0 median +0.01 (fit) / −0.02 (held out: STARLINK-1008 odd years); VisorSat +0.08 (fit, STARLINK-1436) / +0.21 (held out: STARLINK-1538) | — | track rms 0.35 / 0.47 / 0.27 / 0.51 | Was +0.88 / +0.89 / +0.96 / +1.08 with the M8 models: ~1 mag too bright at MMT's geometry. 13 VisorSat bright-mode tracks (2021-03 on, −0.6..−1.9) excluded: unmodelled attitude. Sun-altitude trend (phase-detrended, mag per 10° of Sun altitude, fit / held out): V1.0 −0.07 / −0.01, VisorSat −0.22 / −0.20 (Lambert refit: −0.29 / −0.18, −0.36 / −0.34). Elevation trend unchanged by Minnaert: +0.10 / −0.05 (V1.0), +0.06 / +0.14 (VisorSat) per 10°. |
| VisorSat scatter (sd m1000) | 0.442 | 0.854 | −0.41 | Attitude is fixed and the sampled geometry (visual ≥ 30°, MMT ≥ 45°) is narrow; real satellites jitter and fly varying roll. No attitude-noise model. |
| VisorSat phase slope | 0.002 mag/deg | 0.005 | −0.003 | Informational. |
| VisorSat − V1.0 differential | +1.523 | +1.29 | +0.233 (tol 0.3) | The visor's real shape is unpublished (inset fitted to MMT, gap from Cole's 23° full-shade constraint). Was +1.086 under the assumed window. |
| V1.0 mean m1000 (Mallama 2020a) | 5.904 | 5.93 | −0.025 | MMT-9's own entry: Sun −33..−11°, ≥ 45° (its STARLINK-1008 tracks); visual sites as VisorSat's. sd 0.211 vs 0.67 (informational: one nominal attitude at narrow geometry). Was 5.925 under the assumed window with the M8 models. |
| V1.0 app bulk export (32.7°S 70.6°W, sim 2036-11-21, 24 h at 30 s, 3848 samples) | 6.466 (median 6.289; re-run 2026-10-22 after the refit; was 5.934 with the M8 models) | 5.93 | +0.54 | NOT comparable to the paper since the refit: the app's export samples SatBench's old assumed window (Sun -18..-6 deg, elevation >= 20 deg), under which the refit reads ~0.5-0.8 faint (the measured windows are deeper and higher). Same run, VisorSat: 7.523 with part occlusion on (differential +1.06), 6.235 with it off — the app's default, where the visor dims nothing. Harness `bulkexport` (harness_runs/bulk). |
| ISS (2023-2026 configuration) | mean m1000 −0.59 (≈ −2.5 overhead at 415 km) | −2 to −4 on favourable passes (satobs.org) | — | **No benchmark yet.** Dimensions sourced, layout derived, every surface material an estimate (`iss.json` sources). The m1000 values in this and the next rows are a V1.0-campaign copy (twilight, ≥ 20°, fully sunlit, 3000 samples, seed 1) on the model's own shell - a sanity check, not a comparison. |
| Tiangong | mean m1000 0.73 (≈ −1.3 overhead at 386 km) | — | — | No benchmark. Attitude and materials estimates. |
| Future stations (art pass 2026-10-20) | mean m1000: Haven-1 2.98 (was 2.89), Haven-2 0.78 (1.41), Axiom 1.27 (1.46), Orbital Reef 0.34 (1.53), ROS 1.44 (2.25), BAS 1.52 (2.24), Starlab 2.39 (1.68) | — | — | No benchmarks; layouts from concept art (`tools/make_station_models.py`, sources per model). Same V1.0-campaign copy on each station's roster shell (3000 samples, seed 1). Most got brighter with more and larger wings (Reef 520 m2 of rigid arrays); Starlab got fainter: its four wings in a 45-deg cross turn about their own diagonal axes and cannot face the Sun as squarely as the old cross-track pair. ISS and Tiangong unchanged (their additions are render-only). |
| Starship HLS depot | mean m1000 2.20, sd 1.3 | — | — | No benchmark and no published depot hardware: length, solar band, attitude and the bare-steel skin (the real depot has in-space insulating tiles, unpublished) are estimates. |
| SpaceX AI satellite (Starmind AI1) | mean m1000 3.60, sd 1.5 (1000 km, 99.5 deg shell) | — | — | Round 2 (2026-09-24), the project owner's layout: nadir-pointing, the flat 10 x 3 x 0.6 m bus's 3 x 0.6 m end toward the Sun, the two 10 x 9.65 m radiators standing up and hanging down (edge-on to the Sun; 193 m² a face - the sheet's 110 m² would be a 2.75 m strip), each wing on one truss, tracking the Sun about its span. 1 mag brighter than round 1 (4.62 / 2.9, same run): the bus's 30 m² aluminium face now looks at the Earth and glints sunlight down whenever the Sun is just below the satellite's horizon (phase 80-140 deg: m1000 ~5.0, was ~7). No benchmark. |
| Reflect Orbital mirror | — | — | — | No benchmark; the operational mirror size is the legacy type's 2376 m² in Earendil-1's square shape. |
| V2 Mini app bulk export | 5.81 (median 5.31, sd 1.57) | 7.87 mitigated; ~5.2 unmitigated (Mallama et al. 2023) | — | **Uncalibrated model**, flown without SpaceX's brightness-mitigation attitude. Next benchmark candidate. |
| V2 Mini, mitigated (Mallama et al. 2023) | 7.965 | 7.87 | +0.095 | Film distribution chosen with this benchmark in view (Beckmann; GGX read 7.43). Phase-curve shape: ~1 mag fainter than the paper's fit below 40 deg and 0.5-0.9 fainter at 80-140 deg; sd 1.3 vs 0.79. A second dataset (Jul-Dec 2024, 550 km, arXiv:2502.03651) gives 7.22 for the same satellites. |
| V2 Mini DTC, mitigated (Mallama et al. 2025) | 6.438 | 6.47 | −0.032 | HELD OUT (no fitting): corroborates the V2 Mini materials. sd 1.02 vs 1.32. |
| Starlink V1.5 (arXiv:2507.00107 Table 1) | 6.065 | 6.34 | −0.275 (tol 0.3) | Narrow margin. Backsheet transmission (0.03) fitted to Mallama & Respler 2022's Post-VisorSat phase function; its low-phase bins stay 0.6-0.8 too bright (weighted RMS 0.63 vs the fit). Observers/period assumed. |
| OneWeb (Mallama 2020b) | 6.962 | 7.18 | −0.218 | The bus MLI (albedo 0.1, F0 0.25) was FITTED to this mean (gold-foil MLI read 6.49). sd 1.1 vs 0.68. |
| Amazon Leo, operational (Mallama et al. 2026) | 6.663 | 6.81 | −0.147 | Held out; layout from the authors' interpretation of Amazon imagery. sd 0.94 vs 0.64. |
| Guowang, orbit-raising (arXiv:2507.00107) | 5.838 | 4.21 | **+1.63, NOT GATED** | Unpublished hardware and attitude during orbit raising; the estimated 10 m-span model is far too faint. (5.80 until the 2026-10-20 realism pass: a radome over the nadir face, wings 5 cm lower.) The benchmark file is `"gated": false`. |
| BlueWalker 3, timed replay (Mallama et al. 2023b App. B + Nandakumar et al. 2023 source data, 2022-11-14..12-07, SGP4 with archived TLEs ≤ 4 d old) | 2.97 (visual geometries) | 3.01 | −0.04 | Not in the gate (a replay, not a SatBench run; scripts outside the repo). The face albedo (0.32) was FITTED to these visual magnitudes. V-band CCD photometry (Steward 'Pomenis', 251 points) reads 0.5 mag brighter than the visual estimates at the same geometry (consistent per pass to 0.14 mag): a calibration difference between the data sets. Light-curve shapes: Pomenis within-pass residual RMS 0.35, MAD 0.14 mag. The facet tilt (3°) was fitted to the 24 grazing-incidence points (flat plate: −1.1 mag there). Within ~30 km tangent height of the Sun's ray the satellite is 1-3 mag fainter than modelled: the evaluator's Earth shadow is geometric (no atmospheric/ozone dimming of the grazing sunlight). The faint periods (roll toward the Sun, unmodelled) read +2 to +3.5 nominal; a 13-20° roll restores them (Mallama et al. 2023a: 13-16°). Unchanged by the 2026-10-21 photometry fixes above the atmosphere (≤ 1e-4 mag); the pass tool's APPARENT magnitudes put high-latitude observers kilometres underground until then (WGS84 radius vs the 6371-km sphere: up to 35 mag of extinction), so the apparent-magnitude scatter of the visual set fell from 4.5 to 1.0 mag rms. |
| Iridium Block 1 flares (validation model `iridium_block1.json`, not in the roster; scripts outside the repo) | Lee/John 1997 visual magnitude vs mirror angle, medians per bin: 0.6-1° −1.91 vs −1.99, 1-2° −0.64 vs −0.89, 2-4° 0.63 vs 1.28, 7-12° 3.43 vs 3.06; MMT-9 IRIDIUM 13 wing (2015-06-05, mirror angle 7-14°, held out) within 0.53 mag; flare centre −7.1 to −4.7 at Heavens-Above's 2000 flares | Lee/John; MMT-9; Heavens-Above | centre ~1.1 mag fainter than H-A | The haze (0.3 of the MMAs' reflectance at α 0.04, a material haze since 2026-10-21; an area split of the panel before) was FITTED to the Lee/John tail. H-A's centre magnitudes (−9 at ~830 km) are brighter than a 1.62 m² flat mirror can make. The 2026-10-21 Sun-disc term made low-elevation (oblique) flare centres 0.1-0.3 mag fainter and the H-A off-centre drop rms 0.40 → 0.37. Occlusion selftest p95 0.239 (gate 0.25). |
| BlueWalker 3, regular mean m1000 (Mallama et al. 2023b) | 3.166 | 3.09 | +0.076 | Gated since 2026-10-22. SatBench now reads a file's `sampling.constraints` (`sun_altitude_deg` [min, max], `min_elevation_deg`); this file's are measured from the paper's own Appendix B visual rows (155 rows, 2022-11-11..12-07): Sun altitude −24..−10° (2.5-97.5 percentile; 44% below −18°), elevation ≥ 15°. With the old fixed −18..−6° window it read 3.48. The face albedo was fitted to these same visual data, so this guards against regressions rather than validating. sd 0.56 vs 0.69. |
| Starlink V3, V2 Mini DTC dims, stations, debris | — | — | — | No benchmarks. Stations are render-level estimates. |

## Fitted, estimated or stood-in values

- **Calibrated (MMT replay, 2026-10-22; scratchpad `starlink_refit`, joint fit of both 2020 designs to 42 MMT tracks,
  39 held out):** `bus_metal` Beckmann α 0.08, diffuse 0.11 (F0 0.9 kept); `antenna_white` 0.31; `visor_black` 0.019;
  visor inset 0.39 m (gap 0.166 m); Minnaert k 0.65 on those three diffuse terms (Mallama 2020a: 0.578 ± 0.084; the
  free fit 0.80, loss 0.1412 vs 0.1426 at 0.65 and 0.1439 Lambert). The M8 values (GGX 0.23, preset albedos 0.10 / 0.8 /
  0.05, inset 0.55 m) were fitted under SatBench's assumed Sun −18..−6° window. `solar_cell` stays 0.02 (M8; MMT is
  insensitive to it).
- **Estimates (model `sources` blocks, status `estimate`):** antenna-region size on both 2020
  Starlink models (its albedo is fitted, and degenerate with the size); V2 Mini wing hinge placement and tracking; most material presets
  outside `solar_cell`.
- **The GEO belt (2026-10-20)** flies four GENERIC class models (`geo_comsat_large`, `geo_comsat_medium`,
  `geo_weather`, `geo_spinner`, tools/make_geo_models.py), not one per satellite, at the mid-2020s slots of
  data/geo_satellites.json (approximate). No benchmark. In-app check (2036-11-22 14:00 UTC from 40 N 10 E):
  Astra 1KR 13.8 seen (5.7 at 1000 km, phase 126°), Nilesat 201 15.1 (6.9, 153°), Meteosat-11 16.3 (8.2, 136°);
  GOES-19 from Quito at night 11.4 (3.6, 61°). Real GEO comsats are typically mag 10-14 away from glints. Baked
  at the 48-lobe budget (validator p95 ≤ 0.01 mag). The spin of MSG / Fengyun-2 is not modelled (axisymmetric drum).
- **Hubble** flies an anti-sun stand-in attitude (no inertial-pointing law) and has no benchmark.
- **Beckmann α 0.08 for the 2020 Starlinks' polished base** (2026-10-22). The M8 note "GGX, not Beckmann" (Beckmann made
  70–110° 0.3–0.5 mag too faint) held only under the assumed −18..−6° window; at MMT's geometry GGX's tail is ~2x too
  bright off the specular beam.
- **Flare-facet population (tested, not shipped, 2026-10-22):** 115 tiny polished facets (normals within 60° of nadir,
  A·F median 3.5e-5 m², GGX α ~0.007; scratchpad `starlink_geometry` s11b) reproduce MMT's VisorSat flare RATE (time
  > 0.5 mag above baseline 1.1–2.4% vs 2.2% observed, > 1 mag 0.16–0.84% vs 0.71%) but not their timing (0–1 of 24
  observed flares hit); on the benchmarks VisorSat 7.379–7.380 (+0.16), median +0.09, curve RMS 0.297–0.302 by
  realization, V1.0 5.890 (−0.04), differential +1.49; MMT replay unchanged (±0.03). Not a clear gain for 115 parts.

## Model approximations (measured against exact references)

| Item | Error | Where checked |
|---|---|---|
| Occlusion sampling (16 samples per component of a lobe) vs ray-cast reference (25 points per triangle, ~0.5 m² each on large ones) | VisorSat p95 0.13, max 0.26 (inset 0.39 m since 2026-10-22); ISS p95 0.17, max 0.33; V2 Mini p95 0.07, max 0.36; others ≤ 0.02 | `--selftest` (gate p95 < 0.25; exact value varies per platform with the selftest's configurations) |
| GPU occlusion: lobes under 1e-4 of the total left unoccluded; skipped below mag 10 | ≤ 0.005 mag | design choice (`kOccMinLobeFrac`, `kOcclusionMagFloor`) |
| Cone occluder uses its larger radius | conservative (over-shadows) | `buildSatOcclusion` |
| Earthshine table vs exact integral | p95 0.004, max 0.014 mag | `--selftest` |
| Earthshine on a tilted plane: order-4 SH fit (floored at the vector value) vs brute-force cap integral | ≤ 0.029 of the vector irradiance (exact coefficients and table) | `--selftest`. Until 2026-09-24 the diffuse term used the vector irradiance alone: 0 for a face edge-on to it, where the cap really gives ~30% of a nadir plate's. Moved the benchmarks by ≤ 0.05 mag (OneWeb −0.03, Guowang −0.04, differential +0.002). |
| Earth is a uniform Lambertian sphere, albedo 0.3 | no clouds, oceans, glint or seasonal albedo | model limit |
| Extinction: Chapman column (erfcx fit) vs brute-force ray integral | max 0.32% of the column | `--selftest` |
| Extinction constants: 8 km / 1.2 km scale heights, 60/40 split | fixed, no weather or site haze | model limit |
| The Sun's disc as an anisotropic α² pair (2026-10-21; `kSunAlpha` 0.00208 + the 1/(s·h)² stretch across the plane of incidence, `sunDiscA2`) vs the material lobe convolved with a limb-darkened disc (u 0.6, brute force) | peak: GGX ≤ 0.006 mag, Beckmann ≤ 0.036 mag, incidence 0-75°, α 0-0.3 (≤ 0.05 at 80°); energy within 0.3% of the old folding; α ≥ 0.05 changed ≤ 0.004 mag. The reflected beam is round (as the disc's image is) but its half-maximum radius is ~0.15° against the disc's 0.26°: a GGX-shaped kernel, peak and energy matched, with wings | scratchpad `photometry_fixes/scripts/sunref*.py`. Until 2026-10-21 one isotropic α 0.0023 (the uniform disc's peak at normal incidence): 1/cos θ too bright at oblique incidence (−0.26 mag at 50°, −0.95 at 70°, −1.25 at 75°), cos θ too narrow across the plane of incidence, +0.22 mag faint at normal incidence against the limb-darkened centre. Every gated benchmark moved ≤ 0.001 mag |
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
