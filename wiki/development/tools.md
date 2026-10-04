# Tools

Everything in `tools/` besides the harness drivers: the two offline C++ tools, the scripts that bake assets,
the consistency checkers and the analysis scripts. The harness and its image, motion and audio analysis tools
are on [Automation harness](harness.md).

Python tools that need third-party packages use one of two virtual environments: `tools/harness/.venv`
(Pillow, numpy, soundfile, scipy; `tools/harness/requirements.txt`) and `tools/perf_analysis/.venv` (pandas,
matplotlib; `tools/perf_analysis/requirements.txt`). Scripts without dependencies run with any Python 3.

## C++ tools

All three are excluded from the default build (`EXCLUDE_FROM_ALL`); build them by name.

### SatModelTool

```bash
cmake --build build --target SatModelTool
build/Debug/SatModelTool.exe data/satellite_models/hubble.json [--out <dir>] [--budget <lobes>]
```

The satellite model workbench. It compiles the app's own `SatModel`, `SatPhotometry`, `SatBenchmark`,
`SatBench` and `SatTrace` sources with no Vulkan and no window, so it runs exactly the pipeline the app runs at
startup: load, tessellate, bake facet lobes, check them against a brute-force per-triangle evaluation, and write
rest and sunlit OBJ poses. It prints each model's lobe table and its provenance summary (every part's source
status, and any unexplained part).

| Mode | What it does |
|---|---|
| `<model.json> ...` | bake, validate, export OBJ, print the lobe table and provenance |
| `--selftest <N>` | the photometric evaluator's checks: known-value geometry (Sun declination at a solstice and equinox, orbit radius and velocity), earthshine, atmospheric extinction against a brute-force integral, then per model N posed configurations comparing the lobe path with brute force, occlusion against a ray-cast reference, the packed GPU form of occlusion, and trace and export round trips |
| `--shadow-study <N>` | how much self-shadowing dims the model: over N random lit configurations, the median, p90, p99 and maximum dimming in magnitudes and the share dimmed by more than 0.1 and 0.5 mag |
| `--benchmark <file.json>` | check a benchmark file's transcription: its observations must reproduce the statistics the paper printed |
| `--run-benchmark <file.json> [--samples N] [--seed S] [--sensitivity] [--report-dir D] [--no-occlusion]` | SatBench: simulate the paper's observing campaign with the benchmark's model and compare |
| `--set [<model>/]<material>.<field>=<value>` | override a material after loading (echoed into reports) |
| `--summarize-samples <csv>` | statistics of a samples CSV (a SatBench run's, or the app's bulk export); checks a run's own statistics are reproduced |
| `--replay-trace <csv> [--models-dir D]` | re-evaluate an exported magnitude trace from the inputs in its header and check every row matches |

The `accuracy-gate` build target runs `--selftest`, `--benchmark` and `--run-benchmark` over every model and
benchmark file; CI runs it on every pull request. See [SatBench and the accuracy gate](../accuracy/satbench.md)
and [Model provenance](../accuracy/provenance.md). The full ISS self-test and the whole gate are slow; for a
change no benchmark depends on, they need not be run locally.

Related scripts: `tools/benchmarks/plot_benchrun.py <report.json>` plots a SatBench report (simulated against
observed magnitude histograms, magnitude against phase angle); `tools/benchmarks/scan_overrides.py` runs the
benchmarks under several `--set` override sets and tabulates the results.

### SoundTool

```bash
cmake --build build --target SoundTool
build/Debug/SoundTool.exe --analyze assets/sound/music/gravity_wave.mp3 [--out <dir>]
build/Debug/SoundTool.exe --render pad --out pad.wav --seconds 12 voices=0/0,1/2,6/8,9/0
```

The ambience workbench, with no window, Vulkan or audio device. `--analyze` runs the soundtrack analysis (key,
tuning curve, pitch set, chords) and writes the same JSON the app caches. `--render` renders one synth voice to
a 16-bit stereo WAV with constant, ramped or keyframed parameters. See [Ambience](../sound/ambience.md#soundtool)
and [Music and tonality](../sound/music.md#soundtrack-analysis).

### UploadStress

```bash
cmake --build build --config Release --target UploadStress
build/Release/UploadStress.exe [--set dem|launch] [--device cycle|once] [--gap S] [--minutes M] [--decode-only]
```

A standalone stress test of the app's texture upload path: the launch textures decoded once with the same
`stb_image` calls, then uploaded in a loop exactly as the app does (staging buffer, image with a full mip chain,
copy and mipmap blits in one command buffer, submit and wait), by default with a fresh Vulkan instance and
device per cycle. Each step appends an fsynced line to a log, so an interrupted run names the cycle and step it
reached. Run it from the repository root.

## Asset bakers

Generated assets are produced by scripts and committed. Edit the script's declarations, not the output, and
rebuild afterwards so the runtime-file sync copies the result next to the executable.

| Script | Builds | From | Notes |
|---|---|---|---|
| `tools/make_icons.py [name ...]` | `assets/icons/ui/pixel--<name>.png` | shape lists in the script | no dependencies; prints 48 px and 16 px previews ([UI](ui.md#icons)) |
| `tools/make_ambience.py [name ...] [--audition]` | the ambience FLAC loops and one-shots, `CREDITS.txt` | CC0 Freesound recordings declared in the script, cached in `build/ambience_cache/` | numpy, soundfile ([Ambience](../sound/ambience.md#samples)) |
| `tools/make_raw_textures.py` | `earth_elevation_0.r8`, `earth_elevation_1.r8`, `8k_earth_specular_map.r8` | the source PNGs | byte-exact raw single-channel maps the app reads without PNG decoding ([Building](building.md#raw-textures)) |
| `tools/make_water_map.py` | `earth_water_sdf.r8` (signed distance to the shore), `earth_water_level.r8` (the level of the nearest water body) | the water mask, the DEM, Natural Earth 10 m land and lakes | scipy; re-run after editing the mask or the DEM ([Terrain](../rendering/terrain.md)) |
| `tools/make_city_roads.py` | `assets/textures/city_roads.bin` | Natural Earth 10 m roads, cached in `build/city_cache/` | a lat/lon cell list of road segments for the procedural city lights ([Cities](../rendering/cities.md)) |
| `tools/make_terrain_materials.py [--size 1024]` | `assets/textures/terrain_materials.rgba8` | CC0 ambientCG texture sets, cached in `build/terrain_material_cache/` | two layers per material: albedo ratio and height; normal, roughness and occlusion |
| `tools/make_cloud_morph.py [--size 1024] [--preview out.png]` | `assets/textures/cloud_morph.rgba8` | four public-domain MODIS scenes (NASA GIBS), one per channel | the cloud morphology texture ([The cloud field](../rendering/clouds/field.md)); `make_cloud_morph_procedural.py` is a procedural alternative |
| `tools/parse_bsc.py` | C++ initialisers for `src/simulations/star_catalog.h` | the Yale Bright Star Catalogue (BSC5) from CDS | stars to the script's magnitude limit, brightest first, with B-V colour |

Raw files written by these scripts start with a small magic header (`SLR8`, `SLRD`, `SLTA`) followed by
little-endian dimensions, so the app reads them without an image decoder.

## Checkers

| Script | Checks |
|---|---|
| `tools/check_cloud_params.py` | the C++ `GpuCloudParams` and the GLSL `CloudParams` block have the same fields in the same order; exit 1 on a mismatch. Run after touching either ([GPU conventions](gpu-conventions.md#the-cloudparams-uniform-block)) |
| `tools/settings_defaults_diff.py [settings.json]` | every key in a settings file whose value differs from its compiled-in default, by parsing `applySettingsJson()` for the member behind each key ([Controls and settings](controls-and-settings.md#defaults)) |
| `tools/wiki/check.py [--lint]` | the wiki's strict build, history-leak lint and inbox report ([Working on this wiki](wiki.md)) |

## Analysis

| Script | What |
|---|---|
| `tools/perf_analysis/analyze_profile.py [profile_log.jsonl]` | reads perf snapshots and knockout sweeps: GPU cost by resolution and per megapixel, a matched-altitude resolution ratio, knockout costs, correlations with scene variables, plots under `analysis_output/`, and a ranked table per sweep ([Profiling](profiling.md#perf-snapshots-and-analyze_profilepy)) |
| `tools/perf_analysis/live_perf_table.py <outbox.json\|results.jsonl> [baseline]` | tabulates harness `perf name=` results, optionally against a baseline |
| `tools/cloud_stats.py image.png [...] [--km-per-px] [--crop] [--mask]` | cloud-field statistics for comparing renders from space with satellite imagery: cover, perimeter-area fractal dimension, object-size exponent, the mask's spectral slope, holes per 1000 km² (numpy, scipy, Pillow) |
| `tools/make_capture_bundle.sh` | macOS: wraps the built executable in a minimal `.app` that declares `MetalCaptureEnabled`, so MoltenVK's auto-capture or Xcode can capture a GPU frame for occupancy analysis (reading the trace needs full Xcode) |

## Where in the code

| Path | What |
|---|---|
| `tools/sat_model_tool/main.cpp`, `bench_run.cpp` | SatModelTool |
| `tools/sound_tool/main.cpp` | SoundTool |
| `tools/upload_stress/main.cpp` | UploadStress |
| `tools/*.py` | bakers and checkers |
| `tools/benchmarks/`, `tools/perf_analysis/` | analysis scripts |
| `cmake/AccuracyGate.cmake` | the accuracy gate's list of checks |
