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
#include <atomic>
#include <functional>
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
    fireGlowDbg.clear();
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
    // (640: the trail pieces below; at least 96 left for the flames after the smoke)
    const uint32_t cap = std::max<uint32_t>(n, std::min<uint32_t>(kMaxDustClouds > 96u ? kMaxDustClouds - 96u : kMaxDustClouds, n + 640u));
    // One Gaussian: centre C (ECEF), orthonormal axes a, b, c with sigmas, Sigma = cross-section, its light.
    auto emit = [&](const glm::dvec3 &C, const glm::dvec3 &a, const glm::dvec3 &b, const glm::dvec3 &c, double sA,
                    double sB, double sC, double Sigma, const glm::dvec3 &sunL, const glm::dvec3 &skyL, double albedo,
                    uint64_t seed, uint32_t capN, double colW) -> bool {
        if (n >= capN || Sigma < 1.0) return false;
        const glm::dvec3 rel = C - eye;
        const double range = std::max(glm::length(rel), 1.0);
        if (colW < 0.0)
        {
            // smoke: no sigma under ~1.5 full-res pixels as seen (the column kept): thinner, the half-res composite
            // draws its edges as stairs (2026-10-27, issue 20261027_003452)
            const double sMin = 1.5 * range * pixFull;
            sA = std::max(sA, sMin);
            sB = std::max(sB, sMin);
            sC = std::max(sC, sMin);
        }
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
    // ── Smoke: TRAJECTORIES (2026-10-26, issue 20261026_204239: "smoke kind of just wavers around randomly and sometimes
    // just changes direction entirely. Wind flow needs to be localized: changing wind direction at the fire's source
    // should have no effect on smoke farther down the trail") ──────────────────────────────────────────────────────────
    // Each source's streak is a chain of PARCELS emitted at fixed times on a nested grid (spacing d x 2^l for the ages in
    // band l, m knots a band: fine near the fire, geometric down the trail; a parcel whose emission time is not on the
    // next band's grid merges into its neighbours over the last 35% of its band, no pop). A parcel leaves the column's
    // source as the fire was then (its sections, its injection height and activity at that moment) and is advected step
    // by step through wx::windAt at ITS OWN position, height and time (fireSmokeAdvect, the schedule fireSmokeStepS): a
    // trajectory, cached (smokeTrajs) and only ever extended, so a turn of the wind at the fire bends only the smoke
    // emitted after it. The streak until then was re-walked every frame from the fire with each stretch's wind taken at
    // the moment that stretch's AGE ago: the whole trail rode on the source's newest wind and swung as one body.
    // The pall (the smoke older than the streak) is the mass-weighted mean of a few long trajectories (4-h grid) the same way.
    const uint32_t sFrame = ++smokeFrame;
    smokeSegDbg.clear();
    smokeTrajSteps = 0;
    const float spread = std::max(wildfireSmokeSpread, 0.05f);
    // The fire's sections now or at an emission time: its active front's heat cut into S sections of equal heat (each a
    // source at its heat-weighted centre, as wide as the section, carrying its share), its injection height, activity,
    // smoke production and the ground wind (the smoke leaves the flames downwind).
    auto sources = [&](const wx::Fire &f, double tq, int S, SmokeEmit &em) {
        const wx::FireState s = wx::fireStateAt(f, tq);
        const double sz = (double)f.size;
        const double tau0 = (2.5 + 5.5 * sz) * (0.6 + 0.4 * f.fuel);
        const double wSrc = std::max(150.0, 0.3 * s.halfWidthM) + 300.0 * sz;
        em.Q = tau0 * std::sqrt(2.0 * kPiF) * wSrc * 5.0;
        em.H = wx::fireInjectionHeightM(f, s, tq);
        em.act = s.activity;
        constexpr int kHs = 48;
        glm::dvec2 hq[kHs];
        double hw[kHs], hTot = 0.0, hRaw = 0.0;
        for (int k = 0; k < kHs; ++k)
        {
            const double a = 2.0 * kPiF * (k + 0.5) / kHs;
            double dsda = 0.0;
            hq[k] = wx::fireFrontAt(f, s, a, &dsda);
            double la, lo;
            wx::fireOffsetLatLon(f, hq[k], la, lo);
            const double h0 = wx::fireFrontHeat(f, s, a, tq) * dsda;
            hw[k] = h0 * glm::smoothstep(0.08, 0.3, (double)biomeMap.burnableAt(la, lo));
            hTot += hw[k];
            hRaw += h0;
        }
        // The smoke made is the share of the front still on burnable ground (2026-10-27, issue 20261027_000126: a fire
        // whose front had all run into the sea or onto bare rock kept its full smoke, from a stand-in source at the head).
        em.Q *= hRaw > 1e-9 ? std::clamp(hTot / hRaw, 0.0, 1.0) : 0.0;
        int k0 = 0;
        {
            double best = 1e300;
            for (int k = 0; k < kHs; ++k)
                if (hw[k] < best) { best = hw[k]; k0 = k; }
        }
        glm::dvec2 c[5]{};
        double m[5]{}, ext[5]{};
        em.nS = 0;
        if (hTot > 1e-6)
        {
            double acc = 0.0;
            int si = 0;
            for (int j = 0; j < kHs; ++j)
            {
                const int k = (k0 + j) % kHs;
                c[si] += hw[k] * hq[k];
                m[si] += hw[k];
                acc += hw[k];
                if (acc >= hTot * (si + 1) / S - 1e-9 && si < S - 1) ++si;
            }
            for (int i = 0; i < S; ++i)
                if (m[i] > 0.0) c[i] /= m[i];
            acc = 0.0;
            si = 0;
            for (int j = 0; j < kHs; ++j)
            {
                const int k = (k0 + j) % kHs;
                if (m[si] > 0.0) ext[si] += hw[k] * glm::dot(hq[k] - c[si], hq[k] - c[si]);
                acc += hw[k];
                if (acc >= hTot * (si + 1) / S - 1e-9 && si < S - 1) ++si;
            }
            for (int i = 0; i < S; ++i)
                if (m[i] > 0.0)
                {
                    em.c[em.nS] = c[i];
                    em.ext[em.nS] = std::sqrt(ext[i] / m[i]);
                    em.w[em.nS] = m[i] / hTot;
                    ++em.nS;
                }
        }
        // Labelled by their bearing round the burnt ellipse's centre from its (fixed) spread axis, not by the order the
        // cut found them in: the cut starts at the arc's coldest point, which jumps, and a parcel's source index then named
        // another stretch of the front at another emission time (the streak zig-zagged across the fire between knots).
        if (em.nS > 1)
        {
            const glm::dvec2 ctr = 0.5 * (wx::fireFrontAt(f, s, 0.0) + wx::fireFrontAt(f, s, kPiF));
            const glm::dvec2 ax(std::sin(s.windAz), std::cos(s.windAz)), px(ax.y, -ax.x);
            int ord[5] = {0, 1, 2, 3, 4};
            double th[5];
            for (int i = 0; i < em.nS; ++i)
            {
                const glm::dvec2 r = em.c[i] - ctr;
                th[i] = std::atan2(glm::dot(r, px), glm::dot(r, ax));
            }
            std::sort(ord, ord + em.nS, [&](int a, int b) { return th[a] < th[b]; });
            SmokeEmit o = em;
            for (int i = 0; i < em.nS; ++i)
            {
                em.c[i] = o.c[ord[i]];
                em.w[i] = o.w[ord[i]];
                em.ext[i] = o.ext[ord[i]];
            }
        }
        if (em.nS == 0)
        {
            em.c[0] = wx::fireFrontAt(f, s, 0.0);
            em.w[0] = 1.0;
            em.ext[0] = 0.3 * s.halfWidthM;
            em.nS = 1;
        }
        em.mainSrc = 0;
        for (int i = 1; i < em.nS; ++i)
            if (em.w[i] > em.w[em.mainSrc]) em.mainSrc = i;
        const glm::dvec2 u = wx::windAt(llDir(f.latDeg, f.lonDeg), 10.0, tq);
        em.wDir = glm::length(u) > 0.05 ? u / glm::length(u) : glm::dvec2(std::sin(s.windAz), std::cos(s.windAz));
        em.ready = true;
    };
    // Source si of S: the section at its rank (an emission whose front cut into fewer sections shares them out), and
    // the share of the smoke it carries.
    auto secOf = [](const SmokeEmit &em, int si, int S) {
        if (si >= 5) return em.mainSrc;
        if (em.nS == S) return si;
        return std::clamp((int)std::floor((si + 0.5) * em.nS / S), 0, em.nS - 1);
    };
    auto shareOf = [&](const SmokeEmit &em, int si, int S) {
        return em.w[secOf(em, si, S)] * (si >= 5 || em.nS == S ? 1.0 : (double)em.nS / S);
    };
    // A parcel's layer and its start (the section's centre, 120 m + 0.4 x the flaming depth downwind).
    auto parcelHb = [&](const SmokeEmit &em, int si, int S) {
        if (si == 5) return 0.4 * em.H;
        if (si == 6) return 0.8 * em.H + 200.0;
        return em.H * std::clamp(0.7 + 0.3 * std::sqrt(shareOf(em, si, S) * S), 0.55, 1.1);
    };
    auto parcelStart = [&](const wx::Fire &f, const SmokeEmit &em, int si, int S) {
        return em.c[secOf(em, si, S)] + em.wDir * (120.0 + 0.4 * wx::fireFlameDepthM(f));
    };
    auto keyOf = [](uint64_t fid, int S, int si, int64_t tEi) {
        return fwMix(fid ^ fwMix(((uint64_t)S << 56) ^ ((uint64_t)(si + 1) << 48) ^ (uint64_t)(tEi & 0xFFFFFFFFFFFFull)));
    };

    // Phase 1: each fire's knots (which parcels exist now), creating the cache entries they need.
    struct Knot
    {
        SmokeTraj *tr = nullptr;
        double age = 0.0, fade = 0.0;   // fade: merging into its neighbours (0..1)
    };
    struct SrcPlan
    {
        int si = 0;
        std::vector<Knot> k;   // by age, youngest first
    };
    struct FirePlan
    {
        bool on = false;
        int S = 1;
        double tauMax = 0.0, aHead = 0.0;
        SmokeEmit cur;
        std::vector<SrcPlan> src;
        std::vector<Knot> pall;
        bool lowLayer = false;
    };
    std::vector<FirePlan> plans(firesNow.size());
    std::vector<SmokeEmit *> emitJobs;
    std::vector<const wx::Fire *> emitFire;
    std::vector<double> emitT;
    std::vector<int> emitS;
    std::vector<SmokeTraj *> trajJobs;
    auto getTraj = [&](const wx::Fire &f, int S, int si, double tE) -> SmokeTraj * {
        const int64_t tEi = (int64_t)std::llround(tE / 60.0);
        const uint64_t ek = keyOf(f.id, S, -1, tEi);
        auto ei = smokeEmits.try_emplace(ek);
        SmokeEmit &em = ei.first->second;
        em.frame = sFrame;
        if (ei.second)
        {
            emitJobs.push_back(&em);
            emitFire.push_back(&f);
            emitT.push_back(tE);
            emitS.push_back(S);
        }
        auto ti = smokeTrajs.try_emplace(keyOf(f.id, S, si, tEi));
        SmokeTraj &tr = ti.first->second;
        tr.frame = sFrame;
        tr.f = &f;
        if (ti.second)
        {
            tr.em = &em;
            tr.si = si;
            tr.tE = tE;
            tr.w0 = wx::fireUpdraftMps(f);
        }
        return &tr;
    };
    for (size_t fi = 0; fi < firesNow.size(); ++fi)
    {
        FireNow &fn = firesNow[fi];
        FirePlan &pl = plans[fi];
        const wx::Fire &f = fn.f;
        const wx::FireState &s = fn.s;
        fn.gauss = 0;
        if (s.ageS <= 0.0) continue;
        const double sz = (double)f.size;
        if (!((2.5 + 5.5 * sz) * (0.6 + 0.4 * f.fuel) * wildfireSmoke > 0.02) || fn.rangeKm >= 2500.0) continue;
        pl.on = true;
        pl.S = fn.rangeKm < 150.0 ? 5 : fn.rangeKm < 500.0 ? 3 : fn.rangeKm < 1200.0 ? 2 : 1;
        // (fixed per fire: a length that followed the activity stretched every knot of the streak as the day went on)
        pl.tauMax = std::clamp((3.0 + 9.0 * sz) * 3600.0 * 0.8, 2.0 * 3600.0, 14.0 * 3600.0);
        sources(f, t, pl.S, pl.cur);
        pl.lowLayer = fn.rangeKm < 800.0 && pl.cur.H > 1200.0;
        pl.aHead = 0.12 * pl.cur.H / wx::fireUpdraftMps(f);   // (the smoke starts above the flames: the hot gas is clear)
        const double ageCap = std::min(pl.tauMax, s.ageS);
        for (int si = 0; si < pl.S + (pl.lowLayer ? 1 : 0); ++si)
        {
            SrcPlan sp;
            sp.si = si < pl.S ? si : 5;
            const bool isMain = sp.si < 5 && secOf(pl.cur, sp.si, pl.S) == pl.cur.mainSrc && fn.rangeKm < 150.0;
            const double d = fn.rangeKm < 150.0 ? (isMain ? 60.0 : 120.0) : fn.rangeKm < 500.0 ? 120.0 : fn.rangeKm < 1200.0 ? 240.0 : 480.0;
            const double mK = isMain ? 2.0 : 1.0;
            for (int l = 0; l < 16; ++l)
            {
                const double D = d * std::ldexp(1.0, l);
                const double bA = mK * d * (std::ldexp(1.0, l) - 1.0), bB = mK * d * (std::ldexp(1.0, l + 1) - 1.0);
                if (bA >= ageCap) break;
                const int64_t kHi = (int64_t)std::floor((t - bA) / D), kLo = (int64_t)std::floor((t - bB) / D) + 1;
                for (int64_t kk = kHi; kk >= kLo; --kk)
                {
                    const double tE = (double)kk * D, age = t - tE;
                    if (age < bA || age >= bB || age > ageCap || age <= pl.aHead + 1.0) continue;
                    Knot kn;
                    kn.age = age;
                    if ((kk & 1) != 0) kn.fade = glm::smoothstep(0.65, 1.0, (age - bA) / (bB - bA));
                    kn.tr = getTraj(f, pl.S, sp.si, tE);
                    sp.k.push_back(kn);
                }
            }
            std::sort(sp.k.begin(), sp.k.end(), [](const Knot &a, const Knot &b) { return a.age < b.age; });
            pl.src.push_back(std::move(sp));
        }
        // the pall's parcels: a 4-h grid over [tauMax, tauMax + 36 h]
        {
            const double D = 4.0 * 3600.0;
            const int64_t kHi = (int64_t)std::floor((t - pl.tauMax) / D), kLo = (int64_t)std::floor((t - pl.tauMax - 36.0 * 3600.0) / D) + 1;
            for (int64_t kk = kHi; kk >= kLo; --kk)
            {
                const double tE = (double)kk * D, age = t - tE;
                if (age > s.ageS || age < pl.tauMax) continue;
                Knot kn;
                kn.age = age;
                kn.fade = std::pow(std::sin(kPiF * (age - pl.tauMax) / (36.0 * 3600.0)), 2.0);   // (its weight in the pall)
                kn.tr = getTraj(f, pl.S, 6, tE);
                pl.pall.push_back(kn);
            }
        }
    }
    // Phase 2: the new emissions' sections (parallel), then the trajectories extended to their ages now (parallel).
    auto par = [&](size_t n, const std::function<void(size_t)> &fn) {
        if (stormCatalog && n >= 4)
            stormCatalog->parallelFor(n, fn);
        else
            for (size_t i = 0; i < n; ++i) fn(i);
    };
    par(emitJobs.size(), [&](size_t i) { sources(*emitFire[i], emitT[i], emitS[i], *emitJobs[i]); });
    for (size_t fi = 0; fi < firesNow.size(); ++fi)
    {
        auto need = [&](Knot &kn) {
            SmokeTraj &tr = *kn.tr;
            if (tr.a.empty())
            {
                tr.Hb = parcelHb(*tr.em, tr.si, plans[fi].S);
                tr.p.push_back(parcelStart(*tr.f, *tr.em, tr.si, plans[fi].S));
                tr.a.push_back(0.0);
            }
            tr.need = std::max(tr.need, kn.age);
            if (tr.Hb > 0.0 && tr.a.back() < kn.age && (trajJobs.empty() || trajJobs.back() != &tr)) trajJobs.push_back(&tr);
        };
        for (SrcPlan &sp : plans[fi].src)
            for (Knot &kn : sp.k) need(kn);
        for (Knot &kn : plans[fi].pall) need(kn);
    }
    std::sort(trajJobs.begin(), trajJobs.end());
    trajJobs.erase(std::unique(trajJobs.begin(), trajJobs.end()), trajJobs.end());
    std::atomic<uint32_t> steps{0};
    par(trajJobs.size(), [&](size_t i) {
        SmokeTraj &tr = *trajJobs[i];
        uint32_t n = 0;
        while (tr.a.back() < tr.need)
        {
            const double a = tr.a.back(), h = wx::fireSmokeStepS(a);
            tr.p.push_back(wx::fireSmokeAdvect(*tr.f, tr.p.back(), a, h, tr.tE, std::max(tr.Hb, 30.0), tr.w0));
            tr.a.push_back(a + h);
            ++n;
        }
        steps += n;
    });
    smokeTrajSteps = steps.load();
    auto trajAt = [](const SmokeTraj &tr, double age) {
        if (tr.a.size() < 2) return tr.p[0];
        auto it = std::upper_bound(tr.a.begin(), tr.a.end(), age);
        const size_t j = (size_t)std::clamp<ptrdiff_t>(it - tr.a.begin(), 1, (ptrdiff_t)tr.a.size() - 1);
        const double u = std::clamp((age - tr.a[j - 1]) / std::max(tr.a[j] - tr.a[j - 1], 1e-6), 0.0, 1.0);
        return glm::mix(tr.p[j - 1], tr.p[j], u);
    };
    // the cache: entries not used for ~10 s of frames go
    if ((sFrame & 255u) == 0u)
    {
        for (auto it = smokeTrajs.begin(); it != smokeTrajs.end();)
            it = (sFrame - it->second.frame > 600u) ? smokeTrajs.erase(it) : std::next(it);
        for (auto it = smokeEmits.begin(); it != smokeEmits.end();)
            it = (sFrame - it->second.frame > 600u) ? smokeEmits.erase(it) : std::next(it);
    }

    // Phase 3: the Gaussians. The smoke's colour (2026-10-26, "it appears so beige"): grey — fresh smoke a little darker
    // (black carbon), aged paler and a touch cooler; the sky's light on it BLUE (it was a warm grey (0.80, 0.79, 0.76)); the
    // tint setting leans it brown or blue-grey. cloud_march.comp gives thin smoke the blue of its own scattering (Angstrom).
    const double tintK = (double)std::clamp(wildfireSmokeTint, -1.0f, 1.0f);
    const glm::dvec3 tintMul(1.0 + 0.10 * tintK, 1.0 + 0.01 * tintK, 1.0 - 0.14 * tintK);
    const glm::dvec3 kSkyBlue(0.66, 0.82, 1.08);
    const double bright = std::max((double)wildfireSmokeBright, 0.0);
    // Segments still to emit (each takes at least one Gaussian; the trail pieces share what is left of the cap).
    int64_t segLeft = 0;
    for (const FirePlan &pl : plans)
        if (pl.on)
        {
            for (const SrcPlan &sp : pl.src) segLeft += (int64_t)sp.k.size() + 1;
            segLeft += 1;
        }
    for (size_t fi = 0; fi < firesNow.size(); ++fi)
    {
        FireNow &fn = firesNow[fi];
        FirePlan &pl = plans[fi];
        if (!pl.on) continue;
        const wx::Fire &f = fn.f;
        const wx::FireState &s = fn.s;
        const glm::dvec3 G = llDir(f.latDeg, f.lonDeg);
        const glm::dvec3 eE = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), G) + glm::dvec3(1e-12, 0, 0));
        const glm::dvec3 nE = glm::cross(G, eE);
        const double ground = fn.groundM;
        auto dirAt = [&](double x, double y) { return glm::normalize(G + (eE * x + nE * y) / kReF); };
        auto at = [&](double x, double y, double z) { return dirAt(x, y) * (kReF + ground + z); };
        const double sz = (double)f.size;
        const double w0 = wx::fireUpdraftMps(f);
        int emitted = 0;
        glm::dvec2 lastXY = pl.cur.c[pl.cur.mainSrc];
        double lastZ = pl.cur.H;
        for (SrcPlan &sp : pl.src)
        {
            const bool low = sp.si == 5;
            // A knot: its place, its widths and the smoke rate it carries (cross-section a second).
            struct KP
            {
                glm::dvec3 q{0.0};
                double sb = 0.0, sz = 0.0, thin = 0.0, rate = 0.0, age = 0.0;
            };
            auto knotOf = [&](const SmokeEmit &em, int si, double age, const glm::dvec2 &xy, double Hb) {
                KP k;
                const double z = wx::fireSmokeRiseM(age, Hb, w0);
                k.q = glm::dvec3(xy, z);
                const int sc = secOf(em, si, pl.S);
                const double sB0 = std::max(120.0, 1.2 * em.ext[sc] / std::sqrt((double)em.nS)) + 100.0 * sz;
                const double ap = std::pow(age, 0.85);
                k.sb = spread * (sB0 + 0.25 * z + 1.1 * ap);
                k.sz = std::min(k.sb, spread * (120.0 + 0.22 * Hb + 0.12 * ap));
                k.thin = std::min(k.sb, spread * (200.0 + 0.15 * z + 0.6 * ap));
                const double wq = shareOf(em, si, pl.S);
                const double massK = low ? 0.2 * wq : (sc == em.mainSrc && pl.lowLayer ? 0.8 * wq : wq);
                k.rate = massK * em.Q * wildfireSmoke * std::max(em.act, 0.05);
                k.age = age;
                return k;
            };
            std::vector<KP> ks;
            ks.reserve(sp.k.size() + 1);
            {   // the head: emitted aHead ago from the sources as they are now (only the head segment rides on the wind now)
                const int si = sp.si;
                const double Hb = parcelHb(pl.cur, si, pl.S);
                glm::dvec2 xy = parcelStart(f, pl.cur, si, pl.S);
                double a = 0.0;
                while (a < pl.aHead)
                {
                    const double h = std::min(wx::fireSmokeStepS(a), pl.aHead - a);
                    xy = wx::fireSmokeAdvect(f, xy, a, h, t - pl.aHead, Hb, w0);
                    a += h;
                }
                ks.push_back(knotOf(pl.cur, si, pl.aHead, xy, Hb));
            }
            for (const Knot &kn : sp.k)
            {
                if (kn.tr->Hb <= 0.0) continue;
                ks.push_back(knotOf(*kn.tr->em, kn.tr->si, kn.age, trajAt(*kn.tr, kn.age), kn.tr->Hb));
            }
            // the merging parcels move toward their neighbours' mean as their band ends (no pop when they go)
            {
                std::vector<KP> raw = ks;
                size_t j = 1;
                for (const Knot &kn : sp.k)
                {
                    if (kn.tr->Hb <= 0.0) continue;
                    if (kn.fade > 0.0 && j + 1 < raw.size())
                    {
                        const KP &a = raw[j - 1], &b = raw[j + 1];
                        KP &k = ks[j];
                        const double u = kn.fade;
                        k.q = glm::mix(k.q, 0.5 * (a.q + b.q), u);
                        k.sb = glm::mix(k.sb, 0.5 * (a.sb + b.sb), u);
                        k.sz = glm::mix(k.sz, 0.5 * (a.sz + b.sz), u);
                        k.thin = glm::mix(k.thin, 0.5 * (a.thin + b.thin), u);
                    }
                    ++j;
                }
            }
            for (size_t k = 0; k + 1 < ks.size(); ++k)
            {
                const KP &A = ks[k], &B = ks[k + 1];
                const glm::dvec3 P0 = at(A.q.x, A.q.y, A.q.z), P1 = at(B.q.x, B.q.y, B.q.z);
                const double tm = 0.5 * (A.age + B.age);
                const glm::dvec3 seg = P1 - P0;
                const double len = std::max(glm::length(seg), 1.0);
                const glm::dvec3 a = seg / len;
                glm::dvec3 b = glm::cross(a, glm::normalize(P0));
                b = glm::length(b) > 1e-6 ? glm::normalize(b) : eE;
                const glm::dvec3 c = glm::cross(a, b);
                const double sb = 0.5 * (A.sb + B.sb), szv = 0.5 * (A.sz + B.sz);
                // c is near vertical for a flat stretch (its thickness) and near horizontal along the wind for the
                // rising column: a curtain along the front, thin in the wind's direction.
                const double cUp = std::fabs(glm::dot(c, glm::normalize(P0)));
                const double sC = glm::mix(0.5 * (A.thin + B.thin), szv, cUp);
                const double endK = 1.0 - glm::smoothstep(0.8 * pl.tauMax, pl.tauMax, B.age);
                const double Sigma = 0.5 * (A.rate + B.rate) * (B.age - A.age) * std::exp(-tm / (2.0 * 86400.0)) * endK;
                if (smokeSegDbg.size() < 256)
                    smokeSegDbg.push_back({(float)(fi * 10 + sp.si), (float)A.age, (float)B.age, (float)len, (float)sb, (float)sC,
                                           (float)Sigma, (float)(Sigma / (2.0 * kPiF * sb * sC)), (float)B.q.z});
                const glm::dvec3 Cc = 0.5 * (P0 + P1);
                const double age01 = glm::smoothstep(0.0, 3.0 * 3600.0, tm);
                const glm::dvec3 tint = tintMul * glm::mix(glm::dvec3(0.62, 0.62, 0.62), glm::dvec3(0.75, 0.76, 0.78), age01);
                const double alb = glm::mix(0.45, 0.6, age01);
                glm::dvec3 sunC;
                double amb;
                sunLightAt(Cc, sunC, amb);
                const glm::dvec3 sunL = bright * 4.0 * kPiF * alb * tint * sunC;
                const glm::dvec3 skyL = bright * 4.0 * kPiF * alb * tint * (0.18 * amb) * kSkyBlue;
                // Pieces (2026-10-27, issue 20261027_000414 "Smoke trail geometry": from 165 km the trail was a chain of
                // huge stretched ovals whose ends swung as the knots moved). The knots stand on a nested time grid whose
                // spacing doubles down the trail, so a segment was 15-30x as long as the smoke is wide at every age: ONE
                // Gaussian of sigma 0.5 x its length, straight, kinked at each knot. A segment is now cut into m round-ish
                // pieces along a Catmull-Rom curve through its neighbours' knots (the trail bends smoothly), spaced at
                // most ~2.2 x its width and never closer than 6 px on screen; each carries its share of the mass.
                int m = 1;
                static const bool kPiecesOff = [] { const char *e = std::getenv("SATLIGHTSIM_SMOKE_PIECES"); return e && e[0] == '0'; }();
                if (k > 0 && segLeft > 0 && !kPiecesOff)
                {
                    const double wSeg = std::max(std::max(sb, sC), 1.0);
                    const double rng = std::max(glm::length(Cc - eye), 1.0);
                    const double lenPx = len / (rng * pixFull);
                    const int mWant = (int)std::ceil(std::min(len / (2.2 * wSeg), lenPx / 6.0));
                    const int fair = (int)std::max<int64_t>(0, (int64_t)(cap - n) - (int64_t)segLeft);
                    m = std::clamp(mWant, 1, std::min(12, 1 + fair));
                }
                --segLeft;
                if (m == 1)
                {
                    if (emit(Cc, a, b, c, std::max((k == 0 ? 0.3 : 0.5) * len, (k == 0 ? 0.25 : 0.6) * sC), (k == 0 ? 0.6 : 1.0) * sb, sC,
                             Sigma, sunL, skyL, alb, f.id ^ (uint64_t)(k + 1 + 64 * sp.si), cap, -1.0))
                        ++emitted;
                }
                else
                {
                    const KP &Am = ks[k - 1], &Bp = (k + 2 < ks.size()) ? ks[k + 2] : B;
                    auto crq = [&](double u) {   // Catmull-Rom (uniform) through Am, A, B, Bp; u in [0, 1]
                        const double u2 = u * u, u3 = u2 * u;
                        return 0.5 * ((2.0 * A.q) + (-Am.q + B.q) * u + (2.0 * Am.q - 5.0 * A.q + 4.0 * B.q - Bp.q) * u2 +
                                      (-Am.q + 3.0 * A.q - 3.0 * B.q + Bp.q) * u3);
                    };
                    for (int j = 0; j < m; ++j)
                    {
                        const double u0 = (double)j / m, u1 = (double)(j + 1) / m, uc = 0.5 * (u0 + u1);
                        const glm::dvec3 qc = crq(uc), qa = crq(u0), qb = crq(u1);
                        const glm::dvec3 Pc = at(qc.x, qc.y, qc.z);
                        glm::dvec3 tg = at(qb.x, qb.y, qb.z) - at(qa.x, qa.y, qa.z);
                        const double lp = std::max(glm::length(tg), 1.0);
                        const glm::dvec3 aj = tg / lp;
                        glm::dvec3 bj = glm::cross(aj, glm::normalize(Pc));
                        bj = glm::length(bj) > 1e-6 ? glm::normalize(bj) : eE;
                        const glm::dvec3 cj = glm::cross(aj, bj);
                        const double sbj = glm::mix(A.sb, B.sb, uc), szj = glm::mix(A.sz, B.sz, uc);
                        const double cUpj = std::fabs(glm::dot(cj, glm::normalize(Pc)));
                        const double sCj = glm::mix(glm::mix(A.thin, B.thin, uc), szj, cUpj);
                        if (emit(Pc, aj, bj, cj, std::max(0.62 * lp, 0.6 * sCj), sbj, sCj, Sigma / m, sunL, skyL, alb,
                                 f.id ^ (uint64_t)(k + 1 + 64 * sp.si) ^ ((uint64_t)(j + 1) << 40), cap, -1.0))
                            ++emitted;
                    }
                }
                if (!low && secOf(pl.cur, sp.si, pl.S) == pl.cur.mainSrc)
                {
                    lastXY = glm::dvec2(B.q);
                    lastZ = B.q.z;
                    if (fn.knots.size() < 24) fn.knots.push_back(glm::vec4((float)B.q.x, (float)B.q.y, (float)B.q.z, (float)B.age));
                }
            }
            ++fireSmokeSources;
        }
        // The pall: the smoke older than the streaks, spread wide and flat, at the mass-weighted mean of its parcels'
        // trajectories (each carried by the winds it met); MASS-CONSERVING: what the fire made (at its mean activity)
        // over its life past the streak, less ~4 days of removal; ~0.01-0.05 from above for one big fire, so a fire
        // season's palls overlap into the regional haze.
        {
            const double kRem = 4.0 * 86400.0;
            const double older = std::max(s.ageS - pl.tauMax, 0.0);
            glm::dvec2 m(0.0);
            double ws = 0.05;
            m += ws * lastXY;
            std::vector<glm::dvec2> ps;
            for (const Knot &kn : pl.pall)
            {
                const glm::dvec2 q = trajAt(*kn.tr, kn.age);
                ps.push_back(q);
                m += kn.fade * q;
                ws += kn.fade;
            }
            m /= ws;
            double rms = 0.0;
            {
                double w2 = 0.05;
                rms += 0.05 * glm::dot(lastXY - m, lastXY - m);
                for (size_t i = 0; i < ps.size(); ++i)
                {
                    rms += pl.pall[i].fade * glm::dot(ps[i] - m, ps[i] - m);
                    w2 += pl.pall[i].fade;
                }
                rms = std::sqrt(rms / w2);
            }
            const double sh = std::min(spread * (20e3 + 0.7 * rms + 1.1 * std::pow(pl.tauMax, 0.85)), 350e3);
            const double zP = 0.8 * pl.cur.H + 200.0;
            const double Sigma = pl.cur.Q * wildfireSmoke * 0.6 * kRem * (1.0 - std::exp(-older / kRem));
            const double sAlong = 1.4 * sh;
            const double tauTop = Sigma / (2.0 * kPiF * sAlong * sh);
            const glm::dvec3 Cp = at(m.x, m.y, zP);
            // Its thickness at least ~3 full-res pixels as seen (the column kept: Sigma is unchanged): a pall ~1 km thick
            // seen edge-on from orbit, ~1000 km off, is under a pixel thick, and the half-res composite drew its top edge
            // along the limb as a hard 2-px staircase (2026-10-27, issue 20261027_003452).
            const double sPz = std::max(0.35 * pl.cur.H + 400.0, 3.0 * glm::length(Cp - eye) * pixFull);
            const glm::dvec3 upP = glm::normalize(Cp);
            glm::dvec2 dv = lastXY - m;
            glm::dvec3 wv = eE * dv.x + nE * dv.y;
            wv = glm::length(wv) > 1.0 ? wv / glm::length(wv) : nE;
            const glm::dvec3 ap = glm::normalize(wv - upP * glm::dot(wv, upP));
            const glm::dvec3 bp = glm::cross(upP, ap);
            const glm::dvec3 tintP = tintMul * glm::dvec3(0.75, 0.76, 0.78);
            const double albP = 0.6;
            glm::dvec3 sunC;
            double amb;
            sunLightAt(Cp, sunC, amb);
            if (smokeSegDbg.size() < 256)
                smokeSegDbg.push_back({(float)(fi * 10 + 9), (float)older, (float)ps.size(), (float)sAlong, (float)sh,
                                       (float)sPz, (float)Sigma, (float)tauTop, (float)zP});
            if (tauTop > 0.002 && emit(Cp, ap, bp, upP, sAlong, sh, sPz, Sigma,
                                      bright * 4.0 * kPiF * albP * tintP * sunC,
                                      bright * 4.0 * kPiF * albP * tintP * (0.18 * amb) * kSkyBlue, albP,
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
                // (the spacing's share is capped at a few flame heights, 2026-10-27, issue 20261027_002954: the spacing
                // follows the range to the fire's CENTRE, ~1 km for a big fire 20 km off, so a flame on the front right
                // under the eye was a glowing pancake ~50 m wide and 2 m tall: seen from 150 m up, an oval spotlight of
                // orange light filling half the view)
                // (its depth across the front no more than its width: seen from above, a 24-m-deep flaming zone made a
                // flat glowing slab)
                const double sA = 0.35 * hh + 0.05 * std::min(ds, 6.0 * hh), sV = 0.33 * hh,
                             sC = std::max(std::min(0.25 * dFl, 0.5 * hh + 1.0), 1.0);
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
                    const double r0L = std::max(250.0, 0.25 * std::max(s.halfWidthM, 200.0));
                    const glm::dvec3 Pl = at(c.x, c.y, 30.0 + 0.5 * hFl);
                    if (fireGlowDbg.size() < 12)
                        fireGlowDbg.push_back(glm::vec4(glm::vec3(toEnu(Pl - eye)), (float)r0L));
                    engineLightAdd(fwEcefToEci(Pl, gr), Il * w3[p] / Itot, glm::vec3(1.0f, 0.42f, 0.12f),
                                   r0L, glm::dvec3(0.0), 0.0, 0.0, false, 0.0);
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
    o["smoke_brightness"] = wildfireSmokeBright;
    o["smoke_tint"] = wildfireSmokeTint;
    o["smoke_spread"] = wildfireSmokeSpread;
    o["smoke_spectral"] = wildfireSmokeSpectral;
    o["smoke_trajectories"] = smokeTrajs.size();
    o["smoke_emissions"] = smokeEmits.size();
    o["smoke_traj_steps_frame"] = smokeTrajSteps;
    {
        nlohmann::json sg = nlohmann::json::array();
        for (const auto &a : smokeSegDbg) sg.push_back(std::vector<float>(a.begin(), a.end()));
        o["smoke_segments_si_a0_a1_len_sb_sc_sigma_tau_z"] = sg;
    }
    o["near_count"] = firesNow.size();
    o["smoke_range"] = {smokeHdrBits >> 8u & 1023u, smokeHdrBits >> 18u & 1023u};
    o["points"] = firePointsLast;
    o["flames"] = fireFlamesLast;
    o["lights"] = fireLightsLast;
    {
        nlohmann::json gl = nlohmann::json::array();
        for (const glm::vec4 &g : fireGlowDbg) gl.push_back({std::round(g.x), std::round(g.y), std::round(g.z), std::round(g.w)});
        o["glow_lights_enu_m_r0"] = gl;
    }
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
