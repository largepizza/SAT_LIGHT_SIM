// SatelliteSimWildfire.cpp — the wildfires drawn (2026-10-23, Wildfire.h, .plans/WEATHER_HAZARDS_PLAN.md section 1;
// reworked 2026-10-23 b from the user's issue 20261023_225322).
//
// SMOKE: plume Gaussians appended to the dust list after the haboobs, one contiguous range with them whose bounds go in
// the list header's word 1 (bits 8-17 first, 18-27 count; smoke.glsl). Each fire's smoke is a STREAK: parcels emitted
// over the last hours, each risen to its layer and carried by the wind of its height at the moment it travelled
// (Wildfire.h windAt: coherent in space, turning with height and changing over hours), so the column bends over, a
// lower and an upper layer go different ways, older smoke follows older winds, and every plume wiggles and widens as it
// goes. The mass of a stretch follows the fire's activity when it was emitted (the afternoon's smoke is thicker). Older
// smoke is one broad, flat, mass-conserving PALL per fire (the regional haze of a fire season). Lit by the Sun's colour
// at each Gaussian (its path through the air), Earth-shadowed per pixel (cloud_march.comp), grey: fresh smoke with
// black carbon darker, aged paler; the Sun seen THROUGH it is dimmed and reddened spectrally (cloud_march.comp,
// sat_sky.frag smokeSunTransmit).
//
// FIRE: the burning front as a field of point sprites (kFireBuf, like the city lights' sprites): the burnt ellipse with
// fingers (Wildfire.h fireFrontAt), its runs of flame and gaps (fireFrontHeat), embers glowing behind it and spot fires
// ahead. Points sit on a nested hierarchy along the front (finest 2^14 round), each point's level chosen by its own
// range so they stay ~kFirePointPx apart on screen at any distance, the next level's points cross-fading in carrying
// half their parents' light (no pops, the light kept); a frame budget widens the spacing smoothly when many fires are in
// view. Brightness from a flame's luminance x its area (L ~2e4 cd/m^2, the flame height and the flaming zone's depth by
// the view), compressed like a sensor's shoulder to the city sprites' range (no glare spikes), flickering (less as a
// point stands for more front), dimmed against a sunlit ground by day. Close up, the flames are emissive Gaussians along
// the front (the dust list, after the smoke's range). The nearest fires' glow lights the smoke, the cloud and the ground
// (engine lights, eye-adapted), fading out with range.
#include "SatelliteSim.h"
#include "FrameProfile.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace launchsim { double demElevCpu(double latDeg, double lonDeg); }

namespace {
constexpr double kReF = 6371000.0;
constexpr double kPiF = 3.14159265358979323846;
constexpr int kFireFineLevels = 14;         // the finest level of the front's points: 2^14 round
constexpr double kFirePointPx = 5.0;        // the points' spacing on screen (full-res px)
constexpr uint32_t kFirePointBudget = 3500; // points a frame (kFireBuf holds 8192 for everything)
constexpr double kFlameLum = 2.0e4;         // cd/m^2: a wildfire flame's bright parts (soot at ~1300-1400 K)
constexpr double kLuxMag0 = 2.54e-6;        // lux of magnitude 0
constexpr double kSunLux = 1.27e5;
uint64_t fwMix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
double fwU(uint64_t h) { return (double)(h >> 11) * (1.0 / 9007199254740992.0); }
glm::dvec3 fwEcefToEci(const glm::dvec3 &v, double ang)
{
    const double c = std::cos(ang), sn = std::sin(ang);
    return {v.x * c - v.y * sn, v.x * sn + v.y * c, v.z};
}
glm::dvec3 llDir(double latDeg, double lonDeg)
{
    const double la = latDeg * kPiF / 180.0, lo = lonDeg * kPiF / 180.0;
    return {std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la)};
}
int fineLevelOf(uint32_t J)
{
    if (J == 0) return 0;
    int tz = 0;
    while (((J >> tz) & 1u) == 0u) ++tz;
    return kFireFineLevels - tz;
}
// A physical flare (the satellites' effectFlare scale) compressed like a sensor's shoulder, approaching 8 (the city
// sprites' cap, under the glare threshold). Physically a fire's points span ~6 decades (a 6-m stretch of flame 1 km off
// to a km of a grass fire's back from orbit); compressed so a big fire's head from LEO (~3e3) is ~5.5, its back ~3,
// one 1 km off saturates, and a faint fire's points stay faint.
double fireShoulder(double x)
{
    if (!(x > 0.0)) return 0.0;
    return 8.0 * (1.0 - std::exp(-std::pow(x / 2000.0, 0.45)));
}
} // namespace

void SatelliteSim::weatherFillWildfires(GpuDustCloud *out, uint32_t &n, double pixFull)
{
    FPROF_SCOPE("weatherFillWildfires");
    smokeHdrBits = 0;
    firesNow.clear();
    firePointsLast = 0;
    fireFlamesLast = 0;
    fireLightsLast = 0;
    if (!out) return;
    if (wildfireRate <= 0.0f || !biomeMap.ready())
    {
        if (n > aerosolFirst) smokeHdrBits = (aerosolFirst << 8u) | ((n - aerosolFirst) << 18u);
        return;
    }
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    const double eyeR = (double)obsEyeRadiusM();
    const glm::dvec3 eye = up * eyeR;
    const double alt = std::max(eyeR - kReF, 0.0);
    const glm::dvec3 E = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up) + glm::dvec3(1e-12, 0, 0));
    const glm::dvec3 N = glm::cross(up, E);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, E), glm::dot(v, N), glm::dot(v, up)); };
    const double eyeLat = glm::degrees(std::asin(std::clamp(up.z, -1.0, 1.0)));
    const double eyeLon = glm::degrees(std::atan2(up.y, up.x));
    pixFull = std::max(pixFull, 1e-6);

    // The fires in reach: from the ground ~600 km (the smoke on the horizon), from orbit to ~3000 km.
    const double reachKm = std::clamp(600.0 + 3.0 * alt / 1000.0, 600.0, 3000.0);
    {
        const int64_t b1 = (int64_t)std::floor(t / 86400.0), b0 = (int64_t)std::floor((t - wx::kFireMaxLifeS) / 86400.0);
        const double moved = std::acos(std::clamp(glm::dot(up, llDir(fireCacheLat, fireCacheLon)), -1.0, 1.0)) * 6371.0;
        const bool rescan = fireCacheRate != wildfireRate || moved > 0.15 * fireCacheR || reachKm > fireCacheR * 0.9 ||
                            reachKm < fireCacheR * 0.5 || b0 > fireCacheB1 || b1 < fireCacheB0;
        if (rescan)
        {
            fireCache.clear();
            fireCacheLat = eyeLat;
            fireCacheLon = eyeLon;
            fireCacheR = reachKm * 1.3;
            fireCacheRate = wildfireRate;
            wx::firesBorn(b0, b1, fireCacheLat, fireCacheLon, fireCacheR, biomeMap, wildfireRate, fireCache);
            fireCacheB0 = b0;
            fireCacheB1 = b1;
        }
        else
        {
            // Time moved: add the new days' fires (forward) or the older days' (backward); the window's other end is
            // dropped by the alive test below, and the cache is bounded by a rescan when it slides past.
            if (b1 > fireCacheB1)
            {
                wx::firesBorn(fireCacheB1 + 1, b1, fireCacheLat, fireCacheLon, fireCacheR, biomeMap, wildfireRate, fireCache);
                fireCacheB1 = b1;
            }
            if (b0 < fireCacheB0)
            {
                wx::firesBorn(b0, fireCacheB0 - 1, fireCacheLat, fireCacheLon, fireCacheR, biomeMap, wildfireRate, fireCache);
                fireCacheB0 = b0;
            }
            if (fireCacheB1 - fireCacheB0 > 45)   // bounded: rebuild for the window
            {
                fireCache.clear();
                wx::firesBorn(b0, b1, fireCacheLat, fireCacheLon, fireCacheR, biomeMap, wildfireRate, fireCache);
                fireCacheB0 = b0;
                fireCacheB1 = b1;
            }
        }
    }
    if (fireGroundCache.size() > 8192) fireGroundCache.clear();
    int groundLookups = 0;
    for (const wx::Fire &f : fireCache)
    {
        if (t < f.t0 || t > f.t0 + f.life) continue;
        const glm::dvec3 g = llDir(f.latDeg, f.lonDeg);
        const double dKm = std::acos(std::clamp(glm::dot(g, up), -1.0, 1.0)) * 6371.0;
        if (dKm > reachKm) continue;
        FireNow fn;
        fn.f = f;
        fn.s = wx::fireStateAt(f, t);
        fn.rangeKm = std::sqrt(dKm * dKm + (alt / 1000.0) * (alt / 1000.0));
        fn.reachK = 1.0 - glm::smoothstep(0.8 * reachKm, reachKm, dKm);   // (no pop at the edge of reach)
        // The ground under it: the full DEM (~2.7 km) once per fire (a file read: a few a frame), the coarse copy until then.
        auto it = fireGroundCache.find(f.id);
        if (it != fireGroundCache.end())
            fn.groundM = it->second;
        else if (groundLookups < 12)
        {
            ++groundLookups;
            fn.groundM = (float)std::max(0.0, launchsim::demElevCpu(f.latDeg, f.lonDeg));
            fireGroundCache[f.id] = fn.groundM;
        }
        else
            fn.groundM = std::max(0.0f, cpuTerrainHeightM((float)f.latDeg, (float)f.lonDeg));
        firesNow.push_back(fn);
    }
    std::sort(firesNow.begin(), firesNow.end(), [](const FireNow &a, const FireNow &b) { return a.rangeKm < b.rangeKm; });

    // The Sun (Earth-fixed).
    const glm::dvec3 si = sunDirEciAt(t);
    const double gr = earthRotationAngle(t);
    const glm::dvec3 sunE = glm::normalize(glm::dvec3(si.x * std::cos(gr) + si.y * std::sin(gr),
                                                      -si.x * std::sin(gr) + si.y * std::cos(gr), si.z));
    // The Sun's light at a point (its colour through the air; the per-pixel Earth shadow is cloud_march.comp's) and the
    // blue sky's on it (relative, 1 = a clear day), as the rocket plumes'.
    auto sunLightAt = [&](const glm::dvec3 &C, glm::dvec3 &sunC, double &amb) {
        const double r = glm::length(C), h = r - kReF;
        const double sinE = glm::dot(C / r, sunE);
        const double dip = -std::sqrt(2.0 * std::max(h, 0.0) / kReF);
        const double dayK = glm::smoothstep(dip - 0.03, dip + 0.02, sinE);
        sunC = glm::dvec3(0.0);
        if (dayK > 0.0)
        {
            const double ext1 = atmExtinctionMag(C, sunE, 0.0, 1.0);
            const double kc[3] = {0.11, 0.2, 0.42};
            for (int ch = 0; ch < 3; ++ch)
                sunC[ch] = dayK * std::pow(10.0, -0.4 * std::min(kc[ch] * ext1, 30.0));
        }
        amb = glm::smoothstep(-0.1, 0.25, sinE) * std::exp(-std::max(h, 0.0) / 15000.0);
    };

    const double kTwoPi15 = 15.749609945722419;
    const uint32_t first = n;
    const uint32_t cap = std::min<uint32_t>(kMaxDustClouds, n + 320u);
    // One Gaussian: centre C (ECEF), orthonormal axes a, b, c with sigmas, Sigma = cross-section, its light.
    auto emit = [&](const glm::dvec3 &C, const glm::dvec3 &a, const glm::dvec3 &b, const glm::dvec3 &c, double sA,
                    double sB, double sC, double Sigma, const glm::dvec3 &sunL, const glm::dvec3 &skyL, double albedo,
                    uint64_t seed, uint32_t capN, double colW) -> bool {
        if (n >= capN || Sigma < 1.0) return false;
        const glm::dvec3 rel = C - eye;
        const double range = std::max(glm::length(rel), 1.0);
        const glm::dvec3 ae = toEnu(a), be = toEnu(b), ce = toEnu(c);
        const double iA = 1.0 / (sA * sA), iB = 1.0 / (sB * sB), iC = 1.0 / (sC * sC);
        auto Mx = [&](int p, int q) { return iA * ae[p] * ae[q] + iB * be[p] * be[q] + iC * ce[p] * ce[q]; };
        GpuDustCloud &d = out[n++];
        const glm::dvec3 re = toEnu(rel);
        d.c = glm::vec4(glm::vec3(re), (float)Sigma);
        d.ia = glm::vec4((float)Mx(0, 0), (float)Mx(1, 1), (float)Mx(2, 2), (float)(1.0 / (kTwoPi15 * sA * sB * sC)));
        const double lS = sunL.x + sunL.y + sunL.z, lI = skyL.x + skyL.y + skyL.z;
        const double share = std::min(lI / std::max(lI + lS, 1e-9), 0.999);
        const double seedK = 128.0 + (double)(fwMix(seed) & 127u);   // (smoke: seeds 128-255; < 128 = haboob walls)
        d.ib = glm::vec4((float)Mx(0, 1), (float)Mx(0, 2), (float)Mx(1, 2), (float)(4.0 * seedK + 2.0 + share));
        d.col = glm::vec4(glm::vec3(sunL + skyL), (float)(colW >= 0.0 ? colW : 2.0 + albedo));
        const double R = 5.5 * std::max(sA, std::max(sB, sC));
        d.cone = R >= 0.95 * range ? glm::vec4(0.0f, 0.0f, 0.0f, -2.0f)
                                   : glm::vec4(glm::vec3(re / range), (float)std::sqrt(1.0 - (R / range) * (R / range)));
        return true;
    };

    const double night = 1.0 - glm::clamp(((double)sunDirENU.w + 0.1) / 0.3, 0.0, 1.0);
    // The view: the points and flames off screen are not made (the camera's ENU look direction and a generous cone).
    const glm::dvec3 fwd(std::sin(glm::radians((double)camera.azDeg)) * std::cos(glm::radians((double)camera.elDeg)),
                         std::cos(glm::radians((double)camera.azDeg)) * std::cos(glm::radians((double)camera.elDeg)),
                         std::sin(glm::radians((double)camera.elDeg)));
    const double halfDiag = std::atan(std::tan(0.5 * glm::radians((double)camera.fovYDeg)) * std::sqrt(1.0 + 2.4 * 2.4));
    const double cosView = std::cos(std::min(halfDiag + glm::radians(4.0), glm::radians(89.0)));
    auto inView = [&](const glm::dvec3 &relEnu, double range, double radius) {
        const double c = glm::dot(relEnu, fwd) / std::max(range, 1.0);
        if (c >= cosView) return true;
        if (radius >= range) return true;
        // within the cone widened by the object's angular radius
        const double a = std::acos(std::clamp(c, -1.0, 1.0)) - std::asin(std::min(radius / range, 1.0));
        return a <= std::acos(cosView);
    };
    auto overHorizon = [&](const glm::dvec3 &P, const glm::dvec3 &rel, double range) {
        const glm::dvec3 uu = rel / range;
        const double tc = -glm::dot(eye, uu);
        return tc > 0.0 && tc < range && glm::length(eye + tc * uu) < kReF - 20.0;
    };

    // ── Smoke ──────────────────────────────────────────────────────────────────────────────────────────────────────
    for (FireNow &fn : firesNow)
    {
        const wx::Fire &f = fn.f;
        const wx::FireState &s = fn.s;
        if (s.ageS <= 0.0) continue;
        const glm::dvec3 G = llDir(f.latDeg, f.lonDeg);
        const glm::dvec3 eE = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), G) + glm::dvec3(1e-12, 0, 0));
        const glm::dvec3 nE = glm::cross(G, eE);
        const double ground = fn.groundM;
        auto dirAt = [&](double x, double y) { return glm::normalize(G + (eE * x + nE * y) / kReF); };
        auto at = [&](double x, double y, double z) { return dirAt(x, y) * (kReF + ground + z); };
        const glm::dvec2 head = wx::fireFrontAt(f, s, 0.0);
        const double sz = (double)f.size;
        // The source: the head front's width (wSrc) sets how much smoke (its optical depth seen side-on near the source
        // at 5 m/s: tau0); the column it rises in is narrower (the front's smoke gathers into the updraft: sB0) and
        // widens as it entrains air.
        const double wSrc = std::max(150.0, 0.3 * s.halfWidthM) + 300.0 * sz;
        const double sB0 = std::max(120.0, 0.12 * s.halfWidthM) + 150.0 * sz;
        const double tau0 = (2.5 + 5.5 * sz) * (0.6 + 0.4 * f.fuel) * wildfireSmoke;
        const double Q = tau0 * std::sqrt(2.0 * kPiF) * wSrc * 5.0;   // cross-section emitted per second at activity 1
        // Its injection height: the fire's (Wildfire.cpp), lower in a strong wind (the column bends over).
        const double H = wx::fireInjectionHeightM(f, s, t);
        const double w0 = wx::fireUpdraftMps(f);
        const double tauMax = std::clamp((3.0 + 9.0 * sz) * 3600.0 * (0.6 + 0.4 * s.activity), 2.0 * 3600.0, 14.0 * 3600.0);
        const int K = fn.rangeKm < 150.0 ? 14 : fn.rangeKm < 500.0 ? 7 : fn.rangeKm < 1200.0 ? 4 : 2;
        int emitted = 0;
        glm::dvec2 lastXY = head;
        double lastZ = H;
        if (tau0 > 0.02 && fn.rangeKm < 2500.0)
        {
            // Two layers: most of the smoke at the injection height, a quarter detrained low (in the boundary layer, its
            // own wind: the plume fans out with height). Far fires: one.
            const bool two = fn.rangeKm < 800.0 && H > 1200.0;
            for (int br = 0; br < (two ? 2 : 1); ++br)
            {
                const double Hb = br == 0 ? H : 0.4 * H;
                const double massK = two ? (br == 0 ? 0.75 : 0.25) : 1.0;
                const double Tr = Hb / w0;
                // Ages of the knots (s): the rise finely (near fires), then geometric out to tauMax.
                double taus[24];
                int nt = 0;
                taus[nt++] = 0.0;
                if (K >= 7)
                    for (double fx : {0.25, 0.6, 1.2, 2.2})
                        taus[nt++] = fx * Tr;
                {
                    const double ta = std::max(taus[nt - 1], 60.0);
                    const int rest = std::max(1, K - (nt - 1));
                    for (int j = 1; j <= rest; ++j)
                        taus[nt++] = ta * std::pow(tauMax / ta, (double)j / rest);
                }
                // Walk the streak: each stretch carried by the wind at its height at the moment it travelled.
                glm::dvec2 xy = wx::fireSmokeSource(f, s, t);
                double z0 = wx::fireSmokeRiseM(0.0, Hb, w0);
                glm::dvec3 P0 = at(xy.x, xy.y, z0);
                double sb0 = sB0, sz0 = 0.5 * sB0;
                for (int k = 0; k + 1 < nt; ++k)
                {
                    const double ta = taus[k], tb = taus[k + 1], tm = 0.5 * (ta + tb);
                    if (tm > s.ageS) break;   // (a young fire: its streak is as old as it is)
                    const double zb = wx::fireSmokeRiseM(tb, Hb, w0);
                    const glm::dvec2 u = wx::windAt(dirAt(xy.x, xy.y), 0.5 * (z0 + zb), t - tm);
                    xy += u * (tb - ta);
                    const glm::dvec3 P1 = at(xy.x, xy.y, zb);
                    // Width: the source, then the plume's turbulent spread; thin vertically (it flattens).
                    const double sb1 = sB0 + 0.25 * zb + 1.1 * std::pow(tb, 0.85);
                    const double sz1 = std::min(sb1, 120.0 + 0.22 * Hb + 0.12 * std::pow(tb, 0.85));
                    const glm::dvec3 seg = P1 - P0;
                    const double len = std::max(glm::length(seg), 1.0);
                    const glm::dvec3 a = seg / len;
                    glm::dvec3 b = glm::cross(a, glm::normalize(P0));
                    b = glm::length(b) > 1e-6 ? glm::normalize(b) : eE;
                    const glm::dvec3 c = glm::cross(a, b);
                    const double sb = 0.5 * (sb0 + sb1), szv = 0.5 * (sz0 + sz1);
                    // c is near vertical for a flat stretch (its thickness: thin, it flattens) and near horizontal along
                    // the wind for the rising column: a curtain along the front, thin in the wind's direction (it was as
                    // thick as the front is wide: a squat ball over a big fire).
                    const double cUp = std::fabs(glm::dot(c, glm::normalize(P0)));
                    const double thinA = std::min(sb, 200.0 + 0.15 * zb + 0.6 * std::pow(tb, 0.85));
                    const double sC = glm::mix(thinA, szv, cUp);
                    // The mass: what the fire made then (its activity at that moment), thinning slowly with age.
                    const double actThen = wx::fireStateAt(f, t - tm).activity;
                    const double Sigma = massK * Q * std::max(actThen, 0.05) * (tb - ta) * std::exp(-tm / (2.0 * 86400.0));
                    const glm::dvec3 Cc = 0.5 * (P0 + P1);
                    // Fresh smoke near the flaming front carries black carbon (darker, a little brown); aged paler grey.
                    const double age01 = glm::smoothstep(0.0, 3.0 * 3600.0, tm);
                    const glm::dvec3 tint = glm::mix(glm::dvec3(0.60, 0.58, 0.55), glm::dvec3(0.74, 0.73, 0.71), age01);
                    const double alb = glm::mix(0.38, 0.55, age01);
                    glm::dvec3 sunC;
                    double amb;
                    sunLightAt(Cc, sunC, amb);
                    const glm::dvec3 sunL = 4.0 * kPiF * alb * tint * sunC;
                    const glm::dvec3 skyL = 4.0 * kPiF * alb * tint * (0.18 * amb) * glm::dvec3(0.80, 0.79, 0.76);
                    if (emit(Cc, a, b, c, std::max((k == 0 ? 0.3 : 0.5) * len, 0.6 * sC), (k == 0 ? 0.6 : 1.0) * sb, sC,
                             Sigma, sunL, skyL, alb, f.id ^ (uint64_t)(k + 1 + 64 * br), cap, -1.0))
                        ++emitted;
                    P0 = P1;
                    sb0 = sb1;
                    sz0 = sz1;
                    z0 = zb;
                    if (br == 0)
                    {
                        lastXY = xy;
                        lastZ = zb;
                        if (fn.knots.size() < 24) fn.knots.push_back(glm::vec4((float)xy.x, (float)xy.y, (float)zb, (float)tb));
                    }
                }
            }
            // The pall: the smoke older than the streak, carried on downwind and spread wide and flat. MASS-CONSERVING:
            // what the fire made (at its mean activity) over its life past the streak, less ~4 days of removal; a column
            // of ~0.01-0.05 from above for one big fire, so a fire season's palls overlap into the regional haze.
            {
                const double kRem = 4.0 * 86400.0;
                const double older = std::max(s.ageS - tauMax, 0.0);
                const double dt = std::min(older, 36.0 * 3600.0);
                const glm::dvec2 uP = wx::windAt(dirAt(lastXY.x, lastXY.y), lastZ, t - tauMax - 0.5 * dt);
                const glm::dvec2 off = lastXY + uP * (0.5 * dt);
                const double sh = std::min(20e3 + 0.25 * glm::length(uP) * dt + 1.1 * std::pow(tauMax, 0.85), 350e3);
                const double zP = 0.8 * H + 200.0;
                const double Sigma = Q * 0.6 * kRem * (1.0 - std::exp(-older / kRem));
                const double sAlong = 1.4 * sh;
                const double tauTop = Sigma / (2.0 * kPiF * sAlong * sh);
                const glm::dvec3 Cp = at(off.x, off.y, zP);
                const glm::dvec3 upP = glm::normalize(Cp);
                glm::dvec3 wv = eE * uP.x + nE * uP.y;
                wv = glm::length(wv) > 1e-6 ? wv / glm::length(wv) : nE;
                const glm::dvec3 ap = glm::normalize(wv - upP * glm::dot(wv, upP));
                const glm::dvec3 bp = glm::cross(upP, ap);
                const glm::dvec3 tintP(0.76, 0.75, 0.73);
                const double albP = 0.55;
                glm::dvec3 sunC;
                double amb;
                sunLightAt(Cp, sunC, amb);
                if (tauTop > 0.002 && emit(Cp, ap, bp, upP, sAlong, sh, 0.35 * H + 400.0, Sigma,
                                          4.0 * kPiF * albP * tintP * sunC,
                                          4.0 * kPiF * albP * tintP * (0.18 * amb) * glm::dvec3(0.80, 0.79, 0.76), albP,
                                          f.id ^ 0xFA11ull, cap, -1.0))
                    ++emitted;
            }
        }
        fn.gauss = emitted;
    }
    // (one range with the haboobs drawn just before: weatherFillHaboobs sets aerosolFirst)
    {
        const uint32_t f0 = std::min(first, aerosolFirst);
        const uint32_t cnt = n - f0;
        if (cnt > 0) smokeHdrBits = (f0 << 8u) | (cnt << 18u);
    }

    // ── The front: points (kFireBuf), the flames close up (emissive Gaussians AFTER the smoke's range: they must not
    // shadow the ground as smoke, smokeSunTransmit), and the glow (engine lights) ─────────────────────────────────────
    GpuFireball *fo = kFireMapped ? reinterpret_cast<GpuFireball *>(static_cast<char *>(kFireMapped) + 16) : nullptr;
    const uint32_t ptBudget = std::min<uint32_t>(kFirePointBudget, kFireballMax - std::min(kFireCount, kFireballMax));
    const uint32_t ptStart = kFireCount;
    const uint32_t flameCap = std::min<uint32_t>(kMaxDustClouds, n + 96u);
    int lightsAdded = 0;
    const double lodScale = std::max((double)fireLodScale, 1.0);
    for (FireNow &fn : firesNow)
    {
        const wx::Fire &f = fn.f;
        const wx::FireState &s = fn.s;
        if (s.activity <= 0.0 || fn.reachK <= 0.0) continue;
        const glm::dvec3 G = llDir(f.latDeg, f.lonDeg);
        const glm::dvec3 eE = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), G) + glm::dvec3(1e-12, 0, 0));
        const glm::dvec3 nE = glm::cross(G, eE);
        const double ground = fn.groundM;
        const float coarse0 = cpuTerrainHeightM((float)f.latDeg, (float)f.lonDeg);
        auto at = [&](double x, double y, double z) {
            const glm::dvec3 d = glm::normalize(G + (eE * x + nE * y) / kReF);
            return d * (kReF + ground + z);
        };
        // The flames, and how bright their light is per metre of front (cd/m) at heat 1, by the view's elevation (from
        // the side the flame's height shows, from above the flaming zone's depth).
        const double hFl = wx::fireFlameHeightM(f, s), dFl = wx::fireFlameDepthM(f);
        const glm::dvec3 rel0 = at(0, 0, 0) - eye;
        const double r0 = std::max(glm::length(rel0), 1.0);
        const double sinV = std::clamp(glm::dot(-rel0 / r0, G), 0.0, 1.0);
        const double Ipm = kFlameLum * (hFl * std::sqrt(1.0 - sinV * sinV) + dFl * sinV) * s.activity;
        // The fire on the ground by day: against a sunlit ground the points stand out ~3.7 magnitudes less (a flame's
        // luminance is only ~5-10x a sunlit forest's).
        const double sinSun = glm::dot(G, sunE);
        const double dayFire = glm::smoothstep(-0.05, 0.12, sinSun);
        const double daySupp = 1.0 + 30.0 * dayFire;
        // The perimeter (for the points' levels) and the fire's extent (for the view test).
        double perim = 0.0;
        {
            glm::dvec2 prev = wx::fireFrontAt(f, s, 0.0);
            for (int k = 1; k <= 32; ++k)
            {
                const glm::dvec2 q = wx::fireFrontAt(f, s, 2.0 * kPiF * k / 32.0);
                perim += glm::length(q - prev);
                prev = q;
            }
        }
        const double radiusM = 0.5 * (s.headM + s.backM) * 1.3 + 500.0;
        const glm::dvec3 Cf = at(0.5 * (s.headM - s.backM) * std::sin(s.windAz), 0.5 * (s.headM - s.backM) * std::cos(s.windAz), 0.0);
        const glm::dvec3 relF = Cf - eye;
        const double rangeF = std::max(glm::length(relF), 1.0);
        const bool fireVisible = inView(toEnu(relF), rangeF, radiusM);
        const double lightK = (double)wildfireLight * fn.reachK;

        // ── Points ──
        if (fo && fireVisible && lightK > 0.0)
        {
            const double pxs = kFirePointPx * pixFull * lodScale;   // radians between points on screen
            // The front's base segments (2^6 round); each subdivided as finely as its nearest point's range asks.
            constexpr int kBaseLv = 6;
            constexpr uint32_t kSeg = 1u << kBaseLv, kPerSeg = 1u << (kFireFineLevels - kBaseLv);
            glm::dvec2 segP[kSeg + 1];
            for (uint32_t sg = 0; sg <= kSeg; ++sg)
                segP[sg] = wx::fireFrontAt(f, s, 2.0 * kPiF * sg / kSeg);
            const double coarseTerr = coarse0;
            for (uint32_t sg = 0; sg < kSeg && kFireCount - ptStart < ptBudget; ++sg)
            {
                // (per segment: the coarse DEM's slope from the ignition point at its ends, the air along the view)
                auto dzAt = [&](const glm::dvec2 &q) {
                    const double latP = f.latDeg + q.y / 111195.0;
                    const double lonP = f.lonDeg + q.x / (111195.0 * std::max(std::cos(glm::radians(f.latDeg)), 0.05));
                    return (double)cpuTerrainHeightM((float)latP, (float)lonP) - coarseTerr;
                };
                const double dzA = dzAt(segP[sg]), dzB = dzAt(segP[sg + 1]);
                const glm::dvec3 A = at(segP[sg].x, segP[sg].y, 15.0 + dzA), B = at(segP[sg + 1].x, segP[sg + 1].y, 15.0 + dzB);
                const double segLen = glm::length(B - A);
                const glm::dvec3 Cm = 0.5 * (A + B);
                const glm::dvec3 relM = Cm - eye;
                const double rM = std::max(glm::length(relM), 1.0);
                if (!inView(toEnu(relM), rM, 0.6 * segLen + 300.0)) continue;
                const double rMin = std::max(rM - 0.6 * segLen - 300.0, 5.0);
                const double kMax = std::log2(std::max(perim, 1.0) / (pxs * rMin)) + 1.0;
                const int lvMax = std::clamp((int)std::ceil(kMax), 0, kFireFineLevels);
                const uint32_t step = 1u << (kFireFineLevels - lvMax);
                const double ext = atmExtinctionMag(eye, relM / rM, rM, (double)extinctionCoeff);
                for (uint32_t J = sg * kPerSeg; J < (sg + 1) * kPerSeg && kFireCount - ptStart < ptBudget; J += step)
                {
                    const int lv = fineLevelOf(J);
                    const uint64_t h = fwMix(f.id ^ ((uint64_t)J * 0x9E3779B97F4A7C15ull));
                    // Its place on the front: its fine index jittered by a quarter of its own level's spacing.
                    const double jit = (fwU(h) - 0.5) * 0.5 * (double)(1u << (kFireFineLevels - lv));
                    const double a = 2.0 * kPiF * ((double)J + jit) / (double)(1u << kFireFineLevels);
                    double dsda = 0.0;
                    const glm::dvec2 q = wx::fireFrontAt(f, s, a, &dsda);
                    const double dz = dzA + (dzB - dzA) * std::clamp(((double)J + jit) / kPerSeg - sg, 0.0, 1.0);
                    const glm::dvec3 P = at(q.x, q.y, 15.0 + dz);
                    const glm::dvec3 rel = P - eye;
                    const double range = std::max(glm::length(rel), 5.0);
                    // Its level now: from its own range (continuous), the next level's points fading in.
                    const double kHere = std::clamp(std::log2(std::max(perim, 1.0) / (pxs * range)), 0.0, (double)kFireFineLevels);
                    const int K = (int)std::floor(kHere);
                    const double fr = (K >= kFireFineLevels) ? 0.0 : kHere - K;
                    double wa;
                    if (lv <= K) wa = 1.0 - 0.5 * fr;
                    else if (lv == K + 1) wa = 0.5 * fr;
                    else continue;
                    if (wa <= 0.0) continue;
                    const glm::dvec3 relE = toEnu(rel);
                    if (glm::dot(relE, fwd) / range < cosView) continue;
                    if (overHorizon(P, rel, range)) continue;
                    const double dA = 2.0 * kPiF / std::ldexp(1.0, K) * wa;   // the front angle it stands for
                    const double ds = dsda * dA;                               // ... in metres
                    const double heat = wx::fireFrontHeat(f, s, a, t);
                    // Flicker: many flames in one point average out (amplitude ~1/sqrt of the front it stands for).
                    const double amp = std::clamp(0.55 * std::sqrt(25.0 / std::max(ds, 1.0)), 0.07, 0.55);
                    auto flick = [&](uint64_t hh, double slow) {
                        const double w1 = (5.0 + 6.0 * fwU(hh ^ 11)) * slow, w2 = (13.0 + 10.0 * fwU(hh ^ 12)) * slow;
                        const double w3 = 1.5 + 1.5 * fwU(hh ^ 13);
                        return 1.0 + amp * (0.6 * std::sin(t * w1 + 6.2832 * fwU(hh ^ 14)) +
                                            0.4 * std::sin(t * w2 + 6.2832 * fwU(hh ^ 15)) * std::sin(t * w3 + 1.3));
                    };
                    auto put = [&](const glm::dvec3 &Pp, double I, const glm::vec3 &col, double fl) {
                        if (kFireCount >= kFireballMax || kFireCount - ptStart >= ptBudget) return;
                        const glm::dvec3 rp = Pp - eye;
                        const double rr = std::max(glm::length(rp), 5.0);
                        const double E = I / (rr * rr);
                        if (!(E > 0.0)) return;
                        const double mag = -2.5 * std::log10(E / kLuxMag0) + ext;
                        const double phys = 0.008 * std::pow(10.0, 0.4 * (6.0 - mag)) / daySupp * lightK;
                        const double flare = std::min(fireShoulder(phys) * fl, 10.5);
                        if (flare < 2e-4) return;
                        fo[kFireCount++] = {glm::vec4(glm::vec3(toEnu(rp) / rr), (float)flare), glm::vec4(col, (float)rr)};
                    };
                    // The flames: yellower where hotter.
                    const float hc = (float)std::clamp(heat, 0.0, 1.0);
                    put(P, Ipm * heat * ds, glm::mix(glm::vec3(1.0f, 0.40f, 0.10f), glm::vec3(1.0f, 0.66f, 0.28f), hc), flick(h, 1.0));
                    // Embers glowing behind the front (the burning-out zone: logs, stumps), redder, dimmer, slower.
                    if (fwU(h ^ 21) < 0.7)
                    {
                        const double depth = (0.015 + 0.22 * fwU(h ^ 22) * fwU(h ^ 22)) * glm::length(q - glm::dvec2(0.5 * (s.headM - s.backM) * std::sin(s.windAz), 0.5 * (s.headM - s.backM) * std::cos(s.windAz)));
                        const glm::dvec2 cxy(0.5 * (s.headM - s.backM) * std::sin(s.windAz), 0.5 * (s.headM - s.backM) * std::cos(s.windAz));
                        const glm::dvec2 inw = glm::length(cxy - q) > 1.0 ? (cxy - q) / glm::length(cxy - q) : glm::dvec2(0.0);
                        const glm::dvec2 qe = q + inw * depth;
                        const glm::dvec3 Pe = at(qe.x, qe.y, 4.0 + dz);
                        const double eI = Ipm * ds * (0.06 + 0.12 * fwU(h ^ 23)) * std::pow(std::max(heat, 0.05), 0.3) * (dFl / std::max(hFl + dFl, 1.0));
                        put(Pe, eI, glm::vec3(1.0f, 0.24f, 0.05f), 1.0 + 0.4 * (flick(h ^ 0xE3, 0.12) - 1.0));
                    }
                    // Spot fires a little ahead of the head (embers carried by the wind).
                    if (std::cos(a) > 0.5 && fwU(h ^ 31) < 0.14)
                    {
                        const glm::dvec2 w2(std::sin(s.windAz), std::cos(s.windAz));
                        const glm::dvec2 qs = q + w2 * (60.0 + 450.0 * fwU(h ^ 32) * fwU(h ^ 32)) +
                                              glm::dvec2(w2.y, -w2.x) * (fwU(h ^ 33) - 0.5) * 200.0;
                        put(at(qs.x, qs.y, 10.0 + dz), Ipm * heat * ds * 0.3, glm::vec3(1.0f, 0.5f, 0.15f), flick(h ^ 0x5F, 1.3));
                    }
                }
            }
        }

        // ── Flames close up: emissive Gaussians standing on the front (by day too: the points are dimmed there) ──
        const double kFlameSky = 1.0;   // x the flame's luminance in the sky's units (pi L / the Sun's illuminance)
        if (fireVisible && fn.rangeKm < 40.0 && n < flameCap && hFl > 0.5)
        {
            const double fadeR = 1.0 - glm::smoothstep(25.0, 40.0, fn.rangeKm);
            // Spacing: at least twice the flame height, and ~40 px; the level cross-faded like the points'.
            const double sp = std::max(2.0 * hFl, rangeF * pixFull * 40.0);
            const double kF = std::clamp(std::log2(std::max(perim, 1.0) / sp), 0.0, 9.0);
            const int K = (int)std::floor(kF);
            const double fr = kF - K;
            const uint32_t nAt = 1u << (K + 1);
            for (uint32_t j = 0; j < nAt && n < flameCap; ++j)
            {
                const bool isNew = (j & 1u) != 0u;
                const double wa = isNew ? 0.5 * fr : 1.0 - 0.5 * fr;
                if (wa <= 0.01) continue;
                const uint64_t h = fwMix(f.id ^ 0xF1A3E000ull ^ (uint64_t)j * 0x9E3779B97F4A7C15ull);
                const double a = 2.0 * kPiF * ((double)j + (fwU(h) - 0.5) * 0.5) / (double)nAt;
                const double heat = wx::fireFrontHeat(f, s, a, t);
                if (heat < 0.08) continue;
                double dsda = 0.0;
                const glm::dvec2 q = wx::fireFrontAt(f, s, a, &dsda);
                const double ds = dsda * 2.0 * kPiF / std::ldexp(1.0, K);
                const double hh = hFl * (0.6 + 0.6 * heat) * (0.8 + 0.4 * std::sin(t * (2.0 + 2.0 * fwU(h ^ 3)) + 6.28 * fwU(h ^ 4)));
                // (on the ground the points stand on: the full DEM at the ignition + the coarse DEM's change)
                const double latP = f.latDeg + q.y / 111195.0;
                const double lonP = f.lonDeg + q.x / (111195.0 * std::max(std::cos(glm::radians(f.latDeg)), 0.05));
                const double dz = (double)cpuTerrainHeightM((float)latP, (float)lonP) - (double)coarse0;
                const glm::dvec3 P = at(q.x, q.y, 0.45 * hh + dz);
                const glm::dvec3 rel = P - eye;
                const double range = std::max(glm::length(rel), 1.0);
                if (!inView(toEnu(rel), range, ds)) continue;
                const glm::dvec3 upP = glm::normalize(P);
                double dsda2 = 0.0;
                const glm::dvec2 q2 = wx::fireFrontAt(f, s, a + 0.01, &dsda2);
                glm::dvec3 T = at(q2.x, q2.y, 0.45 * hh + dz) - P;
                T = glm::normalize(T - upP * glm::dot(T, upP) + glm::dvec3(1e-9, 0, 0));
                const glm::dvec3 Cx = glm::cross(T, upP);
                // (a tongue, not a tube: narrower than the spacing, so the front shows its gaps)
                const double sA = std::max(0.3 * ds, 0.4 * hh), sV = 0.33 * hh, sC = std::max(0.25 * dFl, 2.0);
                const double Sigma = 2.0 * kPiF * sA * sV;   // the side-on peak optical depth 1: the radiance is its colour
                const double rad = kFlameSky * kPiF * kFlameLum / kSunLux * std::pow(std::min(heat, 1.2), 1.5) * wa * fadeR * lightK;
                const glm::vec3 col = glm::mix(glm::vec3(1.0f, 0.36f, 0.08f), glm::vec3(1.0f, 0.62f, 0.22f), (float)std::clamp(heat, 0.0, 1.0));
                if (emit(P, T, upP, Cx, sA, sV, sC, Sigma, glm::dvec3(col) * rad, glm::dvec3(0.0), 0.0, h, flameCap, 1.0))
                    ++fireFlamesLast;
            }
        }

        // ── The glow: the nearest fires' light on their smoke, the cloud and the ground (engine lights, eye-adapted by
        // engineLightsFlush). Its intensity is the front's light (a 24-point sum), x the share that escapes the
        // vegetation, the terrain and the smoke round it; faded out with range (from orbit the points carry the fire).
        if (lightsAdded < 6 && night > 0.05 && fn.rangeKm < 320.0)
        {
            double Itot = 0.0;
            for (int k = 0; k < 24; ++k)
            {
                const double a = 2.0 * kPiF * (k + 0.5) / 24.0;
                double dsda = 0.0;
                wx::fireFrontAt(f, s, a, &dsda);
                Itot += kFlameLum * hFl * s.activity * wx::fireFrontHeat(f, s, a, t) * dsda * (2.0 * kPiF / 24.0);
            }
            const double fadeL = 1.0 - glm::smoothstep(120.0, 320.0, fn.rangeKm);
            // (the share that escapes: ~6%. At a quarter, physical for open ground, a big fire lit the haze and the
            // ground brown over a whole view from 175 km, the user's "too bright": the flames' light is mostly
            // blocked by the trees, the terrain and the smoke round them, and the night's lift is not physical)
            const double Il = 0.06 * Itot / kSunLux * lightK * night * fadeL;   // suns x m^2
            if (Il > 0.05)
            {
                const glm::dvec2 head = wx::fireFrontAt(f, s, 0.0);
                const glm::dvec3 Pl = at(head.x, head.y, 40.0 + 0.5 * hFl);
                engineLightAdd(fwEcefToEci(Pl, gr), Il, glm::vec3(1.0f, 0.45f, 0.14f),
                               std::max(150.0, 0.15 * std::max(s.halfWidthM, 200.0)), glm::dvec3(0.0), 0.0, 0.0, false, 0.0);
                ++lightsAdded;
                ++fireLightsLast;
            }
        }
        fn.points = 0;
    }
    firePointsLast = kFireCount - ptStart;
    if (kFireMapped) static_cast<uint32_t *>(kFireMapped)[0] = kFireCount;   // (the fireball pass reads this count)
    // The points' budget: when a crowd of fires asks for more, their spacing widens (continuously: the levels
    // cross-fade); it relaxes back slowly.
    if (firePointsLast >= (uint32_t)(0.95 * ptBudget))
        fireLodScale = std::min(fireLodScale * 1.12f, 64.0f);
    else if (firePointsLast < (uint32_t)(0.6 * ptBudget) && fireLodScale > 1.0f)
        fireLodScale = std::max(fireLodScale / 1.03f, 1.0f);
}

nlohmann::json SatelliteSim::wildfiresJson(bool world)
{
    nlohmann::json o;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    o["rate"] = wildfireRate;
    o["smoke_gain"] = wildfireSmoke;
    o["light_gain"] = wildfireLight;
    o["near_count"] = firesNow.size();
    o["smoke_range"] = {smokeHdrBits >> 8u & 1023u, smokeHdrBits >> 18u & 1023u};
    o["points"] = firePointsLast;
    o["flames"] = fireFlamesLast;
    o["lights"] = fireLightsLast;
    o["point_lod_scale"] = fireLodScale;
    nlohmann::json a = nlohmann::json::array();
    for (size_t i = 0; i < firesNow.size() && i < 12; ++i)
    {
        const FireNow &f = firesNow[i];
        nlohmann::json r;
        r["id"] = f.f.id;
        r["lat"] = f.f.latDeg;
        r["lon"] = f.f.lonDeg;
        r["biome"] = wx::biomeName(biomeMap.at(f.f.latDeg, f.f.lonDeg));
        r["range_km"] = f.rangeKm;
        r["age_h"] = f.s.ageS / 3600.0;
        r["life_d"] = f.f.life / 86400.0;
        r["size"] = f.f.size;
        r["pyro"] = f.f.pyro > 0.5f;
        r["activity"] = f.s.activity;
        r["head_km"] = f.s.headM / 1000.0;
        r["plume_top_m"] = f.s.plumeTopM;
        r["flame_h_m"] = wx::fireFlameHeightM(f.f, f.s);
        r["gaussians"] = f.gauss;
        if (i == 0)
        {
            nlohmann::json kn = nlohmann::json::array();
            for (const glm::vec4 &k : f.knots)
                kn.push_back({std::round(k.x), std::round(k.y), std::round(k.z), std::round(k.w)});
            r["smoke_knots_xyz_age"] = kn;
            const glm::dvec3 sm = wx::fireSmokeAt(f.f, f.s, t, 5400.0, wx::fireInjectionHeightM(f.f, f.s, t));
            r["smoke_at_1p5h"] = {std::round(sm.x), std::round(sm.y), std::round(sm.z)};
        }
        r["ground_m"] = f.groundM;
        {
            const glm::dvec3 G = llDir(f.f.latDeg, f.f.lonDeg);
            const glm::dvec2 w10 = wx::windAt(G, 10.0, t), w3 = wx::windAt(G, 3000.0, t);
            r["wind10_en"] = {w10.x, w10.y};
            r["wind3km_en"] = {w3.x, w3.y};
        }
        a.push_back(r);
    }
    o["near"] = a;
    if (world)
    {
        std::vector<wx::Fire> all;
        wx::firesWorld(t, biomeMap, wildfireRate, all);
        o["world_count"] = all.size();
        int big = 0;
        std::array<int, (int)wx::Biome::Count> byBiome{};
        for (const wx::Fire &f : all)
        {
            big += f.size > 0.5f;
            byBiome[(int)biomeMap.at(f.latDeg, f.lonDeg)]++;
        }
        o["world_big"] = big;
        nlohmann::json b;
        for (int k = 0; k < (int)wx::Biome::Count; ++k)
            if (byBiome[k]) b[wx::biomeName((wx::Biome)k)] = byBiome[k];
        o["world_by_biome"] = b;
    }
    return o;
}
