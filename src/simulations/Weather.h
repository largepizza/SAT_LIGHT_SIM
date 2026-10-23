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
    // Organised systems (2026-10-23), spawned by their own long-lived pass (Catalog's line bins):
    SquallLine = 3,  // a line of 4-16 regenerating towers 100-350 km long, moving fast, trailing stratiform rain
                     // under one shared shield blown rearward; often bowed
    Mcs = 4,         // a mesoscale convective complex: a broad arc / cluster of regenerating towers under one
                     // round shield 200-500 km across, long-lived (night over continents)
    Tropical = 5,    // a tropical cyclone's eyewall / inner-band towers (wx::Cyclone; not a Storm record)
};
constexpr int kStormKinds = 5;   // the Storm kinds (Tropical has its own records)
inline bool isLineSystem(StormKind k) { return k == StormKind::SquallLine || k == StormKind::Mcs; }
const char *stormKindName(StormKind k);   // "air-mass", "multicell", "supercell", "squall line", "MCS" 

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
    // A REGENERATING slot (line systems): a new tower every `period` s from t0 while the system supports it
    // ([sysOn, sysOff] s after the storm's birth, ramped over sysRamp), each cycle with its own seed, peak and a
    // small shift along the line. period 0 = one tower (the single-cell kinds).
    float period = 0, sysOn = 0, sysOff = 0, sysRamp = 1;
    // When it reaches the anvil and when it falls back below it (s after the storm's birth; anvOff <= anvOn = never):
    // the shield's feed (precomputed at birth, single-cell kinds).
    float anvOn = 0, anvOff = 0;
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
    TowerSpec towers[16];
    // The anvil SHIELD (2026-10-23): spread out by the anvil-reaching towers, outliving them by `linger` s while it
    // drifts downwind at `upperWind` m/s (storm-relative) and thins into cirrus. Line systems: their shared
    // shield, along the line (lineHalfM either side of the centre, across `lineAz`) and trailing rearward.
    float     linger = 0;       // s
    float     upperWind = 10;   // m/s, along shear
    float     lineHalfM = 0;    // line systems: half the line's length (m)
    float     lineAz = 0;       // line systems: the line's axis, rad from local east toward north at pos0
    float     trailM = 0;       // line systems: how far the stratiform region / shield trails behind the line (m)
    float     bow = 0;          // line systems: the line's forward bulge at its middle (m)
    float     sysLife = 0;      // line systems: the convective life (s), the shield lingers past it
    // The storm centre at time t (map frame): pos0 moved along the great circle of vel.
    glm::dvec3 centreAt(double t) const;
};

// The anvil shield of one storm at one moment (map frame): a capsule from a to b (the spine, great-circle chord),
// radius rA at a growing to rB at b. What cv2AnvilSigmaP draws (cv2_storms.glsl CV2Shield).
struct Shield {
    glm::dvec3 a{0, 0, 1}, b{0, 0, 1};
    float rA = 0, rB = 0;       // m
    float strength = 0;         // 0..1
    float thick = 1;            // 0..1, x the anvil thickness (thins as it lingers)
    float age01 = 0;            // 0 fed by its towers .. 1 gone (the shader frays and thins it)
    float rain = 0;             // 0..1: a precipitating stratiform region under it (line systems' trailing rain)
    float lineK = 0;            // 0..1: share of its spine that is a convective line (the leading edge at a)
    glm::vec3 rear{0, 0, 0};    // line systems: map-frame tangent toward the trailing side at the spine's middle
    float eyeR = 0;             // tropical cyclones' central dense overcast: the eye's radius (m; 0 none)
    uint32_t seed = 0;
    uint64_t stormId = 0;
    StormKind kind = StormKind::AirMass;
};

// A TROPICAL CYCLONE (2026-10-23): one per (basin, season, index), its whole life fixed at genesis like every storm.
// Unlike storms it lives in the EARTH-fixed frame (its track: westward in the trades, recurving poleward and east; the
// cloud map's rigid drift is eastward everywhere), converted to the map frame for the GPU each frame.
struct Cyclone {
    uint64_t id = 0;
    int basin = 0, year = 0, seq = 0;   // seq: its order among the season's storms that reached tropical-storm strength
    std::string name;                   // WMO list name ("" until named: a depression that never reached 17.5 m/s)
    double t0 = 0;                      // genesis (J2000 s)
    float life = 0;                     // s
    float hemi = 1;                     // +1 north (counter-clockwise), -1 south
    float rmw0 = 40000.0f;              // radius of maximum wind at genesis (m); it contracts as it intensifies
    float size = 0.5f;                  // 0..1: the cloud shield's and bands' extent
    float vPeak = 0;                    // its peak 1-min sustained wind (m/s)
    double tPeak = 0;                   // when (J2000 s)
    uint32_t seed = 0;
    std::vector<glm::dvec3> track;      // Earth-fixed unit vectors, hourly from t0
    std::vector<float> vmax;            // m/s, hourly
    std::vector<uint8_t> overLand;      // hourly
    static constexpr double kStepS = 3600.0;
    glm::dvec3 earthAt(double t) const;
    float vmaxAt(double t) const;
};

// A cyclone's state at one moment.
struct CycloneState {
    const Cyclone *c = nullptr;
    glm::dvec3 earth{0, 0, 1}, map{0, 0, 1};   // centre
    glm::dvec3 motionEarth{0};                  // m/s, Earth-fixed tangent
    float vmax = 0, pressureHpa = 1010;
    int category = -1;                          // -1 depression, 0 tropical storm, 1-5 Saffir-Simpson
    float rmw = 40000, eyeR = 20000, cdoR = 200000, outerR = 600000;   // m
    float intensity01 = 0;                      // vmax mapped 17.5 .. 75 m/s -> 0..1
    bool overLand = false;
};
const char *cycloneBasinName(int basin);        // "North Atlantic" ...
const char *cycloneCategoryName(int cat, int basin);   // "Tropical depression", "Tropical storm", "Category 3 hurricane" /
                                                       // "typhoon" / "severe cyclonic storm" ... by basin

struct ShieldParams {
    float headRadiusM = 20000.0f;   // a mature anvil-reaching head's radius (the tower radius x flare)
    float headDriftM = 15000.0f;    // how far the head is blown downwind ("Cb head drift")
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
    // Line systems (squall lines, MCSs): the mean number alive worldwide at frequency 1 if moisture were 1
    // everywhere (x "Storm complexes" setting).
    float lineFrequency = 1.0f;
    float targetLineActive = 160.0f;
    // Tropical cyclones: x each basin's climatological count per season (~85 a year worldwide at 1).
    float cycloneFrequency = 1.0f;
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
    // Every anvil shield at t (storms alive or lingering), from the storms stormsAt(t) returns.
    void shieldsAt(double t, const ShieldParams &p, std::vector<Shield> &out);
    // The same from a list stormsAt(t) already returned (one scan of the bins a frame).
    void towersAt(double t, const std::vector<const Storm *> &storms, std::vector<Tower> &out);
    void shieldsAt(double t, const std::vector<const Storm *> &storms, const ShieldParams &p, std::vector<Shield> &out);
    // Tropical cyclones alive at t (their seasons built and cached by year), strongest first; and their towers (the
    // eyewall and inner rainbands) and their central dense overcast (a shield with an eye) APPENDED to the lists.
    void cyclonesAt(double t, std::vector<CycloneState> &out);
    void cycloneTowersAt(double t, const std::vector<CycloneState> &cs, std::vector<Tower> &out) const;
    void cycloneShieldsAt(double t, const std::vector<CycloneState> &cs, std::vector<Shield> &out) const;
    // Every cyclone of a basin's season (built if needed), in genesis order; and one by id (cached seasons only).
    void cycloneSeason(int basin, int year, std::vector<const Cyclone *> &out);
    const Cyclone *findCyclone(uint64_t id) const;
    static constexpr int kCycloneBasins = 6;
    // The tower specs of one storm at the cycle running at tau (s since its birth): the dominant spec's cycle state
    // for a regenerating slot (age in the cycle, its life). False if the slot has no tower at tau.
    static bool towerCycleAt(const TowerSpec &ts, double tau, float &strength, float &age, float &life,
                             uint32_t &seed, float &shift);
    // The storm with this id if it is cached (a selection), else nullptr.
    const Storm *find(uint64_t id) const;
    // Every storm born in one time bin (index floor(t / kBinS)), building it if needed (tools, the births view).
    void stormsInBin(int64_t binIndex, std::vector<const Storm *> &out);
    // Threads for building bins (default 1). Above 1 the Env callbacks are called from worker threads at once,
    // so they must be thread-safe; the result is identical for any count.
    void setBuildThreads(int n);
    // Diagnostics: the spawn rate's calibration constant and each kind's mean lifetime (s).
    double calibrationK() const;
    double calibrationKLine() const;
    double meanLifeS(StormKind k) const;

    struct Stats {
        int binsCached = 0;
        int binsBuiltLastCall = 0;
        double buildMsLastCall = 0;
        int stormsAlive = 0, towersAlive = 0, lineSystemsAlive = 0, shieldsAlive = 0;
    };
    Stats stats() const;

    // Seconds per spawn time bin, and the longest a storm can live (the window a query looks back over).
    static constexpr double kBinS = 1800.0;
    static constexpr double kMaxLifeS = 8.0 * 3600.0;      // incl. the anvil's linger
    // The line systems' own spawn bins (rarer, much longer-lived).
    static constexpr double kLineBinS = 3.0 * 3600.0;
    static constexpr double kLineMaxLifeS = 22.0 * 3600.0;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The tower's strength at time t from its spec (0 outside its life).
float towerStrength(const TowerSpec &ts, double tSinceStormBirth);

} // namespace wx
