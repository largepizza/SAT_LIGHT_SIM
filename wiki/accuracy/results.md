# Results and known residuals

This page gives the current result of every benchmark and the inaccuracies that are known and accepted,
each with its explanation. The gated numbers are from the accuracy gate's configuration: 5000 simulated
observations, seed 1, occlusion between parts on, every benchmark model baked with its exact lobes, each
benchmark sampled in the Sun and elevation window its file states (SatBench's defaults otherwise). They are identical on MSVC and GCC to the printed
precision. The light-curve replays (Starlink against MMT tracks, BlueWalker 3, Iridium) are separate studies
run with `SatModelTool --pass`; they are not part of the gate.

The maintained record is
[`data/benchmarks/KNOWN_RESIDUALS.md`](https://github.com/largepizza/SAT_LIGHT_SIM/blob/main/data/benchmarks/KNOWN_RESIDUALS.md),
which is updated whenever a change moves one of these numbers. This page explains it; where the two
differ, the file and a fresh gate run are authoritative. How the numbers are produced is on
[SatBench and the accuracy gate](satbench.md); what the datasets are is on [Benchmark datasets](benchmarks.md).

## Summary

| Benchmark | Model | Model \( \bar m_{1000} \) | Published | Gap | Margin to 0.3 | Verdict | Status of the model |
|---|---|---|---|---|---|---|---|
| Starlink V1.0 (Mallama 2020a) | `starlink_v1_0` | 5.904 | 5.93 | −0.025 | 0.275 | pass | fitted jointly with VisorSat to MMT light curves, not to this dataset |
| Starlink VisorSat, mean (Mallama 2021) | `starlink_visorsat` | 7.428 | 7.218 | +0.210 | 0.090 | pass | fitted to MMT light curves |
| Starlink VisorSat, median | | 7.500 | 7.365 | +0.135 | 0.165 | pass | |
| Starlink VisorSat, phase-curve RMS | | 0.349 | 0 | — | — | fail, **metric not gated** | see below |
| VisorSat − V1.0 (differential) | both | +1.523 | +1.29 | +0.233 | 0.067 | pass | tests the visor's occlusion |
| OneWeb (Mallama 2020b) | `oneweb` | 6.962 | 7.18 | −0.218 | 0.082 | pass | bus MLI fitted to this mean |
| Starlink V2 Mini, mitigated (Mallama et al. 2023) | `starlink_v2_mini` | 7.965 | 7.87 | +0.095 | 0.205 | pass | film distribution chosen with this dataset in view |
| Starlink DTC, mitigated (Mallama et al. 2025) | `starlink_v2_mini_dtc` | 6.438 | 6.47 | −0.032 | 0.268 | pass | held out |
| Starlink V1.5 (Mallama et al. 2025) | `starlink_v1_5` | 6.065 | 6.34 | −0.275 | **0.025** | pass | backsheet transmission fitted to a different paper |
| Amazon Leo, operational (Mallama et al. 2026) | `amazon_leo` | 6.663 | 6.81 | −0.147 | 0.153 | pass | held out |
| BlueWalker 3, regular (Mallama et al. 2023b) | `bluewalker3` | 3.166 | 3.09 | +0.076 | 0.224 | pass | face albedo fitted to the same visual data |
| Guowang, orbit raising (Mallama et al. 2025) | `guowang` | 5.838 | 4.21 | **+1.628** | — | fail, **not gated** | hardware and attitude unpublished |

A negative gap means the model is brighter than observed. Eight of the nine designs are within the 0.3 mag
tolerance on the mean. The narrowest gated margin is Starlink V1.5's, 0.025 mag.

"Held out" means no value in the model was adjusted with that dataset in view, so its agreement is a genuine
prediction. Where a model has been fitted to a dataset, agreement with that dataset shows only that the fit
worked; the evidence is in the held-out comparisons. See [Model provenance](provenance.md) for what was
fitted to what.

## The 2020 Starlinks against MMT light curves

The two 2020 Starlink reference models (V1.0 and VisorSat) are calibrated to the Mini-MegaTORTORA (MMT) camera's
own light curves of these satellites, not to the papers' summary statistics. 94 MMT tracks from 2020 to 2025 were
replayed with `SatModelTool --pass` (SGP4 states from archived element sets, the model evaluated along each
track with occlusion and extinction), each reduced to 1.5-second medians. Both designs were fitted jointly on 42
tracks; the other 39 are held out. 13 VisorSat tracks in a bright mode (from March 2021 on, −0.6 to −1.9 mag
off nominal) are excluded: they come from an attitude the model does not fly.

| Set | Median MMT − model | Track rms |
|---|---|---|
| V1.0, fitted tracks | +0.01 | 0.35 |
| V1.0, held out (STARLINK-1008, odd years) | −0.02 | 0.47 |
| VisorSat, fitted tracks (STARLINK-1436) | +0.08 | 0.27 |
| VisorSat, held out (STARLINK-1538) | +0.21 | 0.51 |

A positive value means the model is brighter than MMT measured. What remains is a trend with the Sun's altitude
(after removing the phase dependence): −0.07 and −0.01 mag per 10 deg for V1.0 (fitted, held out), −0.22 and
−0.20 for VisorSat. The Minnaert diffuse term is what takes the trend down; with Lambertian faces it is −0.29 /
−0.18 and −0.36 / −0.34. The trend with elevation is small: +0.10 / −0.05 (V1.0) and +0.06 / +0.14 (VisorSat)
per 10 deg.

## Against published measurements

### Starlink VisorSat (Mallama 2021)

The only dataset with individual observations. The model is sampled at each observer's measured window
(visual sites Sun −24 to −10 deg, elevation ≥ 30 deg; MMT Sun −24 to −11 deg, elevation ≥ 45 deg).

| Metric | Model | Observed | Delta | Kind |
|---|---|---|---|---|
| mean \( m_{1000} \) | 7.428 | 7.218 | +0.210 | gated |
| median | 7.500 | 7.365 | +0.135 | gated |
| phase-matched mean | 7.533 | 7.218 | +0.315 | informational |
| standard deviation | 0.442 | 0.854 | −0.41 | informational |
| linear phase slope | 0.002 mag/deg | 0.005 | −0.003 | informational |
| phase-curve RMS (scored bins) | 0.349 | — | — | reported, not gated |

By site the mean is 7.42 against 7.26 observed (Mallama), 7.44 against 7.41 (Respler) and 7.43 against 7.06
(MMT). The MMT rows of the table carry bright-attitude passes: their mean is 7.06 and their median 7.37.

**The phase-curve RMS is reported but not gated.** The residual is mostly an offset: the 40 to 100 deg bins are
0.33 to 0.45 mag too faint, 100 to 110 deg 0.18 too faint, 110 to 120 deg 0.09 too bright. The bin *means* carry
the bright-attitude passes that a nominal attitude cannot make, and the same model reads about 0.2 mag too
**bright** against MMT's own held-out tracks of these satellites (above): the table and the tracks disagree by
about 0.4 mag at the fitted geometry. The file lists `phase_curve_rms` under `ungated_metrics`, so its miss prints
`known`; the mean and median stay gated.

**The scatter is too small** (0.44 against 0.85) because every simulated satellite flies its nominal attitude
exactly and the measured windows keep only high passes. Real satellites jitter and fly varying roll; there is
no attitude-noise model. The same holds for every other benchmark that reports a standard deviation.

### VisorSat minus V1.0 (differential)

The model's visor dims the satellite by 1.523 mag against the published 1.29. The two models share chassis and
materials, so this isolates the visor. Its real shape is unpublished: its inset (0.39 m) is fitted to the MMT
tracks, and its gap (0.166 m) follows from the constraint that the antennas are fully shaded at 23° of Sun
depression (Cole 2021). The visor dims only through occlusion, so this benchmark is also the main check of the
occlusion model.

### Starlink V1.0 (Mallama 2020a)

5.904 against 5.93. MMT's own entry is sampled at Sun −33 to −11 deg and elevation ≥ 45 deg (from its V1.0
tracks); the visual sites as VisorSat's. Standard deviation 0.211 against 0.67 (one nominal attitude at a narrow
geometry).

The app's bulk export samples SatBench's default window (Sun −18 to −6 deg, elevation ≥ 20 deg), which is
shallower and lower than the measured windows. Under it the calibrated V1.0 reads 6.47 at 32.7° S (median 6.29),
about 0.5 mag fainter than the paper, so a bulk export is not comparable to the benchmark. In the same run
VisorSat reads 7.52 with part occlusion on and 6.24 with it off, the app's default, where the visor dims nothing.

### OneWeb (Mallama 2020b)

6.962 against 7.18. The bus's multi-layer insulation is **fitted** to this mean: the generic gold-foil MLI
preset (albedo 0.2, F0 0.6) reads 6.49, a dark MLI (albedo 0.1, F0 0.25) reads 6.97. No other OneWeb data is
held out. Standard deviation 1.1 against 0.68.

### Starlink V2 Mini, mitigated (Mallama et al. 2023)

7.965 against 7.87. The dielectric mirror film on the nadir face uses a Beckmann microfacet distribution,
chosen with this benchmark in view: with GGX's longer tail the film forward-scatters grazing terminator
sunlight towards the ground and the model reads 7.43. The mean agrees; the shape does not. Against the paper's
quadratic phase fit the model is about 1 mag fainter below 40° phase and 0.5–0.9 mag fainter at 80–140°, and
its standard deviation is 1.30 against 0.79. A second, later dataset of the same satellites
(arXiv:2502.03651) gives 7.22, which shows how much campaigns of one design differ.

### Starlink Direct-to-Cell, mitigated (Mallama et al. 2025)

6.438 against 6.47, **held out**: nothing was fitted to it, and it corroborates the V2 Mini's materials at a
lower altitude with a large phased array added. Standard deviation 1.02 against 1.32.

### Starlink V1.5 (Mallama et al. 2025)

6.065 against 6.34, a margin of 0.025 mag, the narrowest of all gated metrics. The array's translucent
backsheet has a diffuse transmission of 0.03, fitted to the Post-VisorSat phase function of Mallama & Respler
(2022), not to this dataset; even so, the low-phase bins of that phase function stay 0.6–0.8 mag too bright
(weighted RMS 0.63 against the fit). The observers, their split and the period are assumptions, and the
satellites' attitude over the period (including the knife-edge roll SpaceX introduced later) is not known.

### Amazon Leo, operational (Mallama et al. 2026)

6.663 against 6.81, **held out**. The layout comes from the authors' interpretation of imagery. Standard
deviation 0.94 against 0.64.

### BlueWalker 3 (Mallama et al. 2023b)

3.166 against 3.09 for the regular, nominal-attitude magnitudes, sampled at the window measured from the paper's
own Appendix B visual rows (Sun −24 to −10 deg, elevation ≥ 15 deg). The face albedo (0.32) was fitted to the
same visual data, so this benchmark guards against regressions rather than validating. Standard deviation 0.56
against 0.69.

A timed replay of the paper's passes (SGP4 with archived element sets at most 4 days old) reads 2.97 against 3.01
at the visual geometries. Its CCD photometry (Steward's 'Pomenis', 251 points) reads 0.5 mag brighter than the
visual estimates at the same geometry, consistently per pass to 0.14 mag: a calibration difference between the
data sets. The light curves' shapes agree to a within-pass rms of 0.35 mag (median absolute deviation 0.14). The
facet tilt (3°) was fitted to the 24 grazing-incidence points. Within about 30 km of tangent height of the Sun's
ray the satellite is 1 to 3 mag fainter than modelled: the evaluator's Earth shadow is geometric, with no
atmospheric dimming of grazing sunlight. In the faint periods, when the operator rolled the satellite toward the
Sun, the model reads 2 to 3.5 mag too bright; a 13 to 20° roll restores them.

### Guowang during orbit raising (not gated)

5.838 against 4.21: the model is 1.6 mag too faint. The hardware is known only as a box about 3 × 1 × 1 m with
a 10 m span, and the satellites were observed while raising their orbits, in an attitude nobody has
published. The model flies the operational attitude estimate. The file is `"gated": false`, so the miss is
reported in every run but does not fail the gate.

### Iridium flares

`iridium_block1.json` is a validation model of the first-generation Iridium satellite. It is not in the roster,
but it runs in the gate's model self-test (its occlusion p95 is 0.239 against the 0.25 limit). Each of its three
Main Mission Antennas (MMAs) is 1.88 × 0.86 m with its normal 50 deg from nadir: the front one faces the
velocity, the other two sit at ±120 deg. The MMA's silvered film is a near-mirror core (F0 0.88, GGX α 0.0005)
plus a [haze](../simulation/photometry.md#a-materials-haze) holding 0.3 of the specular reflectance at α 0.04.

The haze is fitted to Ron Lee's and Randy John's 1997 visual magnitudes against mirror angle (SKYSAT
`iridrpt.txt`). Without it the flare's tail at 0.6 to 12 deg reads about 3 mag too faint.

| Mirror angle | Model (median) | Lee/John (median) |
|---|---|---|
| 0.6–1° | −1.91 | −1.99 |
| 1–2° | −0.64 | −0.89 |
| 2–4° | 0.63 | 1.28 |
| 7–12° | 3.43 | 3.06 |

An MMT-9 light curve held out of the fit confirms the wing: an IRIDIUM 13 flare, at mirror angles 7 to 14 deg,
matches within 0.53 mag, where the mirror lobe alone is 1.5 to 1.7 mag too faint. Against 12 Heavens-Above
predictions (using element sets of the time) the geometry agrees within 0.5 deg in elevation and azimuth and
about 3 km rms in distance to the flare's centre line, and the drop of magnitude away from the centre line
within 0.37 mag rms. The model's centre magnitudes (−7.1 to −4.7) are about 1 mag fainter than Heavens-Above's,
whose −9 at about 830 km is brighter than any 1.62 m² flat mirror can make. The predictors' attitude frame (x
exactly along the velocity) differs from the model's (z exactly nadir) by up to 0.08 deg, which moves a flare's
peak by 0.3 to 0.5 s.

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
| Haven-1 | 2.98 | layouts of every future station from the operators' concept art and published dimensions |
| Haven-2 | 0.78 | four radially docked modules, a pair of wings on each |
| Axiom Station | 1.27 | curved body-mounted array plus Kapton wings on the power module |
| Orbital Reef | 0.34 | about 520 m² of rigid arrays on its aft truss |
| Russian Orbital Service Station | 1.44 | the published module plan |
| Bharatiya Antariksh Station | 1.52 | five 3.8 m modules |
| Starlab | 2.39 | four wings in a 45° cross, each turning about its own diagonal axis, so none can face the Sun squarely |
| GEO belt (four generic class models) | — | in the app, mag 11 to 16 seen away from glints (Astra 1KR 13.8, GOES-19 from Quito 11.4); real GEO comsats are typically 10 to 14 |
| Starlink V3, debris | — | render-level estimates |

The future stations' numbers come from the same V1.0-campaign copy run on each station's roster shell
(3000 samples, seed 1). The models are generated by `tools/make_station_models.py`; their sources are listed
per model in its `sources` block. Every wing turns about its own long axis, and their surfaces are estimates,
so these are render-level predictions, not validated magnitudes.

## Fitted and estimated values

- **Calibrated against MMT light curves** (both 2020 Starlinks, jointly): the polished bus `bus_metal` is
  Beckmann α 0.08, F0 0.9, diffuse 0.11; `antenna_white` diffuse 0.31; `visor_black` diffuse 0.019; the visor
  inset 0.39 m (gap 0.166 m at the 23° cutoff); Minnaert \( k = 0.65 \) on those three diffuse terms. Mallama
  (2020a) measured \( k = 0.578 \pm 0.084 \); the free fit gives 0.80, but the loss is nearly flat in \( k \).
  Beckmann rather than GGX for the bus: at MMT's geometry GGX's tail is about twice too bright away from the
  specular beam.
- **Calibrated against VisorSat:** the `solar_cell` diffuse albedo, 0.02 (from the low-phase bins, where the
  array face dominates; MMT is insensitive to it).
- **Calibrated against OneWeb:** the bus MLI (albedo 0.1, F0 0.25, roughness 0.35).
- **Calibrated against Mallama & Respler (2022):** the V1.5 backsheet transmission, 0.03.
- **Calibrated against BlueWalker 3's visual data:** its face albedo, 0.32, and facet tilt, 3°.
- **Calibrated against Lee & John (1997):** the Iridium MMA haze, 0.3 at α 0.04.
- **Chosen with a benchmark in view:** Beckmann for the dielectric mirror films (V2 Mini).
- **Estimates:** the antenna region's size on both 2020 Starlink models (its albedo is fitted and degenerate
  with the size); most material presets other than `solar_cell`; every station's surfaces. Hubble flies an
  anti-Sun stand-in attitude, since the simulator has no inertial-pointing law. The GEO belt flies four generic
  class models, not one per satellite, and the spin of MSG and Fengyun-2 is not modelled.

## Model approximations

Each of these is measured against an exact reference by the self-tests.

| Approximation | Error | Checked by |
|---|---|---|
| Occlusion between parts from 16 samples per component of a lobe, against a ray cast from ≥ 25 points per triangle | VisorSat p95 0.13, max 0.26; ISS p95 0.17, max 0.33; V2 Mini p95 0.07, max 0.36; Iridium p95 0.24; others ≤ 0.02 mag | `--selftest` (gate p95 < 0.25) |
| GPU occlusion skips lobes under 1e-4 of the total and satellites fainter than magnitude 10 | ≤ 0.005 mag | design choice |
| A cone occluder uses its larger radius | conservative: over-shadows | by construction |
| Earthshine table against the exact integral | p95 0.004, max 0.014 mag | `--selftest` |
| Earthshine on a tilted plane as an order-4 spherical-harmonic fit, floored at the vector value | ≤ 2.9% of the vector irradiance | `--selftest` |
| The Earth as a uniform Lambertian sphere, albedo 0.3 | no clouds, oceans, glint or seasonal albedo | model limit |
| Chapman-column extinction (fitted `erfcx`) against a brute-force ray integral | ≤ 0.32% of the column | `--selftest` |
| Extinction with fixed 8 km / 1.2 km scale heights and a 60/40 split | no weather or site haze | model limit |
| The Sun's disc as an anisotropic \( \alpha^2 \) pair, against the lobe convolved with a limb-darkened disc | peak: GGX ≤ 0.006 mag, Beckmann ≤ 0.036 mag, incidence 0–75° and α 0–0.3 (≤ 0.05 at 80°); the reflected beam is round but its half-maximum radius is about 0.15° against the disc's 0.26° | brute-force study |
| The Earth's shadow is geometric | no atmospheric dimming of grazing sunlight: 1–3 mag too bright within ~30 km of tangent height | model limit |
| Facets merged beyond the lobe budget (48, or 256 for rosters of 10 000 or fewer) | exact for the shipped models within budget, except the ISS (514 exact lobes): p95 0.08, max 5.6 mag at 256, in rare geometries where module-cylinder facets merge; Hubble at a forced 24: p95 0.20 | lobe validator |

## Frame, precision and scope

- **GPU orbit math is float32.** The orbital phase is formed exactly; what remains is float rounding of the
  final angle and position, a few metres. It exceeds the 0.02 mag parity threshold only with the camera very
  close to a satellite; close-up rendering uses a double-precision, camera-relative path.
- **Benchmarks exclude penumbra**, as the papers do. The app draws satellites in penumbra.
- **Legacy two-surface types are not physical magnitudes.** They cannot be traced, exported or benchmarked
  until they have geometry models.
- **SatBench comparisons are above the atmosphere.** Only the censoring rule touches apparent magnitudes, and it
  compares above-atmosphere magnitudes with the observers' apparent limit
  ([details](satbench.md#drawing-one-observation)). The light-curve replays (`--pass`) compute apparent
  magnitudes with extinction from the observer's real height.

## Where in the code

- `data/benchmarks/KNOWN_RESIDUALS.md`: the maintained record of everything on this page.
- `cmake/AccuracyGate.cmake`: the configuration these numbers come from.
- Reports: `<build dir>/accuracy_gate/benchmark_runs/*.json` after a gate run.
- `data/satellite_models/iridium_block1.json`, `starlink_v1_0.json`, `starlink_visorsat.json`,
  `bluewalker3.json`: the validation and benchmark models.
- `tools/sat_model_tool/pass_eval.cpp`: `SatModelTool --pass`, the light-curve replay.
