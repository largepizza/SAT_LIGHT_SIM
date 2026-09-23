// SatBench — see SatBench.h for the overview.
#include "SatBench.h"
#include "SatPhotometry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

using namespace satphot;

namespace
{
// Portable uniform double in [0, 1): the mt19937_64 sequence is fixed by the standard; the std
// distributions are not, so they are never used here.
struct BenchRng
{
    std::mt19937_64 eng;
    explicit BenchRng(uint64_t seed) : eng(seed) {}
    double uniform() { return (double)(eng() >> 11) * (1.0 / 9007199254740992.0); }
};

// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil).
long long daysFromCivil(long long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

glm::dvec3 siteEcef(const BenchSite &s)
{
    const double lat = s.latDeg * kPi / 180.0, lon = s.lonDeg * kPi / 180.0;
    return {std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)};
}
} // namespace

bool benchIsoDateToJ2000(const std::string &iso, double &tJ2000)
{
    int y = 0, m = 0, d = 0;
    if (std::sscanf(iso.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31)
        return false;
    // J2000 = 2000-01-01 12:00 = 10957.5 days after the Unix epoch.
    tJ2000 = ((double)daysFromCivil(y, (unsigned)m, (unsigned)d) - 10957.5) * 86400.0;
    return true;
}

BenchStats benchStatsOfValues(const std::vector<double> &m1000, int notSeen)
{
    std::vector<BenchObservation> obs(m1000.size());
    for (size_t i = 0; i < m1000.size(); ++i)
        obs[i].m1000 = m1000[i];
    BenchStats s = benchStats(obs);
    s.notSeen = notSeen;
    return s;
}

bool runDistributionBenchmark(const Benchmark &b, const SatModel &m, const std::vector<GpuSatLobe> &lobes,
                              const BenchRunConfig &cfg, BenchRunResult &out, std::string &err,
                              const SatOcclusion *occ)
{
    out = BenchRunResult{};
    out.config = cfg;
    if (b.sites.empty() || b.shellAltKm <= 0.0)
    {
        err = "benchmark has no sites or no shell altitude";
        return false;
    }
    double t0 = 0.0, t1 = 0.0;
    const std::string start = b.periodStart.empty() ? cfg.periodStartFallback : b.periodStart;
    if (!benchIsoDateToJ2000(start, t0) || !benchIsoDateToJ2000(b.periodEnd, t1))
    {
        err = "benchmark period is missing or malformed ('" + start + "' .. '" + b.periodEnd + "')";
        return false;
    }
    t1 += 86400.0; // the end date is inclusive
    if (t1 <= t0)
    {
        err = "benchmark period ends before it starts";
        return false;
    }

    // Site weights: the share of the paper's own observations each site contributed (an
    // observation's `source` named in the site name), else the site's published count, else equal.
    std::vector<double> w(b.sites.size(), 0.0);
    for (size_t i = 0; i < b.sites.size(); ++i)
    {
        for (const BenchObservation &o : b.observations)
            if (!o.source.empty() && b.sites[i].name.find(o.source) != std::string::npos)
                w[i] += 1.0;
        if (w[i] == 0.0 && b.sites[i].count > 0)
            w[i] = b.sites[i].count;
    }
    double wSum = 0.0;
    for (double &x : w)
        wSum += x;
    if (wSum <= 0.0)
    {
        std::fill(w.begin(), w.end(), 1.0);
        wSum = (double)w.size();
    }

    const double rSat = kEarthRadiusM + b.shellAltKm * 1000.0;
    const double incl = b.shellInclDeg * kPi / 180.0;
    const double n = std::sqrt(kGM / (rSat * rSat * rSat));
    const double sinMinEl = std::sin(cfg.minElevationDeg * kPi / 180.0);
    const bool canCensor = cfg.censor && !std::isnan(b.censorThresholdMag) && !std::isnan(b.censorLimitMag);
    constexpr int kSatTriesPerTime = 4000;

    BenchRng rng(cfg.seed);
    std::vector<double> m1000, phase;
    int notSeen = 0;
    const int maxTimeDraws = std::max(1000, cfg.samples * 400);
    while ((int)out.samples.size() < cfg.samples && out.timeDraws < maxTimeDraws)
    {
        ++out.timeDraws;
        // Site, then a time in the period with the Sun inside the twilight window at that site.
        double pick = rng.uniform() * wSum;
        size_t si = 0;
        while (si + 1 < w.size() && pick >= w[si])
            pick -= w[si++];
        const double t = t0 + rng.uniform() * (t1 - t0);
        const glm::dvec3 obs = observerEciAt(siteEcef(b.sites[si]), kEarthRadiusM, t);
        const glm::dvec3 up = glm::normalize(obs);
        const glm::dvec3 sun = sunDirEciAt(t);
        const double sunAltDeg = std::asin(std::clamp(glm::dot(sun, up), -1.0, 1.0)) * 180.0 / kPi;
        if (sunAltDeg < cfg.sunAltMinDeg || sunAltDeg > cfg.sunAltMaxDeg)
            continue;

        // A random satellite of the shell (uniform RAAN and argument of latitude at time t) that is
        // above the elevation limit and fully sunlit (penumbra excluded, as in the papers).
        const bool visualSite = b.sites[si].instrument.find("visual") != std::string::npos;
        for (int k = 0; k < kSatTriesPerTime; ++k)
        {
            ++out.satelliteDraws;
            SatOrbitElems e;
            e.raan = rng.uniform() * 2.0 * kPi;
            e.incl = incl;
            e.rSatM = rSat;
            e.meanMot = n;
            const double u = rng.uniform() * 2.0 * kPi;
            e.u0 = std::fmod(u - std::fmod(n * t, 2.0 * kPi) + 4.0 * kPi, 2.0 * kPi);
            // Cheap elevation pre-check before the full evaluation.
            const SatOrbitState st = satOrbitStateAt(e, t);
            const glm::dvec3 rel = st.posEci - obs;
            if (glm::dot(rel, up) < sinMinEl * glm::length(rel))
                continue;
            SatPhotInputs in;
            in.sunDirEci = sun;
            in.obsEci = obs;
            SatPhotResult r = evalSatPhotometry(m.groups, lobes, e, t, in, nullptr, occ);
            if (!r.supported || r.litFactor < 0.999)
                continue;

            BenchSample s;
            s.tJ2000 = t;
            s.site = (int)si;
            s.sunAltDeg = sunAltDeg;
            s.elevationDeg = std::asin(std::clamp(r.sinElevation, -1.0, 1.0)) * 180.0 / kPi;
            s.rangeM = r.rangeM;
            s.phaseDeg = r.phaseAngleRad * 180.0 / kPi;
            s.offSpecularDeg = r.offSpecularRad * 180.0 / kPi;
            s.mag = r.magnitude;
            s.dominantLobe = r.dominantLobe;
            // The paper's censoring rule: too faint to see -> an assigned apparent magnitude.
            if (canCensor && visualSite && !(r.magnitude <= b.censorThresholdMag))
            {
                s.censored = true;
                s.m1000 = satMagnitudeTo1000km(b.censorLimitMag, r.rangeM);
                ++notSeen;
            }
            else if (std::isfinite(r.magnitude1000))
                s.m1000 = r.magnitude1000;
            else
                break; // not recorded (an instrument with no censoring rule saw nothing): new time
            out.samples.push_back(s);
            m1000.push_back(s.m1000);
            phase.push_back(s.phaseDeg);
            break;
        }
    }
    if (out.samples.empty())
    {
        err = "no sample satisfied the sampling constraints";
        return false;
    }
    out.stats = benchStatsOfValues(m1000, notSeen);
    out.phaseFitLinear = benchPolyFit(phase, m1000, 1);
    int bright = 0;
    for (double v : m1000)
        bright += v < 5.0 ? 1 : 0;
    out.brightFrac = (double)bright / m1000.size();
    return true;
}
