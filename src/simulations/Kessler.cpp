// Kessler syndrome core (see Kessler.h).
#include "Kessler.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <execution>
#include <limits>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "SpaceWeather.h"

namespace kessler
{
// ── Elements ──────────────────────────────────────────────────────────────────────────────────────
static double wrap2Pi(double x)
{
    x = std::fmod(x, kTwoPi);
    return x < 0.0 ? x + kTwoPi : x;
}

Elems elemsFromState(const State &s)
{
    Elems el;
    const glm::dvec3 r = s.r, v = s.v;
    const double rm = glm::length(r), v2 = glm::dot(v, v);
    const glm::dvec3 h = glm::cross(r, v);
    const double hm = glm::length(h);
    const glm::dvec3 nVec(-h.y, h.x, 0.0); // node vector z × h
    const double nm = glm::length(nVec);
    const glm::dvec3 eVec = ((v2 - kGM / rm) * r - glm::dot(r, v) * v) / kGM;
    el.e = glm::length(eVec);
    const double energy = 0.5 * v2 - kGM / rm;
    el.a = -kGM / (2.0 * energy);
    el.i = std::acos(std::clamp(h.z / hm, -1.0, 1.0));
    el.raan = nm > 1e-9 ? wrap2Pi(std::atan2(nVec.y, nVec.x)) : 0.0;
    // Argument of latitude u (node -> position) is well defined even for circular orbits.
    const glm::dvec3 nHat = nm > 1e-9 ? nVec / nm : glm::dvec3(1.0, 0.0, 0.0);
    const glm::dvec3 q = glm::cross(h / hm, nHat); // in-plane, 90 deg ahead of the node
    const double u = std::atan2(glm::dot(r, q), glm::dot(r, nHat));
    double nu; // true anomaly
    if (el.e > 1e-9)
    {
        el.argp = wrap2Pi(std::atan2(glm::dot(eVec, q), glm::dot(eVec, nHat)));
        nu = wrap2Pi(u - el.argp);
    }
    else
    {
        el.argp = 0.0;
        nu = wrap2Pi(u);
    }
    // True -> eccentric -> mean anomaly.
    const double E = 2.0 * std::atan2(std::sqrt(std::max(0.0, 1.0 - el.e)) * std::sin(nu * 0.5),
                                      std::sqrt(1.0 + el.e) * std::cos(nu * 0.5));
    el.M = wrap2Pi(E - el.e * std::sin(E));
    return el;
}

State stateFromElems(const Elems &el)
{
    double E = el.M + el.e * std::sin(el.M);
    for (int k = 0; k < 12; ++k)
        E -= (E - el.e * std::sin(E) - el.M) / (1.0 - el.e * std::cos(E));
    const double cE = std::cos(E), sE = std::sin(E), sq = std::sqrt(std::max(0.0, 1.0 - el.e * el.e));
    const double x = el.a * (cE - el.e), y = el.a * sq * sE;
    const double n = std::sqrt(kGM / (el.a * el.a * el.a));
    const double rr = el.a * (1.0 - el.e * cE);
    const double vx = -el.a * n * sE / (1.0 - el.e * cE) * 1.0, vy = el.a * n * sq * cE / (1.0 - el.e * cE);
    (void)rr;
    const double cO = std::cos(el.raan), sO = std::sin(el.raan), cw = std::cos(el.argp), sw = std::sin(el.argp);
    const double ci = std::cos(el.i), si = std::sin(el.i);
    const glm::dvec3 P(cw * cO - sw * sO * ci, cw * sO + sw * cO * ci, sw * si);
    const glm::dvec3 Q(-sw * cO - cw * sO * ci, -sw * sO + cw * cO * ci, cw * si);
    return {x * P + y * Q, vx * P + vy * Q};
}

Secular secularJ2(double a, double e, double incl)
{
    Secular s;
    const double n0 = std::sqrt(kGM / (a * a * a));
    const double p = a * (1.0 - e * e);
    const double k = kJ2 * (kReq / p) * (kReq / p);
    const double ci = std::cos(incl);
    s.raanDot = -1.5 * n0 * k * ci;
    s.argpDot = 0.75 * n0 * k * (5.0 * ci * ci - 1.0);
    s.Mdot = n0 * (1.0 + 0.75 * k * std::sqrt(std::max(0.0, 1.0 - e * e)) * (3.0 * ci * ci - 1.0));
    return s;
}

// ── Hashing ───────────────────────────────────────────────────────────────────────────────────────
static uint64_t mix64(uint64_t h)
{
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

double hash01(uint64_t seed, uint64_t index, uint32_t channel)
{
    uint64_t h = mix64(seed * 0x9E3779B97F4A7C15ull ^ mix64(index + 0x632BE59BD9B4E019ull) ^ ((uint64_t)channel << 48));
    return (double)(h >> 11) * (1.0 / 9007199254740992.0);
}

double hashNormal(uint64_t seed, uint64_t index, uint32_t channel)
{
    const double u1 = std::max(hash01(seed, index, channel * 2u + 0x100u), 1e-300);
    const double u2 = hash01(seed, index, channel * 2u + 0x101u);
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(kTwoPi * u2);
}

// ── NASA Standard Breakup Model ───────────────────────────────────────────────────────────────────
// Area-to-mass of a spacecraft fragment (Lc > 11 cm): a bimodal normal in chi = log10(A/m).
static double sampleChi(double Lc, uint64_t seed, uint64_t idx)
{
    const double l = std::log10(Lc);
    const double alpha = l <= -1.95 ? 0.0 : (l >= 0.55 ? 1.0 : 0.3 + 0.4 * (l + 1.2));
    const double mu1 = l <= -1.1 ? -0.6 : (l >= 0.0 ? -0.95 : -0.6 - 0.318 * (l + 1.1));
    const double s1 = l <= -1.3 ? 0.1 : (l >= -0.3 ? 0.3 : 0.1 + 0.2 * (l + 1.3));
    const double mu2 = l <= -0.7 ? -1.2 : (l >= -0.1 ? -2.0 : -1.2 - 1.333 * (l + 0.7));
    const double s2 = l <= -0.5 ? 0.5 : (l >= -0.3 ? 0.3 : 0.5 - (l + 0.5));
    const bool first = hash01(seed, idx, 3) < std::clamp(alpha, 0.0, 1.0);
    const double z = hashNormal(seed, idx, 4);
    return first ? mu1 + s1 * z : mu2 + s2 * z;
}

// One fragment of size Lc: its area, mass (from the bimodal A/m) and ejection velocity.
static Fragment makeFragment(double Lc, bool collision, double sideBShare, uint64_t seed, uint64_t k)
{
    Fragment f;
    f.Lc = Lc;
    const double chi = sampleChi(f.Lc, seed, k);
    const double am = std::pow(10.0, chi);
    f.areaM2 = 0.556945 * std::pow(f.Lc, 2.0047077);
    f.massKg = f.areaM2 / am;
    const double nu = (collision ? 0.9 * chi + 2.9 : 0.2 * chi + 1.85) + 0.4 * hashNormal(seed, k, 5);
    const double dvm = std::pow(10.0, nu);
    const double z = 2.0 * hash01(seed, k, 6) - 1.0;
    const double ph = kTwoPi * hash01(seed, k, 7);
    const double rxy = std::sqrt(std::max(0.0, 1.0 - z * z));
    f.dv = dvm * glm::dvec3(rxy * std::cos(ph), rxy * std::sin(ph), z);
    f.side = hash01(seed, k, 8) < sideBShare ? 1 : 0;
    return f;
}

// The size distribution N(>Lc) ~ Lc^-b, truncated to [LcMin, LcMax], cut at LcRecord: the large pieces as
// records (weight 1 up to maxLarge, and never more mass than maxLargeMass), the small ones as nSmallSamples
// statistical samples carrying the rest.
static void fillFragments(BreakupResult &res, double nReal, const BreakupSizes &sz, bool collision, double sideBShare,
                          uint64_t seed)
{
    res.nReal = nReal;
    if (nReal <= 0.0)
        return;
    const double b = collision ? 1.71 : 1.6;
    const double LcMax = std::max(sz.LcMax, sz.LcMin * 1.5);
    const double LcRec = std::min(sz.LcRecord, LcMax);
    // Complementary CDF of the truncated power law.
    auto ccdf = [&](double L) {
        return (std::pow(L, -b) - std::pow(LcMax, -b)) / (std::pow(sz.LcMin, -b) - std::pow(LcMax, -b));
    };
    auto sampleIn = [&](double lo, double hi, double u) { // inverse CDF on [lo, hi]
        const double a = std::pow(lo, -b), c = std::pow(hi, -b);
        return std::pow(a - u * (a - c), -1.0 / b);
    };
    res.nLarge = LcRec < LcMax ? nReal * ccdf(LcRec) : 0.0;
    // Large: real pieces.
    if (res.nLarge > 0.0 && sz.maxLarge > 0)
    {
        int K = (int)std::floor(res.nLarge);
        if (hash01(seed, 0xFFFFFFFFull, 9) < res.nLarge - (double)K)
            ++K;
        double w = 1.0;
        if (K > sz.maxLarge)
        {
            w = res.nLarge / (double)sz.maxLarge;
            K = sz.maxLarge;
        }
        double mass = 0.0;
        for (int k = 0; k < K; ++k)
        {
            Fragment f = makeFragment(sampleIn(LcRec, LcMax, hash01(seed, (uint64_t)k, 1)), collision, sideBShare, seed,
                                      (uint64_t)k);
            if (mass + f.massKg * w > sz.maxLargeMass)
                continue; // the parent has no more mass to give
            mass += f.massKg * w;
            f.weight = w;
            res.fragments.push_back(f);
        }
    }
    // Small: statistical samples.
    const double nSmall = nReal - res.nLarge;
    if (nSmall > 0.0 && sz.nSmallSamples > 0)
    {
        const int S = std::clamp((int)std::ceil(nSmall / 50.0), std::min(4, sz.nSmallSamples), sz.nSmallSamples); // a few for a handful of fragments
        for (int k = 0; k < S; ++k)
        {
            // Stratified in the size distribution so 16 samples cover it.
            const double u = ((double)k + hash01(seed, (uint64_t)k + 1000, 1)) / (double)S;
            Fragment f = makeFragment(sampleIn(sz.LcMin, LcRec, u), collision, sideBShare, seed, (uint64_t)k + 1000);
            f.weight = nSmall / (double)S;
            res.small.push_back(f);
        }
    }
}

BreakupResult breakupCollision(double mT, double mP, double vRel, const BreakupSizes &sz, uint64_t seed)
{
    BreakupResult res;
    if (mP > mT)
        std::swap(mT, mP);
    const double emr = 0.5 * mP * vRel * vRel / std::max(mT, 1e-3); // J/kg
    res.catastrophic = emr >= 40000.0;
    const double M = res.catastrophic ? (mT + mP) : mP * (vRel * 1e-3) * (vRel * 1e-3);
    res.massKg = res.catastrophic ? M : std::min(M, mT);
    const double nReal = 0.1 * std::pow(M, 0.75) * std::pow(sz.LcMin, -1.71);
    BreakupSizes s = sz;
    if (!res.catastrophic)
        s.maxLargeMass = std::min(s.maxLargeMass, res.massKg); // cratering: only the ejected mass
    fillFragments(res, nReal, s, true, res.catastrophic ? mP / (mT + mP) : 0.0, seed);
    return res;
}

BreakupResult breakupExplosion(double m, const BreakupSizes &sz, uint64_t seed)
{
    BreakupResult res;
    res.catastrophic = true;
    res.massKg = m;
    // The NASA SBM's explosion law is for an upper stage of a few tonnes (scale factor S = 1). A much heavier parent
    // (a 420-t station) breaks into far more pieces: S = (m / 2 t)^0.75, the collision law's mass exponent, never
    // below 1 (2026-10-20: the ISS exploded into 239 pieces over 10 cm, 7 of them over 1 m; now ~13,000 and ~330).
    const double S = std::max(1.0, std::pow(m / 2000.0, 0.75));
    const double nReal = 6.0 * S * std::pow(sz.LcMin, -1.6);
    fillFragments(res, nReal, sz, false, 0.0, seed);
    return res;
}

// ── Drag ──────────────────────────────────────────────────────────────────────────────────────────
// Exponential atmosphere, moderate solar activity (Vallado, Fundamentals of Astrodynamics, table 8-4).
static const double kAtmo[][3] = {
    {0, 1.225, 7.249},          {25, 3.899e-2, 6.349},     {30, 1.774e-2, 6.682},     {40, 3.972e-3, 7.554},
    {50, 1.057e-3, 8.382},      {60, 3.206e-4, 7.714},     {70, 8.770e-5, 6.549},     {80, 1.905e-5, 5.799},
    {90, 3.396e-6, 5.382},      {100, 5.297e-7, 5.877},    {110, 9.661e-8, 7.263},    {120, 2.438e-8, 9.473},
    {130, 8.484e-9, 12.636},    {140, 3.845e-9, 16.149},   {150, 2.070e-9, 22.523},   {180, 5.464e-10, 29.740},
    {200, 2.789e-10, 37.105},   {250, 7.248e-11, 45.546},  {300, 2.418e-11, 53.628},  {350, 9.518e-12, 53.298},
    {400, 3.725e-12, 58.515},   {450, 1.585e-12, 60.828},  {500, 6.967e-13, 63.822},  {600, 1.454e-13, 71.835},
    {700, 3.614e-14, 88.667},   {800, 1.170e-14, 124.64},  {900, 5.245e-15, 181.05},  {1000, 3.019e-15, 268.00}};

double atmosDensity(double hM)
{
    const double h = std::max(0.0, hM * 1e-3);
    const int n = (int)(sizeof(kAtmo) / sizeof(kAtmo[0]));
    int k = 0;
    while (k + 1 < n && h >= kAtmo[k + 1][0])
        ++k;
    return kAtmo[k][1] * std::exp(-(h - kAtmo[k][0]) / kAtmo[k][2]);
}

double decayTime(double tStart, double a, double e, double cdAm, double maxS,
                 const std::function<double(double)> &densityFactor)
{
    const double aEnd = kRMean + 120000.0;
    if (a * (1.0 - e) <= aEnd)
        return 0.0;
    const double e0 = e, a0 = a;
    double t = 0.0;
    for (int it = 0; it < 20000 && t < maxS; ++it)
    {
        // Eccentric orbits lose their apogee first: e shrinks with a, the drag acts near perigee.
        e = e0 * std::clamp((a - aEnd) / std::max(a0 - aEnd, 1.0), 0.0, 1.0);
        const double hp = a * (1.0 - e) - kRMean;
        const double f = densityFactor ? densityFactor(tStart + t) : 1.0;
        const double rho = atmosDensity(hp) * f * (e > 1e-4 ? std::sqrt(1.0 / std::max(e * 50.0, 1.0)) : 1.0);
        const double adot = -rho * cdAm * std::sqrt(kGM * a); // m/s
        if (adot > -1e-12)
        {
            if (f <= 1.0001)
                return maxS * 2.0; // no measurable decay and no storm ahead: outlives the horizon
        }
        double dt = std::clamp(0.01 * (a - aEnd) / std::max(-adot, 1e-15), 600.0, 30.0 * 86400.0);
        if (f > 1.0001)
            dt = std::min(dt, 3600.0); // inside a storm: hour steps
        // Do not step over a storm starting ahead (the factor at the step's end).
        if (densityFactor && densityFactor(tStart + t + dt) > 1.0001 && f <= 1.0001)
            dt = std::min(dt, 3600.0);
        const double da = adot * dt;
        if (a + da <= aEnd)
            return t + (a - aEnd) / std::max(-adot, 1e-15);
        a += da;
        t += dt;
    }
    return t >= maxS ? maxS * 2.0 : t;
}

// ── Engine ────────────────────────────────────────────────────────────────────────────────────────
std::vector<EngineCollision> engineSample(const std::vector<EngineObject> &objs, double dt, const EngineParams &p,
                                          uint64_t seed)
{
    std::vector<EngineCollision> out;
    if (objs.size() < 2)
        return out;
    struct Key
    {
        int64_t k;
        uint32_t i;
    };
    std::vector<Key> keys(objs.size());
    const double inv = 1.0 / p.cubeM;
    for (size_t i = 0; i < objs.size(); ++i)
    {
        const glm::dvec3 &r = objs[i].r;
        const int64_t x = (int64_t)std::floor(r.x * inv) & 0x1FFFFF, y = (int64_t)std::floor(r.y * inv) & 0x1FFFFF,
                      z = (int64_t)std::floor(r.z * inv) & 0x1FFFFF;
        keys[i] = {(x << 42) | (y << 21) | z, (uint32_t)i};
    }
    // Keys are unique (k, i): the order is the same however the sort runs, so it may run in parallel.
    std::sort(std::execution::par, keys.begin(), keys.end(),
              [](const Key &a, const Key &b) { return a.k != b.k ? a.k < b.k : a.i < b.i; });
    const double V = p.cubeM * p.cubeM * p.cubeM;
    for (size_t s = 0; s < keys.size();)
    {
        size_t e = s + 1;
        while (e < keys.size() && keys[e].k == keys[s].k)
            ++e;
        if (e - s >= 2)
        {
            bool anyPassive = false;
            for (size_t q = s; q < e && !anyPassive; ++q)
                anyPassive = !objs[keys[q].i].active;
            if (anyPassive)
                for (size_t q1 = s; q1 < e; ++q1)
                    for (size_t q2 = q1 + 1; q2 < e; ++q2)
                    {
                        const EngineObject &A = objs[keys[q1].i], &B = objs[keys[q2].i];
                        if (A.active && B.active)
                            continue;
                        const double vRel = glm::length(A.v - B.v);
                        const double rr = (double)A.radiusM + (double)B.radiusM;
                        double P = vRel * kPi * rr * rr * dt / V * (double)A.weight * (double)B.weight * p.rateMultiplier;
                        if (A.active || B.active)
                            P *= (1.0 - p.avoidance);
                        if (P <= 0.0)
                            continue;
                        const uint32_t lo = std::min(A.id, B.id), hi = std::max(A.id, B.id);
                        const uint64_t pairKey = ((uint64_t)lo << 32) | hi;
                        if (hash01(seed, pairKey, 11) < std::min(P, 1.0))
                            out.push_back({A.id, B.id, vRel});
                    }
        }
        s = e;
    }
    return out;
}
} // namespace kessler

// ══ The world ═════════════════════════════════════════════════════════════════════════════════════
namespace kessler
{
static uint32_t hashU32(uint32_t x) // sat_orbit.comp hashU
{
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

glm::dvec3 poolTumbleAxis(uint32_t j)
{
    const uint32_t h1 = hashU32(j * 0x9E3779B9u + 0x7F4A7C15u);
    const uint32_t h2 = hashU32(h1 ^ 0x85EBCA6Bu);
    const double z = 1.0 - 2.0 * ((double)(float)h1 * (1.0 / 4294967296.0));
    const double ph = kTwoPi * ((double)(float)h2 * (1.0 / 4294967296.0));
    const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
    return {r * std::cos(ph), r * std::sin(ph), z};
}

SatOrbitElems rosterOrbitElems(const RosterObj &o, double raanAnchorT)
{
    SatOrbitElems e;
    e.raan = o.raan;
    e.incl = o.incl;
    e.u0 = o.u0;
    e.rSatM = o.R;
    e.meanMot = o.n;
    e.sso = o.sso;
    e.raanAnchorT = raanAnchorT;
    e.formation = o.formation;
    e.formP = o.formP;
    e.formQ = o.formQ;
    e.formK = o.formK;
    return e;
}

SatOrbitElems poolOrbitElems(const PoolRec &r, uint32_t j)
{
    SatOrbitElems e;
    e.kepler = true;
    e.u0 = r.M0;
    e.raan = r.raan0;
    e.argp = r.argp0;
    e.meanMot = r.n;
    e.argpDot = r.argpDot;
    e.raanDot = r.raanDot;
    e.rSatM = r.a;
    e.ecc = r.e;
    e.incl = r.incl;
    e.tumbleRate = r.tumbleRate;
    e.tumblePhase = r.tumble0;
    e.tumbleAxis = poolTumbleAxis(j);
    e.forceTumble = (r.flags & kRecDead) != 0;
    e.areaScale = r.flux;
    return e;
}

// The engine's threads run below normal priority: the frame loop wins any contention.
void lowerThreadPriority()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

static void parallelChunks(size_t n, const std::function<void(size_t, size_t)> &fn)
{
    // Half the cores: the cascade runs beside the app on its worker thread.
    const size_t hw = std::max<size_t>(1, std::min<size_t>(std::thread::hardware_concurrency() / 2 - 1, 12));
    if (n < 20000 || hw == 1)
    {
        fn(0, n);
        return;
    }
    std::vector<std::thread> th;
    const size_t chunk = (n + hw - 1) / hw;
    for (size_t k = 0; k < hw; ++k)
    {
        const size_t a = k * chunk, b = std::min(n, a + chunk);
        if (a < b)
        {
            th.emplace_back([&fn, a, b] {
                lowerThreadPriority();
                fn(a, b);
            });
        }
    }
    for (std::thread &t : th)
        t.join();
}

void deadTumble(uint32_t sat, uint32_t &periodS, double &phase0)
{
    const uint32_t h = hashU32(sat * 0x2C1B3C6Du + 0x297A2D39u);
    periodS = 40u + (h % 560u);                                    // 40 s .. 10 min a turn
    phase0 = kTwoPi * (double)(float)((hashU32(h) >> 8) * (1.0 / 16777216.0));
}

double formationDriftTurns(uint32_t sat, uint32_t tauSec, double frac)
{
    // sat_orbit.comp formationDrift: the same hash, the same integer arithmetic.
    const uint32_t h = hashU32(sat * 0x51ED27F1u + 0x3C6EF372u);
    const uint64_t j = 1u + (h % 3u);
    const double sign = (h & 0x100u) ? -1.0 : 1.0;
    const uint64_t t2 = (uint64_t)tauSec * (uint64_t)tauSec;
    const uint64_t m = (t2 * j) & ((1ull << 48) - 1ull);
    double turns = (double)m * (1.0 / 281474976710656.0);
    turns += (double)j * (2.0 * (double)tauSec * frac + frac * frac) * (1.0 / 281474976710656.0);
    return sign * turns;
}

// ── The small fragments' density grid ─────────────────────────────────────────────────────────────
void DebrisGrid::clear()
{
    cnt.assign((size_t)NR * NL * NH, 0);
    massG.assign((size_t)NR * NL, 0);
}

// The heading bin (8 x 45 deg, clockwise from north) of a horizontal direction (east, north): octant tests, no atan2.
static int headingBin(double e, double nn)
{
    const double ae = std::abs(e), an = std::abs(nn);
    if (e >= 0.0)
        return nn >= 0.0 ? (ae < an ? 0 : 1) : (ae > an ? 2 : 3);
    return nn < 0.0 ? (ae < an ? 4 : 5) : (ae > an ? 6 : 7);
}

void DebrisGrid::depositInto(std::vector<int64_t> &c, std::vector<int64_t> &m, double a, double e, double incl,
                             double argp, double weight, double massKg, uint64_t seed, int sign) const
{
    if (c.empty())
    {
        c.assign((size_t)NR * NL * NH, 0);
        m.assign((size_t)NR * NL, 0);
    }
    // sign -1 takes exactly the same integers back out (the rollback undoes a deposit this way).
    const int64_t q = (int64_t)std::llround(weight * kFix / NS) * sign;
    const int64_t mq = (int64_t)std::llround(weight * massKg * 1000.0 * kFix / NS) * sign;
    if (q == 0)
        return;
    const double ci = std::cos(incl), si = std::sin(incl);
    const double ca = std::cos(argp), sa = std::sin(argp);
    // Argument of latitude u uniform in time (near-circular): NS samples by a rotation recurrence (no trig per sample).
    const double du = kTwoPi / NS, u0 = du * hash01(seed, 0, 77);
    double cu = std::cos(u0), su = std::sin(u0);
    const double cd = std::cos(du), sd = std::sin(du);
    for (int k = 0; k < NS; ++k)
    {
        const double r = a * (1.0 - e * (cu * ca + su * sa)); // radius at true anomaly u - argp
        const int ir = (int)std::floor((r - kRMin) / kDR);
        if (ir >= 0 && ir < NR)
        {
            // Equal-area latitude bands (uniform in sin lat); heading: east = cos i, north = sin i cos u.
            const int il = std::clamp((int)std::floor((si * su + 1.0) * 0.5 * NL), 0, NL - 1);
            const int ih = headingBin(ci, si * cu);
            c[((size_t)ir * NL + il) * NH + ih] += q;
            m[(size_t)ir * NL + il] += mq;
        }
        const double c2 = cu * cd - su * sd;
        su = su * cd + cu * sd;
        cu = c2;
    }
}

void DebrisGrid::deposit(double a, double e, double incl, double argp, double weight, double massKg, uint64_t seed,
                         int sign)
{
    if (cnt.empty())
        clear();
    (void)sign; // removal goes through the death buckets (World::retireParticles)
    depositInto(cnt, massG, a, e, incl, argp, weight, massKg, seed);
}

double DebrisGrid::rate(const glm::dvec3 &r, const glm::dvec3 &v, double pick, double &vRelOut, double &massOut) const
{
    vRelOut = 0.0;
    massOut = 0.1;
    if (cnt.empty())
        return 0.0;
    const double rm = glm::length(r);
    const int ir = (int)std::floor((rm - kRMin) / kDR);
    if (ir < 0 || ir >= NR)
        return 0.0;
    const double sinLat = std::clamp(r.z / rm, -1.0, 1.0);
    const int il = std::clamp((int)std::floor((sinLat + 1.0) * 0.5 * NL), 0, NL - 1);
    const size_t cell = (size_t)ir * NL + il;
    const int64_t *c = &cnt[cell * NH];
    int64_t sum = 0;
    for (int k = 0; k < NH; ++k)
        sum += std::max<int64_t>(c[k], 0);
    if (sum <= 0)
        return 0.0;
    const glm::dvec3 up = r / rm;
    glm::dvec3 east = glm::cross(glm::dvec3(0.0, 0.0, 1.0), up);
    const double el = glm::length(east);
    east = el > 1e-9 ? east / el : glm::dvec3(1.0, 0.0, 0.0);
    const glm::dvec3 north = glm::cross(up, east);
    const double psiT = std::atan2(glm::dot(v, east), glm::dot(v, north));
    const double speed = glm::length(v);
    // The cell is a latitude band of a spherical shell: all longitudes (orbit-averaged).
    const double r1 = kRMin + ir * kDR, r2 = r1 + kDR;
    const double V = kTwoPi / 3.0 * (r2 * r2 * r2 - r1 * r1 * r1) * (2.0 / NL); // equal-area band
    double contrib[NH], tot = 0.0;
    for (int k = 0; k < NH; ++k)
    {
        contrib[k] = 0.0;
        if (c[k] <= 0)
            continue;
        const double n = (double)c[k] / kFix / V;
        const double dpsi = psiT - ((double)k + 0.5) * (kTwoPi / NH);
        const double vh = 2.0 * speed * std::abs(std::sin(0.5 * dpsi));
        const double vrel = std::sqrt(vh * vh + 300.0 * 300.0); // + the eccentricities' radial speeds
        contrib[k] = n * vrel;
        tot += contrib[k];
    }
    double acc = 0.0;
    for (int k = 0; k < NH; ++k)
    {
        acc += contrib[k];
        if (contrib[k] > 0.0 && acc >= pick * tot)
        {
            const double dpsi = psiT - ((double)k + 0.5) * (kTwoPi / NH);
            const double vh = 2.0 * speed * std::abs(std::sin(0.5 * dpsi));
            vRelOut = std::sqrt(vh * vh + 300.0 * 300.0);
            break;
        }
    }
    massOut = std::max(1e-3, (double)massG[cell] / 1000.0 / (double)sum);
    return tot;
}

double DebrisGrid::total() const
{
    int64_t s = 0;
    for (int64_t x : cnt)
        s += x;
    return (double)s / kFix;
}

void World::reset()
{
    rosterDead.assign(roster.size(), kNever);
    rosterEnd.assign(roster.size(), kNever);
    rosterDecay.assign(roster.size(), kNever);
    pool.clear();
    events.clear();
    poolChanges.clear();
    poolGen.clear();
    poolFree.clear();
    releaseHeap.clear();
    recAdded = 0;
    rosterChanged.clear();
    grid.clear();
    deathBuckets.clear();
    smallAlive = 0;
    smallStreams.clear();
    smallStreamsMade = 0;
    frontier = s.startT != 0.0 ? std::min(s.startT, s.epoch) : s.epoch;
    sampleCount = 0;
    journal.clear();
    journalOff = 0;
    journalOn = false;
    checkpoints.clear();
}

// ── Rollback (2026-10-19) ──────────────────────────────────────────────────────────────────────────
void World::checkpoint(double t, size_t pendingNext)
{
    if (!journalOn)
    {
        journal.clear();
        journalOff = 0;
        checkpoints.clear();
        journalOn = true;
    }
    Checkpoint c;
    c.t = t;
    c.journal = journalOff + journal.size();
    c.events = events.size();
    c.pool = pool.size();
    c.recAdded = recAdded;
    c.smallAlive = smallAlive;
    c.smallStreamsMade = smallStreamsMade;
    c.sampleCount = sampleCount;
    c.pendingNext = pendingNext;
    checkpoints.push_back(c);
    if (journal.size() > journalCap)
        dropJournal(); // too much since the oldest kept one: no rollback until the next checkpoint
}

void World::dropJournal()
{
    journal.clear();
    journalOff = 0;
    checkpoints.clear();
    journalOn = false;
}

void World::dropCheckpointsBefore(double t)
{
    // Keep the latest checkpoint at or before t (a breakup at t rewinds there) and every later one.
    size_t keep = 0;
    for (size_t k = 0; k < checkpoints.size(); ++k)
        if (checkpoints[k].t <= t)
            keep = k;
    if (keep == 0)
        return;
    const size_t cut = checkpoints[keep].journal - journalOff;
    journal.erase(journal.begin(), journal.begin() + (ptrdiff_t)std::min(cut, journal.size()));
    journalOff += cut;
    checkpoints.erase(checkpoints.begin(), checkpoints.begin() + (ptrdiff_t)keep);
}

int World::checkpointAtOrBefore(double t) const
{
    int best = -1;
    for (size_t k = 0; k < checkpoints.size(); ++k)
        if (checkpoints[k].t <= t)
            best = (int)k;
    return best;
}

size_t World::rollback(int k)
{
    const Checkpoint c = checkpoints[(size_t)k];
    const size_t stop = c.journal - journalOff;
    // The release heap is a multiset (its pop order is fixed by (sec, slot), whatever its layout): what was pushed
    // since goes, what was popped comes back, then it is re-heaped once.
    std::vector<Release> pushed, popped;
    while (journal.size() > stop)
    {
        JEntry &e = journal.back();
        switch (e.op)
        {
        case J_POOL:
            pool[e.a] = e.rec;
            break;
        case J_GEN:
            poolGen[e.a] = (uint32_t)e.b;
            break;
        case J_RDEAD:
            rosterDead[e.a] = (int32_t)e.b;
            break;
        case J_REND:
            rosterEnd[e.a] = (int32_t)e.b;
            break;
        case J_RDECAY:
            rosterDecay[e.a] = (int32_t)e.b;
            break;
        case J_FREE_PUSH:
            if (!poolFree.empty())
                poolFree.pop_back();
            break;
        case J_FREE_POP:
            poolFree.push_back(e.a);
            break;
        case J_HEAP_PUSH:
            pushed.push_back({(int32_t)e.b, e.a, e.c});
            break;
        case J_HEAP_POP:
            popped.push_back({(int32_t)e.b, e.a, e.c});
            break;
        case J_GRID:
            grid.depositInto(grid.cnt, grid.massG, e.d[0], e.d[1], e.d[2], e.d[3], e.d[4], e.d[5], (uint64_t)e.b, -1);
            break;
        case J_BUCKET:
        {
            DeathBucket &bk = deathBuckets[(int32_t)e.a];
            grid.depositInto(bk.cnt, bk.massG, e.d[0], e.d[1], e.d[2], e.d[3], e.d[4], e.d[5], (uint64_t)e.b, -1);
            break;
        }
        case J_RETIRE:
            if (e.bucket)
            {
                if (!grid.cnt.empty() && !e.bucket->cnt.empty())
                {
                    for (size_t i = 0; i < grid.cnt.size(); ++i)
                        grid.cnt[i] += e.bucket->cnt[i];
                    for (size_t i = 0; i < grid.massG.size(); ++i)
                        grid.massG[i] += e.bucket->massG[i];
                }
                deathBuckets[(int32_t)e.a] = std::move(*e.bucket);
            }
            break;
        }
        journal.pop_back();
    }
    if (!pushed.empty() || !popped.empty())
    {
        auto key = [](const Release &r) { return std::make_tuple(r.sec, r.slot, r.gen); };
        std::map<std::tuple<int32_t, uint32_t, uint32_t>, int> remove;
        for (const Release &r : pushed)
            ++remove[key(r)];
        releaseHeap.insert(releaseHeap.end(), popped.begin(), popped.end());
        std::vector<Release> kept;
        kept.reserve(releaseHeap.size());
        for (const Release &r : releaseHeap)
        {
            auto it = remove.find(key(r));
            if (it != remove.end() && it->second > 0)
            {
                --it->second;
                continue;
            }
            kept.push_back(r);
        }
        releaseHeap.swap(kept);
        std::make_heap(releaseHeap.begin(), releaseHeap.end(), std::greater<>());
    }
    events.resize(c.events);
    pool.resize(c.pool);
    poolGen.resize(c.pool);
    // Free slots past the old end cannot exist (they were appended after the checkpoint).
    recAdded = c.recAdded;
    smallAlive = c.smallAlive;
    smallStreamsMade = c.smallStreamsMade;
    sampleCount = c.sampleCount;
    frontier = c.t;
    poolChanges.clear();
    rosterChanged.clear();
    smallStreams.clear();
    checkpoints.resize((size_t)k);
    if (checkpoints.empty())
    {
        journal.clear();
        journalOff = 0;
        journalOn = false;
    }
    return c.pendingNext;
}


int32_t World::secOf(double t) const
{
    const double d = std::floor(t - s.epoch);
    return (int32_t)std::clamp(d, -2147483000.0, 2147483000.0);
}

State World::stateOf(uint32_t ref, double t) const
{
    SatOrbitState st;
    if (ref & kPoolBit)
    {
        // The pool's Kepler orbit directly (satOrbitStateAt's keplerStateAt without the attitude terms the engine
        // never reads, and with Newton stopping once converged: the engine's largest cost is these positions).
        const PoolRec &r = pool[ref & ~kPoolBit];
        const double M = std::fmod((double)r.M0 + (double)r.n * t, kTwoPi);
        const double w = std::fmod((double)r.argp0 + (double)r.argpDot * t, kTwoPi);
        const double O = std::fmod((double)r.raan0 + (double)r.raanDot * t, kTwoPi);
        const double ecc = r.e;
        double E = M + ecc * std::sin(M);
        for (int it = 0; it < 8; ++it)
        {
            const double dE = (E - ecc * std::sin(E) - M) / (1.0 - ecc * std::cos(E));
            E -= dE;
            if (std::abs(dE) < 1e-12)
                break;
        }
        const double cE = std::cos(E), sE = std::sin(E), sq = std::sqrt(std::max(0.0, 1.0 - ecc * ecc));
        const double a = r.a;
        const double x = a * (cE - ecc), y = a * sq * sE;
        const double kk = a * std::sqrt(kGM / (a * a * a)) / (1.0 - ecc * cE);
        const double cO = std::cos(O), sO = std::sin(O), cw = std::cos(w), sw = std::sin(w);
        const double ci = std::cos(r.incl), si = std::sin(r.incl);
        const glm::dvec3 P(cw * cO - sw * sO * ci, cw * sO + sw * cO * ci, sw * si);
        const glm::dvec3 Q(-sw * cO - cw * sO * ci, -sw * sO + cw * cO * ci, cw * si);
        return {x * P + y * Q, (-kk * sE) * P + (kk * sq * cE) * Q};
    }
    else
    {
        SatOrbitElems e = rosterOrbitElems(roster[ref], s.raanAnchorT);
        // A dead formation member drifts along-track (sat_orbit.comp formationDrift).
        if (roster[ref].formation && ref < rosterDead.size() && rosterDead[ref] != kNever)
        {
            const double tau = t - s.epoch - (double)rosterDead[ref];
            if (tau > 0.0)
            {
                const double whole = std::floor(tau);
                e.u0 += kTwoPi * formationDriftTurns(ref, (uint32_t)std::min(whole, 4294967295.0), tau - whole);
            }
        }
        st = satOrbitStateAt(e, t);
    }
    return {st.posEci, st.velEci};
}

bool World::dead(uint32_t ref, double t) const
{
    if (ref & kPoolBit)
        return true;
    return ref < roster.size() && secOf(t) >= rosterDead[ref];
}

bool World::alive(uint32_t ref, double t) const
{
    if (ref == kNoRef)
        return false;
    const int32_t sec = secOf(t);
    if (ref & kPoolBit)
    {
        const uint32_t j = ref & ~kPoolBit;
        return j < pool.size() && sec >= pool[j].bornSec && sec < pool[j].deathSec;
    }
    return ref < roster.size() && sec < rosterEnd[ref];
}

uint32_t World::addRecord(const PoolRec &r)
{
    // Slots whose record has been gone kReleaseDelayS by this one's birth are free (a stale entry: the slot moved on
    // to a later generation, or its record's death changed and a newer entry was pushed).
    while (!releaseHeap.empty() && releaseHeap.front().sec <= r.bornSec)
    {
        std::pop_heap(releaseHeap.begin(), releaseHeap.end(), std::greater<>());
        const Release rel = releaseHeap.back();
        releaseHeap.pop_back();
        jSimple(J_HEAP_POP, rel.slot, rel.sec, rel.gen);
        if (rel.slot < pool.size() && poolGen[rel.slot] == rel.gen && pool[rel.slot].deathSec != kNever &&
            pool[rel.slot].deathSec + kReleaseDelayS == rel.sec && !(pool[rel.slot].flags & kRecFree))
        {
            jPool(rel.slot);
            pool[rel.slot].flags |= kRecFree;
            poolFree.push_back(rel.slot);
            jSimple(J_FREE_PUSH, rel.slot);
        }
    }
    uint32_t slot;
    if (!poolFree.empty())
    {
        slot = poolFree.back();
        poolFree.pop_back();
        jSimple(J_FREE_POP, slot);
        jPool(slot);
        pool[slot] = r;
        jSimple(J_GEN, slot, poolGen[slot]);
        ++poolGen[slot];
    }
    else if (pool.size() < s.poolCapacity)
    {
        slot = (uint32_t)pool.size();
        pool.push_back(r);
        poolGen.push_back(0);
    }
    else
        return kNoRef; // full of live pieces
    ++recAdded;
    if (r.deathSec != kNever)
    {
        releaseHeap.push_back({r.deathSec + kReleaseDelayS, slot, poolGen[slot]});
        std::push_heap(releaseHeap.begin(), releaseHeap.end(), std::greater<>());
        jSimple(J_HEAP_PUSH, slot, r.deathSec + kReleaseDelayS, poolGen[slot]);
    }
    poolChanges.push_back({slot, poolGen[slot], r.deathSec, 1, r});
    return slot;
}

double World::deathTime(const PoolRec &r, double t) const
{
    if (r.e >= 0.99 || r.a <= 0.0)
        return t;
    return t + lifetime(t, r.a, r.e, r.cdAm);
}

double World::lifetime(double t, double a, double e, double cdAm) const
{
    const double gain = s.stormDensityGain;
    auto exact = [&](double aa, double ee, double cc, double tt) {
        return decayTime(tt, aa, ee, cc, s.horizonS, [gain](double x) { return 1.0 + gain * scriptedStormShape(x); });
    };
    if (t < s.epoch + 12.0 * 86400.0 || e > 0.2)
        return exact(a, e, cdAm, t);
    // The table: height (km) 100..2100 in 10-km steps x log10(cdAm) -4..1 in 0.1 steps, circular orbits at the
    // height h = perigee + 0.3 (apogee - perigee) (an eccentric orbit loses its apogee first; drag acts near perigee).
    constexpr int NH = 201, NC = 51;
    if (lifeTab.empty())
        lifeTab.assign((size_t)NH * NC, std::numeric_limits<float>::quiet_NaN());
    const double hp = a * (1.0 - e) - kRMean, ha = a * (1.0 + e) - kRMean;
    const double h = (hp + 0.3 * (ha - hp)) * 1e-3;
    const double x = std::clamp((h - 100.0) / 10.0, 0.0, (double)(NH - 1) - 1e-9);
    const double y = std::clamp((std::log10(std::max(cdAm, 1e-6)) + 4.0) / 0.1, 0.0, (double)(NC - 1) - 1e-9);
    const int ix = (int)x, iy = (int)y;
    const double fx = x - ix, fy = y - iy;
    auto cell = [&](int i, int j) {
        float &v = lifeTab[(size_t)i * NC + j];
        if (std::isnan(v))
        {
            // Evaluated after the storm (a fixed time past the epoch): only the orbit and cdAm matter.
            const double T = exact(kRMean + (100.0 + 10.0 * i) * 1e3, 0.0, std::pow(10.0, -4.0 + 0.1 * j),
                                   s.epoch + 13.0 * 86400.0);
            v = (float)std::log(std::max(T, 1.0));
        }
        return (double)v;
    };
    const double L = (1 - fx) * (1 - fy) * cell(ix, iy) + fx * (1 - fy) * cell(ix + 1, iy) +
                     (1 - fx) * fy * cell(ix, iy + 1) + fx * fy * cell(ix + 1, iy + 1);
    return std::exp(L);
}

PoolRec World::recordFromState(const State &st, double t, uint32_t parentRef, const glm::dvec3 &dv) const
{
    PoolRec r;
    const Elems el = elemsFromState({st.r, st.v + dv});
    // The parent's actual rates, and its J2 rates as a Keplerian orbit: the child keeps the parent's
    // motion plus the J2 DIFFERENCE between them, so a dead satellite with no kick stays exactly on its
    // formation's track and a fragment cloud spreads around its parent's plane (roster orbits have no
    // J2 of their own outside the SSO precession).
    double pa, pe, pi, along, raanDot0, argpDot0;
    if (parentRef & kPoolBit)
    {
        const PoolRec &p = pool[parentRef & ~kPoolBit];
        pa = p.a;
        pe = p.e;
        pi = p.incl;
        along = (double)p.n + (double)p.argpDot;
        raanDot0 = p.raanDot;
        argpDot0 = p.argpDot;
    }
    else
    {
        const RosterObj &o = roster[parentRef];
        pa = o.R;
        pe = 0.0;
        pi = o.incl;
        along = o.n;
        raanDot0 = o.sso ? satphot::kSSOPrecRate : 0.0;
        argpDot0 = secularJ2(pa, pe, pi).argpDot;
    }
    const Secular sc = secularJ2(std::max(el.a, kRMean), std::min(el.e, 0.99), el.i);
    const Secular sp = secularJ2(pa, pe, pi);
    const double raanDot = raanDot0 + sc.raanDot - sp.raanDot;
    const double argpDot = argpDot0 + sc.argpDot - sp.argpDot;
    const double alongC = along + (sc.Mdot + sc.argpDot) - (sp.Mdot + sp.argpDot);
    const double n = alongC - argpDot;
    r.n = (float)n;
    r.argpDot = (float)argpDot;
    r.raanDot = (float)raanDot;
    r.M0 = (float)wrap2Pi(el.M - (double)r.n * t);
    r.argp0 = (float)wrap2Pi(el.argp - (double)r.argpDot * t);
    r.raan0 = (float)wrap2Pi(el.raan - (double)r.raanDot * t);
    r.a = el.a;
    r.e = el.e;
    r.incl = el.i;
    r.parent = parentRef;
    r.bornSec = secOf(t);
    return r;
}

void World::killRoster(uint32_t ref, double t, uint32_t eventIdx)
{
    (void)eventIdx;
    if ((ref & kPoolBit) || !alive(ref, t) || dead(ref, t))
        return;
    const RosterObj &o = roster[ref];
    const TypeInfo &ty = types[o.type];
    const int32_t sec = secOf(t);
    jSimple(J_RDEAD, ref, rosterDead[ref]);
    rosterDead[ref] = sec;
    // No longer flying against drag: it re-enters when its (circular) orbit decays.
    const double cdAm = 2.2 * ty.areaM2 / std::max(ty.massKg, 1.0);
    const double td = t + lifetime(t, (double)o.R, 0.0, cdAm);
    if (td < s.epoch + s.horizonS * 1.5)
    {
        jSimple(J_RDECAY, ref, rosterDecay[ref]);
        jSimple(J_REND, ref, rosterEnd[ref]);
        rosterDecay[ref] = std::max(sec, secOf(td));
        rosterEnd[ref] = std::min(rosterEnd[ref], rosterDecay[ref]);
    }
    rosterChanged.push_back(ref);
}

// The kind of large piece a breakup of `ref` leaves: by its surface (arrays, radiators, the rest); a kit piece
// breaks into more of its own kind.
int World::kindOf(uint32_t ref, double h) const
{
    if (ref == kNoRef || ref == kSmallRef)
        return 2;
    const TypeInfo &ty = types[(ref & kPoolBit) ? pool[ref & ~kPoolBit].type : roster[ref].type];
    if (ty.kitKind >= 0)
        return ty.kitKind;
    if (h < ty.arrayShare)
        return 0;
    if (h < ty.arrayShare + ty.radiatorShare)
        return 1;
    return 2;
}

// Where a breakup between refA and refB is cut: the largest piece is a fraction of the larger object's size (a
// 70-m Starmind leaves array sections metres long; a 30-cm fragment nothing over a metre), and the large pieces
// never carry more than 60% of the mass.
BreakupSizes World::sizesFor(uint32_t refA, uint32_t refB, int maxLarge) const
{
    auto sizeOf = [&](uint32_t ref) {
        if (ref == kNoRef || ref == kSmallRef)
            return 0.0;
        if (ref & kPoolBit)
            return 2.0 * (double)pool[ref & ~kPoolBit].radiusM;
        return types[roster[ref].type].sizeM;
    };
    auto massOfRef = [&](uint32_t ref) {
        if (ref == kNoRef || ref == kSmallRef)
            return 0.0;
        if (ref & kPoolBit)
            return (double)pool[ref & ~kPoolBit].massKg;
        return types[roster[ref].type].massKg;
    };
    BreakupSizes sz;
    sz.LcMin = s.LcMin;
    sz.LcRecord = s.LcRecord;
    sz.LcMax = std::clamp(0.15 * std::max(sizeOf(refA), sizeOf(refB)), sz.LcMin * 1.5, 25.0);
    sz.maxLarge = maxLarge;
    sz.maxLargeMass = 0.6 * (massOfRef(refA) + massOfRef(refB));
    sz.nSmallSamples = 16;
    return sz;
}

void World::spawnFragments(const BreakupResult &br, uint32_t refA, uint32_t refB, const State &sa, const State &sb,
                           double t, uint32_t eventIdx, uint32_t seed)
{
    const bool haveB = refB != kNoRef && refB != kSmallRef;
    for (size_t k = 0; k < br.fragments.size(); ++k)
    {
        const Fragment &f = br.fragments[k];
        const bool sideB = f.side == 1 && haveB;
        const uint32_t parent = sideB ? refB : refA;
        PoolRec r = recordFromState(sideB ? sb : sa, t, parent, f.dv);
        double area = f.areaM2, mass = f.massKg;
        r.type = (uint16_t)s.debrisType;
        r.flux = (float)std::clamp(area / std::max(s.debrisModelArea, 1e-6), 1e-4, 1e3);
        // A kit piece of the parent's kind, its area snapped to the nearest size class (flux 1: it can be drawn
        // as a mesh; the A/m, and so the drag, is the fragment's).
        const int kind = f.kind >= 0 ? f.kind : kindOf(parent, hash01(seed, k, 35));
        int best = -1;
        double bestD = 1e30;
        for (int c = 0; c < 3; ++c)
            if (s.kitType[kind][c] >= 0 && s.kitArea[kind][c] > 0.0)
            {
                const double d = std::abs(std::log(area / s.kitArea[kind][c]));
                if (d < bestD)
                {
                    bestD = d;
                    best = c;
                }
            }
        if (best >= 0)
        {
            mass *= s.kitArea[kind][best] / area;
            area = s.kitArea[kind][best];
            r.type = (uint16_t)s.kitType[kind][best];
            r.flux = 1.0f;
        }
        r.constIdx = s.debrisConst;
        r.flags = kRecFragment;
        r.massKg = (float)mass;
        r.radiusM = (float)std::sqrt(area / kPi);
        r.cdAm = (float)std::clamp(2.2 * area / std::max(mass, 1e-6), 1e-4, 10.0);
        r.weight = (float)f.weight;
        // Large pieces tumble slowly (seconds to a couple of minutes a turn): their glints are long flashes.
        r.tumbleRate = (float)((0.05 + 0.75 * hash01(seed, k, 33)) / std::sqrt(std::max(f.Lc, 1.0)));
        r.tumble0 = (float)wrap2Pi(kTwoPi * hash01(seed, k, 34) - (double)r.tumbleRate * t);
        r.event = eventIdx;
        const double td = deathTime(r, t);
        r.deathSec = std::max(r.bornSec, td >= s.epoch + s.horizonS * 1.5 ? kNever : secOf(td));
        r.decaySec = r.deathSec;
        addRecord(r);
    }
}

// A large satellite's appendages shear off whole when it shatters: array sections (2-6) and radiator sections (1-2)
// many metres long, which the size distribution (made for compact bodies) leaves out. Slow (10-50 m/s: they tear off
// rather than being blasted), so they stay near the parent's orbit and tumble: the brightest flashes in the debris.
void World::addStructuralPieces(BreakupResult &br, uint32_t ref, int side, uint32_t seed) const
{
    if (ref == kNoRef || ref == kSmallRef || (ref & kPoolBit))
        return;
    const TypeInfo &ty = types[roster[ref].type];
    if (ty.sizeM < 20.0)
        return;
    auto add = [&](int kind, double Lc, uint64_t k) {
        Fragment f;
        f.Lc = Lc;
        f.areaM2 = 0.556945 * std::pow(Lc, 2.0047077);
        f.massKg = f.areaM2 / 0.15;
        const double dvm = 10.0 + 40.0 * hash01(seed, k, 71);
        const double z = 2.0 * hash01(seed, k, 72) - 1.0, ph = kTwoPi * hash01(seed, k, 73);
        const double rxy = std::sqrt(std::max(0.0, 1.0 - z * z));
        f.dv = dvm * glm::dvec3(rxy * std::cos(ph), rxy * std::sin(ph), z);
        f.side = side;
        f.kind = kind;
        f.weight = 1.0;
        br.fragments.push_back(f);
    };
    const double big = std::min(0.15 * ty.sizeM, 12.0);
    if (ty.arrayShare > 0.2)
    {
        const int n = 2 + (int)(5.0 * hash01(seed, 900, 74));
        for (int k = 0; k < n; ++k)
            add(0, big * (0.6 + 0.4 * hash01(seed, 910 + k, 75)), 910 + k);
    }
    if (ty.radiatorShare > 0.05)
    {
        const int n = 1 + (int)(2.0 * hash01(seed, 920, 76));
        for (int k = 0; k < n; ++k)
            add(1, big * (0.5 + 0.4 * hash01(seed, 930 + k, 77)), 930 + k);
    }
}

// The small fragments: each statistical sample's orbit joins the density grid until drag brings it down.
void World::spawnSmall(const BreakupResult &br, const State &st, double t, uint32_t seed, Event &ev)
{
    double n = 0.0, dv2 = 0.0;
    struct Cand
    {
        double deathT, half, w, m;
        Elems el;
    };
    std::vector<Cand> cands;
    for (size_t k = 0; k < br.small.size(); ++k)
    {
        const Fragment &f = br.small[k];
        n += f.weight;
        dv2 += f.weight * glm::dot(f.dv, f.dv);
        const Elems el = elemsFromState({st.r, st.v + f.dv});
        if (el.e >= 0.99 || el.a <= 0.0 || el.a * (1.0 - el.e) <= kRMean + 120000.0)
            continue;
        const uint64_t pseed = ((uint64_t)seed << 8) ^ (uint64_t)k ^ ((uint64_t)events.size() << 32);
        const double cdAm = std::clamp(2.2 * f.areaM2 / std::max(f.massKg, 1e-6), 1e-4, 10.0);
        const double deathT = t + lifetime(t, el.a, el.e, cdAm);
        grid.deposit(el.a, el.e, el.i, el.argp, f.weight, f.massKg, pseed, +1);
        jDeposit(J_GRID, 0, el.a, el.e, el.i, el.argp, f.weight, f.massKg, pseed);
        ++smallAlive;
        if (deathT < s.epoch + s.horizonS * 1.5)
        {
            const int32_t bkey = (int32_t)std::floor((deathT - s.epoch) / kDeathBucketS);
            DeathBucket &bk = deathBuckets[bkey];
            grid.depositInto(bk.cnt, bk.massG, el.a, el.e, el.i, el.argp, f.weight, f.massKg, pseed);
            jDeposit(J_BUCKET, (uint32_t)bkey, el.a, el.e, el.i, el.argp, f.weight, f.massKg, pseed);
        }
        if (deathT < s.epoch + s.horizonS)
            cands.push_back({deathT, std::clamp(0.15 * (deathT - t), 3600.0, 5.0 * 86400.0), f.weight, f.massKg, el});
    }
    // The re-entry streams: merged by decay time into at most kMaxStreamsPerBreakup (count-weighted means; the window
    // covers the merged ones' windows, up to 5 days).
    std::sort(cands.begin(), cands.end(), [](const Cand &x, const Cand &y) { return x.deathT < y.deathT; });
    const size_t G = std::min<size_t>(cands.size(), SmallStream::kMaxStreamsPerBreakup);
    for (size_t g = 0; g < G; ++g)
    {
        const size_t b0 = g * cands.size() / G, b1 = (g + 1) * cands.size() / G;
        double w = 0.0, dT = 0.0, lm = 0.0, lo = 1e300, hi = -1e300, rd = 0.0;
        for (size_t q = b0; q < b1; ++q)
        {
            const Cand &c = cands[q];
            w += c.w;
            dT += c.w * c.deathT;
            lm += c.w * std::log2(std::max(c.m, 1e-4));
            lo = std::min(lo, c.deathT - c.half);
            hi = std::max(hi, c.deathT + c.half);
            rd += c.w * secularJ2(c.el.a, std::min(c.el.e, 0.99), c.el.i).raanDot;
        }
        if (w <= 0.0)
            continue;
        dT /= w;
        const Elems &el0 = cands[b0].el;
        SmallStream ss;
        ss.deathSec = secOf(dT);
        ss.bornSec = secOf(t);
        ss.halfMin = (uint16_t)std::clamp(std::max(dT - lo, hi - dT) / 60.0, 60.0, 7200.0);
        const double tp = kTwoPi;
        ss.inclQ = (uint16_t)std::clamp(el0.i / tp * 65536.0, 0.0, 65535.0);
        ss.raanQ = (uint16_t)((uint32_t)std::llround(std::fmod(el0.raan + 2.0 * tp, tp) / tp * 65536.0) & 0xFFFFu);
        ss.massQ = (uint16_t)std::clamp((lm / w + 16.0) * 2048.0, 0.0, 65535.0);
        ss.count = (float)w;
        ss.raanDot = (float)(rd / w);
        smallStreams.push_back(ss);
        ++smallStreamsMade;
    }
    ev.nSmall = (float)n;
    ev.dvSmall = n > 0.0 ? (float)std::sqrt(dv2 / n) : 0.0f;
}

void World::retireParticles(double t)
{
    // Every window that has ended leaves the grid (exact: the same integer deposits, subtracted).
    while (!deathBuckets.empty() && s.epoch + (deathBuckets.begin()->first + 1) * kDeathBucketS <= t)
    {
        const DeathBucket &bk = deathBuckets.begin()->second;
        if (!grid.cnt.empty() && !bk.cnt.empty())
        {
            for (size_t i = 0; i < grid.cnt.size(); ++i)
                grid.cnt[i] -= bk.cnt[i];
            for (size_t i = 0; i < grid.massG.size(); ++i)
                grid.massG[i] -= bk.massG[i];
        }
        if (journalOn)
        {
            JEntry e;
            e.op = J_RETIRE;
            e.a = (uint32_t)deathBuckets.begin()->first;
            e.bucket = std::make_shared<DeathBucket>(std::move(deathBuckets.begin()->second));
            journal.push_back(std::move(e));
        }
        deathBuckets.erase(deathBuckets.begin());
    }
}

// A formation member broke up: the cloud sweeps through its cluster within minutes. Each neighbour is hit with
// clusterHitP: slow fragments (the cloud's own spread, ~100 m/s), so damaged and tumbling, not shattered.
void World::damageCluster(uint32_t ref, double t, uint32_t eventIdx, uint32_t seed)
{
    if ((ref & kPoolBit) || ref >= roster.size())
        return;
    const RosterObj &o = roster[ref];
    if (!o.formation || o.clusterSize < 2)
        return;
    const uint32_t first = ref - o.clusterSlot;
    for (uint32_t m = first; m < first + o.clusterSize && m < roster.size(); ++m)
        if (m != ref && hash01(seed, m, 51) < s.clusterHitP)
            killRoster(m, t, eventIdx);
}

void endObject(World &w, uint32_t ref, double t, bool keepWeighted)
{
    if (ref == kNoRef || ref == kSmallRef)
        return;
    const int32_t sec = w.secOf(t);
    if (ref & kPoolBit)
    {
        PoolRec &r = w.pool[ref & ~kPoolBit];
        if (keepWeighted && r.weight > 1.5f)
            return; // a representative record: one of its many members was lost
        if (sec < r.deathSec)
        {
            const uint32_t slot = ref & ~kPoolBit;
            w.jPool(slot);
            r.deathSec = std::max(sec, r.bornSec);
            w.poolChanges.push_back({slot, w.poolGen[slot], r.deathSec, 0, {}});
            w.releaseHeap.push_back({r.deathSec + World::kReleaseDelayS, slot, w.poolGen[slot]});
            std::push_heap(w.releaseHeap.begin(), w.releaseHeap.end(), std::greater<>());
            w.jSimple(World::J_HEAP_PUSH, slot, r.deathSec + World::kReleaseDelayS, w.poolGen[slot]);
        }
    }
    else if (sec < w.rosterEnd[ref])
    {
        w.jSimple(World::J_REND, ref, w.rosterEnd[ref]);
        w.rosterEnd[ref] = sec;
        w.rosterChanged.push_back(ref);
    }
}

static double massOf(const World &w, uint32_t ref)
{
    if (ref & kPoolBit)
        return w.pool[ref & ~kPoolBit].massKg;
    return w.types[w.roster[ref].type].massKg;
}

bool World::apply(Event ev)
{
    const uint32_t idx = (uint32_t)events.size();
    const uint64_t added0 = recAdded;
    // Occupancy: live pieces (and the dead ones not yet released) over the capacity.
    const double fill = (double)(pool.size() - poolFree.size()) / std::max<double>(s.poolCapacity, 1.0);
    // Large-piece records: up to maxRecordsPerEvent, fewer as the pool fills (beyond: each stands for several).
    const int maxRec = fill >= 1.0 ? 0
                                   : std::clamp((int)std::ceil(s.maxRecordsPerEvent * (1.0 - fill)), 1,
                                                std::max(s.maxRecordsPerEvent, 1));
    auto setWhere = [&](const State &st) {
        for (int k = 0; k < 3; ++k)
        {
            ev.pos[k] = (float)st.r[k];
            ev.vel[k] = (float)st.v[k];
        }
    };
    switch (ev.kind)
    {
    case EV_KILL:
        if (!alive(ev.a, ev.t) || dead(ev.a, ev.t))
            return false;
        killRoster(ev.a, ev.t, idx);
        break;
    case EV_BUMP:
        // a: the dead member drifting in; b: its neighbour, killed when the seed said so (catastrophic).
        if (!alive(ev.b, ev.t) || dead(ev.b, ev.t))
            return false;
        if (ev.catastrophic)
            killRoster(ev.b, ev.t, idx);
        break;
    case EV_COLLISION:
    {
        const bool fromGrid = ev.b == kSmallRef;
        if (!alive(ev.a, ev.t) || (!fromGrid && !alive(ev.b, ev.t)))
            return false;
        uint32_t tgt = ev.a, prj = ev.b;
        double mT, mP, v;
        State st, sp;
        if (fromGrid)
        {
            mT = massOf(*this, tgt);
            mP = std::max((double)ev.mProj, 1e-3);
            st = stateOf(tgt, ev.t);
            sp = st;
            v = ev.vRel;
            if (mP > mT)
            {
                std::swap(mT, mP); // a small fragment heavier than the target (a piece of a piece): it breaks
            }
        }
        else
        {
            if (massOf(*this, prj) > massOf(*this, tgt))
                std::swap(tgt, prj);
            mT = massOf(*this, tgt);
            mP = massOf(*this, prj);
            st = stateOf(tgt, ev.t);
            if (ev.sampleDt != 0.0f)
            {
                // The engine found the pair together at the step's sample time ts and spread the event's time through
                // the step (no bursts). At ev.t the two are anywhere on their orbits (350-13000 km apart measured,
                // 2026-10-24): the projectile's fragments (and the dust Watch) were built where IT was, far from the
                // target's. The encounter is carried along the target's orbit instead: the relative velocity at ts in
                // the target's local frame (radial, along, normal) is turned into its frame at ev.t, and the projectile
                // is placed on the target. Same impact speed and geometry, one place.
                const double ts = ev.t - (double)ev.sampleDt;
                const State a0 = stateOf(tgt, ts), p0 = stateOf(prj, ts);
                auto frame = [](const State &s, glm::dvec3 &R, glm::dvec3 &A, glm::dvec3 &N) {
                    R = glm::normalize(s.r);
                    N = glm::cross(s.r, s.v);
                    N = glm::length(N) > 1e-9 ? glm::normalize(N) : glm::dvec3(0, 0, 1);
                    A = glm::cross(N, R);
                };
                glm::dvec3 R0, A0, N0, R1, A1, N1;
                frame(a0, R0, A0, N0);
                frame(st, R1, A1, N1);
                const glm::dvec3 vr = p0.v - a0.v;
                sp.r = st.r;
                sp.v = st.v + R1 * glm::dot(vr, R0) + A1 * glm::dot(vr, A0) + N1 * glm::dot(vr, N0);
            }
            else
                sp = stateOf(prj, ev.t);
            v = glm::length(st.v - sp.v);
        }
        const double emr = 0.5 * std::min(mT, mP) * v * v / std::max(std::max(mT, mP), 1e-3);
        const int rec = emr >= 40000.0 ? maxRec : (maxRec > 0 ? std::max(1, maxRec / 8) : 0);
        BreakupResult br = breakupCollision(mT, mP, v, sizesFor(tgt, prj, rec), ev.seed);
        if (br.catastrophic && rec > 0)
        {
            addStructuralPieces(br, tgt, 0, ev.seed);
            if (!fromGrid)
                addStructuralPieces(br, prj, 1, ev.seed ^ 0x77u);
        }
        // A hit that changes nothing visible is not an event: a cratering hit that leaves no large piece, by a
        // projectile that survives it (a small fragment, a representative record), on a target it does not kill
        // (millions of these once the debris abounds; recording them would only grow the log).
        const bool kills = !br.catastrophic && !(tgt & kPoolBit) && !dead(tgt, ev.t) && hash01(ev.seed, 7, 41) < 0.5;
        if (!br.catastrophic && br.fragments.empty() && !kills)
        {
            const bool prjSurvives = fromGrid || ((prj & kPoolBit) && pool[prj & ~kPoolBit].weight > 1.5f);
            if (prjSurvives)
                return false;
        }
        ev.vRel = (float)v;
        ev.nReal = (float)br.nReal;
        ev.catastrophic = br.catastrophic ? 1 : 0;
        ev.dustKg = (float)(s.dustShare * br.massKg * (br.catastrophic ? 1.0 : 0.3));
        setWhere(st);
        spawnFragments(br, tgt, fromGrid ? kSmallRef : prj, st, sp, ev.t, idx, ev.seed);
        spawnSmall(br, st, ev.t, ev.seed, ev);
        if (!fromGrid)
            endObject(*this, prj, ev.t, true);
        if (br.catastrophic)
        {
            endObject(*this, tgt, ev.t, true);
            damageCluster(tgt, ev.t, idx, ev.seed);
            if (!fromGrid)
                damageCluster(prj, ev.t, idx, ev.seed ^ 0x5A5Au);
        }
        else if (kills)
            killRoster(tgt, ev.t, idx); // damaged: the target dies and tumbles (its fragments came off it)
        break;
    }
    case EV_EXPLOSION:
    case EV_ASAT:
    {
        if (!alive(ev.a, ev.t))
            return false;
        const State st = stateOf(ev.a, ev.t);
        const double m = massOf(*this, ev.a);
        const BreakupSizes sz = sizesFor(ev.a, kNoRef, maxRec);
        BreakupResult br = ev.kind == EV_ASAT ? breakupCollision(m, 20.0, 9000.0, sz, ev.seed)
                                              : breakupExplosion(m, sz, ev.seed);
        if (maxRec > 0)
            addStructuralPieces(br, ev.a, 0, ev.seed);
        ev.vRel = ev.kind == EV_ASAT ? 9000.0f : 0.0f;
        ev.nReal = (float)br.nReal;
        ev.catastrophic = br.catastrophic ? 1 : 0;
        ev.dustKg = (float)(s.dustShare * br.massKg);
        setWhere(st);
        spawnFragments(br, ev.a, kNoRef, st, st, ev.t, idx, ev.seed);
        spawnSmall(br, st, ev.t, ev.seed, ev);
        endObject(*this, ev.a, ev.t, false);
        damageCluster(ev.a, ev.t, idx, ev.seed);
        break;
    }
    default:
        return false;
    }
    ev.nRec = (uint32_t)(recAdded - added0);
    events.push_back(ev);
    return true;
}

void World::simulate(double tEnd, std::vector<Event> &pending, size_t &pendingNext, const bool *stop)
{
    std::vector<uint32_t> refs;
    std::vector<EngineObject> objs;
    std::vector<float> hitV, hitM;
    while (frontier < tEnd && !(stop && *stop))
    {
        const double t0 = frontier, t1 = std::min(stepStart(frontier) + s.stepS, tEnd);
        if (t0 == stepStart(t0) && wantCheckpoints && t0 >= checkpointFrom)
            checkpoint(t0, pendingNext);
        // A step applies its scripted events, then the engine's, then the player's (sortPending's order: a replay of
        // the step from its engine events gives the same history as sampling it).
        while (pendingNext < pending.size() && pending[pendingNext].t <= t1 && pending[pendingNext].origin != ORIGIN_USER)
            apply(pending[pendingNext++]);
        {
            const double ts = 0.5 * (t0 + t1), dt = t1 - t0;
            const int32_t sec = secOf(ts);
            // The step's own index on the grid (seeds): the same at any replay, whenever the step is run.
            const uint64_t stepK = (uint64_t)stepIndex(stepStart(t0));
            // Before the CME every working satellite dodges (avoidance 1): only the dead and the debris can collide,
            // so the working ones are left out altogether (they could not collide; the draws are per pair).
            const bool preStorm = ts < s.epoch;
            EngineParams ep = s.engine;
            if (preStorm)
                ep.avoidance = 1.0;
            auto clk = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
            double c0 = clk();
            retireParticles(ts);
            refs.clear();
            for (uint32_t i = 0; i < (uint32_t)roster.size(); ++i)
            {
                if (sec >= rosterEnd[i] || !rosterPresent(i, sec))
                    continue;
                if (!types[roster[i].type].passive && sec < rosterDead[i] &&
                    (preStorm || hash01(stepK, i, 21) >= s.activeSample))
                    continue;
                refs.push_back(i);
            }
            for (uint32_t j = 0; j < (uint32_t)pool.size(); ++j)
                if (sec >= pool[j].bornSec && sec < pool[j].deathSec)
                    refs.push_back(j | kPoolBit);
            double c1 = clk();
            profRefs += c1 - c0;
            objs.resize(refs.size());
            hitV.assign(refs.size(), 0.0f);
            hitM.assign(refs.size(), 0.0f);
            const bool gridOn = grid.total() > 0.0;
            const uint64_t gseed = mix64(0x6121D5ull ^ stepK);
            parallelChunks(refs.size(), [&](size_t a, size_t b) {
                for (size_t k = a; k < b; ++k)
                {
                    const uint32_t ref = refs[k];
                    const State st = stateOf(ref, ts);
                    EngineObject &o = objs[k];
                    o.id = ref;
                    o.r = st.r;
                    o.v = st.v;
                    if (ref & kPoolBit)
                    {
                        const PoolRec &p = pool[ref & ~kPoolBit];
                        o.radiusM = p.radiusM;
                        o.weight = p.weight;
                        o.active = false;
                    }
                    else
                    {
                        const TypeInfo &ty = types[roster[ref].type];
                        o.radiusM = (float)ty.radiusM;
                        o.active = !ty.passive && sec < rosterDead[ref];
                        o.weight = o.active ? (float)(1.0 / s.activeSample) : 1.0f;
                    }
                    // The small fragments (the grid): expected hits = flux x cross-section x time.
                    if (gridOn)
                    {
                        double vr = 0.0, mp = 0.0;
                        const double rate = grid.rate(o.r, o.v, hash01(gseed, ref, 14), vr, mp);
                        if (rate > 0.0)
                        {
                            const double rr = (double)o.radiusM + 0.15;
                            double P = rate * kPi * rr * rr * dt * (double)o.weight * s.engine.rateMultiplier;
                            if (o.active)
                                P *= (1.0 - ep.avoidance);
                            if (hash01(gseed, ref, 13) < std::min(P, 1.0))
                            {
                                // The projectile is drawn from the small population's size distribution (most
                                // hits are by ~10-cm, tens-of-gram pieces), not the cell's mean mass: with the
                                // mean (~0.5 kg) nearly every hit on a satellite was catastrophic.
                                (void)mp;
                                const double b = 1.71, lo = s.LcMin, hi = std::max(s.LcRecord, lo * 1.5);
                                const double u = hash01(gseed, ref, 15);
                                const double Lc = std::pow(std::pow(lo, -b) - u * (std::pow(lo, -b) - std::pow(hi, -b)), -1.0 / b);
                                const double chi = sampleChi(Lc, gseed ^ (uint64_t)ref, 16);
                                hitV[k] = (float)vr;
                                hitM[k] = (float)(0.556945 * std::pow(Lc, 2.0047077) / std::pow(10.0, chi));
                            }
                        }
                    }
                }
            });
            double c2 = clk();
            profState += c2 - c1;
            std::vector<EngineCollision> col = engineSample(objs, dt, ep, mix64(0xC0111DEull ^ stepK));
            double c3 = clk();
            profPairs += c3 - c2;
            std::vector<Event> evs;
            evs.reserve(col.size());
            for (const EngineCollision &c : col)
            {
                Event ev;
                ev.t = ts;
                ev.a = c.a;
                ev.b = c.b;
                ev.kind = EV_COLLISION;
                ev.origin = ORIGIN_ENGINE;
                ev.seed = (uint32_t)mix64(((uint64_t)c.a << 32 | c.b) ^ (stepK * 0x9E3779B97F4A7C15ull));
                evs.push_back(ev);
            }
            for (size_t k = 0; k < refs.size(); ++k)
                if (hitV[k] > 0.0f)
                {
                    Event ev;
                    ev.t = ts;
                    ev.a = refs[k];
                    ev.b = kSmallRef;
                    ev.kind = EV_COLLISION;
                    ev.origin = ORIGIN_ENGINE;
                    ev.vRel = hitV[k];
                    ev.mProj = hitM[k];
                    ev.seed = (uint32_t)mix64(((uint64_t)refs[k] << 32 | kSmallRef) ^ (stepK * 0x9E3779B97F4A7C15ull));
                    evs.push_back(ev);
                }
            // Each at its own time within the step (all at the midpoint, every collision of 6 h happened in the same
            // second: dust clouds and re-entries in bursts), applied in time order.
            for (Event &ev : evs)
            {
                ev.t = t0 + dt * hash01(ev.seed, 0x7135ull, 91);
                if (ev.b != kSmallRef)
                    ev.sampleDt = (float)(ev.t - ts);   // (apply carries the pair's encounter from ts to ev.t)
            }
            std::sort(evs.begin(), evs.end(), [](const Event &x, const Event &y) {
                return x.t != y.t ? x.t < y.t : (x.a != y.a ? x.a < y.a : x.b < y.b);
            });
            for (const Event &ev : evs)
                apply(ev);
            profApply += clk() - c3;
            ++sampleCount;
        }
        while (pendingNext < pending.size() && pending[pendingNext].t <= t1)
            apply(pending[pendingNext++]);
        frontier = t1;
    }
}

void sortPending(std::vector<Event> &pending, size_t from, const World &w)
{
    auto cls = [](const Event &e) { return e.origin == ORIGIN_USER ? 2 : e.origin == ORIGIN_ENGINE ? 1 : 0; };
    std::stable_sort(pending.begin() + (ptrdiff_t)from, pending.end(), [&](const Event &a, const Event &b) {
        const int64_t sa = w.stepIndex(w.stepStart(a.t)), sb = w.stepIndex(w.stepStart(b.t));
        if (sa != sb)
            return sa < sb;
        if (cls(a) != cls(b))
            return cls(a) < cls(b);
        return a.t < b.t;
    });
}

std::vector<Event> stormSeedEvents(const World &w, double arriveT, uint64_t seed)
{
    std::vector<Event> out;
    for (uint32_t i = 0; i < (uint32_t)w.roster.size(); ++i)
    {
        const RosterObj &o = w.roster[i];
        const TypeInfo &ty = w.types[o.type];
        if (ty.passive || !w.rosterPresent(i, (int32_t)std::floor(arriveT - w.s.epoch)))
            continue;
        const double alt = o.R - kRMean;
        double p = alt < 650e3 ? 0.015
                   : alt < 1100e3 ? 0.015 + 0.035 * (alt - 650e3) / 450e3
                   : alt < 5000e3 ? 0.08
                                  : 0.12;
        const double si = std::sin(o.incl);
        p *= 0.7 + 0.6 * si * si;
        if (ty.massKg > 10000.0)
            p *= 0.1; // crewed stations: shielded, redundant
        if (hash01(seed, i, 1) >= p)
            continue;
        Event ev;
        ev.kind = EV_KILL;
        ev.origin = ORIGIN_SEED;
        ev.a = i;
        const double h2 = hash01(seed, i, 2);
        ev.t = hash01(seed, i, 3) < 0.85 ? arriveT + 3600.0 * (0.5 + 14.0 * h2 * h2)
                                         : arriveT + 86400.0 * (1.0 + 20.0 * h2); // latent failures
        ev.seed = (uint32_t)(hash01(seed, i, 4) * 4294967295.0);
        out.push_back(ev);
        // A dead formation member drifts (no longer flying against differential drag) into the next
        // member: lognormal, median 4 days. Each bump kills with probability 1/2, and a killed member
        // drifts on in turn.
        if (o.formation && o.clusterSize > 1)
        {
            uint32_t from = i;
            double t = ev.t;
            for (int k = 1; k < o.clusterSize; ++k)
            {
                const RosterObj &f = w.roster[from];
                const uint32_t next = from - f.clusterSlot + (uint32_t)((f.clusterSlot + 1) % f.clusterSize);
                if (next >= w.roster.size())
                    break;
                t += 86400.0 * std::exp(std::log(4.0) + 0.7 * hashNormal(seed, (uint64_t)from * 64 + k, 5));
                Event b;
                b.kind = EV_BUMP;
                b.origin = ORIGIN_SEED;
                b.a = from;
                b.b = next;
                b.t = t;
                b.seed = (uint32_t)(hash01(seed, (uint64_t)from * 64 + k, 6) * 4294967295.0);
                b.catastrophic = hash01(seed, (uint64_t)from * 64 + k, 7) < 0.5 ? 1 : 0;
                out.push_back(b);
                if (!b.catastrophic)
                    break;
                from = next;
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Event &a, const Event &b) { return a.t < b.t; });
    return out;
}
} // namespace kessler
