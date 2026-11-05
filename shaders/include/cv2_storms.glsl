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
#endif
};
#endif

// The dust wall at a point (its offset from the observer's sea-level point, ECEF axes, m): x the wall's strength 0..1
// (the dust x the gust front's profile across it), y its top (m above the ground), z the rain-free air ahead of the
// front 0..1. C1-smooth bilinear of four cells (linear facets of 0.4-3 km cells drew straight creases on the wall's face).
vec3 cv2DustUnpack(uint c) { return vec3(float(c & 0xFFFFu) * (1.0 / 65535.0), float((c >> 16u) & 0xFFFu) * 2.0, float(c >> 28u) * (1.0 / 15.0)); }
vec3 cv2DustWallAt(vec3 seaProjE)
{
#ifdef CV2_STORM_BINDING
    if (cv2DustHead.x == 0u) return vec3(0.0);
    vec2  g  = vec2(dot(seaProjE, cv2DustFrame[0].xyz), dot(seaProjE, cv2DustFrame[1].xyz)) * cv2DustFrame[0].w
             + cv2DustFrame[3].xy;
    vec2  lo = vec2(float(cv2DustHead.y & 0xFFFFu), float(cv2DustHead.y >> 16u));
    vec2  hi = vec2(float(cv2DustHead.z & 0xFFFFu), float(cv2DustHead.z >> 16u));
    if (any(lessThan(g, lo)) || any(greaterThanEqual(g, hi))) return vec3(0.0);
    ivec2 i  = ivec2(floor(g));
    vec2  f  = g - vec2(i);
    f = f * f * (3.0 - 2.0 * f);
    uint  N  = kCv2DustGridN;
    uint  b  = uint(i.y) * N + uint(i.x);
    return mix(mix(cv2DustUnpack(cv2DustGrid[b]), cv2DustUnpack(cv2DustGrid[b + 1u]), f.x),
               mix(cv2DustUnpack(cv2DustGrid[b + N]), cv2DustUnpack(cv2DustGrid[b + N + 1u]), f.x), f.y);
#else
    return vec3(0.0);
#endif
}

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
        if (e.x == key) return uvec2(e.y >> 5u, e.y & 31u);
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

// A tropical cyclone's upper deck (2026-11-03, issue 20261030_205220): the spiral cut into its shield (c.w < 0, packed by
// Weather.cpp cycloneSpiral; d.x hemi x the arms, d.y 1 / tan pitch, d.z the phase: the low arms' own log spirals, so the
// deck's arms stand over the weather bake's). rM: the distance from the centre (m), u: in the disc's radii. Only the
// strongest shield's (cv2ShieldAt, once a sample).
//   mode 0, the CDO: its outer part wound into the arms (the outline follows them out, notched between them);
//   mode 1, the cirrus deck: the arms' cirrus (crests of the arms, thinning outward) + minor swirls (k: three times as
//   many, a tighter pitch) + the outflow cirrus between them (o) in thin streaks curving ANTICYCLONICALLY (m: their
//   contrast), as the outflow aloft turns the other way.
float cv2CycSpiral(vec3 wd, CV2Shield s, float u, float rM)
{
    float code = -s.c.w - 1.0;
    float mode = mod(code, 2.0);  code = floor(code * 0.5);
    float k    = mod(code, 16.0) * (1.0 / 15.0);  code = floor(code * (1.0 / 16.0));
    float o    = mod(code, 16.0) * (1.0 / 15.0);  code = floor(code * (1.0 / 16.0));
    float m    = mod(code, 16.0) * (1.0 / 15.0);  code = floor(code * (1.0 / 16.0));
    float aK   = mod(code, 16.0) * (1.0 / 15.0);
    float ro   = max(floor(code * (1.0 / 16.0)) * (1.0 / 15.0), 0.05);   // the outflow's reach (disc radii)
    vec3  a    = s.a.xyz;
    vec3  e    = normalize(vec3(-a.y, a.x, 0.0) + vec3(1e-7, 0.0, 0.0));
    vec3  v    = wd - a;
    float hemi = s.d.x < 0.0 ? -1.0 : 1.0;
    float nA   = abs(s.d.x);
    float th   = hemi * atan(dot(v, cross(a, e)), dot(v, e));
    float lr   = log(max(rM, 1000.0)) * s.d.y;
    float arm  = 0.5 + 0.5 * cos(nA * (th + lr + s.d.z));
    if (mode < 0.5)
        // (The same narrow moat as the low deck under it (cloud_v2_weather.comp dkS): thin over bA < 0.35 from half the
        // radius out, untouched over the core and along the arms.)
        return 1.0 - min(1.15 * k, 0.95) * smoothstep(0.3, 0.6, u) * (1.0 - smoothstep(0.0, 0.45, arm));
    float armB = smoothstep(0.6, 0.92, arm) * (1.0 - 0.5 * smoothstep(0.3, 1.0, u));
    float mi   = smoothstep(0.7, 0.95, 0.5 + 0.5 * cos(3.0 * nA * (th + 1.4 * lr + s.d.z) + 1.7)) * (1.0 - smoothstep(0.4, 1.1, u));
    float st   = smoothstep(0.35, 0.9, 0.5 + 0.5 * cos((3.0 * nA + 2.0) * (th - 0.5 * lr) + 0.9 * s.d.z));
    // (The outflow ends where the old outflow disc's frayed edge did, ~1.25 of its reach: past it the deck is exactly 0
    // between the arms, so cv2AnvilSigmaP's early out skips its texture reads there (shot 1: +3 ms of march without).)
    // (The streaks only thin the sheet, never thicken it past the outflow's own strength: gathered to 2x on their
    // crests they crossed the shield fray's presence threshold and drew flat grey patches with hard edges between the
    // arms — the old outflow at 0.4 did the same.)
    // (x the shield's own radial fade, without its noise fray: cv2ShieldAt hands the caller u / 10 for this disc, whose
    // edge is not a cloud's edge. Frayed at 6-7.5 x the cluster noise over ~700 km, its thin sheet showed the noise's
    // contours as flat grey patches with hard edges between the arms.)
    return max(aK * max(armB, 1.2 * k * mi), o * mix(1.0, st, m) * (1.0 - smoothstep(0.9 * ro, 1.3 * ro, u)))
         * (1.0 - smoothstep(0.25, 1.25, u));
}

// The anvil shields over a map-frame direction: the strongest one's strength (x its taper toward the far end), its
// distance from the spine in radii (uOut; the caller frays the outline with noise, so a shield reaches 1.6 radii), and
// its thickness factor and age.
// It takes the shield with the largest PLATE MARGIN m = (a - 0.5) x (inside, a > 0.5: its thickness x age factors),
// a = sT x (1 - smoothstep(0.25, 1.25, u / ext)) its plate presence (storms through the cumulus field: cv2StormGeom's
// plate), not the strongest by the wider fade p = sT x (1 - smoothstep(0.3, 1.6, u)) it took until 2026-11-04. The
// plates' outline is then the union of the shields' outlines and their thickness continuous where they overlap: with p,
// a smaller shield won inside a big one's plate where it stood outside its own, a channel cut through the plate (the
// dark crease across issue 111947's anvil from 14.5 km), and the thickness stepped. Beyond every plate (m < 0) it is
// the nearest outline's shield, which the veil and the old path's presence fray read as before. (A second running
// choice beside the first, or a bool parameter choosing the metric, put the march over the 128-register cliff.)
// A tropical cyclone's CDO under this sample (2026-11-05, the user: hurricane tops "a nice smooth but textured swirl,
// right now it is a bumpy mess"): when the strongest shield is a cyclone's central dense overcast, cv2ShieldAtR reads its
// SWIRL TEXTURE into gCv2CycT (-1e9: none; else ~-1.5..1.5, ridged streaks along the flow), with the footprint the caller
// left in gCv2FpM. cv2Field adds it to the plate's margin as relief and cv2FieldLow smooths the plate. The texture is the shape volume's
// Perlin in log-polar coordinates about the eye: x along the flow (the angle, 8 periods a turn), y across it (log r,
// 6 periods an e-folding, + 8 periods a turn: an inward spiral of ~12 deg; whole periods, so seam-free). Streaks
// ~r / 25 apart (4 km at 100 km), ~5x as long as wide. Only where the shape volume is bound (CV2_CYC_TEX, clouds_v2.glsl); elsewhere 0 (smoothed, untextured). (Read here, in the storm walk,
// not in cv2FieldLow: a fetch there put the march at 250 registers.) Reset per sample by cv2Field and here.
float gCv2CycT = -1e9;
float gCv2FpM  = 0.0;
float cv2ShieldPlateA(float sT, float u) { return sT * (1.0 - smoothstep(0.25, 1.25, u / (0.25 + 0.85 * sqrt(sT)))); }
float cv2ShieldAtR(vec3 wd, out float thickK, out float ageK, out float uOut, out float rOut)
{
    thickK = 1.0; ageK = 0.0; uOut = 9.0; rOut = 1.0;
    gCv2CycT = -1e9;
    float best = -1.0, bestS = 0.0;
#ifdef CV2_STORM_BINDING
    uint  bestI = 0xFFFFFFFFu;
    if (cv2ShieldHead.x == 0u) return 0.0;
    uint key = cv2StormBinKeyN(wd, cv2ShieldHead.y);   // (y: the bins per face side the CPU listed them in)
    uint h   = cv2StormHash(key) & (kCv2ShieldTableSize - 1u);
    uvec2 bin = uvec2(0u);
    [[dont_unroll]] for (int p = 0; p < 32; ++p) {
        uvec2 e = cv2ShieldTable[h];
        if (e.x == key) { bin = uvec2(e.y >> 5u, e.y & 31u); break; }
        if (e.x == 0u) break;
        h = (h + 1u) & (kCv2ShieldTableSize - 1u);
    }
    [[dont_unroll]] for (uint k = 0u; k < bin.y; ++k) {
        uint      si = cv2ShieldList[bin.x + k];
        CV2Shield s = cv2Shields[si];
        // (A bin lists its shields strongest first, and m <= sT - 0.5 <= a.w - 0.5 (the factors are <= 1): none after
        // this can do better. Exact.)
        if (s.a.w - 0.5 <= best) break;
        vec3  ab = s.b.xyz - s.a.xyz;
        float t  = clamp(dot(wd - s.a.xyz, ab) / max(dot(ab, ab), 1e-14), 0.0, 1.0);
        float r  = mix(s.c.x, s.b.w, t);
        float u  = length(wd - (s.a.xyz + ab * t)) * (6371000.0 / max(r, 1.0));
        if (u >= 1.6) continue;
        float sT = s.a.w * mix(1.0, 0.3, smoothstep(0.35, 1.0, t));   // thinning out toward its far (downwind) end
        if (s.d.w < 0.0) sT *= smoothstep(0.9, 1.6, u * r / -s.d.w);   // a cyclone's eye, open to the sea
        float aP = cv2ShieldPlateA(sT, u);
        float p  = (aP - 0.5) * ((aP > 0.5) ? s.c.y * mix(1.0, 0.65, t) * mix(1.0, 0.5, s.c.z) : 1.0);
        if (p > best) { best = p; bestS = sT; uOut = u; rOut = r; thickK = s.c.y * mix(1.0, 0.65, t); ageK = s.c.z; bestI = si; }
    }
    // A cyclone's deck (c.w < 0, a disc: t = 0, r = c.x): its spiral, for the strongest shield only (<= 1: the bins'
    // strongest-first early out above stays exact).
    if (bestI != 0xFFFFFFFFu) {
        CV2Shield s = cv2Shields[bestI];
        if (s.c.w < 0.0 && s.d.w < 0.0) {
            bestS *= cv2CycSpiral(wd, s, uOut, uOut * s.c.x);
            if (mod(-s.c.w - 1.0, 2.0) > 0.5) { uOut *= 0.1; rOut *= 10.0; }   // (the cirrus deck: faded inside, not frayed)
        }
        // The CDO (a cyclone's eye, d.w < 0; not its cirrus deck): its swirl texture (gCv2CycT).
        if (s.d.w < 0.0 && (s.c.w >= 0.0 || mod(-s.c.w - 1.0, 2.0) < 0.5)) {
            gCv2CycT = 0.0;
#ifdef CV2_CYC_TEX
            vec3  a    = s.a.xyz;
            vec3  e    = normalize(vec3(-a.y, a.x, 0.0) + vec3(1e-7, 0.0, 0.0));
            vec3  v    = wd - a;
            float hemi = ((s.c.w < 0.0) ? s.d.x : a.z) < 0.0 ? -1.0 : 1.0;
            float th   = hemi * atan(dot(v, cross(a, e)), dot(v, e)) * (1.0 / 6.2831853);   // (turns)
            float ph   = (s.c.w < 0.0) ? s.d.z * (1.0 / 6.2831853) : 0.0;              // (the arms' phase: it turns with them)
            float rM   = max(uOut * s.c.x, 1000.0);
            // (cv2Lod, defined after this file, + 1: the ridges run round the eye, so a grazing view looks ACROSS them and
            // the footprint along the ray is several times the isotropic one; at + 0 they drew moire spokes behind the eye.)
            float lodC = max(0.0, log2(max(gCv2FpM * (6.0 / rM) * 128.0, 1e-6)) + 1.0);
            vec4  ct   = textureLod(cv2ShapeTex, vec3((th + ph) * 8.0, log(rM) * 6.0 + th * 8.0, 0.37), lodC);
            float n    = (ct.a - 0.5) * (1.0 / 0.057);
            gCv2CycT   = (1.0 - 2.0 * smoothstep(0.0, 1.2, abs(n))) * (1.0 - smoothstep(3.0, 6.0, lodC))
                       + 0.15 * (ct.g - 0.475) * (1.0 / 0.12);   // (a little of the billow; at 0.4 its cells drew scales)
            // Calm toward the eye (within ~4 eye radii: the eyewall's heads, the thickest and smoothest part): the eye's rim is a plate edge too, where cv2Field's edge boost
            // drew a fingerprint of rings round a smooth stadium wall.
            gCv2CycT  *= smoothstep(1.2, 4.0, rM / max(-s.d.w, 1000.0));
#endif
        }
    }
#endif
    return bestS;
}
float cv2ShieldAt(vec3 wd, out float thickK, out float ageK, out float uOut)
{
    float r;
    return cv2ShieldAtR(wd, thickK, ageK, uOut, r);
}

#if defined(CV2_STORM_BINDING) && defined(CV2_STORM_ENV)
// The SHIELDS whose capsule touches this (flowed) map-frame direction: start and count in cv2ShieldEnvList.
uvec2 cv2ShieldEnvBin(vec3 wd)
{
    if (cv2ShieldHead.x == 0u) return uvec2(0u);
    uint key = cv2StormBinKeyN(wd, kCv2ShieldEnvBinsPerFace);
    uint h   = cv2StormHash(key) & (kCv2ShieldTableSize - 1u);
    [[dont_unroll]] for (int p = 0; p < 32; ++p) {
        uvec2 e = cv2ShieldEnvTable[h];
        if (e.x == key) return uvec2(e.y >> 5u, e.y & 31u);
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
        if (e.x == key) return uvec2(e.y >> 5u, e.y & 31u);
        if (e.x == 0u) break;
        h = (h + 1u) & (kCv2StormEnvTableSize - 1u);
    }
    return uvec2(0u);
}
#endif

#endif
