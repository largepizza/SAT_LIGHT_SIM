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
#include <bit>
#include <cstdlib>
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
    // Fast path: a small disc well inside its centre's face. A face's equal-angle coordinate ax = atan(u / mj) changes at
    // 1 / sqrt(mj^2 + u^2) per unit of arc (at most sqrt(2) on a face), so with that rate at the centre (+5%, the rate
    // varies little over < 0.05 rad) the rectangle about the centre's coordinates bounds the disc as the rim's sampled
    // one does (4 atans instead of ~68).
    {
        int f0;
        stormBinKey(d, nB, &f0);
        double cx, cy;
        if (ang < 0.05 && faceAngles(d, f0, cx, cy))
        {
            const int fc = f0 >> 1;
            const double mj = std::abs((fc == 0) ? d.x : ((fc == 1) ? d.y : d.z));
            const double ux = (fc == 0) ? d.y : d.x, uy = (fc == 2) ? d.y : d.z;
            const double dAx = 1.05 * ang / std::sqrt(mj * mj + ux * ux) + 1e-5;
            const double dAy = 1.05 * ang / std::sqrt(mj * mj + uy * uy) + 1e-5;
            if (std::abs(cx) + dAx >= 0.785 || std::abs(cy) + dAy >= 0.785) goto rim;
            auto idx = [&](double a) {
                return (int)std::clamp(std::floor((a + 0.7853981633974483) / binA), 0.0, (double)(nB - 1));
            };
            const int i0 = idx(cx - dAx), i1 = idx(cx + dAx), j0 = idx(cy - dAy), j1 = idx(cy + dAy);
            for (int jj = j0; jj <= j1; ++jj)
                for (int ii = i0; ii <= i1; ++ii)
                    out.push_back({1u + (uint32_t)f0 * nB * nB + (uint32_t)jj * nB + (uint32_t)ii, tower});
            return;
        }
    }
rim:
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
// The list is built in host memory and copied over in one sequential run, the table likewise: both live in
// device-local, host-visible (write-combined) memory, where every scattered 4-byte store is its own bus write (the
// entries' scattered stores were most of the index's cost). `used` lists the slots the last call filled: only those
// are cleared, and the start pass walks the occupied slots in slot order. The result is the same as a full
// build: the slots are taken in the same probe order from an all-empty table, and the list starts handed out in slot
// order (an occupancy bitmap, not a sort). `counts` is all zero between calls.
struct StormIndexStats { uint32_t bins = 0, entries = 0, truncated = 0, lost = 0, used = 0; };
template <class E>
StormIndexStats fillStormIndex(const std::vector<E> &ents, std::vector<glm::uvec2> &table, std::vector<uint32_t> &used,
                               std::vector<uint32_t> &counts, uint32_t tableSize, uint32_t listSize, uint32_t maxPerBin,
                               uint32_t *list, glm::uvec2 *gpuTable)
{
    StormIndexStats st;
    static std::vector<uint32_t> old, hostList;   // main thread only
    if (table.size() != tableSize)
    {
        table.assign(tableSize, glm::uvec2(0u));
        used.clear();
    }
    if (counts.size() < tableSize) counts.resize(tableSize, 0u);
    static std::vector<uint64_t> occ;   // occupied slots; all zero between calls
    const uint32_t nWords = (tableSize + 63u) / 64u;
    if (occ.size() < nWords) occ.resize(nWords, 0ull);
    old.swap(used);
    used.clear();
    for (uint32_t sl : old) table[sl] = glm::uvec2(0u);
    auto slotOf = [&](uint32_t k) -> uint32_t {
        uint32_t h = stormHash(k) & (tableSize - 1u);
        for (uint32_t p = 0; p < 32u; ++p)
        {
            if (table[h].x == k || table[h].x == 0u) return h;
            h = (h + 1u) & (tableSize - 1u);
        }
        return 0xFFFFFFFFu;
    };
    for (const E &e : ents)
    {
        if (st.bins * 2u >= tableSize) { ++st.lost; continue; }
        const uint32_t sl = slotOf(e.key);
        if (sl == 0xFFFFFFFFu) { ++st.lost; continue; }
        if (table[sl].x == 0u) { table[sl].x = e.key; ++st.bins; used.push_back(sl); occ[sl >> 6] |= 1ull << (sl & 63u); }
        ++counts[sl];
    }
    // The occupied slots in slot order from the bitmap (std::sort of ~10-40k slots was 0.4-1.3 ms a call).
    used.clear();
    for (uint32_t w = 0; w < nWords; ++w)
        for (uint64_t b = occ[w]; b; b &= b - 1ull)
            used.push_back(w * 64u + (uint32_t)std::countr_zero(b));
    std::fill(occ.begin(), occ.begin() + nWords, 0ull);
    uint32_t start = 0;
    for (uint32_t sl : used)
    {
        const uint32_t c = std::min(counts[sl], maxPerBin);
        st.truncated += counts[sl] - c;
        if (start + c > listSize) { table[sl] = glm::uvec2(0u); st.lost += c; continue; }
        table[sl].y = (start << 5u) | 0u;   // count filled below (5 bits: cv2_storms.glsl's bins decode y >> 5, y & 31)
        start += c;
    }
    if (hostList.size() < start) hostList.resize(start);
    for (const E &e : ents)
    {
        const uint32_t sl = slotOf(e.key);
        if (sl == 0xFFFFFFFFu || table[sl].x != e.key) continue;
        const uint32_t c = table[sl].y & 31u;
        if (c >= maxPerBin) continue;
        hostList[(table[sl].y >> 5u) + c] = e.tower;
        table[sl].y += 1u;
        ++st.entries;
    }
    for (uint32_t sl : used) counts[sl] = 0u;
    std::memcpy(list, hostList.data(), sizeof(uint32_t) * start);
    std::memcpy(gpuTable, table.data(), sizeof(glm::uvec2) * tableSize);
    st.used = start;
    return st;
}

// body(i, list) for i in [0, n) on the storm catalog's thread pool, in chunks that each append to their own list, the
// lists then joined in chunk order: `out` ends up exactly as a serial loop appending in i order would leave it (the
// envelope index's flowed copies). Main thread only.
// Below minN it runs serially (waking the pool costs more than ~1000 cheap iterations: the per-frame tower and shield
// lists stay serial, the envelope index's flowed copies go parallel).
template <class E, class F>
void parallelEntries(wx::Catalog *cat, uint32_t n, uint32_t minN, std::vector<E> &out, F &&body)
{
    constexpr uint32_t kChunk = 96;
    const uint32_t nc = (n + kChunk - 1) / kChunk;
    if (!cat || nc <= 1 || n < minN)
    {
        for (uint32_t i = 0; i < n; ++i) body(i, out);
        return;
    }
    static std::vector<std::vector<E>> parts;   // kept for their capacity (one instance per entry type)
    if (parts.size() < nc) parts.resize(nc);
    cat->parallelFor(nc, [&](size_t c) {
        std::vector<E> &pc = parts[c];
        pc.clear();
        const uint32_t i1 = std::min<uint32_t>(n, (uint32_t)(c + 1) * kChunk);
        for (uint32_t i = (uint32_t)c * kChunk; i < i1; ++i) body(i, pc);
    });
    size_t tot = out.size();
    for (uint32_t c = 0; c < nc; ++c) tot += parts[c].size();
    out.reserve(tot);
    for (uint32_t c = 0; c < nc; ++c) out.insert(out.end(), parts[c].begin(), parts[c].end());
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
                     (double)stormComplexes, (double)stormCyclones})
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
        // A persistent pool (the bins after a jump, the per-frame tower index): three quarters of the threads, at most 12.
        // (SATLIGHTSIM_STORM_THREADS overrides it: an A/B of the build cost.)
        int nThreads = std::clamp((int)std::thread::hardware_concurrency() * 3 / 4, 1, 12);
        if (const char *e = std::getenv("SATLIGHTSIM_STORM_THREADS")) nThreads = std::clamp(std::atoi(e), 1, 64);
        stormCatalog->setBuildThreads(nThreads);
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
        env.landAt = [this](double lat, double lon) -> float {   // = env.land at (asin(z), atan2(y, x))
            if (oceanMaskCpu.empty()) return 0.3f;
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
        s.cycloneFrequency = stormObjects ? std::max(stormCyclones, 0.0f) : 0.0f;
        stormCatalog->configure(env, s, stormClim.get(), key);
        stormEnvKey = key;
        stormUploadedT = -1e300;
    }
    // Time running faster than ~2 h a frame (1 week/s and up): each frame is a new set of storms (dozens of new spawn
    // bins a frame, 17-45 ms of this function in hitch_check's ladder), random from frame to frame at that pace. Hidden
    // while it lasts; a single jump (one big step) still computes them.
    {
        const double step = std::fabs(t - stormLastFrameT);
        stormLastFrameT = t;
        stormFastFrames = step > 7200.0 ? stormFastFrames + 1 : 0;
        if (stormFastFrames >= 2)
        {
            static_cast<uint32_t *>(stormMapped)[0] = 0u;
            reinterpret_cast<uint32_t *>(static_cast<char *>(stormMapped) + kShieldHeadOffset)[0] = 0u;
            reinterpret_cast<uint32_t *>(static_cast<char *>(stormMapped) + kCycHeadOffset)[0] = 0u;
            stormTowers.clear();
            stormShields.clear();
            stormCyclonesNow.clear();
            stormGpuTowers.clear();
            stormGpuShields.clear();
            stormStats.towers = stormStats.shields = stormStats.cyclones = 0;
            stormUploadedT = -1e300;
            return;
        }
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
    // Sections of this function (local splits: fprof::mark would move recordCompute's own rc.seg splits).
    auto splitT = std::chrono::steady_clock::now();
    auto split = [&splitT](const char *name) {
        const auto now = std::chrono::steady_clock::now();
        fprof::add(name, std::chrono::duration<float, std::milli>(now - splitT).count());
        splitT = now;
    };
    std::vector<const wx::Storm *> stormsNow;
    stormCatalog->stormsAt(t, stormsNow);
    stormCatalog->towersAt(t, stormsNow, stormTowers);
    // Tropical cyclones: their eyewall and inner-band towers join the list (Earth-fixed tracks, in the map frame now).
    stormCatalog->cyclonesAt(t, stormCyclonesNow);
    stormCatalog->cycloneTowersAt(t, stormCyclonesNow, stormTowers);
    const auto tCat = std::chrono::steady_clock::now();
    split("storms.catalog");

    // Strongest first: past the buffer's capacity the weakest go, and a crowded bin keeps its strongest.
    std::sort(stormTowers.begin(), stormTowers.end(), [](const wx::Tower &a, const wx::Tower &b) {
        return a.strength != b.strength ? a.strength > b.strength
                                        : (a.stormId != b.stormId ? a.stormId < b.stormId : a.towerIndex < b.towerIndex);
    });
    const uint32_t nT = (uint32_t)std::min<size_t>(stormTowers.size(), kStormMaxTowers);
    split("storms.sort");
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
    parallelEntries(stormCatalog.get(), nT, 4096u, stormEntries, [&](uint32_t i, std::vector<StormEntry> &ents) {
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
                           : (tw.kind == wx::StormKind::Multicell || wx::isLineSystem(tw.kind)) ? 1.1
                           : tw.kind == wx::StormKind::Tropical ? 1.0 : 0.85;
        const double hs = (double)(stormHash(tw.seed ^ 0xA5A5A5A5u) & 0xFFFFu) / 65535.0;   // this storm's own spread
        double anvilK = std::max(1.0 / flare, (0.65 + (0.55 + 0.5 * hs) * mature) * kindK);
        double driftK = (0.25 + (0.9 + 0.8 * hs) * mature) * kindK;
        // A cyclone's towers do not flare: its anvil is the central dense overcast (a shield, cycloneShieldsAt). With
        // flared ~40-km heads every eyewall tower reached across the whole eye and eyewall (reach ~60 km), so every
        // sample there walked a full bin of towers: 16-19 ms of the cloud march from 12 km in the eye at Medium.
        // Measured (Medium, Rosa 2036-09-10, cloud bucket): 12 km in the eye 15.2 -> 12.5 ms, sea level 8.0 -> 6.5.
        if (tw.kind == wx::StormKind::Tropical) { anvilK = 1.0 / flare; driftK = 0.25; }
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
        listStormBins(d, reach, i, kStormBinsPerFace, ents);
    });
    split("storms.towerBins");
    auto *list = reinterpret_cast<uint32_t *>(static_cast<char *>(stormMapped) + kStormListOffset);
    const StormIndexStats ix = fillStormIndex(stormEntries, stormTable, stormTableUsed, stormCounts, kStormTableSize,
                                              kStormListSize, kStormMaxPerBin, list,
                                              reinterpret_cast<glm::uvec2 *>(static_cast<char *>(stormMapped) + kStormTableOffset));
    const uint32_t bins = ix.bins, entries = ix.entries, truncated = ix.truncated, lost = ix.lost, start = ix.used;
    head[0] = nT;
    head[1] = bins;
    head[2] = start;
    head[3] = (uint32_t)cv2Frame;
    const auto tIdx = std::chrono::steady_clock::now();
    split("storms.towerIndex");

    // The anvil shields (2026-10-23): one per storm, alive or lingering. Listed in every coarse bin their capsule
    // touches (the disc round the spine's middle that holds it).
    {
        wx::ShieldParams sp;
        sp.headRadiusM = (float)(Rb * flare * 1.15);
        sp.headDriftM = (float)drift;
        stormCatalog->shieldsAt(t, stormsNow, sp, stormShields);
        stormCatalog->cycloneShieldsAt(t, stormCyclonesNow, stormShields);   // their central dense overcast
        std::sort(stormShields.begin(), stormShields.end(), [](const wx::Shield &a, const wx::Shield &b) {
            return a.strength != b.strength ? a.strength > b.strength : a.stormId < b.stormId;
        });
        const uint32_t nS = (uint32_t)std::min<size_t>(stormShields.size(), kShieldMax);
        auto *sdst = reinterpret_cast<GpuStormShield *>(static_cast<char *>(stormMapped) + kShieldsOffset);
        stormGpuShields.resize(nS);
        shieldEntries.clear();
        parallelEntries(stormCatalog.get(), nS, 4096u, shieldEntries, [&](uint32_t i, std::vector<StormEntry> &ents) {
            const wx::Shield &s = stormShields[i];
            GpuStormShield g;
            g.a = glm::vec4(glm::vec3(s.a), s.strength);
            g.b = glm::vec4(glm::vec3(s.b), s.rB);
            // (c.w: a line system's rain share + 2 x its bow in whole km: cloud_v2_weather.comp stormStamp unpacks it.)
            g.c = glm::vec4(s.rA, s.thick, s.age01,
                            s.lineK > 0.0f ? std::min(s.rain, 0.999f) + 2.0f * std::round(s.bowM / 1000.0f) : s.rain);
            // The convective line's distance ahead of the spine (line systems: the spine sits 45% of the trail behind
            // it; rA = 0.55 trail + the tower radius).
            const float front = s.lineK > 0.0f ? 0.45f / 0.55f * std::max(s.rA - (float)Rb, 0.0f) : 0.0f;
            g.d = glm::vec4(s.rear, s.kind == wx::StormKind::Tropical ? -std::max(s.eyeR, 1.0f) : front);
            sdst[i] = g;
            stormGpuShields[i] = g;
            const glm::dvec3 mid = glm::normalize(s.a + s.b);
            const double half = 0.5 * std::acos(std::clamp(glm::dot(s.a, s.b), -1.0, 1.0)) * kReW;
            // (The shader frays the outline out to 1.6 radii: cv2ShieldAt.)
            listStormBins(mid, 1.03 * (half + 1.6 * std::max(s.rA, s.rB)) + 2000.0, i, kShieldBinsPerFace, ents);
        });
        auto *slist = reinterpret_cast<uint32_t *>(static_cast<char *>(stormMapped) + kShieldListOffset);
        const StormIndexStats sx = fillStormIndex(shieldEntries, shieldTable, shieldTableUsed, stormCounts, kShieldTableSize,
                                                  kShieldListSize, kShieldMaxPerBin, slist,
                                                  reinterpret_cast<glm::uvec2 *>(static_cast<char *>(stormMapped) + kShieldTableOffset));
        auto *shead = reinterpret_cast<uint32_t *>(static_cast<char *>(stormMapped) + kShieldHeadOffset);
        shead[0] = nS;
        shead[1] = stormObjects ? 1u : 0u;
        shead[2] = sx.bins;
        shead[3] = sx.used;
        stormStats.shields = (int)nS;
        // The cyclones' records for the bake (centre where the low cloud reads the cube; the bands turn slowly).
        const uint32_t nC = (uint32_t)std::min<size_t>(stormCyclonesNow.size(), kCycloneMax);
        auto *cdst = reinterpret_cast<GpuStormCyclone *>(static_cast<char *>(stormMapped) + kCyclonesOffset);
        for (uint32_t i = 0; i < nC; ++i)
        {
            const wx::CycloneState &c = stormCyclonesNow[i];
            const glm::dvec3 f = stormFlowedDir(c.map, t);
            GpuStormCyclone g;
            g.a = glm::vec4(glm::vec3(c.map), c.intensity01);
            g.b = glm::vec4(glm::vec3(f), c.c->hemi);
            g.c = glm::vec4(c.eyeR, c.cdoR, c.outerR, (float)c.bandRot);
            g.d = glm::vec4(c.rmw, c.vmax, c.overLand ? 1.0f : 0.0f, c.arms);
            cdst[i] = g;
        }
        reinterpret_cast<uint32_t *>(static_cast<char *>(stormMapped) + kCycHeadOffset)[0] = nC;
        stormStats.cyclones = (int)nC;
        stormStats.shieldBins = (int)sx.bins;
        stormStats.shieldEntries = (int)sx.entries;
        stormStats.shieldTruncated = (int)sx.truncated;
    }

    split("storms.shields");
    stormStats.towers = (int)nT;
    stormStats.bins = (int)bins;
    stormStats.entries = (int)entries;
    stormStats.truncated = (int)truncated;
    stormStats.lost = (int)lost;
    const auto tEnd = std::chrono::steady_clock::now();
    stormStats.cpuMs = std::chrono::duration<double, std::milli>(tEnd - t0).count();
    stormStats.msCatalog = std::chrono::duration<double, std::milli>(tCat - t0).count();
    stormStats.msTowerIndex = std::chrono::duration<double, std::milli>(tIdx - tCat).count();
    stormStats.msShields = std::chrono::duration<double, std::milli>(tEnd - tIdx).count();
}

// The ENVELOPE index for the weather bake's storm cloud field (cloud_v2_weather.comp stormStamp): every tower stronger
// than 0.05 listed out to cv2StormEnvRadius round where the low cloud reads the weather cube for it (the flowed map,
// clouds_v2.glsl cv2FlowDisp: the same constants, in double), with its own copy of the towers (a.xyz = that flowed
// axis), so the march's per-frame order cannot make its indices stale. Built only on a frame the bake runs (at most
// every ~45 s of sim time at 1x): ~1-2 ms.
// Where the low cloud reads the weather cube for a map-frame direction (clouds_v2.glsl cv2FlowDisp, the same constants,
// in double): the bake stamps a storm's own cloud there.
glm::dvec3 SatelliteSim::stormFlowedDir(const glm::dvec3 &wd, double t) const
{
    const double flowP = std::clamp((double)cv2FlowPeriodKm, 200.0, 20000.0) * 1000.0;
    const double flowX = std::clamp((double)cv2FlowWarp, 0.0, 0.1) * flowP;
    const double flowPh = (double)cv2WindMps * 0.3 * t / flowP;
    if (flowX <= 0.0) return wd;
    static const double kd[8][3] = {{0.62, 0.48, 0.62}, {-0.71, 0.30, 0.64}, {0.18, -0.93, 0.32}, {0.85, 0.21, -0.48},
                                    {-0.34, -0.55, -0.76}, {0.05, 0.77, -0.64}, {-0.90, -0.40, 0.17}, {0.44, -0.29, 0.85}};
    static const double ks[8] = {1.0, 1.13, 0.87, 1.31, 0.74, 1.52, 0.93, 1.21};
    static const double ph[8] = {0.3, 2.1, 4.4, 1.7, 5.6, 3.2, 0.9, 2.7};
    // The wave directions normalised once (the same expression: bit-identical; this runs for every tower and shield
    // end on each frame the envelope index is built).
    struct KDir
    {
        glm::dvec3 v[8];
        KDir()
        {
            for (int i = 0; i < 8; ++i) v[i] = glm::normalize(glm::dvec3(kd[i][0], kd[i][1], kd[i][2]));
        }
    };
    static const KDir kDir;
    const double k0 = glm::two_pi<double>() * kReW / flowP;
    const double phase = (flowPh - std::floor(flowPh)) * glm::two_pi<double>();
    glm::dvec3 g(0.0);
    for (int i = 0; i < 8; ++i)
    {
        const glm::dvec3 kv = kDir.v[i] * (k0 * ks[i]);
        g += kv * std::cos(glm::dot(kv, wd) + ph[i] + phase) / ks[i];
    }
    const glm::dvec3 v = glm::cross(wd, g) / (k0 * 2.0) * flowX;
    return glm::normalize(wd + v / kReW);
}

void SatelliteSim::buildStormEnvIndex()
{
    FPROF_SCOPE("buildStormEnvIndex");
    if (!stormMapped)
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    auto flowed = [&](const glm::dvec3 &wd) -> glm::dvec3 { return stormFlowedDir(wd, t); };
    auto splitT = std::chrono::steady_clock::now();
    auto split = [&splitT](const char *name) {
        const auto now = std::chrono::steady_clock::now();
        fprof::add(name, std::chrono::duration<float, std::milli>(now - splitT).count());
        splitT = now;
    };
    auto *envTowers = reinterpret_cast<GpuStormTower *>(static_cast<char *>(stormMapped) + kStormEnvTowersOffset);
    stormEnvEntries.clear();
    const uint32_t n = (uint32_t)std::min<size_t>(stormGpuTowers.size(), kStormMaxTowers);
    parallelEntries(stormCatalog.get(), n, 256u, stormEnvEntries, [&](uint32_t i, std::vector<StormEntry> &ents) {
        GpuStormTower g = stormGpuTowers[i];
        const glm::dvec3 df = flowed(glm::normalize(glm::dvec3(glm::vec3(g.a))));
        g.a = glm::vec4(glm::vec3(df), g.a.w);
        envTowers[i] = g;
        if (g.a.w > 0.05f)
            listStormBins(df, 1.04 * (double)stormEnvRadius(g.c.x), i, kStormBinsPerFace, ents);
    });
    split("env.towerBins");
    auto *envList = reinterpret_cast<uint32_t *>(static_cast<char *>(stormMapped) + kStormEnvListOffset);
    const StormIndexStats ex = fillStormIndex(stormEnvEntries, stormEnvTable, stormEnvTableUsed, stormCounts,
                                              kStormEnvTableSize, kStormEnvListSize, kStormMaxPerBin, envList,
                                              reinterpret_cast<glm::uvec2 *>(static_cast<char *>(stormMapped) + kStormEnvTableOffset));
    stormStats.envBins = (int)ex.bins;
    stormStats.envEntries = (int)ex.entries;
    stormStats.envTruncated = (int)ex.truncated;
    stormStats.envLost = (int)ex.lost;

    split("env.towerIndex");
    // The shields' flowed copy (the line systems' stratiform rain is stamped where the low cloud reads it).
    auto *envShields = reinterpret_cast<GpuStormShield *>(static_cast<char *>(stormMapped) + kShieldEnvOffset);
    shieldEnvEntries.clear();
    const uint32_t nS = (uint32_t)std::min<size_t>(stormGpuShields.size(), kShieldMax);
    parallelEntries(stormCatalog.get(), nS, 256u, shieldEnvEntries, [&](uint32_t i, std::vector<StormEntry> &ents) {
        GpuStormShield g = stormGpuShields[i];
        const glm::dvec3 a = flowed(glm::normalize(glm::dvec3(glm::vec3(g.a))));
        const glm::dvec3 b = flowed(glm::normalize(glm::dvec3(glm::vec3(g.b))));
        g.a = glm::vec4(glm::vec3(a), g.a.w);
        g.b = glm::vec4(glm::vec3(b), g.b.w);
        envShields[i] = g;
        if (g.c.w <= 0.0f) return;   // only the raining (line-system) shields are stamped
        const glm::dvec3 mid = glm::normalize(a + b);
        const double half = 0.5 * std::acos(std::clamp(glm::dot(a, b), -1.0, 1.0)) * kReW;
        // + the clear air ahead of the line (100 km past the capsule, stormStamp).
        listStormBins(mid, 1.03 * (half + std::max(g.c.x, g.b.w)) + 100000.0 + 1000.0 * std::floor(0.5 * g.c.w), i,
                      kShieldBinsPerFace, ents);   // (+ the line's bow)
    });
    auto *senv = reinterpret_cast<uint32_t *>(static_cast<char *>(stormMapped) + kShieldEnvListOffset);
    fillStormIndex(shieldEnvEntries, shieldEnvTable, shieldEnvTableUsed, stormCounts, kShieldTableSize, kShieldListSize,
                   kShieldMaxPerBin, senv,
                   reinterpret_cast<glm::uvec2 *>(static_cast<char *>(stormMapped) + kShieldEnvTableOffset));
    split("env.shields");
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
        if (stormCatalog && tw.kind == wx::StormKind::Tropical)
            if (const wx::Cyclone *cy = stormCatalog->findCyclone(tw.stormId))
            {
                r["kind"] = "tropical cyclone";
                r["name"] = cy->name;
            }
        if (stormCatalog && tw.kind != wx::StormKind::Tropical)
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

// ── Tornadoes and waterspouts (2026-10-23) ──────────────────────────────────────────────────────────────────────────
// A storm's tornado is fixed by its id like everything else about it: ~35% of supercells put one down for 5-25 min in
// their mature phase, on the rear flank of the dominant tower; a strong cell over the sea may put down a waterspout.
// Drawn with the plume Gaussians cloud_march.comp already composites (its billows below ~10 km are an Earth-fixed
// field: the funnel's texture): a funnel of stacked Gaussians from the cloud base, lowering to touch down and roping
// out at its end, a lowered wall cloud above it and a debris cloud (spray over water) at its foot. Lit mostly by the sky
// (it stands under the storm's base).
namespace
{
uint64_t tnMix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
double tnU(uint64_t h) { return (double)(h >> 11) * (1.0 / 9007199254740992.0); }
glm::dvec3 tnMove(const glm::dvec3 &p, const glm::dvec3 &t, double distM)
{
    if (distM <= 0.0) return p;
    const double a = distM / kReW;
    return glm::normalize(p * std::cos(a) + t * std::sin(a));
}
} // namespace

bool SatelliteSim::tornadoOf(const wx::Storm &S, double t, TornadoNow &out) const
{
    out = TornadoNow{};
    if (S.kind != wx::StormKind::Supercell && S.kind != wx::StormKind::AirMass && S.kind != wx::StormKind::Multicell)
        return false;
    const uint64_t h = tnMix(S.id ^ 0x70A2AD0ull);
    const bool super = S.kind == wx::StormKind::Supercell;
    const wx::TowerSpec &ts = S.towers[0];
    double w0, w1;
    if (super)
    {
        if (tnU(h) > 0.35 || ts.anvOff <= ts.anvOn) return false;
        w0 = (double)ts.anvOn + 1200.0;
        w1 = (double)ts.anvOff - 600.0;
    }
    else
    {
        // Waterspouts: strong cells over the sea (the caller checks the sea), ~12%.
        if (tnU(h) > 0.12 || ts.peak < 0.6f) return false;
        w0 = (double)ts.t0 + (double)ts.growS;
        w1 = (double)ts.t0 + 0.75 * (double)ts.life;
        out.spout = true;
    }
    const double dur = (super ? 300.0 + 1200.0 * tnU(tnMix(h ^ 1)) : 240.0 + 600.0 * tnU(tnMix(h ^ 1)));
    if (w1 - w0 < dur) return false;
    out.t0 = S.t0 + w0 + (w1 - w0 - dur) * tnU(tnMix(h ^ 2));
    out.t1 = out.t0 + dur;
    out.seed = (uint32_t)(tnMix(h ^ 3) >> 32);
    out.stormId = S.id;
    out.r0 = (float)(out.spout ? 25.0 + 40.0 * tnU(tnMix(h ^ 4)) : 60.0 + 160.0 * std::pow(tnU(tnMix(h ^ 4)), 1.5));
    // Where: the tower's axis at t (as Catalog::towersAt), then the rear flank (behind and to the right of the motion
    // in the north: across the shear from the forward flank).
    const glm::dvec3 c = S.centreAt(t);
    const glm::dvec3 e = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), c) + glm::dvec3(1e-12, 0, 0));
    const glm::dvec3 n = glm::cross(c, e);
    const glm::dvec3 o = e * (double)ts.offE + n * (double)ts.offN;
    const double ol = glm::length(o);
    const glm::dvec3 axis = ol > 0.0 ? tnMove(c, o / ol, ol) : c;
    glm::dvec3 sh = glm::dvec3(S.shear) - axis * glm::dot(glm::dvec3(S.shear), axis);
    sh = glm::length(sh) > 1e-12 ? glm::normalize(sh) : e;
    const glm::dvec3 perp = glm::cross(axis, sh) * (axis.z >= 0.0 ? -1.0 : 1.0);
    // The tower's own radius at maturity, as cv2ColumnSigma sizes it (x mix(0.7, 1.3, size)): its rain core reaches 0.9
    // of it (gCv2RainCore). At a fixed 0.8 x "Cb tower radius" a big tower's core (to 1.17 of that) hid the tornado.
    const double Rc = std::clamp((double)cv2CbRadiusKm, 1.0, 20.0) * 1000.0 * (0.7 + 0.6 * (double)ts.size);
    // (Just past the core, ~1.05 tower radii out: the rain-free base under the updraft's rear edge.)
    const glm::dvec3 off = (-0.65 * sh + 0.7 * perp) * (1.1 * Rc) * (out.spout ? 0.3 : 1.0);
    out.upwindMap = -sh;
    const double offL = glm::length(off);
    out.mapDir = offL > 0.0 ? tnMove(axis, off / offL, offL) : axis;
    out.active = t >= out.t0 && t < out.t1;
    return true;
}

void SatelliteSim::weatherFillTornadoes(GpuDustCloud *out, uint32_t &n)
{
    FPROF_SCOPE("weatherFillTornadoes");
    tornadoesNow.clear();
    if (!stormObjects || !stormCatalog || !out) return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const double ph = cloudDriftPhase(), cph = std::cos(ph), sph = std::sin(ph);
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    const glm::dvec3 eye = up * (double)obsEyeRadiusM();
    const glm::dvec3 E = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up) + glm::dvec3(1e-12, 0, 0));
    const glm::dvec3 N = glm::cross(up, E);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, E), glm::dot(v, N), glm::dot(v, up)); };
    const glm::dvec3 si = sunDirEciAt(t);
    const double g = earthRotationAngle(t);
    const glm::dvec3 sunE = glm::normalize(glm::dvec3(si.x * std::cos(g) + si.y * std::sin(g),
                                                      -si.x * std::sin(g) + si.y * std::cos(g), si.z));
    // The storms whose dominant tower stands within ~200 km (the tower list: this frame's, map frame).
    std::vector<uint64_t> seen;
    for (const wx::Tower &tw : stormTowers)
    {
        if (tw.towerIndex != 0 || tw.kind == wx::StormKind::Tropical) continue;
        const glm::dvec3 d = glm::normalize(tw.dir);
        const glm::dvec3 ed(d.x * cph + d.y * sph, -d.x * sph + d.y * cph, d.z);
        if (glm::dot(ed, up) < std::cos(200000.0 / kReW)) continue;
        if (std::find(seen.begin(), seen.end(), tw.stormId) != seen.end()) continue;
        seen.push_back(tw.stormId);
        const wx::Storm *S = stormCatalog->find(tw.stormId);
        if (!S) continue;
        TornadoNow T;
        if (!tornadoOf(*S, t, T) || !T.active) continue;
        const glm::dvec3 md = T.mapDir;
        const glm::dvec3 te(md.x * cph + md.y * sph, -md.x * sph + md.y * cph, md.z);   // Earth-fixed
        const float lat = (float)glm::degrees(std::asin(std::clamp(te.z, -1.0, 1.0)));
        const float lon = (float)glm::degrees(std::atan2(te.y, te.x));
        const bool sea = oceanFraction(glm::radians((double)lat), glm::radians((double)lon), 3000.0, 1) > 0.5f;
        if (T.spout && !sea) continue;   // waterspouts only over water
        T.earthDir = te;
        T.sea = sea;
        tornadoesNow.push_back(T);
    }
    const double kTwoPi15 = 15.749609945722419;
    for (const TornadoNow &T : tornadoesNow)
    {
        const double ground = T.sea ? 0.0 : std::max(0.0, (double)cpuTerrainHeightM(
            (float)glm::degrees(std::asin(std::clamp(T.earthDir.z, -1.0, 1.0))), (float)glm::degrees(std::atan2(T.earthDir.y, T.earthDir.x))));
        const double Hb = std::max(800.0, (double)cv2Types[4].alt.x) * (T.spout ? 0.8 : 1.0);
        const glm::dvec3 base = T.earthDir * (kReW + ground);
        const glm::dvec3 rel0 = base - eye;
        if (glm::length(rel0) > 250000.0) continue;
        // The lifecycle: the funnel lowers over the first ~90 s, roping out (thinner, bent) over the last third.
        const double life = T.t1 - T.t0, tau = t - T.t0;
        const double lower = glm::smoothstep(0.0, std::min(90.0, 0.2 * life), tau);
        const double rope = glm::smoothstep(0.66, 1.0, tau / life);
        const double fade = 1.0 - glm::smoothstep(0.92, 1.0, tau / life);
        const double zBot = (1.0 - lower) * 0.65 * Hb;
        const glm::dvec3 eE = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), T.earthDir) + glm::dvec3(1e-12, 0, 0));
        const glm::dvec3 nE = glm::cross(T.earthDir, eE);
        const double bendAz = 6.2831853 * tnU(tnMix((uint64_t)T.seed ^ 9));
        const glm::dvec3 bendDir = eE * std::cos(bendAz) + nE * std::sin(bendAz);
        const double bend = (0.15 + 0.6 * rope) * Hb;
        const double sinE = glm::dot(T.earthDir, sunE);
        const double amb = glm::smoothstep(-0.1, 0.25, sinE);
        if (amb <= 0.0) continue;                        // (night: unlit; lightning lights it in the shader's flash pass)
        // The spin (2026-10-23): each part turns cyclonically (counter-clockwise from above in the north) as a rigid
        // body at its own rate: the funnel ~35 m/s at its radius, the debris a little slower, the wall cloud a turn in
        // ~5 min. cloud_march.comp reads the billows in cylindrical coordinates turned by this phase (seedK >= 256).
        const double hemi = T.earthDir.z >= 0.0 ? 1.0 : -1.0;
        const double wFun = std::clamp(35.0 / std::max((double)T.r0, 1.0), 0.12, 0.6);
        auto spinK = [&](double omega) {
            const double turns = hemi * omega * tau / (2.0 * glm::pi<double>());
            return 256.0 + std::floor((turns - std::floor(turns)) * 3840.0);
        };
        auto emit = [&](const glm::dvec3 &C, const glm::dvec3 &axis, double sA, double sP, double tau0, double albedo,
                        const glm::dvec3 &tint, double sunShare, double omega) {
            if (n >= kMaxDustClouds) return;
            const glm::dvec3 rel = C - eye;
            const double range = std::max(glm::length(rel), 1.0);
            const double Sigma = tau0 * 2.0 * glm::pi<double>() * sP * sA * fade;
            if (Sigma < 1.0) return;
            const glm::dvec3 a = toEnu(axis);
            glm::dvec3 b2 = glm::cross(a, glm::dvec3(0, 0, 1));
            b2 = glm::length(b2) > 1e-6 ? glm::normalize(b2) : glm::dvec3(1, 0, 0);
            const glm::dvec3 c2 = glm::cross(a, b2);
            const double iA = 1.0 / (sA * sA), iP = 1.0 / (sP * sP);
            auto Mx = [&](int p, int q) { return iA * a[p] * a[q] + iP * (b2[p] * b2[q] + c2[p] * c2[q]); };
            GpuDustCloud &d = out[n++];
            const glm::dvec3 re = toEnu(rel);
            d.c = glm::vec4(glm::vec3(re), (float)Sigma);
            d.ia = glm::vec4((float)Mx(0, 0), (float)Mx(1, 1), (float)Mx(2, 2), (float)(1.0 / (kTwoPi15 * sA * sP * sP)));
            // Under the storm's base: mostly the sky's light (isotropic), a little direct Sun (the shader shadows it).
            const glm::dvec3 iso = 4.0 * glm::pi<double>() * amb * 0.06 * glm::dvec3(0.66, 0.72, 0.82) * albedo * tint;
            const glm::dvec3 sun = 4.0 * glm::pi<double>() * amb * sunShare * albedo * tint;
            const double lI = iso.x + iso.y + iso.z, lS = sun.x + sun.y + sun.z;
            const double share = std::min(lI / std::max(lI + lS, 1e-9), 0.999);
            const double seedK = spinK(omega);
            d.ib = glm::vec4((float)Mx(0, 1), (float)Mx(0, 2), (float)Mx(1, 2), (float)(4.0 * seedK + 2.0 + share));
            d.col = glm::vec4(glm::vec3(iso + sun), (float)(2.0 + albedo));
            const double R = 5.5 * std::max(sA, sP);
            d.cone = R >= 0.95 * range ? glm::vec4(0.0f, 0.0f, 0.0f, -2.0f)
                                       : glm::vec4(glm::vec3(re / range), (float)std::sqrt(1.0 - (R / range) * (R / range)));
        };
        // The funnel: 10 Gaussians from its bottom to the base, wider upward (r0 at the ground to ~5 x), thinner as it
        // ropes out, bent.
        const int kSeg = 10;
        const double r0 = (double)T.r0 * (1.0 - 0.6 * rope);
        const glm::vec3 grey(0.78f, 0.78f, 0.80f);
        for (int k = 0; k < kSeg; ++k)
        {
            const double f0 = (double)k / kSeg, f1 = (double)(k + 1) / kSeg;
            const double z0 = zBot + (Hb - zBot) * f0, z1 = zBot + (Hb - zBot) * f1, zm = 0.5 * (z0 + z1);
            const double q = zm / Hb;
            const double rr = r0 * (1.0 + (T.spout ? 3.0 : 2.2) * std::pow(q, 2.2));
            const double lat0 = bend * std::sin(3.14159 * q) * (1.0 - q * 0.3);
            const glm::dvec3 p = T.earthDir * (kReW + ground + zm) + bendDir * lat0;
            const glm::dvec3 axis = glm::normalize(T.earthDir * (z1 - z0) + bendDir * (bend * 3.14159 / Hb *
                                                    std::cos(3.14159 * q) * (z1 - z0)));
            // (Dense and narrow: a Gaussian's visible edge sharpens with its peak optical depth; this puts the edge,
            // tau ~ 1, at ~rr, as a condensation funnel's defined outline. At tau 3.5 and sigma rr it was a soft cone.)
            emit(p, axis, 0.6 * (z1 - z0), rr / 2.6, 40.0, 0.45, glm::dvec3(grey), 0.06, wFun);
        }
        // The wall cloud: a lowered, rotating disc under the base, 1.5-3 km across.
        emit(T.earthDir * (kReW + ground + Hb - 120.0), T.earthDir, 160.0, (T.spout ? 450.0 : 1500.0), 1.0, 0.4,
             glm::dvec3(0.6, 0.62, 0.66), 0.02, 0.021);
        // The debris cloud (land) or the spray ring (sea) once it touches down.
        if (zBot < 30.0)
        {
            const bool spray = T.sea;
            emit(T.earthDir * (kReW + ground + (spray ? 40.0 : 90.0)), T.earthDir, spray ? 50.0 : 110.0,
                 r0 * (spray ? 2.2 : 2.8), spray ? 1.2 : 2.0, spray ? 0.8 : 0.45,
                 spray ? glm::dvec3(0.95, 0.97, 1.0) : glm::dvec3(0.62, 0.52, 0.42), 0.15, 0.7 * wFun);
        }
    }
}

// The two tropical cyclones nearest the observer, for the sea state (sat_sky.frag oceanSeaState: the waves, whitecaps and
// surf follow the storm's wind field, as the rain and the ambience do through surfaceWindAt).
void SatelliteSim::weatherFillSeaParams(GpuCloudParams &cp)
{
    for (int k = 0; k < 2; ++k) { cp.seaCycA[k] = glm::vec4(0.0f); cp.seaCycB[k] = glm::vec4(0.0f); }
    if (!stormObjects) return;
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    std::vector<std::pair<double, size_t>> near;
    for (size_t i = 0; i < stormCyclonesNow.size(); ++i)
    {
        const wx::CycloneState &c = stormCyclonesNow[i];
        const double reach = (double)c.outerR * 1.5;
        const double d = std::acos(std::clamp(glm::dot(up, c.earth), -1.0, 1.0)) * kReW;
        if (d < reach + 3.0e6) near.push_back({d - reach, i});   // (anything the eye can see the sea of)
    }
    std::sort(near.begin(), near.end());
    for (size_t k = 0; k < near.size() && k < 2; ++k)
    {
        const wx::CycloneState &c = stormCyclonesNow[near[k].second];
        cp.seaCycA[k] = glm::vec4(glm::vec3(c.earth), std::max(c.rmw, 5000.0f));
        cp.seaCycB[k] = glm::vec4(c.vmax, (float)c.outerR * 1.5f, 0.0f, 0.0f);
    }
}

