#pragma once
// Weather events (.plans/WEATHER_EVENTS_PLAN.md, 2026-10-22): storms are OBJECTS, not regions of the cloud map.
//
// Every storm is a pure function of absolute sim time (seconds since J2000), like SpaceWeather's substorms and
// the Kessler small re-entries: spawned by hashes per (space cell, time bin), its whole life fixed at birth, so
// time reversal, warp, bookmarks and snapshots see the same storms. Nothing is integrated frame to frame.
//
// Frames: storms live in the cloud MAP's frame (the drifted sphere: map direction d = Rz(drift) x Earth-fixed e,
// clouds_v2.glsl cv2Drift), so a storm rides with the moisture it was born in. Env::mapToEarth gives the Earth-fixed
// point for the climatology, the land mask and the Sun. A unit vector's components are ECEF-like (z = the pole).
//
// App-independent: no Vulkan, no SatelliteSim. The sim (SatelliteSimWeather.cpp) supplies Env, asks for the towers
// at the frame's time and uploads them (GpuStormTower in SatelliteSim.h, shaders/include/cv2_storms.glsl).
#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wx {

class StormClimatology;

enum class StormKind : uint8_t {
    AirMass   = 0,   // ordinary single cell: one tower, 30-90 min; many never reach the anvil
    Multicell = 1,   // a cluster: new towers form on one flank every ~15-30 min, each living ~45-60 min
    Supercell = 2,   // one long-lived dominant tower (2-4 h), overshooting top, 0-2 flanking towers
};

// One convective tower of a storm, fixed at the storm's birth.
struct TowerSpec {
    float offE = 0, offN = 0;   // offset from the storm centre (m; the storm's local east / north in the map frame)
    float t0 = 0, life = 0;     // seconds after the storm's birth; its own lifetime
    float peak = 1;             // peak strength 0..1 (cv2ColumnSigma's 'str': >~0.45 reaches the anvil if dominant)
    float growS = 1200;         // seconds to grow to the peak (the tower rising)
    float decayS = 1200;        // seconds of decay at the end
    float role = 1;             // 1 dominant (the one that can reach the anvil), 0.7 / 0.5 flanking
    float size = 0.5;           // 0..1 -> radius x mix(0.7, 1.3, size) of the tower-radius setting
    float overshoot = 0.5;      // 0..1 hash (only ~the top third overshoot, cv2ColumnSigma)
    uint32_t seed = 0;
};

struct Storm {
    uint64_t  id = 0;           // stable: from the spawn cell, the time bin and the index in it
    StormKind kind = StormKind::AirMass;
    double    t0 = 0;           // birth, J2000 s
    float     life = 0;         // s, until its last tower has decayed
    glm::dvec3 pos0{0, 0, 1};   // map-frame unit vector at birth
    glm::dvec3 vel{0};          // map-frame tangent velocity at pos0 (m/s, a vector tangent to the sphere there)
    glm::vec3 shear{1, 0, 0};   // map-frame unit tangent at pos0: the downwind direction of the anvil
    float     potential = 0;    // diagnostics: the spawn rate's climatology part at birth
    float     moisture = 0;     // diagnostics: the map's cover at birth
    uint8_t   towerCount = 0;
    TowerSpec towers[4];
    // The storm centre at time t (map frame): pos0 moved along the great circle of vel.
    glm::dvec3 centreAt(double t) const;
};

// A tower's state at one moment: what the GPU draws.
struct Tower {
    glm::dvec3 dir{0, 0, 1};    // map-frame unit vector of the tower's axis
    float      strength = 0;    // 0..1 (lifecycle x peak)
    float      role = 1, size = 0.5f, overshoot = 0.5f;
    glm::vec3  shear{1, 0, 0};  // map-frame unit tangent at dir
    uint32_t   seed = 0;
    uint64_t   stormId = 0;
    uint8_t    towerIndex = 0;
    StormKind  kind = StormKind::AirMass;
    float      age = 0;         // seconds since the tower was born (its spec's t0)
    float      growS = 1200;    // its growth time (the anvil starts spreading once it has grown)
    float      life = 0;        // its lifetime (s)
};

struct Env {
    // The map's cloud cover (0..1, after the Coverage / clear / full remap) averaged over ~100 km around a
    // map-frame direction. Called for spawn candidates only.
    std::function<float(const glm::dvec3 &mapDir)> moisture;
    // Map frame -> Earth-fixed at time t (J2000 s).
    std::function<glm::dvec3(const glm::dvec3 &mapDir, double t)> mapToEarth;
    // Land share at an Earth-fixed direction: 0 sea .. 1 land.
    std::function<float(const glm::dvec3 &earthDir)> land;
    // The Sun's Earth-fixed unit direction at time t (local solar time comes from it).
    std::function<glm::dvec3(double t)> sunEarth;
};

struct Settings {
    float frequency = 1.0f;       // x the calibrated spawn rate ("Storm frequency")
    float targetActive = 3000.0f; // calibration: mean number of storms alive worldwide at frequency 1 IF moisture()
                                  // were 1 everywhere (the map's thinning then removes a share; tuned in the app)
    float moistureMin = 0.15f;    // no storm where the regional cover is below this
    float moistureFull = 0.6f;    // full rate from this cover
    float towerRadiusM = 15000.0f;// the tower-radius setting (cv2.column.z): flanking offsets scale with it
    float lifeScale = 1.0f;       // x every lifetime
    // Storms per flash over the sea vs land: a maritime storm makes ~an order of magnitude fewer flashes than a
    // continental one of the same size (weaker updrafts), so lightning density alone under-counts ocean storms.
    float seaFactor = 5.0f;
};

class Catalog {
public:
    Catalog();
    ~Catalog();
    // envKey: change it whenever anything Env answers changes (drift settings, coverage remap, the map): every
    // cached bin is dropped. Settings changes drop them too.
    void configure(const Env &env, const Settings &s, const StormClimatology *clim, uint64_t envKey);
    // Every storm alive at t (pure function of t). Builds the time bins it needs and caches them; bins far from
    // t are evicted. The pointers stay valid until the next call.
    void stormsAt(double t, std::vector<const Storm *> &out);
    // Every tower alive at t with strength > 0.
    void towersAt(double t, std::vector<Tower> &out);
    // The storm with this id if it is cached (a selection), else nullptr.
    const Storm *find(uint64_t id) const;
    // Every storm born in one time bin (index floor(t / kBinS)), building it if needed (tools, the births view).
    void stormsInBin(int64_t binIndex, std::vector<const Storm *> &out);
    // Threads for building bins (default 1). Above 1 the Env callbacks are called from worker threads at once,
    // so they must be thread-safe; the result is identical for any count.
    void setBuildThreads(int n);
    // Diagnostics: the spawn rate's calibration constant and each kind's mean lifetime (s).
    double calibrationK() const;
    double meanLifeS(StormKind k) const;

    struct Stats {
        int binsCached = 0;
        int binsBuiltLastCall = 0;
        double buildMsLastCall = 0;
        int stormsAlive = 0, towersAlive = 0;
    };
    Stats stats() const;

    // Seconds per spawn time bin, and the longest a storm can live (the window a query looks back over).
    static constexpr double kBinS = 1800.0;
    static constexpr double kMaxLifeS = 6.0 * 3600.0;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The tower's strength at time t from its spec (0 outside its life).
float towerStrength(const TowerSpec &ts, double tSinceStormBirth);

} // namespace wx
