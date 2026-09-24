#pragma once
// SatModelTool --run-benchmark (benchmarking milestone M5): runs data/benchmarks files against their
// models with SatBench and writes a self-describing JSON report per run.
#include <cstdint>
#include <string>
#include <vector>

struct BenchRunOptions
{
    int samples = 2000;
    uint64_t seed = 1;
    int lobeBudget = 256;          // the app's budget for small rosters; benchmarks want the exact model
    bool sensitivity = false;      // also rerun under alternative assumptions and report the spread
    bool occlusion = true;         // Phase 3b occlusion between parts (--no-occlusion: the M6 baseline)
    std::string reportDir = "benchmark_runs";
    std::string modelsDir;         // default: <benchmark dir>/../satellite_models
    // --set [<model id>/]<material>.<field>=<value>: material overrides applied after loading, for
    // calibration scans. Fields: diffuse_albedo, specular_f0, roughness, distribution (ggx|beckmann).
    // Without a model id it applies to every model that has the material. Echoed into the report.
    std::vector<std::string> overrides;
};

// Returns true when every compared metric is within tolerance (informational metrics excluded).
bool runBenchmarkCommand(const std::string &benchmarkPath, const BenchRunOptions &opt);
