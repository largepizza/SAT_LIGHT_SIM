// Meteors.cpp — meteor showers and the sporadic background (2026-10-27; Meteors.h, CLAUDE.md "Meteors").
#include "Meteors.h"
#include "SatPhotometry.h"
#include "Comets.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>

namespace meteors
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kR = 6371000.0;
constexpr double kEps = 23.4392911 * kPi / 180.0; // the obliquity (J2000)
double rad(double d) { return d * (kPi / 180.0); }
double deg(double r) { return r * (180.0 / kPi); }
double wrap180(double a) { return std::remainder(a, 360.0); }
double wrap360(double a)
{
    a = std::fmod(a, 360.0);
    return a < 0.0 ? a + 360.0 : a;
}

// The IMO working list (Rendtel, IMO 2026 calendar; Jenniskens 1994 for the profile slopes), J2000. The bright-end
// index rb is ours: the visual r describes the meteors observers count (mag ~1-5); fireball networks see a shallower
// bright end, and the Taurids and Perseids are fireball-rich.
//                id            name                   code   lam0    ZHR    B-     B+    ZHR2  B2     r    rb    v    RA     Dec   dRA   dDec  spread
const Shower kShowers[] = {
    {"quadrantids", "Quadrantids", "QUA", 283.15, 110.0, 2.5, 2.5, 0.0, 0.0, 2.1, 1.85, 41.0, 230.1, 48.5, 0.8, -0.2, 0.8},
    {"lyrids", "Lyrids", "LYR", 32.32, 18.0, 0.35, 0.35, 0.0, 0.0, 2.1, 1.8, 49.0, 271.4, 33.6, 1.1, 0.0, 0.8},
    {"eta_aquariids", "Eta Aquariids", "ETA", 45.5, 50.0, 0.08, 0.08, 0.0, 0.0, 2.4, 2.0, 66.0, 338.0, -1.0, 0.9, 0.4, 1.0},
    {"delta_aquariids", "Delta Aquariids", "SDA", 127.0, 25.0, 0.091, 0.091, 0.0, 0.0, 2.5, 2.1, 41.0, 340.0, -16.0, 0.8, 0.2, 1.2},
    {"perseids", "Perseids", "PER", 140.0, 80.0, 0.20, 0.20, 22.0, 0.05, 2.2, 1.7, 59.0, 48.0, 58.0, 1.35, 0.12, 0.8},
    {"draconids", "Draconids", "DRA", 195.4, 10.0, 2.5, 2.5, 0.0, 0.0, 2.6, 2.0, 20.0, 262.0, 54.0, 0.0, 0.0, 0.8},
    {"orionids", "Orionids", "ORI", 208.0, 20.0, 0.12, 0.12, 0.0, 0.0, 2.5, 2.0, 66.0, 95.0, 16.0, 0.7, 0.1, 1.0},
    {"s_taurids", "Southern Taurids", "STA", 197.0, 5.0, 0.026, 0.026, 0.0, 0.0, 2.3, 1.35, 27.0, 32.0, 9.0, 0.8, 0.3, 2.0},
    {"n_taurids", "Northern Taurids", "NTA", 230.0, 5.0, 0.026, 0.026, 0.0, 0.0, 2.3, 1.35, 29.0, 58.0, 22.0, 0.8, 0.2, 2.0},
    {"leonids", "Leonids", "LEO", 235.27, 15.0, 0.39, 0.39, 0.0, 0.0, 2.5, 1.8, 71.0, 152.0, 22.0, 0.7, -0.4, 0.8},
    {"geminids", "Geminids", "GEM", 262.2, 150.0, 0.39, 0.81, 0.0, 0.0, 2.6, 1.8, 35.0, 112.0, 33.0, 1.0, -0.15, 0.8},
    {"ursids", "Ursids", "URS", 270.7, 10.0, 0.6, 0.6, 0.0, 0.0, 3.0, 2.2, 33.0, 217.0, 76.0, 0.0, 0.0, 0.8},
    // The Great Comet of 2037's debris (assumptions in cometStorm() below): a spike of 8000 + a 400 residual at the peak,
    // fireball-rich (the comet is a large sungrazer shedding big grains), entry speed and radiant from the orbit geometry
    // (speedKmsAtTime / radiantEciAtTime; the vKms, ra, dec here are nominal values only).
    {"comet_2037", "C/2036 X1 meteors", "CX1", 0.0, 8000.0, 0.0, 0.0, 400.0, 0.0, 2.2, 1.5, 70.0, 0.0, 0.0, 0.0, 0.0, 1.0},
    {"sporadic", "Sporadic", "SPO", 0.0, 10.0, 0.0, 0.0, 0.0, 0.0, 3.0, 2.0, 40.0, 0.0, 0.0, 0.0, 0.0, 15.0},
};
static_assert(sizeof(kShowers) / sizeof(kShowers[0]) == kSporadic + 1, "kSporadic is the last row");
static_assert(kGreatComet == kSporadic - 1, "the comet row sits just before the sporadic one");

uint64_t mix64(uint64_t z)
{
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
double u01(uint64_t seed, uint64_t k) { return (double)(mix64(seed ^ mix64(k + 0x51ED27ull)) >> 11) * (1.0 / 9007199254740992.0); }
double gauss(uint64_t seed, uint64_t k)
{
    const double a = std::max(u01(seed, k), 1e-12), b = u01(seed, k + 1000003ull);
    return std::sqrt(-2.0 * std::log(a)) * std::cos(2.0 * kPi * b);
}
int poisson(double lam, double u)
{
    if (!(lam > 1e-12)) return 0;
    if (u < 1.0 - lam - 1e-12) return 0; // exact: exp(-lam) >= 1 - lam (the common case, no exp)
    double p = std::exp(-lam), F = p;
    int k = 0;
    while (u > F && k < 200)
    {
        ++k;
        p *= lam / k;
        F += p;
    }
    return k;
}
glm::dvec3 eclToEci(double lamDeg, double betDeg)
{
    const double l = rad(lamDeg), b = rad(betDeg);
    const double x = std::cos(b) * std::cos(l), y = std::cos(b) * std::sin(l), z = std::sin(b);
    return {x, y * std::cos(kEps) - z * std::sin(kEps), y * std::sin(kEps) + z * std::cos(kEps)};
}
glm::dvec3 eciToEcef(const glm::dvec3 &v, double t)
{
    const double th = earthRotationAngle(t), c = std::cos(th), s = std::sin(th);
    return {c * v.x + s * v.y, -s * v.x + c * v.y, v.z};
}
// A unit vector turned from d by a 2D Gaussian of sigma sDeg.
glm::dvec3 scatter(const glm::dvec3 &d, double sDeg, uint64_t seed, uint64_t k)
{
    glm::dvec3 a = std::abs(d.z) < 0.9 ? glm::dvec3(0, 0, 1) : glm::dvec3(1, 0, 0);
    const glm::dvec3 e1 = glm::normalize(glm::cross(d, a)), e2 = glm::cross(d, e1);
    const double gx = rad(sDeg) * gauss(seed, k), gy = rad(sDeg) * gauss(seed, k + 7);
    return glm::normalize(d + e1 * gx + e2 * gy);
}
} // namespace

int showerCount() { return kSporadic + 1; }
const Shower &shower(int i) { return kShowers[std::clamp(i, 0, kSporadic)]; }
int showerByName(const char *s)
{
    if (!s || !*s) return -1;
    auto eq = [](const char *a, const char *b) {
        for (; *a && *b; ++a, ++b)
            if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b)) return false;
        return *a == *b;
    };
    for (int i = 0; i <= kSporadic; ++i)
        if (eq(s, kShowers[i].id) || eq(s, kShowers[i].name) || eq(s, kShowers[i].code)) return i;
    // a prefix ("perse", "gem")
    const size_t n = std::strlen(s);
    for (int i = 0; i <= kSporadic; ++i)
    {
        bool ok = n >= 3;
        for (size_t c = 0; ok && c < n; ++c)
            ok = kShowers[i].id[c] && std::tolower((unsigned char)s[c]) == kShowers[i].id[c];
        if (ok) return i;
    }
    return -1;
}

double solarLongitudeDeg(const glm::dvec3 &sd)
{
    const double ye = sd.y * std::cos(kEps) + sd.z * std::sin(kEps);
    return wrap360(deg(std::atan2(ye, sd.x)));
}

double zhrAt(int s, double lam)
{
    const Shower &S = shower(s);
    if (s == kSporadic) return S.zhr;
    if (s == kGreatComet) return 0.0; // (a function of time only: zhrAtTime)
    const double d = wrap180(lam - S.lam0);
    double z = S.zhr * std::pow(10.0, -(d < 0.0 ? S.bMinus : S.bPlus) * std::abs(d));
    if (S.zhr2 > 0.0) z += S.zhr2 * std::pow(10.0, -S.b2 * std::abs(d));
    return z < 0.2 ? 0.0 : z;
}

glm::dvec3 radiantEci(int s, double lam)
{
    const Shower &S = shower(s);
    const double d = wrap180(lam - S.lam0);
    const double ra = rad(S.ra + S.dRa * d), de = rad(std::clamp(S.dec + S.dDec * d, -89.0, 89.0));
    return {std::cos(de) * std::cos(ra), std::cos(de) * std::sin(ra), std::sin(de)};
}

namespace
{
constexpr double kDayS = 86400.0;
constexpr double kAuKmS = 1.495978707e8 / 86400.0; // AU/day -> km/s
// The comet storm's log10 slopes per day before / after its peak (cometStorm()), and the days before the peak where the
// total (spike + residual, 8400 at the peak) passes ZHR 10.
constexpr double kStormRiseB = 5.0, kStormFallB = 4.0;
const double kStormLeadD = std::log10(8400.0 / 10.0) / kStormRiseB;
glm::dvec3 eclVecToEci(const glm::dvec3 &v) { return {v.x, v.y * std::cos(kEps) - v.z * std::sin(kEps), v.y * std::sin(kEps) + v.z * std::cos(kEps)}; }
// Earth's heliocentric velocity (km/s, ECI/J2000 equatorial axes) from the sim's own Sun: a central difference of -Sun x distance.
glm::dvec3 earthVelKms(double t)
{
    auto pos = [](double tt) { return -glm::normalize(sunDirEciAt(tt)) * sunDistAuAt(tt) * 1.495978707e8; }; // km
    const double h = 1800.0;
    return (pos(t + h) - pos(t - h)) / (2.0 * h);
}
// The stream's heliocentric velocity where it crosses the Earth's path: the comet's at its closest approach (km/s, ECI axes).
glm::dvec3 streamVelKms()
{
    static const glm::dvec3 v = eclVecToEci(comet::stateAt(comet::greatComet(), 1174145400.0).v * kAuKmS);
    return v;
}
glm::dvec3 relVelKms(double t) { return streamVelKms() - earthVelKms(t); } // the meteoroids' velocity relative to the Earth
} // namespace

const CometStorm &cometStorm()
{
    // Assumptions (all of them ours; no real comet to measure). The meteoroids are the big grains the comet shed round its
    // sungrazing perihelion (2037-02-17). Flown from the comet's own orbit (a scratch Monte Carlo, 2026-10-30: grains emitted
    // from 3 d before perihelion to the encounter, beta 1e-5..3e-3, the tails' ejection law, Kepler under mu (1 - beta)),
    // they are still a compact cloud round the nucleus a month later, a few million km across, and the Earth's path meets it
    // only round its closest approach to the comet (2037-03-17 03:30 UTC, 560,000 km; tools/comet_design.py): grains within
    // 3e6 km of the Earth from 03-16 ~06:00 to 03-17 ~19:00, within 1e6 km from 03-16 ~21:00 to 03-17 ~08:00, the nearest
    // (280,000 km) at 02:30. Before that the Earth is tens of millions of km from any of it (and from the comet's orbit:
    // 23 million km on 03-08), so nothing comes earlier. The storm peaks at the closest approach: ZHR 8000 falling as
    // 10^(-5 |d|) before it (half-maximum 1.4 h before, ZHR 10 ~14 h before) and 10^(-4 d) after (the trailing side is a
    // little wider). Under it a residual (ZHR 400 at the peak) rising with the spike, then decaying with an e-folding of
    // 75 d, so the shower "spikes up and then gradually slows through 2037" (an artistic choice kept from the first design:
    // physically the Earth leaves the cloud at ~1.2 million km a day). t0 / t1 = where the total passes 10 (sporadic rate).
    static const CometStorm s = {1174145400.0, 1174145400.0 - kStormLeadD * kDayS, 1174145400.0 + 280.0 * kDayS, 8400.0};
    return s;
}
static double cometZhr(double t)
{
    const double d = (t - cometStorm().tPeak) / kDayS;
    const double core = 8000.0 * std::pow(10.0, d < 0.0 ? kStormRiseB * d : -kStormFallB * d);
    const double res = d < 0.0 ? 400.0 * std::pow(10.0, kStormRiseB * d) : 400.0 * std::exp(-d / 75.0);
    const double z = core + res;
    return z < 0.2 ? 0.0 : z;
}
double zhrAtTime(int s, double t) { return s == kGreatComet ? cometZhr(t) : zhrAt(s, solarLongitudeDeg(sunDirEciAt(t))); }
glm::dvec3 radiantEciAtTime(int s, double t)
{
    // The radiant is where the meteoroids come FROM: opposite their velocity relative to the Earth (zenith attraction left out).
    if (s == kGreatComet) return glm::normalize(-relVelKms(t));
    return radiantEci(s, solarLongitudeDeg(sunDirEciAt(t)));
}
double speedKmsAtTime(int s, double t) { return s == kGreatComet ? glm::length(relVelKms(t)) : shower(s).vKms; }

double fluxPerKm2h(double zhr, double r)
{
    r = std::max(r, 1.35);
    return zhr * (13.1 * r - 16.5) * std::pow(r - 1.3, 0.748) / 37200.0;
}

double magFrac(double M, double r, double rb)
{
    if (M >= 2.0) return std::pow(r, M - 6.5);
    return std::pow(r, 2.0 - 6.5) * std::pow(rb, M - 2.0);
}
namespace
{
double magInv(double F, double r, double rb)
{
    const double F2 = std::pow(r, 2.0 - 6.5);
    if (F >= F2) return 6.5 + std::log(F) / std::log(r);
    return 2.0 + (std::log(F) - std::log(F2)) / std::log(rb);
}
} // namespace

double peakAfter(int s, double t, double *t0, double *t1)
{
    if (s == kGreatComet)
    {
        const CometStorm &c = cometStorm();
        if (t0) *t0 = c.t0;
        if (t1) *t1 = c.t1;
        return c.tPeak;
    }
    const Shower &S = shower(s);
    const double rate = 0.98565 / 86400.0; // deg per s
    double tp = t + wrap360(S.lam0 - solarLongitudeDeg(sunDirEciAt(t))) / rate;
    for (int i = 0; i < 4; ++i) tp += wrap180(S.lam0 - solarLongitudeDeg(sunDirEciAt(tp))) / rate;
    const double hm = std::log10(2.0);
    if (t0) *t0 = tp - (S.bMinus > 0.0 ? hm / S.bMinus : 1.0) / rate;
    if (t1) *t1 = tp + (S.bPlus > 0.0 ? hm / S.bPlus : 1.0) / rate;
    return tp;
}

bool buildPath(Meteor &m, const glm::dvec3 &endUp, const glm::dvec3 &rad_, double vMs, double M)
{
    const glm::dvec3 up = glm::normalize(endUp), rd = glm::normalize(rad_);
    const double sh = glm::dot(rd, up);
    if (sh <= 0.01) return false;
    const double vk = vMs / 1000.0;
    m.hB = (80.0 + 0.55 * vk) * 1000.0;
    m.hE = std::clamp(65.0 + 0.35 * vk + 2.5 * M, 20.0, m.hB / 1000.0 - 8.0) * 1000.0;
    m.E = up * (kR + m.hE);
    const double b = glm::dot(m.E, rd), c = glm::dot(m.E, m.E) - (kR + m.hB) * (kR + m.hB);
    double L = -b + std::sqrt(std::max(b * b - c, 0.0));
    L = std::min(L, 250000.0);
    m.B = m.E + rd * L;
    m.radiant = rd;
    m.vMs = vMs;
    m.M = (float)M;
    m.dur = L / vMs;
    m.trainLife = M <= -5.5 ? (float)(2.5 * std::pow(10.0, 0.2 * (-5.5 - M)) * (vk > 45.0 ? 2.0 : 1.0)) : 0.0f;
    return true;
}

int generate(const GenParams &p, const std::function<void(const Meteor &)> &cb)
{
    int draws = 0;
    const double eyeR = glm::length(p.eyeEcef);
    const glm::dvec3 eyeU = p.eyeEcef / std::max(eyeR, 1.0);
    const double hEye = std::max(eyeR - kR, 0.0) + std::max(p.eyeMarginM, 0.0);
    // The ground arc to the farthest meteor the eye can see (a meteor 120 km up on the eye's horizon), from any eye
    // within eyeMarginM of eyeEcef (its height + the margin; the margin again sideways).
    const double reachArc = std::acos(kR / (kR + hEye)) + std::acos(kR / (kR + 120000.0)) + std::max(p.eyeMarginM, 0.0) / kR;
    const double eyeLat = deg(std::asin(std::clamp(eyeU.z, -1.0, 1.0))), eyeLon = deg(std::atan2(eyeU.y, eyeU.x));
    for (int s = 0; s <= kSporadic; ++s)
    {
        if (p.onlyShower >= 0 && s != p.onlyShower) continue;
        const Shower &S = kShowers[s];
        for (int b = 0; b < kBands; ++b)
        {
            if (p.onlyBand >= 0 && b != p.onlyBand) continue;
            const double Mlo = kBandEdge[b], Mhi = kBandEdge[b + 1], T = kBandBinS[b];
            // Too faint to be seen past this distance (5 mag per decade of distance from 100 km, +1 mag margin).
            const double dMax = std::min(reachArc * kR, 1.0e5 * std::pow(10.0, (p.cullMag + 1.0 - Mlo) / 5.0) + std::max(p.eyeMarginM, 0.0));
            if (dMax < 50000.0) continue;
            const double arcMax = dMax / kR;
            const double cosReach = std::cos(std::min(arcMax + rad(1.0), kPi));
            double tA = p.tA, tB = p.tB;
            if (p.maxAgeOnly > 0.0) tA = p.tB - kBandMaxAgeS[b];
            const int64_t k0 = (int64_t)std::floor(tA / T), k1 = (int64_t)std::floor((tB - 1e-6) / T);
            double frac = magFrac(Mhi, S.r, S.rb) - magFrac(Mlo, S.r, S.rb);
            if (Mhi <= -4.0 + 1e-9) frac *= p.fireballGain;
            const double F_lo = magFrac(Mlo, S.r, S.rb), F_hi = magFrac(Mhi, S.r, S.rb);
            // The cells within reach (the same for every bin of the call: built once; 2026-10-28, time-lapse calls span
            // many bins). Same cells, same order, same arithmetic as per bin.
            struct Cell
            {
                int j, i;
                double la1, la2, lac, dLon, area;
                glm::dvec3 cu;
                uint64_t cellMix;
            };
            std::vector<Cell> cells;
            {
                const int j0 = std::max(0, (int)std::floor(eyeLat - deg(arcMax) + 90.0) - 1);
                const int j1 = std::min(179, (int)std::floor(eyeLat + deg(arcMax) + 90.0) + 1);
                for (int j = j0; j <= j1; ++j)
                {
                    const double la1 = j - 90.0, la2 = la1 + 1.0, lac = la1 + 0.5;
                    const int nLon = std::max(1, (int)std::lround(360.0 * std::cos(rad(lac))));
                    const double dLon = 360.0 / nLon;
                    const double area = kR * kR * rad(dLon) * (std::sin(rad(la2)) - std::sin(rad(la1))) * 1e-6; // km^2
                    const double cl = std::cos(rad(lac));
                    int i0 = 0, i1 = nLon - 1;
                    if (cl > 1e-3 && std::abs(lac) < 88.0)
                    {
                        const double halfLon = std::min(180.0, deg(arcMax) / std::max(cl, 0.05) + dLon);
                        if (halfLon < 180.0)
                        {
                            i0 = (int)std::floor((eyeLon - halfLon + 180.0) / dLon);
                            i1 = (int)std::floor((eyeLon + halfLon + 180.0) / dLon);
                            if (i1 - i0 + 1 >= nLon)
                            {
                                i0 = 0;
                                i1 = nLon - 1;
                            }
                        }
                    }
                    for (int ii = i0; ii <= i1; ++ii)
                    {
                        const int i = ((ii % nLon) + nLon) % nLon;
                        const double loc = -180.0 + (i + 0.5) * dLon;
                        const glm::dvec3 cu(cl * std::cos(rad(loc)), cl * std::sin(rad(loc)), std::sin(rad(lac)));
                        if (glm::dot(cu, eyeU) < cosReach) continue;
                        cells.push_back({j, i, la1, la2, lac, dLon, area, cu, mix64((uint64_t)j * 4096ull + (uint64_t)i)});
                    }
                }
            }
            const uint64_t sbMix = mix64((uint64_t)(s == kSporadic ? 12 : s) * 0x1000193ull + (uint64_t)b); // (the sporadic row's seed index stays 12)
            for (int64_t k = k0; k <= k1; ++k)
            {
                const double tm = ((double)k + 0.5) * T;
                const glm::dvec3 sunE = sunDirEciAt(tm);
                const double lam = solarLongitudeDeg(sunE);
                const double zhr = s == kGreatComet ? cometZhr(tm) : zhrAt(s, lam);
                if (zhr <= 0.0) continue;
                const double Q = fluxPerKm2h(zhr, S.r) * p.rateGain;
                glm::dvec3 radE(0.0);
                double vMean = S.vKms;
                if (s == kGreatComet)
                {
                    vMean = speedKmsAtTime(s, tm);
                    radE = eciToEcef(radiantEciAtTime(s, tm), tm);
                }
                else if (s != kSporadic) radE = eciToEcef(radiantEci(s, lam), tm);
                const uint64_t kMix = mix64((uint64_t)k * 0x9E3779B97F4A7C15ull);
                for (const Cell &cell : cells)
                {
                    const int j = cell.j, i = cell.i;
                    const double la1 = cell.la1, la2 = cell.la2, dLon = cell.dLon;
                    const glm::dvec3 &cu = cell.cu;
                    const double lam0 = Q * cell.area * (T / 3600.0) * frac;
                    if (lam0 <= 0.0) continue;
                    {
                        double lamC;
                        if (s == kSporadic)
                            lamC = lam0 * 4.0; // accepted per meteor by its radiant's elevation (mean 1/4)
                        else
                        {
                            const double sh = glm::dot(radE, cu);
                            if (sh < -0.03) continue;
                            lamC = lam0 * std::max(sh + 0.01, 0.0);
                        }
                        (void)j;
                        const uint64_t seed = mix64(sbMix ^ cell.cellMix ^ kMix);
                        ++draws;
                        const int n = poisson(lamC, u01(seed, 0));
                        for (int q = 0; q < n; ++q)
                        {
                            const uint64_t ms = mix64(seed ^ (0xABCDull + (uint64_t)q * 0x632BE59BD9B4E019ull));
                            Meteor m;
                            m.shower = s;
                            m.band = b;
                            m.seed = ms;
                            m.tStart = ((double)k + u01(ms, 1)) * T;
                            m.bin = k;
                            if (m.tStart < tA || m.tStart >= tB) continue;
                            const double sl = std::sin(rad(la1)) + u01(ms, 2) * (std::sin(rad(la2)) - std::sin(rad(la1)));
                            const double lo = rad(-180.0 + (i + u01(ms, 3)) * dLon);
                            const double cla = std::sqrt(std::max(0.0, 1.0 - sl * sl));
                            const glm::dvec3 up(cla * std::cos(lo), cla * std::sin(lo), sl);
                            const double M = magInv(F_lo + u01(ms, 4) * (F_hi - F_lo), S.r, S.rb);
                            double v = vMean * (1.0 + 0.03 * gauss(ms, 5));
                            glm::dvec3 rd;
                            if (s == kSporadic)
                            {
                                // The sporadic sources, in Sun-centred ecliptic longitude: the apex (270), helion (340)
                                // and antihelion (200); fast from the apex, slow from the others.
                                const double lamS = lam; // (the bin's: the Sun moves < 0.002 deg in a bin; per meteor it was most of the cost)
                                const double w = u01(ms, 6);
                                double dl = 270.0, sp = 18.0;
                                v = 58.0 + 6.0 * gauss(ms, 7);
                                if (w > 0.45)
                                {
                                    dl = w > 0.72 ? 340.0 : 200.0;
                                    sp = 14.0;
                                    v = 28.0 + 6.0 * gauss(ms, 7);
                                }
                                v = std::clamp(v, 12.0, 72.0);
                                const glm::dvec3 src = eclToEci(lamS + dl + sp * gauss(ms, 8), sp * gauss(ms, 9));
                                rd = eciToEcef(glm::normalize(src), m.tStart);
                                if (u01(ms, 10) > glm::dot(rd, up)) continue;
                            }
                            else
                                rd = scatter(radE, S.radiantSpreadDeg, ms, 11);
                            if (!buildPath(m, up, rd, v * 1000.0, M)) continue;
                            if (b >= 2 && m.dur > kBandMaxAgeS[b] - T) continue;
                            cb(m);
                        }
                    }
                }
            }
        }
    }
    return draws;
}

double lightCurve(const Meteor &m, double x)
{
    if (x <= 0.0 || x >= 1.0) return 0.0;
    double I = 6.75 * x * x * (1.0 - x);
    int nf = 0;
    if (m.M <= -1.0) nf = 1 + (int)(u01(m.seed, 20) * 3.0);
    else if (m.M <= 1.5 && u01(m.seed, 21) < 0.3) nf = 1;
    for (int f = 0; f < nf; ++f)
    {
        const double xf = 0.45 + 0.47 * u01(m.seed, 22 + f), w = 0.018 + 0.02 * u01(m.seed, 30 + f);
        const double a = 0.8 + 2.2 * u01(m.seed, 40 + f);
        I += a * std::exp(-0.5 * ((x - xf) / w) * ((x - xf) / w));
    }
    if (m.M <= -8.0) // the terminal burst
    {
        const double d = (x - 0.93) / 0.025;
        I += (3.0 + 3.0 * u01(m.seed, 50)) * std::exp(-0.5 * d * d);
    }
    return I;
}

double lightIntegral(const Meteor &m, double x0, double x1)
{
    x0 = std::clamp(x0, 0.0, 1.0);
    x1 = std::clamp(x1, 0.0, 1.0);
    if (x1 <= x0) return 0.0;
    auto P = [](double x) { return 6.75 * (x * x * x / 3.0 - x * x * x * x / 4.0); };
    double J = P(x1) - P(x0);
    auto gaussInt = [&](double a, double xf, double w) {
        const double k = 1.0 / (w * std::sqrt(2.0));
        return a * w * std::sqrt(kPi / 2.0) * (std::erf((x1 - xf) * k) - std::erf((x0 - xf) * k));
    };
    int nf = 0; // (lightCurve's flares, same hashes)
    if (m.M <= -1.0) nf = 1 + (int)(u01(m.seed, 20) * 3.0);
    else if (m.M <= 1.5 && u01(m.seed, 21) < 0.3) nf = 1;
    for (int f = 0; f < nf; ++f)
        J += gaussInt(0.8 + 2.2 * u01(m.seed, 40 + f), 0.45 + 0.47 * u01(m.seed, 22 + f), 0.018 + 0.02 * u01(m.seed, 30 + f));
    if (m.M <= -8.0) J += gaussInt(3.0 + 3.0 * u01(m.seed, 50), 0.93, 0.025);
    return std::max(J, 0.0);
}

int fragmentCount(const Meteor &m) { return m.M <= -4.0 ? 2 + (int)(u01(m.seed, 60) * 4.0) : 0; }

bool fragmentAt(const Meteor &m, int k, double tau, glm::dvec3 &off, double &rel)
{
    const double xf = 0.45 + 0.47 * u01(m.seed, 22); // the first flare (lightCurve's)
    const double tf = xf * m.dur, dt = tau - tf;
    if (dt <= 0.0) return false;
    const double life = (m.dur - tf) * (0.5 + 0.6 * u01(m.seed, 70 + k)) + 0.2;
    if (dt >= life) return false;
    const glm::dvec3 dir = glm::normalize(m.E - m.B);
    glm::dvec3 a = std::abs(dir.z) < 0.9 ? glm::dvec3(0, 0, 1) : glm::dvec3(1, 0, 0);
    const glm::dvec3 e1 = glm::normalize(glm::cross(dir, a)), e2 = glm::cross(dir, e1);
    const double ang = 2.0 * kPi * u01(m.seed, 80 + k);
    const glm::dvec3 perp = e1 * std::cos(ang) + e2 * std::sin(ang);
    const double vl = 40.0 + 220.0 * u01(m.seed, 90 + k), acc = 1500.0 + 6000.0 * u01(m.seed, 100 + k);
    off = perp * (vl * dt) - dir * (0.5 * acc * dt * dt);
    const double sfr = dt / life;
    rel = (0.12 + 0.35 * u01(m.seed, 110 + k)) * std::sqrt(std::max(0.0, 1.0 - sfr)) * (1.0 + 0.3 * std::sin(37.0 * dt + 9.0 * k));
    return rel > 0.0;
}

glm::dvec2 trainWind(const Meteor &m, double hM)
{
    const double w0 = 25.0 + 50.0 * u01(m.seed, 120), th0 = 2.0 * kPi * u01(m.seed, 121);
    const double shear = rad((8.0 + 14.0 * u01(m.seed, 122)) * (u01(m.seed, 123) < 0.5 ? -1.0 : 1.0)); // per km
    const double dh = (hM - 90000.0) / 1000.0;
    const double th = th0 + shear * dh;
    const double w = w0 * (0.7 + 0.5 * std::sin(dh / 6.0 + 6.0 * u01(m.seed, 124)));
    return {w * std::sin(th), w * std::cos(th)};
}

glm::vec3 color(const Meteor &m, double rel)
{
    const double vk = m.vMs / 1000.0;
    glm::dvec3 c(0.93, 0.95, 1.0);
    const double Mnow = m.M - 2.5 * std::log10(std::max(rel, 1e-6));
    // Fast and bright: the green of Mg / Fe / O; slow: sodium's orange-yellow.
    const double g = glm::smoothstep(42.0, 62.0, vk) * glm::smoothstep(1.0, -5.0, Mnow) * 0.6;
    c = glm::mix(c, glm::dvec3(0.5, 1.0, 0.6), g);
    const double o = glm::smoothstep(34.0, 18.0, vk) * (0.35 + 0.5 * glm::smoothstep(2.0, -4.0, Mnow));
    c = glm::mix(c, glm::dvec3(1.0, 0.72, 0.36), o);
    if (rel > 1.15) c = glm::mix(c, glm::dvec3(1.0), std::min(0.6, (rel - 1.15) * 0.5)); // flares burn white
    const double mx = std::max(c.x, std::max(c.y, c.z));
    return glm::vec3(c / mx);
}
} // namespace meteors
