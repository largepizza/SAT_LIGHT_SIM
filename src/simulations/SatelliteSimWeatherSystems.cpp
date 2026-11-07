// Weather v2 (2026-11-06, .plans/WEATHER_V2_PLAN.md): the weather systems' upload. WeatherSystems.h is the model; this
// fills the bake's list (cv2_storms.glsl cv2SysHead / cv2Systems, read by cloud_v2_weather.comp wxSystems).
#include <unordered_map>
#include "SatelliteSim.h"
#include "../FrameProfile.h"
#include <algorithm>
#include <cmath>
#include <cstring>

void SatelliteSim::updateWeatherSystems(double t)
{
    FPROF_SCOPE("updateWeatherSystems");
    if (!stormMapped)
        return;
    wxSystemSettings.waves = wxWindWaves;
    wxSystemSettings.eddies = wxWindEddies;
    wxSystems.systemsAt(t, 0.0, wxSystemSettings, wxSystemsNow);
    // The strongest first (the list is capped).
    std::sort(wxSystemsNow.begin(), wxSystemsNow.end(),
              [](const wx::SystemState &a, const wx::SystemState &b) { return a.intensity > b.intensity; });
    const uint32_t n = (uint32_t)std::min<size_t>(wxSystemsNow.size(), kWxSystemMax);
    char *base = static_cast<char *>(stormMapped);
    auto *sys = reinterpret_cast<GpuWxSystem *>(base + kSystemsOffset);
    for (uint32_t i = 0; i < n; ++i)
    {
        const wx::SystemState &S = wxSystemsNow[i];
        // The reach: the ownership ellipse reaches 3.2 / 0.55 = 5.8 head radii down the cold front's trail.
        const double reach = 6.2 * S.radiusM;
        sys[i].a = glm::vec4(glm::vec3(S.centre), (float)S.radiusM);
        sys[i].b = glm::vec4(glm::vec3(S.heading), (float)S.intensity);
        sys[i].c = glm::vec4((float)S.age01, (float)S.hemi, (float)S.kind, (float)S.seed);
        sys[i].d = glm::vec4((float)S.speedMps, 0.0f, 0.0f, (float)std::cos(std::min(reach / 6371000.0, 3.0)));
    }
    auto *head = reinterpret_cast<uint32_t *>(base + kSysHeadOffset);
    head[0] = n; head[1] = head[2] = head[3] = 0u;
    convStatSystems = n;
    ctx_->mirrorDirty(stormMapped, kSysHeadOffset, 16 + 64ull * n);
}

// Weather v2 convection: the catalog's towers as convective CELLS and its storms as ANVIL sources, indexed on a 2-deg
// lat-lon grid, for the bake (cloud_v2_weather.comp wxConvection), which writes the convection cube the cumulus layer
// draws storms from (clouds_v2.glsl cv2StormGeom). Nothing here is geometry: the march never walks these lists.
void SatelliteSim::updateConvection(double t, const std::vector<const wx::Storm *> &storms)
{
    if (!stormMapped)
        return;
    char *base = static_cast<char *>(stormMapped);
    auto *cells = reinterpret_cast<GpuConvCell *>(base + kConvCellsOffset);
    auto *anvils = reinterpret_cast<GpuConvAnvil *>(base + kConvAnvilsOffset);
    convEntries.clear();
    const double kR = 6371000.0;
    auto binCircle = [&](const glm::dvec3 &c, double radiusM, uint32_t value) {
        const double lat = std::asin(std::clamp(c.z, -1.0, 1.0)) * 57.29577951, lon = std::atan2(c.y, c.x) * 57.29577951;
        const double dLat = radiusM / kR * 57.29577951;
        const int la0 = std::max(0, (int)std::floor((lat - dLat + 90.0) / 2.0));
        const int la1 = std::min((int)kConvGridLat - 1, (int)std::floor((lat + dLat + 90.0) / 2.0));
        const double cosMax = std::cos(std::min(std::abs(lat) + dLat, 89.0) / 57.29577951);
        const double dLon = dLat / std::max(cosMax, 1e-3);
        const bool allLon = dLon >= 180.0;
        const int lo0 = (int)std::floor((lon - dLon + 180.0) / 2.0), lo1 = (int)std::floor((lon + dLon + 180.0) / 2.0);
        for (int la = la0; la <= la1; ++la)
        {
            if (allLon)
                for (int lo = 0; lo < (int)kConvGridLon; ++lo) convEntries.push_back({(uint32_t)(la * kConvGridLon + lo), value});
            else
                for (int lo = lo0; lo <= lo1; ++lo)
                {
                    const int w = ((lo % (int)kConvGridLon) + (int)kConvGridLon) % (int)kConvGridLon;
                    convEntries.push_back({(uint32_t)(la * kConvGridLon + w), value});
                }
        }
    };
    // CELLS: every tower alive (the catalog's lifecycle in its strength and height cap).
    const double Rb = std::clamp((double)cv2CbRadiusKm, 1.0, 20.0) * 1000.0;
    // A line system's cells carry its motion: their gust front (the cold pool's leading edge) runs ahead of the line and
    // lifts a shelf cloud along it (the bake, wxConvection). It leads the columns more as the system matures.
    convLineFwd.clear();
    for (const wx::Storm *S : storms)
        if (wx::isLineSystem(S->kind) && glm::length(S->vel) > 0.5)
        {
            const double m = std::clamp((t - S->t0) / std::max((double)S->sysLife, 600.0), 0.0, 1.0);
            convLineFwd.push_back({S->id, glm::vec4(glm::vec3(glm::normalize(S->vel)), (float)((3.0 + 7.0 * std::sqrt(m)) * 1000.0))});
        }
    auto lineFwd = [&](uint64_t id, const glm::dvec3 &at) {
        for (const auto &e : convLineFwd)
            if (e.first == id)
            {
                glm::dvec3 f = glm::dvec3(e.second);
                f -= at * glm::dot(f, at);
                return glm::length(f) > 1e-9 ? glm::vec4(glm::vec3(glm::normalize(f)), e.second.w) : glm::vec4(0.0f);
            }
        return glm::vec4(0.0f);
    };
    // THE DRAWN HEIGHT'S LIFECYCLE (2026-11-06, the user: "rise and fall evolution is very sharp. It should accelerate,
    // decelerate, then change directions"): an S-curve (smootherstep) up over the tower-growth time, an S-curve down over
    // the tower's decay, x how tall its peak makes it. It was min(smoothstep(0.02, 0.6, strength), a cubic ease-out cap):
    // saturated at 60% of the peak, the top shot up, sat flat, then dropped as the strength fell under 0.6. The anvil
    // starts when the drawn top passes ~0.85 of the way (kAnvilAt of the rise), not when the strength did.
    const double riseS = std::clamp((double)stormTowerRiseMin, 0.0, 120.0) * 60.0;
    auto s5 = [](double x) { x = std::clamp(x, 0.0, 1.0); return x * x * x * (x * (x * 6.0 - 15.0) + 10.0); };
    constexpr double kAnvilAt = 0.72;   // s5(0.72) ~ 0.85
    // The drawn rise starts when the column APPEARS: its width is sigma sqrt(ln(str / 0.35)) (cv2CellColumn), nothing
    // until the strength (peak x smooth01 over growS) passes 0.35. Timed from the tower's birth, the top had climbed a
    // third of the way by then: a 5-km tower out of clear air in a minute, no cumulus growing into it (2026-11-07, the
    // report's ITCZ on video).
    constexpr double kColumnStr = 0.35;
    auto visibleAt = [](double pk, double growS) {
        if (pk <= kColumnStr || growS <= 0.0) return 0.0;
        double lo = 0.0, hi = 1.0;   // x with x^2 (3 - 2x) = 0.35 / peak (smooth01's inverse)
        for (int i = 0; i < 24; ++i) { const double m = 0.5 * (lo + hi); ((m * m * (3.0 - 2.0 * m)) < kColumnStr / pk ? lo : hi) = m; }
        return growS * 0.5 * (lo + hi);
    };
    uint32_t nC = 0;
    for (const wx::Tower &T : stormTowers)
    {
        if (nC >= kConvCellMax) break;
        if (T.strength <= 0.02f) continue;
        // (A tropical cyclone's eyewall / band towers: its eyewall and bands are the bake's own fields now, wxCyclones;
        // as cells too they drew a second eyewall ring off the eye.)
        if (T.kind == wx::StormKind::Tropical) continue;
        const double str = T.strength;
        auto ss = [](double e0, double e1, double x) { const double u = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0); return u * u * (3 - 2 * u); };
        const double Rc = Rb * (0.7 + 0.6 * T.size) * (0.8 + 0.2 * ss(0.1, 0.6, str)) * (0.75 + 0.25 * T.role);
        // The core: its 0.45 contour ~0.6 Rc out at full strength (an updraft core ~10-20 km across at the default 15-km
        // "Cb tower radius", which sized the old geometry's base; at 1.12 Rc the columns stood ~36 km wide).
        // (0.35 Rc: a core ~11 km across at the 15-km "Cb tower radius", a real updraft tower's ~10; at 0.7 Rc the
        // columns stood 25 km wide)
        const double sigma = 0.35 * Rc;
        const double up = riseS > 0.0 ? s5(((double)T.age - visibleAt(T.peak, T.growS)) / riseS) : 1.0;
        const double dn = T.decayS > 0.0f ? s5(((double)T.life - (double)T.age) / (double)T.decayS) : 1.0;
        const double top = ss(0.2, 0.6, (double)T.peak) * up * dn * T.role;
        cells[nC].a = glm::vec4(glm::vec3(glm::normalize(T.dir)), (float)sigma);
        uint32_t seedBits = T.seed;   // z: the tower's seed as float bits (the lightning's stable per-cell hash)
        float seedF;
        std::memcpy(&seedF, &seedBits, 4);
        cells[nC].b = glm::vec4((float)str, (float)top, seedF, T.role);
        // (c: a line cell's forward + gust-front lead; a single cell's DECAY as -w, 0 .. 1 over its decay: the march softens a
        // dying tower's billows -- clouds_v2.glsl cv2CellColumn -- the user: "on decay, clouds continue to move up")
        cells[nC].c = wx::isLineSystem(T.kind) ? lineFwd(T.stormId, glm::normalize(T.dir)) : glm::vec4(0.0f, 0.0f, 0.0f, -(float)(1.0 - dn));
        // (the bake's environment field reaches 2.5 sigma wide; a gust front its lead + its clear band ahead)
        binCircle(glm::normalize(T.dir), std::max(4.5 * sigma, 1.2 * Rc + cells[nC].c.w + 27000.0 * (cells[nC].c.w > 0.0f)), nC);
        ++nC;
    }
    // The harness's test storm (`storms test`): one cell rising over ~25 min, mature, decaying over its last 40 min.
    auto ssD = [](double e0, double e1, double x) { const double u = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0); return u * u * (3 - 2 * u); };
    const WxTestStorm &TS = wxTestStorm;
    const double tsAge = t - TS.t0;
    const double tsLat = glm::radians(TS.lat), tsLon = glm::radians(TS.lon);
    const glm::dvec3 tsDir(std::cos(tsLat) * std::cos(tsLon), std::cos(tsLat) * std::sin(tsLon), std::sin(tsLat));
    // (kind 1: a squall line: 12 cells over 200 km along az, bowed 25 km forward at its middle, of varying strength)
    const glm::dvec3 tsE = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), tsDir) + glm::dvec3(1e-9, 0, 0)), tsN = glm::cross(tsDir, tsE);
    const glm::dvec3 tsAx = tsE * std::cos(glm::radians(TS.az)) + tsN * std::sin(glm::radians(TS.az));
    const glm::dvec3 tsFw = glm::cross(tsDir, tsAx) * -1.0;   // forward: the axis turned 90 deg clockwise
    const double tsVis = visibleAt(TS.str, 1500.0);   // (its strength: TS.str x smooth01 over its first 25 min)
    const double tsUp = riseS > 0.0 ? s5((tsAge - tsVis) / riseS) : 1.0, tsDn = s5((TS.life - tsAge) / 2400.0);   // (as a catalog tower's)
    if (TS.on && TS.kind == 1 && tsAge > 0.0 && tsAge < TS.life)
    {
        const double str0 = TS.str * ssD(0.0, 1500.0, tsAge) * (1.0 - ssD(TS.life - 2400.0, TS.life, tsAge));
        for (int k = 0; k < 12 && nC < kConvCellMax; ++k)
        {
            const double u = (k + 0.5) / 12.0 * 2.0 - 1.0;
            const uint64_t hk = (uint64_t)(k * 2654435761u + 12345u);
            const double hv = (double)((hk >> 8) & 0xFFFF) / 65535.0, hv2 = (double)((hk >> 3) & 0xFFFF) / 65535.0;
            const glm::dvec3 d = glm::normalize(tsDir + (tsAx * (u * 1.0e5 + (hv2 - 0.5) * 6000.0) + tsFw * (25000.0 * (1.0 - u * u))) / kR);
            const double str = str0 * (0.6 + 0.4 * hv);
            const double sg = 0.35 * Rb * (0.8 + 0.3 * hv2);
            uint32_t sb = 0x7E57u + (uint32_t)k; float sf; std::memcpy(&sf, &sb, 4);
            cells[nC].a = glm::vec4(glm::vec3(d), (float)sg);
            cells[nC].b = glm::vec4((float)str, (float)(tsUp * tsDn * ssD(0.2, 0.6, TS.str * (0.6 + 0.4 * hv))), sf, 1.0f);
            const double lead = (3.0 + 7.0 * std::sqrt(std::clamp(tsAge / std::max(TS.life, 600.0), 0.0, 1.0))) * 1000.0;
            glm::dvec3 fw = tsFw - d * glm::dot(tsFw, d);
            cells[nC].c = glm::vec4(glm::vec3(glm::normalize(fw)), (float)lead);
            binCircle(d, std::max(4.5 * sg, 3.5 * sg + lead + 27000.0), nC);
            ++nC;
        }
    }
    if (TS.on && TS.kind == 0 && tsAge > 0.0 && tsAge < TS.life && nC < kConvCellMax)
    {
        const double str = TS.str * ssD(0.0, 1500.0, tsAge) * (1.0 - ssD(TS.life - 2400.0, TS.life, tsAge));
        const double Rc = Rb * 1.0;
        cells[nC].a = glm::vec4(glm::vec3(tsDir), (float)(0.35 * Rc));
        uint32_t sb = 0x7E57u; float sf; std::memcpy(&sf, &sb, 4);
        cells[nC].b = glm::vec4((float)str, (float)(tsUp * tsDn * ssD(0.2, 0.6, TS.str)), sf, 1.0f);
        cells[nC].c = glm::vec4(0.0f, 0.0f, 0.0f, -(float)(1.0 - tsDn));
        binCircle(tsDir, 4.5 * 0.35 * Rc, nC);
        ++nC;
    }
    // ANVIL sources: each storm whose towers reached the lid, from the moment the first did; it feeds the anvil until the
    // last falls back, then what it put up drifts downwind and decays (its linger). Line systems feed along the line.
    uint32_t nA = 0;
    // The bin circle round the oldest material's midpoint (0.5 U age downwind of the source) must hold every tap the bake
    // sums: a disc 1.1 x its spread radius rk, centred a further 0.6 (rk - r0) downwind, + the line's
    // half-length. Short of it (2.5 r0 + 7 s until 2026-11-06) a mature anvil was cut ~20 km in along the 2-degree grid's
    // lines: straight-edged slabs and parallelograms (issue_20261106_171931 shot 2; the map overlay's anvil layer).
    // (Its spread radius rk = r0 + kAnvSpreadM sqrt(s / 1 h), s <= 4 h: = cloud_v2_weather.comp kCv2AnvSpreadM.)
    constexpr double kAnvSpreadM = 45000.0;
    auto anvilBinRadius = [&](double U, double age, double r0, double half) {
        const double rk = r0 + kAnvSpreadM * std::sqrt(std::min(age, 4.0 * 3600.0) / 3600.0);
        // (its disc centred 0.6 (rk - r0) on, 1.1 rk downwind x its outline's lobes, up to 1.35 (the bake's kO): 2.1 rk)
        return 0.5 * U * age + 2.1 * rk - 0.6 * r0 + half + 5000.0;
    };
    for (const wx::Storm *S : storms)
    {
        if (nA >= kConvAnvilMax) break;
        const double tau = t - S->t0;
        double on = 1e30, off = -1e30, peak = 0.0;
        if (wx::isLineSystem(S->kind))
        {
            on = 0.15 * S->sysLife; off = S->sysLife; peak = 0.9;
        }
        else
            for (int k = 0; k < S->towerCount; ++k)
            {
                const wx::TowerSpec &ts = S->towers[k];
                if (ts.anvOff <= ts.anvOn) continue;
                on = std::min(on, std::max((double)ts.anvOn, (double)ts.t0 + visibleAt(ts.peak, ts.growS) + kAnvilAt * riseS));
                off = std::max(off, (double)ts.anvOff);
                peak = std::max(peak, (double)ts.peak * ts.role);
            }
        if (on > 1e29 || tau < on) continue;
        const double lidAge = tau - on, feed = std::max(off - on, 60.0), linger = std::max((double)S->linger, 600.0);
        if (lidAge > feed + 4.0 * linger) continue;
        glm::dvec3 c = glm::normalize(S->centreAt(t));
        // Where the anvil is FED: its anvil towers' centroid (a tower stands up to ~20 km off the storm's centre: fed from the
        // centre, the anvil grew over clear air beside its storm), each weighted by (peak x role)^2, ramping in over the 15
        // min before its column reaches the lid and fading at e^-1 per 30 min after it falls back, so the point slides
        // between a multicell's towers and freezes once all have fallen (their ratios then constant). From the towers ALIVE
        // it snapped to the storm's centre when the last one ended: the whole anvil, which is integrated from this one
        // point, jumped ~20 km in a frame (2026-11-07, the report's ITCZ on video).
        if (!wx::isLineSystem(S->kind))
        {
            const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), c) + glm::dvec3(1e-9, 0, 0));
            const glm::dvec3 north = glm::cross(c, east);
            glm::dvec3 off(0.0);
            double wSum = 0.0;
            for (int k = 0; k < S->towerCount; ++k)
            {
                const wx::TowerSpec &ts = S->towers[k];
                if (ts.anvOff <= ts.anvOn) continue;
                const double onK = std::max((double)ts.anvOn, (double)ts.t0 + visibleAt(ts.peak, ts.growS) + kAnvilAt * riseS);
                const double pr = (double)ts.peak * ts.role;
                const double w = pr * pr * ssD(onK - 900.0, onK, tau) * std::exp(-std::max(tau - (double)ts.anvOff, 0.0) / 1800.0);
                off += (east * (double)ts.offE + north * (double)ts.offN) * w;
                wSum += w;
            }
            if (wSum > 1e-12) c = glm::normalize(c + off / (wSum * kR));
        }
        glm::dvec3 dw = glm::dvec3(S->shear);
        // A line system's shield and its trailing stratiform go REARWARD of its motion (leading line, trailing stratiform:
        // the front-to-rear flow aloft), at least ~10 m/s system-relative.
        if (wx::isLineSystem(S->kind) && glm::length(S->vel) > 0.5)
            dw = -S->vel;
        dw -= c * glm::dot(dw, c);
        dw = glm::length(dw) > 1e-9 ? glm::normalize(dw) : glm::normalize(glm::cross(glm::dvec3(0, 0, 1), c) + glm::dvec3(1e-9, 0, 0));
        const double r0 = (wx::isLineSystem(S->kind) ? 1.1 : 0.8) * Rb;
        const double U = wx::isLineSystem(S->kind) ? std::max((double)S->upperWind, 10.0) : std::max((double)S->upperWind, 0.0);
        glm::dvec3 axis(0.0);
        double half = 0.0;
        if (wx::isLineSystem(S->kind))
        {
            const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), c) + glm::dvec3(1e-9, 0, 0));
            const glm::dvec3 north = glm::cross(c, east);
            axis = east * std::cos((double)S->lineAz) + north * std::sin((double)S->lineAz);
            half = S->lineHalfM;
        }
        // (a bowed line's towers stand up to `bow` ahead of its centre: its shield is fed from the bow, ~0.7 of it forward,
        // or it stood apart from its towers with clear air between)
        glm::dvec3 cA = c;
        if (wx::isLineSystem(S->kind) && glm::length(S->vel) > 0.5)
            cA = glm::normalize(c - dw * (0.7 * (double)S->bow / kR));
        // It DISSOLVES over the last half linger (<= 1 h) of the storm's life: the catalog drops a storm at its towers' end +
        // one linger, when the outflow still holds e^-1 of itself -- a saturated plate -- and a whole anvil vanished from
        // the sky in a frame (2026-11-07, the report's ITCZ from orbit, at a catalog time bin's edge).
        const double endW = std::clamp(0.5 * linger, 600.0, 3600.0);
        peak *= 1.0 - ssD((double)S->life - endW, (double)S->life, tau);
        anvils[nA].a = glm::vec4(glm::vec3(cA), (float)r0);
        anvils[nA].b = glm::vec4(glm::vec3(dw), (float)peak);
        anvils[nA].c = glm::vec4((float)lidAge, (float)feed, (float)linger, (float)U);
        anvils[nA].d = glm::vec4((float)half, glm::vec3(axis));
        // Its reach: the drift of the oldest material it still holds, the spread and the line, from the source.
        const double age = std::min(lidAge, feed + 4.0 * linger);
        binCircle(c + dw * (0.5 * U * age / kR), anvilBinRadius(U, age, r0, half), 0x80000000u | nA);
        ++nA;
    }
    if (TS.on && nA < kConvAnvilMax)
    {
        // Its anvil: fed from when the column nears the lid (~20 min) to 40 min before its end, carried by the 10-km wind.
        const double on = std::max(tsVis + kAnvilAt * riseS, 60.0), off = std::max(TS.life - 2400.0, on + 60.0);
        if (tsAge > on && tsAge - on < (off - on) + 4.0 * 3600.0)
        {
            const wx::WindState ws = windStateNow();
            glm::dvec3 U = wx::windAt(tsDir, 10000.0, ws);
            U -= tsDir * glm::dot(U, tsDir);
            double Um = glm::length(U);
            glm::dvec3 dw = Um > 0.1 ? U / Um : glm::normalize(glm::cross(glm::dvec3(0, 0, 1), tsDir) + glm::dvec3(1e-9, 0, 0));
            const bool line = TS.kind == 1;
            if (line) { dw = -tsFw; Um = 12.0; }   // (a line's shield trails rearward)
            const double r0 = (line ? 1.1 : 0.8) * Rb, lidAge = tsAge - on, age = std::min(lidAge, (off - on) + 4.0 * 3600.0);
            const glm::dvec3 cA = line ? glm::normalize(tsDir + tsFw * (0.7 * 25000.0 / kR)) : tsDir;   // (the bow, as above)
            anvils[nA].a = glm::vec4(glm::vec3(cA), (float)r0);
            anvils[nA].b = glm::vec4(glm::vec3(dw), (float)TS.str);
            anvils[nA].c = glm::vec4((float)lidAge, (float)(off - on), 3600.0f, (float)Um);
            anvils[nA].d = line ? glm::vec4(1.0e5f, glm::vec3(tsAx)) : glm::vec4(0.0f);
            binCircle(glm::normalize(tsDir + dw * (0.5 * Um * age / kR)), anvilBinRadius(Um, age, r0, line ? 1.0e5 : 0.0),
                      0x80000000u | nA);
            ++nA;
        }
    }
    // The grid: a counting sort of the entries by cell.
    const uint32_t nCells = kConvGridLon * kConvGridLat;
    convGridCount.assign(nCells, 0u);
    for (const auto &e : convEntries) ++convGridCount[e.first];
    convGridStart.assign(nCells, 0u);
    uint32_t acc = 0;
    for (uint32_t i = 0; i < nCells; ++i) { convGridStart[i] = acc; acc += convGridCount[i]; }
    const uint32_t used = std::min<uint32_t>(acc, kConvListMax);
    auto *grid = reinterpret_cast<uint32_t *>(base + kConvGridOffset);
    auto *list = reinterpret_cast<uint32_t *>(base + kConvListOffset);
    std::vector<uint32_t> &fill = convGridCount;   // (reused as the write cursor)
    for (uint32_t i = 0; i < nCells; ++i) fill[i] = 0u;
    for (const auto &e : convEntries)
    {
        const uint32_t at = convGridStart[e.first] + fill[e.first]++;
        if (at < kConvListMax) list[at] = e.second;
    }
    for (uint32_t i = 0; i < nCells; ++i)
    {
        const uint32_t s0 = std::min(convGridStart[i], used);
        grid[2 * i] = s0;
        grid[2 * i + 1] = std::min(fill[i], used - s0);
    }
    auto *head = reinterpret_cast<uint32_t *>(base + kConvHeadOffset);
    head[0] = nC; head[1] = nA; head[2] = used; head[3] = 0u;
    convStatCells = nC; convStatAnvils = nA; convStatEntries = acc;
    convCellsN = nC;
    ctx_->mirrorDirty(stormMapped, kConvHeadOffset, 16 + 48ull * nC);
    ctx_->mirrorDirty(stormMapped, kConvAnvilsOffset, 64ull * nA);
    ctx_->mirrorDirty(stormMapped, kConvGridOffset, 8ull * nCells);
    ctx_->mirrorDirty(stormMapped, kConvListOffset, 4ull * used);
}
