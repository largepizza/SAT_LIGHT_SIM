# SatBench and the accuracy gate

This page describes the machinery that turns a satellite model and a [benchmark dataset](benchmarks.md) into
a pass or a fail: the CPU photometric evaluator and its parity with the GPU, SatBench's campaign simulation,
the self-tests, the magnitude-trace and bulk-export tools, and the `accuracy-gate` target that CI runs. The
current numbers are on [Results and known residuals](results.md).

Everything here is driven from `SatModelTool`, a command-line program that runs the same model pipeline the
app runs at startup (load, tessellate, bake facet lobes, build occlusion) without a window or a GPU. It is
not part of the default build:

```bash
cmake --build build --config Release --target SatModelTool
```

## The CPU photometric evaluator

`evalSatPhotometry()` (`src/simulations/SatPhotometry.cpp`) computes one satellite's brightness at one
instant, in double precision. Its inputs are the type's attitude groups and baked lobes, the satellite's
orbit elements, a time in seconds since J2000, the Sun direction and the observer's position. The chain is:

1. **Orbit.** `satOrbitStateAt()`: a circular orbit, closed form in time (position, nadir, along-track
   velocity, tumble angle), with sun-synchronous RAAN precession where the constellation asks for it.
2. **Attitude.** `evalGroupPoses()` poses each attitude group from the Sun, nadir, velocity and, for mirrors
   aimed at a ground site, the site the lock-window search picks (`satGroundSiteIdeal()`).
3. **Light.** The Earth-shadow factor; earthshine from the lit cap of the Earth; each lobe's Lambert +
   microfacet reflection and diffuse transmission; occlusion between parts if enabled.
4. **Magnitude.** \( m = -26.74 - 2.5 \log_{10}(I / r^2) \) above the atmosphere, the 1000-km magnitude,
   phase angle, off-specular angle, and the dominant lobe (which surface the observer mostly sees).

The reflectance model itself is described in [Satellite photometry](../simulation/photometry.md). The time
and frame helpers (`sunDirEciAt()`, `observerEciAt()`, `earthRotationAngle()`) are the single copies of those
formulas; the app's own position update calls them, so the evaluator and the app cannot disagree about where
the Sun or the observer is.

Every measured number in the project comes from this function: SatBench, the selection panel's magnitude,
magnitude traces and bulk exports. The GPU's per-satellite shader (`shaders/sat_orbit.comp`) computes the same
thing in float for every satellite every frame; the evaluator is its hand-maintained mirror.

!!! warning "Invariant"
    `evalSatPhotometry()` and `satOrbitStateAt()` must stay in step with `sat_orbit.comp`'s orbit block and
    `modelFlux()`, and `evalGroupPoses()` with the shader's group frames. GLSL and C++ cannot share code, so a
    change to one side that is not mirrored on the other shows up as a parity mismatch (below), not as a
    compile error.

### GPU parity check

While a satellite is selected, the app records the exact inputs of each frame's orbit dispatch (sim time, Sun,
observer, flare-mitigation tilt, brightness scale, occlusion on or off). On the next frame
`updateSelectedPhotometry()` re-evaluates the selection with the evaluator at those inputs and compares its
raw flux with the value the GPU wrote for that satellite. A difference above 0.02 mag is logged to
`satlight_log.txt` (at most every 120 frames). Pairs where both sides are fainter than about magnitude 20 are
skipped: there both are float noise.

The gap that remains is float32 against double. The orbital phase product \( u_0 + n\,\Delta t \), the one
large-number operation, is computed on the GPU as an exact two-float product, which leaves about 1 m median
and 9 m maximum of position error from rounding of the final angle. That is far below 0.02 mag from the ground;
it can exceed it only with the camera within a few kilometres of a satellite.

## SatBench: simulating a campaign

`runDistributionBenchmark()` (`src/simulations/SatBench.cpp`) reproduces an observation campaign with a model.
The idea is to measure the model **the way the observers measured the satellites**: same sites, same season,
twilight only, satellites only when they are high enough and fully sunlit, and the same censoring of what the
eye could not see. Then the simulated magnitudes are summarised exactly as the paper summarised its own.

### Drawing one observation

Repeat until the requested number of samples is drawn:

1. **Site.** Pick an observing site with probability proportional to its weight. The weight is the number of
   the paper's own observations attributed to that observer (an observation's `source` appearing in the site
   name); failing that, the site's published `count`; failing both, equal weights.
2. **Time.** Pick a time uniformly within the benchmark's period (the end date inclusive). Reject it unless
   the Sun's altitude at the site is within the twilight window (default \(-18°\) to \(-6°\)).
3. **Satellite.** Draw a random satellite of the shell: a circular orbit at the shell's altitude and
   inclination, with uniform right ascension of the ascending node and uniform argument of latitude at that
   time. Evaluate it; accept it if it is at or above the minimum elevation (default 20°) and fully sunlit
   (shadow factor ≥ 0.999, so penumbra is excluded as the papers exclude it). Up to 4000 satellites are tried
   per time; the first accepted one becomes the sample.
4. **Censoring.** At a site whose instrument is `visual`, if the satellite's magnitude is fainter than the
   observers' limit (`limiting_mag_apparent`), the sample is censored: its \( m_{1000} \) becomes the paper's
   assigned "not seen" magnitude reduced from that range. Censored samples stay in the statistics, as the
   papers keep their "not seen" rows.

Because each accepted time contributes one satellite chosen uniformly among the visible ones, the samples are
a time-uniform draw over visible, sunlit satellites. That is the same population the app's bulk export
samples on a fixed cadence (below), which is what lets the two be compared.

!!! note "Censoring compares above-atmosphere magnitudes"
    SatBench evaluates with zero extinction, so the censoring test compares the satellite's above-atmosphere
    magnitude with the observers' apparent limit. A satellite low in the sky is therefore treated as brighter
    than the observer saw it by its line-of-sight extinction (a few tenths of a magnitude at 20° elevation),
    and is censored less often than it would have been.

### Assumptions are configuration, and are reported

Papers rarely state their selection limits. Everything SatBench needs and the paper does not give is a field
of `BenchRunConfig`, and every report echoes it:

| Field | Default | Why |
|---|---|---|
| `minElevationDeg` | 20 | the papers do not state an elevation limit |
| `sunAltMinDeg`, `sunAltMaxDeg` | −18, −6 | twilight to astronomical night; not stated |
| `periodStartFallback` | 2020-01-01 <!-- history-ok --> | for a period with no stated start |
| `censor` | true | apply the paper's censoring rule at visual sites |

`--sensitivity` reruns the same benchmark four more times with the elevation limit at 10° and 30° and the Sun
window at −12…−6° and −18…−12°, and reports each run's mean and median and its shift against the base run.
Those shifts are the size of the uncertainty that the unpublished selection contributes.

### Determinism

A run is a pure function of the model file, the benchmark file, the sample count and the seed. The only
randomness is a `std::mt19937_64` seeded with the seed; its output sequence is fixed by the C++ standard. Its
64-bit outputs become doubles in \([0, 1)\) by keeping the top 53 bits (`(x >> 11) · 2⁻⁵³`). The standard
library's distributions (`std::uniform_real_distribution` and the like) are deliberately not used, because
their algorithms are implementation-defined and differ between MSVC, libstdc++ and libc++. The result is that
the same seed gives the same samples, bit for bit, on every platform: reports from MSVC on Windows and GCC on
Linux match to the printed precision.

### What is compared

Statistics of the simulated \( m_{1000} \) are computed with the same function that checks a transcription
(`benchStats()`), then compared with the reference: the statistics recomputed from the paper's observations
when the file has them, otherwise the paper's printed values.

| Metric | Verdict | Tolerance |
|---|---|---|
| `mean_m1000` | gated | ±0.3 mag |
| `median_m1000` | gated (when the reference has one) | ±0.3 mag |
| `phase_curve_rms` | gated (observations only) | 0.3 mag |
| `delta_mean_m1000` (differential) | gated | ±0.3 mag |
| `sd_m1000` | informational | — |
| `mean_m1000_phase_matched` | informational (observations only) | — |
| `phase_slope_mag_per_deg`, `phase_fit_at_90deg` | informational | — |

A run passes when no gated metric fails. The 0.3 mag tolerance is provisional. It is much wider than the
standard errors of the published means (0.02 to 0.09 mag) because those reflect only the number of
observations, not the observers' unpublished selection of passes or the satellites' unpublished attitude;
independent campaigns of the same satellites can differ by more than that (two V2 Mini datasets give 7.87 and
7.22).

**Phase-matched mean.** Observers do not sample geometry the way a random visible satellite does: which passes
they choose, and when, narrows their distribution of phase angles. That selection is unpublished. The
phase-matched mean reweights the simulated samples so that their histogram of phase angle (18 bins of 10°)
matches the observed one, then takes the mean. It compares like with like, at the cost of depending on the
observed phase distribution. It is reported with the number of observed bins the simulation never reached.

**Phase-curve RMS.** A model can match the mean while being too bright at one end of the phase curve and too
faint at the other. Over 10° phase bins, the simulated and observed mean \( m_{1000} \) are differenced; bins
with at least 10 observations are scored, and the RMS of the differences, weighted by each bin's observation
count, must be under 0.3 mag. The report lists every bin, scored or not.

Each report also tabulates the **dominant surface**: for each lobe, the share of samples in which it is the
brightest, labelled by group and body-frame normal. This answers "which surface is the observer actually
seeing" and is usually the first thing to read when a benchmark misses.

### Differential runs

A differential file names two distribution files. SatBench runs each one with its own model, sites and
period, then compares the difference of the two simulated means with the published difference. The two
sub-reports are written alongside the differential report.

### Running it

```bash
# one benchmark, the gate's configuration
SatModelTool --run-benchmark data/benchmarks/mallama2021_visorsat.json --samples 5000 --seed 1

# with the sensitivity study, reports to a chosen directory
SatModelTool --run-benchmark data/benchmarks/mallama2023_v2mini.json --sensitivity --report-dir runs/v2

# without occlusion between parts
SatModelTool --run-benchmark data/benchmarks/visorsat_vs_original.json --no-occlusion

# a material override, for a calibration scan
SatModelTool --run-benchmark data/benchmarks/mallama2020b_oneweb.json \
    --set oneweb/oneweb_mli.diffuse_albedo=0.15
```

| Option | Default | Effect |
|---|---|---|
| `--samples N` | 2000 | number of simulated observations (the gate uses 5000) |
| `--seed S` | 1 | random seed |
| `--sensitivity` | off | the four alternative-assumption reruns |
| `--no-occlusion` | occlusion on | drop occlusion between parts |
| `--budget N` | 256 | lobe budget; 256 bakes every benchmark model's lobes exactly |
| `--report-dir D` | `benchmark_runs` | where reports and sample CSVs go |
| `--models-dir D` | `<benchmark dir>/../satellite_models` | where the model named by the benchmark is loaded from |
| `--set [<model>/]<material>.<field>=<value>` | none | override `diffuse_albedo`, `specular_f0`, `roughness` or `distribution` (`ggx`/`beckmann`) after loading; repeatable; echoed into the report |

Occlusion between parts is on by default in SatBench even though it is off by default in the app, where it is
an opt-in for performance. SatBench is a physics check, so it uses the full model.

`tools/benchmarks/scan_overrides.py` runs the VisorSat, V1.0 and differential benchmarks for a list of
override sets with one seed and tabulates them, so calibration choices are made on identical samples.
`tools/benchmarks/plot_benchrun.py <report.json>` writes, next to a distribution report, the simulated and
observed \( m_{1000} \) histograms and \( m_{1000} \) against phase angle with binned means (it needs
matplotlib).

### The report: `sat-light-sim-benchrun/1`

Each run writes `<benchmark id>__<model id>__seed<S>.json` in the report directory:

| Key | Contents |
|---|---|
| `benchmark` | id, file path, FNV-1a 64-bit hash of the file, citation URL |
| `model` | id, file, FNV-1a hash, lobe budget, lobe count, exact lobe count, occlusion description, applied overrides |
| `config` | the `BenchRunConfig` used (samples, seed, elevation limit, Sun window, fallback, censoring) |
| `reference` | source (observations or published summary), n, mean, median, sd, linear phase fit |
| `result` | n, censored count, mean, median, sd, standard error, linear phase fit, share brighter than \( m_{1000} = 5 \), satellite and time draws |
| `comparison` | one row per metric: model, reference, delta, tolerance, verdict (`pass`/`fail`/`info`/`n/a`) |
| `phase_curve` | per 10° bin: observed and simulated counts and means, delta, whether scored |
| `sensitivity` | the alternative runs, when requested |
| `dominant_surface` | per lobe: group, body normal, area, albedo, share |
| `samples` | every simulated observation (time, site, Sun altitude, elevation, range, phase, off-specular angle, magnitude, \( m_{1000} \), censored, dominant lobe) |
| `reference_observations` | the observations compared against |
| `git`, `run_utc` | commit and whether the tree had uncommitted changes; the run time |

The file hashes and commit make a result traceable: a report says exactly which model file, which benchmark
file and which code produced it. Apart from `run_utc` (and `git` if the commit differs), two runs with the same
inputs produce identical reports. The same samples are also written as
`<benchmark id>__<model id>__seed<S>.samples.csv` in the samples format below.

## Self-tests

`SatModelTool <model.json>... --selftest N` checks the evaluator against independent references. The
model-independent checks run once; the rest run per model with N configurations.

| Check | Reference | Gate |
|---|---|---|
| Sun declination at the 2020 June solstice and March equinox | almanac values 23.44° and 0° | within 0.05° |
| Orbit radius, velocity ⟂ nadir, numerical derivative of position, inclination from \( r \times v \) | construction | 1 mm, 1e-12, 1e-5 relative, 1e-9° |
| Off-specular angle at a constructed mirror image, phase angle toward the Sun, range | construction | 1e-6 rad, 1 mm |
| Magnitude of \( I/r^2 = 10^{-12} \); 5.92 at 550 km reduced to 1000 km | 3.26; 7.218 | 1e-9; 1e-3 |
| Earthshine closed-form ring integral | brute-force double integral over the visible cap | 1% irradiance, 0.01 rad tilt |
| Earthshine table lookup (what the GPU reads) | exact integral | p95 < 0.02 mag, max < 0.15 mag |
| Earthshine order-4 spherical-harmonic plane irradiance | brute force | < 5% (exact), < 6% (table) of the vector irradiance |
| Chapman-function extinction column | brute-force ray integral, sea level to 400 km, +90° to −19° | < 2% relative |
| Posed lobes (exact bake) | posed per-triangle brute force, observers within ~25° of the sub-satellite point, brighter than mag 20 | max < 0.001 mag |
| Ground-site mirror aim (mirror types) | where the reflected beam lands | median miss < 0.01° |
| Occlusion between parts (16 samples per lobe component) | ray cast from ≥ 25 points per triangle | p95 < 0.25 mag |
| Occlusion in the GPU's packed float form | the double-precision CPU form | < 0.5% of lobe evaluations differ; offsets < 1e-4 m |
| Trace round trip | a pass written, read back and replayed | every row identical |
| Bulk-export round trip | 400 satellites × 2 days at 60 s, written, read and rewritten | byte-identical |

Lobe budgets also matter: a type flown by many satellites bakes its facets into at most 48 lobes (256 for
rosters of 10 000 or fewer satellites), merging similar facets. The self-test reports the budgeted bake's
error against the brute force as information; the accepted values are in [Results](results.md#model-approximations).

The tool also prints each model's **provenance** (counts of sourced, derived, calibrated and estimated parts,
every non-sourced entry, and any part with no `sources` entry at all); see [Model provenance](provenance.md).

## Magnitude traces

The selection panel's **Trace pass** button runs the evaluator over the selected satellite's current pass, or
its next one within two orbital periods, at 400 points, and plots apparent magnitude (after extinction),
above-atmosphere magnitude and phase angle against time. With **Live** on it retraces up to 10 times a second
whenever the result would change (the observer moved, the pass ended, a setting changed). A legacy type, or a
ground-site mirror with no targets loaded, cannot be traced, and the window says why.

**Export CSV** writes `<user data>/traces/trace_<model>_<satellite>_<sim time>.csv`, format
`sat-light-sim-trace/1`. The header (`#`-prefixed `key: value` lines) carries every input the evaluator used:
the model id and file hash, the lobe budget, whether occlusion was applied (exactly, with no brightness floor),
the orbit elements as the app baked them, the observer's Earth-fixed direction and radius, the
flare-mitigation tilt, the extinction coefficient, and for ground-site mirrors the targets and lock-window
settings. Inputs are written at full precision (`%.17g`). The columns are:

`t_j2000, sun_alt_deg, elevation_deg, azimuth_deg, range_m, phase_deg, off_specular_deg, lit_factor, mag,
m1000, extinction_mag, mag_apparent, dominant_lobe`

A satellite in the Earth's shadow leaves its magnitude fields empty.

```bash
SatModelTool --replay-trace traces/trace_starlink_v2_mini_1234_<time>.csv --models-dir data/satellite_models
```

Replay rebuilds the model from the header, re-evaluates every row at its time and compares each row's output
fields **as the formatted strings the CSV holds**. The check is that a trace means exactly what it says: any
row that does not come out the same is printed. If the model file has changed since the export, the hash
mismatch is reported and the replay still runs, so the report says how much the change moved the trace.

## Bulk export and the samples CSV

Settings → Photometry → **Bulk export** samples a whole population as SatBench does, but at the app's own
observer and time. Choose the source (the selected satellite or a constellation), the window (next 24 h or
next 7 days from the sim time) and the cadence (10, 30 or 60 s). A worker thread (`runBulkExport()`) visits
every instant at the cadence with the Sun in SatBench's twilight window, and for every satellite of the
source records those instants where it is at or above 20° and fully sunlit, evaluated with `benchEvalSample()`, the
same function SatBench uses. Consecutive visible instants of one satellite are numbered as one `pass`.

The output is `<user data>/exports/samples_<source>_<sim time>.csv`, format `sat-light-sim-samples/1`, which
is the one schema for simulated observations: SatBench's `.samples.csv` uses it too. The header records the
source, model and hash, type, lobe budget, occlusion, satellite count, observer, window, cadence, elevation
limit, Sun window, extinction coefficient, app version and commit. The columns are:

`t_j2000, site, sun_alt_deg, elevation_deg, azimuth_deg, range_m, phase_deg, off_specular_deg, mag, m1000,
mag_apparent, censored, dominant_lobe, satellite, pass`

`satellite` is the roster index (−1 for SatBench's random draws), and `mag_apparent` adds the line-of-sight
extinction at the app's extinction coefficient (equal to `mag` in SatBench files, which use none).

```bash
SatModelTool --summarize-samples exports/samples_Starlink_Gen1_<time>.csv
```

prints n, censored count, mean, median, standard deviation, linear phase fit and mean apparent magnitude. For
a SatBench file, whose header carries the run's own statistics, it also checks that it reproduces them (to
5e-6), which ties the CSV to its report.

## The accuracy gate

```bash
cmake --build build --config Release --target accuracy-gate
```

The target builds `SatModelTool` and runs `cmake/AccuracyGate.cmake`, which is **the only list** of gated
checks. In order:

1. Every `data/satellite_models/*.json` through `--selftest 400` (all the self-tests above, including the
   trace and bulk-export round trips).
2. Every `data/benchmarks/*.json` through `--benchmark` (the transcription check).
3. Every `data/benchmarks/*.json` through `--run-benchmark --samples 5000 --seed 1`.

Any step that exits non-zero fails the target, and the failing steps are named. Reports and sample CSVs go to
`<build dir>/accuracy_gate/benchmark_runs`. The model and benchmark lists are globs, so a new model or dataset
is gated as soon as it is added. A full run takes about 40 seconds.

CI (`.github/workflows/release.yml`) runs the gate in the Linux job, between building and packaging, on every
push to `main`, every pull request targeting `main`, and every release tag, through the
`linux-accuracy-gate` build preset. A change that moves a satellite's brightness out of tolerance, or breaks a
transcription, fails the pull request. One platform suffices because the tool is platform-independent C++
with deterministic sampling.

!!! tip "When a change moves a number"
    A change to a material preset, the earthshine model, the lobe baking or the evaluator will move benchmark
    results even when it passes. Re-run the gate and update `data/benchmarks/KNOWN_RESIDUALS.md` with the new
    numbers, so the next change is judged against where this one left things.

The gate is deliberately not a model optimiser: its tolerances are not tightened to chase the closest
benchmark, and a dataset the model is known not to reproduce is marked ungated rather than tuned towards.

## Where in the code

- `src/simulations/SatPhotometry.h/.cpp`: `evalSatPhotometry()`, `satOrbitStateAt()`, `satGroundSiteIdeal()`,
  earthshine, extinction, `sunDirEciAt()`, `observerEciAt()`, `earthRotationAngle()`.
- `src/simulations/SatBench.h/.cpp`: `BenchRunConfig`, `runDistributionBenchmark()`, `benchEvalSample()`,
  `runBulkExport()`, `writeBenchSamplesCsv()`/`readBenchSamplesCsv()`.
- `src/simulations/SatTrace.h/.cpp`: `evalSatTraceRow()`, `satTracePassWindow()`, the trace CSV.
- `src/simulations/SatelliteSim.cpp`: `updateSelectedPhotometry()` (parity check), `exportTrace()`,
  the bulk-export job; `SatelliteSimUI.cpp` for the trace window and the Photometry tab.
- `tools/sat_model_tool/main.cpp`: the CLI, self-tests, `checkBenchmark()`, `replayTrace()`,
  `summarizeSamples()`; `tools/sat_model_tool/bench_run.cpp`: `--run-benchmark` and the report.
- `cmake/AccuracyGate.cmake`, `CMakeLists.txt` (`accuracy-gate` target), `CMakePresets.json`
  (`linux-accuracy-gate`), `.github/workflows/release.yml`.
- `tools/benchmarks/scan_overrides.py`, `tools/benchmarks/plot_benchrun.py`.
