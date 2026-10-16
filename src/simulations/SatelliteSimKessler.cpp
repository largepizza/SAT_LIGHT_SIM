// Kessler syndrome, the sim side: "The Sun's Fury" (branch kessler, 2026-10-16; plan .plans/KESSLER_PLAN.md).
//
// kessler::World (Kessler.cpp) holds the deterministic history — the storm's scripted deaths, the dead
// formation members' bumps, the player's breakups and the engine's collisions — and runs on a worker
// thread. This file builds it from the roster, mirrors what it produces (pool records, end seconds,
// events) on the main thread, uploads that to kesslerBuf for sat_orbit.comp, and keeps an event cache
// so a later launch replays instead of re-simulating.
#include "SatelliteSim.h"
#include "../Log.h"
#include "../Paths.h"
#include "../VulkanContext.h"
#include "../UIRenderer.h"
#include "UIPalette.h"
#include "clay.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>

namespace
{
// 2037-07-14 09:30 UTC the flare, 2037-07-15 03:00 UTC the CME's arrival (17.5 h: Carrington's transit).
constexpr double kFuryFlareT = 1184448600.0;
constexpr double kFuryArriveT = 1184511600.0;
constexpr double kFuryPeakKp = 11.0;
constexpr uint64_t kStormSeed = 0x5EEDF0221ull;
constexpr uint32_t kCacheMagic = 0x4B534C52u; // "RLSK"
constexpr uint32_t kCacheVersion = 3; // bump whenever the world's logic changes

uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = static_cast<const unsigned char *>(p);
    for (size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
template <class T> uint64_t fnvv(uint64_t h, const T &v) { return fnv(h, &v, sizeof(T)); }

// Masses (kg) for the shipped models when a type gives no "mass_kg": published or estimated.
double defaultMassKg(const std::string &model, const std::string &name)
{
    static const struct
    {
        const char *id;
        double kg;
    } kM[] = {{"starlink_v1_5", 306},   {"starlink_v2_mini", 800},    {"starlink_v3", 1900},
              {"starlink_v2_mini_dtc", 970}, {"oneweb", 150},       {"amazon_leo", 600},
              {"guowang", 800},         {"iss", 420000},             {"tiangong", 100000},
              {"haven1", 14000},        {"haven2", 60000},           {"axiom_station", 60000},
              {"orbital_reef", 100000}, {"ross", 50000},             {"bharatiya_station", 52000},
              {"starlab", 90000},       {"starship_depot", 200000},  {"spacex_ai_sat", 2500},
              {"reflect_orbital", 500}, {"debris_fragment", 1},      {"hubble", 11110},
              {"starlink_v1_0", 260},   {"starlink_visorsat", 260}};
    for (const auto &m : kM)
        if (model == m.id)
            return m.kg;
    if (name.find("Debris") != std::string::npos)
        return 1.0;
    return 300.0;
}
} // namespace

// ── The worker ───────────────────────────────────────────────────────────────────────────────────
struct SatelliteSim::KesslerWorker
{
    kessler::World w;
    std::vector<kessler::Event> pending; // seeds + user + cached engine events, by time
    size_t pendingNext = 0;
    double replayUntil = 0.0; // engine sampling resumes here (the cache's frontier)
    double tEnd = 0.0;
    std::atomic<double> wantUntil{0.0}; // lazy: simulate only this far (the main thread moves it with sim time)
    std::thread th;
    std::mutex mx;
    bool stop = false;
    std::atomic<bool> done{false};
    // The outbox (under mx): what the main thread has not taken yet.
    size_t pubPool = 0, pubEvents = 0;
    std::vector<kessler::PoolRec> outPool;
    std::vector<kessler::Event> outEvents;
    std::vector<std::pair<uint32_t, int32_t>> outPoolDeaths;
    struct RosterChange
    {
        uint32_t sat;
        int32_t dead, end;
    };
    std::vector<RosterChange> outRoster;
    double outFrontier = 0.0;

    void publish()
    {
        std::lock_guard<std::mutex> lk(mx);
        outPool.insert(outPool.end(), w.pool.begin() + (ptrdiff_t)pubPool, w.pool.end());
        pubPool = w.pool.size();
        outEvents.insert(outEvents.end(), w.events.begin() + (ptrdiff_t)pubEvents, w.events.end());
        pubEvents = w.events.size();
        outPoolDeaths.insert(outPoolDeaths.end(), w.poolDeaths.begin(), w.poolDeaths.end());
        for (uint32_t i : w.rosterChanged)
            outRoster.push_back({i, w.rosterDead[i], w.rosterEnd[i]});
        w.poolDeaths.clear();
        w.rosterChanged.clear();
        outFrontier = w.frontier;
    }

    void run()
    {
        kessler::lowerThreadPriority();
        const auto t0 = std::chrono::steady_clock::now();
        // Replay (no sampling) up to the cache's frontier, then simulate. A day of sim time per chunk,
        // published after each.
        while (!stop && w.frontier < tEnd)
        {
            const double want = std::min(wantUntil.load(), tEnd);
            if (w.frontier >= want)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
            const bool sampling = w.frontier >= replayUntil;
            const double chunk = sampling ? 86400.0 : 30.0 * 86400.0;
            double to = std::min({w.frontier + chunk, tEnd, std::max(want, w.frontier + w.s.stepS)});
            if (!sampling)
                to = std::min(to, replayUntil);
            simulateChunk(to, sampling);
            publish();
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        char msg[200];
        snprintf(msg, sizeof(msg), "kessler: worker %s at %.0f days after the CME: %zu events, %zu records, %.1f s",
                 stop ? "stopped" : "done", (w.frontier - w.s.epoch) / 86400.0, w.events.size(), w.pool.size(), s);
        Log::line(msg);
        done = !stop;
    }

    void simulateChunk(double to, bool sampling)
    {
        if (sampling)
        {
            w.simulate(to, pending, pendingNext, &stop);
            return;
        }
        // Replay: apply the pending events only, keeping the steps (and the sample counter) aligned.
        while (w.frontier < to && !stop)
        {
            const double t1 = std::min(w.frontier + w.s.stepS, to);
            while (pendingNext < pending.size() && pending[pendingNext].t <= t1)
                w.apply(pending[pendingNext++]);
            if (t1 > w.s.epoch)
                ++w.sampleCount;
            w.frontier = t1;
        }
    }
};

void SatelliteSim::KesslerWorkerDelete::operator()(KesslerWorker *w) const { delete w; }

double SatelliteSim::kesslerEpoch() const { return kFuryArriveT; }

uint32_t SatelliteSim::kesslerRefOfSat(uint32_t sat) const
{
    if (sat < activeSatCount)
        return sat;
    const uint32_t j = sat - activeSatCount;
    return j < kPool.size() ? (j | kessler::kPoolBit) : kessler::kNoRef;
}

// ── Building the world ───────────────────────────────────────────────────────────────────────────
static void buildWorld(const SatelliteSim &sim, kessler::World &w, const std::vector<SatOrbit> &orbits,
                       uint32_t rosterCount, const std::vector<ConstellationConfig> &consts,
                       const std::vector<SatelliteType> &types, double raanAnchorT, double epoch, float rateMult,
                       float avoidance, float horizonYears, uint32_t debrisType, int debrisConst)
{
    (void)sim;
    w.s = kessler::WorldSettings{};
    w.s.epoch = epoch;
    w.s.raanAnchorT = raanAnchorT;
    w.s.horizonS = std::max(1.0f, horizonYears) * 365.25 * 86400.0;
    w.s.engine.rateMultiplier = rateMult;
    w.s.engine.avoidance = avoidance;
    w.s.debrisType = debrisType;
    w.s.debrisConst = (uint8_t)std::max(debrisConst, 0);
    w.types.resize(types.size());
    for (size_t t = 0; t < types.size(); ++t)
    {
        const SatelliteType &ty = types[t];
        kessler::TypeInfo &ti = w.types[t];
        ti.massKg = ty.massKg > 0.0f ? ty.massKg : defaultMassKg(ty.modelId, ty.name);
        ti.areaM2 = (ty.meshRadiusM > 0.0f && ty.meshFill > 0.0f)
                        ? (double)ty.meshFill * glm::pi<double>() * ty.meshRadiusM * ty.meshRadiusM
                        : std::max(0.01f, ty.crossSectionM2);
        ti.radiusM = std::sqrt(ti.areaM2 / glm::pi<double>());
        ti.passive = ty.modelId == "debris_fragment" || ty.name.find("Debris") != std::string::npos;
    }
    w.s.debrisModelArea = w.types.empty() ? 0.03 : w.types[debrisType].areaM2;
    w.roster.resize(rosterCount);
    for (uint32_t i = 0; i < rosterCount; ++i)
    {
        const SatOrbit &o = orbits[i];
        kessler::RosterObj &r = w.roster[i];
        r.raan = o.raan;
        r.incl = o.incl;
        r.u0 = o.u0;
        r.R = o.R_sat;
        r.n = o.meanMot;
        r.sso = o.alignTerminator;
        r.formation = o.formation;
        r.formP = o.formP;
        r.formQ = o.formQ;
        r.formK = o.formK;
        r.type = (uint16_t)o.typeIdx;
        r.constIdx = (uint8_t)o.constIdx;
        if (o.formation && o.constIdx < consts.size())
        {
            const ConstellationConfig &cc = consts[o.constIdx];
            const uint32_t C = (uint32_t)std::clamp(cc.clusterSize, 1, 255);
            r.clusterSize = (uint8_t)C;
            r.clusterSlot = (uint8_t)((i - cc.orbitStart) % C);
            // A short last cluster: the neighbour must exist in it.
            if (i - r.clusterSlot + C > cc.orbitStart + cc.orbitCount)
                r.clusterSize = (uint8_t)std::max<uint32_t>(1, cc.orbitStart + cc.orbitCount - (i - r.clusterSlot));
        }
    }
    w.reset();
}

static uint64_t worldHash(const kessler::World &w)
{
    uint64_t h = 1469598103934665603ull;
    h = fnvv(h, kCacheVersion);
    h = fnvv(h, w.s.epoch);
    h = fnvv(h, w.s.raanAnchorT);
    h = fnvv(h, w.s.stepS);
    h = fnvv(h, w.s.LcMin);
    h = fnvv(h, w.s.maxRecordsPerEvent);
    h = fnvv(h, w.s.poolCapacity);
    h = fnvv(h, w.s.activeSample);
    h = fnvv(h, w.s.stormDensityGain);
    h = fnvv(h, w.s.debrisType);
    h = fnvv(h, w.s.engine.cubeM);
    h = fnvv(h, w.s.engine.avoidance);
    h = fnvv(h, w.s.engine.rateMultiplier);
    h = fnvv(h, kFuryArriveT);
    h = fnvv(h, kFuryPeakKp);
    for (const kessler::TypeInfo &t : w.types)
    {
        h = fnvv(h, t.massKg);
        h = fnvv(h, t.areaM2);
        h = fnvv(h, t.passive);
    }
    if (!w.roster.empty())
        h = fnv(h, w.roster.data(), w.roster.size() * sizeof(kessler::RosterObj));
    return h;
}

static std::string cacheName(uint64_t h)
{
    char b[64];
    snprintf(b, sizeof(b), "suns_fury_%016llx.bin", (unsigned long long)h);
    return b;
}

// The cache: the engine's events (with the user events they were made beside) and the frontier.
static bool readCache(const std::filesystem::path &p, uint64_t hash, std::vector<kessler::Event> &engineEvents,
                      double &frontier)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return false;
    uint32_t magic = 0, ver = 0;
    uint64_t h = 0, n = 0;
    double fr = 0.0;
    f.read((char *)&magic, 4).read((char *)&ver, 4).read((char *)&h, 8).read((char *)&fr, 8).read((char *)&n, 8);
    if (!f || magic != kCacheMagic || ver != kCacheVersion || h != hash || n > 100000000ull)
        return false;
    engineEvents.resize((size_t)n);
    f.read((char *)engineEvents.data(), (std::streamsize)(n * sizeof(kessler::Event)));
    if (!f)
        return false;
    frontier = fr;
    return true;
}

static void writeCache(const std::filesystem::path &p, uint64_t hash, const std::vector<kessler::Event> &events,
                       double frontier)
{
    std::vector<kessler::Event> eng;
    for (const kessler::Event &e : events)
        if (e.origin == kessler::ORIGIN_ENGINE)
            eng.push_back(e);
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    const std::filesystem::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
        {
            Log::line("kessler: cannot write " + tmp.string());
            return;
        }
        const uint64_t n = eng.size();
        f.write((const char *)&kCacheMagic, 4).write((const char *)&kCacheVersion, 4).write((const char *)&hash, 8);
        f.write((const char *)&frontier, 8).write((const char *)&n, 8);
        f.write((const char *)eng.data(), (std::streamsize)(n * sizeof(kessler::Event)));
    }
    std::filesystem::rename(tmp, p, ec);
    Log::line("kessler: cache " + p.filename().string() + (ec ? " NOT written: " + ec.message() : " written") + ", " +
              std::to_string(eng.size()) + " engine events, day " + std::to_string((int)((frontier - kFuryArriveT) / 86400.0)));
}

// ── Start / stop ────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::kesslerInit(VulkanContext &ctx)
{
    spaceWeatherSetScriptedStorm(kesslerEnabled, kFuryArriveT, kFuryPeakKp);
    kesslerRestart(ctx, kUserEvents, -1.0);
}

void SatelliteSim::kesslerShutdown()
{
    if (!kWorker)
        return;
    {
        std::lock_guard<std::mutex> lk(kWorker->mx);
        kWorker->stop = true;
    }
    if (kWorker->th.joinable())
        kWorker->th.join();
    // Cache what was simulated (only while the player has caused nothing: their breakups are session state).
    if (kUserEvents.empty() && kWorker->w.frontier > kCacheFrontier + 1.0)
        writeCache(std::filesystem::path(Paths::userDataDir()) / "kessler" / cacheName(kHash), kHash,
                   kWorker->w.events, kWorker->w.frontier);
    kWorker.reset();
}

// (Re)starts the world from scratch: the storm's seeds, the user's events and the engine events before
// keepEngineBefore (< 0: everything the cache holds) are replayed, then the engine simulates on.
void SatelliteSim::kesslerRestart(VulkanContext &ctx, const std::vector<kessler::Event> &userEvents,
                                  double keepEngineBefore)
{
    std::vector<kessler::Event> keptEngine;
    double keptFrontier = kFuryArriveT;
    if (kWorker)
    {
        {
            std::lock_guard<std::mutex> lk(kWorker->mx);
            kWorker->stop = true;
        }
        if (kWorker->th.joinable())
            kWorker->th.join();
        if (keepEngineBefore >= 0.0)
        {
            for (const kessler::Event &e : kWorker->w.events)
                if (e.origin == kessler::ORIGIN_ENGINE && e.t < keepEngineBefore)
                    keptEngine.push_back(e);
            keptFrontier = std::min(keepEngineBefore, kWorker->w.frontier);
        }
        kWorker.reset();
    }
    // The main thread's mirror starts empty; the pool tail of satOrbits goes with it.
    kPool.clear();
    kEvents.clear();
    kRosterWords.assign(2ull * activeSatCount, kessler::kNever);
    satOrbits.resize(activeSatCount);
    kPoolGpu = 0;
    kDirtyPool.clear();
    kDirtyRoster.clear();
    kRosterFullUpload = true;
    kFrontier = kFuryArriveT;
    kWorkerDone = false;
    if (selectedSatIndex >= (int)activeSatCount)
        selectedSatIndex = -1;
    if (followActive && followSatIndex >= (int)activeSatCount)
        stopFollow();
    if (!kesslerEnabled || activeSatCount == 0)
    {
        kWorkerDone = true;
        return;
    }

    std::unique_ptr<KesslerWorker, KesslerWorkerDelete> wk(new KesslerWorker());
    const double raanAnchorT = (double)simInitDayJ2000 * 86400.0 + simInitSecInDay;
    buildWorld(*this, wk->w, satOrbits, activeSatCount, constellations, satTypes, raanAnchorT, kFuryArriveT,
               kesslerRateMult, kesslerAvoidance, kesslerHorizonYears, kDebrisType, kDebrisConst);
    kHash = worldHash(wk->w);
    wk->tEnd = kFuryArriveT + wk->w.s.horizonS;

    // The engine's events: those kept from the run being replaced, else the cache (user data, then the
    // shipped copy next to the exe), else none.
    double replayUntil = kFuryArriveT;
    if (keepEngineBefore >= 0.0)
        replayUntil = keptFrontier;
    else
    {
        const std::string name = cacheName(kHash);
        std::vector<kessler::Event> cached;
        double fr = 0.0;
        if (readCache(std::filesystem::path(Paths::userDataDir()) / "kessler" / name, kHash, cached, fr) ||
            readCache(std::filesystem::path(Paths::exeDir()) / "kessler" / name, kHash, cached, fr))
        {
            keptEngine = std::move(cached);
            replayUntil = fr;
            Log::line("kessler: cache " + name + ": " + std::to_string(keptEngine.size()) + " engine events, " +
                      std::to_string((int)((fr - kFuryArriveT) / 86400.0)) + " days");
        }
        else
            Log::line("kessler: no cache " + name + " - simulating from the storm");
    }
    // Align the replay boundary to the engine's steps.
    replayUntil = kFuryArriveT + std::floor((replayUntil - kFuryArriveT) / wk->w.s.stepS) * wk->w.s.stepS;
    std::vector<kessler::Event> pend = kessler::stormSeedEvents(wk->w, kFuryArriveT, kStormSeed);
    for (const kessler::Event &e : userEvents)
        pend.push_back(e);
    for (const kessler::Event &e : keptEngine)
        if (e.t < replayUntil)
            pend.push_back(e);
    std::stable_sort(pend.begin(), pend.end(), [](const kessler::Event &a, const kessler::Event &b) { return a.t < b.t; });
    wk->pending = std::move(pend);
    wk->replayUntil = replayUntil;
    kCacheFrontier = keepEngineBefore >= 0.0 ? kFuryArriveT : replayUntil;
    wk->wantUntil = std::max(kFuryArriveT + 2.0 * 86400.0, (double)simDayJ2000 * 86400.0 + simSecInDay + 86400.0);
    {
        char msg[200];
        snprintf(msg, sizeof(msg), "kessler: world %016llx: %u satellites, %zu pending events (replay to day %.1f)",
                 (unsigned long long)kHash, activeSatCount, wk->pending.size(), (replayUntil - kFuryArriveT) / 86400.0);
        Log::line(msg);
    }
    kesslerEnsureCapacity(ctx, std::max<size_t>(16384, kPoolCap));
    KesslerWorker *raw = wk.get();
    wk->th = std::thread([raw] { raw->run(); });
    kWorker = std::move(wk);
}

// ── Per frame ───────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::kesslerWant(double tJ2000)
{
    if (kWorker && tJ2000 > kWorker->wantUntil.load())
        kWorker->wantUntil = tJ2000;
}

void SatelliteSim::kesslerSync(VulkanContext &ctx)
{
    if (!kWorker)
        return;
    {
        // Lazy: keep the world a minute of the current time warp ahead of sim time (at least two days, and
        // always the storm's first two days), not the whole horizon at once.
        const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
        const double ahead = std::clamp(60.0 * (double)kTimeScales[timeScaleIdx], 2.0 * 86400.0, 400.0 * 86400.0);
        const double want = std::max(kFuryArriveT + 2.0 * 86400.0, t + ahead);
        if (want > kWorker->wantUntil.load())
            kWorker->wantUntil = want;
    }
    std::vector<kessler::PoolRec> nPool;
    std::vector<kessler::Event> nEv;
    std::vector<std::pair<uint32_t, int32_t>> pd;
    std::vector<KesslerWorker::RosterChange> rd;
    {
        std::lock_guard<std::mutex> lk(kWorker->mx);
        nPool.swap(kWorker->outPool);
        nEv.swap(kWorker->outEvents);
        pd.swap(kWorker->outPoolDeaths);
        rd.swap(kWorker->outRoster);
        kFrontier = kWorker->outFrontier > 0.0 ? kWorker->outFrontier : kFrontier;
    }
    if (kWorker->done && !kWorkerDone)
    {
        kWorkerDone = true;
        if (kUserEvents.empty() && kWorker->w.frontier > kCacheFrontier + 1.0)
            writeCache(std::filesystem::path(Paths::userDataDir()) / "kessler" / cacheName(kHash), kHash,
                       kWorker->w.events, kWorker->w.frontier);
    }
    if (!nPool.empty())
    {
        const size_t base = kPool.size();
        kPool.insert(kPool.end(), nPool.begin(), nPool.end());
        satOrbits.reserve(activeSatCount + kPool.size());
        for (size_t j = base; j < kPool.size(); ++j)
        {
            const kessler::PoolRec &r = kPool[j];
            SatOrbit o{};
            o.typeIdx = r.type;
            o.constIdx = r.constIdx;
            o.incl = (float)r.incl;
            o.altM = (float)(r.a - satphot::kEarthRadiusM);
            o.R_sat = (float)r.a;
            o.meanMot = r.n;
            o.tumbleRate = r.tumbleRate;
            o.tumblePhase = r.tumble0;
            o.tumbleAxis = glm::vec3(kessler::poolTumbleAxis((uint32_t)j));
            o.poolSlot = (int32_t)j;
            satOrbits.push_back(o);
        }
        kesslerEnsureCapacity(ctx, kPool.size());
    }
    for (const auto &d : pd)
        if (d.first < kPool.size())
        {
            kPool[d.first].deathSec = d.second;
            if (d.first < kPoolGpu)
                kDirtyPool.push_back(d.first);
        }
    for (const auto &d : rd)
        if (2ull * d.sat + 1 < kRosterWords.size())
        {
            kRosterWords[2 * d.sat] = d.dead;
            kRosterWords[2 * d.sat + 1] = d.end;
            kDirtyRoster.push_back(d.sat);
        }
    kEvents.insert(kEvents.end(), nEv.begin(), nEv.end());
}

void SatelliteSim::kesslerEnsureCapacity(VulkanContext &ctx, size_t records)
{
    if (records <= kPoolCap && kesslerBuf != VK_NULL_HANDLE)
        return;
    // Grow by doubling, but never past the world's cap (records beyond it are never made).
    const uint32_t maxCap = kessler::WorldSettings{}.poolCapacity;
    uint32_t cap = kPoolCap;
    while (cap < records)
        cap = (uint32_t)std::min<uint64_t>((uint64_t)cap * 2, 0x7FFFFFF0ull);
    if (records <= maxCap)
        cap = std::min(cap, maxCap);
    vkDeviceWaitIdle(ctx.device);
    // The visible list holds every satellite: it grows with the pool. Its contents are per frame.
    VkBuffer bufs[] = {satVisibleBuf, satVisibleIdxBuf, kesslerBuf, kPoolBaseBuf};
    VkDeviceMemory mems[] = {satVisibleMem, satVisibleIdxMem, kesslerMem, kPoolBaseMem};
    for (int i = 0; i < 4; ++i)
    {
        vkDestroyBuffer(ctx.device, bufs[i], nullptr);
        vkFreeMemory(ctx.device, mems[i], nullptr);
    }
    kPoolCap = cap;
    const VkDeviceSize slots = (VkDeviceSize)std::max<uint32_t>(activeSatCount, 1) + kPoolCap + kCitySpriteMax;
    ctx.createBuffer(sizeof(GpuSatVisible) * slots, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, satVisibleBuf, satVisibleMem);
    ctx.createBuffer(sizeof(uint32_t) * slots, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, satVisibleIdxBuf, satVisibleIdxMem);
    kPoolWord0 = (2u * std::max<uint32_t>(activeSatCount, 1) + 3u) & ~3u;
    ctx.createBuffer(4ull * ((VkDeviceSize)kPoolWord0 + 16ull * kPoolCap),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, kesslerBuf, kesslerMem);
    ctx.createBuffer(16ull * kPoolCap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, kPoolBaseBuf, kPoolBaseMem);
    writeSatRosterDescriptors(ctx);
    kesslerUploadAllSync(ctx);
    char msg[160];
    snprintf(msg, sizeof(msg), "kessler: GPU pool capacity %u records (%.0f MB)", kPoolCap,
             (double)kPoolCap * (64 + 16 + sizeof(GpuSatVisible) + 4) / (1024.0 * 1024.0));
    Log::line(msg);
}

// After (re)allocating: the roster's end seconds now (synchronously), the records progressively
// (kesslerRecordUploads, from record 0).
void SatelliteSim::kesslerUploadAllSync(VulkanContext &ctx)
{
    kPoolGpu = 0;
    kDirtyPool.clear();
    kDirtyRoster.clear();
    kRosterFullUpload = false;
    if (kRosterWords.size() < 2ull * activeSatCount)
        kRosterWords.resize(2ull * activeSatCount, kessler::kNever);
    const VkDeviceSize bytes = 8ull * std::max<uint32_t>(activeSatCount, 1);
    VkBuffer st;
    VkDeviceMemory sm;
    ctx.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, st, sm);
    void *m = nullptr;
    vkMapMemory(ctx.device, sm, 0, bytes, 0, &m);
    if (activeSatCount > 0)
        memcpy(m, kRosterWords.data(), 8ull * activeSatCount);
    else
        memset(m, 0x7F, bytes);
    vkUnmapMemory(ctx.device, sm);
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    VkBufferCopy region{0, 0, bytes};
    vkCmdCopyBuffer(cmd, st, kesslerBuf, 1, &region);
    ctx.endOneTimeCommands(cmd);
    vkDestroyBuffer(ctx.device, st, nullptr);
    vkFreeMemory(ctx.device, sm, nullptr);
    if (kStageBuf == VK_NULL_HANDLE)
    {
        ctx.createBuffer(kStageBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, kStageBuf, kStageMem);
        vkMapMemory(ctx.device, kStageMem, 0, kStageBytes, 0, &kStageMapped);
    }
}

void SatelliteSim::kesslerRecordUploads(VkCommandBuffer cmd)
{
    if (kesslerBuf == VK_NULL_HANDLE || !kStageMapped)
        return;
    if (kRosterFullUpload)
    {
        // A restart: every roster end second again (the GPU still holds the replaced world's).
        kDirtyRoster.clear();
        for (uint32_t i = 0; i < activeSatCount; ++i)
            kDirtyRoster.push_back(i);
        kRosterFullUpload = false;
    }
    const size_t newRecs = kPool.size() > kPoolGpu ? kPool.size() - kPoolGpu : 0;
    if (newRecs == 0 && kDirtyPool.empty() && kDirtyRoster.empty())
        return;
    char *stage = static_cast<char *>(kStageMapped);
    VkDeviceSize off = 0;
    std::vector<VkBufferCopy> toK, toB;
    // Roster dead / end seconds: runs of consecutive satellites, a quarter of the staging a frame (a long run,
    // e.g. the whole roster after a restart, goes up in pieces over several frames).
    if (!std::is_sorted(kDirtyRoster.begin(), kDirtyRoster.end()))
        std::sort(kDirtyRoster.begin(), kDirtyRoster.end());
    kDirtyRoster.erase(std::unique(kDirtyRoster.begin(), kDirtyRoster.end()), kDirtyRoster.end());
    size_t takenRoster = 0;
    const VkDeviceSize rosterBudget = kStageBytes / 4;
    for (size_t k = 0; k < kDirtyRoster.size();)
    {
        size_t e = k + 1;
        while (e < kDirtyRoster.size() && kDirtyRoster[e] == kDirtyRoster[e - 1] + 1)
            ++e;
        const size_t avail = (size_t)((rosterBudget - off) / 8);
        const size_t take = std::min(e - k, avail);
        if (take == 0)
            break;
        const VkDeviceSize bytes = 8ull * take;
        memcpy(stage + off, &kRosterWords[2ull * kDirtyRoster[k]], (size_t)bytes);
        toK.push_back({off, 8ull * kDirtyRoster[k], bytes});
        off += bytes;
        k += take;
        takenRoster = k;
        if (k < e)
            break; // the budget ran out inside this run
    }
    kDirtyRoster.erase(kDirtyRoster.begin(), kDirtyRoster.begin() + (ptrdiff_t)takenRoster);
    // A record as the GPU reads it, its phases at the CURRENT orbit epoch (as sat_rebake.comp would make them).
    const double T = (double)orbitEpochDay * 86400.0 + orbitEpochSec;
    auto gpuRec = [&](uint32_t j) {
        const kessler::PoolRec &r = kPool[j];
        auto ph = [&](float x0, float rate) {
            double v = std::fmod((double)x0 + (double)rate * T, glm::two_pi<double>());
            return (float)(v < 0.0 ? v + glm::two_pi<double>() : v);
        };
        GpuKepOrbit g{};
        g.M = ph(r.M0, r.n);
        g.argp = ph(r.argp0, r.argpDot);
        g.raan = ph(r.raan0, r.raanDot);
        g.n = r.n;
        g.a = (float)r.a;
        g.e = (float)r.e;
        g.cosI = (float)std::cos(r.incl);
        g.sinI = (float)std::sin(r.incl);
        g.argpDot = r.argpDot;
        g.raanDot = r.raanDot;
        g.tumbleRate = r.tumbleRate;
        g.tumblePhase = ph(r.tumble0, r.tumbleRate);
        g.flux = r.flux;
        g.bornSec = r.bornSec;
        g.deathSec = r.deathSec;
        g.typeConst = (uint32_t)r.type | ((uint32_t)r.constIdx << 16) | ((uint32_t)(r.flags & kessler::kRecDead) << 24);
        return g;
    };
    // Deaths of records already up.
    std::sort(kDirtyPool.begin(), kDirtyPool.end());
    kDirtyPool.erase(std::unique(kDirtyPool.begin(), kDirtyPool.end()), kDirtyPool.end());
    size_t takenPool = 0;
    for (; takenPool < kDirtyPool.size() && off + 64 <= kStageBytes / 2; ++takenPool)
    {
        const uint32_t j = kDirtyPool[takenPool];
        const GpuKepOrbit g = gpuRec(j);
        memcpy(stage + off, &g, 64);
        toK.push_back({off, 4ull * kPoolWord0 + 64ull * j, 64});
        off += 64;
    }
    kDirtyPool.erase(kDirtyPool.begin(), kDirtyPool.begin() + (ptrdiff_t)takenPool);
    // New records (and their J2000 phases), as many as the staging holds.
    // At most 16k new records a frame (~1 ms): building one costs a few fmods and a sin / cos.
    const size_t room = (size_t)((kStageBytes - off) / 80);
    const size_t n = std::min({newRecs, room, (size_t)(kPoolCap - kPoolGpu), (size_t)16384});
    if (n > 0)
    {
        const VkDeviceSize recOff = off;
        for (size_t k = 0; k < n; ++k)
        {
            const GpuKepOrbit g = gpuRec(kPoolGpu + (uint32_t)k);
            memcpy(stage + off + 64 * k, &g, 64);
        }
        off += 64 * n;
        toK.push_back({recOff, 4ull * kPoolWord0 + 64ull * kPoolGpu, 64ull * n});
        const VkDeviceSize baseOff = off;
        for (size_t k = 0; k < n; ++k)
        {
            const kessler::PoolRec &r = kPool[kPoolGpu + k];
            const float b[4] = {r.M0, r.argp0, r.raan0, r.tumble0};
            memcpy(stage + off + 16 * k, b, 16);
        }
        off += 16 * n;
        toB.push_back({baseOff, 16ull * kPoolGpu, 16ull * n});
        kPoolGpu += (uint32_t)n;
    }
    if (!toK.empty())
        vkCmdCopyBuffer(cmd, kStageBuf, kesslerBuf, (uint32_t)toK.size(), toK.data());
    if (!toB.empty())
        vkCmdCopyBuffer(cmd, kStageBuf, kPoolBaseBuf, (uint32_t)toB.size(), toB.data());
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
}

// ── The player's breakups ───────────────────────────────────────────────────────────────────────
bool SatelliteSim::kesslerUserEvent(uint8_t kind, uint32_t satA, uint32_t satB, std::string *why)
{
    if (!kesslerEnabled || !kWorker || !ctx_)
    {
        if (why)
            *why = "the Kessler scenario is off";
        return false;
    }
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    if (t < kFuryArriveT)
    {
        // Before the storm there is no world yet to break things in (its seconds start at the CME).
        if (why)
            *why = "breakups start with the scenario (after the CME arrives, 2037-07-15 03:00 UTC)";
        return false;
    }
    if (t > kFrontier + 1.0)
    {
        if (why)
            *why = "the cascade is still being computed up to here";
        return false;
    }
    kessler::Event ev;
    ev.t = t;
    ev.kind = kind;
    ev.origin = kessler::ORIGIN_USER;
    ev.a = kesslerRefOfSat(satA);
    ev.b = satB == 0xFFFFFFFFu ? kessler::kNoRef : kesslerRefOfSat(satB);
    ev.seed = (uint32_t)(fnvv(fnvv(1469598103934665603ull, t), (uint64_t)kUserEvents.size()) & 0xFFFFFFFFu);
    if (ev.a == kessler::kNoRef || (kind == kessler::EV_COLLISION && ev.b == kessler::kNoRef))
    {
        if (why)
            *why = "no such satellite";
        return false;
    }
    // Whether the objects are still alive then is the world's to judge (apply() skips an event naming a
    // dead one): the restart replays everything up to it.
    kUserEvents.push_back(ev);
    kesslerRestart(*ctx_, kUserEvents, t);
    return true;
}

// ── Readouts ────────────────────────────────────────────────────────────────────────────────────
std::string SatelliteSim::satLabel(int idx) const
{
    if (idx < 0 || idx >= (int)satOrbits.size())
        return "?";
    char b[160];
    auto rosterName = [&](uint32_t i) {
        const SatOrbit &o = satOrbits[i];
        if (o.constIdx < constellations.size())
            snprintf(b, sizeof(b), "%s #%d", constellations[o.constIdx].name.c_str(),
                     (int)i - (int)constellations[o.constIdx].orbitStart);
        else
            snprintf(b, sizeof(b), "Satellite #%u", i);
        return std::string(b);
    };
    if (idx < (int)activeSatCount)
    {
        const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
        const int32_t sec = (int32_t)std::clamp(std::floor(t - kFuryArriveT), -2147483000.0, 2147483000.0);
        const bool isDead = 2ull * idx + 1 < kRosterWords.size() && sec >= kRosterWords[2ull * idx];
        return rosterName((uint32_t)idx) + (isDead ? " (dead)" : "");
    }
    const uint32_t j = (uint32_t)idx - activeSatCount;
    if (j >= kPool.size())
        return "?";
    const kessler::PoolRec &r = kPool[j];
    const uint32_t k = (r.event < kEvents.size()) ? j - kEvents[r.event].firstRec : j;
    snprintf(b, sizeof(b), "Debris %u.%u", r.event, k);
    return b;
}

std::string SatelliteSim::kesslerStatusLine() const
{
    if (!kesslerEnabled || kWorkerDone)
        return {};
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    if (t + 1.0 < kFrontier)
        return {};
    char b[128];
    snprintf(b, sizeof(b), "Computing the debris cascade: day %.0f of %.0f",
             (kFrontier - kFuryArriveT) / 86400.0, kesslerHorizonYears * 365.25);
    return b;
}

nlohmann::json SatelliteSim::kesslerStateJson() const
{
    nlohmann::json j;
    j["enabled"] = kesslerEnabled;
    j["epoch_j2000"] = kFuryArriveT;
    j["flare_j2000"] = kFuryFlareT;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    j["days_since_cme"] = (t - kFuryArriveT) / 86400.0;
    j["frontier_days"] = (kFrontier - kFuryArriveT) / 86400.0;
    j["done"] = kWorkerDone;
    j["events"] = kEvents.size();
    j["user_events"] = kUserEvents.size();
    j["records"] = kPool.size();
    j["records_gpu"] = kPoolGpu;
    j["capacity"] = kPoolCap;
    j["hash"] = cacheName(kHash);
    // Counts at the current time.
    const int32_t sec = (int32_t)std::clamp(std::floor(t - kFuryArriveT), -2147483000.0, 2147483000.0);
    size_t dead = 0, frag = 0, fragReal = 0, rosterGone = 0;
    for (const kessler::PoolRec &r : kPool)
        if (sec >= r.bornSec && sec < r.deathSec)
        {
            ++frag;
            fragReal += (size_t)std::llround(r.weight);
        }
    for (size_t i = 0; 2 * i + 1 < kRosterWords.size(); ++i)
    {
        if (sec >= kRosterWords[2 * i + 1])
            ++rosterGone;
        else if (sec >= kRosterWords[2 * i])
            ++dead;
    }
    size_t kills = 0, bumps = 0, col = 0, cat = 0, user = 0;
    for (const kessler::Event &e : kEvents)
    {
        if (e.t > t)
            continue;
        if (e.origin == kessler::ORIGIN_USER)
            ++user;
        switch (e.kind)
        {
        case kessler::EV_KILL: ++kills; break;
        case kessler::EV_BUMP: ++bumps; break;
        case kessler::EV_COLLISION: ++col; cat += e.catastrophic; break;
        default: break;
        }
    }
    j["now"] = {{"dead_satellites", dead},  {"fragments", frag},     {"fragments_real", fragReal},
                {"roster_gone", rosterGone}, {"storm_kills", kills}, {"bumps", bumps},
                {"collisions", col},        {"catastrophic", cat},   {"user_events", user},
                {"kp", spaceWeather_.kp}};
    return j;
}

// ── UI ──────────────────────────────────────────────────────────────────────────────────────────
// The banner while sim time is held at the simulated frontier (top centre, under the HUD's notices).
void SatelliteSim::buildKesslerBanner(UIRenderer &ui)
{
    (void)ui;
    const std::string s = kesslerStatusLine();
    if (s.empty())
        return;
    snprintf(kBannerBuf, sizeof(kBannerBuf), "%s", s.c_str());
    Clay_String str{false, (int32_t)strlen(kBannerBuf), kBannerBuf};
    CLAY(CLAY_ID("KesslerBanner"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                               .padding = {14, 14, 8, 8},
                                               .childAlignment = {.x = CLAY_ALIGN_X_CENTER}},
                                    .backgroundColor = Pal::toastInfo,
                                    .cornerRadius = CLAY_CORNER_RADIUS(6),
                                    .floating = {.offset = {0, 60},
                                                 .zIndex = 24,
                                                 .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_TOP,
                                                                  .parent = CLAY_ATTACH_POINT_CENTER_TOP},
                                                 .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                                                 .attachTo = CLAY_ATTACH_TO_ROOT}})
    {
        CLAY_TEXT(str, CLAY_TEXT_CONFIG({.textColor = Pal::textBright, .fontSize = fs(13)}));
    }
}

// Settings > Constellations: the scenario, its tunables (a change re-simulates from the storm) and what
// it has done by the current sim time.
void SatelliteSim::buildKesslerSettingsSection(const UIInput &inp, UIRenderer &ui)
{
    uiSection("ConstSec", 2, "THE SUN'S FURY");
    bool on = kesslerEnabled;
    if (uiToggleRow(inp, ui, "KesslerOn", 0, "Scenario (a storm on 2037-07-15)", on,
                    "A Carrington-class solar storm arrives two days after the Great Australian Eclipse. Satellites it "
                    "kills tumble, dead ones collide, and every collision is a debris cloud (NASA breakup model). "
                    "Nothing differs before it."))
    {
        kesslerEnabled = on;
        spaceWeatherSetScriptedStorm(kesslerEnabled, kFuryArriveT, kFuryPeakKp);
        if (ctx_)
            kesslerRestart(*ctx_, kUserEvents, -1.0);
    }
    static const char *kRate[] = {"x0.1", "x1", "x10", "x100"};
    static const float kRateV[] = {0.1f, 1.0f, 10.0f, 100.0f};
    int rateSel = 1;
    for (int i = 0; i < 4; ++i)
        if (std::abs(kesslerRateMult - kRateV[i]) < 1e-3f * kRateV[i])
            rateSel = i;
    const int r = uiChoiceRow(inp, ui, "KesslerRate", 0, "Collision rate", kRate, 4, rateSel,
                              "x1 is physical (the cube method: relative speed x cross-section x time / volume). "
                              "Higher is faster, not realistic. Re-simulates from the storm.");
    static const char *kAvoid[] = {"0%", "50%", "90%", "99%"};
    static const float kAvoidV[] = {0.0f, 0.5f, 0.9f, 0.99f};
    int avoidSel = 2;
    for (int i = 0; i < 4; ++i)
        if (std::abs(kesslerAvoidance - kAvoidV[i]) < 1e-3f)
            avoidSel = i;
    const int a = uiChoiceRow(inp, ui, "KesslerAvoid", 0, "Collision avoidance", kAvoid, 4, avoidSel,
                              "The share of encounters between a working satellite and a tracked object (a dead satellite, a "
                              "fragment) that it dodges. Dead satellites dodge nothing. Re-simulates from the storm.");
    static const char *kYears[] = {"5 y", "20 y", "50 y"};
    static const float kYearsV[] = {5.0f, 20.0f, 50.0f};
    int ySel = 1;
    for (int i = 0; i < 3; ++i)
        if (std::abs(kesslerHorizonYears - kYearsV[i]) < 0.01f)
            ySel = i;
    const int y = uiChoiceRow(inp, ui, "KesslerYears", 0, "Simulate up to", kYears, 3, ySel,
                              "How far after the storm the cascade is simulated (it is computed as sim time gets there).");
    if ((r >= 0 && r != rateSel) || (a >= 0 && a != avoidSel) || (y >= 0 && y != ySel))
    {
        if (r >= 0)
            kesslerRateMult = kRateV[r];
        if (a >= 0)
            kesslerAvoidance = kAvoidV[a];
        if (y >= 0)
            kesslerHorizonYears = kYearsV[y];
        if (ctx_)
            kesslerRestart(*ctx_, kUserEvents, -1.0);
    }
    if (!kesslerEnabled)
        return;
    const nlohmann::json j = kesslerStateJson();
    const nlohmann::json &n = j["now"];
    const float lw = (float)fs(170);
    const double days = j["days_since_cme"].get<double>();
    uiKV("KesslerKV", 0, "Since the storm", days < 0.0 ? uiKitStr("in %.1f days", -days) : uiKitStr("%.1f days", days), lw);
    uiKV("KesslerKV", 1, "Simulated to", uiKitStr("day %.1f%s", j["frontier_days"].get<double>(), kWorkerDone ? " (done)" : ""), lw);
    uiKV("KesslerKV", 2, "Dead satellites (tumbling)", uiKitStr("%d", n["dead_satellites"].get<int>()), lw);
    uiKV("KesslerKV", 3, "Destroyed / re-entered", uiKitStr("%d", n["roster_gone"].get<int>()), lw);
    uiKV("KesslerKV", 4, "Fragments >= 10 cm", uiKitStr("%d drawn of ~%d", n["fragments"].get<int>(), n["fragments_real"].get<int>()), lw);
    uiKV("KesslerKV", 5, "Collisions", uiKitStr("%d (%d catastrophic)", n["collisions"].get<int>(), n["catastrophic"].get<int>()), lw);
    if (!kUserEvents.empty())
    {
        uiKV("KesslerKV", 6, "Your breakups", uiKitStr("%d", (int)kUserEvents.size()), lw);
        if (uiButton(inp, ui, "KesslerClear", 0, "Undo my breakups", "Re-simulate without them"))
        {
            kUserEvents.clear();
            if (ctx_)
                kesslerRestart(*ctx_, kUserEvents, -1.0);
        }
    }
    if (kUiMsg[0])
        uiKV("KesslerKV", 7, "", kUiMsg, lw, &Pal::textDim);
}

// A player breakup from the info window (the viewed satellite).
void SatelliteSim::kesslerBreakupFromUi(int sat, uint8_t kind)
{
    std::string why;
    if (sat < 0)
        snprintf(kUiMsg, sizeof(kUiMsg), "No satellite to break up");
    else if (kesslerUserEvent(kind, (uint32_t)sat, 0xFFFFFFFFu, &why))
        snprintf(kUiMsg, sizeof(kUiMsg), "%s: %s", kind == kessler::EV_ASAT ? "ASAT strike" : "Explosion",
                 satLabel(sat).c_str());
    else
        snprintf(kUiMsg, sizeof(kUiMsg), "Not now: %s", why.c_str());
}
