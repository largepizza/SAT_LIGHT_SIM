// SatBenchmark — see SatBenchmark.h for the overview.
#include "SatBenchmark.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace
{
double numOr(const nlohmann::json &j, const char *key, double def)
{
    return (j.contains(key) && j[key].is_number()) ? j[key].get<double>() : def;
}

std::string strOr(const nlohmann::json &j, const char *key)
{
    return (j.contains(key) && j[key].is_string()) ? j[key].get<std::string>() : std::string();
}

std::vector<double> numArray(const nlohmann::json &j, const char *key)
{
    std::vector<double> v;
    if (j.contains(key) && j[key].is_array())
        for (const auto &e : j[key])
            v.push_back(e.is_number() ? e.get<double>() : std::numeric_limits<double>::quiet_NaN());
    return v;
}
} // namespace

bool loadBenchmark(const std::string &path, Benchmark &out, std::string &err)
{
    std::ifstream f(path);
    if (!f)
    {
        err = "cannot open " + path;
        return false;
    }
    nlohmann::json j;
    try
    {
        f >> j;
    }
    catch (const std::exception &e)
    {
        err = std::string("JSON parse error: ") + e.what();
        return false;
    }
    if (strOr(j, "format") != "sat-light-sim-benchmark/1")
    {
        err = "unknown format '" + strOr(j, "format") + "' (expected sat-light-sim-benchmark/1)";
        return false;
    }

    Benchmark b;
    b.path = path;
    b.id = strOr(j, "id");
    b.title = strOr(j, "title");
    b.gated = j.value("gated", true);
    b.kind = strOr(j, "kind");
    if (b.id.empty() || (b.kind != "distribution" && b.kind != "differential"))
    {
        err = "missing id, or kind is not 'distribution'/'differential'";
        return false;
    }
    if (j.contains("citation"))
    {
        const auto &c = j["citation"];
        b.citationAuthors = strOr(c, "authors");
        b.citationTitle = strOr(c, "title");
        b.citationUrl = strOr(c, "url");
        b.citationYear = (int)numOr(c, "year", 0);
    }
    if (b.citationUrl.empty())
    {
        err = "every benchmark needs citation.url";
        return false;
    }
    if (j.contains("satellite"))
    {
        b.satelliteName = strOr(j["satellite"], "name");
        b.modelId = strOr(j["satellite"], "model");
    }
    if (j.contains("published") && j["published"].is_object())
        for (auto it = j["published"].begin(); it != j["published"].end(); ++it)
        {
            BenchPublished p;
            p.value = numOr(*it, "value", p.value);
            p.uncertainty = numOr(*it, "uncertainty", p.uncertainty);
            p.coefficients = numArray(*it, "coefficients");
            p.coefficientUncertainties = numArray(*it, "coefficient_uncertainties");
            p.variable = strOr(*it, "variable");
            p.locator = strOr(*it, "locator");
            if (p.locator.empty())
            {
                err = "published." + it.key() + " has no locator";
                return false;
            }
            b.published[it.key()] = p;
        }
    if (j.contains("sampling"))
    {
        const auto &s = j["sampling"];
        if (s.contains("shell"))
        {
            b.shellAltKm = numOr(s["shell"], "alt_km", 0.0);
            b.shellInclDeg = numOr(s["shell"], "incl_deg", 0.0);
        }
        if (s.contains("period"))
        {
            b.periodStart = strOr(s["period"], "start");
            b.periodEnd = strOr(s["period"], "end");
        }
        if (s.contains("sites") && s["sites"].is_array())
            for (const auto &e : s["sites"])
            {
                BenchSite site;
                site.name = strOr(e, "name");
                site.latDeg = numOr(e, "lat_deg", 0.0);
                site.lonDeg = numOr(e, "lon_deg", 0.0);
                site.count = (int)numOr(e, "count", -1.0);
                site.instrument = strOr(e, "instrument");
                b.sites.push_back(site);
            }
        if (s.contains("censoring"))
        {
            b.censoredCount = (int)numOr(s["censoring"], "count", -1.0);
            b.censorLimitMag = numOr(s["censoring"], "not_seen_assigned_apparent", b.censorLimitMag);
            b.censorThresholdMag = numOr(s["censoring"], "limiting_mag_apparent", b.censorThresholdMag);
        }
    }
    if (j.contains("references"))
    {
        b.refTest = strOr(j["references"], "test");
        b.refBaseline = strOr(j["references"], "baseline");
    }
    if (b.kind == "differential" && (b.refTest.empty() || b.refBaseline.empty()))
    {
        err = "a differential benchmark needs references.test and references.baseline";
        return false;
    }

    // Observations: rows are arrays, columns named by "columns".
    if (j.contains("observations") && j["observations"].is_array() && !j["observations"].empty())
    {
        std::vector<std::string> cols;
        for (const auto &c : j.value("columns", nlohmann::json::array()))
            cols.push_back(c.get<std::string>());
        auto col = [&](const char *name) {
            auto it = std::find(cols.begin(), cols.end(), name);
            return it == cols.end() ? -1 : (int)(it - cols.begin());
        };
        const int cSat = col("sat"), cDay = col("day_2020"), cOps = col("ops_days"), cPhase = col("phase_deg"),
                  cMag = col("m1000"), cSrc = col("source"), cNotSeen = col("not_seen");
        if (cPhase < 0 || cMag < 0)
        {
            err = "observations need at least the phase_deg and m1000 columns";
            return false;
        }
        size_t row = 0;
        for (const auto &r : j["observations"])
        {
            ++row;
            if (!r.is_array() || r.size() != cols.size())
            {
                err = "observation row " + std::to_string(row) + " does not match the column list";
                return false;
            }
            BenchObservation o;
            if (cSat >= 0)
                o.sat = r[cSat].get<int>();
            if (cDay >= 0)
                o.day2020 = r[cDay].get<int>();
            if (cOps >= 0)
                o.opsDays = r[cOps].get<int>();
            o.phaseDeg = r[cPhase].get<double>();
            o.m1000 = r[cMag].get<double>();
            if (cSrc >= 0)
                o.source = r[cSrc].get<std::string>();
            if (cNotSeen >= 0)
                o.notSeen = r[cNotSeen].get<bool>();
            b.observations.push_back(o);
        }
    }
    out = std::move(b);
    return true;
}

BenchStats benchStats(const std::vector<BenchObservation> &obs)
{
    BenchStats s;
    s.n = (int)obs.size();
    if (obs.empty())
        return s;
    std::vector<double> m;
    for (const BenchObservation &o : obs)
    {
        m.push_back(o.m1000);
        s.notSeen += o.notSeen ? 1 : 0;
    }
    double sum = 0.0;
    for (double v : m)
        sum += v;
    s.mean = sum / m.size();
    double ss = 0.0;
    for (double v : m)
        ss += (v - s.mean) * (v - s.mean);
    s.sd = m.size() > 1 ? std::sqrt(ss / (m.size() - 1)) : 0.0;
    s.sem = s.sd / std::sqrt((double)m.size());
    std::sort(m.begin(), m.end());
    s.median = m.size() % 2 ? m[m.size() / 2] : 0.5 * (m[m.size() / 2 - 1] + m[m.size() / 2]);
    return s;
}

std::vector<double> benchPolyFit(const std::vector<double> &x, const std::vector<double> &y, int degree)
{
    const int n = degree + 1;
    if (degree < 0 || degree > 4 || x.size() != y.size() || (int)x.size() < n)
        return {};
    // Normal equations, solved by Gaussian elimination with partial pivoting (tiny, well-scaled
    // systems here: x in degrees, degree ≤ 2 in practice).
    std::vector<std::vector<double>> A(n, std::vector<double>(n + 1, 0.0));
    for (size_t k = 0; k < x.size(); ++k)
    {
        std::vector<double> p(2 * n, 1.0);
        for (int i = 1; i < 2 * n; ++i)
            p[i] = p[i - 1] * x[k];
        for (int r = 0; r < n; ++r)
        {
            for (int c = 0; c < n; ++c)
                A[r][c] += p[r + c];
            A[r][n] += p[r] * y[k];
        }
    }
    for (int c = 0; c < n; ++c)
    {
        int piv = c;
        for (int r = c + 1; r < n; ++r)
            if (std::abs(A[r][c]) > std::abs(A[piv][c]))
                piv = r;
        if (std::abs(A[piv][c]) < 1e-300)
            return {};
        std::swap(A[c], A[piv]);
        for (int r = 0; r < n; ++r)
            if (r != c)
            {
                double f = A[r][c] / A[c][c];
                for (int k = c; k <= n; ++k)
                    A[r][k] -= f * A[c][k];
            }
    }
    std::vector<double> coef(n);
    for (int i = 0; i < n; ++i)
        coef[i] = A[i][n] / A[i][i];
    return coef;
}

double benchPolyEval(const std::vector<double> &c, double x)
{
    double y = 0.0;
    for (size_t i = c.size(); i-- > 0;)
        y = y * x + c[i];
    return y;
}
