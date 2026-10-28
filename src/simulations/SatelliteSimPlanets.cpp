// ─── Resolved planets and Jupiter's Galilean moons (2026-10-27) ────────────────────────────────────────────────────
// CLAUDE.md "Planets" (the resolved discs). A planet is a point of the star pipeline until its true equatorial angular
// radius passes ~1 px (the camera zooms to a 0.5-deg field: Jupiter ~12 px, Venus near inferior conjunction ~17 px,
// Saturn's rings ~12 px, Mars at opposition ~6 px); from 1 to 2.5 px it hands over to a disc drawn by planet_disc.vert /
// .frag (its own pipeline and set: sat_sky.frag is at the sampled-image floor), the star pipeline skipping its record
// (angularSize < 0). The disc:
//  - its light is the planet's own magnitude (Saturn: Meeus's formula with the rings' term) spread by its photometric
//    law over the disc (the CPU integral below), in sky units (pi x radiance / solar irradiance), x the Moon disc's
//    calibration and "Planet disc gain", through the sky's exposure and tonemap; extinction as the point, reddened;
//  - while it is small (2.5 -> 8 px) its light is scaled so the disc's displayed light equals the point's (the point
//    style is not linear), relaxing geometrically to the physical surface brightness: no step at either end;
//  - orientation: the IAU pole and prime meridian (WGCCRE 2009, light-time corrected); Jupiter: Meeus's System II
//    central meridian, the map's Great Red Spot put at its System II longitude extrapolated from the 2020s drift;
//  - maps: tools/make_planet_maps.py (Mars, Jupiter, Saturn; Saturn's ring profile); Mercury, Venus, Uranus are flat.
// The Galilean moons: Meeus ch. 44 (low accuracy, light time included), placed in Jupiter's frame (its IAU pole, the
// Earth's jovicentric direction), eclipsed by Jupiter's shadow, hidden behind its disc, dimmed in transit over a
// resolved disc; their shadows on Jupiter are drawn by the disc shader.
#include "../FrameProfile.h"
#include "SatelliteSim.h"
#include "SatPhotometry.h"
#include "../Log.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kAuKm = 1.495978707e8;
constexpr float kMoonDiscCal = 1.35f; // the sim's Moon disc is ~1.35x physical (pi L / E): the planets match it

struct BodyConst
{
    double reqKm, rpKm;
    double a0, a0d, d0, d0d; // IAU pole RA / Dec (deg) and rates per Julian century
    double W0, Wd;           // prime meridian (deg) and its rate (deg / day)
    int law;                 // 0 Lommel-Seeliger, 1 Minnaert
    float k;                 // Minnaert k
    glm::vec3 tint;          // albedo colour (no map), luminance-normalised below
    int layer;               // map layer, -1 = none
};
// IAU WGCCRE 2009 rotational elements; radii (equatorial / polar) from the same report. Venus: the cloud tops (~70 km up).
const BodyConst kBody[kPlanetCount] = {
    {2440.53, 2438.26, 281.0103, -0.0328, 61.4155, -0.0049, 329.5988, 6.1385108, 0, 1.0f, {0.62f, 0.57f, 0.52f}, -1},
    {6120.0, 6120.0, 272.76, 0.0, 67.16, 0.0, 160.20, -1.4813688, 1, 0.80f, {1.00f, 0.95f, 0.82f}, -1},
    {3396.19, 3376.20, 317.68143, -0.1061, 52.88650, -0.0609, 176.630, 350.89198226, 0, 1.0f, {1, 1, 1}, 0},
    {71492.0, 66854.0, 268.056595, -0.006499, 64.495303, 0.002413, 284.95, 870.5360000, 1, 0.85f, {1, 1, 1}, 1},
    {60268.0, 54364.0, 40.589, -0.036, 83.537, -0.004, 38.90, 810.7939024, 1, 0.90f, {1, 1, 1}, 2},
    {25559.0, 24973.0, 257.311, 0.0, -15.175, 0.0, 203.81, -501.1600928, 1, 0.80f, {0.68f, 0.88f, 0.93f}, -1},
};
const char *const kGalileanNames[kGalileanCount] = {"Io", "Europa", "Ganymede", "Callisto"};
const double kGalileanRkm[kGalileanCount] = {1821.6, 1560.8, 2634.1, 2410.3};
const double kGalileanV10[kGalileanCount] = {-1.68, -1.41, -2.09, -1.05};
const glm::vec3 kGalileanColor[kGalileanCount] = {{1.0f, 0.92f, 0.70f}, {1.0f, 0.98f, 0.92f}, {0.95f, 0.93f, 0.90f}, {0.85f, 0.82f, 0.78f}};

float smoothstepf(float a, float b, float x)
{
    float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
glm::dvec3 raDecVec(double raDeg, double decDeg)
{
    const double a = raDeg * kDeg, d = decDeg * kDeg;
    return {std::cos(d) * std::cos(a), std::cos(d) * std::sin(a), std::sin(d)};
}
// The point's light curve in the sky's tonemap for a grey input (AgX's inset rows sum to 1, so a grey stays grey).
float agxGrey(float x, float contrast)
{
    const float minEv = -12.47393f, maxEv = 4.026069f;
    float v = std::clamp(std::log2(std::max(x, 1e-10f)), minEv, maxEv);
    v = (v - minEv) / (maxEv - minEv);
    float v2 = v * v, v4 = v2 * v2;
    v = 15.5f * v4 * v2 - 40.14f * v4 * v + 31.96f * v4 - 6.868f * v2 * v + 0.4298f * v2 + 0.1191f * v - 0.00232f;
    v = std::pow(std::max(v, 0.0f), contrast);
    return std::pow(std::clamp(v, 0.0f, 1.0f), 2.2f);
}
// The nearest hit (largest z) of the line through (x, y) along z with the spheroid |p|^2 + k (p.P)^2 = 1 (planet_disc.frag).
bool spheroidHitCpu(double x, double y, const glm::dvec3 &P, double k, double &z)
{
    const double s0 = x * P.x + y * P.y;
    const double A = 1.0 + k * P.z * P.z, B = 2.0 * k * P.z * s0, C = x * x + y * y + k * s0 * s0 - 1.0;
    const double disc = B * B - 4.0 * A * C;
    z = (-B + std::sqrt(std::max(disc, 0.0))) / (2.0 * A);
    return disc >= 0.0;
}
double lawCpu(int law, double k, double mu0, double mu)
{
    if (mu0 <= 0.0)
        return 0.0;
    mu = std::max(mu, 1e-3);
    return law == 0 ? 2.0 * mu0 / (mu0 + mu) : std::pow(mu0, k) * std::pow(mu, k - 1.0);
}
// Meeus, Astronomical Algorithms ch. 44, the low-accuracy method: each moon's angle u from the inferior conjunction
// (radians, increasing toward the sky's west on the near side) and distance (Jupiter equatorial radii), and Jupiter's
// System II central meridian (deg). d = days since J2000.0.
void galileanMeeus(double d, double u[4], double r[4], double &omega2)
{
    auto sinD = [](double x) { return std::sin(x * kDeg); };
    auto cosD = [](double x) { return std::cos(x * kDeg); };
    const double V = 172.74 + 0.00111588 * d;
    const double M = 357.529 + 0.9856003 * d;
    const double N = 20.020 + 0.0830853 * d + 0.329 * sinD(V);
    const double J = 66.115 + 0.9025179 * d - 0.329 * sinD(V);
    const double A = 1.915 * sinD(M) + 0.020 * sinD(2 * M);
    const double B = 5.555 * sinD(N) + 0.168 * sinD(2 * N);
    const double K = J + A - B;
    const double R = 1.00014 - 0.01671 * cosD(M) - 0.00014 * cosD(2 * M);
    const double rj = 5.20872 - 0.25208 * cosD(N) - 0.00611 * cosD(2 * N);
    const double Del = std::sqrt(rj * rj + R * R - 2 * rj * R * cosD(K));
    const double psi = std::asin(std::clamp(R / Del * sinD(K), -1.0, 1.0)) / kDeg;
    const double dl = d - Del / 173.0;
    omega2 = std::fmod(187.23 + 870.1869088 * dl + psi - B, 360.0);
    if (omega2 < 0)
        omega2 += 360.0;
    double u1 = 163.8069 + 203.4058646 * dl + psi - B;
    double u2 = 358.4140 + 101.2916335 * dl + psi - B;
    double u3 = 5.7176 + 50.2345180 * dl + psi - B;
    double u4 = 224.8092 + 21.4879800 * dl + psi - B;
    const double G = 331.18 + 50.310482 * dl;
    const double H = 87.45 + 21.569231 * dl;
    const double c1 = 0.473 * sinD(2 * (u1 - u2)), c2 = 1.065 * sinD(2 * (u2 - u3)), c3 = 0.165 * sinD(G), c4 = 0.843 * sinD(H);
    r[0] = 5.9057 - 0.0244 * cosD(2 * (u1 - u2));
    r[1] = 9.3966 - 0.0882 * cosD(2 * (u2 - u3));
    r[2] = 14.9883 - 0.0216 * cosD(G);
    r[3] = 26.3627 - 0.1939 * cosD(H);
    u[0] = (u1 + c1) * kDeg;
    u[1] = (u2 + c2) * kDeg;
    u[2] = (u3 + c3) * kDeg;
    u[3] = (u4 + c4) * kDeg;
}
} // namespace

const char *planetBodyName(int body)
{
    if (body >= 0 && body < kPlanetCount)
        return kPlanetNames[body];
    if (body >= kPlanetCount && body < kPlanetBodyCount)
        return kGalileanNames[body - kPlanetCount];
    return "?";
}

double SatelliteSim::planetSinRingB(int planet) const
{
    if (planet != kSaturn)
        return 0.0;
    const double T = ((double)simDayJ2000 + simSecInDay / 86400.0) / 36525.0;
    const BodyConst &b = kBody[kSaturn];
    const glm::dvec3 P = raDecVec(b.a0 + b.a0d * T, b.d0 + b.d0d * T);
    const glm::dvec3 g = -glm::normalize(planetStates[kSaturn].geoEci);
    return glm::dot(P, g);
}

// ─── updatePlanetDiscs ──────────────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::updatePlanetDiscs(GpuSatVisible *dst)
{
    FPROF_SCOPE("updatePlanetDiscs");
    planetDiscCount = 0;
    if (!planetDiscMapped)
        return;
    auto *ubo = static_cast<GpuPlanetDiscUBO *>(planetDiscMapped);
    std::memset(ubo, 0, sizeof(GpuPlanetDiscUBO));
    ubo->head.y = 99u;
    ubo->head.z = planetMapLayers > 3 ? 3u : 0u;
    const bool fullSky = (debugDisableMask & (262144u | 524288u)) == 0u;
    ubo->tone = glm::vec4(fullSky && cv2Tonemap > 0.5f ? 1.0f : 0.0f, std::clamp(cv2ToneSat, 0.0f, 2.0f),
                          std::clamp(cv2ToneContrast, 0.5f, 2.0f), cv2HighlightRolloff);

    const double dJ = (double)simDayJ2000 + simSecInDay / 86400.0;
    const double T = dJ / 36525.0;
    const float H = ctx_ ? (float)std::max(1u, ctx_->swapExtent.height) : 1000.0f;
    const double pixAng = 2.0 * std::tan(glm::radians((double)camera.fovYDeg) * 0.5) / H;
    const float exposure = skyExposure();
    const glm::dvec3 eX(eci2enuX), eY(eci2enuY), eZ(eci2enuZ);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, eX), glm::dot(v, eY), glm::dot(v, eZ)); };

    for (int i = 0; i < kPlanetCount; ++i)
    {
        PlanetView &pv = planetView[i];
        const PlanetState &ps = planetStates[i];
        const BodyConst &b = kBody[i];
        const double thetaEq = b.reqKm / (ps.distanceAU * kAuKm);
        pv.angRadiusArcsec = (float)(thetaEq / kDeg * 3600.0);
        pv.discPx = (float)(thetaEq / pixAng);
        pv.share = 0.0f;
        pv.handScale = 1.0f;
        pv.ringTiltDeg = (float)(std::asin(std::clamp((double)pv.sinRingB, -1.0, 1.0)) / kDeg);

        // The local frame round the apparent (refracted) direction: x = the sky's right, y = up (toward the zenith), z = toward us.
        const glm::dvec3 c = glm::normalize(glm::dvec3(dst[i].skyDir));
        glm::dvec3 up(0.0, 0.0, 1.0);
        if (std::abs(c.z) > 0.999)
            up = glm::dvec3(0.0, 1.0, 0.0);
        const glm::dvec3 by = glm::normalize(up - c * glm::dot(up, c));
        const glm::dvec3 bz = -c;
        const glm::dvec3 bx = glm::cross(by, bz);
        auto toLocal = [&](const glm::dvec3 &eci)
        {
            const glm::dvec3 e = toEnu(eci);
            return glm::dvec3(glm::dot(e, bx), glm::dot(e, by), glm::dot(e, bz));
        };
        // Orientation (light time: the planet as it was Delta / c ago).
        const double dRet = dJ - ps.distanceAU / 173.1446;
        const glm::dvec3 Peci = raDecVec(b.a0 + b.a0d * T, b.d0 + b.d0d * T);
        const glm::dvec3 P = glm::normalize(toLocal(Peci));
        const glm::dvec3 s = glm::normalize(toLocal(-ps.helioEci));
        glm::dvec3 X0;
        double uOff = 0.0;
        if (i == kJupiter)
        {
            double uu[4], rr[4], om2;
            galileanMeeus(dJ, uu, rr, om2);
            const glm::dvec3 g(0.0, 0.0, 1.0);
            const glm::dvec3 eCm = glm::normalize(g - P * glm::dot(P, g));
            const glm::dvec3 ePE = glm::cross(P, eCm);   // planetary east at the central meridian
            X0 = std::cos(om2 * kDeg) * eCm + std::sin(om2 * kDeg) * ePE;
            pv.cmLonDeg = (float)om2;
            // The map's Great Red Spot (u 0.3682, tools/make_planet_maps.py) at its System II longitude: ~55 deg in mid
            // 2024, drifting ~+18 deg a year (the 2016-2024 trend, extrapolated: an estimate).
            const double year = 2000.0 + dJ / 365.25;
            const double lGrs = std::fmod(55.0 + 18.0 * (year - 2024.5), 360.0);
            uOff = lGrs / 360.0 - (0.5 - 0.3682);

            // ── The Galilean moons in Jupiter's frame ──
            const double qJ = b.rpKm / b.reqKm, kJ = 1.0 / (qJ * qJ) - 1.0;
            const glm::dvec3 eE = eCm, eW = glm::cross(P, eE);  // toward the Earth / the sky's west, in the equator plane
            const double rSun = glm::length(ps.helioEci);
            // The sky's west at Jupiter (celestial north up): c x north.
            glm::dvec3 ncp = toEnu(glm::dvec3(0, 0, 1));
            ncp = glm::normalize(ncp - c * glm::dot(ncp, c));
            const glm::dvec3 westSky = glm::cross(c, ncp);
            for (int m = 0; m < kGalileanCount; ++m)
            {
                GalileanState &gs = galilean[m];
                const glm::dvec3 M = rr[m] * (std::cos(uu[m]) * eE + std::sin(uu[m]) * eW);
                gs.x = (float)M.x;
                gs.y = (float)M.y;
                gs.z = (float)M.z;
                gs.xWest = (float)glm::dot(M.x * bx + M.y * by, westSky);
                gs.yNorth = (float)glm::dot(M.x * bx + M.y * by, ncp);
                const double rm = kGalileanRkm[m] / b.reqKm;
                // Jupiter's shadow: distance from the shadow's axis, the pole's component stretched by the flattening.
                const double ms = glm::dot(M, s);
                glm::dvec3 v = M - s * ms;
                v += (1.0 / qJ - 1.0) * glm::dot(v, P) * P;
                const double perp = glm::length(v);
                const double edge = rm + 0.00093 * std::abs(ms);
                float lit = 1.0f;
                gs.eclipsed = gs.shadowOnJupiter = false;
                if (ms < 0.0)
                {
                    lit = smoothstepf((float)(1.0 - edge), (float)(1.0 + edge), (float)perp);
                    gs.eclipsed = perp < 1.0;
                }
                else
                    gs.shadowOnJupiter = perp < 1.0;
                // Behind the disc: the limb's radius toward the moon's direction (bisection on the spheroid).
                const double rxy = std::sqrt(M.x * M.x + M.y * M.y);
                double lo = 0.0, hi = 1.2;
                for (int it = 0; it < 24; ++it)
                {
                    const double mid = 0.5 * (lo + hi);
                    double zz;
                    (spheroidHitCpu(M.x / std::max(rxy, 1e-9) * mid, M.y / std::max(rxy, 1e-9) * mid, P, kJ, zz) ? lo : hi) = mid;
                }
                const float overDisc = 1.0f - smoothstepf((float)(lo - rm), (float)(lo + rm), (float)rxy);
                gs.occulted = M.z < 0.0 && rxy < lo;
                gs.transit = M.z > 0.0 && rxy < lo;
                const float vis = M.z < 0.0 ? 1.0f - overDisc : 1.0f;
                // Brightness: V(1,0) + 5 log10(r Delta) (au), the line of sight's extinction and suppression as Jupiter's.
                gs.mag = (float)(kGalileanV10[m] + 5.0 * std::log10(rSun * ps.distanceAU));
                float inten = std::pow(10.0f, -0.4f * gs.mag) * pv.extinction * pv.supp * lit * vis;
                pv.share = planetDiscs ? smoothstepf(1.0f, 2.5f, pv.discPx) : 0.0f;
                if (M.z > 0.0)
                    inten *= 1.0f - 0.95f * pv.share * overDisc;   // in transit over a resolved disc: hardly seen (as Io against the zones)
                const glm::dvec3 jc = c;   // Jupiter's apparent direction
                const glm::dvec3 off = thetaEq * (M.x * bx + M.y * by);
                GpuSatVisible &e = dst[kPlanetCount + m];
                e.skyDir = glm::vec3(glm::normalize(jc + off));
                e.flareIntensity = (planetEnabled[kJupiter] && pv.above) ? inten : 0.0f;
                e.color = packVisibleColor(kGalileanColor[m]);
                e.rangeM = 0.0f;
                e.meshPx = 0.0f;
                e.angularSize = pointSpriteSizePx(pointPsf(inten > 0.0f ? -2.5f * std::log10(inten) : 99.0f).y);
                ubo->moons[m] = glm::vec4((float)M.x, (float)M.y, (float)M.z, (float)rm);
            }
        }
        else
        {
            const double W = (b.W0 + b.Wd * dRet) * kDeg;
            const glm::dvec3 Nn = glm::normalize(glm::cross(toLocal(glm::dvec3(0, 0, 1)), P));   // the ascending node on the ICRF equator
            X0 = std::cos(W) * Nn + std::sin(W) * glm::cross(P, Nn);
            const glm::dvec3 Y0 = glm::cross(P, X0);
            pv.cmLonDeg = (float)(std::atan2(Y0.z, X0.z) / kDeg);   // the sub-Earth point's east longitude
        }

        // ── Share, and nothing more to do for a point ──
        pv.share = planetDiscs ? smoothstepf(1.0f, 2.5f, pv.discPx) : 0.0f;
        if (pv.share <= 0.0f || !pv.above || !planetEnabled[i] || planetDiscCount >= (uint32_t)kPlanetCount)
            continue;

        // ── The law's mean over the disc (pi in equatorial radii^2), and its lit share ──
        const double q = b.rpKm / b.reqKm, k = 1.0 / (q * q) - 1.0;
        double sumLaw = 0.0;
        int nLit = 0;
        const int N = 32;
        for (int yi = 0; yi < N; ++yi)
            for (int xi = 0; xi < N; ++xi)
            {
                const double x = -1.0 + (xi + 0.5) * 2.0 / N, y = -1.0 + (yi + 0.5) * 2.0 / N;
                double z;
                if (!spheroidHitCpu(x, y, P, k, z))
                    continue;
                const glm::dvec3 p(x, y, z);
                const glm::dvec3 n = glm::normalize(p + k * glm::dot(p, P) * P);
                const double l = lawCpu(b.law, b.k, glm::dot(n, s), n.z);
                sumLaw += l;
                nLit += l > 0.0 ? 1 : 0;
            }
        const double cell = (2.0 / N) * (2.0 / N);
        const double meanLaw = std::max(sumLaw * cell / kPi, 1e-6);
        const double litFrac = std::max(nLit * cell / kPi, 0.01);

        // ── The light: the planet's ball magnitude over its disc (pi L / E units, Moon-calibrated) ──
        const float mBall = planetApparentMagnitude((PlanetId)i, ps.sunDistAU, ps.distanceAU, ps.phaseAngleDeg, 0.0);
        const double lAvg = std::pow(10.0, -0.4 * (mBall + 26.74)) / (thetaEq * thetaEq) * kMoonDiscCal * planetDiscGain;
        const glm::dvec3 tRgb(std::pow(10.0, -0.4 * pv.extMag * 0.75), std::pow(10.0, -0.4 * pv.extMag),
                              std::pow(10.0, -0.4 * pv.extMag * 1.40));
        // The hand-off: the disc's displayed light = the point's (its style is not linear in flux) at 2.5 px, relaxing to
        // the physical by 5 px.
        // (The point as drawn: the suppression's dimming included, so the hand-off is seamless whatever the sky.)
        const glm::vec2 psf0 = pointPsf(-2.5f * std::log10(std::max(dst[i].flareIntensity, 1e-30f)));
        const double ePoint = psf0.x * 2.0 * kPi * psf0.y * psf0.y;
        const double vBar = exposure * lAvg * tRgb.y / litFrac;
        const double area = litFrac * kPi * pv.discPx * pv.discPx;
        double lo = -24.0, hi = 24.0;
        for (int it = 0; it < 40; ++it)
        {
            const double mid = 0.5 * (lo + hi);
            const double e = area * agxGrey((float)(std::exp2(mid) * vBar), ubo->tone.z);
            (e < ePoint ? lo : hi) = mid;
        }
        // The point style is far from linear (a zoomed point is a saturated 6-px-sigma blob): the match is held to
        // +-3 stops of the physical (beyond it the share's cross-fade carries the step), relaxed by 5 px (Mars and
        // Saturn reach only ~5 px at the 0.5-deg field).
        const double relax = 1.0 - smoothstepf(2.5f, 5.0f, pv.discPx);
        pv.handScale = (float)std::exp2(std::clamp(0.5 * (lo + hi), -3.0, 3.0) * relax);

        GpuPlanetDisc &D = ubo->disc[planetDiscCount];
        const double qd = (i == kSaturn ? 2.35 : (i == kVenus ? 1.08 : 1.0));
        const glm::vec2 psfS = pointPsf(-2.5f * std::log10(std::max(dst[i].flareIntensity, 1e-30f)));
        const double ptSig = psfS.y;
        const double halfExt = std::max(qd + 2.0 * pixAng / thetaEq, (3.0 * ptSig + 1.5) * pixAng / thetaEq) * 1.05;
        D.c = glm::vec4(glm::vec3(c), (float)thetaEq);
        D.bx = glm::vec4(glm::vec3(bx), (float)halfExt);
        D.by = glm::vec4(glm::vec3(by), (float)(pixAng / thetaEq));
        D.pole = glm::vec4(glm::vec3(P), (float)q);
        D.x0 = glm::vec4(glm::vec3(X0), (b.layer >= 0 && (uint32_t)b.layer < planetMapLayers) ? (float)b.layer : -1.0f);
        D.sun = glm::vec4(glm::vec3(s), (float)b.law);
        glm::vec3 tint = (D.x0.w >= 0.0f) ? glm::vec3(1.0f) : b.tint;
        tint /= std::max(glm::dot(tint, glm::vec3(0.2126f, 0.7152f, 0.0722f)), 1e-4f);
        D.tint = glm::vec4(tint, b.k);
        const glm::dvec3 lin = exposure * lAvg / meanLaw * tRgb * (double)pv.handScale;
        D.lin = glm::vec4(glm::vec3(lin), pv.share);
        D.pt = glm::vec4(psfS.x * (1.0f - pv.share), (float)ptSig, pv.supp, (float)uOff);
        // Rings and Venus's rim: linear light per unit I/F (pi L / E at the planet: 1 / r^2).
        const double rAU = ps.sunDistAU;
        const double ifScale = exposure / (rAU * rAU) * kMoonDiscCal * planetDiscGain * tRgb.y * pv.handScale;
        float rimIF = 0.0f;
        if (i == kVenus)   // the haze's forward scattering round the limb, strong past ~140 deg of phase
            rimIF = 1.5f * smoothstepf(140.0f, 176.0f, ps.phaseAngleDeg);
        D.ring = glm::vec4(i == kSaturn ? 2.30f : 0.0f, (float)ifScale, rimIF, 0.0f);
        // Seeing: the refraction table's angle-of-arrival (the planet's record carries it, star_point.vert's packing).
        float aoa = dst[i].meshPx >= 1.0f ? std::floor(dst[i].meshPx - 1.0f) * 1e-6f : 0.0f;
        const float blurPx = (float)(aoa / pixAng);
        const double tt = std::fmod(simSecInDay, 600.0);
        const double jx = aoa * (std::sin(2 * kPi * 1.31 * tt + i) + 0.5 * std::sin(2 * kPi * 3.70 * tt + 2.1 * i)) * 0.9 / thetaEq;
        const double jy = aoa * (std::sin(2 * kPi * 1.07 * tt + 0.7 * i) + 0.5 * std::sin(2 * kPi * 2.90 * tt + 1.3 * i)) * 0.9 / thetaEq;
        D.misc = glm::vec4(blurPx, (float)jx, (float)jy, (float)i);
        if (i == kJupiter)
            ubo->head.y = planetDiscCount;
        ++planetDiscCount;
        // The star pipeline skips this record (star_point.vert: angularSize < 0); picking and the panel keep it.
        dst[i].angularSize = -std::abs(dst[i].angularSize);
    }
    ubo->head.x = planetDiscCount;
}

// ─── Vulkan: maps, set, pipeline ────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::createPlanetDiscs(VulkanContext &ctx)
{
    // The maps: "SLPL" | w | h | layers | mips | per level, per layer, RGBA8 rows (tools/make_planet_maps.py).
    uint32_t w = 1, h = 1, layers = 4, mips = 1;
    std::vector<unsigned char> data;
    {
        const char *path = "assets/textures/planet_maps.rgba8";
        Log::line(std::string("init: texture: ") + path);
        FILE *f = std::fopen(path, "rb");
        bool ok = false;
        if (f)
        {
            char magic[4];
            uint32_t hdr[4];
            if (std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "SLPL", 4) == 0 && std::fread(hdr, 4, 4, f) == 4 &&
                hdr[0] > 0 && hdr[0] <= 8192 && hdr[1] > 0 && hdr[1] <= 4096 && hdr[2] > 0 && hdr[2] <= 16 && hdr[3] > 0 && hdr[3] <= 14)
            {
                w = hdr[0];
                h = hdr[1];
                layers = hdr[2];
                mips = hdr[3];
                size_t total = 0;
                for (uint32_t m = 0; m < mips; ++m)
                    total += (size_t)std::max(1u, w >> m) * std::max(1u, h >> m) * 4 * layers;
                data.resize(total);
                ok = std::fread(data.data(), 1, total, f) == total;
            }
            std::fclose(f);
        }
        if (!ok)
        {
            Log::line("init: planet_maps.rgba8 missing (tools/make_planet_maps.py): flat planets, no ring profile");
            w = h = 1;
            layers = 4;
            mips = 1;
            data.assign(4 * 4, 0);
            for (int l = 0; l < 3; ++l)   // sqrt(1 / 8) x 255: the map's mean
                data[l * 4 + 0] = data[l * 4 + 1] = data[l * 4 + 2] = data[l * 4 + 3] = 90;
            // ring layer: a plain grey ring of optical depth 1
            data[12] = data[13] = data[14] = 128;
            data[15] = 64;
        }
    }
    planetMapLayers = layers;
    VkBuffer stageBuf;
    VkDeviceMemory stageMem;
    ctx.createBuffer(data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stageBuf, stageMem);
    std::vector<VkBufferImageCopy> regions(mips);
    {
        void *mapped;
        vkMapMemory(ctx.device, stageMem, 0, data.size(), 0, &mapped);
        std::memcpy(mapped, data.data(), data.size());
        vkUnmapMemory(ctx.device, stageMem);
        VkDeviceSize off = 0;
        for (uint32_t m = 0; m < mips; ++m)
        {
            regions[m] = {};
            regions[m].bufferOffset = off;
            regions[m].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, layers};
            regions[m].imageExtent = {std::max(1u, w >> m), std::max(1u, h >> m), 1};
            off += (VkDeviceSize)std::max(1u, w >> m) * std::max(1u, h >> m) * 4 * layers;
        }
    }
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {w, h, 1};
    ici.mipLevels = mips;
    ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(ctx.device, &ici, nullptr, &planetMapImg);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx.device, planetMapImg, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = ctx.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(ctx.device, &mai, nullptr, &planetMapMem);
    vkBindImageMemory(ctx.device, planetMapImg, planetMapMem, 0);
    {
        auto cmd = ctx.beginOneTimeCommands();
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = planetMapImg;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers};
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        vkCmdCopyBufferToImage(cmd, stageBuf, planetMapImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mips, regions.data());
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        ctx.endOneTimeCommands(cmd);
    }
    vkDestroyBuffer(ctx.device, stageBuf, nullptr);
    vkFreeMemory(ctx.device, stageMem, nullptr);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = planetMapImg;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers};
    vkCreateImageView(ctx.device, &vci, nullptr, &planetMapView);
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;   // longitude wraps
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = (float)mips;
    vkCreateSampler(ctx.device, &sci, nullptr, &planetMapSampler);
    Log::line("init: planet maps " + std::to_string(w) + "x" + std::to_string(h) + " x " + std::to_string(layers) +
              " layers, " + std::to_string(mips) + " mips");

    planetDiscMapped = createMirrored(ctx, sizeof(GpuPlanetDiscUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, planetDiscBuf,
                                      planetDiscMem, true, "planet discs");

    VkDescriptorSetLayoutBinding bnd[5] = {};
    bnd[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (int i = 1; i < 5; ++i)
        bnd[i] = {(uint32_t)i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 5;
    li.pBindings = bnd;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &planetDiscLayout);
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    pi.maxSets = 1;
    vkCreateDescriptorPool(ctx.device, &pi, nullptr, &planetDiscPool);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = planetDiscPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &planetDiscLayout;
    vkAllocateDescriptorSets(ctx.device, &ai, &planetDiscSet);
    VkDescriptorBufferInfo ubInfo{planetDiscBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo mapInfo{planetMapSampler, planetMapView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr[2] = {};
    wr[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, planetDiscSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubInfo, nullptr};
    wr[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, planetDiscSet, 4, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &mapInfo, nullptr, nullptr};
    frameSafeUpdateDescriptorSets(ctx.device, 2, wr, 0, nullptr);
    writePlanetDiscTargets(ctx);

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PointDrawPC)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &planetDiscLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(ctx.device, &pli, nullptr, &planetDiscPipeLayout);
    createPlanetDiscPipeline(ctx);
}

void SatelliteSim::writePlanetDiscTargets(VulkanContext &ctx)
{
    if (!planetDiscSet)
        return;
    VkDescriptorImageInfo aInfo{cloudMarchSampler, cloudMarchTargetAView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo bInfo{cloudMarchSampler, cloudMarchTargetBView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo dInfo{sceneDepthSampler, sceneDepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr[3] = {};
    wr[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, planetDiscSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &aInfo, nullptr, nullptr};
    wr[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, planetDiscSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &bInfo, nullptr, nullptr};
    wr[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, planetDiscSet, 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dInfo, nullptr, nullptr};
    frameSafeUpdateDescriptorSets(ctx.device, sceneDepthView ? 3 : 2, wr, 0, nullptr);
}

void SatelliteSim::createPlanetDiscPipeline(VulkanContext &ctx)
{
    if (!planetDiscPipeLayout)
        return;
    if (planetDiscPipeline)
    {
        vkDestroyPipeline(ctx.device, planetDiscPipeline, nullptr);
        planetDiscPipeline = VK_NULL_HANDLE;
    }
    VkShaderModule vert = ctx.loadShader("shaders/planet_disc.vert.spv");
    VkShaderModule frag = ctx.loadShader("shaders/planet_disc.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0, 0, (float)ctx.swapExtent.width, (float)ctx.swapExtent.height, 0, 1};
    VkRect2D sc{{0, 0}, ctx.swapExtent};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rast{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.cullMode = VK_CULL_MODE_NONE;
    rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;   // at kDepthFar, as the stars: terrain, opaque cloud and the Moon hide it
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vps;
    ci.pRasterizationState = &rast;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.layout = planetDiscPipeLayout;
    ci.renderPass = ctx.renderPass;
    ci.subpass = 0;
    if (vkCreateGraphicsPipelines(ctx.device, ctx.pipelineCache, 1, &ci, nullptr, &planetDiscPipeline) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: failed to create the planet disc pipeline");
    vkDestroyShaderModule(ctx.device, vert, nullptr);
    vkDestroyShaderModule(ctx.device, frag, nullptr);
}

void SatelliteSim::destroyPlanetDiscs(VkDevice device)
{
    if (planetDiscPipeline) vkDestroyPipeline(device, planetDiscPipeline, nullptr);
    if (planetDiscPipeLayout) vkDestroyPipelineLayout(device, planetDiscPipeLayout, nullptr);
    if (planetDiscPool) vkDestroyDescriptorPool(device, planetDiscPool, nullptr);
    if (planetDiscLayout) vkDestroyDescriptorSetLayout(device, planetDiscLayout, nullptr);
    if (planetDiscBuf) vkDestroyBuffer(device, planetDiscBuf, nullptr);
    if (planetDiscMem) vkFreeMemory(device, planetDiscMem, nullptr);
    if (planetMapSampler) vkDestroySampler(device, planetMapSampler, nullptr);
    if (planetMapView) vkDestroyImageView(device, planetMapView, nullptr);
    if (planetMapImg) vkDestroyImage(device, planetMapImg, nullptr);
    if (planetMapMem) vkFreeMemory(device, planetMapMem, nullptr);
    planetDiscPipeline = VK_NULL_HANDLE;
    planetDiscPipeLayout = VK_NULL_HANDLE;
    planetDiscPool = VK_NULL_HANDLE;
    planetDiscLayout = VK_NULL_HANDLE;
    planetDiscSet = VK_NULL_HANDLE;
    planetDiscBuf = VK_NULL_HANDLE;
    planetDiscMem = VK_NULL_HANDLE;
    planetMapSampler = VK_NULL_HANDLE;
    planetMapView = VK_NULL_HANDLE;
    planetMapImg = VK_NULL_HANDLE;
    planetMapMem = VK_NULL_HANDLE;
    planetDiscMapped = nullptr;
}

void SatelliteSim::recordPlanetDiscs(VkCommandBuffer cmd, const PointDrawPC &pc)
{
    if (planetDiscCount == 0 || !planetDiscPipeline || !showPlanets)
        return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, planetDiscPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, planetDiscPipeLayout, 0, 1, &planetDiscSet, 0, nullptr);
    vkCmdPushConstants(cmd, planetDiscPipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 6, planetDiscCount, 0, 0);
}

// ─── The selection panel ────────────────────────────────────────────────────────────────────────────────────────────
// Re-called every frame the selection is active (the astronomy moves). Planets: angular diameter (and the disc's size on
// screen), magnitude, distance, phase; line 6 the body's own: Jupiter its moons' configuration, Saturn its rings' tilt,
// Mars its central meridian. A Galilean moon: its offset from Jupiter and what it is doing.
void SatelliteSim::formatSelectedPlanetInfo()
{
    for (auto &line : planetInfoLine)
        line[0] = '\0';
    const int sel = selectedPlanetIndex;
    if (sel < 0 || sel >= kPlanetBodyCount)
        return;
    const int L = (int)sizeof(planetInfoLine[0]);
    if (sel >= kPlanetCount)
    {
        const int m = sel - kPlanetCount;
        const GalileanState &g = galilean[m];
        const PlanetState &ps = planetStates[kJupiter];
        const float sep = std::sqrt(g.xWest * g.xWest + g.yNorth * g.yNorth);
        const float sepArc = sep * planetView[kJupiter].angRadiusArcsec;
        std::snprintf(planetInfoLine[0], L, "%s", kGalileanNames[m]);
        std::snprintf(planetInfoLine[1], L, "Moon of Jupiter, %.2f\" across", 2.0 * kGalileanRkm[m] / (ps.distanceAU * kAuKm) / kDeg * 3600.0);
        std::snprintf(planetInfoLine[2], L, "Mag: %.1f", g.mag);
        std::snprintf(planetInfoLine[3], L, "%.1f Jupiter radii %s of Jupiter (%.0f\")", sep, g.xWest >= 0.0f ? "west" : "east", sepArc);
        const char *what = g.eclipsed ? "In Jupiter's shadow (eclipsed)" : g.occulted ? "Behind Jupiter (occulted)"
                         : g.transit ? (g.shadowOnJupiter ? "Transiting Jupiter, its shadow on the disc" : "Transiting Jupiter")
                         : g.shadowOnJupiter ? "Its shadow is on Jupiter" : (g.z > 0 ? "In front of Jupiter's distance" : "Beyond Jupiter");
        std::snprintf(planetInfoLine[4], L, "%s", what);
        std::snprintf(planetInfoLine[5], L, "Distance: %.2f AU", ps.distanceAU);
        return;
    }
    const PlanetState &ps = planetStates[sel];
    const PlanetView &pv = planetView[sel];
    const float vmag = planetApparentMagnitude((PlanetId)sel, ps.sunDistAU, ps.distanceAU, ps.phaseAngleDeg, pv.sinRingB);
    const float illumFrac = (1.0f + std::cos(glm::radians(ps.phaseAngleDeg))) * 0.5f;
    std::snprintf(planetInfoLine[0], L, "%s", kPlanetNames[sel]);
    std::snprintf(planetInfoLine[1], L, "Planet, %.1f\" across (%.1f px)", 2.0f * pv.angRadiusArcsec, 2.0f * pv.discPx);
    std::snprintf(planetInfoLine[2], L, "Mag: %.1f", vmag);
    std::snprintf(planetInfoLine[3], L, "Distance: %.2f AU", ps.distanceAU);
    std::snprintf(planetInfoLine[4], L, "Phase: %.0f%% lit (phase angle %.0f deg)", illumFrac * 100.0f, ps.phaseAngleDeg);
    std::snprintf(planetInfoLine[5], L, "Sun dist: %.2f AU", ps.sunDistAU);
    if (sel == kJupiter)
    {
        // East on the left, as on the sky (north up): each moon's offset in Jupiter radii, a flag for what it is doing.
        int ord[kGalileanCount] = {0, 1, 2, 3};
        std::sort(ord, ord + kGalileanCount, [&](int a, int b) { return galilean[a].xWest < galilean[b].xWest; });   // west last
        char buf[kSelInfoLineLen];
        int n = std::snprintf(buf, sizeof(buf), "E ");
        bool jDone = false;
        for (int k = 0; k < kGalileanCount; ++k)
        {
            const GalileanState &g = galilean[ord[k]];
            if (!jDone && g.xWest > 0.0f)
            {
                n += std::snprintf(buf + n, sizeof(buf) - n, "[J] ");
                jDone = true;
            }
            const char *f = g.eclipsed ? "(ecl)" : g.occulted ? "(occ)" : g.transit ? "(tr)" : g.shadowOnJupiter ? "(sh)" : "";
            n += std::snprintf(buf + n, sizeof(buf) - n, "%.2s %.1f%s ", kGalileanNames[ord[k]], std::abs(g.xWest), f);
        }
        if (!jDone)
            n += std::snprintf(buf + n, sizeof(buf) - n, "[J] ");
        std::snprintf(buf + n, sizeof(buf) - n, "W");
        std::snprintf(planetInfoLine[6], L, "Moons: %s", buf);
    }
    else if (sel == kSaturn)
        std::snprintf(planetInfoLine[6], L, "Rings open %.1f deg (%s face)", std::abs(pv.ringTiltDeg), pv.ringTiltDeg >= 0 ? "north" : "south");
    else if (sel == kMars)
        std::snprintf(planetInfoLine[6], L, "Central meridian %.0f E", std::fmod(pv.cmLonDeg + 360.0f, 360.0f));
}

nlohmann::json SatelliteSim::planetsJson() const
{
    nlohmann::json j = nlohmann::json::object();
    nlohmann::json arr = nlohmann::json::array();
    for (int i = 0; i < kPlanetCount; ++i)
    {
        const PlanetView &pv = planetView[i];
        arr.push_back({{"name", kPlanetNames[i]}, {"diameter_arcsec", 2.0f * pv.angRadiusArcsec}, {"disc_px", pv.discPx},
                       {"share", pv.share}, {"hand_scale", pv.handScale}, {"phase_deg", planetStates[i].phaseAngleDeg},
                       {"dist_au", planetStates[i].distanceAU}, {"ring_tilt_deg", pv.ringTiltDeg}, {"cm_lon_deg", pv.cmLonDeg},
                       {"supp", pv.supp}});
    }
    j["bodies"] = arr;
    j["discs_drawn"] = planetDiscCount;
    nlohmann::json moons = nlohmann::json::array();
    for (int m = 0; m < kGalileanCount; ++m)
    {
        const GalileanState &g = galilean[m];
        moons.push_back({{"name", kGalileanNames[m]}, {"x_west_rj", g.xWest}, {"y_north_rj", g.yNorth}, {"z_toward_earth_rj", g.z},
                         {"x_local", g.x}, {"y_local", g.y},
                         {"mag", g.mag}, {"transit", g.transit}, {"occulted", g.occulted}, {"eclipsed", g.eclipsed},
                         {"shadow_on_jupiter", g.shadowOnJupiter}});
    }
    j["galilean"] = moons;
    return j;
}
