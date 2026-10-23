// Weather events, the sim's side (.plans/WEATHER_EVENTS_PLAN.md, 2026-10-22): the storm catalog (Weather.h) asked
// for the towers alive at the frame's sim time, and the storm buffer the cloud shaders read them from
// (shaders/include/cv2_storms.glsl: tower records + an open-addressed table of cube-face bins over them).
//
// The buffer is device-local + host-visible like the flash buffer (every cloud sample near a storm reads it), and
// rewritten on the CPU whenever sim time or the storm settings moved: positions and strengths change continuously.
// Single frame in flight: the previous frame's GPU work has finished when buildUI/recordCompute run.
#include "../FrameProfile.h"
#include "SatelliteSim.h"
#include "SatPhotometry.h"
#include "StormClimatology.h"
#include "Weather.h"

#include "Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace
{
constexpr double kReW = 6371000.0;

// The bin key of a map-frame unit vector, as cv2_storms.glsl's cv2StormBinKey (same face rule, same rounding up to
// float noise at a bin's edge: a tower is listed in every bin its padded reach touches, so a disagreement there
// only ever falls where the tower has faded to nothing).
uint32_t stormBinKey(const glm::dvec3 &wd, uint32_t n, int *faceOut = nullptr)
{
    const glm::dvec3 aw = glm::abs(wd);
    const int fc = (aw.x >= aw.y && aw.x >= aw.z) ? 0 : ((aw.y >= aw.z) ? 1 : 2);
    const double mj = (fc == 0) ? wd.x : ((fc == 1) ? wd.y : wd.z);
    const double ux = (fc == 0) ? wd.y : wd.x, uy = (fc == 2) ? wd.y : wd.z;
    const uint32_t face = (uint32_t)(fc * 2) + (mj < 0.0 ? 1u : 0u);
    const double ax = std::atan(ux / std::abs(mj)), ay = std::atan(uy / std::abs(mj));
    const double s = (double)n / 1.5707963267948966;
    const uint32_t i = (uint32_t)std::clamp(std::floor((ax + 0.7853981633974483) * s), 0.0, (double)(n - 1));
    const uint32_t j = (uint32_t)std::clamp(std::floor((ay + 0.7853981633974483) * s), 0.0, (double)(n - 1));
    if (faceOut) *faceOut = (int)face;
    return 1u + face * n * n + j * n + i;
}

uint32_t stormHash(uint32_t k)
{
    k ^= k >> 16; k *= 0x7FEB352Du; k ^= k >> 15; k *= 0x846CA68Bu; k ^= k >> 16;
    return k;
}

// A face's equal-angle coordinates of a direction (for the bin rectangle around a tower on that face).
bool faceAngles(const glm::dvec3 &d, int face, double &ax, double &ay)
{
    const int fc = face >> 1;
    const double sg = (face & 1) ? -1.0 : 1.0;
    const double mj = sg * ((fc == 0) ? d.x : ((fc == 1) ? d.y : d.z));
    if (mj <= 1e-3) return false;
    const double ux = (fc == 0) ? d.y : d.x, uy = (fc == 2) ? d.y : d.z;
    ax = std::atan(ux / mj);
    ay = std::atan(uy / mj);
    return true;
}
} // namespace

void SatelliteSim::createStormBuffer(VulkanContext &ctx)
{
    bool done = false;
    try
    {
        ctx.createBuffer(kStormBufBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                             | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         stormBuf, stormMem);
        done = true;
    }
    catch (const std::exception &)
    {
        if (stormBuf) vkDestroyBuffer(ctx.device, stormBuf, nullptr);
        stormBuf = VK_NULL_HANDLE;
        stormMem = VK_NULL_HANDLE;
    }
    if (!done)
        ctx.createBuffer(kStormBufBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stormBuf, stormMem);
    vkMapMemory(ctx.device, stormMem, 0, kStormBufBytes, 0, &stormMapped);
    std::memset(stormMapped, 0, kStormBufBytes);   // no towers until the first update
    stormTable.assign(kStormTableSize, glm::uvec2(0u));
    Log::line(std::string("weather: storm buffer (") + std::to_string(kStormBufBytes >> 10) + " KB) in "
              + (done ? "device-local host-visible memory" : "host memory"));
}

void SatelliteSim::destroyStormBuffer(VkDevice device)
{
    if (stormMapped) vkUnmapMemory(device, stormMem);
    stormMapped = nullptr;
    if (stormBuf) vkDestroyBuffer(device, stormBuf, nullptr);
    if (stormMem) vkFreeMemory(device, stormMem, nullptr);
    stormBuf = VK_NULL_HANDLE;
    stormMem = VK_NULL_HANDLE;
}

// Everything the catalog's answers depend on besides sim time: the cloud map's drift (storms live in its frame and
// their spawn reads its cover), the coverage remap, and the storm settings.
uint64_t SatelliteSim::stormEnvHash() const
{
    uint64_t h = 1469598103934665603ull;
    auto mixD = [&](double v) { uint64_t b; std::memcpy(&b, &v, 8); h = (h ^ b) * 1099511628211ull; };
    for (double v : {(double)cloudDriftRate, cloudDriftPhaseOffset, (double)cv2Coverage, (double)cv2CoverClear,
                     (double)cv2CoverFull, (double)stormFrequency, (double)cv2CbRadiusKm, (double)stormLifeScale})
        mixD(v);
    return h;
}

void SatelliteSim::updateStorms()
{
    FPROF_SCOPE("updateStorms");
    if (!stormMapped)
        return;
    const auto t0 = std::chrono::steady_clock::now();
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    if (!stormCatalog)
    {
        stormClim = std::make_unique<wx::StormClimatology>();
        if (stormClim->load("assets/textures/storm_climatology.bin"))
            Log::line("weather: storm climatology loaded (assets/textures/storm_climatology.bin)");
        else
            Log::line("weather: no storm climatology table; using the analytic model");
        stormCatalog = std::make_unique<wx::Catalog>();
        stormCatalog->setBuildThreads(std::clamp((int)std::thread::hardware_concurrency() / 2, 1, 6));
    }
    const uint64_t key = stormEnvHash() ^ (stormObjects ? 0x9E3779B97F4A7C15ull : 0ull);
    if (key != stormEnvKey)
    {
        wx::Env env;
        // The map's cover over ~100 km: the CPU's 1024 x 512 copy (~39 km a texel), 3 x 3 texels, remapped like the
        // shaders' coverage (clamp((b x Coverage - clear) / (full - clear))). The image IS the map frame.
        env.moisture = [this](const glm::dvec3 &d) -> float {
            if (earthCloudsCpu.empty()) return 0.5f;
            const double lon = std::atan2(d.y, d.x), lat = std::asin(std::clamp(d.z, -1.0, 1.0));
            const double u = (lon + glm::pi<double>()) / glm::two_pi<double>() * earthCloudsCpuW;
            const double v = (0.5 * glm::pi<double>() - lat) / glm::pi<double>() * earthCloudsCpuH;
            const int cx = (int)std::floor(u), cy = (int)std::floor(v);
            const float span = std::max(cv2CoverFull - cv2CoverClear, 1e-3f);
            float acc = 0.0f;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i)
                {
                    const int x = ((cx + i) % earthCloudsCpuW + earthCloudsCpuW) % earthCloudsCpuW;
                    const int y = std::clamp(cy + j, 0, earthCloudsCpuH - 1);
                    const float b = earthCloudsCpu[(size_t)y * earthCloudsCpuW + x] / 255.0f;
                    acc += std::clamp((b * cv2Coverage - cv2CoverClear) / span, 0.0f, 1.0f);
                }
            return acc / 9.0f;
        };
        // Map frame -> Earth-fixed: undo the drift (cv2Drift turns Earth-fixed by +phase about the pole).
        const double rate = (double)cloudDriftRate, off = cloudDriftPhaseOffset;
        env.mapToEarth = [rate, off](const glm::dvec3 &d, double tt) -> glm::dvec3 {
            const double ph = rate * (tt - kCloudDriftEpochS) + off;
            const double c = std::cos(ph), s = std::sin(ph);
            return glm::dvec3(d.x * c + d.y * s, -d.x * s + d.y * c, d.z);
        };
        env.land = [this](const glm::dvec3 &e) -> float {
            if (oceanMaskCpu.empty()) return 0.3f;
            const double lat = std::asin(std::clamp(e.z, -1.0, 1.0)), lon = std::atan2(e.y, e.x);
            return 1.0f - oceanFraction(lat, lon, 30000.0, 1);
        };
        env.sunEarth = [](double tt) -> glm::dvec3 {
            const glm::dvec3 si = sunDirEciAt(tt);
            const double g = earthRotationAngle(tt);
            return glm::normalize(glm::dvec3(si.x * std::cos(g) + si.y * std::sin(g),
                                             -si.x * std::sin(g) + si.y * std::cos(g), si.z));
        };
        wx::Settings s;
        s.frequency = stormObjects ? std::max(stormFrequency, 0.0f) : 0.0f;
        s.towerRadiusM = std::clamp(cv2CbRadiusKm, 1.0f, 20.0f) * 1000.0f;
        s.lifeScale = std::max(stormLifeScale, 0.05f);
        stormCatalog->configure(env, s, stormClim.get(), key);
        stormEnvKey = key;
        stormUploadedT = -1e300;
    }
    if (t == stormUploadedT)
        return;   // paused: nothing moved
    stormUploadedT = t;
    stormCatalog->towersAt(t, stormTowers);

    // Strongest first: past the buffer's capacity the weakest go, and a crowded bin keeps its strongest.
    std::sort(stormTowers.begin(), stormTowers.end(), [](const wx::Tower &a, const wx::Tower &b) {
        return a.strength != b.strength ? a.strength > b.strength
                                        : (a.stormId != b.stormId ? a.stormId < b.stormId : a.towerIndex < b.towerIndex);
    });
    const uint32_t nT = (uint32_t)std::min<size_t>(stormTowers.size(), kStormMaxTowers);
    stormStats = {};
    stormStats.dropped = (int)(stormTowers.size() - nT);

    // The reach of a tower (cv2ColumnSigma): its widest head (flare), blown downwind, the lean over the full span,
    // the anvil hang's footprint and the storm cumulus round it, padded 6% for the sampled bin bounds below.
    const double Rb = std::clamp((double)cv2CbRadiusKm, 1.0, 20.0) * 1000.0;
    const double flare = std::max((double)std::clamp(cv2CbFlare, 0.5f, 3.0f), 1.0);
    const double drift = std::clamp((double)cv2CbHeadDriftKm, 0.0, 30.0) * 1000.0;
    const double span = std::max((double)(cv2Types[4].alt.y - cv2Types[4].alt.x), 0.0);
    const double cuReach = std::clamp((double)cv2CbCumulusReachKm, 0.0, 60.0) * 1000.0;

    auto *head = static_cast<uint32_t *>(stormMapped);
    auto *dst = reinterpret_cast<GpuStormTower *>(static_cast<char *>(stormMapped) + kStormTowersOffset);
    std::fill(stormTable.begin(), stormTable.end(), glm::uvec2(0u));
    stormEntries.clear();   // (key, tower) pairs, towers strongest first
    const uint32_t nB = kStormBinsPerFace;
    const double binA = 1.5707963267948966 / nB;
    for (uint32_t i = 0; i < nT; ++i)
    {
        const wx::Tower &tw = stormTowers[i];
        const glm::dvec3 d = glm::normalize(tw.dir);
        const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), d) + glm::dvec3(1e-12, 0, 0));
        const glm::dvec3 north = glm::cross(d, east);
        const glm::dvec3 sh(tw.shear);
        const double bearing = std::atan2(glm::dot(sh, north), glm::dot(sh, east));
        const double Rc = Rb * (0.7 + 0.6 * tw.size) * (0.75 + 0.25 * tw.role);
        // The anvil grows with the storm (2026-10-22): a young head is narrow and stands over its tower; over the
        // ~45 minutes after the tower has grown it spreads and is blown downwind, an organised storm's (multicell,
        // supercell) furthest. The flared head IS the anvil (cv2ColumnSigma), so this is its size.
        const double mature = glm::smoothstep(0.0, 2700.0, (double)tw.age - (double)tw.growS);
        const double kindK = tw.kind == wx::StormKind::Supercell ? 1.35 : tw.kind == wx::StormKind::Multicell ? 1.1 : 0.85;
        const double hs = (double)(stormHash(tw.seed ^ 0xA5A5A5A5u) & 0xFFFFu) / 65535.0;   // this storm's own spread
        const double anvilK = std::max(1.0 / flare, (0.65 + (0.55 + 0.5 * hs) * mature) * kindK);
        const double driftK = (0.25 + (0.9 + 0.8 * hs) * mature) * kindK;
        const double reach = 1.06 * std::max(Rc * 1.3 * flare * anvilK + drift * driftK + Rc * 0.6 + 0.25 * (double)cv2Lean * span,
                                             1.1 * Rc + cuReach) + 1000.0;
        GpuStormTower g;
        g.a = glm::vec4(glm::vec3(d), tw.strength);
        g.b = glm::vec4(tw.role, tw.size, tw.overshoot, (float)bearing);
        uint32_t seedBits = tw.seed;
        float seedF;
        std::memcpy(&seedF, &seedBits, 4);
        g.c = glm::vec4((float)reach, seedF, (float)anvilK, (float)driftK);
        dst[i] = g;

        // The bins: on every face the reach's rim touches, the rectangle of bins spanned by 16 points on the rim and
        // the centre (equal-angle bins: the rectangle bounds the disc's image).
        const double ang = reach / kReW;
        int faces[17];
        int nf = 0;
        glm::dvec3 rim[17];
        rim[0] = d;
        for (int k = 0; k < 16; ++k)
        {
            const double b = k * (glm::two_pi<double>() / 16.0);
            rim[k + 1] = glm::normalize(d * std::cos(ang) + (east * std::cos(b) + north * std::sin(b)) * std::sin(ang));
        }
        for (int k = 0; k < 17; ++k)
        {
            int f;
            stormBinKey(rim[k], nB, &f);
            bool seen = false;
            for (int q = 0; q < nf; ++q) seen |= faces[q] == f;
            if (!seen) faces[nf++] = f;
        }
        for (int q = 0; q < nf; ++q)
        {
            double lo[2] = {1e9, 1e9}, hi[2] = {-1e9, -1e9};
            for (int k = 0; k < 17; ++k)
            {
                double ax, ay;
                if (!faceAngles(rim[k], faces[q], ax, ay)) continue;
                lo[0] = std::min(lo[0], ax); hi[0] = std::max(hi[0], ax);
                lo[1] = std::min(lo[1], ay); hi[1] = std::max(hi[1], ay);
            }
            if (lo[0] > hi[0]) continue;
            auto idx = [&](double a) {
                return (int)std::clamp(std::floor((a + 0.7853981633974483) / binA), 0.0, (double)(nB - 1));
            };
            const int i0 = idx(lo[0]), i1 = idx(hi[0]), j0 = idx(lo[1]), j1 = idx(hi[1]);
            for (int jj = j0; jj <= j1; ++jj)
                for (int ii = i0; ii <= i1; ++ii)
                    stormEntries.push_back({1u + (uint32_t)faces[q] * nB * nB + (uint32_t)jj * nB + (uint32_t)ii, i});
        }
    }

    // Counting pass into the open-addressed table, then each bin's start, then the fill (tower order kept: strongest
    // first, a bin past kStormMaxPerBin keeps its strongest).
    auto slotOf = [&](uint32_t k) -> uint32_t {
        uint32_t h = stormHash(k) & (kStormTableSize - 1u);
        for (uint32_t p = 0; p < 32u; ++p)
        {
            if (stormTable[h].x == k || stormTable[h].x == 0u) return h;
            h = (h + 1u) & (kStormTableSize - 1u);
        }
        return 0xFFFFFFFFu;
    };
    uint32_t bins = 0, entries = 0, truncated = 0, lost = 0;
    stormCounts.assign(kStormTableSize, 0u);
    for (const StormEntry &e : stormEntries)
    {
        if (bins * 2u >= kStormTableSize) { ++lost; continue; }   // load factor 0.5: the probes stay short
        const uint32_t s = slotOf(e.key);
        if (s == 0xFFFFFFFFu) { ++lost; continue; }
        if (stormTable[s].x == 0u) { stormTable[s].x = e.key; ++bins; }
        ++stormCounts[s];
    }
    uint32_t start = 0;
    for (uint32_t s = 0; s < kStormTableSize; ++s)
    {
        if (stormTable[s].x == 0u) continue;
        const uint32_t c = std::min(stormCounts[s], kStormMaxPerBin);
        truncated += stormCounts[s] - c;
        if (start + c > kStormListSize) { stormTable[s] = glm::uvec2(0u); lost += c; continue; }
        stormTable[s].y = (start << 4u) | 0u;   // count filled below
        start += c;
    }
    auto *list = reinterpret_cast<uint32_t *>(static_cast<char *>(stormMapped) + kStormListOffset);
    for (const StormEntry &e : stormEntries)
    {
        const uint32_t s = slotOf(e.key);
        if (s == 0xFFFFFFFFu || stormTable[s].x != e.key) continue;
        const uint32_t c = stormTable[s].y & 15u;
        if (c >= kStormMaxPerBin) continue;
        list[(stormTable[s].y >> 4u) + c] = e.tower;
        stormTable[s].y += 1u;
        ++entries;
    }
    std::memcpy(static_cast<char *>(stormMapped) + kStormTableOffset, stormTable.data(), sizeof(glm::uvec2) * kStormTableSize);
    head[0] = nT;
    head[1] = bins;
    head[2] = start;
    head[3] = (uint32_t)cv2Frame;

    stormStats.towers = (int)nT;
    stormStats.bins = (int)bins;
    stormStats.entries = (int)entries;
    stormStats.truncated = (int)truncated;
    stormStats.lost = (int)lost;
    stormStats.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

nlohmann::json SatelliteSim::stormNearestJson(int n, float minStrength, float rangeKm, bool dominantOnly) const
{
    nlohmann::json out = nlohmann::json::array();
    const double ph = cloudDriftPhase();
    const double c = std::cos(ph), s = std::sin(ph);
    const glm::dvec3 oe = glm::normalize(glm::dvec3(obsDir));
    const glm::dvec3 oEast = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), oe) + glm::dvec3(1e-12, 0, 0));
    const glm::dvec3 oNorth = glm::cross(oe, oEast);
    struct Row { double dist; size_t i; glm::dvec3 e; };
    std::vector<Row> rows;
    for (size_t i = 0; i < stormTowers.size(); ++i)
    {
        const wx::Tower &tw = stormTowers[i];
        if (tw.strength < minStrength || (dominantOnly && tw.role < 1.0f)) continue;
        const glm::dvec3 d = glm::normalize(tw.dir);
        const glm::dvec3 e(d.x * c + d.y * s, -d.x * s + d.y * c, d.z);   // map frame -> Earth-fixed
        const double dist = std::acos(std::clamp(glm::dot(e, oe), -1.0, 1.0)) * kReW;
        if (dist > rangeKm * 1000.0) continue;
        rows.push_back({dist, i, e});
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.dist < b.dist; });
    for (size_t k = 0; k < rows.size() && (int)k < n; ++k)
    {
        const wx::Tower &tw = stormTowers[rows[k].i];
        const glm::dvec3 &e = rows[k].e;
        const glm::dvec3 to = e - oe * glm::dot(e, oe);
        const glm::dvec3 d = glm::normalize(tw.dir);
        const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), d) + glm::dvec3(1e-12, 0, 0));
        const glm::dvec3 north = glm::cross(d, east);
        const glm::dvec3 sh(tw.shear);
        nlohmann::json r;
        r["lat_deg"] = glm::degrees(std::asin(std::clamp(e.z, -1.0, 1.0)));
        r["lon_deg"] = glm::degrees(std::atan2(e.y, e.x));
        r["dist_km"] = rows[k].dist / 1000.0;
        r["bearing_deg"] = glm::degrees(std::atan2(glm::dot(to, oEast), glm::dot(to, oNorth)));
        r["strength"] = tw.strength;
        r["role"] = tw.role;
        r["size"] = tw.size;
        r["shear_bearing_deg"] = glm::degrees(std::atan2(glm::dot(sh, east), glm::dot(sh, north)));   // from north
        r["storm_id"] = tw.stormId;
        r["tower"] = tw.towerIndex;
        if (stormCatalog)
            if (const wx::Storm *st = stormCatalog->find(tw.stormId))
            {
                const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
                r["kind"] = st->kind == wx::StormKind::Supercell ? "supercell" : st->kind == wx::StormKind::Multicell ? "multicell" : "airmass";
                r["age_min"] = (t - st->t0) / 60.0;
                r["life_min"] = st->life / 60.0;
                r["moisture"] = st->moisture;
            }
        out.push_back(r);
    }
    return out;
}
