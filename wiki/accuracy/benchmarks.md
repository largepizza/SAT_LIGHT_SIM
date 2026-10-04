# Benchmark datasets

A benchmark dataset is one published observation campaign, transcribed into a JSON file in
`data/benchmarks/` so that the model can be compared against it traceably. This page describes the file
format, the rule that decides when a transcription is trusted, and every dataset that ships. How a dataset
is simulated is on [SatBench and the accuracy gate](satbench.md); the current numbers are on
[Results and known residuals](results.md).

## The format: `sat-light-sim-benchmark/1`

A file is a single JSON object. The loader (`loadBenchmark()` in `src/simulations/SatBenchmark.cpp`) rejects
a file whose `format` is anything else, whose `id` is empty, or whose `kind` is not `distribution` or
`differential`.

| Key | Contents |
|---|---|
| `format` | `"sat-light-sim-benchmark/1"` |
| `id` | stable identifier, the file stem by convention (`mallama2021_visorsat`) |
| `title` | one line |
| `kind` | `distribution` or `differential` (below) |
| `citation` | `authors`, `year`, `title`, `url` |
| `satellite` | `name`, `model` (a `data/satellite_models/<id>.json` stem, or `null` for none), `notes` |
| `magnitudes` | `band`, `atmosphere` (above or not), `reduction` (`1000 km`), `locator`; descriptive |
| `published` | the paper's printed statistics, each an object with a `locator` |
| `sampling` | how the observations were taken: `shell`, `period`, `sites`, `constraints`, `censoring` |
| `references` | differential only: `test` and `baseline`, the ids of two distribution files |
| `transcription` | `source`, `method`, `date`, `check`: how the numbers were read and how they were verified |
| `columns`, `observations` | individual measurements, when the paper publishes them |
| `gated` | optional, default `true`; `false` reports a miss without failing (below) |

### Published statistics and locators

Every printed number the comparison uses lives under `published`, keyed by what it is:

| Key | Meaning | Fields |
|---|---|---|
| `n` | number of magnitudes | `value` |
| `mean_m1000` | mean 1000-km magnitude | `value`, `uncertainty` (standard error) |
| `median_m1000` | median | `value` |
| `sd_m1000` | standard deviation | `value` |
| `phase_fit_linear`, `phase_fit_quadratic`, `phase_fit_cubic` | polynomial fit of \( m_{1000} \) against phase | `coefficients` (constant first), `coefficient_uncertainties`, `variable` (`phase_deg` or `phase_rad`) |
| `delta_mean_m1000` | differential: test mean minus baseline mean | `value`, `uncertainty` |

Each entry carries a `locator`, the section, table or figure of the paper the value was read from, so any
number can be checked against its source in a minute. When a value is not printed but follows from printed
ones, the locator says so and shows the arithmetic; for example a population size derived as
\( (\sigma / \sigma_{\bar m})^2 \) from a printed standard deviation and standard error is marked
`DERIVED`.

### Sampling

`sampling` is what lets a simulation draw its observations the way the paper drew them.

- `shell`: `alt_km` and `incl_deg` of the satellites observed, with a `locator`. Several papers do not state
  the inclination; the locator then names the operational shell it was taken from.
- `period`: `start` and `end` as ISO dates; `start` may be `null` when the paper does not state it (SatBench
  then uses a fallback date).
- `sites`: each observer with `name`, `lat_deg`, `lon_deg` (east positive), `instrument` and, where
  published, `count` (observations from that site). The word `visual` in `instrument` matters: the censoring
  rule applies only at visual sites.
- `constraints`: `penumbra_excluded`, `sun_altitude_deg`, `min_elevation_deg` and a `note`. A `null` is an
  honest "the paper does not say", and SatBench fills it with an explicit, reported assumption.
- `censoring`: visual observers report satellites they could not see. `limiting_mag_apparent` is the faintest
  apparent magnitude they could see, `not_seen_assigned_apparent` the magnitude the paper assigned to a
  satellite that was not seen, and `count` the number of such rows.

### Observations

When the paper lists its measurements, they are stored as arrays whose meaning is given once in `columns`:
`sat`, `day_2020`, `ops_days`, `phase_deg`, `m1000`, `source` (the observer) and `not_seen` (a censored row).
The loader recognises those names. Only one shipped file has observations: Mallama (2021), 430 rows from its
appendix. The other papers publish summary statistics only, and their files say so in `transcription.check`.

### Kinds

- **`distribution`**: one population's magnitudes. Compared on mean and median (gated), standard deviation
  and phase slope (informational), and, when observations exist, phase-matched mean (informational) and
  phase-curve RMS (gated).
- **`differential`**: the published difference between two distributions, named in `references`. The test
  for a differential is the difference of two simulated means against the published difference. It isolates
  what differs between two designs from what they share: VisorSat and the original Starlink share a chassis
  and materials, so their difference tests the visor's occlusion, not the material calibration.

### `gated`

A file with `"gated": false` (and a `gated_note` explaining why) is run and reported like any other, and a
miss is printed as a failure, but it does not fail `SatModelTool` or the accuracy gate. This exists for a
dataset the model is known not to reproduce yet, so that the miss stays visible in every report rather than
the file being dropped. It is not a way to silence a regression: every ungated file is listed with its gap in
`KNOWN_RESIDUALS.md`. One file is ungated: Guowang during orbit raising.

## The transcription-trust rule

A benchmark is only as good as its transcription. A digit read wrongly from a PDF table would turn the
comparison into a comparison against a typo, so a file is trusted only once it **reproduces its own paper**:

```bash
SatModelTool --benchmark data/benchmarks/mallama2021_visorsat.json
```

For a file with observations, `checkBenchmark()` (`tools/sat_model_tool/main.cpp`) recomputes from the rows
and requires:

| Check | Tolerance |
|---|---|
| observation count equals `n` | exact |
| censored rows equal `censoring.count` | exact |
| mean, median, standard deviation of \( m_{1000} \) | within 0.006 mag (half a unit in the printed second decimal, plus slack) |
| each published phase fit, refitted at the same degree and variable | the two curves within 0.02 mag at every whole degree over the observed phase range |

Fits are compared as curves rather than coefficient by coefficient because the coefficients of a polynomial
fit are strongly correlated: two fits can differ noticeably in each coefficient and still describe the same
curve.

For a differential file the check is that the published difference equals the referenced files' published
means subtracted (within 0.006 mag). A summary-only file has nothing to recompute; the tool prints its values
with their locators and passes, and its `transcription.check` says it is summary only.

The VisorSat file reproduces \( n = 430 \), the 7 censored rows, mean 7.218 (printed 7.22), median 7.365
(printed 7.36), standard deviation 0.854 (printed 0.85) and both phase fits. When observations exist,
SatBench compares against the statistics recomputed from them, since they have been shown to match the paper
and carry more digits.

This check runs in the accuracy gate for every file, so a hand edit that breaks a transcription fails CI.

## The datasets

All campaigns are by A. Mallama and co-observers, using visual observers with binoculars and the
Mini-MegaTORTORA (MMT9) robotic camera in the Caucasus. Magnitudes are above the atmosphere (differential
photometry against nearby catalogue stars, or extinction-corrected), in V or close to it.

| File | Citation | Satellite | Model | n | Published mean \( m_{1000} \) | Data | What it tests |
|---|---|---|---|---|---|---|---|
| `mallama2020a_original` | Mallama 2020, arXiv:2006.08422 | Starlink V1.0 (original, no visor) | `starlink_v1_0` | 830 | 5.93 ± 0.02 | summary | the bare chassis; held out of all fitting |
| `mallama2021_visorsat` | Mallama 2021, arXiv:2101.00374 | Starlink VisorSat | `starlink_visorsat` | 430 | 7.22 ± 0.04 | 430 rows | mean, median, phase curve; the visor size is fitted to its phase curve |
| `visorsat_vs_original` | Mallama 2021, Table 2 | VisorSat minus V1.0 | both | — | difference 1.29 ± 0.045 | differential | occlusion by the visor |
| `mallama2020b_oneweb` | Mallama 2020, arXiv:2012.05100 | OneWeb Gen1 at 1200 km | `oneweb` | 639 | 7.18 ± 0.03 | summary | a different bus and altitude; the MLI is fitted to this mean |
| `mallama2023_v2mini` | Mallama, Cole, Harrington & Maley 2023, arXiv:2306.06657 | Starlink V2 Mini, mitigated | `starlink_v2_mini` | 77 (derived) | 7.87 ± 0.09 | summary | SpaceX's mitigations: mirror film, black paint, limb-tracking arrays |
| `mallama2025_dtc` | Mallama, Cole, Respler & Harrington 2025, arXiv:2502.03651 | Starlink Direct-to-Cell, mitigated | `starlink_v2_mini_dtc` | 484 (derived) | 6.47 ± 0.06 | summary | held out; corroborates the V2 Mini materials at 360 km |
| `mallama2025_starlink_v15` | Mallama et al. 2025, arXiv:2507.00107, Table 1 | Starlink V1.5 | `starlink_v1_5` | 268 | 6.34 ± 0.05 | summary | the gen-1 mirror film and translucent backsheet |
| `mallama2026_amazon_leo` | Mallama, Cole, Respler & Harrington 2026, arXiv:2601.07708 | Amazon Leo, operational | `amazon_leo` | 758 | 6.81 ± 0.02 | summary | held out; a layout read from the authors' interpretation of imagery |
| `mallama2025_guowang` | Mallama et al. 2025, arXiv:2507.00107, Table 1 | Guowang, orbit raising | `guowang` | 455 | 4.21 ± 0.04 | summary | **not gated**: unpublished hardware and attitude |

### Notes on individual files

**Mallama 2020a (V1.0).** Six observers including MMT9, with per-site counts from the paper's Table 1. The
start of the period is not stated (the paper covers observations "through May 2020"). Selection limits are
not stated. It also prints a flat-panel fit (magnitude against the projected nadir area toward observer and
Sun), stored for reference and not compared.

**Mallama 2021 (VisorSat).** Two visual observers and MMT. The observations are the appendix's rows,
transcribed with the "not seen" rows (printed in red) flagged from the page images. The censoring rule:
visual observers' limit is apparent magnitude 8.0 and a satellite not seen is assigned 8.5. The site weights
in a simulation come from how many rows each observer contributed.

**Mallama et al. 2023 (V2 Mini).** Only the mitigated population is modelled; the model does not fly the
early, unmitigated attitude. The paper does not print the mitigated count, so `n` is derived from its
standard deviation and standard error. The per-observer counts split all observations, not only the
mitigated ones. 18 of 506 attempts were not seen (both populations).

**Mallama et al. 2025 (DTC).** The "late" observations, after SpaceX adjusted the attitudes to dim the
satellites. The observer list and split are not published: MMT9 plus the four authors, weighted equally (an
assumption written into the file).

**Mallama et al. 2025, arXiv:2507.00107 (V1.5, Guowang).** Both rows come from one summary table, whose text
export shifts the count column by one row; it was realigned by hand, as `transcription.method` records.
Observers, split and period are assumptions. For Guowang the satellites were orbit raising, from about
900 km to 1170 km (mean 1053 km), in an attitude nobody has published.

**Mallama et al. 2026 (Amazon Leo).** Satellites holding 630 km in the operational attitude, from the paper's
Table 3. Observations by a 50 cm telescope of orbit-raising satellites are excluded.

!!! note "Satellites with no dataset"
    The ISS, Tiangong, the commercial stations, Starlink V3, the Starship depot, the Reflect Orbital mirror,
    the SpaceX AI satellite, Hubble and debris have no benchmark. Their magnitudes are model predictions with
    no measurement behind them; see [Model provenance](provenance.md) for what each is built from.

## Adding a dataset

1. Transcribe the paper into a new `data/benchmarks/<id>.json` following an existing file of the same kind.
   Give every published value a `locator`. Write `null` for anything the paper does not state.
2. Name an existing model in `satellite.model`, or add one ([Satellite models](../modding/satellite-models.md))
   with a full `sources` block.
3. Run `SatModelTool --benchmark <file>`. If the paper lists observations, every check above must pass before
   the file is used for anything.
4. Run `SatModelTool --run-benchmark <file> --samples 5000 --seed 1` and record the result, with its cause if
   it misses, in `data/benchmarks/KNOWN_RESIDUALS.md`.

The accuracy gate picks the new file up automatically: it globs the directory.

## Where in the code

- `src/simulations/SatBenchmark.h/.cpp`: `Benchmark`, `loadBenchmark()`, `benchStats()`, `benchPolyFit()`.
- `tools/sat_model_tool/main.cpp`: `checkBenchmark()`, the `--benchmark` command.
- `data/benchmarks/*.json`: the datasets.
