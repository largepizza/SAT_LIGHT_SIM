// SatBench — see SatBench.h for the overview.
#include "SatBench.h"
#include "SatPhotometry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

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

bool benchEvalSample(const std::vector<AttitudeGroup> &groups, const std::vector<GpuSatLobe> &lobes,
                     const SatOcclusion *occ, const SatOrbitElems &orbit, double t, glm::dvec3 obs, glm::dvec3 sun,
                     double minElevationDeg, double extinctionK, BenchSample &s, const SatGroundSiteAim *aim,
                     uint32_t satIndex)
{
    const glm::dvec3 up = glm::normalize(obs);
    // Cheap elevation pre-check before the full evaluation.
    const SatOrbitState st = satOrbitStateAt(orbit, t);
    const glm::dvec3 rel = st.posEci - obs;
    if (glm::dot(rel, up) < std::sin(minElevationDeg * kPi / 180.0) * glm::length(rel))
        return false;
    SatPhotInputs in;
    in.sunDirEci = sun;
    in.obsEci = obs;
    if (aim)
    {
        in.hasSiteIdeal = true;
        in.siteIdeal = satGroundSiteIdeal(*aim, orbit, satIndex, t, sun).ideal;
    }
    const SatPhotResult r = evalSatPhotometry(groups, lobes, orbit, t, in, nullptr, occ);
    if (!r.supported || r.litFactor < 0.999)
        return false;
    s = BenchSample{};
    s.tJ2000 = t;
    s.sunAltDeg = std::asin(std::clamp(glm::dot(sun, up), -1.0, 1.0)) * 180.0 / kPi;
    s.elevationDeg = std::asin(std::clamp(r.sinElevation, -1.0, 1.0)) * 180.0 / kPi;
    glm::dvec3 east = glm::cross(glm::dvec3(0.0, 0.0, 1.0), up);
    east = glm::length(east) > 1e-12 ? glm::normalize(east) : glm::dvec3(1.0, 0.0, 0.0);
    const double az = std::atan2(glm::dot(rel, east), glm::dot(rel, glm::cross(up, east))) * 180.0 / kPi;
    s.azimuthDeg = az < 0.0 ? az + 360.0 : az;
    s.rangeM = r.rangeM;
    s.phaseDeg = r.phaseAngleRad * 180.0 / kPi;
    s.offSpecularDeg = r.offSpecularRad * 180.0 / kPi;
    s.mag = r.magnitude;
    s.m1000 = r.magnitude1000;
    s.magApparent = r.magnitude + (extinctionK > 0.0 ? atmExtinctionMag(obs, glm::normalize(rel), r.rangeM, extinctionK)
                                                     : 0.0);
    s.dominantLobe = r.dominantLobe;
    return true;
}

namespace
{
constexpr const char *kSamplesFormat = "sat-light-sim-samples/1";
constexpr const char *kSamplesColumns = "t_j2000,site,sun_alt_deg,elevation_deg,azimuth_deg,range_m,phase_deg,"
                                        "off_specular_deg,mag,m1000,mag_apparent,censored,dominant_lobe,satellite,pass";

std::string csvMag(double v)
{
    if (!std::isfinite(v))
        return "";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
}
} // namespace

bool writeBenchSamplesCsv(const std::string &path, const BenchCsvHeader &header, const std::vector<BenchSample> &samples,
                          std::string &err)
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
    {
        err = "cannot write " + path;
        return false;
    }
    f << "# format: " << kSamplesFormat << "\n";
    for (const auto &kv : header)
        f << "# " << kv.first << ": " << kv.second << "\n";
    f << kSamplesColumns << "\n";
    char buf[256];
    for (const BenchSample &x : samples)
    {
        std::snprintf(buf, sizeof(buf), "%.17g,%d,%.6f,%.6f,%.6f,%.3f,%.6f,%.6f,", x.tJ2000, x.site, x.sunAltDeg,
                      x.elevationDeg, x.azimuthDeg, x.rangeM, x.phaseDeg, x.offSpecularDeg);
        f << buf << csvMag(x.mag) << "," << csvMag(x.m1000) << "," << csvMag(x.magApparent) << ","
          << (x.censored ? 1 : 0) << "," << x.dominantLobe << "," << x.satellite << "," << x.pass << "\n";
    }
    if (!f)
    {
        err = "write failed: " + path;
        return false;
    }
    return true;
}

bool readBenchSamplesCsv(const std::string &path, BenchCsvHeader &header, std::vector<BenchSample> &samples,
                         std::string &err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        err = "cannot open " + path;
        return false;
    }
    header.clear();
    samples.clear();
    std::string line, format;
    bool sawColumns = false;
    int lineNo = 0;
    auto num = [](const std::string &v) { return v.empty() ? INFINITY : std::strtod(v.c_str(), nullptr); };
    while (std::getline(f, line))
    {
        ++lineNo;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        if (line[0] == '#')
        {
            const size_t colon = line.find(':');
            if (colon == std::string::npos || colon < 2)
                continue;
            const std::string key = line.substr(2, colon - 2), v = colon + 2 <= line.size() ? line.substr(colon + 2) : "";
            if (key == "format")
                format = v;
            else
                header.emplace_back(key, v);
            continue;
        }
        if (!sawColumns)
        {
            if (line != kSamplesColumns)
            {
                err = path + ":" + std::to_string(lineNo) + ": unexpected column header";
                return false;
            }
            sawColumns = true;
            continue;
        }
        std::vector<std::string> fld;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ','))
            fld.push_back(cell);
        if (line.back() == ',')
            fld.push_back("");
        if (fld.size() != 15)
        {
            err = path + ":" + std::to_string(lineNo) + ": expected 15 fields, got " + std::to_string(fld.size());
            return false;
        }
        BenchSample x;
        x.tJ2000 = num(fld[0]);
        x.site = std::atoi(fld[1].c_str());
        x.sunAltDeg = num(fld[2]);
        x.elevationDeg = num(fld[3]);
        x.azimuthDeg = num(fld[4]);
        x.rangeM = num(fld[5]);
        x.phaseDeg = num(fld[6]);
        x.offSpecularDeg = num(fld[7]);
        x.mag = num(fld[8]);
        x.m1000 = num(fld[9]);
        x.magApparent = num(fld[10]);
        x.censored = fld[11] == "1";
        x.dominantLobe = std::atoi(fld[12].c_str());
        x.satellite = std::atoi(fld[13].c_str());
        x.pass = std::atoi(fld[14].c_str());
        samples.push_back(x);
    }
    if (format != kSamplesFormat)
    {
        err = path + ": format '" + format + "' is not " + kSamplesFormat;
        return false;
    }
    return true;
}

bool runBulkExport(const BulkExportSpec &spec, const std::vector<BulkExportSat> &sats, std::vector<BenchSample> &out,
                   std::atomic<float> *progress, const std::atomic<bool> *cancel)
{
    out.clear();
    // The instants with the Sun inside the window depend only on the observer: find them once.
    struct Instant
    {
        double t;
        glm::dvec3 obs, sun;
    };
    std::vector<Instant> instants;
    for (double t = spec.t0; t < spec.t1; t += spec.cadenceS)
    {
        const glm::dvec3 obs = observerEciAt(spec.obsDirEcef, spec.obsRadiusM, t);
        const glm::dvec3 sun = sunDirEciAt(t);
        const double sunAlt = std::asin(std::clamp(glm::dot(sun, glm::normalize(obs)), -1.0, 1.0)) * 180.0 / kPi;
        if (sunAlt >= spec.sunAltMinDeg && sunAlt <= spec.sunAltMaxDeg)
            instants.push_back({t, obs, sun});
    }
    int pass = -1;
    for (size_t si = 0; si < sats.size(); ++si)
    {
        if (cancel && cancel->load())
            return false;
        double lastT = -INFINITY;
        for (const Instant &in : instants)
        {
            BenchSample x;
            if (!benchEvalSample(*spec.groups, *spec.lobes, spec.occlusion, sats[si].orbit, in.t, in.obs, in.sun,
                                 spec.minElevationDeg, spec.extinctionK, x, spec.groundAim,
                                 (uint32_t)std::max(0, sats[si].index)))
                continue;
            if (in.t - lastT > 1.5 * spec.cadenceS)
                ++pass; // a gap: a new pass (or a new twilight)
            lastT = in.t;
            x.site = 0;
            x.satellite = sats[si].index;
            x.pass = pass;
            out.push_back(x);
        }
        if (progress && (si & 63) == 0)
            progress->store((float)(si + 1) / (float)sats.size());
    }
    if (progress)
        progress->store(1.0f);
    return true;
}

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
            BenchSample s;
            if (!benchEvalSample(m.groups, lobes, occ, e, t, obs, sun, cfg.minElevationDeg, 0.0, s))
                continue;
            s.site = (int)si;
            // The paper's censoring rule: too faint to see -> an assigned apparent magnitude.
            if (canCensor && visualSite && !(s.mag <= b.censorThresholdMag))
            {
                s.censored = true;
                s.m1000 = satMagnitudeTo1000km(b.censorLimitMag, s.rangeM);
                ++notSeen;
            }
            else if (!std::isfinite(s.m1000))
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
