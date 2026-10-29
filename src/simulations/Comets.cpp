// Comets.cpp — comet orbits, brightness, catalog and tail geometry (2026-10-27; CLAUDE.md "Comets"; Comets.h).
#include "Comets.h"
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
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

double particleBuildStepS(double rAU)
{
    const double dynS = std::sqrt(rAU * rAU * rAU / kMuSun) * kDayS;
    const double s = std::clamp(0.003 * dynS, 4.0, 14400.0);
    return std::exp2(std::floor(std::log2(s)));
}

namespace
{
double smoothstepD(double a, double b, double x)
{
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
double u01(uint64_t h) { return (double)(h >> 11) * (1.0 / 9007199254740992.0); }
// A smooth value noise in one dimension (hashed knots, smoothstep blend), [0, 1).
double vnoise1(double x, uint64_t seed)
{
    const double fl = std::floor(x);
    const double f = x - fl;
    const int64_t i = (int64_t)fl;
    const double a = u01(mix64(seed ^ (uint64_t)i * 0x9E3779B97F4A7C15ull));
    const double b = u01(mix64(seed ^ (uint64_t)(i + 1) * 0x9E3779B97F4A7C15ull));
    const double w = f * f * (3.0 - 2.0 * f);
    return a + (b - a) * w;
}
glm::dvec3 accelAt(const glm::dvec3 &r, double mu)
{
    const double d = glm::length(r);
    return -mu * r / (d * d * d);
}
// The size distribution per ln(beta): cross-section ~ beta^0.5 (dN/da ~ a^-3.5), the sub-micron end (beta past ~0.5)
// scattering less.
double sizeWeight(double b) { return std::sqrt(b) * std::exp(-(b / 0.55) * (b / 0.55)); }
} // namespace

void buildParticles(const Elements &el, double tBuild, const glm::dvec3 &obsHelio, int slot, const ParticleOpts &o,
                    ParticleBuild &out)
{
    const auto c0 = std::chrono::steady_clock::now();
    out.tBuild = tBuild;
    out.grains.clear();
    out.nDust = out.nIon = 0;
    const State now = stateAt(el, tBuild);
    out.anchor = now.r;
    const double Fhead0 = std::pow(10.0, -0.4 * el.H);
    const double slotTag = 4.0 * (double)slot;

    // ── Dust slots: u = asinh((te - tPeri) / tau0), uniform steps du ────────────────────────────────────────────
    const double tau0S = o.tau0Days * kDayS;
    const double uNow = std::asinh((tBuild - el.tPeri) / tau0S);
    const double uOld = std::asinh((tBuild - o.tauMaxDays * kDayS - el.tPeri) / tau0S);
    const int64_t k0 = (int64_t)std::floor(uOld / o.du), k1 = (int64_t)std::floor(uNow / o.du);
    struct Slot
    {
        int64_t k;
        int n;
        size_t off;
        double tc, w0, w1; // centre time (s), the slot's time span (s)
        State s;           // the nucleus at tc
    };
    std::vector<Slot> slots;
    size_t total = 0;
    for (int64_t k = k0; k <= k1; ++k)
    {
        Slot s;
        s.k = k;
        const double uc = ((double)k + 0.5) * o.du;
        s.n = std::clamp((int)std::lround(o.grainsMin * std::pow(std::cosh(uc), 0.33)), o.grainsMin, o.grainsMax);
        s.off = total;
        s.tc = el.tPeri + tau0S * std::sinh(uc);
        s.w0 = el.tPeri + tau0S * std::sinh((double)k * o.du);
        s.w1 = el.tPeri + tau0S * std::sinh((double)(k + 1) * o.du);
        total += (size_t)s.n;
        slots.push_back(s);
    }
    if (total > (size_t)o.maxGrains) // (never at the shipped settings: drop the oldest slots)
    {
        size_t drop = 0, cut = 0;
        while (cut < slots.size() && total - drop > (size_t)o.maxGrains) drop += (size_t)slots[cut++].n;
        slots.erase(slots.begin(), slots.begin() + (ptrdiff_t)cut);
        total = 0;
        for (Slot &s : slots) { s.off = total; total += (size_t)s.n; }
    }
    // the size distribution's normalisation over [lnLo, lnHi]
    const double lnLo = std::log(0.002), lnHi = std::log(0.95);
    double wNorm = 0.0;
    for (int k = 0; k < 256; ++k) wNorm += sizeWeight(std::exp(lnLo + (lnHi - lnLo) * (k + 0.5) / 256.0)) * (lnHi - lnLo) / 256.0;
    const double Kd = o.dustFluxK * el.dust * Fhead0;
    const double v0 = o.v0KmS * kDayS / kAuKm; // AU/day
    const uint64_t seed = mix64((uint64_t)el.seed * 0x2545F4914F6CDD1Dull + 0xD057ull);

    std::vector<GpuGrain> dust(total);
    auto doSlots = [&](size_t a, size_t b) {
        for (size_t si = a; si < b; ++si)
        {
            Slot &S = slots[si];
            S.s = stateAt(el, S.tc);
            const glm::dvec3 aC = accelAt(S.s.r, kMuSun);
            // The local (emission time, size) field round the slot (the tail's zoom detail, comet_splat.frag): grains
            // without ejection velocity of kNB sizes, emitted at the slot's centre and dTe later; each grain takes the
            // field's derivatives at its own size, and the emission time / size of the field at its actual position
            // (its ejection offset decomposed on them), so a pixel of its Gaussian maps to the dust there.
            constexpr int kNB = 10;
            glm::dvec3 R0[kNB], Gt[kNB], Bl[kNB];
            bool fieldOk = o.detail;
            const double ageC = (tBuild - S.tc) / kDayS;
            const double dTe = std::max(0.25 * (S.w1 - S.w0) / kDayS, 0.001);
            if (fieldOk)
            {
                const State n1{S.s.r + S.s.v * dTe + 0.5 * aC * dTe * dTe, S.s.v + aC * dTe};
                for (int j = 0; j < kNB && fieldOk; ++j)
                {
                    const double bj = std::exp(lnLo + (lnHi - lnLo) * (double)j / (double)(kNB - 1));
                    State a, b;
                    fieldOk = propagate(S.s, ageC, kMuSun * (1.0 - bj), a) && propagate(n1, ageC - dTe, kMuSun * (1.0 - bj), b);
                    R0[j] = a.r;
                    Gt[j] = (b.r - a.r) / dTe;
                }
                const double dl = (lnHi - lnLo) / (double)(kNB - 1);
                for (int j = 0; j < kNB; ++j)
                {
                    const int ja = std::max(j - 1, 0), jb = std::min(j + 1, kNB - 1);
                    Bl[j] = (R0[jb] - R0[ja]) / ((double)(jb - ja) * dl);
                }
            }
            for (int i = 0; i < S.n; ++i)
            {
                GpuGrain &G = dust[S.off + (size_t)i];
                G = GpuGrain{};
                uint64_t h = mix64(seed ^ ((uint64_t)S.k * 0x632BE59BD9B4E019ull) ^ ((uint64_t)i * 0x9E6C63D0676A9A99ull));
                auto rnd = [&]() { h = mix64(h + 0x9E3779B97F4A7C15ull); return u01(h); };
                const double u = ((double)S.k + rnd()) * o.du;
                const double te = el.tPeri + tau0S * std::sinh(u);
                const double age = (tBuild - te) / kDayS;
                if (age < 0.0 || age > o.tauMaxDays) continue;
                // the nucleus at te: Taylor from the slot's centre (a slot is short where the orbit turns fast)
                const double dt = (te - S.tc) / kDayS;
                const glm::dvec3 rE = S.s.r + S.s.v * dt + 0.5 * aC * dt * dt;
                const glm::dvec3 vN = S.s.v + aC * dt;
                const double rE1 = glm::length(rE);
                // size: stratified in ln beta, its order shuffled by the slot
                const int iz = (int)((uint64_t)(i + (int)(mix64((uint64_t)S.k ^ seed) % (uint64_t)S.n)) % (uint64_t)S.n);
                const double beta = std::exp(lnLo + (lnHi - lnLo) * ((double)iz + rnd()) / (double)S.n);
                // ejection: ~80% into the sunward hemisphere
                const double cz = 2.0 * rnd() - 1.0, ph = 2.0 * 3.14159265358979 * rnd();
                const double sz = std::sqrt(std::max(0.0, 1.0 - cz * cz));
                glm::dvec3 dir(sz * std::cos(ph), sz * std::sin(ph), cz);
                const glm::dvec3 sunward = -rE / rE1;
                if (glm::dot(dir, sunward) < 0.0 && rnd() < 0.6) dir = glm::reflect(dir, sunward);
                const double vej = v0 * std::sqrt(beta) / std::sqrt(std::max(rE1, 0.08)) * (0.6 + 0.8 * rnd());
                const State g0{rE, vN + dir * vej};
                State g;
                if (!propagate(g0, age, kMuSun * (1.0 - beta), g)) continue;
                const double width = std::max(S.w1 - S.w0, 1.0) / kDayS;
                double w = Kd * dustRate(el, rE1) * width / (double)S.n * sizeWeight(beta) * (lnHi - lnLo) / wNorm;
                // soft ends: the youngest dust (the coma holds it), the oldest emission and the smallest grains (the
                // highest-beta syndyne and the oldest synchrone drew as hard edges where the sampled ranges stopped)
                w *= smoothstepD(0.0, 0.08, age) * (1.0 - smoothstepD(0.45 * o.tauMaxDays, o.tauMaxDays, age)) *
                     (1.0 - smoothstepD(0.45, 0.95, beta)); // (no fade at the large-grain end: those make the anti-tail)
                if (!(w > 0.0)) continue;
                const double dv = glm::length(g.v - now.v);
                const double sig = std::sqrt((0.35 * vej * age) * (0.35 * vej * age) + (0.5 * width * dv) * (0.5 * width * dv)) + 3e-6;
                G.p = glm::vec4(glm::vec3(g.r - now.r), (float)w);
                G.v = glm::vec4(glm::vec3(g.v), (float)sig);
                G.q = glm::vec4((float)(1.0 - beta), (float)(0.35 * vej), (float)age, (float)slotTag);
                if (fieldOk)
                {
                    const double lb = std::log(beta);
                    const double x = std::clamp((lb - lnLo) / (lnHi - lnLo) * (double)(kNB - 1), 0.0, (double)(kNB - 1) - 1e-9);
                    const int j = (int)x;
                    const double f = x - (double)j;
                    const glm::dvec3 Gv = Gt[j] + (Gt[j + 1] - Gt[j]) * f, Bv = Bl[j] + (Bl[j + 1] - Bl[j]) * f;
                    const double teC = (te - S.tc) / kDayS;
                    const glm::dvec3 Pf = R0[j] + (R0[j + 1] - R0[j]) * f + Gv * teC;
                    // the grain's offset from the field = a G + b B + (out of the sheet): a, b from the 2x2 normal equations
                    const glm::dvec3 e = g.r - Pf;
                    const double gg = glm::dot(Gv, Gv), bb = glm::dot(Bv, Bv), gb = glm::dot(Gv, Bv);
                    const double det = gg * bb - gb * gb;
                    double da = 0.0;
                    if (det > 1e-6 * gg * bb)
                    {
                        const double eg = glm::dot(e, Gv), eb = glm::dot(e, Bv);
                        da = std::clamp((eg * bb - eb * gb) / det, -5.0, 5.0);
                    }
                    const float teF = (float)((te - el.tPeri) / kDayS + da);
                    uint32_t teBits;
                    std::memcpy(&teBits, &teF, 4);
                    G.d = glm::uvec4(glm::packHalf2x16(glm::vec2((float)Gv.x, (float)Gv.y)),
                                     glm::packHalf2x16(glm::vec2((float)Gv.z, (float)Bv.x)),
                                     glm::packHalf2x16(glm::vec2((float)Bv.y, (float)Bv.z)), teBits);
                    // its spread also covers the stretch of syndyne its size stratum stands for (ln beta / n of the
                    // range): young dust's grains of one slot otherwise sat as separate dots along the syndyne (the
                    // perihelion swing's speckle, issue 20261028_221813). Young dust only: older dust's ejection spread
                    // covers it, and widening it there blurred the tail's streaks and cost fill.
                    const double sB = 0.6 * glm::length(Bv) * (lnHi - lnLo) / (double)S.n * (1.0 - smoothstepD(2.0, 6.0, age));
                    G.v.w = (float)std::sqrt((double)G.v.w * G.v.w + sB * sB);
                }
            }
        }
    };
    {
        const int nt = std::max(1, std::min(o.threads, (int)slots.size()));
        std::vector<std::thread> th;
        // interleave slot chunks so the cost (more grains in the wide slots) spreads
        std::atomic<size_t> next{0};
        for (int k = 0; k < nt; ++k)
            th.emplace_back([&]() {
                for (;;)
                {
                    const size_t a = next.fetch_add(16);
                    if (a >= slots.size()) break;
                    doSlots(a, std::min(a + 16, slots.size()));
                }
            });
        for (std::thread &t : th) t.join();
    }
    for (const GpuGrain &g : dust)
        if (g.p.w > 0.0f) out.grains.push_back(g);
    out.nDust = (uint32_t)out.grains.size();

    // ── Ion parcels: an absolute grid of pick-up times, carried radially by the solar wind ─────────────────────
    if (o.ionPerSlot > 0 && el.gas > 0.0)
    {
        // The pick-up grid: every whole w = t / ionDtDays + nu / ionDnuRad (nu the true anomaly, unwrapped by the orbit
        // count), so a parcel leaves at least every ionDtDays and every ionDnuRad of the radial direction's swing: the
        // tail stays continuous through a sungrazer's perihelion (the radial turns ~0.5 rad in 0.01 day there, and on a
        // time grid alone the streamers broke into separate puffs). A pure function of the comet's orbit and the indices.
        const double dnu = o.ionDnuRad > 0.0 ? o.ionDnuRad : 1e30;
        const double aOrb = el.e < 1.0 ? el.q / (1.0 - el.e) : 0.0;
        const double Porb = el.e < 1.0 ? 2.0 * 3.14159265358979 * std::sqrt(aOrb * aOrb * aOrb / kMuSun) : 0.0; // days
        auto wOf = [&](double t, State &st, double &dwdt) { // dwdt per day
            st = stateAt(el, t);
            double nu = std::atan2(glm::dot(st.r, el.tHat), glm::dot(st.r, el.pHat));
            if (Porb > 0.0) nu += 2.0 * 3.14159265358979 * std::floor((t - el.tPeri) / kDayS / Porb + 0.5);
            const double r2 = glm::dot(st.r, st.r);
            const double om = glm::length(glm::cross(st.r, st.v)) / r2; // rad/day
            dwdt = 1.0 / o.ionDtDays + om / dnu;
            return (t / kDayS) / o.ionDtDays + nu / dnu;
        };
        const double tLo = tBuild - o.ionMaxAgeDays * kDayS;
        State stLo, stHi;
        double dwLo, dwHi;
        const double wLo = wOf(tLo, stLo, dwLo), wHi = wOf(tBuild, stHi, dwHi);
        const int64_t j0 = (int64_t)std::ceil(wLo), j1 = (int64_t)std::floor(wHi);
        const double vInf = 250.0 * kDayS / kAuKm, tauA = 0.15, tauC = 0.03, tauIon = 1.0;
        const int S = 5;
        const uint64_t iseed = mix64(seed ^ 0x10A5ull);
        double tPrev = tLo, wPrev = wLo, dwPrev = dwLo;
        for (int64_t j = j0; j <= j1 && j - j0 < 20000; ++j)
        {
            // the time where w = j: Newton from the last slot's, kept inside [tPrev, tBuild]
            double te = std::min(tPrev + ((double)j - wPrev) / dwPrev * kDayS, tBuild), dwdt = dwPrev;
            double lo = tPrev, hi = tBuild;
            State n;
            for (int it = 0; it < 12; ++it)
            {
                const double wv = wOf(te, n, dwdt);
                if (wv < (double)j) lo = te; else hi = te;
                const double step = ((double)j - wv) / dwdt * kDayS;
                double tn = te + step;
                if (!(tn > lo && tn < hi)) tn = 0.5 * (lo + hi);
                if (std::abs(tn - te) < 0.05) { te = tn; break; }
                te = tn;
            }
            n = stateAt(el, te);
            const double om = glm::length(glm::cross(n.r, n.v)) / glm::dot(n.r, n.r);
            dwdt = 1.0 / o.ionDtDays + om / dnu;
            tPrev = te; wPrev = (double)j; dwPrev = dwdt;
            const double dtSlot = 1.0 / dwdt; // days this pick-up stands for
            const double a = (tBuild - te) / kDayS;
            if (a < 0.0) continue;
            const double rN = glm::length(n.r);
            const glm::dvec3 rh = n.r / rN;
            glm::dvec3 e1 = glm::cross(rh, n.v);
            e1 = glm::length(e1) > 1e-12 ? glm::normalize(e1) : glm::dvec3(0, 0, 1);
            const glm::dvec3 e2 = glm::cross(rh, e1);
            // The ions picked up per day follow the gas production, which is the head's law less the 1/r^2 of sunlight
            // (Q ~ r_eff^-(n - 2)); each fluoresces by the sunlight where it is now (comet_splat.vert, / r^2 saturated
            // as the head's law), so at pick-up it is the head's r_eff^-n.
            const double rEff = std::sqrt(rN * rN + el.rSat * el.rSat);
            const double Fion = o.ionFluxK * el.gas * Fhead0 * std::pow(rEff, -(el.n - 2.0)) / (1.0 + el.rSat * el.rSat);
            const double s = vInf * (a - tauA * (1.0 - std::exp(-a / tauA)));
            const double sd = vInf * (1.0 - std::exp(-a / tauA));
            const double tD = te / kDayS;
            const double fan = 6.2831853 * vnoise1(tD / 2.0, iseed); // the streamers' plane turns slowly
            for (int p = 0; p < o.ionPerSlot; ++p)
            {
                uint64_t h = mix64(iseed ^ ((uint64_t)j * 0x632BE59BD9B4E019ull) ^ ((uint64_t)p * 0x9E6C63D0676A9A99ull));
                auto rnd = [&]() { h = mix64(h + 0x9E3779B97F4A7C15ull); return u01(h); };
                const int st = p % S;
                // streamer angle from the radial: a fan of S rays, each wandering slowly; most light in the middle
                const double th0 = ((double)st - 0.5 * (S - 1)) / (double)S * 0.16 *
                                   (0.6 + 0.8 * vnoise1(tD / 0.7 + 11.0 * st, iseed ^ 0x51ull));
                const double kink = 0.035 * std::sin(6.2831853 * (tD / 0.35 + 0.37 * st) + 3.0 * vnoise1(tD / 1.3, iseed ^ 0x77ull));
                const double th = th0 + kink + 0.012 * (rnd() - 0.5);
                const double phi = fan + 0.25 * (rnd() - 0.5);
                const glm::dvec3 dirI = glm::normalize(rh + std::tan(th) * (std::cos(phi) * e1 + std::sin(phi) * e2));
                const glm::dvec3 P = n.r + n.v * tauC * (1.0 - std::exp(-a / tauC)) + dirI * s;
                const glm::dvec3 V = n.v * std::exp(-a / tauC) + dirI * sd;
                const double mid = 1.0 - 0.5 * std::abs((double)st - 0.5 * (S - 1)) / (0.5 * (S - 1) + 1e-9);
                double w = Fion * (dtSlot / tauIon) * std::exp(-a / tauIon) / (double)o.ionPerSlot * (0.6 + 0.8 * mid);
                w *= smoothstepD(0.0, 0.02, a) * (1.0 - smoothstepD(0.8 * o.ionMaxAgeDays, o.ionMaxAgeDays, a));
                if (!(w > 0.0)) continue;
                GpuGrain G;
                // its spread: the streamer's width + the stretch of tail one pick-up stands for (across: the radial's
                // swing over it; along: the wind's speed over it), so neighbouring pick-ups overlap
                const double omS = om * dtSlot;
                const double sig = std::sqrt((2e-4 + 0.025 * s) * (2e-4 + 0.025 * s) + (0.45 * s * omS) * (0.45 * s * omS) +
                                             (0.45 * sd * dtSlot) * (0.45 * sd * dtSlot));
                G.p = glm::vec4(glm::vec3(P - now.r), (float)w);
                G.v = glm::vec4(glm::vec3(V), (float)sig);
                G.q = glm::vec4(0.0f, (float)(0.025 * sd + 0.45 * sd * omS), (float)a, (float)(slotTag + 1.0));
                G.d = glm::uvec4(0u);
                out.grains.push_back(G);
                out.nIon++;
            }
        }
    }

    // ── Framing: the tail's visible span from the head, from the observer ──────────────────────────────────────
    {
        const glm::dvec3 hd = glm::normalize(now.r - obsHelio);
        std::vector<std::pair<double, double>> af; // (angle, flux)
        af.reserve(out.nDust);
        double tot = 0.0;
        for (uint32_t k = 0; k < out.nDust; ++k)
        {
            const GpuGrain &g = out.grains[k];
            const glm::dvec3 P = now.r + glm::dvec3(g.p);
            const glm::dvec3 D = P - obsHelio;
            const double d = glm::length(D), rp = glm::length(P);
            const double sc = std::acos(std::clamp(glm::dot(P / rp, -D / d), -1.0, 1.0)) * 180.0 / 3.14159265358979;
            const double f = (double)g.p.w * dustPhase(sc) / (rp * rp * d * d);
            const double ang = std::acos(std::clamp(glm::dot(D / d, hd), -1.0, 1.0));
            af.push_back({ang, f});
            tot += f;
        }
        if (tot > 0.0)
        {
            std::vector<std::pair<double, double>> s = af;
            std::sort(s.begin(), s.end());
            double c = 0.0;
            out.spanRad = s.back().first;
            for (const auto &x : s)
            {
                c += x.second;
                if (c > 0.92 * tot) { out.spanRad = x.first; break; }
            }
            glm::dvec3 acc(0.0);
            for (uint32_t k = 0; k < out.nDust; ++k)
                if (af[k].first > 0.55 * out.spanRad && af[k].first <= out.spanRad)
                    acc += af[k].second * glm::normalize(now.r + glm::dvec3(out.grains[k].p) - obsHelio);
            out.aimEcl = glm::length(acc) > 0.0 ? glm::normalize(acc) : hd;
        }
        else
            out.aimEcl = hd;
    }
    out.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
}

glm::dvec3 ionAxis(const State &s)
{
    const double vWind = 400.0 * kDayS / kAuKm; // 400 km/s in AU/day
    const glm::dvec3 w = glm::normalize(s.r) * vWind - s.v;
    return glm::normalize(w);
}
} // namespace comet
