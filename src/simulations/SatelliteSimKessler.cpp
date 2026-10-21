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
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif
#include <fstream>

namespace
{
// 2037-07-14 09:30 UTC the flare, 2037-07-15 03:00 UTC the CME's arrival (17.5 h: Carrington's transit).
constexpr double kFuryFlareT = 1184448600.0;
constexpr double kFuryArriveT = 1184511600.0;
constexpr double kFuryPeakKp = 11.0;
constexpr uint64_t kStormSeed = 0x5EEDF0221ull;
constexpr uint32_t kCacheMagic = 0x4B534C52u; // "RLSK"
constexpr uint32_t kCacheVersion = 15; // (15: the deployment era: roster born / gone seconds, 2026-10-20; 14: the GEO belt joins the roster, 2026-10-20; 13: an explosion's fragment count scales with a parent over 2 t) bump whenever the world's logic changes (4: round 2, large pieces + grid;
                                       // 12: steps on a fixed grid, seeded by their index; a step's events ordered)

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

// The process's working set (the benchmark's memory figure).
static double kesslerWorkingSetMB()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
#endif
    return 0.0;
}

// ── The worker ───────────────────────────────────────────────────────────────────────────────────
struct SatelliteSim::KesslerWorker
{
    kessler::World w;
    std::vector<kessler::Event> pending; // seeds + user + cached engine events, by time
    size_t pendingNext = 0;
    double replayUntil = 0.0; // engine sampling resumes here (the cache's frontier)
    double tEnd = 0.0;
    std::atomic<double> wantUntil{0.0}; // lazy: simulate only this far (the main thread moves it with sim time)
    std::atomic<double> simNow{0.0};    // the player's sim time: checkpoints are kept from just before it (rollback)
    std::thread th;
    std::mutex mx;
    bool stop = false;
    std::atomic<bool> done{false};
    // The outbox (under mx): what the main thread has not taken yet.
    size_t pubEvents = 0;
    // Pool changes in order: a record placed in a slot (with its generation) or a record's death changed.
    struct OutPoolChange
    {
        uint32_t slot, gen;
        int32_t deathSec;
        uint8_t isRecord;
        kessler::PoolRec rec;
    };
    std::vector<OutPoolChange> outPool;
    std::vector<kessler::Event> outEvents;
    std::vector<kessler::SmallStream> outStreams;
    struct RosterChange
    {
        uint32_t sat;
        int32_t dead, end, decay;
    };
    std::vector<RosterChange> outRoster;
    double outFrontier = 0.0;

    void publish()
    {
        std::lock_guard<std::mutex> lk(mx);
        for (const kessler::World::PoolChange &c : w.poolChanges)
        {
            // Every generation goes, in order (one that lived and was replaced between two publishes too: its
            // fireball and its stretch of the timeline need it).
            outPool.push_back({c.slot, c.gen, c.deathSec, c.isRecord, c.rec});
        }
        w.poolChanges.clear();
        outEvents.insert(outEvents.end(), w.events.begin() + (ptrdiff_t)pubEvents, w.events.end());
        pubEvents = w.events.size();
        outStreams.insert(outStreams.end(), w.smallStreams.begin(), w.smallStreams.end());
        w.smallStreams.clear();
        for (uint32_t i : w.rosterChanged)
            outRoster.push_back({i, w.rosterDead[i], w.rosterEnd[i], w.rosterDecay[i]});
        w.rosterChanged.clear();
        outFrontier = w.frontier;
    }

    void run()
    {
        kessler::lowerThreadPriority();
        const auto t0 = std::chrono::steady_clock::now();
        double lastLog = w.frontier;
        // Replay (no sampling) up to the cache's frontier, then simulate. A day of sim time per chunk,
        // published after each.
        while (!stop && w.frontier < tEnd)
        {
            keepCheckpoints();
            const double want = std::min(wantUntil.load(), tEnd);
            if (w.frontier >= want)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
            const bool sampling = w.frontier >= replayUntil;
            const double chunk = sampling ? 86400.0 : 30.0 * 86400.0;
            double to = std::min({w.frontier + chunk, tEnd, std::max(want, w.frontier + w.s.stepS)});
            // Chunks end on the step grid: the steps, and so the history, do not depend on when the worker paused.
            to = std::min(w.s.epoch + std::ceil((to - w.s.epoch) / w.s.stepS - 1e-9) * w.s.stepS, tEnd);
            if (!sampling)
                to = std::min(to, replayUntil);
            simulateChunk(to, sampling);
            publish();
            // Throughput log every 10 simulated days (the benchmark reads these).
            if (w.frontier - lastLog >= 10.0 * 86400.0)
            {
                lastLog = w.frontier;
                size_t live = 0;
                const int32_t sec = w.secOf(w.frontier);
                for (const kessler::PoolRec &r : w.pool)
                    live += (sec >= r.bornSec && sec < r.deathSec);
                char m[420];
                snprintf(m, sizeof(m),
                         "kessler: bench day %.0f wall %.1f s: %zu events, %zu records (%zu live), %zu death buckets, "
                         "%.3g small fragments (%llu re-entry streams), %.0f MB working set; engine refs %.1f states %.1f pairs %.1f "
                         "apply %.1f",
                         (w.frontier - w.s.epoch) / 86400.0,
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), w.events.size(),
                         w.pool.size(), live, (size_t)w.deathBuckets.size(), w.grid.total(), (unsigned long long)w.smallStreamsMade,
                         kesslerWorkingSetMB(), w.profRefs,
                         w.profState, w.profPairs, w.profApply);
                Log::line(m);
            }
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        char msg[200];
        snprintf(msg, sizeof(msg), "kessler: worker %s at %.0f days after the CME: %zu events, %zu records, %.1f s",
                 stop ? "stopped" : "done", (w.frontier - w.s.epoch) / 86400.0, w.events.size(), w.pool.size(), s);
        Log::line(msg);
        snprintf(msg, sizeof(msg), "kessler: engine time: refs %.1f s, states + grid %.1f s, pairs %.1f s, apply %.1f s",
                 w.profRefs, w.profState, w.profPairs, w.profApply);
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
            const double t1 = std::min(w.stepStart(w.frontier) + w.s.stepS, to);
            if (w.frontier == w.stepStart(w.frontier) && w.wantCheckpoints && w.frontier >= w.checkpointFrom)
                w.checkpoint(w.frontier, pendingNext);
            while (pendingNext < pending.size() && pending[pendingNext].t <= t1)
                w.apply(pending[pendingNext++]);
            ++w.sampleCount;
            w.frontier = t1;
        }
    }

    // Rollback checkpoints: one per step from a step before the player's time on; older ones go.
    void keepCheckpoints()
    {
        const double now = simNow.load();
        w.wantCheckpoints = true;
        w.checkpointFrom = w.stepStart(now) - w.s.stepS;
        w.dropCheckpointsBefore(now - w.s.stepS);
    }

    void startThread()
    {
        stop = false;
        done = false;
        th = std::thread([this] { run(); });
    }
    void stopThread()
    {
        {
            std::lock_guard<std::mutex> lk(mx);
            stop = true;
        }
        if (th.joinable())
            th.join();
        stop = false; // the main thread may step the world itself now (kesslerBreakupInPlace)
    }
};

void SatelliteSim::KesslerWorkerDelete::operator()(KesslerWorker *w) const { delete w; }

double SatelliteSim::kesslerEpoch() const { return kFuryArriveT; }

uint32_t SatelliteSim::kesslerRefOfSat(uint32_t sat) const
{
    if (sat < activeSatCount)
        return sat;
    if (sat < poolIdx0())
        return kessler::kNoRef; // a launch record (not in the world)
    const uint32_t j = sat - poolIdx0();
    return j < kPool.size() ? (j | kessler::kPoolBit) : kessler::kNoRef;
}

// ── Building the world ───────────────────────────────────────────────────────────────────────────
static void buildWorld(const SatelliteSim &sim, kessler::World &w, const std::vector<SatOrbit> &orbits,
                       uint32_t rosterCount, const std::vector<ConstellationConfig> &consts,
                       const std::vector<SatelliteType> &types, double raanAnchorT, double epoch, float rateMult,
                       float avoidance, float horizonYears, uint32_t debrisType, int debrisConst,
                       const int (&kit)[3][3])
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
        ti.sizeM = ty.sizeM > 0.0f ? (double)ty.sizeM : 2.0 * ti.radiusM;
        ti.arrayShare = ty.debrisArrayShare;
        ti.radiatorShare = ty.debrisRadiatorShare;
    }
    // The debris kit: which types the large pieces are drawn with, and the area each class snaps to.
    for (int k = 0; k < 3; ++k)
        for (int c = 0; c < 3; ++c)
        {
            const int t = kit[k][c];
            if (t < 0 || t >= (int)types.size() || types[t].meanAreaM2 <= 0.0f)
                continue;
            w.s.kitType[k][c] = t;
            w.s.kitArea[k][c] = types[t].meanAreaM2;
            w.types[t].kitKind = k;
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
    // The deployment era (2026-10-20): when each roster satellite is there (rosterLife: born, gone).
    w.rosterBorn.clear();
    w.rosterGone.clear();
    if (sim.rosterLifeTable().size() >= 2ull * rosterCount && rosterCount > 0)
    {
        w.rosterBorn.resize(rosterCount);
        w.rosterGone.resize(rosterCount);
        for (uint32_t i = 0; i < rosterCount; ++i)
        {
            w.rosterBorn[i] = sim.rosterLifeTable()[2ull * i];
            w.rosterGone[i] = sim.rosterLifeTable()[2ull * i + 1];
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
    h = fnvv(h, w.s.LcRecord);
    h = fnvv(h, w.s.dustShare);
    h = fnvv(h, w.s.clusterHitP);
    h = fnv(h, w.s.kitType, sizeof(w.s.kitType));
    h = fnv(h, w.s.kitArea, sizeof(w.s.kitArea));
    h = fnvv(h, kFuryArriveT);
    h = fnvv(h, kFuryPeakKp);
    {
        // The deployment era: who is there from the CME on (born seconds before it are all alike to the cached
        // engine; a gone second matters).
        uint64_t m = 0x9E3779B97F4A7C15ull;
        for (size_t i = 0; i < w.rosterBorn.size(); ++i)
        {
            const uint64_t v = ((uint64_t)(uint32_t)std::max(w.rosterBorn[i], -1) << 32) | (uint32_t)w.rosterGone[i];
            m = (m ^ (v + i * 0xC2B2AE3D27D4EB4Full)) * 0x100000001B3ull;
            m ^= m >> 29;
        }
        h = fnvv(h, m);
    }
    for (const kessler::TypeInfo &t : w.types)
    {
        h = fnvv(h, t.massKg);
        h = fnvv(h, t.areaM2);
        h = fnvv(h, t.passive);
        h = fnvv(h, t.sizeM);
        h = fnvv(h, t.arrayShare);
        h = fnvv(h, t.radiatorShare);
        h = fnvv(h, t.kitKind);
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
    // Keep the three newest caches (each set of settings has its own; one can be tens of MB).
    {
        std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> all;
        for (const auto &de : std::filesystem::directory_iterator(p.parent_path(), ec))
            if (de.path().extension() == ".bin" && de.path().filename().string().rfind("suns_fury_", 0) == 0)
                all.push_back({de.last_write_time(ec), de.path()});
        std::sort(all.begin(), all.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        for (size_t k = 3; k < all.size(); ++k)
            std::filesystem::remove(all[k].second, ec);
    }
    Log::line("kessler: cache " + p.filename().string() + (ec ? " NOT written: " + ec.message() : " written") + ", " +
              std::to_string(eng.size()) + " engine events, day " + std::to_string((int)((frontier - kFuryArriveT) / 86400.0)));
}

// ── Start / stop ────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::kesslerInit(VulkanContext &ctx)
{
    if (kFireBuf == VK_NULL_HANDLE)
    {
        ctx.createBuffer(16 + sizeof(GpuFireball) * kFireballMax, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, kFireBuf, kFireMem);
        vkMapMemory(ctx.device, kFireMem, 0, VK_WHOLE_SIZE, 0, &kFireMapped);
        memset(kFireMapped, 0, 16);
    }
    spaceWeatherSetScriptedStorm(kesslerEnabled, kFuryArriveT, kFuryPeakKp);
    kesslerRestart(ctx, kUserEvents, -1.0);
    kInitDone = true;
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
                                  double keepEngineBefore, double syncUntil)
{
    std::vector<kessler::Event> keptEngine;
    // The world starts at the CME, or at the step of the player's first breakup before it (2026-10-19).
    double startT = kFuryArriveT;
    {
        const double stepS = kessler::WorldSettings{}.stepS;
        for (const kessler::Event &e : userEvents)
            startT = std::min(startT, kFuryArriveT + std::floor((e.t - kFuryArriveT) / stepS) * stepS);
    }
    double keptFrontier = startT;
    if (kWorker)
    {
        kWorker->stopThread();
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
    kRosterDecay.assign(activeSatCount, kessler::kNever);
    kReentries.clear();
    kSmallStreams.clear();
    kSmallStreamCount = 0;
    kSmallReHours.clear();
    kPoolGen.clear();
    kPoolHist.clear();
    kGenBounds.clear();
    kShownSec = kHistPrunedUntil = kHistPrunedAt = INT32_MIN;
    kReplayPending = false;
    satOrbits.resize(poolIdx0()); // the roster and the launch block; the pool tail goes
    launchInitBlock();            // the launch block's entries (empty) and the launch state
    kPoolGpu = 0;
    kDirtyPool.clear();
    kDirtyRoster.clear();
    kRosterFullUpload = true;
    kFrontier = startT;
    kWorldStart = startT;
    kWorkerDone = false;
    if (selectedSatIndex >= (int)poolIdx0())
        selectedSatIndex = -1;
    if (followActive && followSatIndex >= (int)poolIdx0()) // its piece may not exist in the new history
        stopFollowHere("The debris you followed is being recomputed: you stay here.");
    if (!kesslerEnabled || activeSatCount == 0)
    {
        kWorkerDone = true;
        return;
    }

    std::unique_ptr<KesslerWorker, KesslerWorkerDelete> wk(new KesslerWorker());
    const double raanAnchorT = (double)simInitDayJ2000 * 86400.0 + simInitSecInDay;
    rosterLifeEnsure(); // the deployment era: who is there when (the world needs it)
    kLifeHashKey = rosterLifeKey;
    buildWorld(*this, wk->w, satOrbits, activeSatCount, constellations, satTypes, raanAnchorT, kFuryArriveT,
               kesslerRateMult, kesslerAvoidance, kesslerHorizonYears, kDebrisType, kDebrisConst, kDebrisKit);
    kHash = worldHash(wk->w);
    wk->w.s.startT = startT;
    wk->w.reset();
    wk->tEnd = kFuryArriveT + wk->w.s.horizonS;

    // The engine's events: those kept from the run being replaced, else the cache (user data, then the
    // shipped copy next to the exe), else none.
    double replayUntil = startT;
    if (keepEngineBefore >= 0.0)
        replayUntil = keptFrontier;
    else if (startT >= kFuryArriveT) // a pre-CME breakup changes everything the cache holds
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
    kessler::sortPending(pend, 0, wk->w);
    wk->pending = std::move(pend);
    wk->replayUntil = replayUntil;
    kCacheFrontier = keepEngineBefore >= 0.0 ? kFuryArriveT : replayUntil;
    const double tNow = (double)simDayJ2000 * 86400.0 + simSecInDay;
    wk->wantUntil = startT < kFuryArriveT ? tNow + 86400.0 : std::max(kFuryArriveT + 2.0 * 86400.0, tNow + 86400.0);
    wk->simNow = tNow;
    {
        char msg[200];
        snprintf(msg, sizeof(msg), "kessler: world %016llx: %u satellites, %zu pending events (replay to day %.1f)",
                 (unsigned long long)kHash, activeSatCount, wk->pending.size(), (replayUntil - kFuryArriveT) / 86400.0);
        Log::line(msg);
    }
    kesslerEnsureCapacity(ctx, std::max<size_t>(16384, kPoolCap));
    kWorker = std::move(wk);
    // A breakup in the world's first steps (one before the CME starts the world at its own step): computed here, so its
    // debris is up this frame.
    if (syncUntil > 0.0)
    {
        kessler::World &w = kWorker->w;
        const double to = w.stepStart(syncUntil) + w.s.stepS;
        if (to - w.frontier <= 2.0 * w.s.stepS)
        {
            const auto c0 = std::chrono::steady_clock::now();
            kWorker->keepCheckpoints();
            kWorker->simulateChunk(to, w.frontier >= kWorker->replayUntil);
            kWorker->publish();
            char msg[160];
            snprintf(msg, sizeof(msg), "kessler: breakup step computed in place (%.0f ms)",
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count());
            Log::line(msg);
        }
    }
    kWorker->startThread();
}

// ── Per frame ───────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::kesslerWant(double tJ2000)
{
    kWantFloor = std::max(kWantFloor, tJ2000);
    if (kWorker && tJ2000 > kWorker->wantUntil.load())
        kWorker->wantUntil = tJ2000;
}

void SatelliteSim::kesslerSync(VulkanContext &ctx)
{
    if (kRestartPending)
    {
        kRestartPending = false;
        spaceWeatherSetScriptedStorm(kesslerEnabled, kFuryArriveT, kFuryPeakKp);
        kesslerRestart(ctx, kUserEvents, -1.0);
    }
    if (kReplayPending && kWorker)
    {
        // Back past the retained generations of reused slots: rebuild the world by replaying its events (no sampling)
        // up to the current time.
        kReplayPending = false;
        Log::line("kessler: time went back past the kept history of reused slots: replaying the cascade");
        kesslerRestart(ctx, kUserEvents, kFrontier);
    }
    if (!kWorker)
        return;
    kWorker->simNow = (double)simDayJ2000 * 86400.0 + simSecInDay;
    {
        // Lazy: keep the world a minute of the CURRENT time warp ahead of sim time (at least two days, and always the
        // storm's first two days), not the whole horizon at once. The target follows the warp DOWN too: it only grew,
        // so one burst at a fast warp (60 s x 1 week/s, capped at 400 days) left the worker computing hundreds of days
        // at full tilt long after the player was back at 1x — the late-cascade lag (issue 20261016_155342: frontier
        // day 342 at sim day 165). Explicit asks (harness waits) stay as a floor.
        const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
        const double ahead = std::clamp(60.0 * (double)kTimeScales[timeScaleIdx], 2.0 * 86400.0, 400.0 * 86400.0);
        const double want = std::max({kWorldStart < kFuryArriveT ? t + ahead : kFuryArriveT + 2.0 * 86400.0, t + ahead,
                                      kWantFloor});
        kWorker->wantUntil = want;
    }
    std::vector<KesslerWorker::OutPoolChange> nPool;
    std::vector<kessler::Event> nEv;
    std::vector<KesslerWorker::RosterChange> rd;
    std::vector<kessler::SmallStream> nStreams;
    {
        std::lock_guard<std::mutex> lk(kWorker->mx);
        nPool.swap(kWorker->outPool);
        nEv.swap(kWorker->outEvents);
        nStreams.swap(kWorker->outStreams);
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
    const double tNow = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const int32_t nowSec = (int32_t)std::clamp(std::floor(tNow - kFuryArriveT), -2147483000.0, 2147483000.0);
    bool grew = false;
    for (const KesslerWorker::OutPoolChange &c : nPool)
    {
        const uint32_t j = c.slot;
        if (c.isRecord)
        {
            if (j >= kPool.size() || kPoolGen[j] == UINT32_MAX)
            {
                // A new slot (or one skipped past earlier in this batch: a slot filled and reused before the next new
                // one was appended). Slots in between stay empty (never alive) until their record arrives.
                if (j >= kPool.size())
                {
                    kessler::PoolRec empty{};
                    empty.bornSec = kessler::kNever;
                    empty.deathSec = kessler::kNever;
                    empty.decaySec = kessler::kNever;
                    const size_t old = kPool.size();
                    kPool.resize(j + 1, empty);
                    kPoolGen.resize(j + 1, UINT32_MAX);
                    satOrbits.resize(poolIdx0() + j + 1);
                    for (size_t q = old; q < j; ++q)
                        kesslerSetPoolOrbit((uint32_t)q);
                }
                kPool[j] = c.rec;
                kPoolGen[j] = c.gen;
                kesslerSetPoolOrbit(j);
                if (j < kPoolGpu)
                    kDirtyPool.push_back(j);
                grew = true;
            }
            else
            {
                // A reused slot: a later generation. Keep the earlier ones (the window) and show the right one now.
                std::vector<KGen> &hist = kPoolHist[j];
                if (hist.empty())
                    hist.push_back({kPoolGen[j], kPool[j]}); // without history the shown record is the latest
                hist.push_back({c.gen, c.rec});
                kGenBounds.insert({c.rec.bornSec, j});
                if (selectedSatIndex == (int)(poolIdx0() + j) && c.rec.bornSec <= nowSec)
                    selectedSatIndex = -1; // that piece is gone; its slot holds another now
                kesslerShowGen(j, nowSec);
            }
            if (c.rec.decaySec != kessler::kNever)
                kesslerIndexReentry(poolIdx0() + j, c.rec.decaySec);
        }
        else if (j < kPool.size())
        {
            // A death: of the shown generation, or of one kept in the history.
            auto it = kPoolHist.find(j);
            if (it != kPoolHist.end())
                for (KGen &g : it->second)
                    if (g.gen == c.gen)
                        g.rec.deathSec = c.deathSec;
            if (kPoolGen[j] == c.gen)
            {
                kPool[j].deathSec = c.deathSec;
                if (j < kPoolGpu)
                    kDirtyPool.push_back(j);
            }
        }
    }
    if (grew)
    {
        satOrbits.reserve(poolIdx0() + kPool.size());
        kesslerEnsureCapacity(ctx, kPool.size());
    }
    // Sim time moved: slots whose hands changed between the shown second and now show the other generation.
    if (nowSec != kShownSec)
    {
        if (kShownSec != INT32_MIN)
        {
            const int32_t lo = std::min(kShownSec, nowSec), hi = std::max(kShownSec, nowSec);
            for (auto it = kGenBounds.upper_bound(lo); it != kGenBounds.end() && it->first <= hi; ++it)
                kesslerShowGen(it->second, nowSec);
        }
        kShownSec = nowSec;
        if (nowSec < kHistPrunedUntil)
            kReplayPending = true;
        // Once a simulated day: drop generations that ended more than kHistKeepS ago (a reversal beyond replays).
        if ((int64_t)nowSec - (int64_t)kHistPrunedAt > 86400)
        {
            kHistPrunedAt = nowSec;
            for (auto it = kPoolHist.begin(); it != kPoolHist.end();)
            {
                std::vector<KGen> &hist = it->second;
                size_t drop = 0;
                while (drop + 1 < hist.size() && hist[drop].rec.deathSec < nowSec - kHistKeepS &&
                       hist[drop + 1].rec.bornSec < nowSec - kHistKeepS)
                    ++drop;
                for (size_t d = 1; d <= drop; ++d)
                {
                    // hist[d] has no earlier generation any more: before its birth this slot cannot be shown.
                    kHistPrunedUntil = std::max(kHistPrunedUntil, hist[d].rec.bornSec);
                    auto range = kGenBounds.equal_range(hist[d].rec.bornSec);
                    for (auto b2 = range.first; b2 != range.second; ++b2)
                        if (b2->second == it->first)
                        {
                            kGenBounds.erase(b2);
                            break;
                        }
                }
                if (drop > 0)
                    hist.erase(hist.begin(), hist.begin() + (ptrdiff_t)drop);
                if (hist.size() <= 1)
                    it = kPoolHist.erase(it);
                else
                    ++it;
            }
        }
    }
    for (const auto &d : rd)
        if (2ull * d.sat + 1 < kRosterWords.size())
        {
            kRosterWords[2 * d.sat] = d.dead;
            kRosterWords[2 * d.sat + 1] = d.end;
            if (d.sat < kRosterDecay.size())
                kRosterDecay[d.sat] = d.decay;
            if (d.decay != kessler::kNever)
                kesslerIndexReentry(d.sat, d.decay);
            kDirtyRoster.push_back(d.sat);
        }
    kEvents.insert(kEvents.end(), nEv.begin(), nEv.end());
    if (!nStreams.empty())
        kesslerAddStreams(nStreams);
}

// ── Small fragments coming down (round 4) ──────────────────────────────────────────────────────────
void SatelliteSim::kesslerAddStreams(const std::vector<kessler::SmallStream> &v)
{
    for (const kessler::SmallStream &ss : v)
    {
        const int32_t w0 = ss.deathSec - ss.halfS();
        kSmallStreams[(w0 >= 0 ? w0 : w0 - 86399) / 86400].push_back(ss);
        // An hour already expanded that this stream reaches is out of date.
        for (auto it = kSmallReHours.begin(); it != kSmallReHours.end();)
            if ((int64_t)(it->first + 1) * 3600 > w0 && (int64_t)it->first * 3600 < (int64_t)ss.deathSec + ss.halfS())
                it = kSmallReHours.erase(it);
            else
                ++it;
    }
    kSmallStreamCount += v.size();
}

// The small re-entries in hour hr (seconds from the epoch): each stream overlapping it spreads its fragments evenly over
// its window, so the hour holds count x overlap / width of them (the fraction rounded by a hash), each at a hashed
// second, point of its orbit and RAAN spread (the fragments' planes drift apart under differential J2 over ~a month).
const std::vector<SatelliteSim::KSmallRe> &SatelliteSim::kesslerSmallReHour(int32_t hr)
{
    auto found = kSmallReHours.find(hr);
    if (found != kSmallReHours.end())
        return found->second;
    std::vector<KSmallRe> &out = kSmallReHours[hr];
    const int64_t h0 = (int64_t)hr * 3600, h1 = h0 + 3600;
    const int32_t dLast = (int32_t)((h1 >= 0 ? h1 : h1 - 86399) / 86400);
    for (int32_t d = dLast - 11; d <= dLast; ++d) // windows are at most 10 days wide
    {
        auto it = kSmallStreams.find(d);
        if (it == kSmallStreams.end())
            continue;
        for (const kessler::SmallStream &ss : it->second)
        {
            const int64_t w0 = (int64_t)ss.deathSec - ss.halfS(), w1 = (int64_t)ss.deathSec + ss.halfS();
            const int64_t o0 = std::max(w0, h0), o1 = std::min(w1, h1);
            if (o1 <= o0)
                continue;
            const double lambda = (double)ss.count * (double)(o1 - o0) / (double)(w1 - w0);
            const uint64_t sd = ((uint64_t)ss.seed() << 32) ^ (uint64_t)(uint32_t)hr;
            const int n = (int)std::min(std::floor(lambda + kessler::hash01(sd, 0, 1)), 4000.0);
            for (int i = 0; i < n; ++i)
            {
                KSmallRe r;
                r.sec = (int32_t)(o0 + (int64_t)std::floor(kessler::hash01(sd, (uint64_t)i + 1, 2) * (double)(o1 - o0)));
                const double age = std::max((double)(r.sec - ss.bornSec), 0.0);
                const double spread = kessler::kTwoPi * std::min(age / (30.0 * 86400.0), 1.0);
                r.raan = (float)std::fmod(ss.raan() + (double)ss.raanDot * age +
                                              spread * (kessler::hash01(sd, (uint64_t)i + 1, 3) - 0.5),
                                          kessler::kTwoPi);
                r.incl = (float)ss.incl();
                r.u = (float)(kessler::kTwoPi * kessler::hash01(sd, (uint64_t)i + 1, 4));
                r.massKg = (float)(ss.massKg() * std::exp(0.5 * kessler::hashNormal(sd, (uint64_t)i + 1, 5)));
                r.seed = (uint32_t)(sd * 0x9E3779B97F4A7C15ull >> 32) ^ (uint32_t)i * 0x85EBCA6Bu;
                out.push_back(r);
            }
        }
    }
    // By second: a frame takes its 200-s window by binary search (tens of thousands an hour late in the cascade).
    std::sort(out.begin(), out.end(), [](const KSmallRe &x, const KSmallRe &y) { return x.sec < y.sec || (x.sec == y.sec && x.seed < y.seed); });
    return out;
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
    const VkDeviceSize slots = visibleCapacity();
    ctx.createBuffer(sizeof(GpuSatVisible) * slots, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, satVisibleBuf, satVisibleMem);
    ctx.createBuffer(sizeof(uint32_t) * slots, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, satVisibleIdxBuf, satVisibleIdxMem);
    // Roster words: dead / end seconds (2 per satellite), then the deployment era's born / gone seconds (2 more,
    // rosterLife), then the launch block.
    kLaunchWord0 = (4u * std::max<uint32_t>(activeSatCount, 1) + 3u) & ~3u;
    kPoolWord0 = kLaunchWord0 + 16u * kLaunchCap;
    ++kesslerBufGen;
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
    // The launch records: all zero = hidden (born = death = 0) until SatelliteSimLaunches.cpp writes them.
    vkCmdFillBuffer(cmd, kesslerBuf, 4ull * kLaunchWord0, 64ull * kLaunchCap, 0u);
    ctx.endOneTimeCommands(cmd);
    vkDestroyBuffer(ctx.device, st, nullptr);
    vkFreeMemory(ctx.device, sm, nullptr);
    rosterLifeUpload(ctx); // the deployment era's born / gone words
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
    auto no = [&](const char *m) {
        if (why)
            *why = m;
        return false;
    };
    if (!ctx_ || activeSatCount == 0)
        return no("the simulation is not ready");
    auto isLaunchObj = [&](uint32_t s) { return s >= activeSatCount && s < poolIdx0(); };
    if (isLaunchObj(satA) || (kind == kessler::EV_COLLISION && isLaunchObj(satB)))
        return no("rockets, raising trains and retiring satellites cannot be broken up");
    if (satA >= satOrbits.size() || (kind == kessler::EV_COLLISION && satB >= satOrbits.size()))
        return no("no such satellite");
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const int32_t nowSec = (int32_t)std::clamp(std::floor(t - kFuryArriveT), -2147483000.0, 2147483000.0);
    // Already gone at this time (destroyed, re-entered; a debris slot not holding a piece now)?
    auto existsNow = [&](uint32_t s) {
        if (s < activeSatCount && rosterFateAt(s, t) >= 4) // the deployment era: not launched yet / retired
            return false;
        if (s < activeSatCount)
            return 2ull * s + 1 >= kRosterWords.size() || nowSec < kRosterWords[2ull * s + 1];
        const uint32_t j = s - poolIdx0();
        return j < kPool.size() && nowSec >= kPool[j].bornSec && nowSec < kPool[j].deathSec;
    };
    if (!existsNow(satA) || (kind == kessler::EV_COLLISION && !existsNow(satB)))
        return no("it no longer exists at this time");
    // A breakup starts the Kessler scenario (2026-10-19, the user: "if the user clicks the explode button, we presume
    // we want to start the Kessler situation"), at any time, before the CME too.
    if (!kesslerEnabled)
    {
        kesslerEnabled = true;
        spaceWeatherSetScriptedStorm(true, kFuryArriveT, kFuryPeakKp);
        kWorker.reset(); // a fresh world below (it cannot be running while disabled)
    }
    kessler::Event ev;
    ev.t = t;
    ev.kind = kind;
    ev.origin = kessler::ORIGIN_USER;
    ev.a = kesslerRefOfSat(satA);
    ev.b = satB == 0xFFFFFFFFu ? kessler::kNoRef : kesslerRefOfSat(satB);
    ev.seed = (uint32_t)(fnvv(fnvv(1469598103934665603ull, t), (uint64_t)kUserEvents.size()) & 0xFFFFFFFFu);
    if (ev.a == kessler::kNoRef || (kind == kessler::EV_COLLISION && ev.b == kessler::kNoRef))
        return no("no such satellite");
    kUserEvents.push_back(ev);
    const auto c0 = std::chrono::steady_clock::now();
    const bool fast = !kForceRebuildOnce && kesslerBreakupInPlace(ev);
    kForceRebuildOnce = false;
    if (!fast)
    {
        // No rollback point (a breakup before the world's start, time played back past the kept checkpoints, the
        // journal over its cap, or no world yet): rebuild the history with this breakup in it.
        kesslerRestart(*ctx_, kUserEvents, t, t);
        kesslerSync(*ctx_); // what the restart computed in place is up this frame
    }
    kLastBreakupMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
    kLastBreakupFast = fast;
    char msg[200];
    snprintf(msg, sizeof(msg), "kessler: player breakup at day %.3f: %s in %.0f ms", (t - kFuryArriveT) / 86400.0,
             fast ? "rolled back and re-ran its step" : "rebuilt the world", kLastBreakupMs);
    Log::line(msg);
    return true;
}

SatelliteSim::ExplodeResult SatelliteSim::explodeSatellite(int satIndex, BreakupKind kind)
{
    ExplodeResult r;
    if (satIndex < 0)
    {
        r.reason = "no satellite selected";
        return r;
    }
    if (satIndex < (int)activeSatCount)
    {
        // (2026-10-20) A slot whose satellite is not launched yet, or whose group is retired, is not there.
        const int fate = rosterFateAt((uint32_t)satIndex, (double)simDayJ2000 * 86400.0 + simSecInDay);
        if (fate >= 4)
        {
            r.reason = std::string("that satellite is ") + rosterFateName(fate);
            return r;
        }
    }
    std::string why;
    r.ok = kesslerUserEvent(kind == BreakupKind::Asat ? kessler::EV_ASAT : kessler::EV_EXPLOSION, (uint32_t)satIndex,
                            0xFFFFFFFFu, &why);
    r.reason = r.ok ? (kind == BreakupKind::Asat ? "ASAT strike: " : "Explosion: ") + satLabel(satIndex) : why;
    return r;
}

// The fast path (2026-10-19): rewind the running world to its last checkpoint at or before the breakup's time, re-run
// up to the end of the breakup's step here (the debris is up this frame), rewind the main thread's mirror to the
// same moment, and let the worker carry on from there. False when there is no checkpoint to rewind to.
bool SatelliteSim::kesslerBreakupInPlace(const kessler::Event &ev)
{
    if (!kWorker || !ctx_)
        return false;
    KesslerWorker &W = *kWorker;
    W.stopThread();
    kessler::World &w = W.w;
    const double t = ev.t;
    const double stepEnd = w.stepStart(t) + w.s.stepS;
    if (t < w.s.startT)
    {
        W.startThread();
        return false;
    }
    if (w.frontier > w.stepStart(t))
    {
        const int k = w.checkpointAtOrBefore(t);
        if (k < 0)
        {
            W.startThread();
            return false;
        }
        const kessler::World::Checkpoint ck = w.checkpoints[(size_t)k];
        // The engine's own events since the checkpoint, up to the end of the breakup's step: replayed as they were
        // (the past does not change; the engine samples the next step with the debris in it).
        std::vector<kessler::Event> eng;
        for (size_t i = ck.events; i < w.events.size(); ++i)
            if (w.events[i].origin == kessler::ORIGIN_ENGINE && w.events[i].t < stepEnd)
                eng.push_back(w.events[i]);
        // The mirror's events up to the checkpoint (identical in both histories): what was published, and what the
        // worker published that the main thread has not taken yet. Its unsynced records and roster changes are taken
        // from the rewound world itself (kesslerMirrorRewind); its small-fragment streams from before the checkpoint
        // are kept.
        {
            std::lock_guard<std::mutex> lk(W.mx);
            kEvents.insert(kEvents.end(), W.outEvents.begin(), W.outEvents.end());
            std::vector<kessler::SmallStream> keep;
            const int32_t ckSec = w.secOf(ck.t);
            for (const kessler::SmallStream &ss : W.outStreams)
                if (ss.bornSec < ckSec)
                    keep.push_back(ss);
            if (!keep.empty())
                kesslerAddStreams(keep);
            W.outEvents.clear();
            W.outPool.clear();
            W.outStreams.clear();
            W.outRoster.clear();
        }
        W.pendingNext = w.rollback(k);
        // The pending tail: no engine events (those past the breakup's step are another future now), then the kept ones.
        std::vector<kessler::Event> &P = W.pending;
        P.erase(std::remove_if(P.begin() + (ptrdiff_t)W.pendingNext, P.end(),
                               [](const kessler::Event &e) { return e.origin == kessler::ORIGIN_ENGINE; }),
                P.end());
        P.insert(P.end(), eng.begin(), eng.end());
        P.push_back(ev);
        kessler::sortPending(P, W.pendingNext, w);
        W.replayUntil = stepEnd;
        W.pubEvents = w.events.size();
        if (kEvents.size() >= w.events.size())
            kEvents.resize(w.events.size());
        else
            kEvents.assign(w.events.begin(), w.events.end());
        kesslerMirrorRewind(ck.t);
    }
    else
    {
        // The world has not reached the breakup's step (time ran a little past the frontier): add it; engine events
        // (a cache being replayed) past its step are another future now.
        std::vector<kessler::Event> &P = W.pending;
        P.erase(std::remove_if(P.begin() + (ptrdiff_t)W.pendingNext, P.end(),
                               [&](const kessler::Event &e) { return e.origin == kessler::ORIGIN_ENGINE && e.t >= stepEnd; }),
                P.end());
        P.push_back(ev);
        kessler::sortPending(P, W.pendingNext, w);
        W.replayUntil = std::min(W.replayUntil, stepEnd);
    }
    // Up to the end of the breakup's step, here: replayed from its engine events (or sampled, if the world had not
    // got there).
    W.keepCheckpoints();
    if (w.frontier < stepEnd && w.frontier < W.replayUntil)
        W.simulateChunk(std::min(W.replayUntil, stepEnd), false);
    if (w.frontier < stepEnd)
        W.simulateChunk(stepEnd, true);
    W.publish();
    kFrontier = w.frontier;
    W.startThread();
    if (ctx_)
        kesslerSync(*ctx_); // the mirror takes the step's records now (the debris is drawn this frame)
    return true;
}

// The main thread's mirror rewound to the world's state at checkpoint time tCk (after World::rollback): every slot
// shows the world's generation, the history keeps only generations from before, the roster's seconds are the world's.
void SatelliteSim::kesslerMirrorRewind(double tCk)
{
    kessler::World &w = kWorker->w;
    const int32_t ckSec = w.secOf(tCk);
    const size_t n = w.pool.size();
    const size_t old = kPool.size();
    // Pool slots past the checkpoint's end: gone.
    for (size_t j = n; j < old; ++j)
        kPoolHist.erase((uint32_t)j);
    kPool.resize(n);
    kPoolGen.resize(n, UINT32_MAX);
    satOrbits.resize(poolIdx0() + n);
    for (uint32_t j = 0; j < (uint32_t)n; ++j)
    {
        const uint32_t g = w.poolGen[j];
        auto it = kPoolHist.find(j);
        if (it != kPoolHist.end())
        {
            std::vector<KGen> &h = it->second;
            h.erase(std::remove_if(h.begin(), h.end(), [&](const KGen &x) { return x.gen >= g; }), h.end());
            h.push_back({g, w.pool[j]});
            if (h.size() <= 1)
                kPoolHist.erase(it);
        }
        else if (j < old && kPoolGen[j] != UINT32_MAX && kPoolGen[j] < g && kPool[j].bornSec < ckSec)
            kPoolHist[j] = {{kPoolGen[j], kPool[j]}, {g, w.pool[j]}};
        const bool same = j < old && kPoolGen[j] == g && memcmp(&kPool[j], &w.pool[j], sizeof(kessler::PoolRec)) == 0;
        if (!same)
        {
            kPool[j] = w.pool[j];
            kPoolGen[j] = g;
            kesslerSetPoolOrbit(j);
            if (j < kPoolGpu)
                kDirtyPool.push_back(j);
            if (w.pool[j].decaySec != kessler::kNever)
                kesslerIndexReentry(poolIdx0() + j, w.pool[j].decaySec);
        }
    }
    kPoolGpu = std::min<uint32_t>(kPoolGpu, (uint32_t)n);
    kGenBounds.clear();
    for (const auto &kv : kPoolHist)
        for (size_t q = 1; q < kv.second.size(); ++q)
            kGenBounds.insert({kv.second[q].rec.bornSec, kv.first});
    kShownSec = INT32_MIN; // re-pick the shown generations at the next sync
    for (uint32_t i = 0; i < activeSatCount && i < w.rosterDead.size(); ++i)
    {
        if (2ull * i + 1 >= kRosterWords.size())
            break;
        if (kRosterWords[2ull * i] != w.rosterDead[i] || kRosterWords[2ull * i + 1] != w.rosterEnd[i] ||
            kRosterDecay[i] != w.rosterDecay[i])
        {
            kRosterWords[2ull * i] = w.rosterDead[i];
            kRosterWords[2ull * i + 1] = w.rosterEnd[i];
            kRosterDecay[i] = w.rosterDecay[i];
            kDirtyRoster.push_back(i);
            if (w.rosterDecay[i] != kessler::kNever)
                kesslerIndexReentry(i, w.rosterDecay[i]);
        }
    }
    // Small-fragment streams made since the checkpoint belong to the other future.
    kSmallStreamCount = 0;
    for (auto it = kSmallStreams.begin(); it != kSmallStreams.end();)
    {
        std::vector<kessler::SmallStream> &v = it->second;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const kessler::SmallStream &s) { return s.bornSec >= ckSec; }), v.end());
        kSmallStreamCount += v.size();
        it = v.empty() ? kSmallStreams.erase(it) : std::next(it);
    }
    kSmallReHours.clear();
    // A selection or follow of a piece that is no longer the same object.
    if (selectedSatIndex >= (int)poolIdx0() && (size_t)(selectedSatIndex - (int)poolIdx0()) >= n)
        selectedSatIndex = -1;
}


// ── The dust clouds (round 2) ───────────────────────────────────────────────────────────────────
// Each breakup of the last few hours leaves a cloud of fine dust and small fragments that follows its parent's
// orbit, expands at the fragments' spread speed and shears along-track (a fragment thrown forward climbs and falls
// behind, 3 x its kick per unit time). Drawn as a tube of Gaussian ellipsoids along the parent's orbit (the
// Clohessy-Wiltshire spread of the fragments' kicks: see below). cloud_march.comp integrates them.
//
// Brightness: a physical estimate (dustShare of the broken-up mass as ~100 um grains, 5.6 m^2/kg, plus the small
// fragments' area, albedo 0.25) x "Dust cloud brightness". Physically a cloud fades below the dark sky within
// minutes (it expands at hundreds of m/s); the default gain keeps it visible for an hour or so.
void SatelliteSim::kesslerFillDust(float pixelAngle)
{
    kDustCount = 0;
    kesslerDropGoneSelection(); // (every frame; cheap)
    if (!kDustMapped)
        return;
    uint32_t *hdr = static_cast<uint32_t *>(kDustMapped);
    hdr[0] = 0;
    hdr[1] = 0; // the engine lights (engineLightsFlush)
    GpuDustCloud *out = reinterpret_cast<GpuDustCloud *>(static_cast<char *>(kDustMapped) + 16);
    uint32_t n = 0;
    // Physical radiance (per unit solar irradiance, 1/sr) -> the sky's units, through the point sources' scale: a
    // cloud of magnitude m carries the on-screen energy of a star of magnitude m. Per full-res pixel of solid angle
    // Omega its light is effectFlare = kFluxToFlare x brightnessScale x L x Omega; the point style shows a flare of
    // flareRef (pointRefMag) as a peak of 1 over a Gaussian of ~1.27 px^2; the sky is shown x skyExposure (whose
    // global EV the point style takes as the same gain).
    const double pixFull = 0.5 * (double)pixelAngle;
    const double flareRef = 0.008 * std::pow(10.0, 0.4 * (6.0 - (double)pointRefMag));
    const double baseExp = (double)skyExposure() / std::exp2((double)globalExposureEV());
    const double toSky = satphot::kFluxToFlare * (double)brightnessScale * pixFull * pixFull /
                         (flareRef * 1.27 * std::max(baseExp, 1e-3));
    // The re-entry fireballs' trails go in the same list, after the dust (which keeps at most 3/4 of it), on every
    // return path.
    struct Finish
    {
        SatelliteSim *s;
        GpuDustCloud *out;
        uint32_t &n;
        uint32_t *hdr;
        double toSky, pixFull;
        ~Finish()
        {
            s->selTransients.clear();
            s->kesslerFillFireballs(out, n, toSky, pixFull);
            {
                // Booked as launches, not as the cascade (this runs inside the dust fill's kessler timer).
                const auto l0 = std::chrono::steady_clock::now();
                s->launchFillVisuals(out, n, toSky, pixFull); // rockets, flames, plumes, the retirees' re-entries
                s->engineLightsFlush(); // the burning engines' and the pads' light (dust buffer tail, header word 1)
                const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - l0).count();
                s->cpuAccumMs[CPU_LAUNCHES] += ms;
                s->cpuAccumMs[CPU_KESSLER] -= ms;
            }
            s->selTransientUpdate();
            hdr[0] = n;
            s->kDustCount = n;
        }
    } finish{this, out, n, hdr, toSky, pixFull};
    if (!kesslerEnabled || kEvents.empty() || kesslerDustGain <= 0.0f)
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    constexpr double kLife = 3.0 * 3600.0;
    if (t < kWorldStart)
        return;
    // Events are in time order to within one engine step (6 h): start the scan that far before the window.
    const double tFrom = t - kLife - 6.0 * 3600.0;
    size_t i0 = (size_t)(std::lower_bound(kEvents.begin(), kEvents.end(), tFrom,
                                          [](const kessler::Event &e, double x) { return e.t < x; }) -
                         kEvents.begin());
    struct Cand
    {
        double pri;
        size_t ev;
        glm::dvec3 r, v;
        double amp, age;
    };
    std::vector<Cand> cands;
    const glm::dvec3 eye = glm::dvec3(obsECI);
    for (size_t k = i0; k < kEvents.size(); ++k)
    {
        const kessler::Event &e = kEvents[k];
        // Stop past the window (+ the step's slack). The scan ran to the END of the log every frame: with the worker
        // 100+ days ahead, millions of future events a frame, 15-25 ms (issue 20261016_154921's "other").
        if (e.t > t + 6.0 * 3600.0)
            break;
        const double age = t - e.t;
        if (age < 0.0 || age > kLife || (e.dustKg <= 0.5f && e.nSmall < 50.0f))
            continue;
        // Fades out over the last 40% of its life (it vanished at 3 h, still bright at a high gain).
        const double ageK = 1.0 - glm::smoothstep(0.6 * kLife, kLife, age);
        // A big parent's cloud (a station: tens of tonnes of dust) is brighter, but not in proportion: the display gain
        // exists to lift the faint clouds of ordinary satellites, and at x100 a 420-t station's cloud blew out the
        // whole view (issue 20261019_231126). Linear up to ~5 t of dust at the default gain (a 100-t parent), then as
        // its square root.
        const double ampLin = 0.25 * (5.6 * (double)e.dustKg + 0.02 * (double)e.nSmall) * (double)kesslerDustGain;
        constexpr double kAmpKnee = 7.0e5;
        const double amp = ampLin / std::sqrt(1.0 + ampLin / kAmpKnee) * ageK;
        kessler::State s0{{e.pos[0], e.pos[1], e.pos[2]}, {e.vel[0], e.vel[1], e.vel[2]}};
        if (glm::length(s0.r) < 1.0)
            continue;
        kessler::Elems el = kessler::elemsFromState(s0);
        if (el.a <= 0.0 || el.e >= 0.99)
            continue;
        el.M += std::sqrt(kessler::kGM / (el.a * el.a * el.a)) * age;
        const kessler::State sc = kessler::stateFromElems(el);
        // Ranked by its nearest part: a cloud strung around its orbit is near the eye somewhere.
        const double rmC = glm::length(sc.r);
        const double nC = std::sqrt(kessler::kGM / (rmC * rmC * rmC));
        const double ntC = nC * age;
        const double ext = 3.5 * std::clamp((double)e.dvSmall, 80.0, 500.0) *
                           std::hypot(2.0 * (std::cos(ntC) - 1.0) / nC, 4.0 * std::sin(ntC) / nC - 3.0 * age);
        const double range = std::max(glm::length(sc.r - eye) - std::min(ext, 2.0 * rmC), 1.0e5);
        cands.push_back({amp / (range * range), k, sc.r, sc.v, amp, age});
    }
    if (cands.empty())
        return;
    std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.pri > b.pri; });
    const glm::dvec3 ex(eci2enuX), ey(eci2enuY), ez(eci2enuZ);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, ex), glm::dot(v, ey), glm::dot(v, ez)); };
    const uint32_t dustMax = kDustMax;
    // The cloud as a TUBE along its orbit (2026-10-20, issue 20261019_231126: a station's cloud was drawn as up to 12
    // straight Gaussians ~2000 km long, "beams" crossing the sky, and the count of them stepped with age (a ceil() of
    // the cloud's length), so its width jumped, and a hash per piece index reshuffled its lumps: "it stretches oddly
    // and glitches"). Now the fragments' spread follows the Clohessy-Wiltshire solution for velocity kicks dv ~ N(0,
    // sv^2 I) at the breakup (x radial, y along-track, z cross-track, n the mean motion):
    //   x = (sin nt / n) dvx + 2 (1 - cos nt) / n dvy
    //   y = 2 (cos nt - 1) / n dvx + (4 sin nt / n - 3 t) dvy
    //   z = (sin nt / n) dvz
    // The cloud is cut into pieces along y (arc length on the parent's orbit): each piece holds the fragments at that
    // y, Gaussian across with the conditional radial mean / spread given y, and cross-track sv |sin theta| / n, theta
    // = nt + y / r the angle those fragments travelled since the breakup (so the cloud pinches through the breakup
    // point and its antipode, where every fragment's orbit crosses, as real debris clouds do, and not at one time for
    // the whole cloud). Along y the pieces are Gaussians on a regular spacing D whose sum is the cloud's along-track
    // profile; D is a CONTINUOUS function of age (no counts that step): short enough that a piece's straight extent
    // stays inside the tube's width as the arc curves, and the pieces at the ends carry the profile's tails, so
    // pieces appear and leave with ~zero weight. Lumps: a smooth noise in the fragments' along-track kick (dvy given
    // y), which moves and stretches with them.
    struct Plan
    {
        double sy = 0.0;    // along-track profile sigma (m)
        double dWant = 0.0; // the spacing it asks for (m)
        double nWant = 0.0; // pieces at that spacing (real-valued: the budget share must not step)
    };
    auto cwParams = [](double nm, double tt, double &a, double &b) {
        const double nt = nm * tt;
        a = 2.0 * (std::cos(nt) - 1.0) / nm;
        b = 4.0 * std::sin(nt) / nm - 3.0 * tt;
    };
    auto plan = [&](const Cand &c) {
        Plan p;
        const kessler::Event &e = kEvents[c.ev];
        const double rm = glm::length(c.r);
        const double nm = std::sqrt(kessler::kGM / (rm * rm * rm));
        const double sv = std::clamp((double)e.dvSmall, 80.0, 500.0);
        double a, b;
        cwParams(nm, std::max(c.age, 1.0), a, b);
        p.sy = sv * std::sqrt(a * a + b * b) + 30.0;
        // The tube's typical half-width: sv |sin| / n, at most the ballistic sv t.
        const double w = std::max(sv * std::min(c.age, 0.7 / nm), 500.0);
        // A piece's straight extent (+-2 x 0.8 D) bows out from the arc by (1.6 D)^2 / 2r: keep that under w / 3.
        const double dBend = std::sqrt(2.0 * rm * w / 3.0) / 1.6;
        p.dWant = std::min(dBend, 0.5 * p.sy);
        p.nWant = 2.0 * 3.5 * p.sy / p.dWant + 1.0;
        return p;
    };
    // The budget: every cloud (by priority) gets at least kMinPieces; what is left is shared out in proportion to what
    // each wants beyond that. Clouds past the budget fade by priority (no popping as the view moves).
    constexpr int kMinPieces = 15;
    std::vector<Plan> plans(cands.size());
    size_t nFit = 0;
    double extra = 0.0;
    double priCut = 0.0;
    for (size_t q = 0; q < cands.size(); ++q)
    {
        if ((nFit + 1) * kMinPieces > dustMax)
        {
            priCut = cands[q].pri;
            break;
        }
        plans[q] = plan(cands[q]);
        extra += std::max(0.0, plans[q].nWant - kMinPieces);
        ++nFit;
    }
    const double share = extra > 0.0 ? std::min(1.0, ((double)dustMax - (double)(nFit * kMinPieces)) / extra) : 1.0;
    for (size_t q = 0; q < nFit && n < dustMax; ++q)
    {
        const Cand &c = cands[q];
        const Plan &P = plans[q];
        const kessler::Event &e = kEvents[c.ev];
        const double cutK = priCut > 0.0 ? glm::smoothstep(priCut, 3.0 * priCut, c.pri) : 1.0;
        if (cutK <= 0.0)
            break;
        const double rm = glm::length(c.r);
        const double nM = std::sqrt(kessler::kGM / (rm * rm * rm));
        const double sv = std::clamp((double)e.dvSmall, 80.0, 500.0);
        const double tA = std::max(c.age, 1.0);
        double ca, cb;
        cwParams(nM, tA, ca, cb);
        const double ab2 = ca * ca + cb * cb;
        // Pieces (continuous in age: the spacing grows when the budget binds, never by a step).
        const double nGot =
            std::min(P.nWant, kMinPieces + share * std::max(0.0, P.nWant - kMinPieces));
        const double D = std::max(P.dWant, 2.0 * 3.5 * P.sy / std::max(nGot - 1.0, 1.0));
        const double sp = 0.8 * D;                                                    // a piece's along-track sigma
        const double vPos = std::max(P.sy * P.sy - sp * sp, 0.25 * P.sy * P.sy); // the centres' spread (deconvolved)
        const int K = (int)std::floor(3.5 * P.sy / D);
        const glm::dvec3 R = c.r / rm;
        const glm::dvec3 W = glm::normalize(glm::cross(c.r, c.v));
        const glm::dvec3 S0 = glm::cross(W, R);
        // Floors: a pinch never closes below a few km (the kicks' tails, J2, the fragments' different shapes) nor below
        // what the straight pieces' bow from the arc would show; never under ~a half-res pixel (below).
        const double bow = (1.6 * D) * (1.6 * D) / (2.0 * rm);
        const double floorW = std::max(30.0 + 0.02 * sv * std::min(tA, 1.0 / nM), 0.5 * bow);
        // Lumps along the cloud: a smooth noise in u = the fragments' along-track kick / sv at that y (material).
        auto lump = [&](double u) {
            const double x = u * 2.5 + 0.37;
            const double fl = std::floor(x), fr = x - fl;
            const double s = fr * fr * (3.0 - 2.0 * fr);
            const double h0 = kessler::hash01(e.seed, (uint64_t)(int64_t)fl, 71);
            const double h1 = kessler::hash01(e.seed, (uint64_t)(int64_t)(fl + 1.0), 71);
            return 0.55 + 0.9 * (h0 + (h1 - h0) * s); // mean ~1
        };
        double wsum = 0.0;
        for (int k = -K; k <= K; ++k)
        {
            const double y = k * D;
            wsum += std::exp(-0.5 * y * y / vPos);
        }
        const double ampC = c.amp * cutK / std::max(wsum, 1e-30);
        const double sMinA = 0.6 * (double)pixelAngle; // the half-res pixel, x the range
        for (int k = -K; k <= K && n < dustMax; ++k)
        {
            const double y = k * D;
            double w = std::exp(-0.5 * y * y / vPos);
            if (w < 1e-4)
                continue;
            // The fragments at y: their kick dvy given y, and the angle they travelled since the breakup.
            w *= lump(cb * y / (sv * ab2)); // E[dvy | y] / sv
            const double th = nM * tA + y / rm;
            const double p = std::sin(th) / nM, qq = 2.0 * (1.0 - std::cos(th)) / nM;
            const double mx = (p * ca + qq * cb) * y / ab2; // the radial mean given y
            const double vx = sv * sv * (p * cb - qq * ca) * (p * cb - qq * ca) / ab2;
            const double vz = sv * sv * p * p;
            const glm::dvec3 Rk = std::cos(y / rm) * R + std::sin(y / rm) * S0;
            const glm::dvec3 ctr = (rm + mx) * Rk;
            const double rng = std::max(glm::length(ctr - eye), 1.0);
            const double sR = std::max(std::sqrt(vx + floorW * floorW), sMinA * rng);
            const double sW = std::max(std::sqrt(vz + floorW * floorW), sMinA * rng);
            const double sS = std::max(sp, sMinA * rng);
            const glm::dvec3 Sk = glm::cross(W, Rk);
            const glm::dvec3 a = toEnu(Rk), bq = toEnu(Sk), cw = toEnu(W);
            const double iR = 1.0 / (sR * sR), iS = 1.0 / (sS * sS), iW = 1.0 / (sW * sW);
            auto M = [&](int i, int j) { return iR * a[i] * a[j] + iS * bq[i] * bq[j] + iW * cw[i] * cw[j]; };
            GpuDustCloud &d = out[n++];
            const glm::dvec3 rel = toEnu(ctr - eye);
            // The directions it can reach (4 sigma): one dot product skips it on every other pixel.
            const double rad = 4.0 * std::max({sR, sS, sW});
            const double dist = glm::length(rel);
            d.cone = dist > 1.05 * rad
                         ? glm::vec4(glm::vec3(rel / dist), (float)std::sqrt(1.0 - (rad / dist) * (rad / dist)))
                         : glm::vec4(0.0f);
            d.c = glm::vec4(glm::vec3(rel), (float)(ampC * w * toSky));
            d.ia = glm::vec4((float)M(0, 0), (float)M(1, 1), (float)M(2, 2),
                             (float)(1.0 / (std::pow(2.0 * glm::pi<double>(), 1.5) * sR * sS * sW)));
            d.ib = glm::vec4((float)M(0, 1), (float)M(0, 2), (float)M(1, 2), 0.0f);
            d.col = glm::vec4(0.96f, 0.92f, 0.86f, 0.0f);
        }
    }
}


// ── Re-entry fireballs (round 3) ────────────────────────────────────────────────────────────────
// What a re-entry looks like (observations of Starlink, ATV-1, Hayabusa re-entries; ESA's re-entry campaigns): the
// object glows from ~100 km, brightening as the air thickens; its main breakup comes almost always at 75-80 km, a
// flash and then a train of many orange fragments, "some pieces leading the main body and others trailing it", each
// with its own glowing trail, flickering as they tumble, a few flaring as tanks or batteries burst; light pieces slow
// first and fall behind and below; nothing glows past ~45-50 km. Seen from the ground a pass crosses the sky in
// 20-60 s. Drawn at the second drag brings the object down (its re-entry), under where its orbit puts it then.
//
// The trajectory is in the frame of that point: up U0, along-track A (its horizontal velocity), cross C. Heads are
// point sprites (kessler_fireballs.comp appends them to the visible list: bloom and glare like any satellite), each
// fragment's trail and the main body's fading train emissive Gaussians in the dust list (cloud_march.comp).
namespace
{
constexpr double kFbV0 = 7600.0;      // entry speed (m/s)
constexpr double kFbH0 = 100000.0;    // glow starts (m)
constexpr double kFbHb = 78000.0;     // main breakup (m)
constexpr double kFbDur = 200.0;      // the whole event (s)
double fbBreakupT() { return (-150.0 + std::sqrt(150.0 * 150.0 + 4.0 * 1.5 * (kFbH0 - kFbHb))) / 3.0; } // ~81 s

struct FbFrag
{
    double s, h, lat;   // along-track distance (m), altitude (m), cross-track (m)
    double v;           // speed (m/s)
    double I;           // light relative to the main body at breakup (0 = dark)
    glm::vec3 col;
};

// The main body (k < 0) or fragment k at tau seconds after the glow starts.
// Fragment materials (each fragment hashes one): the temperature colour is tinted by what burns. Steel, soot and
// composites stay orange-red; aluminium burns bright white (AlO bands); copper (wiring, motors) green; titanium and
// magnesium blue-white sparks; sodium yellow-orange; lithium cells flare crimson. matK = the tint's strength.
FbFrag fbFrag(uint64_t seed, int k, int N, double tau, float matK = 0.0f)
{
    FbFrag f{};
    const double tb = fbBreakupT();
    if (k < 0 || tau < tb)
    {
        // Before the breakup: one body, brightening as it descends.
        const double t = std::min(tau, tb);
        f.s = kFbV0 * t - 2.0 * t * t;
        f.h = kFbH0 - 150.0 * t - 1.5 * t * t;
        f.v = kFbV0 - 4.0 * t;
        f.I = std::pow(10.0, -0.4 * 6.0 * (f.h - kFbHb) / (kFbH0 - kFbHb)); // 6 mag fainter at 100 km
        f.col = glm::vec3(1.0f, 0.86f, 0.62f);
        if (tau >= tb && k < 0)
            f.I = 0.0;
        return f;
    }
    const double tp = tau - tb;
    const double s0 = kFbV0 * tb - 2.0 * tb * tb;
    auto H = [&](int c) { return kessler::hash01(seed, (uint64_t)k * 16 + 7, (uint32_t)c); };
    // The remnant (k = 0) keeps its speed longest; light pieces slow within seconds.
    const double T = k == 0 ? 160.0 : 15.0 + 100.0 * H(1) * H(1);
    double dvA = (H(2) - 0.5) * 160.0;
    if (k > 0 && H(3) < 0.12)
        dvA += 150.0 + 200.0 * H(4); // a few lead the main body
    const double dvC = kessler::hashNormal(seed, (uint64_t)k * 16 + 7, 5) * 110.0;
    const double dvU = kessler::hashNormal(seed, (uint64_t)k * 16 + 7, 6) * 35.0;
    f.v = kFbV0 / (1.0 + tp / T);
    f.s = s0 + kFbV0 * T * std::log(1.0 + tp / T) + dvA * tp;
    f.h = kFbHb - 220.0 * tp - 1.5 * tp * tp - 400.0 * tp * (1.0 - f.v / kFbV0) + dvU * tp;
    f.lat = dvC * tp;
    // Its share of the light: the remnant 35%, the rest lognormal.
    // Together the fragments shine about as the body did (more surface, each ablating on its own).
    double w = k == 0 ? 0.35 : 1.0 / std::max(N - 1, 1) * std::exp(0.9 * kessler::hashNormal(seed, (uint64_t)k * 16 + 7, 8)) * 0.67;
    const double vr = f.v / kFbV0;
    const double dens = std::min(std::exp(-(f.h - kFbHb) / 7000.0), 3.0);
    double I = w * vr * vr * dens;
    const double sm1 = std::clamp((f.v - 1800.0) / 1500.0, 0.0, 1.0), sm2 = std::clamp((f.h - 42000.0) / 8000.0, 0.0, 1.0);
    I *= sm1 * sm1 * (3.0 - 2.0 * sm1) * sm2 * sm2 * (3.0 - 2.0 * sm2);
    // Tumbling flicker, and a quarter of them flare once (a tank, a battery).
    I *= 1.0 + 0.35 * std::sin(kessler::kTwoPi * (0.5 + 3.5 * H(9)) * tau + kessler::kTwoPi * H(10));
    if (H(11) < 0.25)
    {
        const double tf = 2.0 + 23.0 * H(12);
        I *= 1.0 + 4.0 * std::exp(-(tp - tf) * (tp - tf) / 0.15);
    }
    // The breakup flash.
    if (k == 0)
        I += 4.0 * std::exp(-tp / 0.5);
    f.I = std::max(I, 0.0);
    // Hot and fast: yellow-white; slowing: orange, then deep red-orange.
    const glm::vec3 hot(1.0f, 0.85f, 0.58f), orange(1.0f, 0.52f, 0.18f), red(1.0f, 0.33f, 0.10f);
    const float tv = (float)std::clamp(vr, 0.0, 1.0);
    f.col = tv > 0.7f ? glm::mix(orange, hot, (tv - 0.7f) / 0.3f) : glm::mix(red, orange, tv / 0.7f);
    if (H(13) < 0.2)
        f.col = glm::mix(f.col, hot, 0.5f);
    if (matK > 0.0f && (k > 0 || N <= 3)) // a big object's remnant (k 0) is the hull: temperature colour
    {
        const double m = H(14);
        glm::vec3 mc = f.col;
        float share = 0.75f;
        if (m < 0.38) // steel, soot, composites: the temperature colour
            share = 0.0f;
        else if (m < 0.63)
            mc = glm::vec3(1.0f, 0.96f, 0.88f); // aluminium
        else if (m < 0.75)
            mc = glm::vec3(0.42f, 1.0f, 0.52f); // copper
        else if (m < 0.83)
            mc = glm::vec3(0.78f, 0.88f, 1.0f); // titanium, magnesium
        else if (m < 0.93)
            mc = glm::vec3(1.0f, 0.74f, 0.28f); // sodium
        else
        {
            // A lithium cell: crimson, strongest while it flares.
            mc = glm::vec3(1.0f, 0.16f, 0.2f);
            const double base = std::max(w * vr * vr * dens, 1e-9);
            share = 0.55f + 0.4f * (float)std::clamp((f.I / base - 1.0) / 2.0, 0.0, 1.0);
        }
        f.col = glm::mix(f.col, mc, share * matK);
    }
    return f;
}
} // namespace

void SatelliteSim::kesslerIndexReentry(uint32_t sat, int32_t sec)
{
    if (sec == kessler::kNever)
        return;
    // By hour (floor: a breakup before the CME gives negative seconds).
    kReentries[(int32_t)std::floor(sec / 3600.0)].push_back({sat, sec});
}

// The CPU's SatOrbit for pool slot j (the shown generation).
void SatelliteSim::kesslerSetPoolOrbit(uint32_t j)
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
    o.tumbleAxis = glm::vec3(kessler::poolTumbleAxis(j));
    o.poolSlot = (int32_t)j;
    satOrbits[poolIdx0() + j] = o;
}

// Shows slot j's generation for second sec: the latest born by then (else its earliest, not yet born: hidden).
void SatelliteSim::kesslerShowGen(uint32_t j, int32_t sec)
{
    auto it = kPoolHist.find(j);
    if (it == kPoolHist.end() || it->second.empty())
        return;
    const KGen *pick = &it->second.front();
    for (const KGen &g : it->second)
        if (g.rec.bornSec <= sec)
            pick = &g;
    if (pick->gen == kPoolGen[j])
        return;
    kPool[j] = pick->rec;
    kPoolGen[j] = pick->gen;
    kesslerSetPoolOrbit(j);
    if (j < kPoolGpu)
        kDirtyPool.push_back(j);
}

// Slot j's generation that re-enters at decaySec (shown or kept), or null.
const kessler::PoolRec *SatelliteSim::kesslerPoolGenWithDecay(uint32_t j, int32_t decaySec) const
{
    if (j >= kPool.size())
        return nullptr;
    if (kPool[j].decaySec == decaySec)
        return &kPool[j];
    auto it = kPoolHist.find(j);
    if (it != kPoolHist.end())
        for (const KGen &g : it->second)
            if (g.rec.decaySec == decaySec)
                return &g.rec;
    return nullptr;
}

// One re-entry: the trajectory frame (up U0, along-track A), the main body's magnitude at breakup (m0, at 100 km),
// its fragment count and seed, tau seconds into the glow. Heads go to kFireBuf, trails to the dust list (out, n).
// Shared by the Kessler re-entries and the launches' (retirees, upper stages: SatelliteSimLaunches.cpp).
void SatelliteSim::fbDrawReentry(const glm::dvec3 &U0, const glm::dvec3 &A, double m0, int N, uint64_t seed, double tau,
                                 GpuDustCloud *out, uint32_t &n, double toSky, double pixFull, uint8_t tagKind,
                                 uint64_t tagA)
{
    if (!kFireMapped)
        return;
    GpuFireball *fo = reinterpret_cast<GpuFireball *>(static_cast<char *>(kFireMapped) + 16);
    const glm::dvec3 eye = glm::dvec3(obsECI);
    const glm::dvec3 ex(eci2enuX), ey(eci2enuY), ez(eci2enuZ);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, ex), glm::dot(v, ey), glm::dot(v, ez)); };
    const double R = satphot::kEarthRadiusM;
    const double dayness = glm::clamp(((double)sunDirENU.w + 0.1) / 0.3, 0.0, 1.0);
    const double K = satphot::kFluxToFlare * (double)brightnessScale;
    const glm::dvec3 C = glm::cross(A, U0);
    auto place = [&](const FbFrag &f) {
        const double a = f.s / R;
        return (R + f.h) * (std::cos(a) * U0 + std::sin(a) * A) + f.lat * C;
    };
    const bool broken = tau >= fbBreakupT();
    // The selected / followed object (or the one a selection or Go to waits for) is listed even where it is not drawn
    // (behind the Earth, too faint, in daylight): its stage's selection must not end because nobody sees it burn
    // (2026-10-20). Picking ignores a listing with no flare.
    const bool wanted = tagKind && transientWanted(tagKind, tagA);
    for (int k = broken ? 0 : -1; k < (broken ? N : 0); ++k)
    {
        const FbFrag f = fbFrag(seed, k, N, tau, kesslerFireColor);
        const bool mainBody = tagKind && k == (broken ? 0 : -1);
        if (f.I <= 1e-4 && !(wanted && mainBody))
            continue;
        const glm::dvec3 P = place(f);
        const glm::dvec3 rel = P - eye;
        const double range = glm::length(rel);
        // The Earth in the way?
        const double b = glm::dot(eye, rel / range), c = glm::dot(eye, eye) - R * R;
        const double disc = b * b - c;
        bool hidden = disc > 0.0 && (-b - std::sqrt(disc)) > 0.0 && (-b - std::sqrt(disc)) < range;
        if (hidden && !(wanted && mainBody))
            continue;
        // Magnitude at the eye: + distance, + the air between the eye and the fireball, x daylight. (A
        // 1 / sin(elevation) airmass clamped at 0.03 added 8 magnitudes to every fireball seen from above.)
        const glm::dvec3 dirE = toEnu(rel) / range;
        const double m = m0 - 2.5 * std::log10(f.I) + 5.0 * std::log10(range / 1.0e5) +
                         atmExtinctionMag(eye, rel / range, range, (double)extinctionCoeff);
        double flare = 0.008 * std::pow(10.0, 0.4 * (6.0 - m)) * (1.0 - 0.95 * dayness);
        if ((flare < 2e-4 || f.I <= 1e-4) && !(wanted && mainBody))
            continue;
        if (flare < 2e-4 || f.I <= 1e-4)
            hidden = true; // listed for the selection, not drawn
        if (hidden)
            flare = 0.0;
        if (!hidden && kFireCount < kFireballMax)
            fo[kFireCount++] = {glm::vec4(glm::vec3(dirE), (float)flare), glm::vec4(f.col, (float)range)};
        // The main body (the remnant once broken up) can be picked.
        if (tagKind && k == (broken ? 0 : -1))
        {
            SelTransient st;
            st.key = seed * 0x9E3779B97F4A7C15ull + tagKind;
            st.kind = tagKind;
            st.a = tagA;
            st.tau = tau;
            const glm::dvec3 P1 = place(fbFrag(seed, k, N, std::max(tau - 0.5, 0.0), kesslerFireColor));
            const glm::dvec3 P2 = place(fbFrag(seed, k, N, std::max(tau - 1.0, 0.0), kesslerFireColor));
            st.pos = P;
            st.vel = (P - P1) / 0.5;
            st.acc = (P - 2.0 * P1 + P2) / 0.25;
            if (tau < 1.0)
                st.acc = glm::dvec3(0.0);
            st.dirEnu = glm::vec3(dirE);
            st.flare = (float)flare;
            addSelTransient(st);
        }
        // Its trail: the path over the last 1.5 s (shorter as it slows), an emissive Gaussian along it.
        if (!hidden && n < kFireTrailMax && flare > 5e-3)
        {
            const FbFrag fp = fbFrag(seed, k, N, std::max(tau - 1.5, 0.0), kesslerFireColor);
            const glm::dvec3 Pp = place(fp);
            const glm::dvec3 axis = P - Pp;
            const double L = glm::length(axis);
            if (L > 1.0)
            {
                const glm::dvec3 a = toEnu(axis / L);
                glm::dvec3 b2 = glm::cross(a, glm::dvec3(0, 0, 1));
                b2 = glm::length(b2) > 1e-6 ? glm::normalize(b2) : glm::dvec3(1, 0, 0);
                const glm::dvec3 c2 = glm::cross(a, b2);
                const double sMin = 0.6 * range * 2.0 * pixFull;
                // Centred 0.55 L behind the head with sigma 0.28 L, so it ends AT the head (a Gaussian about the
                // path's midpoint with sigma 0.45 L reached ~0.4 L ahead of it: issue 20261018_205438).
                const double sA = std::max(0.28 * L, sMin), sP = std::max(150.0, sMin);
                const double iA = 1.0 / (sA * sA), iP = 1.0 / (sP * sP);
                auto M = [&](int p, int q) { return iA * a[p] * a[q] + iP * (b2[p] * b2[q] + c2[p] * c2[q]); };
                // Intensity (photometry units) from the flare: flare = K J / r^2.
                const double J = 0.7 * flare * range * range / K;
                GpuDustCloud &d = out[n++];
            d.cone = glm::vec4(0.0f); // no view cone: tested everywhere
                d.c = glm::vec4(glm::vec3(toEnu(P - 0.55 * axis - eye)), (float)(J * toSky));
                d.ia = glm::vec4((float)M(0, 0), (float)M(1, 1), (float)M(2, 2),
                                 (float)(1.0 / (std::pow(2.0 * glm::pi<double>(), 1.5) * sA * sP * sP)));
                d.ib = glm::vec4((float)M(0, 1), (float)M(0, 2), (float)M(1, 2), 0.0f);
                d.col = glm::vec4(glm::mix(f.col, glm::vec3(1.0f, 0.35f, 0.12f), 0.25f), 1.0f); // emissive, the material's tint
                ++kFireTrails;
            }
        }
    }
    // The main body's train: its glowing path, fading over ~10 s and spreading.
    for (int j = 1; j <= 6 && n < kFireTrailMax; ++j)
    {
        const double tj = std::min(tau, fbBreakupT()) - 3.0 * j;
        if (tj <= 0.0)
            break;
        const double age = tau - tj;
        const FbFrag a0 = fbFrag(seed, -1, N, tj), a1 = fbFrag(seed, -1, N, tj + 3.0);
        const glm::dvec3 P0 = place(a0), P1 = place(a1);
        const glm::dvec3 rel = 0.5 * (P0 + P1) - eye;
        const double range = glm::length(rel);
        const double mT = m0 - 2.5 * std::log10(std::max(a0.I, 1e-6)) + 5.0 * std::log10(range / 1.0e5) + 2.5 +
                          atmExtinctionMag(eye, rel / range, range, (double)extinctionCoeff);
        const double flare = 0.008 * std::pow(10.0, 0.4 * (6.0 - mT)) * std::exp(-age / 8.0) * (1.0 - 0.95 * dayness);
        if (flare < 1e-3)
            continue;
        const glm::dvec3 axis = P1 - P0;
        const double L = glm::length(axis);
        const glm::dvec3 a = toEnu(axis / std::max(L, 1.0));
        glm::dvec3 b2 = glm::cross(a, glm::dvec3(0, 0, 1));
        b2 = glm::length(b2) > 1e-6 ? glm::normalize(b2) : glm::dvec3(1, 0, 0);
        const glm::dvec3 c2 = glm::cross(a, b2);
        const double sMin = 0.6 * range * 2.0 * pixFull;
        const double sA = std::max(0.3 * L, sMin), sP = std::max(200.0 + 40.0 * age, sMin);
        const double iA = 1.0 / (sA * sA), iP = 1.0 / (sP * sP);
        auto M = [&](int p, int q) { return iA * a[p] * a[q] + iP * (b2[p] * b2[q] + c2[p] * c2[q]); };
        const double J = flare * range * range / K;
        GpuDustCloud &d = out[n++];
            d.cone = glm::vec4(0.0f); // no view cone: tested everywhere
        d.c = glm::vec4(glm::vec3(toEnu(rel)), (float)(J * toSky));
        d.ia = glm::vec4((float)M(0, 0), (float)M(1, 1), (float)M(2, 2),
                         (float)(1.0 / (std::pow(2.0 * glm::pi<double>(), 1.5) * sA * sP * sP)));
        d.ib = glm::vec4((float)M(0, 1), (float)M(0, 2), (float)M(1, 2), 0.0f);
        d.col = glm::vec4(1.0f, 0.55f, 0.25f, 1.0f);
        ++kFireTrails;
    }
}

void SatelliteSim::kesslerFillFireballs(GpuDustCloud *out, uint32_t &n, double toSky, double pixFull)
{
    kFireCount = 0;
    kFireTrails = 0;
    kReentriesNow = 0;
    if (!kFireMapped)
        return;
    uint32_t *fh = static_cast<uint32_t *>(kFireMapped);
    fh[0] = 0;
    if (!kesslerEnabled || kReentries.empty())
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const int32_t nowSec = (int32_t)std::floor(t - kFuryArriveT); // negative before the CME (a player's breakup)
    if (t < kWorldStart)
        return;
    GpuFireball *fo = reinterpret_cast<GpuFireball *>(static_cast<char *>(kFireMapped) + 16);
    const glm::dvec3 eye = glm::dvec3(obsECI);
    const glm::dvec3 ex(eci2enuX), ey(eci2enuY), ez(eci2enuZ);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, ex), glm::dot(v, ey), glm::dot(v, ez)); };
    const double R = satphot::kEarthRadiusM;
    const double dayness = glm::clamp(((double)sunDirENU.w + 0.1) / 0.3, 0.0, 1.0);
    const double K = satphot::kFluxToFlare * (double)brightnessScale;
    auto drawReentry = [&](const glm::dvec3 &U0, const glm::dvec3 &A, double m0, int N, uint64_t seed, double tau,
                           uint8_t kind = 6, uint64_t a = 0) {
        fbDrawReentry(U0, A, m0, N, seed, tau, out, n, toSky, pixFull, kind, a);
    };
    std::vector<uint32_t> seen;
    const auto hourOf = [](int64_t sec) { return (int32_t)std::floor((double)sec / 3600.0); };
    for (int32_t hr = hourOf((int64_t)nowSec - (int64_t)kFbDur); hr <= hourOf(nowSec); ++hr)
    {
        auto it = kReentries.find(hr);
        if (it == kReentries.end())
            continue;
        for (const auto &entry : it->second)
        {
            // Still a re-entry (a later collision may have ended it first), and now.
            const uint32_t sat = entry.first;
            int32_t dsec;
            double mass;
            const kessler::PoolRec *pr = nullptr;
            if (sat < activeSatCount)
            {
                if (2ull * sat + 1 >= kRosterWords.size() || sat >= kRosterDecay.size() ||
                    kRosterWords[2ull * sat + 1] != kRosterDecay[sat])
                    continue;
                dsec = kRosterDecay[sat];
                const SatOrbit &o = satOrbits[sat];
                mass = 800.0;
                if (o.typeIdx < satTypes.size())
                    mass = satTypes[o.typeIdx].massKg > 0.0f ? (double)satTypes[o.typeIdx].massKg
                                                             : defaultMassKg(satTypes[o.typeIdx].modelId, satTypes[o.typeIdx].name);
            }
            else
            {
                pr = kesslerPoolGenWithDecay(sat - poolIdx0(), entry.second);
                if (!pr || pr->deathSec != pr->decaySec)
                    continue;
                dsec = pr->decaySec;
                mass = pr->massKg;
            }
            const double tau = t - (kFuryArriveT + (double)dsec);
            if (tau < 0.0 || tau > kFbDur)
                continue;
            if (std::find(seen.begin(), seen.end(), sat) != seen.end())
                continue;
            seen.push_back(sat);
            const SatOrbitElems oe = pr ? kessler::poolOrbitElems(*pr, sat - poolIdx0()) : orbitElemsOf(satOrbits[sat]);
            ++kReentriesNow;
            // Where: under the orbit at the re-entry second.
            const SatOrbitState so = satOrbitStateAt(oe, kFuryArriveT + (double)dsec);
            const glm::dvec3 U0 = glm::normalize(so.posEci);
            if (glm::length(R * U0 - eye) > 3.0e6)
                continue; // well over the horizon
            glm::dvec3 A = so.velEci - glm::dot(so.velEci, U0) * U0;
            A = glm::length(A) > 1.0 ? glm::normalize(A) : glm::dvec3(1.0, 0.0, 0.0);
            // Brightness: the main body at breakup is magnitude m0 at 100 km (-7.5 for a 2.5-t Starmind, -4 for a
            // 100-kg piece: satellite re-entries are reported at -3 to -6 from a few hundred km). Fragments: 3 +
            // sqrt(mass) scaled, up to 80.
            const double m0 = -4.0 - 2.5 * std::log10(std::max(mass, 1.0) / 100.0);
            const int N = (int)std::clamp(3.0 + 40.0 * std::sqrt(std::max(mass, 1.0) / 2500.0), 3.0, 80.0);
            const uint64_t seed = (uint64_t)sat * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uint32_t)dsec;
            drawReentry(U0, A, m0, N, seed, tau, 5, sat);
        }
    }
    // The small fragments (10 cm - 1 m) coming down: meteor-like, 1-3 pieces, magnitude -1.5 for 1 kg at 100 km (a
    // 10-cm, 0.1-kg piece +1). Not past 1 h/s of warp (a fireball lasts a frame there, and each hour costs a scan).
    kSmallReNow = 0;
    kSmallReHourCount = 0;
    if (kesslerSmallReGain > 0.0f && (kTimeScales[timeScaleIdx] <= 3600.0f || timePaused))
    {
        const int32_t hr0 = (int32_t)std::floor((nowSec - kFbDur) / 3600.0), hr1 = (int32_t)std::floor(nowSec / 3600.0);
        for (auto it = kSmallReHours.begin(); it != kSmallReHours.end();)
            it = (it->first < hr0 - 1 || it->first > hr1 + 1) ? kSmallReHours.erase(it) : std::next(it);
        kSmallReHourCount = (uint32_t)kesslerSmallReHour(hr1).size();
        const double secNow = t - kFuryArriveT;
        for (int32_t hr = hr0; hr <= hr1; ++hr)
        {
            const std::vector<KSmallRe> &hv = kesslerSmallReHour(hr);
            auto first = std::lower_bound(hv.begin(), hv.end(), secNow - kFbDur - 1.0,
                                          [](const KSmallRe &x, double v) { return (double)x.sec < v; });
            for (auto itR = first; itR != hv.end() && (double)itR->sec <= secNow; ++itR)
            {
                const KSmallRe &r = *itR;
                const double tau = t - (kFuryArriveT + (double)r.sec);
                if (tau < 0.0 || tau > kFbDur)
                    continue;
                ++kSmallReNow;
                const double cu = std::cos(r.u), su = std::sin(r.u), cO = std::cos(r.raan), sO = std::sin(r.raan);
                const double ci = std::cos(r.incl), si = std::sin(r.incl);
                const glm::dvec3 U0(cu * cO - su * ci * sO, cu * sO + su * ci * cO, su * si);
                if (glm::length(R * U0 - eye) > 3.0e6)
                    continue;
                const glm::dvec3 A(-su * cO - cu * ci * sO, -su * sO + cu * ci * cO, cu * si); // prograde
                const double mass = std::max((double)r.massKg, 1e-3);
                const double m0 = -1.5 - 2.5 * std::log10(mass) - 2.5 * std::log10((double)kesslerSmallReGain);
                const int N = mass > 3.0 ? 3 : mass > 0.5 ? 2 : 1;
                drawReentry(U0, A, m0, N, (uint64_t)r.seed * 0x9E3779B97F4A7C15ull + 0x51ull, tau);
            }
        }
    }
    fh[0] = kFireCount;
}

// The next re-entry after now (within 6 h) whose breakup is in darkness, the heaviest (dead satellites first).
bool SatelliteSim::kesslerNextReentry(uint32_t &satOut, double &tOut, glm::dvec3 &breakupEci, glm::dvec3 &velEci) const
{
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const int32_t nowSec = (int32_t)std::floor(t - kFuryArriveT);
    double best = -1.0;
    // Dead satellites (the spectacle) over the next 48 h, else the heaviest piece over the next 6.
    for (int32_t hr = (int32_t)std::floor(nowSec / 3600.0); hr <= (int32_t)std::floor(nowSec / 3600.0) + 48; ++hr)
    {
        if (best >= 1.0e6 && hr > (int32_t)std::floor(nowSec / 3600.0) + 6)
            break;
        auto it = kReentries.find(hr);
        if (it == kReentries.end())
            continue;
        for (const auto &entry : it->second)
        {
            const uint32_t sat = entry.first;
            int32_t dsec;
            double mass;
            const kessler::PoolRec *pr = nullptr;
            if (sat < activeSatCount)
            {
                if (2ull * sat + 1 >= kRosterWords.size() || sat >= kRosterDecay.size() ||
                    kRosterWords[2ull * sat + 1] != kRosterDecay[sat])
                    continue;
                dsec = kRosterDecay[sat];
                mass = 1.0e6; // a satellite beats any piece
            }
            else
            {
                if (hr > std::max(nowSec, 0) / 3600 + 6)
                    continue;
                pr = kesslerPoolGenWithDecay(sat - poolIdx0(), entry.second);
                if (!pr || pr->deathSec != pr->decaySec)
                    continue;
                dsec = pr->decaySec;
                mass = pr->massKg;
            }
            if (dsec <= nowSec + 30)
                continue;
            const double td = kFuryArriveT + (double)dsec;
            const SatOrbitState so = satOrbitStateAt(pr ? kessler::poolOrbitElems(*pr, sat - poolIdx0())
                                                        : orbitElemsOf(satOrbits[sat]), td);
            const glm::dvec3 U0 = glm::normalize(so.posEci);
            if (glm::dot(U0, sunDirEciAt(td + fbBreakupT())) > -0.14) // sun above -8 deg there
                continue;
            const double score = mass - 1e-3 * (double)(dsec - nowSec); // the heaviest, then the soonest
            if (score > best)
            {
                best = score;
                satOut = sat;
                tOut = td;
                glm::dvec3 A = so.velEci - glm::dot(so.velEci, U0) * U0;
                A = glm::normalize(A);
                const double tb = fbBreakupT();
                const double sb = kFbV0 * tb - 2.0 * tb * tb, a = sb / satphot::kEarthRadiusM;
                breakupEci = (satphot::kEarthRadiusM + kFbHb) * (std::cos(a) * U0 + std::sin(a) * A);
                velEci = A;
            }
        }
    }
    return best >= 0.0;
}

// ── The fireball pass: kessler_fireballs.comp appends the heads to the finished visible list ─────────────────────
void SatelliteSim::kesslerCreateFireballPass(VulkanContext &ctx)
{
    VkDescriptorSetLayoutBinding b[5] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 5;
    li.pBindings = b;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &kFireDescLayout);
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    pi.maxSets = 1;
    vkCreateDescriptorPool(ctx.device, &pi, nullptr, &kFireDescPool);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = kFireDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &kFireDescLayout;
    vkAllocateDescriptorSets(ctx.device, &ai, &kFireDescSet);
    VkDescriptorBufferInfo vis{satVisibleBuf, 0, VK_WHOLE_SIZE}, idx{satVisibleIdxBuf, 0, VK_WHOLE_SIZE},
        lst{satListBuf, 0, VK_WHOLE_SIZE}, pst{pointStyleBuf, 0, VK_WHOLE_SIZE}, fire{kFireBuf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[5] = {};
    const VkDescriptorBufferInfo *infos[5] = {&vis, &idx, &lst, &pst, &fire};
    for (uint32_t i = 0; i < 5; ++i)
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, kFireDescSet, i, 0, 1,
                i == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, infos[i], nullptr};
    vkUpdateDescriptorSets(ctx.device, 5, w, 0, nullptr);
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &kFireDescLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(ctx.device, &pli, nullptr, &kFirePipeLayout);
    VkShaderModule mod = ctx.loadShader("shaders/kessler_fireballs.comp.spv");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
    ci.layout = kFirePipeLayout;
    if (vkCreateComputePipelines(ctx.device, ctx.pipelineCache, 1, &ci, nullptr, &kFirePipeline) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: failed to create kessler_fireballs pipeline");
    vkDestroyShaderModule(ctx.device, mod, nullptr);
}

void SatelliteSim::kesslerRecordFireballs(VkCommandBuffer cmd, VulkanContext &ctx)
{
    if (kFireCount == 0)
        return;
    if (kFirePipeline == VK_NULL_HANDLE)
        kesslerCreateFireballPass(ctx);
    VkBufferMemoryBarrier cb[2] = {};
    for (int i = 0; i < 2; ++i)
    {
        cb[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        cb[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        cb[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        cb[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        cb[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        cb[i].size = VK_WHOLE_SIZE;
    }
    cb[0].buffer = satVisibleBuf;
    cb[1].buffer = satListBuf;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2,
                         cb, 0, nullptr);
    const uint32_t pc[4] = {visibleCapacity(), kFireCount, 0, 0};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kFirePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kFirePipeLayout, 0, 1, &kFireDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, kFirePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc);
    vkCmdDispatch(cmd, (kFireCount + 63) / 64, 1, 1);
}

void SatelliteSim::kesslerDestroyFireballPass(VkDevice device)
{
    if (kFirePipeline)
        vkDestroyPipeline(device, kFirePipeline, nullptr);
    if (kFirePipeLayout)
        vkDestroyPipelineLayout(device, kFirePipeLayout, nullptr);
    if (kFireDescPool)
        vkDestroyDescriptorPool(device, kFireDescPool, nullptr);
    if (kFireDescLayout)
        vkDestroyDescriptorSetLayout(device, kFireDescLayout, nullptr);
    if (kFireBuf)
    {
        vkDestroyBuffer(device, kFireBuf, nullptr);
        vkFreeMemory(device, kFireMem, nullptr);
    }
    kFirePipeline = VK_NULL_HANDLE;
    kFirePipeLayout = VK_NULL_HANDLE;
    kFireDescPool = VK_NULL_HANDLE;
    kFireDescLayout = VK_NULL_HANDLE;
    kFireDescSet = VK_NULL_HANDLE;
    kFireBuf = VK_NULL_HANDLE;
    kFireMapped = nullptr;
}

bool SatelliteSim::kesslerGoToNextReentry(std::string &msg)
{
    uint32_t sat = 0;
    double td = 0.0;
    glm::dvec3 B(0.0), A(0.0);
    if (!kesslerEnabled || !kesslerNextReentry(sat, td, B, A))
    {
        msg = "no re-entry over a dark sky soon (satellites: 48 h, pieces: 6 h; the cascade may not be computed that far yet)";
        return false;
    }
    if (followActive)
        stopFollow();
    if (trackActive)
        stopTrack();
    const double tb = td + fbBreakupT();
    const double t0 = tb - 25.0;
    const double days = std::floor(t0 / 86400.0);
    simDayJ2000 = (int64_t)days;
    simSecInDay = t0 - days * 86400.0;
    trailClearPending = true;
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    // The observer: 120 km to the side of the track and 80 km back along it (the train crosses the sky ~30 deg up).
    const double R = satphot::kEarthRadiusM;
    const glm::dvec3 U = glm::normalize(B);
    const glm::dvec3 C = glm::normalize(glm::cross(A, U));
    const glm::dvec3 G = glm::normalize(U * R + C * 120000.0 - A * 80000.0);
    auto toEcef = [](const glm::dvec3 &v, double t) {
        const double era = earthRotationAngle(t), c = std::cos(era), s = std::sin(era);
        return glm::dvec3(v.x * c + v.y * s, -v.x * s + v.y * c, v.z);
    };
    const glm::dvec3 g = toEcef(G, t0);
    setObserverLatLon(glm::degrees(std::asin(glm::clamp(g.z, -1.0, 1.0))), glm::degrees(std::atan2(g.y, g.x)));
    // Look at the breakup point (Earth-fixed at the breakup).
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    const glm::dvec3 rel = toEcef(B, tb) - up * R;
    glm::dvec3 east = glm::cross(glm::dvec3(0, 0, 1), up);
    east = glm::length(east) > 1e-9 ? glm::normalize(east) : glm::dvec3(1, 0, 0);
    const glm::dvec3 north = glm::cross(up, east);
    const glm::dvec3 rn = glm::normalize(rel);
    aimCameraAzEl((float)glm::degrees(std::atan2(glm::dot(rn, east), glm::dot(rn, north))),
                  (float)glm::degrees(std::asin(glm::clamp(glm::dot(rn, up), -1.0, 1.0))));
    camera.fovYDeg = 80.0f;
    updatePositions(t0);
    char b[200];
    snprintf(b, sizeof(b), "%s re-enters: breakup in 25 s, %.0f km away", satLabel((int)sat).c_str(),
             glm::length(rel) / 1000.0);
    msg = b;
    return true;
}

// ── Readouts ────────────────────────────────────────────────────────────────────────────────────
int SatelliteSim::rosterFateAt(uint32_t idx, double t, double *whenT) const
{
    // The deployment era (2026-10-20): not launched yet (4), or retired with its group (5).
    if (idx < activeSatCount && 2ull * idx + 1 < rosterLife.size())
    {
        const int32_t sec = (int32_t)std::clamp(std::floor(t - kFuryArriveT), -2147483000.0, 2147483000.0);
        if (sec < rosterLife[2ull * idx])
        {
            if (whenT)
                *whenT = kFuryArriveT + (double)rosterLife[2ull * idx];
            return 4;
        }
        if (sec >= rosterLife[2ull * idx + 1])
        {
            if (whenT)
                *whenT = kFuryArriveT + (double)rosterLife[2ull * idx + 1];
            return 5;
        }
    }
    if (!kesslerEnabled || idx >= activeSatCount || 2ull * idx + 1 >= kRosterWords.size())
        return 0;
    const int32_t sec = (int32_t)std::clamp(std::floor(t - kFuryArriveT), -2147483000.0, 2147483000.0);
    const int32_t dead = kRosterWords[2ull * idx], end = kRosterWords[2ull * idx + 1];
    if (end != kessler::kNever && sec >= end)
    {
        if (whenT)
            *whenT = kFuryArriveT + (double)end;
        return (idx < kRosterDecay.size() && end == kRosterDecay[idx]) ? 3 : 2;
    }
    if (dead != kessler::kNever && sec >= dead)
    {
        if (whenT)
            *whenT = kFuryArriveT + (double)dead;
        return 1;
    }
    return 0;
}

const char *SatelliteSim::rosterFateName(int fate)
{
    switch (fate)
    {
    case 1: return "dead";
    case 2: return "destroyed";
    case 3: return "re-entered";
    case 4: return "not launched yet";
    case 5: return "retired";
    default: return "working";
    }
}

void SatelliteSim::kesslerDropGoneSelection()
{
    // A roster satellite the cascade (or the player) destroyed, or that re-entered, is not there to select: drop it
    // (the selection panel kept its reticle on the orbit it no longer flies: the user's "ghost", issue 20261019_231126).
    // Going back in time before its end does not bring the selection back.
    if (selectedSatIndex < 0 || selectedSatIndex >= (int)activeSatCount)
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    if (rosterFateAt((uint32_t)selectedSatIndex, t) >= 2)
        selectedSatIndex = -1;
}

std::string SatelliteSim::satLabel(int idx) const
{
    if (idx < 0 || idx >= (int)satOrbits.size())
        return "?";
    char b[160];
    auto rosterName = [&](uint32_t i) {
        const SatOrbit &o = satOrbits[i];
        const char *member = rosterMemberName(i); // a List member's own name (the GEO belt), a station's
        if (member)
            snprintf(b, sizeof(b), "%s", member);
        else if (o.constIdx < constellations.size())
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
    if (idx < (int)poolIdx0())
        return launchLabel((uint32_t)idx - activeSatCount);
    const uint32_t j = (uint32_t)idx - poolIdx0();
    if (j >= kPool.size())
        return "?";
    const kessler::PoolRec &r = kPool[j];
    snprintf(b, sizeof(b), "Debris %u (breakup %u)", j, r.event);
    return b;
}

std::string SatelliteSim::kesslerStatusLine() const
{
    if (!kesslerEnabled || kWorkerDone)
        return {};
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    // Only while time is actually held (SatelliteSim.cpp: it may lead the frontier by ~2 s of the warp).
    const double lead = std::clamp(2.0 * (double)kTimeScales[timeScaleIdx], 2.0, 6.0 * 3600.0);
    if (t < kWorldStart || t + 1.0 < kFrontier + lead)
        return {};
    char b[128];
    snprintf(b, sizeof(b), "Computing the debris cascade: day %.0f of %.0f",
             (kFrontier - kFuryArriveT) / 86400.0, kesslerHorizonYears * 365.25);
    return b;
}

const nlohmann::json &SatelliteSim::kesslerStateCached() const
{
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (kStateCacheWall < 0.0 || now - kStateCacheWall > 0.25)
    {
        kStateCache = kesslerStateJsonImpl(false);
        kStateCacheWall = now;
    }
    kCountsWantedWall = now;
    return kStateCache;
}

nlohmann::json SatelliteSim::kesslerStateJson() const { return kesslerStateJsonImpl(true); }

nlohmann::json SatelliteSim::kesslerStateJsonImpl(bool full) const
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
    j["world_start_days"] = (kWorldStart - kFuryArriveT) / 86400.0;
    j["last_breakup_ms"] = kLastBreakupMs;
    j["last_breakup_in_place"] = kLastBreakupFast;
    j["time_held"] = !kesslerStatusLine().empty();
    j["records"] = kPool.size();
    j["records_gpu"] = kPoolGpu;
    j["capacity"] = kPoolCap;
    j["hash"] = cacheName(kHash);
    j["dust_clouds_drawn"] = kDustCount;
    // The scattering dust Gaussians drawn this frame (2026-10-20): how many, their summed light (sky units) and the
    // spread of their sizes and distances, to check a breakup's cloud without a picture.
    if (kDustMapped && kDustCount > 0)
    {
        const GpuDustCloud *dc = reinterpret_cast<const GpuDustCloud *>(static_cast<const char *>(kDustMapped) + 16);
        int nd = 0;
        double ampSum = 0.0, dMin = 1e30, dMax = 0.0, sMin = 1e30, sMax = 0.0;
        for (uint32_t i = 0; i < kDustCount; ++i)
        {
            if (dc[i].col.w != 0.0f)
                continue;
            ++nd;
            ampSum += dc[i].c.w;
            const double d = glm::length(glm::vec3(dc[i].c));
            dMin = std::min(dMin, d);
            dMax = std::max(dMax, d);
            // ia.w = 1 / ((2 pi)^1.5 sR sS sW): the geometric-mean sigma
            const double prod = 1.0 / (std::pow(2.0 * glm::pi<double>(), 1.5) * std::max((double)dc[i].ia.w, 1e-300));
            const double sg = std::cbrt(prod);
            sMin = std::min(sMin, sg);
            sMax = std::max(sMax, sg);
        }
        if (nd > 0)
            j["dust"] = {{"gaussians", nd}, {"light_sum", ampSum}, {"range_km", {dMin / 1000.0, dMax / 1000.0}},
                         {"sigma_km_geomean", {sMin / 1000.0, sMax / 1000.0}}};
    }
    j["reentries_now"] = kReentriesNow;
    j["small_reentries"] = {{"now", kSmallReNow}, {"this_hour", kSmallReHourCount}, {"streams", kSmallStreamCount},
                            {"gain", kesslerSmallReGain}};
    {
        // This hour's fragment masses (median, 90th percentile, kg).
        const int32_t hrNow = (int32_t)std::floor(((double)simDayJ2000 * 86400.0 + simSecInDay - kFuryArriveT) / 3600.0);
        auto it = kSmallReHours.find(hrNow);
        if (it != kSmallReHours.end() && !it->second.empty())
        {
            std::vector<float> m;
            m.reserve(it->second.size());
            for (const KSmallRe &r : it->second)
                m.push_back(r.massKg);
            std::sort(m.begin(), m.end());
            j["small_reentries"]["mass_p50_kg"] = m[m.size() / 2];
            j["small_reentries"]["mass_p90_kg"] = m[m.size() * 9 / 10];
        }
    }
    {
        // Diagnostics: re-entries due in the next 24 h (roster / pieces), and how long the pieces live.
        const int32_t s0 = (int32_t)std::floor((double)simDayJ2000 * 86400.0 + simSecInDay - kFuryArriveT);
        size_t nr = 0, np = 0;
        for (int32_t hr = std::max(s0, 0) / 3600; hr < std::max(s0, 0) / 3600 + 24; ++hr)
        {
            auto it = kReentries.find(hr);
            if (it == kReentries.end())
                continue;
            for (const auto &entry : it->second)
                (entry.first < activeSatCount ? nr : np)++;
        }
        j["reentries_next_24h"] = {{"satellites", nr}, {"pieces", np}};
    }
    if (full)
    {
        size_t lt[5] = {0, 0, 0, 0, 0}; // < 1 d, < 10 d, < 100 d, < 1000 d, longer
        double hpSum = 0.0;
        size_t hpN = 0;
        for (const kessler::PoolRec &r : kPool)
        {
            const double life = r.decaySec == kessler::kNever ? 1e12 : (double)(r.decaySec - r.bornSec);
            lt[life < 86400.0 ? 0 : life < 864000.0 ? 1 : life < 8640000.0 ? 2 : life < 86400000.0 ? 3 : 4]++;
            if (life < 864000.0)
            {
                hpSum += r.a * (1.0 - r.e) - satphot::kEarthRadiusM;
                ++hpN;
            }
        }
        j["piece_lifetimes"] = {{"lt_1d", lt[0]}, {"lt_10d", lt[1]}, {"lt_100d", lt[2]}, {"lt_1000d", lt[3]}, {"longer", lt[4]},
                                {"short_mean_perigee_km", hpN ? hpSum / hpN / 1000.0 : 0.0}};
    }
    j["fireball_heads"] = kFireCount;
    j["fireball_trails"] = kFireTrails;
    if (kFireMapped && kFireCount > 0)
    {
        // The brightest heads now: azimuth / elevation (deg), magnitude, range (km).
        const GpuFireball *fo = reinterpret_cast<const GpuFireball *>(static_cast<const char *>(kFireMapped) + 16);
        std::vector<uint32_t> ord(kFireCount);
        for (uint32_t i = 0; i < kFireCount; ++i)
            ord[i] = i;
        std::sort(ord.begin(), ord.end(), [&](uint32_t a, uint32_t b) { return fo[a].dirFlare.w > fo[b].dirFlare.w; });
        nlohmann::json arr = nlohmann::json::array();
        for (uint32_t q = 0; q < std::min<uint32_t>(kFireCount, 8); ++q)
        {
            const GpuFireball &f = fo[ord[q]];
            arr.push_back({std::round(glm::degrees(std::atan2(f.dirFlare.x, f.dirFlare.y)) * 10.0) / 10.0,
                           std::round(glm::degrees(std::asin(std::clamp(f.dirFlare.z, -1.0f, 1.0f))) * 10.0) / 10.0,
                           std::round((6.0 - 2.5 * std::log10(std::max(f.dirFlare.w, 1e-9f) / 0.008f)) * 10.0) / 10.0,
                           std::round(f.colRange.w / 100.0) / 10.0});
        }
        j["fireballs_az_el_mag_km"] = arr;
    }
    j["dust_gain"] = kesslerDustGain;
    // Counts at the current time: exact (one walk of everything), or the settings tab's from the walk spread over
    // frames.
    KCountsWalk w;
    if (full)
    {
        w.t = t;
        w.sec = (int32_t)std::clamp(std::floor(t - kFuryArriveT), -2147483000.0, 2147483000.0);
        kesslerCountsWalk(w, SIZE_MAX / 8);
    }
    const nlohmann::json c = kesslerCountsJson(full ? w.acc : kCountsLive);
    j["pieces_by_class"] = c["pieces_by_class"];
    j["now"] = c["now"];
    return j;
}

bool SatelliteSim::kesslerCountsWalk(KCountsWalk &w, size_t budget) const
{
    if (kTypeClass.size() != satTypes.size())
    {
        kTypeClass.assign(satTypes.size(), (int8_t)-1);
        for (int kk = 0; kk < 3; ++kk)
            for (int cc = 0; cc < 3; ++cc)
                if (kDebrisKit[kk][cc] >= 0 && (size_t)kDebrisKit[kk][cc] < kTypeClass.size())
                    kTypeClass[(size_t)kDebrisKit[kk][cc]] = (int8_t)cc;
    }
    KCounts &a = w.acc;
    const int32_t sec = w.sec;
    for (const size_t end = std::min(kPool.size(), w.pool + budget); w.pool < end; ++w.pool)
    {
        const kessler::PoolRec &r = kPool[w.pool];
        if (sec >= r.bornSec && sec < r.deathSec)
        {
            ++a.frag;
            a.fragReal += (size_t)std::llround(r.weight);
            if (r.type < kTypeClass.size() && kTypeClass[r.type] >= 0)
                ++a.byClass[kTypeClass[r.type]];
        }
    }
    const size_t nRoster = kRosterWords.size() / 2;
    for (const size_t end = std::min(nRoster, w.roster + budget); w.roster < end; ++w.roster)
    {
        const size_t i = w.roster;
        if (sec >= kRosterWords[2 * i + 1])
            ++a.rosterGone;
        else if (sec >= kRosterWords[2 * i])
            ++a.dead;
    }
    for (const size_t end = std::min(kEvents.size(), w.ev + 4 * budget); w.ev < end; ++w.ev)
    {
        const kessler::Event &e = kEvents[w.ev];
        if (e.t > w.t)
            continue;
        a.smallMade += e.nSmall;
        if (e.origin == kessler::ORIGIN_USER)
            ++a.user;
        switch (e.kind)
        {
        case kessler::EV_KILL: ++a.kills; break;
        case kessler::EV_BUMP: ++a.bumps; break;
        case kessler::EV_COLLISION: ++a.col; a.cat += e.catastrophic; break;
        default: break;
        }
    }
    return w.pool >= kPool.size() && w.roster >= nRoster && w.ev >= kEvents.size();
}

nlohmann::json SatelliteSim::kesslerCountsJson(const KCounts &c) const
{
    nlohmann::json j;
    j["pieces_by_class"] = {{"S", c.byClass[0]}, {"M", c.byClass[1]}, {"L", c.byClass[2]}};
    j["now"] = {{"dead_satellites", c.dead},  {"fragments", c.frag},     {"fragments_real", c.fragReal},
                {"roster_gone", c.rosterGone}, {"storm_kills", c.kills}, {"bumps", c.bumps},
                {"collisions", c.col},        {"catastrophic", c.cat},   {"user_events", c.user},
                {"kp", spaceWeather_.kp},     {"small_fragments_made", (double)std::llround(c.smallMade)}};
    return j;
}

// Per frame, only while the settings tab reads the counts (kCountsWantedWall): one slice of the walk. A pass counts
// at the second it started (a second or so stale at 1x).
void SatelliteSim::kesslerCountsTick()
{
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (kCountsWantedWall < 0.0 || now - kCountsWantedWall > 1.0)
        return;
    KCountsWalk &w = kCountsWalkState;
    if (!w.active)
    {
        w = KCountsWalk{};
        w.t = (double)simDayJ2000 * 86400.0 + simSecInDay;
        w.sec = (int32_t)std::clamp(std::floor(w.t - kFuryArriveT), -2147483000.0, 2147483000.0);
        w.active = true;
    }
    if (kesslerCountsWalk(w, 150000))
    {
        kCountsLive = w.acc;
        kCountsLiveValid = true;
        w.active = false;
        kStateCacheWall = -1.0; // publish with the next read
    }
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
                    "Nothing differs before it, unless you break something up (that starts it there)."))
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
    static const char *kDust[] = {"x1", "x10", "x30", "x100", "x1000"};
    static const float kDustV[] = {1.0f, 10.0f, 30.0f, 100.0f, 1000.0f};
    int dSel = 3;
    for (int i = 0; i < 5; ++i)
        if (std::abs(kesslerDustGain - kDustV[i]) < 1e-3f * kDustV[i])
            dSel = i;
    const int dg = uiChoiceRow(inp, ui, "KesslerDust", 0, "Dust cloud brightness", kDust, 5, dSel,
                               "Each breakup's dust and small fragments as a glowing cloud that spreads along its orbit. x1 is "
                               "the physical estimate (a few % of the mass as fine dust): it fades below the dark sky within "
                               "minutes. Higher keeps the clouds visible longer.");
    if (dg >= 0 && dg != dSel)
        kesslerDustGain = kDustV[dg];
    static const char *kFireCol[] = {"Off", "Subtle", "Vivid"};
    static const float kFireColV[] = {0.0f, 0.4f, 0.8f};
    int fSel = 2;
    for (int i = 0; i < 3; ++i)
        if (std::abs(kesslerFireColor - kFireColV[i]) < 0.05f)
            fSel = i;
    const int fc = uiChoiceRow(inp, ui, "KesslerFireCol", 0, "Fireball material colours", kFireCol, 3, fSel,
                               "Re-entry fragments tinted by what burns: aluminium white, copper wiring green, titanium "
                               "blue-white, sodium yellow, lithium cells crimson; steel and composites stay orange-red.");
    if (fc >= 0 && fc != fSel)
        kesslerFireColor = kFireColV[fc];
    static const char *kSmallRe[] = {"Off", "x1", "x3", "x10", "x30", "x100"};
    static const float kSmallReV[] = {0.0f, 1.0f, 3.0f, 10.0f, 30.0f, 100.0f};
    int sSel = 4;
    for (int i = 0; i < 6; ++i)
        if (std::abs(kesslerSmallReGain - kSmallReV[i]) <= 1e-3f * std::max(kSmallReV[i], 1.0f))
            sSel = i;
    const int sg = uiChoiceRow(inp, ui, "KesslerSmallRe", 0, "Small re-entry brightness", kSmallRe, 6, sSel,
                               "The 10 cm - 1 m fragments coming down: thousands an hour once the cascade runs, each a short "
                               "meteor-like streak. x1 is physical: about magnitude +1 from 100 km for a 0.1-kg piece, so "
                               "faint from orbit. Higher makes them easier to spot.");
    if (sg >= 0 && sg != sSel)
        kesslerSmallReGain = kSmallReV[sg];
    if (kesslerEnabled && uiButton(inp, ui, "KesslerReentry", 0, "Watch the next re-entry",
                                   "Jumps to the next satellite or large piece coming down over a dark sky: 25 s before it "
                                   "breaks up, from beside its track."))
    {
        std::string why;
        kesslerGoToNextReentry(why);
        snprintf(kUiMsg, sizeof(kUiMsg), "%s", why.c_str());
    }
    if (!kesslerEnabled)
        return;
    const nlohmann::json &j = kesslerStateCached();
    const nlohmann::json &n = j["now"];
    const float lw = (float)fs(170);
    const double days = j["days_since_cme"].get<double>();
    uiKV("KesslerKV", 0, "Since the storm", days < 0.0 ? uiKitStr("in %.1f days", -days) : uiKitStr("%.1f days", days), lw);
    uiKV("KesslerKV", 1, "Simulated to", uiKitStr("day %.1f%s", j["frontier_days"].get<double>(), kWorkerDone ? " (done)" : ""), lw);
    uiKV("KesslerKV", 2, "Dead satellites (tumbling)", uiKitStr("%d", n["dead_satellites"].get<int>()), lw);
    uiKV("KesslerKV", 3, "Destroyed / re-entered", uiKitStr("%d", n["roster_gone"].get<int>()), lw);
    uiKV("KesslerKV", 4, "Large pieces (>= 1 m)", uiKitStr("%d drawn of ~%d", n["fragments"].get<int>(), n["fragments_real"].get<int>()), lw);
    uiKV("KesslerKV", 8, "Fragments 10 cm - 1 m made", uiKitStr("~%.0f (a density field)", n["small_fragments_made"].get<double>()), lw);
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
    const ExplodeResult r = explodeSatellite(sat, kind == kessler::EV_ASAT ? BreakupKind::Asat : BreakupKind::Explosion);
    snprintf(kUiMsg, sizeof(kUiMsg), "%s%s", r.ok ? "" : "Not now: ", r.reason.c_str());
}
