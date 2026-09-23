#pragma once
// ── SatBench — simulate a published photometry campaign with a geometry model ───────────────────
// Benchmarking milestone M5 (design page: "Satellite Brightness Benchmarking"). Given a benchmark
// file (SatBenchmark.h) and a model, draw simulated observations the way the paper's observations
// were taken — its sites, its period, twilight, a random satellite of the shell that is above the
// horizon limit and fully sunlit — evaluate each with the CPU photometric evaluator
// (SatPhotometry.h), apply the paper's censoring rule, and summarise the result exactly as the
// paper summarised its data (SatBenchmark's BenchStats + phase fits).
//
// Everything the paper does not state (the twilight window, the minimum elevation, a missing period
// start) is an explicit BenchRunConfig field, echoed into every report, so a result is never more
// certain than its assumptions. Deterministic: the only randomness is a std::mt19937_64 (whose
// sequence the C++ standard fixes) converted to doubles by bit manipulation, never through the
// implementation-defined std distributions — the same seed gives the same samples on every platform.

#include "SatBenchmark.h"
#include "SatModel.h"

#include <cstdint>
#include <string>
#include <vector>

struct BenchRunConfig
{
    int samples = 2000;
    uint64_t seed = 1;
    double minElevationDeg = 20.0; // not stated by Mallama 2020a/2021 — assumption
    double sunAltMinDeg = -18.0;   // observer's Sun altitude window (twilight to astronomical night)
    double sunAltMaxDeg = -6.0;
    std::string periodStartFallback = "2020-01-01"; // used when the benchmark's period has no start
    bool censor = true;            // apply the paper's "not seen" rule at visual sites
    std::string label = "base";
};

struct BenchSample
{
    double tJ2000 = 0.0;
    int site = 0;
    double sunAltDeg = 0.0;
    double elevationDeg = 0.0;
    double rangeM = 0.0;
    double phaseDeg = 0.0;
    double offSpecularDeg = 0.0;
    double mag = 0.0;     // above-atmosphere apparent magnitude
    double m1000 = 0.0;   // reduced to 1000 km (after censoring)
    bool censored = false;
    int dominantLobe = -1; // brightest lobe under sunlight (which surface the observer mostly sees)
};

struct BenchRunResult
{
    BenchRunConfig config;
    std::vector<BenchSample> samples;
    BenchStats stats;              // of m1000, censored samples included (as the papers do)
    std::vector<double> phaseFitLinear; // m1000 = c0 + c1 * phase_deg
    double brightFrac = 0.0;       // fraction with m1000 < 5.0 (flare tail)
    long long satelliteDraws = 0;  // satellites evaluated for visibility (efficiency / audit)
    int timeDraws = 0;
};

// Seconds since J2000 (2000-01-01 12:00) of an ISO date's 00:00 UTC. False on a malformed date.
bool benchIsoDateToJ2000(const std::string &iso, double &tJ2000);

// Runs one distribution benchmark. `lobes` are the model's baked lobes. False with `err` when the
// benchmark lacks what sampling needs (sites, shell) or no sample could be drawn.
bool runDistributionBenchmark(const Benchmark &b, const SatModel &m, const std::vector<GpuSatLobe> &lobes,
                              const BenchRunConfig &cfg, BenchRunResult &out, std::string &err);

// Summary statistics of a plain list of 1000-km magnitudes.
BenchStats benchStatsOfValues(const std::vector<double> &m1000, int notSeen);
