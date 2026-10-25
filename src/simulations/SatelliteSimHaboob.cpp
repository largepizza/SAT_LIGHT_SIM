// SatelliteSimHaboob.cpp — haboobs (2026-10-23, .plans/WEATHER_HAZARDS_PLAN.md section 2).
//
// A haboob is a storm's gust front lifting dust: the cold outflow spreads ahead of a squall line / MCS (and of strong
// multicells and supercells) and where it crosses bare, dry ground (the biome map's DUST field: the Sahara, the Sahel,
// Arabia, the Thar, the Gobi, the SW US, inland Australia) it raises a wall of dust 1-2.5 km tall, dense inside
// (visibility of tens of metres), billowing on its face, with a thinning pall behind it.
// Drawn from this frame's storm towers (map frame, wx::Catalog), as plume Gaussians in the dust list just before the
// wildfire smoke, in the same "aerosol" range (smoke.glsl: no sky light given back, extinction spectral: the Sun through a
// haboob goes orange, the ground under it is shadowed): along the front, one wall Gaussian per arc spacing ahead of each
// dominant / line tower (pushed further out as the storm ages), and a lower, wider pall behind every few. Below ~3 km
// cloud_march.comp gives them the shared Earth-fixed billow field (the cauliflower face of the wall).
#include "SatelliteSim.h"
#include "FrameProfile.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
#include <cmath>

namespace {
constexpr double kReH = 6371000.0;
constexpr double kPiH = 3.14159265358979323846;
template <typename F> void parallelForHaboob(size_t n, F &&fn)
{
    const size_t workers = std::min<size_t>(std::max(1u, std::thread::hardware_concurrency() / 2), n);
    if (workers <= 1) { for (size_t i = 0; i < n; ++i) fn(i); return; }
    std::atomic<size_t> next{0};
    auto run = [&] { for (size_t i = next.fetch_add(1); i < n; i = next.fetch_add(1)) fn(i); };
    std::vector<std::thread> th;
    th.reserve(workers - 1);
    for (size_t k = 1; k < workers; ++k) th.emplace_back(run);
    run();
    for (std::thread &x : th) x.join();
}
uint64_t hbMix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
} // namespace

// The gust front points of one storm now (Earth-fixed unit directions, the front's local tangent and outward
// direction, and the local strength 0..1). Empty when the storm has no mature tower.
void SatelliteSim::haboobFrontOf(const wx::Storm &S, double t, std::vector<HaboobPoint> &pts,
                                 const std::vector<wx::Tower> *towers) const
{
    pts.clear();
    const double ph = cloudDriftPhase(), cph = std::cos(ph), sph = std::sin(ph);
    auto toE = [&](const glm::dvec3 &m) { return glm::dvec3(m.x * cph + m.y * sph, -m.x * sph + m.y * cph, m.z); };
    const double sp = glm::length(S.vel);
    const glm::dvec3 nAx = sp > 1e-9 ? glm::normalize(glm::cross(S.pos0, S.vel)) : glm::dvec3(0, 0, 1);
    const double age = t - S.t0;
    const bool line = wx::isLineSystem(S.kind);
    // The outflow leads the towers by a few km once the storm has rained a while, more for the organised systems.
    const double grow = glm::smoothstep(1800.0, 3.0 * 3600.0, age);
    const double dG = line ? 5000.0 + 14000.0 * grow : 3000.0 + 7000.0 * grow;
    struct TP { glm::dvec3 d; double s; double along; };
    std::vector<TP> tw;
    for (const wx::Tower &T : towers ? *towers : stormTowers)
        if (T.stormId == S.id && (line || T.role >= 0.99f) && T.strength > 0.25f)
            tw.push_back({T.dir, (double)T.strength, 0.0});
    if (tw.empty()) return;
    const glm::dvec3 c = glm::normalize(S.centreAt(t));
    glm::dvec3 mv = glm::normalize(glm::cross(nAx, c));            // the motion at the centre (map frame)
    if (sp < 0.5) mv = glm::normalize(glm::dvec3(S.shear));
    // The outflow leads the storm's motion OVER THE GROUND: its motion in the map frame plus the map's drift. Where the two
    // disagree (a storm moving against the drift slower than the drift carries it) the front is on the other side of the
    // line (2026-10-24: it was always ahead of the map-frame motion, so every Gobi MCS of the tests ran its wall away from
    // the observer a Go to put "ahead" of it, receding at ~15 m/s).
    if (wx::isLineSystem(S.kind) || sp >= 0.5)
    {
        const glm::dvec3 ea = weatherMapToEarth(glm::normalize(S.centreAt(t - 60.0)), t - 60.0);
        const glm::dvec3 eb = weatherMapToEarth(glm::normalize(S.centreAt(t + 60.0)), t + 60.0);
        if (glm::dot(eb - ea, toE(mv)) < 0.0) mv = -mv;
    }
    const glm::dvec3 acr = glm::normalize(glm::cross(c, mv));      // across the motion
    for (TP &p : tw) p.along = glm::dot(p.d - c, acr) * kReH;
    std::sort(tw.begin(), tw.end(), [](const TP &a, const TP &b) { return a.along < b.along; });
    auto push = [&](const glm::dvec3 &mapDir, const glm::dvec3 &outMap, double s) {
        HaboobPoint q;
        const glm::dvec3 e = glm::normalize(toE(mapDir));
        glm::dvec3 o = toE(outMap);
        o = glm::normalize(o - e * glm::dot(o, e));
        q.dir = e;
        q.out = o;
        q.tan = glm::cross(e, o);
        q.strength = s;
        pts.push_back(q);
    };
    auto ahead = [&](const glm::dvec3 &d, const glm::dvec3 &dirMap, double dist) {
        return glm::normalize(d + dirMap * (dist / kReH));
    };
    if (line || tw.size() >= 2)
    {
        // Along the line of towers, dG ahead; extended a little past each end, curving back (the front's flanks).
        const double stepM = 4000.0;
        auto at = [&](size_t i) { return tw[i].d; };
        for (size_t i = 0; i + 1 < tw.size(); ++i)
        {
            const double L = std::max(glm::length(at(i + 1) - at(i)) * kReH, 1.0);
            const int nS = std::max(1, (int)(L / stepM));
            for (int k = 0; k < nS; ++k)
            {
                const double f = (double)k / nS;
                const glm::dvec3 d = glm::normalize(glm::mix(at(i), at(i + 1), f));
                push(ahead(d, mv, dG), mv, glm::mix(tw[i].s, tw[i + 1].s, f));
            }
        }
        for (int end = 0; end < 2; ++end)
        {
            const TP &e = end == 0 ? tw.front() : tw.back();
            const double sgn = end == 0 ? -1.0 : 1.0;
            for (int k = 0; k <= 3; ++k)
            {
                const double a = (0.25 + 0.2 * k) * 0.5 * kPiH;    // swinging back round the end
                const glm::dvec3 o = glm::normalize(mv * std::cos(a) + acr * (sgn * std::sin(a)));
                push(ahead(e.d, o, dG), o, e.s * (1.0 - 0.2 * k));
            }
        }
    }
    else
    {
        // A single cell: a semicircle of outflow round the strongest tower, widest ahead of its motion.
        const TP &e = tw.front();
        for (int k = -5; k <= 5; ++k)
        {
            const double a = k * (0.8 * kPiH / 10.0);
            const glm::dvec3 o = glm::normalize(mv * std::cos(a) + acr * std::sin(a));
            push(ahead(e.d, o, dG * (0.7 + 0.3 * std::cos(a))), o, e.s * (0.6 + 0.4 * std::cos(a)));
        }
    }
}

void SatelliteSim::weatherFillHaboobs(GpuDustCloud *out, uint32_t &n)
{
    FPROF_SCOPE("weatherFillHaboobs");
    haboobsNow = 0;
    haboobNearestKm = -1.0;
    aerosolFirst = n;   // the haboobs and the wildfire smoke are one range (smoke.glsl): their own light, spectral
    if (!out || haboobGain <= 0.0f || !stormObjects || !stormCatalog || !biomeMap.ready()) return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    const double eyeR = (double)obsEyeRadiusM();
    const glm::dvec3 eye = up * eyeR;
    const double alt = std::max(eyeR - kReH, 0.0);
    const double reachKm = std::clamp(350.0 + 2.0 * alt / 1000.0, 350.0, 2000.0);
    const glm::dvec3 E = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up) + glm::dvec3(1e-12, 0, 0));
    const glm::dvec3 N = glm::cross(up, E);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, E), glm::dot(v, N), glm::dot(v, up)); };
    const double ph = cloudDriftPhase(), cph = std::cos(ph), sph = std::sin(ph);
    const glm::dvec3 si = sunDirEciAt(t);
    const double gr = earthRotationAngle(t);
    const glm::dvec3 sunE = glm::normalize(glm::dvec3(si.x * std::cos(gr) + si.y * std::sin(gr),
                                                      -si.x * std::sin(gr) + si.y * std::cos(gr), si.z));
    const double kTwoPi15 = 15.749609945722419;
    const uint32_t cap = std::min<uint32_t>(kMaxDustClouds, n + 200u);
    // The storms with a dominant tower in reach, nearest first.
    std::vector<std::pair<double, uint64_t>> ids;
    for (const wx::Tower &tw : stormTowers)
    {
        if ((tw.role < 0.99f && !wx::isLineSystem(tw.kind)) || tw.kind == wx::StormKind::Tropical ||
            tw.kind == wx::StormKind::AirMass) continue;
        const glm::dvec3 d = glm::normalize(tw.dir);
        const glm::dvec3 ed(d.x * cph + d.y * sph, -d.x * sph + d.y * cph, d.z);
        const double km = std::acos(std::clamp(glm::dot(ed, up), -1.0, 1.0)) * 6371.0;
        if (km > reachKm) continue;
        bool have = false;
        for (auto &p : ids)
            if (p.second == tw.stormId) { p.first = std::min(p.first, km); have = true; }
        if (!have) ids.push_back({km, tw.stormId});
    }
    std::sort(ids.begin(), ids.end());
    std::vector<HaboobPoint> pts;
    for (const auto &sid : ids)
    {
        const wx::Storm *S = stormCatalog->find(sid.second);
        if (!S) continue;
        haboobFrontOf(*S, t, pts);
        if (pts.empty()) continue;
        // Spacing on screen: the wall pieces ~3-4 km apart near, coarser far.
        const double sp = std::clamp(sid.first * 1000.0 * 0.015, 3500.0, 20000.0);
        double acc = 1e30;
        glm::dvec3 prev = pts.front().dir;
        int made = 0;
        for (size_t i = 0; i < pts.size() && n < cap; ++i)
        {
            acc += glm::length(pts[i].dir - prev) * kReH;
            prev = pts[i].dir;
            if (acc < sp) continue;
            acc = 0.0;
            const HaboobPoint &q = pts[i];
            const double lat = glm::degrees(std::asin(std::clamp(q.dir.z, -1.0, 1.0)));
            const double lon = glm::degrees(std::atan2(q.dir.y, q.dir.x));
            const double dust = biomeMap.dustAt(lat, lon);
            double k = dust * q.strength * haboobGain;
            if (k < 0.08) continue;
            // Within the dust grid the march draws the wall as cloud (weatherFillDustGrid): the Gaussians only beyond it,
            // faded in over its outer 15%.
            if (dustGridCellM > 0.0)
            {
                const glm::dvec3 sp = q.dir * kReH - dustGridUp * kReH;
                const double u = std::max(std::abs(glm::dot(sp, dustGridE)), std::abs(glm::dot(sp, dustGridN))) /
                                 (0.5 * kDustGridN * dustGridCellM);
                k *= glm::smoothstep(0.8, 0.95, u);
                if (k < 0.02) continue;
            }
            const double ground = std::max(0.0, (double)cpuTerrainHeightM((float)lat, (float)lon));
            const uint64_t h = hbMix(S->id ^ (uint64_t)(i * 2654435761u));
            const double H = (1500.0 + 1500.0 * std::min(1.0, k)) * (0.85 + 0.3 * ((h & 1023) / 1023.0));
            const double sinE = glm::dot(q.dir, sunE);
            const double dayK = glm::smoothstep(-0.06, 0.06, sinE);
            const double airm = 1.0 / (std::max(sinE, 0.0) + 0.05);
            const glm::dvec3 sunCol = glm::exp(-glm::dvec3(0.03, 0.05, 0.12) * airm) * dayK;
            const glm::dvec3 skyCol = 0.2 * glm::dvec3(0.62, 0.78, 1.0) * dayK;
            // Ochre, and dimmer than cloud: a thick wall of mineral dust (absorbing in the blue) reads tan-brown, never the
            // white of a cumulus (its in-scatter at 0.55 of the plume scale; in the aerosol range it gives back no sky light either).
            const glm::dvec3 tint = glm::dvec3(0.78, 0.55, 0.34) * 0.55;
            const double alb = 0.62;
            auto emit = [&](const glm::dvec3 &C, const glm::dvec3 &a, const glm::dvec3 &b, const glm::dvec3 &c, double sA,
                            double sB, double sC, double Sigma, uint64_t seed) {
                if (n >= cap || Sigma < 1.0) return;
                const glm::dvec3 rel = C - eye;
                const double range = std::max(glm::length(rel), 1.0);
                const glm::dvec3 ae = toEnu(a), be = toEnu(b), ce = toEnu(c);
                const double iA = 1.0 / (sA * sA), iB = 1.0 / (sB * sB), iC = 1.0 / (sC * sC);
                auto Mx = [&](int p, int r) { return iA * ae[p] * ae[r] + iB * be[p] * be[r] + iC * ce[p] * ce[r]; };
                GpuDustCloud &d = out[n++];
                const glm::dvec3 re = toEnu(rel);
                d.c = glm::vec4(glm::vec3(re), (float)Sigma);
                d.ia = glm::vec4((float)Mx(0, 0), (float)Mx(1, 1), (float)Mx(2, 2), (float)(1.0 / (kTwoPi15 * sA * sB * sC)));
                const glm::dvec3 sunL = 4.0 * kPiH * alb * tint * sunCol, skyL = 4.0 * kPiH * alb * tint * skyCol;
                const double lS = sunL.x + sunL.y + sunL.z, lI = skyL.x + skyL.y + skyL.z;
                const double share = std::min(lI / std::max(lI + lS, 1e-9), 0.999);
                d.ib = glm::vec4((float)Mx(0, 1), (float)Mx(0, 2), (float)Mx(1, 2),
                                 (float)(4.0 * (double)(hbMix(seed) & 127u) + 2.0 + share));   // seeds < 128: a wall (cloud_march.comp)
                d.col = glm::vec4(glm::vec3(sunL + skyL), (float)(2.0 + alb));
                const double R = 5.5 * std::max(sA, std::max(sB, sC));
                d.cone = R >= 0.95 * range ? glm::vec4(0.0f, 0.0f, 0.0f, -2.0f)
                                           : glm::vec4(glm::vec3(re / range), (float)std::sqrt(1.0 - (R / range) * (R / range)));
            };
            // The wall: a tall, dense slab along the front (its outer face ~1.5 km deep), resting on the ground.
            const double sAl = 0.7 * sp, sOut = 1500.0, sZ = 0.42 * H;
            const glm::dvec3 Cw = q.dir * (kReH + ground + 0.45 * H);
            const double tauOut = 25.0 * std::min(1.0, k);   // through the wall, outward (visibility tens of m inside)
            emit(Cw, q.tan, q.out, q.dir, sAl, sOut, sZ, tauOut * 2.0 * kPiH * sAl * sZ, h);
            // The head: the gust front's rolled nose, bulging forward high up (the wall leans out over its foot).
            {
                const glm::dvec3 Ch = glm::normalize(q.dir + q.out * (900.0 / kReH)) * (kReH + ground + 0.72 * H);
                const double sZh = 0.22 * H;
                emit(Ch, q.tan, q.out, q.dir, 0.8 * sAl, 1100.0, sZh, 0.8 * tauOut * 2.0 * kPiH * 0.8 * sAl * sZh, h ^ 0x4EADull);
            }
            ++made;
            // The pall behind every other piece: lower, wider, thinner (the dust settling behind the front).
            if ((made & 1) == 0)
            {
                const glm::dvec3 Cp = glm::normalize(q.dir - q.out * (9000.0 / kReH)) * (kReH + ground + 0.3 * H);
                const double sPal = 1.4 * sp, sBack = 7000.0, sZp = 0.35 * H;
                emit(Cp, q.tan, q.out, glm::normalize(Cp), sPal, sBack, sZp, 1.5 * std::min(1.0, k) * 2.0 * kPiH * sPal * sZp,
                     h ^ 0x9A11ull);
            }
        }
        if (made > 0)
        {
            ++haboobsNow;
            if (haboobNearestKm < 0.0) haboobNearestKm = sid.first;
        }
    }
}

double SatelliteSim::haboobPointK(const HaboobPoint &q) const
{
    const double lat = glm::degrees(std::asin(std::clamp(q.dir.z, -1.0, 1.0)));
    const double lon = glm::degrees(std::atan2(q.dir.y, q.dir.x));
    return biomeMap.dustAt(lat, lon) * q.strength * haboobGain;
}

// Is the Sun on a haboob's face at pE (Earth-fixed) clear of its storm's anvil? The Sun's ray from 1 km up meets the
// anvil's height (~9 km above it) d = 8 km / tan(el) toward the Sun: it is shadowed where that point lies under an
// anvil-reaching tower's flared head (+ its drift) or the storm's shield. 0..1 (soft over the last few km). The walls are
// cloud now and take the anvil's shadow: the first Go to's stood front-lit walls in a dark blue shade under the anvil.
double SatelliteSim::haboobSunOnFace(const glm::dvec3 &pE, double t, const std::vector<wx::Tower> &tw,
                                     const std::vector<wx::Shield> &sh) const
{
    const glm::dvec3 si = sunDirEciAt(t);
    const double gr = earthRotationAngle(t);
    const glm::dvec3 se(si.x * std::cos(gr) + si.y * std::sin(gr), -si.x * std::sin(gr) + si.y * std::cos(gr), si.z);
    const double sinEl = glm::dot(se, pE);
    if (sinEl < 0.05) return 0.0;
    glm::dvec3 hz = se - pE * sinEl;
    if (glm::length(hz) < 1e-6) return 1.0;   // the Sun overhead: nothing in the way but what is over the face itself
    hz = glm::normalize(hz);
    const double d = 8000.0 * std::sqrt(std::max(1.0 - sinEl * sinEl, 0.0)) / sinEl;
    const glm::dvec3 qE = glm::normalize(pE + hz * (std::min(d, 200000.0) / kReH));
    // to the map frame (the inverse of weatherMapToEarth)
    const double ph = (double)cloudDriftRate * (t - kCloudDriftEpochS) + cloudDriftPhaseOffset;
    const double c = std::cos(ph), sn = std::sin(ph);
    const glm::dvec3 q(qE.x * c - qE.y * sn, qE.x * sn + qE.y * c, qE.z);
    const double headR = std::clamp((double)cv2CbRadiusKm, 1.0, 20.0) * 1000.0 * std::max((double)cv2CbFlare, 1.0) * 1.15 +
                         std::clamp((double)cv2CbHeadDriftKm, 0.0, 30.0) * 1000.0;
    double lit = 1.0;
    for (const wx::Tower &T : tw)
        if (T.role >= 0.99f && T.strength > 0.3f)
            lit = std::min(lit, glm::smoothstep(headR, headR + 6000.0, glm::length(glm::normalize(T.dir) - q) * kReH));
    for (const wx::Shield &s : sh)
    {
        const glm::dvec3 ab = s.b - s.a;
        const double u = std::clamp(glm::dot(q - s.a, ab) / std::max(glm::dot(ab, ab), 1e-14), 0.0, 1.0);
        const double r = (double)s.rA + ((double)s.rB - (double)s.rA) * u;
        const double dist = glm::length(q - (s.a + ab * u)) * kReH;
        lit = std::min(lit, 1.0 - (double)s.strength * (1.0 - glm::smoothstep(0.8 * r, 1.2 * r, dist)));
    }
    return lit;
}

void SatelliteSim::haboobStormParts(wx::Catalog &cat, const wx::Storm &S, double t, std::vector<wx::Tower> &tw,
                                    std::vector<wx::Shield> &sh) const
{
    std::vector<const wx::Storm *> one{&S};
    tw.clear();
    sh.clear();
    cat.towersAt(t, one, tw);
    wx::ShieldParams sp;
    sp.headRadiusM = (float)(std::clamp((double)cv2CbRadiusKm, 1.0, 20.0) * 1000.0 * std::max((double)cv2CbFlare, 1.0) * 1.15);
    sp.headDriftM = (float)(std::clamp((double)cv2CbHeadDriftKm, 0.0, 30.0) * 1000.0);
    cat.shieldsAt(t, one, sp, sh);
}

bool SatelliteSim::haboobStrongestPoint(const wx::Storm &S, double t, HaboobPoint &best, double &bestK) const
{
    bestK = 0.0;
    if (!stormCatalog || !biomeMap.ready()) return false;
    std::vector<wx::Tower> tw;
    std::vector<wx::Shield> shl;
    haboobStormParts(*stormCatalog, S, t, tw, shl);
    std::vector<HaboobPoint> pts;
    haboobFrontOf(S, t, pts, &tw);
    // The strongest stretch of the front (a 5-point running mean: one dusty point among farmland draws little), weighted
    // toward where the Sun stands behind a viewer ahead of it (the front curves back at a line's ends) and reaches the
    // face past the anvil: front-lit.
    const glm::dvec3 si = sunDirEciAt(t);
    const double gr = earthRotationAngle(t);
    const glm::dvec3 se(si.x * std::cos(gr) + si.y * std::sin(gr), -si.x * std::sin(gr) + si.y * std::cos(gr), si.z);
    double bestS = 0.0;
    for (size_t i = 0; i < pts.size(); ++i)
    {
        double sum = 0.0;
        int n = 0;
        for (size_t j = (i >= 2 ? i - 2 : 0); j < std::min(pts.size(), i + 3); ++j, ++n) sum += haboobPointK(pts[j]);
        const double k = sum / std::max(n, 1);
        glm::dvec3 sh = se - pts[i].dir * glm::dot(se, pts[i].dir);
        const double lit = glm::length(sh) > 1e-9 ? std::max(0.0, glm::dot(glm::normalize(sh), pts[i].out)) : 0.0;
        const double sc = k * (0.25 + 0.75 * lit * haboobSunOnFace(pts[i].dir, t, tw, shl));
        if (k >= 0.08 && sc > bestS) { bestS = sc; bestK = k; best = pts[i]; }
    }
    return bestK >= 0.08;
}

// The dust walls as CLOUD (2026-10-24): the storms' gust fronts rasterised into the storm buffer's dust grid round the
// observer, which the clouds v2 field reads (cv2_storms.glsl cv2DustWallAt, clouds_v2.glsl cv2DustWallSigma). Per cell:
// the wall's strength e (0..1: the dust raised x a profile across the front: nothing 2 km ahead of it, full 1.5 km behind,
// the pall behind it at half, gone 26 km back) and its top (m above the ground: the head's height, falling to 0.4 of it
// over the pall). Each front point stamps a strip along the front (+-1.2 of its spacing, the strips overlapping: max),
// the strongest wins a cell. Grid: kDustGridN^2 cells on the tangent plane at the observer's sea-level point, 400 m near
// the ground growing with altitude (6 km from far orbit). Rebuilt when sim time moves or the observer moves a quarter cell.
void SatelliteSim::weatherFillDustGrid()
{
    FPROF_SCOPE("weatherFillDustGrid");
    if (!stormMapped) return;
    auto *head = reinterpret_cast<uint32_t *>(static_cast<char *>(stormMapped) + kDustHeadOffset);
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const bool on = haboobField && haboobGain > 0.0f && stormObjects && stormCatalog && biomeMap.ready() && !stormTowers.empty();
    if (!on)
    {
        head[0] = 0u;
        dustGpuOn = false;
        dustGridCellM = 0.0;
        dustWallsNow = dustCellsNow = 0;
        dustGridT = -1e300;
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    const double eyeR = (double)obsEyeRadiusM();
    const double alt = std::max(eyeR - kReH, 0.0);
    const double cell = std::clamp(400.0 + 0.006 * alt, 400.0, 6000.0);
    const glm::dvec3 eye = up * eyeR;
    // The lobe noise moves with the wall (the nearest drawn haboob's storm, Earth-fixed) and rises ~2 m/s: integrated
    // over sim time (a jump of more than 10 min keeps it where it was).
    const double dtS = t - dustNoiseT;
    dustNoiseT = t;
    if (std::abs(dtS) < 600.0) dustNoiseOff += (dustNoiseVel + up * 2.0) * dtS;
    auto *frame = reinterpret_cast<glm::vec4 *>(static_cast<char *>(stormMapped) + kDustFrameOffset);
    const glm::dvec3 sea = up * kReH;
    {
        // Every frame: the lobe noise's anchor, and where the observer's sea-level point sits on the grid (built round an
        // earlier one: cv2DustWallAt adds it, exact on the grid's own plane).
        const double P = kDustLobePeriodM;
        const glm::dvec3 an = (sea - dustNoiseOff) / P;
        auto fr = [](double x) { return x - std::floor(x); };
        frame[2] = glm::vec4((float)fr(an.x), (float)fr(an.y), (float)fr(an.z), (float)(1.0 / P));
        if (dustGridCellM > 0.0)
        {
            const glm::dvec3 d = sea - dustGridUp * kReH;
            frame[3] = glm::vec4((float)(glm::dot(d, dustGridE) / dustGridCellM + 0.5 * kDustGridN - 0.5),
                                 (float)(glm::dot(d, dustGridN) / dustGridCellM + 0.5 * kDustGridN - 0.5), 0.0f, 0.0f);
        }
    }
    // Rebuilt when sim time moved 2 s (the walls move ~10-20 m/s: 0.1 of a cell), the eye a quarter of a cell, or the cell
    // size; a still view at 1x rebuilds every 2 s (~1-2 ms on the workers, measured in `fires` -> dust_grid.ms).
    const bool moved = glm::length(eye - dustGridEye) > 0.25 * cell || std::abs(cell - dustGridCellM) > 0.02 * cell;
    if (std::abs(t - dustGridT) < 2.0 && !moved) return;
    dustGridT = t;
    dustGridEye = eye;
    dustGridCellM = cell;
    const glm::dvec3 E = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up) + glm::dvec3(1e-12, 0, 0));
    const glm::dvec3 N = glm::cross(up, E);
    dustGridE = E;
    dustGridN = N;
    dustGridUp = up;
    const int G = (int)kDustGridN;
    if (dustGridCpu.size() != (size_t)G * G) dustGridCpu.assign((size_t)G * G, 0u);
    // Clear what was written last time.
    const glm::ivec4 prevBox = dustBox;
    for (int y = prevBox.y; y < prevBox.w; ++y)
        std::fill(dustGridCpu.begin() + (size_t)y * G + prevBox.x, dustGridCpu.begin() + (size_t)y * G + prevBox.z, 0u);
    glm::ivec4 box(G, G, 0, 0);
    double maxTop = 0.0;
    const double half = 0.5 * G * cell;
    const double inv = 1.0 / cell;
    // The storms with a dominant / line tower within the grid's reach (+ a front's reach ahead of it).
    const double ph = cloudDriftPhase(), cph = std::cos(ph), sph = std::sin(ph);
    std::vector<std::pair<double, uint64_t>> ids;
    for (const wx::Tower &tw : stormTowers)
    {
        if ((tw.role < 0.99f && !wx::isLineSystem(tw.kind)) || tw.kind == wx::StormKind::Tropical ||
            tw.kind == wx::StormKind::AirMass) continue;
        const glm::dvec3 d = glm::normalize(tw.dir);
        const glm::dvec3 ed(d.x * cph + d.y * sph, -d.x * sph + d.y * cph, d.z);
        const double km = std::acos(std::clamp(glm::dot(ed, up), -1.0, 1.0)) * 6371.0;
        if (km > half / 1000.0 * 1.42 + 60.0) continue;
        bool have = false;
        for (auto &p : ids)
            if (p.second == tw.stormId) { p.first = std::min(p.first, km); have = true; }
        if (!have) ids.push_back({km, tw.stormId});
    }
    std::sort(ids.begin(), ids.end());
    std::vector<HaboobPoint> pts;
    int walls = 0;
    bool haveVel = false;
    // The stamps: one per dusty front point (its place on the plane, the front's axes, spacing, strength, head height).
    struct Stamp { double px, py, tx, ty, ox, oy, spc, e0, H0, s0; uint64_t seed; int ix0, ix1, iy0, iy1; };
    std::vector<Stamp> st;
    const double cLo = -26000.0, cHi = 1500.0, cAhead = 12000.0;   // across the front: the pall's end, the wall's foot, the rain-free air
    for (const auto &sid : ids)
    {
        const wx::Storm *S = stormCatalog->find(sid.second);
        if (!S) continue;
        haboobFrontOf(*S, t, pts);
        if (pts.empty()) continue;
        bool any = false;
        double arc = 0.0;   // the distance along the front (m): the heads' noise is a function of it
        for (size_t i = 0; i < pts.size(); ++i)
        {
            const HaboobPoint &q = pts[i];
            if (i > 0) arc += glm::length(pts[i].dir - pts[i - 1].dir) * kReH;
            const double k = haboobPointK(q);
            if (k < 0.08) continue;
            Stamp m;
            m.s0 = arc;
            m.seed = S->id;
            m.e0 = std::min(1.0, 2.0 * k);
            const uint64_t h = hbMix(S->id ^ (uint64_t)(i * 2654435761u));
            m.H0 = (1300.0 + 900.0 * std::min(1.0, k)) * (0.7 + 0.6 * ((h & 1023) / 1023.0));   // 0.9-2.9 km, head by head
            // The point on the plane, and the front's axes there.
            const glm::dvec3 sp = q.dir * kReH - sea;
            m.px = glm::dot(sp, E);
            m.py = glm::dot(sp, N);
            if (std::abs(m.px) > half + 30000.0 || std::abs(m.py) > half + 30000.0) continue;
            glm::dvec2 ta(glm::dot(q.tan, E), glm::dot(q.tan, N)), ou(glm::dot(q.out, E), glm::dot(q.out, N));
            if (glm::length(ta) < 1e-6 || glm::length(ou) < 1e-6) continue;
            ta = glm::normalize(ta);
            ou = glm::normalize(ou);
            m.tx = ta.x; m.ty = ta.y; m.ox = ou.x; m.oy = ou.y;
            // Its spacing along the front (the neighbours' distance).
            double spc = 1000.0;
            if (i > 0) spc = std::max(spc, glm::length(pts[i].dir - pts[i - 1].dir) * kReH);
            if (i + 1 < pts.size()) spc = std::max(spc, glm::length(pts[i + 1].dir - pts[i].dir) * kReH);
            m.spc = std::min(spc, 9000.0);
            const double aMax = 1.2 * m.spc;
            double bx0 = 1e30, bx1 = -1e30, by0 = 1e30, by1 = -1e30;
            for (int c = 0; c < 4; ++c)
            {
                const double a = (c & 1) ? aMax : -aMax, r = (c & 2) ? cAhead : cLo;
                const double x = m.px + ta.x * a + ou.x * r, y = m.py + ta.y * a + ou.y * r;
                bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); by0 = std::min(by0, y); by1 = std::max(by1, y);
            }
            m.ix0 = std::max(1, (int)std::floor(bx0 * inv + 0.5 * G - 0.5));
            m.ix1 = std::min(G - 3, (int)std::ceil(bx1 * inv + 0.5 * G - 0.5));
            m.iy0 = std::max(1, (int)std::floor(by0 * inv + 0.5 * G - 0.5));
            m.iy1 = std::min(G - 3, (int)std::ceil(by1 * inv + 0.5 * G - 0.5));
            if (m.ix0 > m.ix1 || m.iy0 > m.iy1) continue;
            st.push_back(m);
            maxTop = std::max(maxTop, 1.2 * m.H0);
            box.x = std::min(box.x, m.ix0); box.y = std::min(box.y, m.iy0);
            box.z = std::max(box.z, m.ix1 + 1); box.w = std::max(box.w, m.iy1 + 1);
            any = true;
        }
        if (any)
        {
            ++walls;
            if (!haveVel)
            {
                // The wall's motion over the ground: its storm's centre, Earth-fixed, over a minute.
                const glm::dvec3 a = weatherMapToEarth(glm::normalize(S->centreAt(t - 30.0)), t - 30.0);
                const glm::dvec3 b = weatherMapToEarth(glm::normalize(S->centreAt(t + 30.0)), t + 30.0);
                dustNoiseVel = (b - a) * (kReH / 60.0);
                haveVel = true;
            }
        }
    }
    // Rasterised in bands of rows on the worker threads (each band its own cells; ~1-1.5 ms in one thread).
    // A cell: bits 0-15 the wall's strength e (the strongest stamp's), 16-27 its top (2 m units, that stamp's),
    // 28-31 the clear air ahead of the front (any stamp's, max): the air ahead of the outflow sinks and is dry; the line's
    // shelf deck and the weather map's rain there hid the wall from the place a Go to puts you (clouds_v2.glsl cv2Field).
    if (!st.empty())
    {
        const int bands = 16;
        const int rows = box.w - box.y;
        parallelForHaboob((size_t)bands, [&](size_t b) {
            const int y0 = box.y + (int)((int64_t)rows * (int64_t)b / bands);
            const int y1 = box.y + (int)((int64_t)rows * (int64_t)(b + 1) / bands);
            for (const Stamp &m : st)
            {
                const int ya = std::max(y0, m.iy0), yb = std::min(y1 - 1, m.iy1);
                const double aMax = 1.2 * m.spc;
                for (int iy = ya; iy <= yb; ++iy)
                {
                    const double cy = (iy - 0.5 * G + 0.5) * cell - m.py;
                    for (int ix = m.ix0; ix <= m.ix1; ++ix)
                    {
                        const double cx = (ix - 0.5 * G + 0.5) * cell - m.px;
                        const double al = cx * m.tx + cy * m.ty, ac = cx * m.ox + cy * m.oy;
                        if (ac > cAhead || ac < cLo || std::abs(al) > aMax) continue;
                        const double wAl = 1.0 - glm::smoothstep(0.55 * m.spc, aMax, std::abs(al));
                        uint32_t &dst = dustGridCpu[(size_t)iy * G + ix];
                        const double clr = wAl * (1.0 - glm::smoothstep(9000.0, cAhead, ac)) * glm::smoothstep(-2500.0, 0.0, ac);
                        const uint32_t cq = (uint32_t)std::lround(clr * 15.0);
                        if (cq > (dst >> 28)) dst = (dst & 0x0FFFFFFFu) | (cq << 28);
                        if (ac > cHi) continue;
                        const double core = glm::smoothstep(cHi, -1000.0, ac);
                        const double pall = (1.0 - 0.5 * glm::smoothstep(-3000.0, -12000.0, ac)) *
                                            (1.0 - glm::smoothstep(-16000.0, cLo, ac));
                        // HEADS along the front: a smooth noise of the distance along it (3.5-km knots, per storm) sets each
                        // stretch's strength and height, so the wall is a row of rounded heads of different sizes, not a slab.
                        const double xs = (m.s0 + al) / 3500.0, x0 = std::floor(xs), fx = xs - x0;
                        const double n0 = (hbMix(m.seed ^ (uint64_t)(int64_t)x0) & 1023) / 1023.0;
                        const double n1 = (hbMix(m.seed ^ (uint64_t)(int64_t)(x0 + 1.0)) & 1023) / 1023.0;
                        const double hn = n0 + (n1 - n0) * fx * fx * (3.0 - 2.0 * fx);
                        const double e = m.e0 * (0.55 + 0.45 * hn) * core * pall * wAl;
                        if (e <= 0.004) continue;
                        const uint32_t eq = (uint32_t)std::min(65535.0, e * 65535.0);
                        if (eq <= (dst & 0xFFFFu)) continue;
                        const double top = m.H0 * (0.65 + 0.55 * hn) * (1.0 - 0.6 * glm::smoothstep(-2500.0, -12000.0, ac));
                        dst = (dst & 0xF0000000u) | eq | ((uint32_t)std::min(4095.0, top * 0.5) << 16);
                    }
                }
            }
        });
    }
    if (!haveVel) dustNoiseVel = glm::dvec3(0.0);
    frame[0] = glm::vec4(glm::vec3(E), (float)inv);
    frame[1] = glm::vec4(glm::vec3(N), (float)(1.35 * maxTop + 300.0));
    frame[3] = glm::vec4((float)(0.5 * kDustGridN - 0.5), (float)(0.5 * kDustGridN - 0.5), 0.0f, 0.0f);
    // Rows that changed: last time's box and this one's.
    auto *grid = reinterpret_cast<uint32_t *>(static_cast<char *>(stormMapped) + kDustGridOffset);
    const bool pE = prevBox.z <= prevBox.x, bE = box.z <= box.x;
    const int ux0 = pE ? box.x : bE ? prevBox.x : std::min(prevBox.x, box.x);
    const int uy0 = pE ? box.y : bE ? prevBox.y : std::min(prevBox.y, box.y);
    const int ux1 = pE ? box.z : bE ? prevBox.z : std::max(prevBox.z, box.z);
    const int uy1 = pE ? box.w : bE ? prevBox.w : std::max(prevBox.w, box.w);
    for (int y = uy0; y < uy1; ++y)
        std::memcpy(grid + (size_t)y * G + ux0, dustGridCpu.data() + (size_t)y * G + ux0, sizeof(uint32_t) * (size_t)(ux1 - ux0));
    dustWallsNow = walls;
    dustCellsNow = 0;
    if (box.z > box.x)
    {
        dustBox = box;
        // The shader reads cells i and i + 1 for g in [lo, hi): the box's border cells are 0, so the wall fades out in it.
        head[1] = (uint32_t)(box.x - 1) | ((uint32_t)(box.y - 1) << 16);
        head[2] = (uint32_t)(box.z) | ((uint32_t)(box.w) << 16);
        for (int y = box.y; y < box.w; ++y)
            for (int x = box.x; x < box.z; ++x) dustCellsNow += (dustGridCpu[(size_t)y * G + x] & 0xFFFFu) ? 1 : 0;
        head[0] = 1u;
    }
    else
    {
        dustBox = glm::ivec4(0, 0, 0, 0);
        head[0] = 0u;
    }
    dustGpuOn = head[0] != 0u;
    dustGridMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
