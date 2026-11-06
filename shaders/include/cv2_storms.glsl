// Storm objects (.plans/WEATHER_EVENTS_PLAN.md, 2026-10-22): the convective towers the CPU's storm catalog
// (src/simulations/Weather.h) puts in the sky this frame, and a spatial index over them. They replaced the Cb
// tower LATTICE, whose cells held a tower wherever the weather map's type read cumulonimbus: storms were map
// regions, continent-sized alleys of anvils. Now a tower exists only where a storm object stands.
//
// Frame: the cloud MAP's frame (the drifted sphere, cv2Drift), like the weather cube's colour channels.
// Index: equal-angle cube-face bins of the drifted sphere (kCv2StormBinsPerFace per side); every tower is listed in
// every bin its reach (CV2Tower.c.x) touches, so a point reads its own bin and nothing else, on any face. The bins
// live in an open-addressed hash table (linear probing, CPU-built at load factor <= 0.5).
//
// Mirrored by SatelliteSim's GpuStormTower / kCv2Storm* (SatelliteSimWeather.cpp): keep the layout in step.
// Needs CV2_STORM_BINDING defined; without it there are no towers (cv2StormBin returns an empty bin).

#ifndef CV2_STORMS_GLSL
#define CV2_STORMS_GLSL

const uint kCv2StormMaxTowers   = 8192u;
const uint kCv2StormTableSize   = 65536u;   // power of two
const uint kCv2StormListSize    = 196608u;
const uint kCv2StormBinsPerFace = 256u;     // ~39 km bins at a face's centre
const uint kCv2StormMaxPerBin   = 12u;      // the CPU truncates a bin's list (strongest first) past this
// The ENVELOPE index (the weather bake's storm cloud field only, CV2_STORM_ENV): the same towers and bins, each tower
// listed out to cv2StormEnvRadius(its reach) instead of its reach.
const uint kCv2StormEnvTableSize = 131072u;
const uint kCv2StormEnvListSize  = 393216u;
// The anvil SHIELDS (2026-10-23): one per storm (cv2AnvilSigmaP), listed in coarser bins (~78 km; 156 km until
// 2026-10-27, when a sample in a storm cluster walked ~3.8x the shields that can reach it: a shield's 1.6-radius disc
// grown by a whole bin). The envelope index (the bake's) keeps 156-km bins.
const uint kCv2ShieldMax          = 4096u;
const uint kCv2ShieldTableSize    = 32768u;
const uint kCv2ShieldListSize     = 131072u;
const uint kCv2ShieldBinsPerFace  = 128u;
const uint kCv2ShieldEnvBinsPerFace = 64u;
const uint kCv2ShieldMaxPerBin    = 31u;      // (5 bits of a bin entry: 2026-10-23, a cyclone's arm cirrus)
const uint kCv2CycloneMax         = 8u;   // tropical cyclones (the weather bake stamps their bands, eyewall and eye)
// The HABOOB dust walls (2026-10-24): a grid on the tangent plane at the observer's sea-level point, Earth-fixed axes,
// rasterised by the CPU each frame from the storms' gust fronts (SatelliteSimHaboob.cpp weatherFillDustGrid).
const uint kCv2DustGridN          = 512u;

struct CV2Tower {
    vec4 a;   // xyz the tower's axis (map-frame unit vector), w strength 0..1 (its lifecycle x peak)
    vec4 b;   // x role (1 dominant, 0.7 / 0.5 flanking), y size 0..1, z overshoot 0..1, w the shear's bearing
              //   (rad from the local east toward north: the anvil's downwind direction)
    vec4 c;   // x reach (m: nothing of the tower or its anvil hang beyond), y seed (uint bits),
              // z the anvil's width factor (x "Cb flare") PACKED with the head's spread (cv2TowerAnvilK / Spread),
              // w its downwind drift factor (x "Cb head drift")
};

// c.z = the anvil width factor (< 8) + 8 x round((1 - spread) x 1000) (2026-11-03, issue 20261030_210137; a fully spread
// head's c.z is its plain width factor, as before): the spread 0..1 is
// how far the flared head and the anvil plate under it have grown since the tower reached the anvil, on the "Anvil
// formation time" ramp (SatelliteSim::updateStorms). Without it the plate popped in over the ~3 sim min the tower's
// strength takes from 0.42 to 0.6 ('full').
float cv2TowerAnvilK(vec4 c) { return c.z - 8.0 * floor(c.z * 0.125); }
float cv2TowerSpread(vec4 c) { return 1.0 - floor(c.z * 0.125) * 0.001; }
// c.w = the head's downwind drift factor (< 4) + 4 x round((1 - cap) x 1000) (2026-11-03): the cap 0..1 is the height
// the tower is DRAWN to as a share of its strength's (wx::Tower::heightCap: its top climbs over the "Tower growth
// time"; a grown tower's c.w is its plain drift factor).
float cv2TowerDriftK(vec4 c)    { return c.w - 4.0 * floor(c.w * 0.25); }
float cv2TowerHeightCap(vec4 c) { return 1.0 - floor(c.w * 0.25) * 0.001; }

// A storm's anvil shield (wx::Shield): a capsule along the great-circle chord a -> b, radius c.x at a growing to b.w
// at b. It outlives its towers (it drifts downwind and thins: c.y, c.z). Line systems: a long capsule along the
// line, its spine d.w behind the convective line (toward d.xyz), over a raining stratiform region (c.w).
struct CV2Shield {
    vec4 a;   // xyz the spine's start (map-frame unit vector), w strength 0..1
    vec4 b;   // xyz the spine's end, w the radius there (m)
    vec4 c;   // x the radius at a (m), y thickness factor 0..1, z age 0 (fed) .. 1 (gone), w stratiform rain 0..1
    vec4 d;   // xyz the rearward tangent (line systems; 0 otherwise), w the line's distance ahead of the spine (m);
              // w < 0: a tropical cyclone's central dense overcast, -w its eye's radius (m)
};

// A tropical cyclone (wx::CycloneState), for the weather bake's stamp (spiral bands, the eyewall, the CDO, the eye).
struct CV2Cyclone {
    vec4 a;   // xyz the centre (map frame), w intensity 0..1 (17.5 .. 75 m/s)
    vec4 b;   // xyz the centre where the low cloud reads the cube (flowed), w hemisphere (+1 north, -1 south)
    vec4 c;   // x the eye's radius (m, 0 none), y the CDO's radius, z the outer bands' reach, w the bands' phase (rad)
    vec4 d;   // x the radius of maximum wind (m), y vmax (m/s), z 1 over land, w the spiral arms (2 .. ~5, continuous)
};

// Weather v2 (2026-11-06): a weather SYSTEM (WeatherSystems.h; SatelliteSimWeatherSystems.cpp), read by the bake only
// (cloud_v2_weather.comp wxSystems): an object travelling on the steering wind that writes its own fields.
const uint kCv2SystemMax = 128u;
struct CV2System {
    vec4 a;   // xyz the centre (unit ECEF), w the comma head's radius (m)
    vec4 b;   // xyz the heading (unit tangent), w intensity 0..1
    vec4 c;   // x age 0..1 (the occlusion's progress), y hemisphere (+1 north), z kind (0 extratropical), w seed 0..1
    vec4 d;   // x speed (m/s), yz spare, w cos(its reach / R)
};

// Weather v2 convection (the bake only): a cell is a convective core (a catalog tower, drawn by the cumulus layer from
// the convection cube, never as geometry); an anvil source is a storm whose towers reached the lid: the bake integrates
// what it detrained along its storm-relative upper wind (cloud_v2_weather.comp wxConvection).
const uint kCv2CellMax = 8192u, kCv2AnvilMax = 4096u, kCv2ConvGridLon = 180u, kCv2ConvGridLat = 90u, kCv2ConvListMax = 131072u;
struct CV2Cell {
    vec4 a;   // xyz the axis (unit ECEF), w the core's radius (m)
    vec4 b;   // x strength 0..1, y the top's share of the way to the lid (0..1), z the tower's seed (uint bits), w role (1 dominant)
};
struct CV2Anvil {
    vec4 a;   // xyz the source's centre now (unit ECEF), w its radius (m)
    vec4 b;   // xyz downwind (unit tangent), w strength 0..1
    vec4 c;   // x seconds since it first reached the lid, y how long it fed the anvil (s), z linger tau (s), w upper wind (m/s, storm-relative)
    vec4 d;   // x a line's half-length (m, 0 = a disc), yzw the line's axis (unit tangent)
};

#ifdef CV2_STORM_BINDING
layout(std430, set = 0, binding = CV2_STORM_BINDING) readonly buffer CV2StormBuf {
    uvec4    cv2StormHead;                          // x towers, y table entries used, z list entries used, w frame
    CV2Tower cv2Towers[kCv2StormMaxTowers];
    uvec2    cv2StormTable[kCv2StormTableSize];     // x key (0 = empty), y (list start << 4) | count
    uint     cv2StormList[kCv2StormListSize];       // tower indices
    uvec4    cv2ShieldHead;                         // x shields, y non-zero = storm objects on (shields replace the map's
                                                    // cumulonimbus as the anvils' source): the bins per face side
                                                    // (kCv2ShieldBinsPerFace), z bins, w list entries used
    CV2Shield cv2Shields[kCv2ShieldMax];
    uvec2    cv2ShieldTable[kCv2ShieldTableSize];   // as cv2StormTable, kCv2ShieldBinsPerFace bins
    uint     cv2ShieldList[kCv2ShieldListSize];
    uvec4    cv2CycHead;                            // x cyclones
    CV2Cyclone cv2Cyclones[kCv2CycloneMax];
    uvec4    cv2DustHead;                           // x 1 = a dust wall in the grid, y its bounding box (lo x | lo y << 16),
                                                    // z (hi x | hi y << 16), w unused
    vec4     cv2DustFrame[4];                       // [0] xyz the grid's x axis (ECEF), w 1 / cell (m)
                                                    // [1] xyz its y axis, w the highest dust top in it (m above the ground)
                                                    // [2] xyz the lobe noise's anchor (frac, moving with the wall), w 1 / period
                                                    // [3] xy the observer's sea-level point in grid cells (- 0.5)
    uint     cv2DustGrid[kCv2DustGridN * kCv2DustGridN];   // e x 65535 | (top / 2 m) << 16 (12 bits) | rain-free x 15 << 28
#ifdef CV2_STORM_ENV
    uvec2    cv2StormEnvTable[kCv2StormEnvTableSize];
    uint     cv2StormEnvList[kCv2StormEnvListSize];
    // The envelope index's own copy of the towers (built when the bake runs), a.xyz where the low cloud reads the
    // weather cube for each (wd + cv2FlowDisp): the cube is read through the flow, so a storm's own cloud is stamped
    // there, or it lands up to hundreds of km beside the tower.
    CV2Tower cv2EnvTowers[kCv2StormMaxTowers];
    // The shields likewise (their spines at the flowed positions), for the line systems' stratiform rain.
    uvec2    cv2ShieldEnvTable[kCv2ShieldTableSize];
    uint     cv2ShieldEnvList[kCv2ShieldListSize];
    CV2Shield cv2EnvShields[kCv2ShieldMax];
    uvec4    cv2SysHead;                            // x systems (Weather v2)
    CV2System cv2Systems[kCv2SystemMax];
    // Weather v2 convection (SatelliteSimWeatherSystems.cpp updateConvection): the CELLS (the catalog's towers as
    // convective cores) and the ANVIL sources (each storm whose towers reached the lid), indexed on a lat-lon grid of
    // kCv2ConvGridLon x kCv2ConvGridLat cells; a list entry's high bit marks an anvil.
    uvec4    cv2ConvHead;                           // x cells, y anvils, z list entries used
    CV2Cell  cv2Cells[kCv2CellMax];
    CV2Anvil cv2Anvils[kCv2AnvilMax];
    uvec2    cv2ConvGrid[kCv2ConvGridLon * kCv2ConvGridLat];   // x list start, y count
    uint     cv2ConvList[kCv2ConvListMax];
#endif
};
#endif

// (The haboobs' dust wall lookup, cv2DustWallAt, was retired 2026-11-06; the grid stays in the buffer's layout.)

// An integer hash (the lightning's per-cell seeds).
uint cv2StormHash(uint k)
{
    k ^= k >> 16; k *= 0x7FEB352Du; k ^= k >> 15; k *= 0x846CA68Bu; k ^= k >> 16;
    return k;
}

// (Weather v2 step 5, 2026-11-06: the towers' bins, the shields (cv2ShieldAt), the cyclones' CDO swirl and the
// envelope index's lookups are gone; the convection cube (cv2ConvTex) holds the storms. The buffer above keeps its
// layout until the CPU side drops those records.)

#endif
