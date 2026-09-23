// SatModelTool — offline bake / validate / OBJ export of satellite geometry models.
//
//   SatModelTool <model.json> [<model.json> ...] [--out <dir>] [--budget <lobes>]
//                [--shadow-study <N>] [--selftest <N>] [--benchmark <file.json>]...
//
// --benchmark checks a data/benchmarks file (M3): its observations must reproduce the statistics
// the paper printed (count, censored count, mean, median, sd, phase fits); a differential file's
// delta must follow from its two referenced files.
//
// --selftest runs the CPU photometric evaluator's checks (SatPhotometry, benchmarking milestone M1):
// known-value geometry checks, then per model N posed configurations comparing the lobe path with
// a posed per-triangle brute force. Exit code 1 if any check fails.
//
// Runs exactly the pipeline the app runs at startup (SatModel.cpp): load → tessellate → bake the
// facet lobes → check them against a brute-force per-triangle evaluation → write the rest and
// sunlit OBJ poses. Prints the lobe table so a model's reflectance can be inspected without
// launching the simulator. Exit code 1 if any model fails to load.
#include "SatBenchmark.h"
#include "SatModel.h"
#include "SatPhotometry.h"
#include "bench_run.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace
{
constexpr double kDeg = satphot::kPi / 180.0;

bool check(bool ok, const char *what, double got, double want)
{
    std::printf("    %s %-58s got %.6g, want %.6g\n", ok ? "ok  " : "FAIL", what, got, want);
    return ok;
}

// Model-independent known-value checks of the evaluator's geometry (design page M1).
bool selfTestGeometry()
{
    bool ok = true;
    std::printf("  geometry:\n");
    // Sun declination at the 2020 June solstice (2020-06-20 21:44 UTC) and March equinox (2020-03-20
    // 03:50 UTC). t counts from J2000 = 2000-01-01 12:00, so 2020-06-20 00:00 is day 7475.5 and
    // 2020-03-20 00:00 is day 7383.5. The almanac formula is good to ~0.01°.
    const double tSol = (7475.5 + 0.9056) * 86400.0;
    const double tEqx = (7383.5 + 0.1597) * 86400.0;
    double decSol = std::asin(sunDirEciAt(tSol).z) / kDeg;
    double decEqx = std::asin(sunDirEciAt(tEqx).z) / kDeg;
    ok &= check(std::abs(decSol - 23.44) < 0.05, "Sun declination, 2020 June solstice (deg)", decSol, 23.44);
    ok &= check(std::abs(decEqx) < 0.05, "Sun declination, 2020 March equinox (deg)", decEqx, 0.0);

    // Circular-orbit state: radius, unit along-track velocity perpendicular to the radius, and the
    // numerical derivative of position matching n·r·velocity (catches a sign or axis error).
    SatOrbitElems e;
    e.raan = 1.1;
    e.incl = 53.0 * kDeg;
    e.u0 = 0.4;
    e.rSatM = satphot::kEarthRadiusM + 550000.0;
    const double t = 7.0e8;
    SatOrbitState s0 = satOrbitStateAt(e, t);
    SatOrbitState s1 = satOrbitStateAt(e, t + 0.01);
    double n = std::sqrt(satphot::kGM / (e.rSatM * e.rSatM * e.rSatM));
    glm::dvec3 vNum = (s1.posEci - s0.posEci) / 0.01;
    ok &= check(std::abs(glm::length(s0.posEci) - e.rSatM) < 1e-3, "orbit radius (m)", glm::length(s0.posEci), e.rSatM);
    ok &= check(std::abs(glm::dot(s0.velocity, s0.nadir)) < 1e-12, "velocity . nadir", glm::dot(s0.velocity, s0.nadir), 0.0);
    double vErr = glm::length(vNum - n * e.rSatM * s0.velocity) / (n * e.rSatM);
    ok &= check(vErr < 1e-5, "d(pos)/dt vs n*r*velocity (relative)", vErr, 0.0);
    double incl = std::acos(glm::normalize(glm::cross(s0.posEci, s0.velocity)).z) / kDeg;
    ok &= check(std::abs(incl - 53.0) < 1e-9, "inclination from r x v (deg)", incl, 53.0);

    // Viewing angles: an observer placed exactly on the Sun's mirror image in a nadir-facing plate
    // has off-specular angle 0; one placed toward the Sun has phase angle 0.
    SatPhotInputs in;
    in.sunDirEci = glm::normalize(glm::dvec3(0.3, -0.5, -0.8));
    glm::dvec3 mirrorDir = glm::reflect(-in.sunDirEci, s0.nadir);
    in.obsEci = s0.posEci + 700000.0 * mirrorDir;
    std::vector<AttitudeGroup> oneGroup(1);
    SatPhotResult r = evalSatPhotometry(oneGroup, {}, e, t, in);
    ok &= check(r.offSpecularRad < 1e-6, "off-specular angle at the mirror image (rad)", r.offSpecularRad, 0.0);
    ok &= check(std::abs(r.rangeM - 700000.0) < 1e-3, "range (m)", r.rangeM, 700000.0);
    in.obsEci = s0.posEci + 700000.0 * in.sunDirEci;
    r = evalSatPhotometry(oneGroup, {}, e, t, in);
    ok &= check(r.phaseAngleRad < 1e-6, "phase angle toward the Sun (rad)", r.phaseAngleRad, 0.0);

    // Magnitude conventions: I/r² = 1e-12 (sunlight ratio) is m = -26.74 + 30 = 3.26; 1000-km at 550 km.
    ok &= check(std::abs(satMagnitudeFromIntensity(1.0, 1.0e6) - 3.26) < 1e-9, "magnitude of I/r^2 = 1e-12",
                satMagnitudeFromIntensity(1.0, 1.0e6), 3.26);
    ok &= check(std::abs(satMagnitudeTo1000km(5.92, 550000.0) - 7.218) < 1e-3, "5.92 at 550 km -> 1000 km",
                satMagnitudeTo1000km(5.92, 550000.0), 7.218);
    return ok;
}

// Posed-model check (the M1 gate): the evaluator's lobe path — every group posed by the attitude
// law, lobe normal = R·B·normalT — against the per-triangle brute force posed by the same poses.
// With the exact bake (no budget merge) the two must agree to float rounding; the app's budget
// bake is reported alongside for information.
bool selfTestModel(const SatModel &m, const std::vector<SatTri> &tris, int budget, int samples)
{
    SatLobeBakeStats exactStats, budgetStats;
    std::vector<GpuSatLobe> exact = bakeSatLobes(m, tris, 1 << 20, exactStats);
    std::vector<GpuSatLobe> budgeted = bakeSatLobes(m, tris, budget, budgetStats);

    std::mt19937 rng(4242);
    std::uniform_real_distribution<double> ud(0.0, 1.0);
    std::normal_distribution<double> nd;
    auto randDir = [&]() { return glm::normalize(glm::dvec3(nd(rng), nd(rng), nd(rng))); };
    const double a2Sun = (double)kSunAlpha * kSunAlpha;

    std::vector<double> errExact, errBudget;
    int unsupported = 0, dark = 0;
    for (int iter = 0; iter < samples * 50 && (int)errExact.size() < samples; ++iter)
    {
        SatOrbitElems e;
        e.raan = ud(rng) * 2.0 * satphot::kPi;
        e.incl = ud(rng) * satphot::kPi;
        e.u0 = ud(rng) * 2.0 * satphot::kPi;
        e.rSatM = satphot::kEarthRadiusM + 350000.0 + ud(rng) * 900000.0;
        const double t = 6.3e8 + ud(rng) * 5.0e8; // 2020 .. 2036
        SatOrbitState st = satOrbitStateAt(e, t);

        SatPhotInputs in;
        in.sunDirEci = sunDirEciAt(t);
        // Observer on the ground, within ~25° of the sub-satellite point.
        in.obsEci = satphot::kEarthRadiusM * glm::normalize(-st.nadir + 0.45 * randDir());
        SatPhotResult r = evalSatPhotometry(m.groups, exact, e, t, in);
        if (!r.supported)
        {
            ++unsupported;
            continue;
        }
        if (r.sinElevation < 0.05)
            continue;
        if (!(r.intensity > 0.0))
        {
            ++dark;
            continue;
        }

        // Brute force over posed triangles, same poses, same sources.
        AttGeometry geo;
        geo.nadir = st.nadir;
        geo.velocity = st.velocity;
        geo.sun = in.sunDirEci;
        geo.siteIdeal = st.nadir;
        geo.tumbleAngle = st.tumbleAngle;
        geo.tumbleAxis = e.tumbleAxis;
        std::vector<GroupPose> poses = evalGroupPoses(m.groups, geo, false);
        glm::dvec3 o = glm::normalize(in.obsEci - st.posEci);
        double rOverD = satphot::kEarthRadiusM / e.rSatM;
        double alphaE = rOverD / (1.0 + std::sqrt(1.0 - rOverD * rOverD));
        double Isun = 0.0, Iearth = 0.0;
        for (const SatTri &tr : tris)
        {
            const SatMaterial &mat = m.materials[tr.material];
            glm::dvec3 n = poses[tr.group].R * tr.n;
            double a2 = (double)mat.roughness * mat.roughness + tr.spread2;
            Isun += satLobeIntensity(n, tr.area, tr.area, mat.diffuseAlbedo, mat.specularF0, a2 + a2Sun, in.sunDirEci, o);
            Iearth += satLobeIntensity(n, tr.area, tr.area, mat.diffuseAlbedo, mat.specularF0, a2 + alphaE * alphaE,
                                       st.nadir, o);
        }
        double ref = Isun * r.litFactor + Iearth * r.earthIrradiance;
        // Significance floor: fainter than magnitude 20 is beyond any instrument this project
        // compares against, and down there (I ~ 1e-17 m², deep penumbra) float rounding of a grazing
        // lobe normal decides whether a sliver of it counts as lit — noise, not a disagreement.
        if (!(ref > 0.0) || satMagnitudeFromIntensity(ref, r.rangeM) > 20.0)
        {
            ++dark;
            continue;
        }
        errExact.push_back(std::abs(2.5 * std::log10(r.intensity / ref)));
        SatPhotResult rb = evalSatPhotometry(m.groups, budgeted, e, t, in);
        errBudget.push_back(std::abs(2.5 * std::log10(std::max(rb.intensity, 1e-300) / ref)));
    }
    if (unsupported > 0)
    {
        std::printf("  photometry self-test: SKIPPED — attitude aims at a ground site (needs the lock-window aim)\n");
        return true;
    }
    auto p = [](std::vector<double> v, double q) {
        if (v.empty())
            return 0.0;
        std::sort(v.begin(), v.end());
        return v[(size_t)(q * (v.size() - 1))];
    };
    double maxExact = p(errExact, 1.0);
    bool ok = !errExact.empty() && maxExact < 1e-3;
    std::printf("  photometry self-test (%zu visible configurations brighter than mag 20; %d darker skipped):\n",
                errExact.size(), dark);
    std::printf("    %s posed lobes (exact bake, %d lobes) vs posed triangles: |dmag| max %.2e (gate < 1e-3)\n",
                ok ? "ok  " : "FAIL", exactStats.lobes, maxExact);
    std::printf("    info budget bake (%d lobes) vs posed triangles: |dmag| p95 %.3f, max %.3f\n", budgetStats.lobes,
                p(errBudget, 0.95), p(errBudget, 1.0));
    return ok;
}
// Benchmark file check (design page M3): a file's own observations must reproduce the statistics
// the paper printed, which proves the transcription; a differential file's published difference
// must follow from its two referenced files. Printed values are rounded, so a scalar may differ by
// up to half a unit in its last printed digit (2 decimals → 0.005, plus float slack).
bool checkBenchmark(const std::string &path)
{
    Benchmark b;
    std::string err;
    if (!loadBenchmark(path, b, err))
    {
        std::printf("[benchmark] %s: FAILED to load: %s\n", path.c_str(), err.c_str());
        return false;
    }
    std::printf("[benchmark %s] %s (%s)\n  %s %d, \"%s\" — %s\n", b.id.c_str(), b.title.c_str(), b.kind.c_str(),
                b.citationAuthors.c_str(), b.citationYear, b.citationTitle.c_str(), b.citationUrl.c_str());
    std::printf("  satellite: %s; model: %s\n", b.satelliteName.c_str(), b.modelId.empty() ? "(none yet)" : b.modelId.c_str());
    bool ok = true;
    const double kRound = 0.006;
    auto pub = [&](const char *key) -> const BenchPublished * {
        auto it = b.published.find(key);
        return it == b.published.end() ? nullptr : &it->second;
    };

    if (b.kind == "differential")
    {
        std::filesystem::path dir = std::filesystem::path(path).parent_path();
        Benchmark t, base;
        std::string e1, e2;
        bool lt = loadBenchmark((dir / (b.refTest + ".json")).string(), t, e1);
        bool lb = loadBenchmark((dir / (b.refBaseline + ".json")).string(), base, e2);
        ok &= check(lt && lb, "both referenced files load", lt && lb, 1.0);
        const BenchPublished *d = pub("delta_mean_m1000");
        if (lt && lb && d && t.published.count("mean_m1000") && base.published.count("mean_m1000"))
        {
            double delta = t.published["mean_m1000"].value - base.published["mean_m1000"].value;
            ok &= check(std::abs(delta - d->value) < kRound, "published delta = test mean - baseline mean", delta, d->value);
        }
        return ok;
    }

    if (b.observations.empty())
    {
        std::printf("  summary only (the paper publishes no individual observations):\n");
        for (const auto &[key, p] : b.published)
            if (!std::isnan(p.value))
                std::printf("    %-22s %.4g  [%s]\n", key.c_str(), p.value, p.locator.c_str());
        return true;
    }

    BenchStats s = benchStats(b.observations);
    if (const BenchPublished *p = pub("n"))
        ok &= check(s.n == (int)p->value, "observation count", s.n, p->value);
    if (b.censoredCount >= 0)
        ok &= check(s.notSeen == b.censoredCount, "censored ('not seen') count", s.notSeen, b.censoredCount);
    if (const BenchPublished *p = pub("mean_m1000"))
        ok &= check(std::abs(s.mean - p->value) < kRound, "mean m1000", s.mean, p->value);
    if (const BenchPublished *p = pub("median_m1000"))
        ok &= check(std::abs(s.median - p->value) < kRound, "median m1000", s.median, p->value);
    if (const BenchPublished *p = pub("sd_m1000"))
        ok &= check(std::abs(s.sd - p->value) < kRound, "standard deviation m1000", s.sd, p->value);

    // Phase fits: refit with the same degree and variable, and compare the two CURVES over the
    // observed phase range (coefficients of a polynomial fit are strongly correlated, so comparing
    // them one by one would overstate small differences).
    double pMin = 1e9, pMax = -1e9;
    for (const BenchObservation &o : b.observations)
    {
        pMin = std::min(pMin, o.phaseDeg);
        pMax = std::max(pMax, o.phaseDeg);
    }
    for (const char *key : {"phase_fit_linear", "phase_fit_quadratic"})
    {
        const BenchPublished *p = pub(key);
        if (!p || p->coefficients.empty())
            continue;
        const double scale = p->variable == "phase_rad" ? kDeg : 1.0;
        std::vector<double> x, y;
        for (const BenchObservation &o : b.observations)
        {
            x.push_back(o.phaseDeg * scale);
            y.push_back(o.m1000);
        }
        std::vector<double> c = benchPolyFit(x, y, (int)p->coefficients.size() - 1);
        double worst = 0.0;
        for (double ph = pMin; ph <= pMax; ph += 1.0)
            worst = std::max(worst, std::abs(benchPolyEval(c, ph * scale) - benchPolyEval(p->coefficients, ph * scale)));
        char what[96];
        std::snprintf(what, sizeof(what), "%s: max |refit - published| over %.0f-%.0f deg", key, pMin, pMax);
        ok &= check(!c.empty() && worst < 0.02, what, worst, 0.0);
    }
    return ok;
}
} // namespace

int main(int argc, char **argv)
{
    std::vector<std::string> paths;
    std::string outDir = "satellite_models_debug";
    int budget = 48; // SatelliteSim::kSatLobeBudget (the app uses 256 for types flown by <= 10k satellites)
    int shadowSamples = 0; // --shadow-study N
    int selfTestSamples = 0; // --selftest N
    std::vector<std::string> benchmarks; // --benchmark <file>, repeatable
    std::vector<std::string> runs;       // --run-benchmark <file>, repeatable
    BenchRunOptions runOpt;
    bool budgetGiven = false;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--benchmark" && i + 1 < argc)
            benchmarks.push_back(argv[++i]);
        else if (a == "--run-benchmark" && i + 1 < argc)
            runs.push_back(argv[++i]);
        else if (a == "--samples" && i + 1 < argc)
            runOpt.samples = std::atoi(argv[++i]);
        else if (a == "--seed" && i + 1 < argc)
            runOpt.seed = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--report-dir" && i + 1 < argc)
            runOpt.reportDir = argv[++i];
        else if (a == "--models-dir" && i + 1 < argc)
            runOpt.modelsDir = argv[++i];
        else if (a == "--sensitivity")
            runOpt.sensitivity = true;
        else if (a == "--out" && i + 1 < argc)
            outDir = argv[++i];
        else if (a == "--budget" && i + 1 < argc)
        {
            budget = std::atoi(argv[++i]);
            budgetGiven = true;
        }
        else if (a == "--shadow-study" && i + 1 < argc)
            shadowSamples = std::atoi(argv[++i]);
        else if (a == "--selftest" && i + 1 < argc)
            selfTestSamples = std::atoi(argv[++i]);
        else
            paths.push_back(a);
    }
    if (paths.empty() && selfTestSamples <= 0 && benchmarks.empty() && runs.empty())
    {
        std::printf("usage: SatModelTool <model.json> [...] [--out <dir>] [--budget <lobes>] [--shadow-study <N>]\n"
                    "                    [--selftest <N>] [--benchmark <file.json>]...\n"
                    "                    [--run-benchmark <file.json>]... [--samples <N>] [--seed <S>]\n"
                    "                    [--sensitivity] [--report-dir <dir>] [--models-dir <dir>]\n");
        return 2;
    }
    int selfTestFailures = 0;
    int benchFailures = 0;
    for (const std::string &bp : benchmarks)
    {
        benchFailures += checkBenchmark(bp) ? 0 : 1;
        std::printf("\n");
    }
    if (budgetGiven)
        runOpt.lobeBudget = budget;
    int runFailures = 0;
    for (const std::string &rp : runs)
        runFailures += runBenchmarkCommand(rp, runOpt) ? 0 : 1;
    if (selfTestSamples > 0)
    {
        std::printf("[selftest] CPU photometric evaluator\n");
        selfTestFailures += selfTestGeometry() ? 0 : 1;
        std::printf("\n");
    }

    int failures = 0;
    for (const std::string &path : paths)
    {
        const std::string id = std::filesystem::path(path).stem().string();
        SatModel m;
        std::string err;
        std::vector<std::string> warn;
        bool ok = loadSatModel(path, id, m, err, warn);
        for (const std::string &w : warn)
            std::printf("[%s] warning: %s\n", id.c_str(), w.c_str());
        if (!ok)
        {
            std::printf("[%s] FAILED: %s\n", id.c_str(), err.c_str());
            ++failures;
            continue;
        }

        std::vector<SatTri> tris = tessellateSatModel(m);
        SatLobeBakeStats stats;
        std::vector<GpuSatLobe> lobes = bakeSatLobes(m, tris, budget, stats);
        validateSatLobes(m, tris, lobes, stats);
        bool objOk = writeSatModelObj(m, tris, outDir);

        double totalArea = 0.0;
        for (const SatTri &t : tris)
            totalArea += t.area;
        std::printf("[%s] %s\n", id.c_str(), m.name.c_str());
        std::printf("  %zu groups, %zu components, %zu materials, %d triangles, %.1f m2 surface\n",
                    m.groups.size(), m.components.size(), m.materials.size(), stats.triangles, totalArea);
        for (size_t gi = 0; gi < m.groups.size(); ++gi)
        {
            const AttitudeGroup &g = m.groups[gi];
            if (g.parent < 0)
                std::printf("  group %zu '%s': root, %s  +axis->%s, constraint->%s, joint %s\n", gi, g.name.c_str(),
                            attLawName(g.law), attTargetName(g.primaryTarget), attTargetName(g.secondaryTarget),
                            jointModeName(g.jointMode));
            else
                std::printf("  group %zu '%s': child of '%s', hinge (%.2f %.2f %.2f) axis (%.2f %.2f %.2f), joint %s->%s\n",
                            gi, g.name.c_str(), m.groups[g.parent].name.c_str(), g.hingePos.x, g.hingePos.y, g.hingePos.z,
                            g.jointAxis.x, g.jointAxis.y, g.jointAxis.z, jointModeName(g.jointMode),
                            attTargetName(g.jointTarget));
        }
        std::printf("  lobes: %d exact -> %d after budget %d\n", stats.exactLobes, stats.lobes, budget);
        std::printf("  %-4s %-5s %-26s %8s %8s %6s %6s %8s\n", "#", "group", "normal (root triad)", "area",
                    "diffA", "albedo", "F0", "alpha");
        for (size_t li = 0; li < lobes.size(); ++li)
        {
            const GpuSatLobe &L = lobes[li];
            std::printf("  %-4zu %-5u (%7.3f %7.3f %7.3f) %8.2f %8.2f %6.3f %6.3f %8.4f\n", li, L.group, L.normalT.x,
                        L.normalT.y, L.normalT.z, L.area, L.diffArea, L.albedoD, L.f0, std::sqrt(L.alpha2Mat));
        }
        std::printf("  validator vs brute force: |dmag| p95 %.3f, max %.3f\n", stats.p95ErrMag, stats.maxErrMag);

        // Provenance (benchmarking M4): every part needs a `sources` entry; estimates are listed.
        {
            int nSourced = 0, nDerived = 0, nEstimate = 0;
            for (const SatModelSource &s : m.sources)
                (s.status == "sourced" ? nSourced : s.status == "derived" ? nDerived : nEstimate)++;
            std::vector<std::string> missing = unexplainedModelParts(m);
            std::printf("  provenance: %d sourced, %d derived, %d estimate; %zu part(s) unexplained\n", nSourced,
                        nDerived, nEstimate, missing.size());
            // One line per entry (an entry listing several subjects is expanded by the loader).
            std::string lastValue;
            for (const SatModelSource &s : m.sources)
                if (s.status != "sourced" && s.value != lastValue)
                {
                    std::printf("    %-8s %-18s %s\n", s.status.c_str(), s.subject.c_str(), s.value.c_str());
                    lastValue = s.value;
                }
            for (const std::string &p : missing)
                std::printf("    UNEXPLAINED %s\n", p.c_str());
        }
        if (shadowSamples > 0)
        {
            SatShadowStudy ss = studySatShadowing(m, tris, shadowSamples);
            std::printf("  shadowing (all groups, posed, %d lit configs): dimming median %.3f, p90 %.3f, p99 %.3f, "
                        "max %.2f mag; >0.1 mag in %.1f%%, >0.5 mag in %.1f%%\n",
                        ss.samples, ss.medianDmag, ss.p90Dmag, ss.p99Dmag, ss.maxDmag, 100.0 * ss.fracOver01,
                        100.0 * ss.fracOver05);
            std::printf("    brighter half only: p90 %.3f mag; >0.1 mag in %.1f%%, >0.5 mag in %.1f%%\n",
                        ss.brightP90Dmag, 100.0 * ss.brightFracOver01, 100.0 * ss.brightFracOver05);
        }
        if (selfTestSamples > 0)
            selfTestFailures += selfTestModel(m, tris, budget, selfTestSamples) ? 0 : 1;
        std::printf("  OBJ: %s\n\n", objOk ? (std::filesystem::path(outDir) / (id + "_rest.obj / _sunlit.obj")).string().c_str()
                                           : "export FAILED");
    }
    if (selfTestSamples > 0)
        std::printf("[selftest] %s\n", selfTestFailures ? "FAILED" : "passed");
    if (!benchmarks.empty())
        std::printf("[benchmark files] %s\n", benchFailures ? "FAILED" : "all reproduce their published values");
    if (!runs.empty())
        std::printf("[benchmark runs] %d of %zu within tolerance\n", (int)runs.size() - runFailures, runs.size());
    return (failures || selfTestFailures || benchFailures || runFailures) ? 1 : 0;
}
