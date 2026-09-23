// SatModelTool --run-benchmark — see bench_run.h.
#include "bench_run.h"

#include "SatBench.h"
#include "SatBenchmark.h"
#include "SatModel.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace
{
namespace fs = std::filesystem;
using nlohmann::json;

// Provisional tolerances from the design page ("Benchmark kinds and pass criteria").
constexpr double kTolMeanMag = 0.3;
constexpr double kTolMedianMag = 0.3;
constexpr double kTolDeltaMag = 0.3;

// FNV-1a 64 of a file's bytes, hex — identifies exactly which model/benchmark a report used.
std::string fileHash(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return "unreadable";
    uint64_t h = 1469598103934665603ull;
    for (std::istreambuf_iterator<char> it(f), end; it != end; ++it)
    {
        h ^= (unsigned char)*it;
        h *= 1099511628211ull;
    }
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return buf;
}

std::string runCommand(const char *cmd)
{
#ifdef _WIN32
    FILE *p = _popen(cmd, "r");
#else
    FILE *p = popen(cmd, "r");
#endif
    if (!p)
        return {};
    std::string out;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), p))
        out += buf;
#ifdef _WIN32
    _pclose(p);
#else
    pclose(p);
#endif
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

json gitInfo()
{
    std::string commit = runCommand("git rev-parse --short HEAD 2>&1");
    if (commit.empty() || commit.find(' ') != std::string::npos)
        return {{"commit", "unknown"}, {"dirty", nullptr}};
    std::string status = runCommand("git status --porcelain --untracked-files=no 2>&1");
    return {{"commit", commit}, {"dirty", !status.empty()}};
}

std::string utcNow()
{
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return buf;
}

double round4(double v) { return std::isfinite(v) ? std::round(v * 1e4) / 1e4 : v; }

struct LoadedModel
{
    SatModel model;
    std::vector<GpuSatLobe> lobes;
    std::string path, hash;
    int exactLobes = 0;
};

bool loadModelFor(const Benchmark &b, const BenchRunOptions &opt, LoadedModel &out, std::string &err)
{
    if (b.modelId.empty())
    {
        err = "benchmark '" + b.id + "' names no model (satellite.model)";
        return false;
    }
    fs::path dir = opt.modelsDir.empty() ? fs::path(b.path).parent_path() / ".." / "satellite_models"
                                         : fs::path(opt.modelsDir);
    out.path = (dir / (b.modelId + ".json")).lexically_normal().string();
    std::vector<std::string> warn;
    if (!loadSatModel(out.path, b.modelId, out.model, err, warn))
        return false;
    std::vector<SatTri> tris = tessellateSatModel(out.model);
    SatLobeBakeStats st;
    out.lobes = bakeSatLobes(out.model, tris, opt.lobeBudget, st);
    out.exactLobes = st.exactLobes;
    out.hash = fileHash(out.path);
    return true;
}

json configJson(const BenchRunConfig &c)
{
    return {{"label", c.label},
            {"samples", c.samples},
            {"seed", c.seed},
            {"min_elevation_deg", c.minElevationDeg},
            {"sun_alt_window_deg", {c.sunAltMinDeg, c.sunAltMaxDeg}},
            {"period_start_fallback", c.periodStartFallback},
            {"censor", c.censor}};
}

json resultJson(const BenchRunResult &r)
{
    return {{"n", r.stats.n},
            {"not_seen", r.stats.notSeen},
            {"mean_m1000", round4(r.stats.mean)},
            {"median_m1000", round4(r.stats.median)},
            {"sd_m1000", round4(r.stats.sd)},
            {"sem_m1000", round4(r.stats.sem)},
            {"phase_fit_linear", r.phaseFitLinear.size() == 2
                                     ? json{round4(r.phaseFitLinear[0]), std::round(r.phaseFitLinear[1] * 1e6) / 1e6}
                                     : json()},
            {"bright_frac_m1000_lt_5", round4(r.brightFrac)},
            {"satellite_draws", r.satelliteDraws},
            {"time_draws", r.timeDraws}};
}

// Reference statistics: recomputed from the transcribed observations when there are any (the M3
// gate proved they reproduce the paper), else the paper's printed values.
bool referenceStats(const Benchmark &b, BenchStats &s, std::vector<double> &fit, std::string &source)
{
    if (!b.observations.empty())
    {
        s = benchStats(b.observations);
        std::vector<double> x, y;
        for (const BenchObservation &o : b.observations)
        {
            x.push_back(o.phaseDeg);
            y.push_back(o.m1000);
        }
        fit = benchPolyFit(x, y, 1);
        source = "observations (" + std::to_string(s.n) + " rows)";
        return true;
    }
    auto it = b.published.find("mean_m1000");
    if (it == b.published.end())
        return false;
    s.mean = it->second.value;
    s.n = b.published.count("n") ? (int)b.published.at("n").value : 0;
    s.median = b.published.count("median_m1000") ? b.published.at("median_m1000").value : NAN;
    s.sd = b.published.count("sd_m1000") ? b.published.at("sd_m1000").value : NAN;
    source = "published summary";
    return true;
}

json compareRow(const char *metric, double model, double ref, double tol)
{
    json row = {{"metric", metric}, {"model", round4(model)}, {"reference", round4(ref)}};
    if (!std::isfinite(ref))
    {
        row["verdict"] = "n/a";
        return row;
    }
    double d = model - ref;
    row["delta"] = round4(d);
    if (tol > 0.0)
    {
        row["tolerance"] = tol;
        row["verdict"] = std::abs(d) <= tol ? "pass" : "fail";
    }
    else
        row["verdict"] = "info";
    return row;
}

void printCompare(const json &rows)
{
    for (const json &r : rows)
    {
        if (r["verdict"] == "n/a")
            continue;
        std::printf("    %-5s %-26s model %8.3f  reference %8.3f  delta %+7.3f\n",
                    r["verdict"].get<std::string>().c_str(), r["metric"].get<std::string>().c_str(),
                    r["model"].get<double>(), r["reference"].get<double>(), r["delta"].get<double>());
    }
}

std::vector<BenchRunConfig> sensitivityVariants(const BenchRunConfig &base)
{
    std::vector<BenchRunConfig> v;
    auto add = [&](const char *label, double minEl, double sunLo, double sunHi) {
        BenchRunConfig c = base;
        c.label = label;
        c.minElevationDeg = minEl;
        c.sunAltMinDeg = sunLo;
        c.sunAltMaxDeg = sunHi;
        v.push_back(c);
    };
    add("min_elevation_10", 10.0, base.sunAltMinDeg, base.sunAltMaxDeg);
    add("min_elevation_30", 30.0, base.sunAltMinDeg, base.sunAltMaxDeg);
    add("sun_alt_-12_-6", base.minElevationDeg, -12.0, -6.0);
    add("sun_alt_-18_-12", base.minElevationDeg, -18.0, -12.0);
    return v;
}

bool writeReport(const json &report, const BenchRunOptions &opt, const std::string &name)
{
    std::error_code ec;
    fs::create_directories(opt.reportDir, ec);
    fs::path p = fs::path(opt.reportDir) / (name + ".json");
    std::ofstream f(p);
    if (!f)
    {
        std::printf("  could not write %s\n", p.string().c_str());
        return false;
    }
    f << report.dump(1) << "\n";
    std::printf("  report: %s\n", p.string().c_str());
    return true;
}

// One distribution benchmark with its model; fills `report` and returns pass/fail.
bool runDistribution(const Benchmark &b, const BenchRunOptions &opt, json &report, BenchRunResult &base)
{
    LoadedModel lm;
    std::string err;
    if (!loadModelFor(b, opt, lm, err))
    {
        std::printf("  FAILED: %s\n", err.c_str());
        return false;
    }
    BenchRunConfig cfg;
    cfg.samples = opt.samples;
    cfg.seed = opt.seed;
    if (!runDistributionBenchmark(b, lm.model, lm.lobes, cfg, base, err))
    {
        std::printf("  FAILED: %s\n", err.c_str());
        return false;
    }

    BenchStats ref;
    std::vector<double> refFit;
    std::string refSource;
    referenceStats(b, ref, refFit, refSource);

    json cmp = json::array();
    cmp.push_back(compareRow("mean_m1000", base.stats.mean, ref.mean, kTolMeanMag));
    cmp.push_back(compareRow("median_m1000", base.stats.median, ref.median, kTolMedianMag));
    cmp.push_back(compareRow("sd_m1000", base.stats.sd, ref.sd, 0.0));
    // Phase-matched mean: the observers' own selection of geometry is unpublished, and it shows as a
    // narrower phase-angle distribution than a random visible satellite gives. Reweighting the
    // simulated samples to the observed phase histogram (10 deg bins) compares like with like.
    // Informational; `phase_bins_uncovered` counts observed bins the simulation never reached.
    if (!b.observations.empty())
    {
        constexpr int kBins = 18;
        double hObs[kBins] = {}, hSim[kBins] = {};
        auto bin = [](double deg) { return std::clamp((int)(deg / 10.0), 0, kBins - 1); };
        for (const BenchObservation &o : b.observations)
            hObs[bin(o.phaseDeg)] += 1.0 / b.observations.size();
        for (const BenchSample &s : base.samples)
            hSim[bin(s.phaseDeg)] += 1.0 / base.samples.size();
        double wSum = 0.0, mSum = 0.0;
        int uncovered = 0;
        for (int k = 0; k < kBins; ++k)
            uncovered += (hObs[k] > 0.0 && hSim[k] == 0.0) ? 1 : 0;
        for (const BenchSample &s : base.samples)
        {
            const int k = bin(s.phaseDeg);
            const double w = hObs[k] / hSim[k];
            wSum += w;
            mSum += w * s.m1000;
        }
        if (wSum > 0.0)
        {
            json row = compareRow("mean_m1000_phase_matched", mSum / wSum, ref.mean, 0.0);
            row["phase_bins_uncovered"] = uncovered;
            cmp.push_back(row);
        }
    }
    if (refFit.size() == 2 && base.phaseFitLinear.size() == 2)
    {
        cmp.push_back(compareRow("phase_slope_mag_per_deg", base.phaseFitLinear[1], refFit[1], 0.0));
        cmp.push_back(compareRow("phase_fit_at_90deg", benchPolyEval(base.phaseFitLinear, 90.0),
                                 benchPolyEval(refFit, 90.0), 0.0));
    }
    bool pass = true;
    for (const json &r : cmp)
        pass &= r["verdict"] != "fail";

    std::printf("  model %s (%zu lobes, budget %d; %d exact) — occlusion: none (Phase 3b not built)\n",
                lm.model.name.c_str(), lm.lobes.size(), opt.lobeBudget, lm.exactLobes);
    std::printf("  %d simulated observations (%d censored), reference: %s\n", base.stats.n, base.stats.notSeen,
                refSource.c_str());
    printCompare(cmp);

    // Which surface dominates: per lobe, the share of samples in which it is the brightest, labelled
    // by group and its rest-pose body-frame normal (the lobe table SatModelTool prints).
    json dominance = json::array();
    {
        std::vector<int> count(lm.lobes.size(), 0);
        for (const BenchSample &s : base.samples)
            if (s.dominantLobe >= 0 && s.dominantLobe < (int)count.size())
                ++count[s.dominantLobe];
        std::printf("  dominant surface (share of samples):\n");
        for (size_t li = 0; li < lm.lobes.size(); ++li)
        {
            if (count[li] == 0)
                continue;
            const GpuSatLobe &L = lm.lobes[li];
            const AttitudeGroup &root = lm.model.groups[attRootOf(lm.model.groups, (int)L.group)];
            // Rest body normal = B_root · normalT (B = the root's body triad; see SatModel.h).
            glm::dvec3 t1 = glm::normalize(glm::dvec3(root.primaryAxis));
            glm::dvec3 t2 = glm::normalize(glm::cross(t1, glm::dvec3(root.secondaryAxis)));
            glm::dvec3 t3 = glm::cross(t1, t2);
            glm::dvec3 n = glm::dvec3(L.normalT.x) * t1 + glm::dvec3(L.normalT.y) * t2 + glm::dvec3(L.normalT.z) * t3;
            if (root.law == AttLaw::Tumble)
                n = glm::dvec3(L.normalT);
            const double share = (double)count[li] / base.samples.size();
            std::printf("    lobe %2zu  group %-10s normal (%5.2f %5.2f %5.2f)  albedo %.2f  area %5.2f m2  %5.1f%%\n", li,
                        lm.model.groups[L.group].name.c_str(), n.x, n.y, n.z, L.albedoD, L.area, 100.0 * share);
            dominance.push_back({{"lobe", li},
                                 {"group", lm.model.groups[L.group].name},
                                 {"body_normal", {round4(n.x), round4(n.y), round4(n.z)}},
                                 {"area_m2", round4(L.area)},
                                 {"albedo", round4(L.albedoD)},
                                 {"share", round4(share)}});
        }
    }

    json sens = json::array();
    if (opt.sensitivity)
    {
        std::printf("  sensitivity to unstated sampling assumptions (mean m1000 shift vs base):\n");
        for (const BenchRunConfig &v : sensitivityVariants(cfg))
        {
            BenchRunResult r;
            if (!runDistributionBenchmark(b, lm.model, lm.lobes, v, r, err))
                continue;
            double d = r.stats.mean - base.stats.mean;
            std::printf("    %-18s mean %6.3f  median %6.3f  (%+.3f)\n", v.label.c_str(), r.stats.mean, r.stats.median, d);
            sens.push_back({{"config", configJson(v)}, {"result", resultJson(r)}, {"delta_mean_vs_base", round4(d)}});
        }
    }

    json rows = json::array();
    for (const BenchSample &s : base.samples)
        rows.push_back({round4(s.tJ2000), s.site, round4(s.sunAltDeg), round4(s.elevationDeg), round4(s.rangeM),
                        round4(s.phaseDeg), round4(s.offSpecularDeg),
                        std::isfinite(s.mag) ? json(round4(s.mag)) : json(), round4(s.m1000), s.censored});
    json refRows = json::array();
    for (const BenchObservation &o : b.observations)
        refRows.push_back({o.phaseDeg, o.m1000, o.source, o.notSeen});

    report = {{"format", "sat-light-sim-benchrun/1"},
              {"kind", "distribution"},
              {"benchmark", {{"id", b.id}, {"file", b.path}, {"fnv1a64", fileHash(b.path)}, {"citation", b.citationUrl}}},
              {"model",
               {{"id", b.modelId},
                {"file", lm.path},
                {"fnv1a64", lm.hash},
                {"lobe_budget", opt.lobeBudget},
                {"lobes", lm.lobes.size()},
                {"exact_lobes", lm.exactLobes},
                {"occlusion", "none (Phase 3b not built)"}}},
              {"config", configJson(cfg)},
              {"reference", {{"source", refSource},
                             {"n", ref.n},
                             {"mean_m1000", round4(ref.mean)},
                             {"median_m1000", round4(ref.median)},
                             {"sd_m1000", round4(ref.sd)},
                             {"phase_fit_linear", refFit.size() == 2 ? json{round4(refFit[0]), std::round(refFit[1] * 1e6) / 1e6} : json()}}},
              {"result", resultJson(base)},
              {"comparison", cmp},
              {"verdict", pass ? "pass" : "fail"},
              {"sensitivity", sens},
              {"dominant_surface", dominance},
              {"samples", {{"columns", {"t_j2000", "site", "sun_alt_deg", "elevation_deg", "range_m", "phase_deg",
                                        "off_specular_deg", "mag", "m1000", "censored"}},
                           {"rows", rows}}},
              {"reference_observations", {{"columns", {"phase_deg", "m1000", "source", "not_seen"}}, {"rows", refRows}}}};
    return pass;
}
} // namespace

bool runBenchmarkCommand(const std::string &benchmarkPath, const BenchRunOptions &opt)
{
    Benchmark b;
    std::string err;
    if (!loadBenchmark(benchmarkPath, b, err))
    {
        std::printf("[run %s] FAILED to load: %s\n", benchmarkPath.c_str(), err.c_str());
        return false;
    }
    std::printf("[run %s] %s (%s), seed %llu, %d samples\n", b.id.c_str(), b.title.c_str(), b.kind.c_str(),
                (unsigned long long)opt.seed, opt.samples);
    json report;
    bool pass = false;
    char seedTag[32];
    std::snprintf(seedTag, sizeof(seedTag), "seed%llu", (unsigned long long)opt.seed);

    if (b.kind == "distribution")
    {
        BenchRunResult base;
        pass = runDistribution(b, opt, report, base);
        if (report.is_null())
            return false;
    }
    else
    {
        // Differential: run both referenced benchmarks with their own models and sampling, compare
        // the difference of the simulated means with the published difference.
        fs::path dir = fs::path(benchmarkPath).parent_path();
        Benchmark test, baseB;
        if (!loadBenchmark((dir / (b.refTest + ".json")).string(), test, err) ||
            !loadBenchmark((dir / (b.refBaseline + ".json")).string(), baseB, err))
        {
            std::printf("  FAILED: %s\n", err.c_str());
            return false;
        }
        json rt, rb;
        BenchRunResult resT, resB;
        std::printf("  test: %s\n", test.id.c_str());
        runDistribution(test, opt, rt, resT);
        std::printf("  baseline: %s\n", baseB.id.c_str());
        runDistribution(baseB, opt, rb, resB);
        if (rt.is_null() || rb.is_null())
            return false;
        const double publishedDelta = b.published.count("delta_mean_m1000") ? b.published.at("delta_mean_m1000").value : NAN;
        const double modelDelta = resT.stats.mean - resB.stats.mean;
        json row = compareRow("delta_mean_m1000", modelDelta, publishedDelta, kTolDeltaMag);
        pass = row["verdict"] != "fail";
        std::printf("  differential:\n");
        printCompare(json::array({row}));
        // Keep the two sub-reports' summaries; their samples are in their own reports.
        auto summary = [](json r) {
            r.erase("samples");
            r.erase("reference_observations");
            return r;
        };
        report = {{"format", "sat-light-sim-benchrun/1"},
                  {"kind", "differential"},
                  {"benchmark", {{"id", b.id}, {"file", b.path}, {"fnv1a64", fileHash(b.path)}, {"citation", b.citationUrl}}},
                  {"comparison", json::array({row})},
                  {"verdict", pass ? "pass" : "fail"},
                  {"test", summary(rt)},
                  {"baseline", summary(rb)}};
        for (json *r : {&rt, &rb})
        {
            (*r)["git"] = gitInfo();
            (*r)["run_utc"] = utcNow();
        }
        writeReport(rt, opt, test.id + "__" + test.modelId + "__" + seedTag);
        writeReport(rb, opt, baseB.id + "__" + baseB.modelId + "__" + seedTag);
    }
    report["git"] = gitInfo();
    report["run_utc"] = utcNow();
    writeReport(report, opt, b.id + "__" + (b.modelId.empty() ? std::string("models") : b.modelId) + "__" + seedTag);
    std::printf("  verdict: %s\n\n", pass ? "PASS" : "FAIL");
    return pass;
}
