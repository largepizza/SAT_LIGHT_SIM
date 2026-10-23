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
// = cv2_storms.glsl cv2StormEnvRadius: how far a tower's storm cloud field reaches (the weather bake's stormStamp).
float stormEnvRadius(float reach)
{
    return reach + std::max(40000.0f, 0.8f * reach);
}

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

struct BinEntry { uint32_t key, tower; };   // = SatelliteSim::StormEntry

// Every bin within `radius` of d: on every face the rim touches, the rectangle of bins spanned by 16 points on the
// rim and the centre (equal-angle bins: the rectangle bounds the disc's image).
template <class E>
void listStormBins(const glm::dvec3 &d, double radius, uint32_t tower, uint32_t nB, std::vector<E> &out)
{
    const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), d) + glm::dvec3(1e-12, 0, 0));
    const glm::dvec3 north = glm::cross(d, east);
    const double binA = 1.5707963267948966 / nB;
    const double ang = radius / kReW;
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
                out.push_back({1u + (uint32_t)faces[q] * nB * nB + (uint32_t)jj * nB + (uint32_t)ii, tower});
    }
}

// Counting pass into the open-addressed table, then each bin's start, then the fill (tower order kept: strongest
// first, a bin past maxPerBin keeps its strongest). Load factor <= 0.5: the GPU's probes stay short.
struct StormIndexStats { uint32_t bins = 0, entries = 0, truncated = 0, lost = 0, used = 0; };
template <class E>
StormIndexStats fillStormIndex(const std::vector<E> &ents, std::vector<glm::uvec2> &table, std::vector<uint32_t> &counts,
                               uint32_t tableSize, uint32_t listSize, uint32_t maxPerBin, uint32_t *list)
{
    StormIndexStats st;
    table.assign(tableSize, glm::uvec2(0u));
    auto slotOf = [&](uint32_t k) -> uint32_t {
        uint32_t h = stormHash(k) & (tableSize - 1u);
        for (uint32_t p = 0; p < 32u; ++p)
        {
            if (table[h].x == k || table[h].x == 0u) return h;
            h = (h + 1u) & (tableSize - 1u);
        }
        return 0xFFFFFFFFu;
    };
    counts.assign(tableSize, 0u);
    for (const E &e : ents)
    {
        if (st.bins * 2u >= tableSize) { ++st.lost; continue; }
        const uint32_t sl = slotOf(e.key);
        if (sl == 0xFFFFFFFFu) { ++st.lost; continue; }
        if (table[sl].x == 0u) { table[sl].x = e.key; ++st.bins; }
        ++counts[sl];
    }
    uint32_t start = 0;
    for (uint32_t sl = 0; sl < tableSize; ++sl)
    {
        if (table[sl].x == 0u) continue;
        const uint32_t c = std::min(counts[sl], maxPerBin);
        st.truncated += counts[sl] - c;
        if (start + c > listSize) { table[sl] = glm::uvec2(0u); st.lost += c; continue; }
        table[sl].y = (start << 4u) | 0u;   // count filled below
        start += c;
    }
    for (const E &e : ents)
    {
        const uint32_t sl = slotOf(e.key);
        if (sl == 0xFFFFFFFFu || table[sl].x != e.key) continue;
        const uint32_t c = table[sl].y & 15u;
        if (c >= maxPerBin) continue;
        list[(table[sl].y >> 4u) + c] = e.tower;
        table[sl].y += 1u;
        ++st.entries;
    }
    st.used = start;
    return st;
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
    stormEnvTable.assign(kStormEnvTableSize, glm::uvec2(0u));
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
                     (double)cv2CoverFull, (double)stormFrequency, (double)cv2CbRadiusKm, (double)stormLifeScale,
                     (double)stormComplexes})
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
        s.lineFrequency = std::max(stormComplexes, 0.0f);
        stormCatalog->configure(env, s, stormClim.get(), key);
        stormEnvKey = key;
        stormUploadedT = -1e300;
    }
    if (t == stormUploadedT)
        return;   // paused: nothing moved
    // The weather cube carries each storm's own cloud (cloud_v2_weather.comp stormStamp) and re-bakes a face at a time
    // as time runs; after a jump (or the first upload: the load-time bake had no towers) every face is stale at once.
    if (std::fabs(t - stormPrevUploadT) > 900.0)
        for (double &b : cv2WxBakedT)
            b = -1e30;
    stormPrevUploadT = t;
    stormUploadedT = t;
    std::vector<const wx::Storm *> stormsNow;
    stormCatalog->stormsAt(t, stormsNow);
    stormCatalog->towersAt(t, stormsNow, stormTowers);

    // Strongest first: past the buffer's capacity the weakest go, and a crowded bin keeps its strongest.
    std::sort(stormTowers.begin(), stormTowers.end(), [](const wx::Tower &a, const wx::Tower &b) {
        return a.strength != b.strength ? a.strength > b.strength
                                        : (a.stormId != b.stormId ? a.stormId < b.stormId : a.towerIndex < b.towerIndex);
    });
    const uint32_t nT = (uint32_t)std::min<size_t>(stormTowers.size(), kStormMaxTowers);
    {
        const StormStats prev = stormStats;
        stormStats = {};
        stormStats.envBins = prev.envBins;
        stormStats.envEntries = prev.envEntries;
        stormStats.envTruncated = prev.envTruncated;
        stormStats.envLost = prev.envLost;
        stormStats.lineSystems = stormCatalog->stats().lineSystemsAlive;
    }
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
    stormEntries.clear();      // (key, tower) pairs, towers strongest first
    stormGpuTowers.resize(nT);
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
        const double kindK = tw.kind == wx::StormKind::Supercell ? 1.35
                           : (tw.kind == wx::StormKind::Multicell || wx::isLineSystem(tw.kind)) ? 1.1 : 0.85;
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
        stormGpuTowers[i] = g;   // the envelope index's copy is made from these (buildStormEnvIndex)
        listStormBins(d, reach, i, kStormBinsPerFace, stormEntries);
    }
    auto *list = reinterpret_cast<uint32_t *>(static_cast<char *>(stormMapped) + kStormListOffset);
    const StormIndexStats ix = fillStormIndex(stormEntries, stormTable, stormCounts, kStormTableSize, kStormListSize,
                                              kStormMaxPerBin, list);
    std::memcpy(static_cast<char *>(stormMapped) + kStormTableOffset, stormTable.data(), sizeof(glm::uvec2) * kStormTableSize);
    const uint32_t bins = ix.bins, entries = ix.entries, truncated = ix.truncated, lost = ix.lost, start = ix.used;
    head[0] = nT;
    head[1] = bins;
    head[2] = start;
    head[3] = (uint32_t)cv2Frame;

    // The anvil shields (2026-10-23): one per storm, alive or lingering. Listed in every coarse bin their capsule
    // touches (the disc round the spine's middle that holds it).
    {
        wx::ShieldParams sp;
        sp.headRadiusM = (float)(Rb * flare * 1.15);
        sp.headDriftM = (float)drift;
        stormCatalog->shieldsAt(t, stormsNow, sp, stormShields);
        std::sort(stormShields.begin(), stormShields.end(), [](const wx::Shield &a, const wx::Shield &b) {
            return a.strength != b.strength ? a.strength > b.strength : a.stormId < b.stormId;
        });
        const uint32_t nS = (uint32_t)std::min<size_t>(stormShields.size(), kShieldMax);
        auto *sdst = reinterpret_cast<GpuStormShield *>(static_cast<char *>(stormMapped) + kShieldsOffset);
        stormGpuShields.resize(nS);
        shieldEntries.clear();
        for (uint32_t i = 0; i < nS; ++i)
        {
            const wx::Shield &s = stormShields[i];
            GpuStormShield g;
            g.a = glm::vec4(glm::vec3(s.a), s.strength);
            g.b = glm::vec4(glm::vec3(s.b), s.rB);
            g.c = glm::vec4(s.rA, s.thick, s.age01, s.rain);
            // The convective line's distance ahead of the spine (line systems: the spine sits 45% of the trail behind
            // it; rA = 0.55 trail + the tower radius).
            const float front = s.lineK > 0.0f ? 0.45f / 0.55f * std::max(s.rA - (float)Rb, 0.0f) : 0.0f;
            g.d = glm::vec4(s.rear, front);
            sdst[i] = g;
            stormGpuShields[i] = g;
            const glm::dvec3 mid = glm::normalize(s.a + s.b);
            const double half = 0.5 * std::acos(std::clamp(glm::dot(s.a, s.b), -1.0, 1.0)) * kReW;
            // (The shader frays the outline out to 1.6 radii: cv2ShieldAt.)
            listStormBins(mid, 1.03 * (half + 1.6 * std::max(s.rA, s.rB)) + 2000.0, i, kShieldBinsPerFace, shieldEntries);
        }
        auto *slist = reinterpret_cast<uint32_t *>(static_cast<char *>(stormMapped) + kShieldListOffset);
        const StormIndexStats sx = fillStormIndex(shieldEntries, shieldTable, stormCounts, kShieldTableSize,
                                                  kShieldListSize, kShieldMaxPerBin, slist);
        std::memcpy(static_cast<char *>(stormMapped) + kShieldTableOffset, shieldTable.data(),
                    sizeof(glm::uvec2) * kShieldTableSize);
        auto *shead = reinterpret_cast<uint32_t *>(static_cast<char *>(stormMapped) + kShieldHeadOffset);
        shead[0] = nS;
        shead[1] = stormObjects ? 1u : 0u;
        shead[2] = sx.bins;
        shead[3] = sx.used;
        stormStats.shields = (int)nS;
        stormStats.shieldBins = (int)sx.bins;
        stormStats.shieldEntries = (int)sx.entries;
        stormStats.shieldTruncated = (int)sx.truncated;
    }

    stormStats.towers = (int)nT;
    stormStats.bins = (int)bins;
    stormStats.entries = (int)entries;
    stormStats.truncated = (int)truncated;
    stormStats.lost = (int)lost;
    stormStats.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// The ENVELOPE index for the weather bake's storm cloud field (cloud_v2_weather.comp stormStamp): every tower stronger
// than 0.05 listed out to cv2StormEnvRadius round where the low cloud reads the weather cube for it (the flowed map,
// clouds_v2.glsl cv2FlowDisp: the same constants, in double), with its own copy of the towers (a.xyz = that flowed
// axis), so the march's per-frame order cannot make its indices stale. Built only on a frame the bake runs (at most
// every ~45 s of sim time at 1x): ~1-2 ms.
void SatelliteSim::buildStormEnvIndex()
{
    FPROF_SCOPE("buildStormEnvIndex");
    if (!stormMapped)
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const double flowP = std::clamp((double)cv2FlowPeriodKm, 200.0, 20000.0) * 1000.0;
    const double flowX = std::clamp((double)cv2FlowWarp, 0.0, 0.1) * flowP;
    const double flowPh = (double)cv2WindMps * 0.3 * t / flowP;
    auto flowed = [&](const glm::dvec3 &wd) -> glm::dvec3 {
        if (flowX <= 0.0) return wd;
        static const double kd[8][3] = {{0.62, 0.48, 0.62}, {-0.71, 0.30, 0.64}, {0.18, -0.93, 0.32}, {0.85, 0.21, -0.48},
                                        {-0.34, -0.55, -0.76}, {0.05, 0.77, -0.64}, {-0.90, -0.40, 0.17}, {0.44, -0.29, 0.85}};
        static const double ks[8] = {1.0, 1.13, 0.87, 1.31, 0.74, 1.52, 0.93, 1.21};
        static const double ph[8] = {0.3, 2.1, 4.4, 1.7, 5.6, 3.2, 0.9, 2.7};
        const double k0 = glm::two_pi<double>() * kReW / flowP;
        const double phase = (flowPh - std::floor(flowPh)) * glm::two_pi<double>();
        glm::dvec3 g(0.0);
        for (int i = 0; i < 8; ++i)
        {
            const glm::dvec3 kv = glm::normalize(glm::dvec3(kd[i][0], kd[i][1], kd[i][2])) * (k0 * ks[i]);
            g += kv * std::cos(glm::dot(kv, wd) + ph[i] + phase) / ks[i];
        }
        const glm::dvec3 v = glm::cross(wd, g) / (k0 * 2.0) * flowX;
        return glm::normalize(wd + v / kReW);
    };
    auto *envTowers = reinterpret_cast<GpuStormTower *>(static_cast<char *>(stormMapped) + kStormEnvTowersOffset);
    stormEnvEntries.clear();
    const uint32_t n = (uint32_t)std::min<size_t>(stormGpuTowers.size(), kStormMaxTowers);
    for (uint32_t i = 0; i < n; ++i)
    {
        GpuStormTower g = stormGpuTowers[i];
        const glm::dvec3 df = flowed(glm::normalize(glm::dvec3(glm::vec3(g.a))));
        g.a = glm::vec4(glm::vec3(df), g.a.w);
        envTowers[i] = g;
        if (g.a.w > 0.05f)
            listStormBins(df, 1.04 * (double)stormEnvRadius(g.c.x), i, kStormBinsPerFace, stormEnvEntries);
    }
    auto *envList = reinterpret_cast<uint32_t *>(static_cast<char *>(stormMapped) + kStormEnvListOffset);
    const StormIndexStats ex = fillStormIndex(stormEnvEntries, stormEnvTable, stormCounts, kStormEnvTableSize,
                                              kStormEnvListSize, kStormMaxPerBin, envList);
    std::memcpy(static_cast<char *>(stormMapped) + kStormEnvTableOffset, stormEnvTable.data(),
                sizeof(glm::uvec2) * kStormEnvTableSize);
    stormStats.envBins = (int)ex.bins;
    stormStats.envEntries = (int)ex.entries;
    stormStats.envTruncated = (int)ex.truncated;
    stormStats.envLost = (int)ex.lost;

    // The shields' flowed copy (the line systems' stratiform rain is stamped where the low cloud reads it).
    auto *envShields = reinterpret_cast<GpuStormShield *>(static_cast<char *>(stormMapped) + kShieldEnvOffset);
    shieldEnvEntries.clear();
    const uint32_t nS = (uint32_t)std::min<size_t>(stormGpuShields.size(), kShieldMax);
    for (uint32_t i = 0; i < nS; ++i)
    {
        GpuStormShield g = stormGpuShields[i];
        const glm::dvec3 a = flowed(glm::normalize(glm::dvec3(glm::vec3(g.a))));
        const glm::dvec3 b = flowed(glm::normalize(glm::dvec3(glm::vec3(g.b))));
        g.a = glm::vec4(glm::vec3(a), g.a.w);
        g.b = glm::vec4(glm::vec3(b), g.b.w);
        envShields[i] = g;
        if (g.c.w <= 0.0f) continue;   // only the raining (line-system) shields are stamped
        const glm::dvec3 mid = glm::normalize(a + b);
        const double half = 0.5 * std::acos(std::clamp(glm::dot(a, b), -1.0, 1.0)) * kReW;
        // + the clear air ahead of the line (100 km past the capsule, stormStamp).
        listStormBins(mid, 1.03 * (half + std::max(g.c.x, g.b.w)) + 100000.0, i, kShieldBinsPerFace, shieldEnvEntries);
    }
    auto *senv = reinterpret_cast<uint32_t *>(static_cast<char *>(stormMapped) + kShieldEnvListOffset);
    fillStormIndex(shieldEnvEntries, shieldEnvTable, stormCounts, kShieldTableSize, kShieldListSize, kShieldMaxPerBin,
                   senv);
    std::memcpy(static_cast<char *>(stormMapped) + kShieldEnvTableOffset, shieldEnvTable.data(),
                sizeof(glm::uvec2) * kShieldTableSize);
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
        {   // The Sun's elevation there (harness `storms go day=1`).
            const double tt = (double)simDayJ2000 * 86400.0 + simSecInDay;
            const glm::dvec3 si = sunDirEciAt(tt);
            const double g = earthRotationAngle(tt);
            const glm::dvec3 se = glm::normalize(glm::dvec3(si.x * std::cos(g) + si.y * std::sin(g),
                                                            -si.x * std::sin(g) + si.y * std::cos(g), si.z));
            r["sun_el_deg"] = glm::degrees(std::asin(std::clamp(glm::dot(se, e), -1.0, 1.0)));
        }
        r["tower"] = tw.towerIndex;
        if (stormCatalog)
            if (const wx::Storm *st = stormCatalog->find(tw.stormId))
            {
                const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
                r["kind"] = wx::stormKindName(st->kind);
                r["age_min"] = (t - st->t0) / 60.0;
                r["life_min"] = st->life / 60.0;
                r["moisture"] = st->moisture;
            }
        out.push_back(r);
    }
    return out;
}
