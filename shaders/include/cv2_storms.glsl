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
// The anvil SHIELDS (2026-10-23): one per storm (cv2AnvilSigmaP), listed in coarser bins (~156 km).
const uint kCv2ShieldMax          = 4096u;
const uint kCv2ShieldTableSize    = 16384u;
const uint kCv2ShieldListSize     = 65536u;
const uint kCv2ShieldBinsPerFace  = 64u;
const uint kCv2ShieldMaxPerBin    = 15u;
const uint kCv2CycloneMax         = 8u;   // tropical cyclones (the weather bake stamps their bands, eyewall and eye)

struct CV2Tower {
    vec4 a;   // xyz the tower's axis (map-frame unit vector), w strength 0..1 (its lifecycle x peak)
    vec4 b;   // x role (1 dominant, 0.7 / 0.5 flanking), y size 0..1, z overshoot 0..1, w the shear's bearing
              //   (rad from the local east toward north: the anvil's downwind direction)
    vec4 c;   // x reach (m: nothing of the tower or its anvil hang beyond), y seed (uint bits),
              // z the anvil's width factor (x "Cb flare"), w its downwind drift factor (x "Cb head drift")
};

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
    vec4 d;   // x the radius of maximum wind (m), y vmax (m/s), z 1 over land, w unused
};

#ifdef CV2_STORM_BINDING
layout(std430, set = 0, binding = CV2_STORM_BINDING) readonly buffer CV2StormBuf {
    uvec4    cv2StormHead;                          // x towers, y table entries used, z list entries used, w frame
    CV2Tower cv2Towers[kCv2StormMaxTowers];
    uvec2    cv2StormTable[kCv2StormTableSize];     // x key (0 = empty), y (list start << 4) | count
    uint     cv2StormList[kCv2StormListSize];       // tower indices
    uvec4    cv2ShieldHead;                         // x shields, y 1 = storm objects on (shields replace the map's
                                                    // cumulonimbus as the anvils' source), z bins, w list entries used
    CV2Shield cv2Shields[kCv2ShieldMax];
    uvec2    cv2ShieldTable[kCv2ShieldTableSize];   // as cv2StormTable, kCv2ShieldBinsPerFace bins
    uint     cv2ShieldList[kCv2ShieldListSize];
    uvec4    cv2CycHead;                            // x cyclones
    CV2Cyclone cv2Cyclones[kCv2CycloneMax];
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
#endif
};
#endif

// How far a tower's storm cloud field reaches (m) from its reach (= SatelliteSimWeather.cpp stormEnvRadius).
float cv2StormEnvRadius(float reach) { return reach + max(40000.0, 0.8 * reach); }

// The bin key of a map-frame unit vector with n bins per face side (0 is never a key).
uint cv2StormBinKeyN(vec3 wd, uint n)
{
    vec3  aw = abs(wd);
    int   fc = (aw.x >= aw.y && aw.x >= aw.z) ? 0 : ((aw.y >= aw.z) ? 1 : 2);
    float mj = (fc == 0) ? wd.x : ((fc == 1) ? wd.y : wd.z);
    vec2  uv = (fc == 0) ? wd.yz : ((fc == 1) ? wd.xz : wd.xy);
    uint  face = uint(fc * 2) + ((mj < 0.0) ? 1u : 0u);
    vec2  ang = vec2(atan(uv.x / abs(mj)), atan(uv.y / abs(mj)));       // -pi/4 .. pi/4
    uvec2 ij  = uvec2(clamp(floor((ang + 0.7853982) * (float(n) / 1.5707963)), 0.0, float(n - 1u)));
    return 1u + face * n * n + ij.y * n + ij.x;
}

// The bin key of a map-frame unit vector (0 is never a key).
uint cv2StormBinKey(vec3 wd)
{
    vec3  aw = abs(wd);
    int   fc = (aw.x >= aw.y && aw.x >= aw.z) ? 0 : ((aw.y >= aw.z) ? 1 : 2);
    float mj = (fc == 0) ? wd.x : ((fc == 1) ? wd.y : wd.z);
    vec2  uv = (fc == 0) ? wd.yz : ((fc == 1) ? wd.xz : wd.xy);
    uint  face = uint(fc * 2) + ((mj < 0.0) ? 1u : 0u);
    vec2  ang = vec2(atan(uv.x / abs(mj)), atan(uv.y / abs(mj)));       // -pi/4 .. pi/4
    uvec2 ij  = uvec2(clamp(floor((ang + 0.7853982) * (float(kCv2StormBinsPerFace) / 1.5707963)),
                            0.0, float(kCv2StormBinsPerFace - 1u)));
    return 1u + face * kCv2StormBinsPerFace * kCv2StormBinsPerFace + ij.y * kCv2StormBinsPerFace + ij.x;
}

uint cv2StormHash(uint k)
{
    k ^= k >> 16; k *= 0x7FEB352Du; k ^= k >> 15; k *= 0x846CA68Bu; k ^= k >> 16;
    return k;
}

// The towers listed at this map-frame direction: start and count in cv2StormList.
uvec2 cv2StormBin(vec3 wd)
{
#ifdef CV2_STORM_BINDING
    if (cv2StormHead.x == 0u) return uvec2(0u);
    uint key = cv2StormBinKey(wd);
    uint h   = cv2StormHash(key) & (kCv2StormTableSize - 1u);
    [[dont_unroll]] for (int p = 0; p < 32; ++p) {
        uvec2 e = cv2StormTable[h];
        if (e.x == key) return uvec2(e.y >> 4u, e.y & 15u);
        if (e.x == 0u) break;
        h = (h + 1u) & (kCv2StormTableSize - 1u);
    }
#endif
    return uvec2(0u);
}

// True when the anvil shields come from the storm objects (the march and its kin bind the storm buffer).
bool cv2ShieldsOn()
{
#ifdef CV2_STORM_BINDING
    return cv2ShieldHead.y != 0u;
#else
    return false;
#endif
}

// The anvil shields over a map-frame direction: the strongest one's strength (x its taper toward the far end), its
// distance from the spine in radii (uOut; the caller frays the outline with noise, so a shield reaches 1.6 radii), and
// its thickness factor and age.
float cv2ShieldAt(vec3 wd, out float thickK, out float ageK, out float uOut)
{
    thickK = 1.0; ageK = 0.0; uOut = 9.0;
    float best = 0.0, bestS = 0.0;
#ifdef CV2_STORM_BINDING
    if (cv2ShieldHead.x == 0u) return 0.0;
    uint key = cv2StormBinKeyN(wd, kCv2ShieldBinsPerFace);
    uint h   = cv2StormHash(key) & (kCv2ShieldTableSize - 1u);
    uvec2 bin = uvec2(0u);
    [[dont_unroll]] for (int p = 0; p < 32; ++p) {
        uvec2 e = cv2ShieldTable[h];
        if (e.x == key) { bin = uvec2(e.y >> 4u, e.y & 15u); break; }
        if (e.x == 0u) break;
        h = (h + 1u) & (kCv2ShieldTableSize - 1u);
    }
    [[dont_unroll]] for (uint k = 0u; k < bin.y; ++k) {
        CV2Shield s = cv2Shields[cv2ShieldList[bin.x + k]];
        vec3  ab = s.b.xyz - s.a.xyz;
        float t  = clamp(dot(wd - s.a.xyz, ab) / max(dot(ab, ab), 1e-14), 0.0, 1.0);
        float r  = mix(s.c.x, s.b.w, t);
        float u  = length(wd - (s.a.xyz + ab * t)) * (6371000.0 / max(r, 1.0));
        if (u >= 1.6) continue;
        float sT = s.a.w * mix(1.0, 0.3, smoothstep(0.35, 1.0, t));   // thinning out toward its far (downwind) end
        if (s.d.w < 0.0) sT *= smoothstep(0.9, 1.6, u * r / -s.d.w);   // a cyclone's eye, open to the sea
        float p  = sT * (1.0 - smoothstep(0.3, 1.6, u));
        if (p > best) { best = p; bestS = sT; uOut = u; thickK = s.c.y * mix(1.0, 0.65, t); ageK = s.c.z; }
    }
#endif
    return bestS;
}

#if defined(CV2_STORM_BINDING) && defined(CV2_STORM_ENV)
// The SHIELDS whose capsule touches this (flowed) map-frame direction: start and count in cv2ShieldEnvList.
uvec2 cv2ShieldEnvBin(vec3 wd)
{
    if (cv2ShieldHead.x == 0u) return uvec2(0u);
    uint key = cv2StormBinKeyN(wd, kCv2ShieldBinsPerFace);
    uint h   = cv2StormHash(key) & (kCv2ShieldTableSize - 1u);
    [[dont_unroll]] for (int p = 0; p < 32; ++p) {
        uvec2 e = cv2ShieldEnvTable[h];
        if (e.x == key) return uvec2(e.y >> 4u, e.y & 15u);
        if (e.x == 0u) break;
        h = (h + 1u) & (kCv2ShieldTableSize - 1u);
    }
    return uvec2(0u);
}

// The towers whose ENVELOPE (cv2StormEnvRadius) touches this map-frame direction: start and count in cv2StormEnvList.
uvec2 cv2StormEnvBin(vec3 wd)
{
    if (cv2StormHead.x == 0u) return uvec2(0u);
    uint key = cv2StormBinKey(wd);
    uint h   = cv2StormHash(key) & (kCv2StormEnvTableSize - 1u);
    [[dont_unroll]] for (int p = 0; p < 32; ++p) {
        uvec2 e = cv2StormEnvTable[h];
        if (e.x == key) return uvec2(e.y >> 4u, e.y & 15u);
        if (e.x == 0u) break;
        h = (h + 1u) & (kCv2StormEnvTableSize - 1u);
    }
    return uvec2(0u);
}
#endif

#endif
