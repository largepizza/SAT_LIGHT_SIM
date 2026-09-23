#pragma once
// ── SatBenchmark — published satellite photometry as data ─────────────────────────────────────
// Benchmarking milestone M3 (design page: "Satellite Brightness Benchmarking"). A benchmark file
// (data/benchmarks/*.json, format "sat-light-sim-benchmark/1") carries one published dataset and
// everything needed to compare the model against it traceably:
//   citation            — where the numbers come from
//   published           — the paper's own printed statistics, each with a locator (section/table)
//   sampling            — how the observations were taken (shell, period, sites, constraints) so a
//                         simulation can draw its passes the same way
//   observations        — the individual measurements, when the paper publishes them
//   transcription       — how the file was produced and how it was checked
// Kinds: "distribution" (a population's magnitudes), "differential" (the published difference
// between two distributions, via `references`). SatBench (M5) runs them; SatModelTool --benchmark
// verifies that a file's observations reproduce its published statistics.

#include <limits>
#include <map>
#include <string>
#include <vector>

struct BenchPublished
{
    double value = std::numeric_limits<double>::quiet_NaN();
    double uncertainty = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> coefficients;             // fits: c0 + c1·x + c2·x² ...
    std::vector<double> coefficientUncertainties;
    std::string variable;                         // fits: "phase_deg" or "phase_rad"
    std::string locator;
};

struct BenchSite
{
    std::string name;
    double latDeg = 0.0, lonDeg = 0.0; // east positive
    int count = -1;                    // observations from this site, if published
    std::string instrument;
};

struct BenchObservation
{
    int sat = 0;
    int day2020 = 0;
    int opsDays = 0;
    double phaseDeg = 0.0;
    double m1000 = 0.0;
    std::string source;
    bool notSeen = false; // censored: too faint to see, magnitude assigned by the paper's rule
};

struct Benchmark
{
    std::string path;
    std::string id, title, kind;
    std::string citationAuthors, citationTitle, citationUrl;
    int citationYear = 0;
    std::string satelliteName, modelId; // modelId empty = no model yet
    std::map<std::string, BenchPublished> published;
    // Sampling
    double shellAltKm = 0.0, shellInclDeg = 0.0;
    std::string periodStart, periodEnd; // ISO dates; empty = not stated
    std::vector<BenchSite> sites;
    int censoredCount = -1;             // published count of censored ("not seen") observations
    double censorLimitMag = std::numeric_limits<double>::quiet_NaN();
    // Differential
    std::string refTest, refBaseline;
    std::vector<BenchObservation> observations;
};

// Loads and checks the structure of one file; false with `err` on any problem.
bool loadBenchmark(const std::string &path, Benchmark &out, std::string &err);

// Statistics of a set of 1000-km magnitudes — what the published values are compared against.
struct BenchStats
{
    int n = 0;
    int notSeen = 0;
    double mean = 0.0, median = 0.0, sd = 0.0, sem = 0.0;
};
BenchStats benchStats(const std::vector<BenchObservation> &obs);

// Least-squares polynomial fit y = c0 + c1·x + ... (degree ≤ 4); empty on a singular system.
std::vector<double> benchPolyFit(const std::vector<double> &x, const std::vector<double> &y, int degree);
double benchPolyEval(const std::vector<double> &c, double x);
