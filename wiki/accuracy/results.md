# Results and known residuals

This page gives the current result of every benchmark and the inaccuracies that are known and accepted,
each with its explanation. The numbers are from the accuracy gate's configuration: 5000 simulated
observations, seed 1, occlusion between parts on, every benchmark model baked with its exact lobes. They are
identical on MSVC and GCC to the printed precision.

The maintained record is
[`data/benchmarks/KNOWN_RESIDUALS.md`](https://github.com/largepizza/SAT_LIGHT_SIM/blob/main/data/benchmarks/KNOWN_RESIDUALS.md),
which is updated whenever a change moves one of these numbers. This page explains it; where the two
differ, the file and a fresh gate run are authoritative. How the numbers are produced is on
[SatBench and the accuracy gate](satbench.md); what the datasets are is on [Benchmark datasets](benchmarks.md).

## Summary

| Benchmark | Model | Model \( \bar m_{1000} \) | Published | Gap | Margin to 0.3 | Verdict | Status of the model |
|---|---|---|---|---|---|---|---|
| Starlink V1.0 (Mallama 2020a) | `starlink_v1_0` | 5.925 | 5.93 | −0.005 | 0.295 | pass | held out of all fitting |
| Starlink VisorSat, mean (Mallama 2021) | `starlink_visorsat` | 7.011 | 7.218 | −0.206 | 0.094 | pass | visor size fitted to this dataset's phase curve |
| Starlink VisorSat, median | | 7.092 | 7.365 | −0.273 | **0.027** | pass | narrowest gated margin |
| Starlink VisorSat, phase-curve RMS | | 0.158 | 0 | — | 0.142 | pass | a fit residual (see above) |
| VisorSat − V1.0 (differential) | both | +1.086 | +1.29 | −0.204 | 0.096 | pass | tests the visor's occlusion |
| OneWeb (Mallama 2020b) | `oneweb` | 6.966 | 7.18 | −0.214 | 0.086 | pass | bus MLI fitted to this mean |
| Starlink V2 Mini, mitigated (Mallama et al. 2023) | `starlink_v2_mini` | 7.881 | 7.87 | +0.011 | 0.289 | pass | film distribution chosen with this dataset in view |
| Starlink DTC, mitigated (Mallama et al. 2025) | `starlink_v2_mini_dtc` | 6.430 | 6.47 | −0.040 | 0.260 | pass | held out |
| Starlink V1.5 (Mallama et al. 2025) | `starlink_v1_5` | 6.055 | 6.34 | −0.285 | **0.015** | pass | backsheet transmission fitted to a different paper |
| Amazon Leo, operational (Mallama et al. 2026) | `amazon_leo` | 6.590 | 6.81 | −0.220 | 0.080 | pass | held out |
| Guowang, orbit raising (Mallama et al. 2025) | `guowang` | 5.800 | 4.21 | **+1.590** | — | fail, **not gated** | hardware and attitude unpublished |

A negative gap means the model is brighter than observed. Seven of the eight designs are within the 0.3 mag
tolerance on the mean. Six of the seven passing means are brighter than observed, by 0.005 to 0.29 mag; only
the V2 Mini is fainter, by 0.01.

"Held out" means no value in the model was adjusted with that dataset in view, so its agreement is a genuine
prediction. Where a model has been fitted to a dataset, agreement with that dataset shows only that the fit
worked; the evidence is in the held-out comparisons. See [Model provenance](provenance.md) for what was
fitted to what.

## Against published measurements

### Starlink VisorSat (Mallama 2021)

The only dataset with individual observations, and the one examined most closely.

| Metric | Model | Observed | Delta | Kind |
|---|---|---|---|---|
| mean \( m_{1000} \) | 7.011 | 7.218 | −0.206 | gated |
| median | 7.092 | 7.365 | −0.273 | gated |
| phase-matched mean | 7.260 | 7.218 | +0.043 | informational |
| standard deviation | 0.713 | 0.854 | −0.141 | informational |
| linear phase slope | 0.0043 mag/deg | 0.0049 | −0.0006 | informational |
| phase-curve RMS (8 scored bins) | 0.158 | — | — | gated, < 0.3 |
| censored ("not seen") share | 6.5% | 1.6% (7 of 430) | | |

**The plain mean and median are too bright because the model samples a different set of geometries.** The
plain statistics average over random visible satellites in twilight; the observers' choice of passes is
unpublished and gives a narrower range of phase angles. Reweighted to the observed phase-angle distribution,
the model's mean is 7.260, within 0.05 of the observed 7.218. That phase-matched mean is the like-for-like
comparison. The median margin, 0.027 mag, is the narrowest of any gated metric.

Phase curve, mean \( m_{1000} \) per 10° bin (scored bins have at least 10 observations):

| Phase (deg) | 40–50 | 50–60 | 60–70 | 70–80 | 80–90 | 90–100 | 100–110 | 110–120 |
|---|---|---|---|---|---|---|---|---|
| observations | 19 | 48 | 59 | 51 | 58 | 90 | 66 | 25 |
| observed | 6.81 | 7.12 | 7.24 | 7.26 | 7.26 | 7.27 | 7.27 | 7.43 |
| model | 6.68 | 7.04 | 7.34 | 7.47 | 7.41 | 7.40 | 7.24 | 7.04 |
| delta | −0.13 | −0.08 | +0.10 | +0.21 | +0.15 | +0.13 | −0.03 | **−0.39** |

The residual is a mild S-shape: slightly too bright at 40–60°, too faint at 60–100°. The 110–120° bin is
0.39 mag too bright, the only scored bin outside 0.3; the curve's weighted RMS of 0.158 still passes. Bins
beyond 120° hold 5 or fewer observations and are not scored.

**The scatter is too small** (0.71 against 0.85) because every simulated satellite flies its nominal attitude
exactly. Real satellites jitter and fly varying roll; there is no attitude-noise model. The same holds for
every other benchmark that reports a standard deviation, in one direction or the other.

### VisorSat minus V1.0 (differential)

The model's visor dims the satellite by 1.086 mag against the published 1.29. The two models share chassis and
materials, so this isolates the visor. Its real shape is unpublished: its stand-off is derived from the
constraint that the antennas are fully shaded at 23° of Sun depression (Cole 2021), and its size is fitted to
the VisorSat phase curve. The visor dims only through occlusion, so this benchmark is also the main check of
the occlusion model.

### Starlink V1.0 (Mallama 2020a)

5.925 against 5.93, held out of all fitting. Standard deviation 0.755 against 0.67 (informational). An
out-of-sample check with the app's bulk export, at 32.7° S rather than the paper's northern sites and in a
different season, gives 5.934.

### OneWeb (Mallama 2020b)

6.966 against 7.18. The bus's multi-layer insulation is **fitted** to this mean: the generic gold-foil MLI
preset (albedo 0.2, F0 0.6) reads 6.49, a dark MLI (albedo 0.1, F0 0.25) reads 6.97. No other OneWeb data is
held out. Standard deviation 1.09 against 0.68.

### Starlink V2 Mini, mitigated (Mallama et al. 2023)

7.881 against 7.87. The dielectric mirror film on the nadir face uses a Beckmann microfacet distribution,
chosen with this benchmark in view: with GGX's longer tail the film forward-scatters grazing terminator
sunlight towards the ground and the model reads 7.43. The mean agrees; the shape does not. Against the paper's
quadratic phase fit the model is about 1 mag fainter below 40° phase and 0.5–0.9 mag fainter at 80–140°, and
its standard deviation is 1.30 against 0.79. The simulated campaign censors 31% of its samples as too faint to
see, where the real campaign could not see 18 of 506 attempts. A second, later dataset of the same satellites
(arXiv:2502.03651) gives 7.22, which shows how much campaigns of one design differ.

### Starlink Direct-to-Cell, mitigated (Mallama et al. 2025)

6.430 against 6.47, **held out**: nothing was fitted to it, and it corroborates the V2 Mini's materials at a
lower altitude with a large phased array added. Standard deviation 1.02 against 1.32.

### Starlink V1.5 (Mallama et al. 2025)

6.055 against 6.34, a margin of 0.015 mag, the narrowest of all means. The array's translucent backsheet has a
diffuse transmission of 0.03, fitted to the Post-VisorSat phase function of Mallama & Respler (2022), not to
this dataset; even so, the low-phase bins of that phase function stay 0.6–0.8 mag too bright (weighted RMS 0.63
against the fit). The observers, their split and the period are assumptions, and the satellites' attitude over
the period (including the knife-edge roll SpaceX introduced later) is not known.

### Amazon Leo, operational (Mallama et al. 2026)

6.590 against 6.81, **held out**. The layout comes from the authors' interpretation of imagery. Standard
deviation 0.92 against 0.64.

### Guowang during orbit raising (not gated)

5.800 against 4.21: the model is 1.6 mag too faint. The hardware is known only as a box about 3 × 1 × 1 m with
a 10 m span, and the satellites were observed while raising their orbits, in an attitude nobody has
published. The model flies the operational attitude estimate. The file is `"gated": false`, so the miss is
reported in every run but does not fail the gate.

## Satellites with no benchmark

For these the magnitudes are predictions with nothing to compare them against. As a sanity check, each has
been run through a copy of the V1.0 campaign (twilight, ≥ 20°, fully sunlit) on its own shell:

| Model | Mean \( m_{1000} \) | Notes |
|---|---|---|
| ISS | −0.59 (about −2.5 overhead at 415 km) | observers report −2 to −4 on favourable passes; dimensions sourced, every surface material an estimate |
| Tiangong | 0.73 (about −1.3 overhead at 386 km) | attitude and materials estimated |
| Starship HLS depot | 2.20, sd 1.3 | length, solar band, attitude and the bare-steel skin are estimates; the real depot's insulating tiles are unpublished |
| SpaceX AI satellite (Starmind AI1) | 3.60, sd 1.5 (1000 km, 99.5° shell) | the bus's 30 m² aluminium face looks at the Earth and glints sunlight down when the Sun is just below the satellite's horizon (phase 80–140°) |
| Reflect Orbital mirror | — | operational mirror size from the quoted 55 × 55 m |
| Starlink V3, commercial stations, debris | — | render-level estimates |

## Fitted and estimated values

- **Calibrated against VisorSat:** the `solar_cell` diffuse albedo, 0.02 (from the low-phase bins, where the
  array face dominates), and the VisorSat visor's size (inset 0.55 m, gap 0.233 m at the 23° cutoff, chosen as
  the minimum of the phase-curve RMS). The V1.0 mean and the differential stay held out.
- **Calibrated against OneWeb:** the bus MLI (albedo 0.1, F0 0.25, roughness 0.35).
- **Calibrated against Mallama & Respler (2022):** the V1.5 backsheet transmission, 0.03.
- **Chosen with a benchmark in view:** Beckmann for the dielectric mirror films (V2 Mini), and GGX for the
  polished Starlink bus (Beckmann makes 70–110° phase 0.3–0.5 mag too faint), even though the bus's roughness
  came from a Beckmann–Phong equivalence.
- **Estimates:** the antenna region's size and albedo (0.8) on both 2020 Starlink models; most material presets
  other than `solar_cell`; every station's surfaces. Hubble flies an anti-Sun stand-in attitude, since the
  simulator has no inertial-pointing law.

## Model approximations

Each of these is measured against an exact reference by the self-tests.

| Approximation | Error | Checked by |
|---|---|---|
| Occlusion between parts from 16 samples per component of a lobe, against a ray cast from ≥ 25 points per triangle | VisorSat p95 0.14, max 0.23; ISS p95 0.17, max 0.33; V2 Mini p95 0.07, max 0.36; others ≤ 0.02 mag | `--selftest` (gate p95 < 0.25) |
| GPU occlusion skips lobes under 1e-4 of the total and satellites fainter than magnitude 10 | ≤ 0.005 mag | design choice |
| A cone occluder uses its larger radius | conservative: over-shadows | by construction |
| Earthshine table against the exact integral | p95 0.004, max 0.014 mag | `--selftest` |
| Earthshine on a tilted plane as an order-4 spherical-harmonic fit, floored at the vector value | ≤ 2.9% of the vector irradiance | `--selftest` |
| The Earth as a uniform Lambertian sphere, albedo 0.3 | no clouds, oceans, glint or seasonal albedo | model limit |
| Chapman-column extinction (fitted `erfcx`) against a brute-force ray integral | ≤ 0.32% of the column | `--selftest` |
| Extinction with fixed 8 km / 1.2 km scale heights and a 60/40 split | no weather or site haze | model limit |
| The Sun's disk folded into every lobe as roughness 0.0023 | a flat mirror's peak +2% | construction |
| Facets merged beyond the lobe budget (48, or 256 for rosters of 10 000 or fewer) | exact for the shipped models within budget, except the ISS (514 exact lobes): p95 0.08, max 5.6 mag at 256, in rare geometries where module-cylinder facets merge; Hubble at a forced 24: p95 0.20 | lobe validator |

## Frame, precision and scope

- **GPU orbit math is float32.** The orbital phase is formed exactly; what remains is float rounding of the
  final angle and position, a few metres. It exceeds the 0.02 mag parity threshold only with the camera very
  close to a satellite; close-up rendering uses a double-precision, camera-relative path.
- **Benchmarks exclude penumbra**, as the papers do. The app draws satellites in penumbra.
- **Legacy two-surface types are not physical magnitudes.** They cannot be traced, exported or benchmarked
  until they have geometry models.
- **Comparisons are above the atmosphere.** Only the censoring rule touches apparent magnitudes, and it
  compares above-atmosphere magnitudes with the observers' apparent limit
  ([details](satbench.md#drawing-one-observation)).

## Where in the code

- `data/benchmarks/KNOWN_RESIDUALS.md`: the maintained record of everything on this page.
- `cmake/AccuracyGate.cmake`: the configuration these numbers come from.
- Reports: `<build dir>/accuracy_gate/benchmark_runs/*.json` after a gate run.
