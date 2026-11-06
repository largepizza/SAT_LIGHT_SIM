// Weather v2 (2026-11-06, .plans/WEATHER_V2_PLAN.md): the weather systems' upload. WeatherSystems.h is the model; this
// fills the bake's list (cv2_storms.glsl cv2SysHead / cv2Systems, read by cloud_v2_weather.comp wxSystems).
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
        const double top = std::min(ss(0.02, 0.6, str), (double)T.heightCap) * T.role;
        cells[nC].a = glm::vec4(glm::vec3(glm::normalize(T.dir)), (float)sigma);
        uint32_t seedBits = T.seed;   // z: the tower's seed as float bits (the lightning's stable per-cell hash)
        float seedF;
        std::memcpy(&seedF, &seedBits, 4);
        cells[nC].b = glm::vec4((float)str, (float)top, seedF, T.role);
        binCircle(glm::normalize(T.dir), 4.5 * sigma, nC);   // (the bake's environment field reaches 2.5 sigma wide)
        ++nC;
    }
    // The harness's test storm (`storms test`): one cell rising over ~25 min, mature, decaying over its last 40 min.
    auto ssD = [](double e0, double e1, double x) { const double u = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0); return u * u * (3 - 2 * u); };
    const WxTestStorm &TS = wxTestStorm;
    const double tsAge = t - TS.t0;
    const double tsLat = glm::radians(TS.lat), tsLon = glm::radians(TS.lon);
    const glm::dvec3 tsDir(std::cos(tsLat) * std::cos(tsLon), std::cos(tsLat) * std::sin(tsLon), std::sin(tsLat));
    if (TS.on && tsAge > 0.0 && tsAge < TS.life && nC < kConvCellMax)
    {
        const double str = TS.str * ssD(0.0, 1500.0, tsAge) * (1.0 - ssD(TS.life - 2400.0, TS.life, tsAge));
        const double Rc = Rb * 1.0;
        cells[nC].a = glm::vec4(glm::vec3(tsDir), (float)(0.35 * Rc));
        uint32_t sb = 0x7E57u; float sf; std::memcpy(&sf, &sb, 4);
        cells[nC].b = glm::vec4((float)str, (float)ssD(0.02, 0.6, str), sf, 1.0f);
        binCircle(tsDir, 4.5 * 0.35 * Rc, nC);
        ++nC;
    }
    // ANVIL sources: each storm whose towers reached the lid, from the moment the first did; it feeds the anvil until the
    // last falls back, then what it put up drifts downwind and decays (its linger). Line systems feed along the line.
    uint32_t nA = 0;
    const double spread = 7.0;   // m/s: the outflow spreading at the lid (= the bake's, wxConvection, for 2 h)
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
                on = std::min(on, (double)ts.anvOn);
                off = std::max(off, (double)ts.anvOff);
                peak = std::max(peak, (double)ts.peak * ts.role);
            }
        if (on > 1e29 || tau < on) continue;
        const double lidAge = tau - on, feed = std::max(off - on, 60.0), linger = std::max((double)S->linger, 600.0);
        if (lidAge > feed + 4.0 * linger) continue;
        const glm::dvec3 c = glm::normalize(S->centreAt(t));
        glm::dvec3 dw = glm::dvec3(S->shear);
        dw -= c * glm::dot(dw, c);
        dw = glm::length(dw) > 1e-9 ? glm::normalize(dw) : glm::normalize(glm::cross(glm::dvec3(0, 0, 1), c) + glm::dvec3(1e-9, 0, 0));
        const double r0 = (wx::isLineSystem(S->kind) ? 1.1 : 0.8) * Rb;
        const double U = std::max((double)S->upperWind, 0.0);
        glm::dvec3 axis(0.0);
        double half = 0.0;
        if (wx::isLineSystem(S->kind))
        {
            const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), c) + glm::dvec3(1e-9, 0, 0));
            const glm::dvec3 north = glm::cross(c, east);
            axis = east * std::cos((double)S->lineAz) + north * std::sin((double)S->lineAz);
            half = S->lineHalfM;
        }
        anvils[nA].a = glm::vec4(glm::vec3(c), (float)r0);
        anvils[nA].b = glm::vec4(glm::vec3(dw), (float)peak);
        anvils[nA].c = glm::vec4((float)lidAge, (float)feed, (float)linger, (float)U);
        anvils[nA].d = glm::vec4((float)half, glm::vec3(axis));
        // Its reach: the drift of the oldest material it still holds, the spread and the line, from the source.
        const double age = std::min(lidAge, feed + 4.0 * linger);
        const double reach = U * age + r0 * 2.5 + spread * std::min(age, 2.0 * 3600.0) + half;
        binCircle(c + dw * (0.5 * U * age / kR), 0.5 * U * age + r0 * 2.5 + spread * std::min(age, 2.0 * 3600.0) + half,
                  0x80000000u | nA);
        (void)reach;
        ++nA;
    }
    if (TS.on && nA < kConvAnvilMax)
    {
        // Its anvil: fed from when the column nears the lid (~20 min) to 40 min before its end, carried by the 10-km wind.
        const double on = 1200.0, off = std::max(TS.life - 2400.0, on + 60.0);
        if (tsAge > on && tsAge - on < (off - on) + 4.0 * 3600.0)
        {
            const wx::WindState ws = windStateNow();
            glm::dvec3 U = wx::windAt(tsDir, 10000.0, ws);
            U -= tsDir * glm::dot(U, tsDir);
            const double Um = glm::length(U);
            const glm::dvec3 dw = Um > 0.1 ? U / Um : glm::normalize(glm::cross(glm::dvec3(0, 0, 1), tsDir) + glm::dvec3(1e-9, 0, 0));
            const double r0 = 0.8 * Rb, lidAge = tsAge - on, age = std::min(lidAge, (off - on) + 4.0 * 3600.0);
            anvils[nA].a = glm::vec4(glm::vec3(tsDir), (float)r0);
            anvils[nA].b = glm::vec4(glm::vec3(dw), (float)TS.str);
            anvils[nA].c = glm::vec4((float)lidAge, (float)(off - on), 3600.0f, (float)Um);
            anvils[nA].d = glm::vec4(0.0f);
            binCircle(glm::normalize(tsDir + dw * (0.5 * Um * age / kR)),
                      0.5 * Um * age + r0 * 2.5 + spread * std::min(age, 2.0 * 3600.0), 0x80000000u | nA);
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
    ctx_->mirrorDirty(stormMapped, kConvHeadOffset, 16 + 32ull * nC);
    ctx_->mirrorDirty(stormMapped, kConvAnvilsOffset, 64ull * nA);
    ctx_->mirrorDirty(stormMapped, kConvGridOffset, 8ull * nCells);
    ctx_->mirrorDirty(stormMapped, kConvListOffset, 4ull * used);
}
