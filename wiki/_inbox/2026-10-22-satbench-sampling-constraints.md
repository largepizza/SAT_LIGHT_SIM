---
pages: [accuracy/benchmarks.md, accuracy/satbench.md, accuracy/results.md]
code: [src/simulations/SatBenchmark.cpp, src/simulations/SatBenchmark.h, src/simulations/SatBench.cpp, tools/sat_model_tool/bench_run.cpp, data/benchmarks/mallama2023_bluewalker3.json, data/benchmarks/mallama2021_visorsat.json, data/benchmarks/mallama2020a_original.json]
---
## Now
SatBench (`SatModelTool --run-benchmark`) reads a benchmark file's `sampling.constraints`: `sun_altitude_deg`
as `[min, max]` (the Sun's altitude at the observer) and `min_elevation_deg` as a number. Either may be `null`,
and then the runner's assumptions apply (Sun −18..−6°, elevation ≥ 20°). A malformed value fails the load. A site in
`sampling.sites` may carry the same two keys, which override the file-level ones for that site's draws. The run
prints the window it used, plus each site that has its own.

The site is drawn ONCE per sample and kept through the rejected time and satellite draws, so each site's share of
the samples is its weight (observation share or published count). Redrawn after every rejection, a site with a
narrower window, a higher elevation limit or a shorter twilight was under-sampled.

Windows are measured, and each file's note says how. BlueWalker 3: its own Appendix B visual rows, Sun −24..−10°,
elevation ≥ 15° (3.166 vs 3.09, gated). The 2020 Starlink files: visual sites Sun −24..−10° (the BlueWalker 3 rows of
the same observers), elevation ≥ 30° (at ≥ 15° the sampler censored 40% of the visual samples as "not seen" where the
VisorSat table has 2.4%; ≥ 30° gives 1%); MMT Sun −24..−11° (VisorSat tracks) / −33..−11° (V1.0 tracks), elevation
≥ 45° (the sampler's median elevation then matches MMT's 56°; MMT follows high passes).

A file may list `"ungated_metrics": ["phase_curve_rms"]` (with `ungated_note`): that metric's miss prints `known`
and does not fail the gate; the file's other metrics stay gated (narrower than `"gated": false`). Used by
`mallama2021_visorsat.json` (curve RMS 0.349; the table's bin means carry bright-attitude passes).

## History
The runner ignored the field and always sampled −18..−6°, so BlueWalker 3 (516 km, still lit and brightest with the
Sun below −18°) read 3.48 vs 3.09 and was left ungated. The 2020 Starlink models had been calibrated (M8) to that
assumed window and were ~1 mag too bright at MMT's real geometry (see the 2026-10-22 Starlink refit note). Stratifying
the site draw moved the other benchmarks: Amazon Leo 6.59 → 6.663, V2 Mini 7.88 → 7.965, the rest ≤ 0.012.
