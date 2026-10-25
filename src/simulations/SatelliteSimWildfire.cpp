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
constexpr double kFirePointPx = 3.5;        // the points' spacing on screen (full-res px; 2 from afar)
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
// A physical flare (the satellites' effectFlare scale) compressed like a sensor's shoulder toward kFireFlareMax.
// Physically a fire's points span ~6 decades (a 6-m stretch of flame 1 km off to a km of a grass fire's back from
// orbit), and the brightest are -10 mag: no display holds that. Compressed so a big fire's head from LEO (raw ~3e3) is
// ~0.4 (a dot of about mag 2.3), one 5 km off at night (~8e4) ~0.8, the faint back and embers from orbit (~30) ~0.06;
// a flare of 0.5 is the point style's white peak (pointRefMag 1.5), so no point is wider than a star's core. Until
// 2026-10-24 it approached 8 (16x that white, each point a swollen disc) with the head from LEO at ~5.5: the user's "way
// too bright, overwhelming even from space".
constexpr double kFireFlareMax = 0.9;
double fireShoulder(double x)
{
    if (!(x > 0.0)) return 0.0;
    return kFireFlareMax * (1.0 - std::exp(-std::pow(x / 1.0e4, 0.45)));
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
    firePtCount = 0;
    flameCount = 0;
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
        FPROF_SCOPE("fire.born");
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
            // The rows split over the storm catalog's thread pool, appended in row order: the same fires in the same
            // order as one firesBorn (~4 ms on the main thread after every jump; perf4-cpu 2026-10-24).
            const double dLat = fireCacheR / 111.2 + 1.0;
            const int r0 = (int)std::floor(fireCacheLat - dLat), r1 = (int)std::floor(fireCacheLat + dLat);
            const int nRows = r1 - r0 + 1;
            if (stormCatalog && nRows >= 8)
            {
                const int parts = std::min(nRows, 16);
                std::vector<std::vector<wx::Fire>> part((size_t)parts);
                stormCatalog->parallelFor((size_t)parts, [&](size_t k) {
                    const int a = r0 + (int)((int64_t)nRows * (int64_t)k / parts);
                    const int b = r0 + (int)((int64_t)nRows * (int64_t)(k + 1) / parts) - 1;
                    wx::firesBornRows(b0, b1, a, b, fireCacheLat, fireCacheLon, fireCacheR, biomeMap, wildfireRate, part[k]);
                });
                for (auto &v : part)
                    fireCache.insert(fireCache.end(), v.begin(), v.end());
            }
            else
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
    fprof::Scope fireStateScope("fire.state");
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
    const uint32_t cap = std::min<uint32_t>(kMaxDustClouds, n + 384u);
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
    // From the whole ACTIVE front (2026-10-24, the user: "smoke trails only generate from singular points, and not the full
    // fire circle / arc"): the front's heat is sampled round it and cut into S sections of equal heat; each section is a
    // source at its heat-weighted centre, as wide as the section, carrying its share of the smoke, and rises to its own
    // layer (a weaker section injects lower). Each source's streak walks the same wind field, so they leave the front as a
    // curtain along it and merge downwind into one plume. It was one streak from a point 250 m downwind of the head.
    fireStateScope.end();
    fireSmokeSources = 0;
    fprof::Scope fireSmokeScope("fire.smoke");
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
        const double sz = (double)f.size;
        const double tau0 = (2.5 + 5.5 * sz) * (0.6 + 0.4 * f.fuel) * wildfireSmoke;
        if (!(tau0 > 0.02) || fn.rangeKm >= 2500.0)
        {
            fn.gauss = 0;
            continue;
        }
        // The fire's smoke production (cross-section a second at activity 1): from the head front's width, as before.
        const double wSrc = std::max(150.0, 0.3 * s.halfWidthM) + 300.0 * sz;
        const double Q = tau0 * std::sqrt(2.0 * kPiF) * wSrc * 5.0;
        const double H = wx::fireInjectionHeightM(f, s, t);
        const double w0 = wx::fireUpdraftMps(f);
        const double tauMax = std::clamp((3.0 + 9.0 * sz) * 3600.0 * (0.6 + 0.4 * s.activity), 2.0 * 3600.0, 14.0 * 3600.0);
        const int S = fn.rangeKm < 150.0 ? 5 : fn.rangeKm < 500.0 ? 3 : fn.rangeKm < 1200.0 ? 2 : 1;
        const int K = fn.rangeKm < 150.0 ? 9 : fn.rangeKm < 500.0 ? 6 : fn.rangeKm < 1200.0 ? 4 : 2;
        // The front's heat round it (48 samples: heat x length).
        constexpr int kHs = 48;
        glm::dvec2 hq[kHs];
        double hw[kHs], hTot = 0.0;
        for (int k = 0; k < kHs; ++k)
        {
            const double a = 2.0 * kPiF * (k + 0.5) / kHs;
            double dsda = 0.0;
            hq[k] = wx::fireFrontAt(f, s, a, &dsda);
            double la, lo;
            wx::fireOffsetLatLon(f, hq[k], la, lo);
            hw[k] = wx::fireFrontHeat(f, s, a, t) * glm::smoothstep(0.08, 0.3, (double)biomeMap.burnableAt(la, lo)) * dsda;
            hTot += hw[k];
        }
        // The sections: S runs of the cumulative heat starting at the arc's cold end (the angle with the least heat
        // before it), so a section never straddles the gap behind the fire.
        int k0 = 0;
        {
            double best = 1e300;
            for (int k = 0; k < kHs; ++k)
                if (hw[k] < best) { best = hw[k]; k0 = k; }
        }
        struct Src
        {
            glm::dvec2 c{0.0};
            double w = 0.0, m = 0.0, ext = 0.0;
        };
        Src src[5];
        int nS = 0;
        if (hTot > 1e-6)
        {
            double acc = 0.0;
            int si = 0;
            for (int j = 0; j < kHs; ++j)
            {
                const int k = (k0 + j) % kHs;
                src[si].c += hw[k] * hq[k];
                src[si].m += hw[k];
                acc += hw[k];
                if (acc >= hTot * (si + 1) / S - 1e-9 && si < S - 1) ++si;
            }
            for (int i = 0; i < S; ++i)
                if (src[i].m > 0.0) src[i].c /= src[i].m;
            // each section's extent: the heat-weighted rms distance of its samples from its centre
            acc = 0.0;
            si = 0;
            for (int j = 0; j < kHs; ++j)
            {
                const int k = (k0 + j) % kHs;
                if (src[si].m > 0.0) src[si].ext += hw[k] * glm::dot(hq[k] - src[si].c, hq[k] - src[si].c);
                acc += hw[k];
                if (acc >= hTot * (si + 1) / S - 1e-9 && si < S - 1) ++si;
            }
            for (int i = 0; i < S; ++i)
                if (src[i].m > 0.0)
                {
                    src[i].ext = std::sqrt(src[i].ext / src[i].m);
                    src[i].w = src[i].m / hTot;
                    src[nS++] = src[i];
                }
        }
        if (nS == 0)
        {
            src[0].c = wx::fireFrontAt(f, s, 0.0);
            src[0].w = 1.0;
            src[0].ext = 0.3 * s.halfWidthM;
            nS = 1;
        }
        // The wind at the ground carries each source's smoke off the flames (the flames stand upwind of it).
        glm::dvec2 wDir;
        {
            const glm::dvec2 u = wx::windAt(G, 10.0, t);
            wDir = glm::length(u) > 0.05 ? u / glm::length(u) : glm::dvec2(std::sin(s.windAz), std::cos(s.windAz));
        }
        int emitted = 0;
        glm::dvec2 lastXY = src[0].c;
        double lastZ = H;
        int mainSrc = 0;
        for (int i = 1; i < nS; ++i)
            if (src[i].w > src[mainSrc].w) mainSrc = i;
        // A quarter of the main source's smoke detrains low (the boundary layer's own wind: the plume fans out with
        // height); near fires only.
        const bool lowLayer = fn.rangeKm < 800.0 && H > 1200.0;
        for (int si = 0; si < nS + (lowLayer ? 1 : 0); ++si)
        {
            const bool low = si == nS;
            const Src &sc = src[low ? mainSrc : si];
            const double massK = low ? 0.2 * sc.w : (si == mainSrc && lowLayer ? sc.w - 0.2 * sc.w : sc.w);
            // (a weaker section's smoke rises less: its share of the fire's heat x S, 1 = an even share)
            const double Hb = low ? 0.4 * H : H * std::clamp(0.7 + 0.3 * std::sqrt(sc.w * nS), 0.55, 1.1);
            const double Tr = Hb / w0;
            // The source's width: the section's extent (a curtain along the front), and how wide its column starts.
            const double sB0 = std::max(120.0, 1.2 * sc.ext / std::sqrt((double)nS)) + 100.0 * sz;
            double taus[24];
            int nt = 0;
            taus[nt++] = 0.12 * Tr;   // (the smoke starts above the flames: the hot gas over them is clear)
            if (K >= 7)
                for (double fx : {0.25, 0.6, 1.2, 2.2})
                    taus[nt++] = fx * Tr;
            {
                const double ta = std::max(taus[nt - 1], 60.0);
                const int rest = std::max(1, K - (nt - 1));
                for (int j = 1; j <= rest; ++j)
                    taus[nt++] = ta * std::pow(tauMax / ta, (double)j / rest);
            }
            glm::dvec2 xy = sc.c + wDir * (120.0 + 0.4 * wx::fireFlameDepthM(f));
            double z0 = wx::fireSmokeRiseM(taus[0], Hb, w0);
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
                // c is near vertical for a flat stretch (its thickness) and near horizontal along the wind for the
                // rising column: a curtain along the front, thin in the wind's direction.
                const double cUp = std::fabs(glm::dot(c, glm::normalize(P0)));
                const double thinA = std::min(sb, 200.0 + 0.15 * zb + 0.6 * std::pow(tb, 0.85));
                const double sC = glm::mix(thinA, szv, cUp);
                const double actThen = wx::fireStateAt(f, t - tm).activity;
                const double Sigma = massK * Q * std::max(actThen, 0.05) * (tb - ta) * std::exp(-tm / (2.0 * 86400.0));
                const glm::dvec3 Cc = 0.5 * (P0 + P1);
                const double age01 = glm::smoothstep(0.0, 3.0 * 3600.0, tm);
                const glm::dvec3 tint = glm::mix(glm::dvec3(0.60, 0.58, 0.55), glm::dvec3(0.74, 0.73, 0.71), age01);
                const double alb = glm::mix(0.38, 0.55, age01);
                glm::dvec3 sunC;
                double amb;
                sunLightAt(Cc, sunC, amb);
                const glm::dvec3 sunL = 4.0 * kPiF * alb * tint * sunC;
                const glm::dvec3 skyL = 4.0 * kPiF * alb * tint * (0.18 * amb) * glm::dvec3(0.80, 0.79, 0.76);
                if (emit(Cc, a, b, c, std::max((k == 0 ? 0.3 : 0.5) * len, (k == 0 ? 0.25 : 0.6) * sC), (k == 0 ? 0.6 : 1.0) * sb, sC,
                         Sigma, sunL, skyL, alb, f.id ^ (uint64_t)(k + 1 + 64 * si), cap, -1.0))
                    ++emitted;
                P0 = P1;
                sb0 = sb1;
                sz0 = sz1;
                z0 = zb;
                if (si == mainSrc && !low)
                {
                    lastXY = xy;
                    lastZ = zb;
                    if (fn.knots.size() < 24) fn.knots.push_back(glm::vec4((float)xy.x, (float)xy.y, (float)zb, (float)tb));
                }
            }
            ++fireSmokeSources;
        }
        // The pall: the smoke older than the streaks, carried on downwind and spread wide and flat. MASS-CONSERVING:
        // what the fire made (at its mean activity) over its life past the streak, less ~4 days of removal; a column of
        // ~0.01-0.05 from above for one big fire, so a fire season's palls overlap into the regional haze.
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
    flameFirst = n;
    firePeakFlare = 0.0;
    fireFlareSum = 0.0;
    fireGlowSum = 0.0;
    int lightsAdded = 0;
    const double lodScale = std::max((double)fireLodScale, 1.0);
    // The display's exposure: the flames' Gaussians are capped in DISPLAYED light (they are emissive in the sky's units,
    // and the night exposure is ~5x the day's: at a physical 0.5 of a sunlit white they were white blobs at night).
    const double expo = std::max((double)skyExposure(), 1e-3);
    fireSmokeScope.end();
    fprof::Scope fireFrontScope("fire.front");
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
        // What can burn at a point of the front (the biome map's fuel less its built-up mask: lakes, towns, bare ground).
        auto burnK = [&](const glm::dvec2 &q) {
            double la, lo;
            wx::fireOffsetLatLon(f, q, la, lo);
            return glm::smoothstep(0.08, 0.3, (double)biomeMap.burnableAt(la, lo));
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
        const glm::dvec2 cxy(0.5 * (s.headM - s.backM) * std::sin(s.windAz), 0.5 * (s.headM - s.backM) * std::cos(s.windAz));
        const glm::dvec3 Cf = at(cxy.x, cxy.y, 0.0);
        const glm::dvec3 relF = Cf - eye;
        const double rangeF = std::max(glm::length(relF), 1.0);
        const bool fireVisible = inView(toEnu(relF), rangeF, radiusM);
        const double lightK = (double)wildfireLight * fn.reachK;
        // (the burnt area's smouldering spots glow whatever the fire's activity now: logs, stumps, roots)
        const double Ismoulder = kFlameLum * 0.6 * (0.3 + 0.7 * (double)f.fuel);
        const double spread = std::max(f.spreadMps, 0.02);

        // ── Points ──
        if (fo && fireVisible && lightK > 0.0)
        {
            // radians between points on screen: 3.5 px near, 2 px from afar (a front under ~100 m a pixel is a line of
            // light, not a string of beads; the light per point is in proportion: the total kept)
            const double pxs = kFirePointPx * pixFull * lodScale * (1.0 - 0.43 * glm::smoothstep(50.0e3, 300.0e3, rangeF));
            // The front's base segments (2^6 round); each subdivided as finely as its nearest point's range asks.
            constexpr int kBaseLv = 6;
            constexpr uint32_t kSeg = 1u << kBaseLv, kPerSeg = 1u << (kFireFineLevels - kBaseLv);
            glm::dvec2 segP[kSeg + 1];
            for (uint32_t sg = 0; sg <= kSeg; ++sg)
                segP[sg] = wx::fireFrontAt(f, s, 2.0 * kPiF * sg / kSeg);
            const double coarseTerr = coarse0;
            // The segments in parallel (perf4-cpu 2026-10-24: ~1.7 ms a frame close to a big fire on the main thread),
            // each into its own list, appended in segment order and cut at the budget: the same points in the same order.
            if (firePtSeg.size() < kSeg) firePtSeg.resize(kSeg);
            auto runSeg = [&](size_t sgi) {
                const uint32_t sg = (uint32_t)sgi;
                FirePtSeg &so = firePtSeg[sg];
                so.pts.clear();
                so.fl.clear();
                // (per segment: the coarse DEM's slope from the ignition point at its ends, the air along the view; the
                // GPU puts each point on the drawn ground, wildfire_ground.comp)
                auto dzAt = [&](const glm::dvec2 &q) {
                    double latP, lonP;
                    wx::fireOffsetLatLon(f, q, latP, lonP);
                    return (double)cpuTerrainHeightM((float)latP, (float)lonP) - coarseTerr;
                };
                const double dzA = dzAt(segP[sg]), dzB = dzAt(segP[sg + 1]);
                const glm::dvec3 A = at(segP[sg].x, segP[sg].y, 15.0 + dzA), B = at(segP[sg + 1].x, segP[sg + 1].y, 15.0 + dzB);
                const double segLen = glm::length(B - A);
                const glm::dvec3 Cm = 0.5 * (A + B);
                const glm::dvec3 relM = Cm - eye;
                const double rM = std::max(glm::length(relM), 1.0);
                // (a segment reaches inward too: the smouldering spots stand inside the burnt area)
                if (!inView(toEnu(relM), rM, 0.6 * segLen + 0.5 * radiusM)) return;
                const double rMin = std::max(rM - 0.6 * segLen - 300.0, 5.0);
                const double kMax = std::log2(std::max(perim, 1.0) / (pxs * rMin)) + 1.0;
                const int lvMax = std::clamp((int)std::ceil(kMax), 0, kFireFineLevels);
                const uint32_t step = 1u << (kFireFineLevels - lvMax);
                const double ext = atmExtinctionMag(eye, relM / rM, rM, (double)extinctionCoeff);
                for (uint32_t J = sg * kPerSeg; J < (sg + 1) * kPerSeg && so.pts.size() < ptBudget; J += step)
                {
                    const int lv = fineLevelOf(J);
                    const uint64_t h = fwMix(f.id ^ ((uint64_t)J * 0x9E3779B97F4A7C15ull));
                    // Its place on the front: its fine index jittered by a quarter of its own level's spacing.
                    const double jit = (fwU(h) - 0.5) * 0.5 * (double)(1u << (kFireFineLevels - lv));
                    const double a = 2.0 * kPiF * ((double)J + jit) / (double)(1u << kFireFineLevels);
                    double dsda = 0.0;
                    glm::dvec2 q = wx::fireFrontAt(f, s, a, &dsda);
                    const double dz = dzA + (dzB - dzA) * std::clamp(((double)J + jit) / kPerSeg - sg, 0.0, 1.0);
                    // Its level now: from the front point's range (continuous), the next level's points fading in.
                    const double range0 = std::max(glm::length(at(q.x, q.y, 15.0 + dz) - eye), 5.0);
                    const double kHere = std::clamp(std::log2(std::max(perim, 1.0) / (pxs * range0)), 0.0, (double)kFireFineLevels);
                    const int K = (int)std::floor(kHere);
                    const double fr = (K >= kFireFineLevels) ? 0.0 : kHere - K;
                    double wa;
                    if (lv <= K) wa = 1.0 - 0.5 * fr;
                    else if (lv == K + 1) wa = 0.5 * fr;
                    else continue;
                    if (wa <= 0.0) continue;
                    const double dA = 2.0 * kPiF / std::ldexp(1.0, K) * wa;   // the front angle it stands for
                    const double ds = dsda * dA;                               // ... in metres
                    const glm::dvec2 inw = glm::length(cxy - q) > 1.0 ? (cxy - q) / glm::length(cxy - q) : glm::dvec2(0.0);
                    const double bk = burnK(q);
                    const double heat = wx::fireFrontHeat(f, s, a, t) * bk;
                    // Flicker: many flames in one point average out (amplitude ~1/sqrt of the front it stands for).
                    const double amp = std::clamp(0.55 * std::sqrt(25.0 / std::max(ds, 1.0)), 0.07, 0.55);
                    auto flick = [&](uint64_t hh, double slow) {
                        const double w1 = (5.0 + 6.0 * fwU(hh ^ 11)) * slow, w2 = (13.0 + 10.0 * fwU(hh ^ 12)) * slow;
                        const double w3 = 1.5 + 1.5 * fwU(hh ^ 13);
                        return 1.0 + amp * (0.6 * std::sin(t * w1 + 6.2832 * fwU(hh ^ 14)) +
                                            0.4 * std::sin(t * w2 + 6.2832 * fwU(hh ^ 15)) * std::sin(t * w3 + 1.3));
                    };
                    // A point: its light (cd) at Pp, hAb metres over the ground. The range stored NEGATIVE marks it for
                    // wildfire_ground.comp, which moves it hAb over the drawn ground under it (hAb rides in the
                    // direction's length: 1 + hAb / 1000; kessler_fireballs.comp normalises it) and writes the range.
                    auto put = [&](const glm::dvec3 &Pp, double I, const glm::vec3 &col, double fl, double hAb) {
                        if (so.pts.size() >= ptBudget) return;
                        const glm::dvec3 rp = Pp - eye;
                        const double rr = std::max(glm::length(rp), 5.0);
                        if (glm::dot(toEnu(rp), fwd) / rr < cosView || overHorizon(Pp, rp, rr)) return;
                        const double E = I / (rr * rr);
                        if (!(E > 0.0)) return;
                        const double mag = -2.5 * std::log10(E / kLuxMag0) + ext;
                        const double phys = 0.008 * std::pow(10.0, 0.4 * (6.0 - mag)) / daySupp * lightK;
                        const double flare = fireShoulder(phys) * fl;
                        if (flare < 2e-4) return;
                        so.pts.push_back({glm::vec4(glm::vec3(toEnu(rp) / rr * (1.0 + hAb / 1000.0)), (float)flare),
                                          glm::vec4(col, (float)-rr)});
                        so.fl.push_back(flare);
                    };
                    // The flames on the active arc (a little in front of or behind the mean front: it is a ragged band
                    // tens of metres deep), yellower where hotter.
                    if (heat > 0.02)
                    {
                        const glm::dvec2 qf = q - inw * ((fwU(h ^ 41) - 0.35) * (0.6 * dFl + 10.0));
                        const float hc = (float)std::clamp(heat, 0.0, 1.0);
                        put(at(qf.x, qf.y, 15.0 + dz), Ipm * heat * ds,
                            glm::mix(glm::vec3(1.0f, 0.36f, 0.08f), glm::vec3(1.0f, 0.62f, 0.25f), hc), flick(h, 1.0),
                            0.5 * hFl * (0.6 + 0.6 * std::min(heat, 1.0)) + 1.0);
                    }
                    // Behind a burning stretch: the burning-out zone (logs, stumps) a few hundred metres deep, redder,
                    // dimmer, slower, fading with how long ago it burned.
                    if (heat > 0.05 && fwU(h ^ 21) < 0.45)
                    {
                        const double ago = (0.25 + 2.5 * fwU(h ^ 22) * fwU(h ^ 22)) * 3600.0;   // s since the front passed
                        const glm::dvec2 qe = q + inw * (dFl + spread * ago);
                        const double eI = Ipm * ds * 0.07 * std::sqrt(heat) * std::exp(-ago / 4000.0) * (dFl / std::max(hFl + dFl, 1.0));
                        put(at(qe.x, qe.y, 4.0 + dz), eI, glm::vec3(1.0f, 0.22f, 0.04f), 1.0 + 0.4 * (flick(h ^ 0xE3, 0.12) - 1.0), 1.5);
                    }
                    // The burn scar: a sparse field of smouldering spots over the whole burnt area (not a ring: each at
                    // a random depth toward the fire's centre), dimmer toward the old, burnt-out middle.
                    if (fwU(h ^ 24) < 0.06 && bk > 0.0)
                    {
                        const double u = 0.06 + 0.86 * fwU(h ^ 25);
                        const glm::dvec2 qs = q + (cxy - q) * u;
                        if (burnK(qs) > 0.0)
                        {
                            const double sI = Ismoulder * ds * 0.03 * std::pow(1.0 - 0.8 * u, 2.0) * (0.3 + 0.7 * fwU(h ^ 26)) *
                                              (0.5 + 0.5 * s.activity);
                            put(at(qs.x, qs.y, 2.0 + dz), sI, glm::vec3(1.0f, 0.18f, 0.03f), 1.0 + 0.5 * (flick(h ^ 0x5C, 0.05) - 1.0), 1.0);
                        }
                    }
                    // Spot fires ahead of the head (embers carried by the wind), in small clusters.
                    if (std::cos(a) > 0.55 && heat > 0.1 && fwU(h ^ 31) < 0.08)
                    {
                        const glm::dvec2 w2(std::sin(s.windAz), std::cos(s.windAz));
                        const glm::dvec2 qs = q + w2 * (80.0 + 600.0 * fwU(h ^ 32) * fwU(h ^ 32)) +
                                              glm::dvec2(w2.y, -w2.x) * (fwU(h ^ 33) - 0.5) * 250.0;
                        if (burnK(qs) > 0.0)
                            put(at(qs.x, qs.y, 10.0 + dz), Ipm * heat * ds * 0.35, glm::vec3(1.0f, 0.48f, 0.13f), flick(h ^ 0x5F, 1.3), 0.3 * hFl + 1.0);
                    }
                }
            };
            if (stormCatalog)
                stormCatalog->parallelFor((size_t)kSeg, runSeg);
            else
                for (uint32_t sg = 0; sg < kSeg; ++sg) runSeg(sg);
            for (uint32_t sg = 0; sg < kSeg; ++sg)
            {
                const FirePtSeg &so = firePtSeg[sg];
                for (size_t i = 0; i < so.pts.size(); ++i)
                {
                    if (kFireCount >= kFireballMax || kFireCount - ptStart >= ptBudget) break;
                    firePeakFlare = std::max(firePeakFlare, so.fl[i]);
                    fireFlareSum += so.fl[i];
                    fo[kFireCount++] = so.pts[i];
                }
            }
        }

        // ── Flames close up: emissive Gaussians standing on the front (by day too: the points are dimmed there) ──
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
                double dsda = 0.0;
                const glm::dvec2 q = wx::fireFrontAt(f, s, a, &dsda);
                const double heat = wx::fireFrontHeat(f, s, a, t) * burnK(q);
                if (heat < 0.08) continue;
                const double ds = dsda * 2.0 * kPiF / std::ldexp(1.0, K);
                const double hh = hFl * (0.6 + 0.6 * heat) * (0.8 + 0.4 * std::sin(t * (2.0 + 2.0 * fwU(h ^ 3)) + 6.28 * fwU(h ^ 4)));
                double latP, lonP;
                wx::fireOffsetLatLon(f, q, latP, lonP);
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
                const double sA = 0.35 * hh + 0.05 * ds, sV = 0.33 * hh, sC = std::max(0.25 * dFl, 2.0);
                const double Sigma = 2.0 * kPiF * sA * sV;   // the side-on peak optical depth 1: the radiance is its colour
                // The flame's radiance in the sky's units (pi L / the Sun's illuminance ~0.5 of a sunlit white), CAPPED in
                // displayed light at 0.6 (an orange flame, not a white blob at the night exposure; a few overlap along a flank).
                const double radPhys = kPiF * kFlameLum / kSunLux * std::pow(std::min(heat, 1.2), 1.5);
                const double rad = std::min(radPhys, 0.6 / expo) * wa * fadeR * lightK;
                const glm::vec3 col = glm::mix(glm::vec3(1.0f, 0.36f, 0.08f), glm::vec3(1.0f, 0.62f, 0.22f), (float)std::clamp(heat, 0.0, 1.0));
                if (emit(P, T, upP, Cx, sA, sV, sC, Sigma, glm::dvec3(col) * rad, glm::dvec3(0.0), 0.0, h, flameCap, 1.0))
                {
                    out[n - 1].ib.w = (float)(0.45 * hh);   // (its height over the ground: wildfire_ground.comp)
                    ++fireFlamesLast;
                }
            }
        }

        // ── The glow: the nearest fires' light on their smoke, the cloud and the ground (engine lights, eye-adapted by
        // engineLightsFlush), from up to three points along the active arc (the heat's thirds), each with its share.
        // Its intensity is the front's light x the share that escapes the trees, the terrain and the smoke round the
        // flames, CAPPED: the night's lift (~2600x, the moonlight's) made a big fire's 0.06 of its front (~2000 suns m^2,
        // 40x a Super Heavy's flame) light the haze and the ground orange over a whole view from 5-100 km (2026-10-24,
        // the user: "light is overwhelming"). A fire's glow now reaches a few km: its own smoke column's base, the
        // ground round it. Faded out by 120 km (from orbit the points carry the fire).
        if (lightsAdded < 6 && night > 0.05 && fn.rangeKm < 120.0)
        {
            double Itot = 0.0;
            glm::dvec2 lq[24];
            double lw[24];
            for (int k = 0; k < 24; ++k)
            {
                const double a = 2.0 * kPiF * (k + 0.5) / 24.0;
                double dsda = 0.0;
                lq[k] = wx::fireFrontAt(f, s, a, &dsda);
                lw[k] = kFlameLum * hFl * s.activity * wx::fireFrontHeat(f, s, a, t) * burnK(lq[k]) * dsda * (2.0 * kPiF / 24.0);
                Itot += lw[k];
            }
            const double fadeL = 1.0 - glm::smoothstep(40.0, 120.0, fn.rangeKm);
            const double Il = std::min(0.004 * Itot / kSunLux, 3.0) * lightK * night * fadeL;   // suns x m^2
            fireGlowSum += Il;
            if (Il > 0.02 && Itot > 0.0)
            {
                // the heat's thirds round the arc
                double acc = 0.0;
                int part = 0;
                glm::dvec2 c3[3] = {glm::dvec2(0.0), glm::dvec2(0.0), glm::dvec2(0.0)};
                double w3[3] = {0.0, 0.0, 0.0};
                for (int k = 0; k < 24; ++k)
                {
                    c3[part] += lw[k] * lq[k];
                    w3[part] += lw[k];
                    acc += lw[k];
                    if (acc >= Itot * (part + 1) / 3.0 && part < 2) ++part;
                }
                for (int p = 0; p < 3 && lightsAdded < 6; ++p)
                {
                    if (w3[p] <= 0.0) continue;
                    const glm::dvec2 c = c3[p] / w3[p];
                    const glm::dvec3 Pl = at(c.x, c.y, 30.0 + 0.5 * hFl);
                    engineLightAdd(fwEcefToEci(Pl, gr), Il * w3[p] / Itot, glm::vec3(1.0f, 0.42f, 0.12f),
                                   std::max(250.0, 0.25 * std::max(s.halfWidthM, 200.0)), glm::dvec3(0.0), 0.0, 0.0, false, 0.0);
                    ++lightsAdded;
                    ++fireLightsLast;
                }
            }
        }
        fn.points = 0;
    }
    firePtFirst = ptStart;
    firePtCount = kFireCount - ptStart;
    flameCount = n - flameFirst;
    firePointsLast = kFireCount - ptStart;
    if (kFireMapped) static_cast<uint32_t *>(kFireMapped)[0] = kFireCount;   // (the fireball pass reads this count)
    // The points' budget: when a crowd of fires asks for more, their spacing widens (continuously: the levels
    // cross-fade); it relaxes back slowly.
    if (firePointsLast >= (uint32_t)(0.95 * ptBudget))
        fireLodScale = std::min(fireLodScale * 1.12f, 64.0f);
    else if (firePointsLast < (uint32_t)(0.6 * ptBudget) && fireLodScale > 1.0f)
        fireLodScale = std::max(fireLodScale / 1.03f, 1.0f);
}

// The burn scars (2026-10-24, sat_sky.frag fireScarAt): the nearest kMaxFiresGpu fires' burnt ellipses (last frame's
// firesNow: the UBO is filled before the dust list) in the observer's ENU frame from its sea-level point.
void SatelliteSim::weatherFillFireParams(GpuCloudParams &cp)
{
    cp.fireInfo = glm::vec4(0.0f);
    if (wildfireRate <= 0.0f || firesNow.empty())
        return;
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    const glm::dvec3 ex = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), up));
    const glm::dvec3 ey = glm::cross(up, ex);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, ex), glm::dot(v, ey), glm::dot(v, up)); };
    int k = 0;
    for (const FireNow &fn : firesNow)
    {
        if (k >= kMaxFiresGpu) break;
        const wx::Fire &f = fn.f;
        const wx::FireState &s = fn.s;
        if (s.ageS <= 0.0 || fn.rangeKm > 2500.0 || s.headM < 60.0) continue;
        const glm::dvec3 G = llDir(f.latDeg, f.lonDeg);
        const glm::dvec3 eE = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), G) + glm::dvec3(1e-12, 0, 0));
        const glm::dvec3 nE = glm::cross(G, eE);
        const double semi = 0.5 * (s.headM + s.backM), off = 0.5 * (s.headM - s.backM);
        const glm::dvec3 W = eE * std::sin(s.windAz) + nE * std::cos(s.windAz);   // the spread axis (fireFrontAt's w)
        const glm::dvec3 Cx = eE * std::cos(s.windAz) - nE * std::sin(s.windAz);  // across (fireFrontAt's c)
        const glm::dvec3 Gc = glm::normalize(G + W * (off / kReF));
        const glm::dvec3 c = toEnu(Gc) * kReF - glm::dvec3(0.0, 0.0, kReF);
        // (the axes at the scar's centre: horizontal there)
        const glm::dvec3 Wc = glm::normalize(W - Gc * glm::dot(W, Gc)), Cc = glm::normalize(Cx - Gc * glm::dot(Cx, Gc));
        cp.fireC[k] = glm::vec4(glm::vec3(c), (float)semi);
        cp.fireU[k] = glm::vec4(glm::vec3(toEnu(Wc)), (float)s.halfWidthM);
        cp.fireV[k] = glm::vec4(glm::vec3(toEnu(Cc)), (float)(fwU(fwMix(f.id ^ 0x5CA4ull)) * 97.0));
        const double fresh = 1.0 - glm::smoothstep(0.85, 1.0, s.ageS / std::max(f.life, 1.0));
        cp.fireAux[k] = glm::vec4((float)fresh, (float)(1.0 / (20.0 + 0.01 * s.halfWidthM)), 0.0f, 0.0f);
        for (int q = 0; q < 4; ++q)
            cp.fireK[k * 4 + q] = glm::uvec4(0u);
        for (int j = 0; j < 64; ++j)
        {
            const double kk = wx::fireFrontRadiusK(f, s, 2.0 * kPiF * j / 64.0);
            const uint32_t b = (uint32_t)std::clamp(std::lround((kk - 0.6) / 0.8 * 255.0), 0l, 255l);
            cp.fireK[k * 4 + (j >> 4)][(j >> 2) & 3] |= b << (8 * (j & 3));
        }
        ++k;
    }
    cp.fireInfo = glm::vec4((float)k, 0.0f, 0.0f, 0.0f);
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
    o["point_peak_flare"] = firePeakFlare;
    o["point_flare_sum"] = fireFlareSum;
    o["glow_suns_m2"] = fireGlowSum;
    o["smoke_sources"] = fireSmokeSources;
    o["ground_snap"] = fireGroundPipe != VK_NULL_HANDLE;
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
        // Fires on built-up land: at the ignition point, and the burning front's samples over lit land (the night
        // map's ~18-km luminance past 30 / 60; the land's base ~14).
        int ign30 = 0, ign60 = 0, frontSamples = 0, front30 = 0, front60 = 0;
        for (const wx::Fire &f : all)
        {
            const float L = nightLumCpu((float)f.latDeg, (float)f.lonDeg);
            ign30 += L >= 30.0f;
            ign60 += L >= 60.0f;
            const wx::FireState s = wx::fireStateAt(f, t);
            for (int k = 0; k < 32; ++k)
            {
                const double a = 2.0 * kPiF * (k + 0.5) / 32.0;
                if (wx::fireFrontHeat(f, s, a, t, &biomeMap) < 0.02) continue;
                double la, lo;
                wx::fireOffsetLatLon(f, wx::fireFrontAt(f, s, a), la, lo);
                const float Lf = nightLumCpu((float)la, (float)lo);
                ++frontSamples;
                front30 += Lf >= 30.0f;
                front60 += Lf >= 60.0f;
            }
        }
        o["world_ignition_on_lights"] = {{"lum30", ign30}, {"lum60", ign60}};
        o["world_front_samples"] = frontSamples;
        o["world_front_on_lights"] = {{"lum30", front30}, {"lum60", front60}};
        nlohmann::json b;
        for (int k = 0; k < (int)wx::Biome::Count; ++k)
            if (byBiome[k]) b[wx::biomeName((wx::Biome)k)] = byBiome[k];
        o["world_by_biome"] = b;
    }
    return o;
}

// The fire's points and flames onto the drawn ground (wildfire_ground.comp; see the members in SatelliteSim.h). Run
// right after the dust list and kFireBuf are filled, before the cloud composite and the points read them.
// Made during the load (setAudio's prewarm): on first use it was a ~340-ms hitch the first time a fire came near.
void SatelliteSim::createWildfireGround(VulkanContext &ctx)
{
    if (fireGroundPipe || fireGroundFailed)
        return;
    {
        VkDescriptorSetLayoutBinding b[5] = {};
        b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[2] = {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 5;
        li.pBindings = b;
        vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &fireGroundDescLayout);
        VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
                                      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.poolSizeCount = 3;
        pi.pPoolSizes = ps;
        pi.maxSets = 1;
        vkCreateDescriptorPool(ctx.device, &pi, nullptr, &fireGroundDescPool);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = fireGroundDescPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &fireGroundDescLayout;
        vkAllocateDescriptorSets(ctx.device, &ai, &fireGroundDescSet);
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 48};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &fireGroundDescLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        vkCreatePipelineLayout(ctx.device, &pli, nullptr, &fireGroundPipeLayout);
        VkShaderModule mod = ctx.loadShader("shaders/wildfire_ground.comp.spv");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main",
                    nullptr};
        ci.layout = fireGroundPipeLayout;
        if (vkCreateComputePipelines(ctx.device, ctx.pipelineCache, 1, &ci, nullptr, &fireGroundPipe) != VK_SUCCESS)
        {
            fireGroundPipe = VK_NULL_HANDLE;
            fireGroundFailed = true;
        }
        vkDestroyShaderModule(ctx.device, mod, nullptr);
    }
}

void SatelliteSim::recordWildfireGround(VkCommandBuffer cmd, VulkanContext &ctx)
{
    if ((firePtCount == 0 && flameCount == 0) || fireGroundFailed || !kFireBuf || !kDustBuf)
        return;
    createWildfireGround(ctx);
    if (!fireGroundPipe)
        return;
    // Descriptors when what they point at changed (the buffers can be recreated). Rewriting them every frame was
    // safe with one frame in flight; with two it would wait for the previous frame every frame.
    VkSampler elevS = earthElevSampler ? earthElevSampler : noiseSampler;
    VkImageView elevV = earthElevView ? earthElevView : noiseTexView;
    VkSampler specS = earthSpecSampler ? earthSpecSampler : noiseSampler;
    VkImageView specV = earthSpecView ? earthSpecView : noiseTexView;
    VkDescriptorImageInfo elev{elevS, elevV, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo spec{specS, specV, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo ubo{cloudParamsBuf, 0, sizeof(GpuCloudParams)};
    VkDescriptorBufferInfo fb{kFireBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo db{kDustBuf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[5] = {};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, fireGroundDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &elev, nullptr, nullptr};
    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, fireGroundDescSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &spec, nullptr, nullptr};
    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, fireGroundDescSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubo, nullptr};
    w[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, fireGroundDescSet, 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &fb, nullptr};
    w[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, fireGroundDescSet, 4, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &db, nullptr};
    const std::array<uint64_t, 6> bound = {(uint64_t)elevV, (uint64_t)specV, (uint64_t)cloudParamsBuf, (uint64_t)kFireBuf,
                                           (uint64_t)kDustBuf, (uint64_t)fireGroundDescSet};
    if (bound != fireGroundBound_)
    {
        frameSafeUpdateDescriptorSets(ctx.device, 5, w, 0, nullptr);
        fireGroundBound_ = bound;
    }
    struct
    {
        glm::vec4 obs, eye;
        glm::uvec4 rng;
    } pc;
    pc.obs = glm::vec4(glm::normalize(obsDir), obsHeightOffset);
    pc.eye = glm::vec4((float)((double)obsEyeRadiusM() - kReF), glm::radians(camera.fovYDeg) / (float)std::max<uint32_t>(ctx.swapExtent.height, 1u),
                       3.0e6f, 0.0f);
    pc.rng = glm::uvec4(firePtFirst, firePtCount, flameFirst, flameCount);
    static_assert(sizeof(pc) == 48);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fireGroundPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fireGroundPipeLayout, 0, 1, &fireGroundDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, fireGroundPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (firePtCount + flameCount + 63) / 64, 1, 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
}

void SatelliteSim::destroyWildfireGround(VkDevice device)
{
    if (fireGroundPipe) vkDestroyPipeline(device, fireGroundPipe, nullptr);
    if (fireGroundPipeLayout) vkDestroyPipelineLayout(device, fireGroundPipeLayout, nullptr);
    if (fireGroundDescPool) vkDestroyDescriptorPool(device, fireGroundDescPool, nullptr);
    if (fireGroundDescLayout) vkDestroyDescriptorSetLayout(device, fireGroundDescLayout, nullptr);
    fireGroundPipe = VK_NULL_HANDLE;
    fireGroundPipeLayout = VK_NULL_HANDLE;
    fireGroundDescPool = VK_NULL_HANDLE;
    fireGroundDescLayout = VK_NULL_HANDLE;
    fireGroundDescSet = VK_NULL_HANDLE;
}
