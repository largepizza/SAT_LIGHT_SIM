// Comets.cpp — comet orbits, brightness, catalog and tail geometry (2026-10-27; CLAUDE.md "Comets"; Comets.h).
#include "Comets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

glm::dvec3 skyEarthHelioAt(double t); // SatelliteSim.cpp: keplerEclipticPos(kEarthElements) at t, the planets' Earth

namespace comet
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
double rad(double d) { return d * (kPi / 180.0); }

// Stumpff functions C(z), S(z), series near 0.
void stumpff(double z, double &C, double &S)
{
    if (z > 1e-6)
    {
        const double s = std::sqrt(z);
        C = (1.0 - std::cos(s)) / z;
        S = (s - std::sin(s)) / (s * z);
    }
    else if (z < -1e-6)
    {
        const double s = std::sqrt(-z);
        C = (std::cosh(s) - 1.0) / (-z);
        S = (std::sinh(s) - s) / (s * (-z));
    }
    else
    {
        C = 0.5 - z / 24.0 + z * z / 720.0;
        S = 1.0 / 6.0 - z / 120.0 + z * z / 5040.0;
    }
}

uint64_t mix64(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x;
}
struct Rng
{
    uint64_t s;
    double u() { s = mix64(s + 0x9E3779B97F4A7C15ull); return (double)(s >> 11) * (1.0 / 9007199254740992.0); }
};

// Days since J2000 of a calendar date (proleptic Gregorian, 0h UTC) -> seconds since J2000 (12:00).
double secOfDate(int y, int m, int d)
{
    // days from civil (H. Hinnant)
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const long days = (long)era * 146097L + (long)doe - 719468L; // since 1970-01-01
    const long j2000 = 10957;                                    // 2000-01-01
    return ((double)(days - j2000) - 0.5) * kDayS;
}
int yearOf(double t)
{
    // the civil year of t (s since J2000)
    const double days = t / kDayS + 0.5 + 10957.0; // since 1970-01-01
    long z = (long)std::floor(days) + 719468L;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long y = (long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return (int)(y + (m <= 2));
}

// The scripted Great Comet of 2037: tools/comet_design.py solved these against the sim's own Earth (kEarthElements):
// a retrograde Kreutz-like sungrazer (q 0.0062 AU = 1.33 solar radii, i 142 deg) at perihelion 2037-02-17 16:49 UTC,
// passing 560,000 km from the Earth (outside the Moon's orbit, over the northern hemisphere) on 2037-03-17 03:30 UTC.
Elements makeGreat()
{
    Elements e;
    std::snprintf(e.name, sizeof(e.name), "C/2036 X1 (Great Comet of 2037)");
    e.q = 0.0062;
    e.e = 1.0;
    e.iDeg = 141.9995;
    e.nodeDeg = 176.6499;
    e.periDeg = 189.4051;
    e.tPeri = 1171774146.9;
    e.H = 6.0;
    e.n = 4.0;
    e.rSat = 0.04;
    e.dust = 1.6;
    e.gas = 1.3;
    e.comaKm = 2.0e5;
    e.seed = 0x6C0337u;
    e.great = true;
    finalize(e);
    return e;
}

const char *const kDiscoverers[] = {"PANSTARRS", "ATLAS", "Rubin", "Lemmon", "ZTF",      "NEOWISE",  "Siding Spring",
                                    "Catalina",  "Borisov", "Tsuchinshan", "Nishimura", "Leonard", "Machholz", "Bradfield",
                                    "Kowalski",  "Lovejoy", "Gibbs", "Jacques", "Iwamoto", "Okazaki"};
} // namespace

void finalize(Elements &el)
{
    const double i = rad(el.iDeg), O = rad(el.nodeDeg), w = rad(el.periDeg);
    const double cO = std::cos(O), sO = std::sin(O), ci = std::cos(i), si = std::sin(i), cw = std::cos(w), sw = std::sin(w);
    el.pHat = glm::dvec3(cO * cw - sO * sw * ci, sO * cw + cO * sw * ci, sw * si);
    el.tHat = glm::dvec3(-cO * sw - sO * cw * ci, -sO * sw + cO * cw * ci, cw * si);
    el.rPeri = el.pHat * el.q;
    el.vPeri = el.tHat * std::sqrt(kMuSun * (1.0 + el.e) / el.q);
}

bool propagate(const State &s0, double dt, double mu, State &out)
{
    out = s0;
    if (dt == 0.0)
        return true;
    const double r0 = glm::length(s0.r);
    const double v2 = glm::dot(s0.v, s0.v);
    const double vr0 = glm::dot(s0.r, s0.v) / r0;
    const double sm = std::sqrt(mu);
    const double alpha = 2.0 / r0 - v2 / mu; // 1/a
    // Initial guess: ellipse sqrt(mu) alpha dt; otherwise a parabolic-ish estimate.
    double x = (alpha > 1e-6) ? sm * alpha * dt : sm * dt / r0;
    if (alpha <= 1e-6)
    {
        // parabola-like: solve r0 x + (r0 vr0 / sqrt mu) x^2/2 + x^3/6 = sqrt(mu) dt roughly by the cube root
        const double xc = std::cbrt(6.0 * sm * std::abs(dt)) * (dt < 0.0 ? -1.0 : 1.0);
        if (std::abs(xc) < std::abs(x)) x = xc;
    }
    const double a1 = r0 * vr0 / sm, a2 = 1.0 - alpha * r0;
    bool ok = false;
    for (int it = 0; it < 60; ++it)
    {
        const double z = alpha * x * x;
        double C, S;
        stumpff(z, C, S);
        const double F = a1 * x * x * C + a2 * x * x * x * S + r0 * x - sm * dt;
        const double dF = a1 * x * (1.0 - z * S) + a2 * x * x * C + r0;
        // Laguerre-Conway step (n = 5): robust for any conic
        const double ddF = a1 * (1.0 - z * C) + a2 * x * (1.0 - z * S);
        const double n = 5.0;
        const double disc = std::abs((n - 1.0) * (n - 1.0) * dF * dF - n * (n - 1.0) * F * ddF);
        const double den = dF + (dF >= 0.0 ? 1.0 : -1.0) * std::sqrt(disc);
        const double dx = (den != 0.0) ? n * F / den : F / dF;
        x -= dx;
        if (std::abs(dx) <= 1e-12 * std::max(1.0, std::abs(x)))
        {
            ok = true;
            break;
        }
    }
    if (!ok)
        return false;
    const double z = alpha * x * x;
    double C, S;
    stumpff(z, C, S);
    const double f = 1.0 - x * x / r0 * C;
    const double g = dt - x * x * x * S / sm;
    out.r = f * s0.r + g * s0.v;
    const double rn = glm::length(out.r);
    const double fd = sm / (rn * r0) * (alpha * x * x * x * S - x);
    const double gd = 1.0 - x * x / rn * C;
    out.v = fd * s0.r + gd * s0.v;
    return std::isfinite(out.r.x) && std::isfinite(out.v.x);
}

State stateAt(const Elements &el, double t)
{
    State p{el.rPeri, el.vPeri}, s;
    propagate(p, (t - el.tPeri) / kDayS, kMuSun, s);
    return s;
}

double dustPhase(double scatterDeg)
{
    auto hg = [](double g, double c) { return (1.0 - g * g) / std::pow(1.0 + g * g - 2.0 * g * c, 1.5); };
    const double c = std::cos(rad(scatterDeg));
    const double k = 0.95, gf = 0.9, gb = -0.7;
    const double p = k * hg(gf, c) + (1.0 - k) * hg(gb, c);
    const double p90 = k * hg(gf, 0.0) + (1.0 - k) * hg(gb, 0.0);
    return p / p90;
}

double magnitude(const Elements &el, double r, double delta, double phaseDeg)
{
    const double rEff = std::sqrt(r * r + el.rSat * el.rSat);
    double m = el.H + 5.0 * std::log10(std::max(delta, 1e-9)) + 2.5 * el.n * std::log10(rEff);
    // Marcus (2007): the dust's share of the light at 90 deg is delta90 / (1 + delta90); scattered by Phi.
    const double d90 = 0.6 * el.dust;
    const double phi = dustPhase(180.0 - phaseDeg);
    m -= 2.5 * std::log10(d90 / (1.0 + d90) * phi + 1.0 / (1.0 + d90));
    return m;
}

const Elements &greatComet()
{
    static const Elements g = makeGreat();
    return g;
}

void minorCometsOfYear(int year, std::vector<Elements> &out)
{
    constexpr int kPerYear = 6;
    const double y0 = secOfDate(year, 1, 1), y1 = secOfDate(year + 1, 1, 1);
    int nP = 0, nC = 0;
    for (int k = 0; k < kPerYear; ++k)
    {
        Rng r{mix64(((uint64_t)(uint32_t)year << 20) ^ (uint64_t)k * 0x632BE59BD9B4E019ull ^ 0xC03E7ull)};
        Elements e;
        e.seed = (uint32_t)(r.s >> 7);
        e.tPeri = y0 + r.u() * (y1 - y0);
        const bool jfc = r.u() < 0.4;
        e.periodic = jfc;
        if (jfc)
        {
            e.q = 0.9 + 1.4 * r.u() * r.u();
            e.e = 0.3 + 0.35 * r.u();
            e.iDeg = 2.0 + 24.0 * r.u() * r.u();
            e.H = 8.0 + 5.0 * r.u();
            e.dust = 0.5 + 0.8 * r.u();
            e.gas = 0.3 + 0.5 * r.u();
            e.comaKm = 4.0e4;
        }
        else
        {
            e.q = 0.35 + 2.4 * std::pow(r.u(), 1.6);
            e.e = 0.995 + 0.007 * r.u(); // long-period to slightly hyperbolic
            e.iDeg = std::acos(1.0 - 2.0 * r.u()) * 180.0 / kPi;
            // Most land at mag 6-12 at their best; about one a year is a naked-eye object.
            e.H = 4.5 + 6.5 * std::pow(r.u(), 1.1);
            e.dust = 0.4 + 1.0 * r.u();
            e.gas = 0.5 + 1.0 * r.u();
            e.comaKm = 8.0e4;
        }
        e.nodeDeg = 360.0 * r.u();
        e.periDeg = 360.0 * r.u();
        e.n = 3.0 + 1.5 * r.u();
        e.rSat = 0.05;
        // Designation from the half-month of perihelion ("C/2037 D2 (ATLAS)").
        const double dayOfYear = (e.tPeri - y0) / kDayS;
        int hm = std::clamp((int)(dayOfYear / 365.25 * 24.0), 0, 23);
        char letter = (char)('A' + hm + (hm >= 8 ? 1 : 0)); // no I
        const int num = 1 + (jfc ? nP++ : nC++);
        const char *who = kDiscoverers[(r.s >> 13) % (sizeof(kDiscoverers) / sizeof(kDiscoverers[0]))];
        std::snprintf(e.name, sizeof(e.name), "%s/%d %c%d (%s)", jfc ? "P" : "C", year, letter, num, who);
        finalize(e);
        out.push_back(e);
    }
}

void catalogNear(double t, double windowYears, std::vector<Elements> &out)
{
    out.clear();
    const Elements &g = greatComet();
    const double w = windowYears * 365.25 * kDayS;
    if (std::abs(t - g.tPeri) < w)
        out.push_back(g);
    const int y = yearOf(t);
    const int span = (int)std::ceil(windowYears) + 1;
    std::vector<Elements> yr;
    for (int Y = y - span; Y <= y + span; ++Y)
    {
        yr.clear();
        minorCometsOfYear(Y, yr);
        for (const Elements &e : yr)
            if (std::abs(t - e.tPeri) < w)
                out.push_back(e);
    }
}

double dustRate(const Elements &el, double r)
{
    const double s2 = el.rSat * el.rSat;
    return (1.0 + s2) / (r * r + s2);
}

void buildDustGrid(const Elements &el, double t, int nBeta, int nTau, double tauMaxDays, DustGrid &g)
{
    g.nBeta = nBeta;
    g.tBuilt = t;
    g.tPeri = el.tPeri;
    g.beta.resize(nBeta);
    const double bLo = std::log(0.004), bHi = std::log(0.95);
    for (int k = 0; k < nBeta; ++k)
        g.beta[k] = std::exp(bLo + (bHi - bLo) * k / (nBeta - 1));
    // The emission ages: log-spaced from the head (the young tail changes fastest) plus a dense run round the
    // perihelion's emission (a sungrazer makes most of its dust in a few hours there: one synchrone, which the log
    // grid alone put in one row, drawn as a jagged band).
    const double tau0 = 0.02;
    const double lr = std::log(tauMaxDays / tau0);
    std::vector<double> taus;
    taus.push_back(0.0);
    for (int j = 0; j < nTau - 1; ++j)
        taus.push_back(tau0 * std::exp(lr * j / (nTau - 2)));
    const double tauP = (t - el.tPeri) / kDayS;
    const double rq = el.q;
    if (rq < 0.3 && tauP > 0.05 && tauP < tauMaxDays)
    {
        const double half = std::clamp(0.6 + 4.0 * rq, 0.6, 1.8), step = half / 12.0;
        for (double x = -half; x <= half + 1e-9; x += step)
            if (tauP + x > tau0) taus.push_back(tauP + x);
    }
    std::sort(taus.begin(), taus.end());
    std::vector<double> u;
    for (double x : taus)
        if (u.empty() || x - u.back() > 0.004)
            u.push_back(x);
    g.tauDays = u;
    nTau = (int)u.size();
    g.nTau = nTau;
    g.relPos.assign((size_t)nBeta * nTau, glm::dvec3(0.0));
    g.rSun.assign((size_t)nBeta * nTau, 1.0);
    g.emitRate.assign(nTau, 0.0);
    const State now = stateAt(el, t);
    for (int j = 0; j < nTau; ++j)
    {
        const double tau = g.tauDays[j];
        // The row's emission: the dust produced over its share of the tau axis (midpoints to its neighbours), so a
        // perihelion burst shorter than the rows' spacing is neither lost nor aliased as time runs.
        const double a = j == 0 ? 0.0 : 0.5 * (g.tauDays[j - 1] + tau);
        const double b = j == nTau - 1 ? tau : 0.5 * (tau + g.tauDays[j + 1]);
        double sum = 0.0;
        const int nSub = 12;
        for (int s = 0; s < nSub; ++s)
        {
            const double ts = a + (b - a) * (s + 0.5) / nSub;
            const State e = stateAt(el, t - ts * kDayS);
            sum += dustRate(el, glm::length(e.r));
        }
        g.emitRate[j] = (b > a) ? sum / nSub : dustRate(el, glm::length(now.r));
        const State e0 = (tau > 0.0) ? stateAt(el, t - tau * kDayS) : now;
        for (int k = 0; k < nBeta; ++k)
        {
            State p = e0;
            if (tau > 0.0)
                propagate(e0, tau, kMuSun * (1.0 - g.beta[k]), p);
            g.relPos[(size_t)j * nBeta + k] = p.r - now.r;
            g.rSun[(size_t)j * nBeta + k] = glm::length(p.r);
        }
    }
}

glm::dvec3 ionAxis(const State &s)
{
    const double vWind = 400.0 * kDayS / kAuKm; // 400 km/s in AU/day
    const glm::dvec3 w = glm::normalize(s.r) * vWind - s.v;
    return glm::normalize(w);
}
} // namespace comet
