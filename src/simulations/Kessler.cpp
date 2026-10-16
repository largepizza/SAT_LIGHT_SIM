// Kessler syndrome core (see Kessler.h).
#include "Kessler.h"

#include <algorithm>
#include <cmath>
#include <thread>

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

static void fillFragments(BreakupResult &res, double nReal, double LcMin, double LcMax, int maxRecords,
                          bool collision, double sideBShare, uint64_t seed)
{
    res.nReal = nReal;
    if (nReal <= 0.0 || maxRecords <= 0)
        return;
    int K;
    double w;
    if (nReal <= (double)maxRecords)
    {
        K = (int)std::floor(nReal);
        if (hash01(seed, 0xFFFFFFFFull, 9) < nReal - (double)K)
            ++K;
        w = 1.0;
    }
    else
    {
        K = maxRecords;
        w = nReal / (double)K;
    }
    const double b = collision ? 1.71 : 1.6;
    const double tail = std::pow(LcMax / LcMin, -b); // the truncated power law's CDF floor
    res.fragments.reserve((size_t)K);
    for (int k = 0; k < K; ++k)
    {
        Fragment f;
        const double u = hash01(seed, (uint64_t)k, 1);
        f.Lc = LcMin * std::pow(1.0 - u * (1.0 - tail), -1.0 / b);
        const double chi = sampleChi(f.Lc, seed, (uint64_t)k);
        const double am = std::pow(10.0, chi);
        f.areaM2 = 0.556945 * std::pow(f.Lc, 2.0047077);
        f.massKg = f.areaM2 / am;
        const double nu = (collision ? 0.9 * chi + 2.9 : 0.2 * chi + 1.85) + 0.4 * hashNormal(seed, (uint64_t)k, 5);
        const double dvm = std::pow(10.0, nu);
        const double z = 2.0 * hash01(seed, (uint64_t)k, 6) - 1.0;
        const double ph = kTwoPi * hash01(seed, (uint64_t)k, 7);
        const double rxy = std::sqrt(std::max(0.0, 1.0 - z * z));
        f.dv = dvm * glm::dvec3(rxy * std::cos(ph), rxy * std::sin(ph), z);
        f.side = hash01(seed, (uint64_t)k, 8) < sideBShare ? 1 : 0;
        f.weight = w;
        res.fragments.push_back(f);
    }
}

BreakupResult breakupCollision(double mT, double mP, double vRel, double LcMin, int maxRecords, uint64_t seed)
{
    BreakupResult res;
    if (mP > mT)
        std::swap(mT, mP);
    const double emr = 0.5 * mP * vRel * vRel / std::max(mT, 1e-3); // J/kg
    res.catastrophic = emr >= 40000.0;
    const double M = res.catastrophic ? (mT + mP) : mP * (vRel * 1e-3) * (vRel * 1e-3);
    const double nReal = 0.1 * std::pow(M, 0.75) * std::pow(LcMin, -1.71);
    // Largest fragments: about the size of the larger object (a few metres at most).
    const double LcMax = std::max(LcMin * 1.5, std::min(4.0, 0.8 * std::cbrt(mT / 50.0)));
    fillFragments(res, nReal, LcMin, LcMax, maxRecords, true, res.catastrophic ? mP / (mT + mP) : 0.0, seed);
    return res;
}

BreakupResult breakupExplosion(double m, double LcMin, int maxRecords, uint64_t seed)
{
    BreakupResult res;
    res.catastrophic = true;
    const double nReal = 6.0 * std::pow(LcMin, -1.6);
    const double LcMax = std::max(LcMin * 1.5, std::min(4.0, 0.8 * std::cbrt(m / 50.0)));
    fillFragments(res, nReal, LcMin, LcMax, maxRecords, false, 0.0, seed);
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
    std::sort(keys.begin(), keys.end(), [](const Key &a, const Key &b) { return a.k != b.k ? a.k < b.k : a.i < b.i; });
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

static void parallelChunks(size_t n, const std::function<void(size_t, size_t)> &fn)
{
    // Half the cores: the cascade runs beside the app on its worker thread.
    const size_t hw = std::max<size_t>(1, std::min<size_t>(std::thread::hardware_concurrency() / 2, 16));
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
            th.emplace_back(fn, a, b);
    }
    for (std::thread &t : th)
        t.join();
}

void World::reset()
{
    rosterEnd.assign(roster.size(), kNever);
    rosterCorpse.assign(roster.size(), kNoRef);
    pool.clear();
    events.clear();
    poolDeaths.clear();
    rosterDeaths.clear();
    frontier = s.epoch;
    sampleCount = 0;
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
        const uint32_t j = ref & ~kPoolBit;
        st = satOrbitStateAt(poolOrbitElems(pool[j], j), t);
    }
    else
        st = satOrbitStateAt(rosterOrbitElems(roster[ref], s.raanAnchorT), t);
    return {st.posEci, st.velEci};
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
    pool.push_back(r);
    return (uint32_t)pool.size() - 1;
}

double World::deathTime(const PoolRec &r, double t) const
{
    if (r.e >= 0.99 || r.a <= 0.0)
        return t;
    const double gain = s.stormDensityGain;
    const double dt = decayTime(t, r.a, r.e, r.cdAm, s.horizonS,
                                [gain](double tt) { return 1.0 + gain * scriptedStormShape(tt); });
    return t + dt;
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

void World::killAsCorpse(uint32_t ref, double t, uint32_t eventIdx)
{
    if ((ref & kPoolBit) || !alive(ref, t))
        return;
    const RosterObj &o = roster[ref];
    const TypeInfo &ty = types[o.type];
    PoolRec r = recordFromState(stateOf(ref, t), t, ref, glm::dvec3(0.0));
    r.type = o.type;
    r.constIdx = o.constIdx;
    r.flags = kRecDead;
    r.flux = 1.0f;
    r.massKg = (float)ty.massKg;
    r.radiusM = (float)ty.radiusM;
    r.cdAm = (float)(2.2 * ty.areaM2 / std::max(ty.massKg, 1.0));
    r.weight = 1.0f;
    r.tumbleRate = (float)(0.01 + 0.15 * hash01(eventIdx, ref, 31));
    r.tumble0 = (float)wrap2Pi(kTwoPi * hash01(eventIdx, ref, 32) - (double)r.tumbleRate * t);
    r.event = eventIdx;
    const double td = deathTime(r, t);
    r.deathSec = std::max(r.bornSec, td >= s.epoch + s.horizonS * 1.5 ? kNever : secOf(td));
    const uint32_t j = addRecord(r);
    rosterEnd[ref] = r.bornSec;
    rosterCorpse[ref] = j;
    rosterDeaths.push_back({ref, r.bornSec});
}

void World::spawnFragments(const BreakupResult &br, uint32_t refA, uint32_t refB, const State &sa, const State &sb,
                           double t, uint32_t eventIdx, uint32_t seed)
{
    for (size_t k = 0; k < br.fragments.size(); ++k)
    {
        const Fragment &f = br.fragments[k];
        const bool sideB = f.side == 1 && refB != kNoRef;
        PoolRec r = recordFromState(sideB ? sb : sa, t, sideB ? refB : refA, f.dv);
        r.type = (uint16_t)s.debrisType;
        r.constIdx = s.debrisConst;
        r.flags = kRecFragment;
        r.flux = (float)std::clamp(f.areaM2 / std::max(s.debrisModelArea, 1e-6), 1e-4, 1e3);
        r.massKg = (float)f.massKg;
        r.radiusM = (float)std::sqrt(f.areaM2 / kPi);
        r.cdAm = (float)std::clamp(2.2 * f.areaM2 / std::max(f.massKg, 1e-6), 1e-4, 10.0);
        r.weight = (float)f.weight;
        r.tumbleRate = (float)(0.2 + 2.5 * hash01(seed, k, 33));
        r.tumble0 = (float)wrap2Pi(kTwoPi * hash01(seed, k, 34) - (double)r.tumbleRate * t);
        r.event = eventIdx;
        const double td = deathTime(r, t);
        r.deathSec = std::max(r.bornSec, td >= s.epoch + s.horizonS * 1.5 ? kNever : secOf(td));
        addRecord(r);
    }
}

static void endObject(World &w, uint32_t ref, double t, bool keepWeighted)
{
    const int32_t sec = w.secOf(t);
    if (ref & kPoolBit)
    {
        PoolRec &r = w.pool[ref & ~kPoolBit];
        if (keepWeighted && r.weight > 1.5f)
            return; // a representative record: one of its many members was lost
        if (sec < r.deathSec)
        {
            r.deathSec = std::max(sec, r.bornSec);
            w.poolDeaths.push_back({ref & ~kPoolBit, r.deathSec});
        }
    }
    else if (sec < w.rosterEnd[ref])
    {
        w.rosterEnd[ref] = sec;
        w.rosterDeaths.push_back({ref, sec});
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
    ev.firstRec = (uint32_t)pool.size();
    const double fill = (double)pool.size() / std::max<double>(s.poolCapacity, 1.0);
    // Representative records: up to maxRecordsPerEvent at an empty pool, (1 - fill)^3 of it as the pool
    // fills, at least one while there is room (each record then stands for more real fragments).
    const double room = 1.0 - fill;
    const int maxRec = fill >= 1.0 ? 0
                                   : std::clamp((int)std::ceil(s.maxRecordsPerEvent * room * room * room), 1,
                                                std::max(s.maxRecordsPerEvent, 1));
    switch (ev.kind)
    {
    case EV_KILL:
        if (!alive(ev.a, ev.t))
            return false;
        killAsCorpse(ev.a, ev.t, idx);
        break;
    case EV_BUMP:
        // a: the dead member drifting in; b: its neighbour, killed when the seed said so (catastrophic).
        if (!alive(ev.b, ev.t))
            return false;
        if (ev.catastrophic)
            killAsCorpse(ev.b, ev.t, idx);
        break;
    case EV_COLLISION:
    {
        if (!alive(ev.a, ev.t) || !alive(ev.b, ev.t))
            return false;
        uint32_t tgt = ev.a, prj = ev.b;
        if (massOf(*this, prj) > massOf(*this, tgt))
            std::swap(tgt, prj);
        const State st = stateOf(tgt, ev.t), sp = stateOf(prj, ev.t);
        const double v = glm::length(st.v - sp.v);
        const BreakupResult br = breakupCollision(massOf(*this, tgt), massOf(*this, prj), v, s.LcMin, maxRec, ev.seed);
        ev.vRel = (float)v;
        ev.nReal = (float)br.nReal;
        ev.catastrophic = br.catastrophic ? 1 : 0;
        spawnFragments(br, tgt, prj, st, sp, ev.t, idx, ev.seed);
        endObject(*this, prj, ev.t, true);
        if (br.catastrophic)
            endObject(*this, tgt, ev.t, true);
        else if (!(tgt & kPoolBit) && hash01(ev.seed, 7, 41) < 0.5)
            killAsCorpse(tgt, ev.t, idx); // damaged: the target dies and tumbles (its fragments came off it)
        break;
    }
    case EV_EXPLOSION:
    case EV_ASAT:
    {
        if (!alive(ev.a, ev.t))
            return false;
        const State st = stateOf(ev.a, ev.t);
        const double m = massOf(*this, ev.a);
        const BreakupResult br = ev.kind == EV_ASAT ? breakupCollision(m, 20.0, 9000.0, s.LcMin, maxRec, ev.seed)
                                                    : breakupExplosion(m, s.LcMin, maxRec, ev.seed);
        ev.vRel = ev.kind == EV_ASAT ? 9000.0f : 0.0f;
        ev.nReal = (float)br.nReal;
        ev.catastrophic = br.catastrophic ? 1 : 0;
        spawnFragments(br, ev.a, kNoRef, st, st, ev.t, idx, ev.seed);
        endObject(*this, ev.a, ev.t, false);
        break;
    }
    default:
        return false;
    }
    ev.nRec = (uint32_t)pool.size() - ev.firstRec;
    events.push_back(ev);
    return true;
}

void World::simulate(double tEnd, std::vector<Event> &pending, size_t &pendingNext, const bool *stop)
{
    std::vector<uint32_t> refs;
    std::vector<EngineObject> objs;
    while (frontier < tEnd && !(stop && *stop))
    {
        const double t0 = frontier, t1 = std::min(frontier + s.stepS, tEnd);
        while (pendingNext < pending.size() && pending[pendingNext].t <= t1)
            apply(pending[pendingNext++]);
        if (t1 > s.epoch)
        {
            const double ts = 0.5 * (t0 + t1);
            const int32_t sec = secOf(ts);
            refs.clear();
            for (uint32_t i = 0; i < (uint32_t)roster.size(); ++i)
            {
                if (sec >= rosterEnd[i])
                    continue;
                if (!types[roster[i].type].passive && hash01(sampleCount, i, 21) >= s.activeSample)
                    continue;
                refs.push_back(i);
            }
            for (uint32_t j = 0; j < (uint32_t)pool.size(); ++j)
                if (sec >= pool[j].bornSec && sec < pool[j].deathSec)
                    refs.push_back(j | kPoolBit);
            objs.resize(refs.size());
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
                        o.active = !ty.passive;
                        o.weight = o.active ? (float)(1.0 / s.activeSample) : 1.0f;
                    }
                }
            });
            std::vector<EngineCollision> col = engineSample(objs, t1 - t0, s.engine, mix64(0xC0111DEull ^ sampleCount));
            std::sort(col.begin(), col.end(), [](const EngineCollision &x, const EngineCollision &y) {
                return x.a != y.a ? x.a < y.a : x.b < y.b;
            });
            for (const EngineCollision &c : col)
            {
                Event ev;
                ev.t = ts;
                ev.a = c.a;
                ev.b = c.b;
                ev.kind = EV_COLLISION;
                ev.origin = ORIGIN_ENGINE;
                ev.seed = (uint32_t)mix64(((uint64_t)c.a << 32 | c.b) ^ (sampleCount * 0x9E3779B97F4A7C15ull));
                apply(ev);
            }
            ++sampleCount;
        }
        frontier = t1;
    }
}

std::vector<Event> stormSeedEvents(const World &w, double arriveT, uint64_t seed)
{
    std::vector<Event> out;
    for (uint32_t i = 0; i < (uint32_t)w.roster.size(); ++i)
    {
        const RosterObj &o = w.roster[i];
        const TypeInfo &ty = w.types[o.type];
        if (ty.passive)
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
