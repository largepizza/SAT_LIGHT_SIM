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
#include "SatPhotometry.h" // SatOrbitElems (shared sample evaluation, bulk export)

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
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

// One simulated observation — the record SatBench draws and the app's bulk export (milestone M10)
// writes, in one CSV schema (writeBenchSamplesCsv), so either can feed the other's tools.
struct BenchSample
{
    double tJ2000 = 0.0;
    int site = 0;          // index into the benchmark's sites (bulk export: 0, the observer)
    double sunAltDeg = 0.0;
    double elevationDeg = 0.0;
    double azimuthDeg = 0.0; // clockwise from north
    double rangeM = 0.0;
    double phaseDeg = 0.0;
    double offSpecularDeg = 0.0;
    double mag = 0.0;     // above-atmosphere apparent magnitude
    double m1000 = 0.0;   // reduced to 1000 km (after censoring)
    double magApparent = 0.0; // mag + line-of-sight extinction (= mag when extinction k is 0)
    bool censored = false;
    int dominantLobe = -1; // brightest lobe under sunlight (which surface the observer mostly sees)
    int satellite = -1;    // roster index (bulk export); -1 for SatBench's random draws
    int pass = -1;         // pass number within the export (bulk export); -1 for SatBench
};

// The evaluation both share: one satellite at one instant for one observer. Fills `out` and returns
// true when the satellite is at or above minElevationDeg, fully sunlit (penumbra excluded, as in
// the papers) and its type is supported; censoring is the caller's business.
bool benchEvalSample(const std::vector<AttitudeGroup> &groups, const std::vector<GpuSatLobe> &lobes,
                     const SatOcclusion *occ, const SatOrbitElems &orbit, double tJ2000, glm::dvec3 obsEci,
                     glm::dvec3 sunDirEci, double minElevationDeg, double extinctionK, BenchSample &out);

// CSV format "sat-light-sim-samples/1": '#'-prefixed "key: value" header lines (the caller's
// provenance, in order), the column header, one row per sample. Magnitude fields are empty when
// non-finite.
using BenchCsvHeader = std::vector<std::pair<std::string, std::string>>;
bool writeBenchSamplesCsv(const std::string &path, const BenchCsvHeader &header, const std::vector<BenchSample> &samples,
                          std::string &err);
bool readBenchSamplesCsv(const std::string &path, BenchCsvHeader &header, std::vector<BenchSample> &samples,
                         std::string &err);

// ── Bulk export (milestone M10) ────────────────────────────────────────────────────────────────
// Every instant, at a fixed cadence, when a satellite of one type satisfies the benchmark sampling
// constraints for one observer: the Sun inside the twilight window, the satellite at or above the
// elevation limit and fully sunlit. Sampling time uniformly over every visible satellite is what
// SatBench's random draws sample from, so the two are statistically the same measurement.
struct BulkExportSpec
{
    const std::vector<AttitudeGroup> *groups = nullptr;
    const std::vector<GpuSatLobe> *lobes = nullptr;
    const SatOcclusion *occlusion = nullptr; // null: no occlusion between parts
    glm::dvec3 obsDirEcef{0.0, 0.0, 1.0};
    double obsRadiusM = 6371000.0;
    double t0 = 0.0, t1 = 0.0;   // seconds since J2000
    double cadenceS = 30.0;
    double minElevationDeg = 20.0;
    double sunAltMinDeg = -18.0, sunAltMaxDeg = -6.0;
    double extinctionK = 0.0;
};
struct BulkExportSat
{
    SatOrbitElems orbit;
    int index = -1; // roster index, copied into BenchSample::satellite
};
// Samples in satellite order, then time. `progress` (0..1) and `cancel` may be null; a cancelled
// run returns false with what it had so far.
bool runBulkExport(const BulkExportSpec &spec, const std::vector<BulkExportSat> &sats, std::vector<BenchSample> &out,
                   std::atomic<float> *progress = nullptr, const std::atomic<bool> *cancel = nullptr);

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

// Runs one distribution benchmark. `lobes` are the model's baked lobes; `occ` (optional) its
// occlusion data (Phase 3b). False with `err` when the benchmark lacks what sampling needs (sites,
// shell) or no sample could be drawn.
bool runDistributionBenchmark(const Benchmark &b, const SatModel &m, const std::vector<GpuSatLobe> &lobes,
                              const BenchRunConfig &cfg, BenchRunResult &out, std::string &err,
                              const SatOcclusion *occ = nullptr);

// Summary statistics of a plain list of 1000-km magnitudes.
BenchStats benchStatsOfValues(const std::vector<double> &m1000, int notSeen);
