#version 450
#extension GL_EXT_control_flow_attributes : require // [[dont_unroll]] in terrain_detail.glsl
// SKY_ENV (-DSKY_ENV -> sat_sky_env.frag.spv, 2026-09-24): the same renderer seen from a SATELLITE —
// the mesh reflection probes (SatEnvProbes: six cube faces per probe) and the model viewer's
// background. The observer (pc.obsECEFDir, w = altitude) is the satellite; everything the frame
// shares with the main view in SCREEN space is cut (the half-res cloud / depth / mesh targets, the
// satellite sky-glow bins, ocean glints, beam ground spots), the flat cloud layers stand in for the
// volumetric ones at full weight (what the main renderer draws from orbit anyway), the aurora gets
// its own short march along the ray, the sun disc and lens flare are left out (a mesh's GGX sun lobe
// is the glint), and the output is PRE-exposure HDR radiance: the post-tonemap terms (Milky Way,
// zodiacal light, moon glow, stars) are divided back by the exposure of whoever LOOKS at the result —
// the main view's for the scene probes, the model viewer's for its own — and gated by that viewer's
// sun glare (2026-09-25; they used the probe position's exposure and no glare, so a mirror showed the
// full Milky Way next to a Sun that had dimmed it everywhere else). pc.sunDirENU.w carries those two
// (w = floor(exposure·100) + glare visibility); the fragment rebuilds sin(elevation) from .z.
//
// SKY_REFL (-DSKY_ENV -DSKY_REFL -> sat_sky_refl.frag.spv, 2026-09-25): the env renderer along the
// reflected rays of one mesh instance's mirror-smooth pixels, at screen resolution — the reflection of
// a flat mirror is exact, not a cube texel stretched over many pixels. The scene mesh pass writes, per
// such pixel, the reflected direction, its weight (Fresnel × tint × fade) and the instance
// (SatMeshRenderer's reflection G-buffer, binding 25); SatMeshRenderer::recordReflections draws this
// once per qualifying instance with the instance's position as the observer and its slot + 1 in
// pc.aspect (unused by an env fragment), and adds the result into the mesh radiance.

// ── Camera + sun push constants (same layout as C++ SatDrawPC, 128 bytes) ─────
// The pipeline layout declares VK_SHADER_STAGE_VERTEX_BIT|FRAGMENT_BIT so both
// stages share one push constant range.  The fragment uses skyView/fovYRad to
// project glowBuf ENU directions into screen UV for the lens flare pass.
//
// Trimmed to 128 bytes (the maxPushConstantsSize floor). The fields that used to trail past
// offset 128 (debugDisableMask, screenSizePx, skyGlareVisibility, beamMaxRangeM, beamSkyGlowGain,
// beamGlowBleedGain, beamProximityGlow, mwSuppressEased) are all per-frame-uniform and now live
// in the CloudParams UBO this shader already binds — read below as cloud.dbgDisableMask,
// vec2(cloud.skyScreenW, cloud.skyScreenH), cloud.skyGlareVisibility, cloud.beamMaxRangeM, etc.
layout(push_constant) uniform PC {
    mat4  skyView;     // ENU -> camera space (rotation, no translation)
    float fovYRad;     // vertical field of view in radians
    float aspect;      // viewport width / height
    float gmst;        // Greenwich Mean Sidereal Time (radians)
    float waveTime;    // sim seconds for wave animation
    vec4  sunDirENU;   // xyz = sun dir in ENU, w = sin(sun elevation)
    vec4  moonDirENU;  // xyz = moon dir in ENU, w = illuminated fraction
    vec4  obsECEFDir;  // xyz = observer ECEF unit vector; w = obsHeightOffset (m)
} pc;

layout(location = 0) in  vec3 enuDir;           // interpolated ENU view ray (not normalised)
#ifdef SKY_ENV
layout(location = 1) in flat vec4 sunDirENUIn;  // w: the viewer's exposure and glare (header note)
vec4 sunDirENU;                                  // xyz, w = sin(elevation) = z — set first in main()
#else
layout(location = 1) in flat vec4 sunDirENU;    // passed through from vertex (same as pc.sunDirENU)
#endif
layout(location = 2) in flat vec4 moonDirENU;   // moon dir + phase pass-through

// Sky glow histogram, written by sat_flare.comp each frame. Must match GpuGlowBuf exactly.
// (flareCount/flareEntries[kFlareMax] lived here — the per-pixel corona loop that read them was
// deleted in the flare architecture overhaul; see FlareSourcePC's comment in SatelliteSim.h. The
// satellite corona/godray effect now comes from a render-to-texture + blur/streak pipeline
// composited once per frame in flare_composite.frag instead.)
layout(std430, set = 0, binding = 0) readonly buffer GlowBuf {
    uint  bins[64]; // sky glow: floatBitsToUint(max effectFlare) per bin
} glowBuf;

// Ocean-glint list (flare architecture overhaul) — matches GpuOceanGlintBuf exactly. Same buffer
// sat_flare.comp's binding 8 writes; read here via skyDescSet's own binding 20.
const uint kOceanGlintMax = 512; // must match kMaxOceanGlints in SatelliteSim.h and
                                  // OCEAN_GLINT_MAX in sat_flare.comp exactly.
layout(std430, set = 0, binding = 20) readonly buffer OceanGlintBuf {
    uint oceanGlintCount;
    uint oceanGlintPad[3];
    vec4 oceanGlintEntries[kOceanGlintMax]; // xyz=ENU dir, w=effectFlare
} oceanGlintBuf;

// RGBA noise texture (binding 1): tiled REPEAT sampler, used for angular corona
// variation in lensFlare().  Replaces the original ShaderToy's iChannel0 lookup.
layout(set = 0, binding = 1) uniform sampler2D noiseTex;

// Moon surface texture (binding 2): near-side face disc image.
// Sampled with an orthographic projection of the surface normal onto the moon's
// local face frame — maps the near hemisphere to the full [0,1] UV range.
layout(set = 0, binding = 2) uniform sampler2D moonTex;

// Earth textures (bindings 3-6): 8K equirectangular maps.
// UV derived from ENU hit point → ECEF → geographic lat/lon.
// earthDayTex:   SRGB colour map (auto-linearised on read).
// earthNightTex: SRGB city-light map.
// earthElevTex:  R8_UNORM land elevation. Ocean baseline = 15/255; land = (p - 15/255) * 8848 m.
// earthSpecTex:  R8_UNORM ocean mask (white=ocean, black=land). Used for wave material.
layout(set = 0, binding = 3) uniform sampler2D earthDayTex;
layout(set = 0, binding = 4) uniform sampler2D earthNightTex;
layout(set = 0, binding = 5) uniform sampler2D earthElevTex;
layout(set = 0, binding = 6) uniform sampler2D earthSpecTex;
// The clouds v2 WEATHER CUBE (cloud_v2_weather.comp; r = the map's brightness, evolved over sim time),
// in the map's drifted frame: the flat layers (the mirrors' and probes' clouds, the fallback tiers) show
// the same, evolving map the volumetric clouds are built from. It was the static 8K equirect map.
layout(set = 0, binding = 7) uniform samplerCube earthCloudsCube;

// City day/night detail textures (bindings 14/15): small tileable maps, REPEAT in both U and V.
// Blended onto dayColor/nightColor near cities (bright earthNightTex pixels) within a fixed
// distance of the observer — see the terrain block in main() below.
layout(set = 0, binding = 14) uniform sampler2D cityDayDetailTex;
layout(set = 0, binding = 15) uniform sampler2D cityNightDetailTex;

// Aurora 3D noise volume (binding 16): 1024x16x256 RGBA8, baked by aurora_noise.comp at init.
// R = curtain fold base, G/B = column-window colA/colB. See that file's header comment for the
// exact UVW layout/frequencies — the sampling code near auroraCurtainNoise/auroraSampleAt below
// must stay in sync with it.
layout(set = 0, binding = 16) uniform sampler3D auroraNoiseTex;

// ── Reflect-Orbital ground beams (C12) — written by sat_orbit.comp, read here for the
// ground-spot direct-lighting term. Capped atomic-append, no site keying/arbitration — see the
// ReflectBeamsBuf comment in sat_orbit.comp for the full history/rationale.
// Struct must match GpuReflectBeam/GpuReflectBeams in SatelliteSim.h and sat_orbit.comp exactly.
#include "reflect_beam.glsl"   // ReflectBeam + BEAM_MAX_ACTIVE
layout(std430, set = 0, binding = 17) readonly buffer ReflectBeamsBuf {
    uint         beamCount;
    uint         beamPad0, beamPad1, beamPad2;
    ReflectBeam  beams[BEAM_MAX_ACTIVE];
};

// Ground-beam compaction (perf follow-up): CPU-built every frame from a fresh readback of
// ReflectBeamsBuf above, filtered to just the entries within cloud.beamMaxRangeM of the observer —
// the exact test the ground-spot loop below used to redo, unconditionally, against the FULL raw
// list (up to BEAM_MAX_ACTIVE=2048 entries) for every ground-hit pixel. Consuming this instead
// bounds that loop's trip count to however many beams are actually close enough to matter, capped
// at GROUND_BEAM_MAX. See the CPU aggregation next to lastActiveBeamCount/beamProximityGlow in
// SatelliteSim.cpp, and GpuGroundBeams in SatelliteSim.h.
//
// 2026-08-10: these are no longer raw ReflectBeam records. Everything in the old loop body that did
// not vary per pixel — the range fade, the elevation fade, the shadow attenuation, and the
// obsPos+satENU / raySphere solve for the beam's real ray/ground intersection — is now folded on
// the CPU into `weight` plus a few reciprocals, once per beam instead of once per ground-hit pixel
// at full resolution. Must match GpuGroundBeam in SatelliteSim.h exactly (hand-mirrored).
struct GroundBeam {
    vec2  groundHitXY;    // observer-relative ENU horizontal position of the REAL landing spot
    float invFootprintSq; // 1 / footprintR^2
    float invCoreSq;      // 1 / coreR^2
    float cutoffSq;       // (footprintR * 1.1)^2 — the loop's first and cheapest reject (the spot is a disk)
    float weight;         // intensity * rangeFade * elevFade * shadowAtten
    float intensity;      // CPU-side top-K ranking only; deliberately unread here
    float pad0;
};
layout(std430, set = 0, binding = 21) readonly buffer GroundBeamsBuf {
    uint        groundBeamCount;
    uint        groundBeamPad0, groundBeamPad1, groundBeamPad2;
    GroundBeam  groundBeams[GROUND_BEAM_MAX];
};

// (binding 18 was cloudShadowTex, cloud_shadow.comp's 128x128 grid. That whole pass is gone —
//  cloud_march.comp now writes a per-pixel shadow into cloudB.a. Bindings 19/20 were compacted
//  down into 18/19 rather than leaving a hole, since the C++ side fills its binding array
//  contiguously.)

// Cloud 3D noise volume (binding 8): 128³ RGBA, baked by cloud_noise.comp at init.
// R = presence (Perlin-Worley), G = pre-summed erosion FBM, B/A = presence at +54/+108 texels
// in Z. See common.glsl's channel-layout block — this packing changed in T2.1, and the only
// reader left in THIS file is the CLOUD_DEBUG==6 visualization below.
layout(set = 0, binding = 8) uniform sampler3D cloudNoiseTex;

// CloudParams UBO + CloudLayer come from the shared header. This block used to be
// hand-copied here; see cloud_params.glsl for why that was a standing hazard.
#define CLOUD_PARAMS_BINDING 9
#include "cloud_params.glsl"

// ── Perf knockout toggles (profiling-only) ──────────────────────────────────────
// Lets the Display settings tab measure the isolated GPU cost of individual blocks of this shader
// via gpuMsSmoothed deltas (App::drawFrame's timestamp query), without a GPU capture tool. Default
// (mask 0) is bit-identical to normal rendering. The mask moved from the push constant into the
// CloudParams UBO (cloud.dbgDisableMask) so this shader's push-constant range fits 128 bytes —
// hence these helpers sit after the #include above, where `cloud` is in scope.
bool dbgSkipTerrain()    { return (cloud.dbgDisableMask & 1u) != 0u; }
bool dbgSkipAtmosphere() { return (cloud.dbgDisableMask & 2u) != 0u; }
bool dbgSkipSunOD()      { return (cloud.dbgDisableMask & 4u) != 0u; }
bool dbgSkipOceanRefl()  { return (cloud.dbgDisableMask & 8u) != 0u; }

// Half-resolution cloud march output (written by cloud_march.comp, see the "velvet-rolling-
// squirrel" plan / TERRAIN_PLAN.md session 23 log). Replaces the old inline cirrusMarch()/
// cloudMarch() calls in main() below — those functions moved to that compute shader.
// Target A: rgb = combined additive radiance (B_total), a = tCloudOcclude (m, -1 = none).
// Target B: rgb = combined multiplicative attenuation (A_total), a = cloudBlock (sun-dim scalar).
layout(set = 0, binding = 10) uniform sampler2D cloudTargetA;
layout(set = 0, binding = 11) uniform sampler2D cloudTargetB;

// Light pollution dome (binding 12): same buffer as sat_flare.comp's binding 3, CPU-written each
// frame, TWO halves of 16 azimuth sectors each (see SatelliteSim::uploadLightDome).
//   lightDome[]      — gain-scaled linear city brightness; what satellites/stars are dimmed by.
//   lightDomeEased[] — the DARK-SKY dome: the same sectors' RAW (pre-lightPollutionGain) brightness
//                      log-mapped to a normalized [0,1] pristine->inner-city axis and temporally
//                      eased. Feeds darkSkySkyMag() for the Milky Way and zodiacal light below,
//                      and for the aurora in cloud_march.comp.
// sat_flare.comp declares only the first half and is unaffected by the growth.
layout(std430, set = 0, binding = 12) readonly buffer LightDomeBuf {
    float lightDome[16];
    float lightDomeEased[16];
};

// Milky Way skybox (binding 13): 8K equirectangular galactic panorama. Sampled against the
// CPU-computed ENU->galactic basis in cloud.mwBasisRow0/1/2 (see updatePositions() in
// SatelliteSim.cpp for how the fixed orientation, including the longitude mirror, is built).
layout(set = 0, binding = 13) uniform sampler2D milkyWayTex;

// Beam-driven sky-glow suppression dome (binding 19, C12 follow-up #31): same buffer as
// sat_orbit.comp's binding 5 / sat_flare.comp's binding 4 — a SECOND, independent 16-sector dome,
// populated by active Reflect-Orbital beams instead of a static night-lights texture. Stored as
// raw atomicMax'd uint bit-patterns (floatBitsToUint on the write side) — reinterpret via
// uintBitsToFloat, NOT a direct float read like LightDomeBuf above.
layout(std430, set = 0, binding = 18) readonly buffer BeamGlowDomeBuf {
    uint beamGlowDome[16];
};

// Shared scene depth (binding 19) — linear metres to the first terrain/ocean surface along each
// view ray, or kNoSurfaceT for rays that reach space. Written by scene_depth.comp earlier in the
// same recordCompute; this binding was already wired on the C++ side (layout/pool/onResize) but
// never actually declared/sampled here until now (2026-07-29) — used below to test whether
// terrain blocks a lens-flare source's own direction, not this fragment's view ray.
layout(set = 0, binding = 19) uniform sampler2D sceneDepthTex;
// x = the observer's eye ground height with terrain detail, computed once per frame by
// scene_depth.comp (terrain_detail.glsl observerEffHeightDetailed).
layout(set = 0, binding = 26, std430) readonly buffer TerrainFrame { vec4 terrainFrame; };
// Phase 4c — satellite meshes (SatMeshRenderer's scene pass, full swap extent). Storage images, not
// samplers: this shader is one binding from the 16 sampled-image floor (CLAUDE.md, hardware table).
layout(set = 0, binding = 22, rgba32f) uniform readonly image2D meshColorImg; // pre-exposure radiance
layout(set = 0, binding = 23, r32f)    uniform readonly image2D meshDistImg;  // true distance, 0 = none
#ifdef SKY_ENV
// The star catalogue for the env renderer (2026-09-25; the main view draws stars as points, which a
// probe or a mirror never saw): SatelliteSim::createStarEnvBuffer. starHdr0 = the point model
// (point_style.glsl: refMag, gamma, limitMag, sigmaPx), starHdr1 = x the main view's pixel angle (rad),
// y the widest sigma drawn here (px), z the grid size, w unused. starRec[2i] = ECI direction + visual
// magnitude, [2i+1] = colour. starCells = per cube cell (6·G·G, ECI, cubeCell below) the first entry,
// then one past the last (so 6·G·G + 1 offsets), then the star indices; each star is listed in every
// cell within the widest PSF's reach, so a lookup reads one cell and no seam can cut a star.
#define STAR_ENV_COUNT 8404 // = the catalogue (static_assert in SatelliteSim.cpp)
layout(std430, set = 0, binding = 24) readonly buffer StarEnvBuf {
    vec4 starHdr0;
    vec4 starHdr1;
    vec4 starRec[STAR_ENV_COUNT * 2];
    uint starCells[];
};
#endif
#ifdef SKY_REFL
layout(set = 0, binding = 25, rgba32ui) uniform readonly uimage2D reflGbuf; // SatMeshRenderer
#endif

layout(location = 0) out vec4 outColor;

// PI, R_EARTH, R_ATMOS, BETA_R/H_R, BETA_M/H_M/G_MIE, SUN_INTENSITY, kCloudHorizFreq/kCloudColFreq,
// raySphere, rotateZ, remap, phaseR/phaseM/phaseCloud and the scene-depth sentinels all live in
// the shared header now. terrain.glsl brings the DEM decode + observer-frame helpers.
#include "common.glsl"
#include "terrain.glsl"
#include "terrain_detail.glsl" // procedural 3D terrain detail + the shared march
#include "atmosphere.glsl" // line-of-sight extinction (atmExtinctionMag)
#include "darksky.glsl"   // dark-sky exposure gate (Milky Way / zodiacal)
#include "depth.glsl"     // unified scene depth (gl_FragDepth)

// ── Close-up terrain materials (terrain v2 P3, binding 27) ─────────────────────────────────────
// tools/make_terrain_materials.py: two layers per material — 2m: albedo as a linear RATIO to the
// material's own mean x 0.4 (the day map stays the colour, the texture adds its structure) + height;
// 2m+1: normal XY (GL) + roughness + AO. Order = kTmGrass .. kTmDirt (keep in step with the tool).
// The 16th and last sampled image of this stage (the guaranteed floor).
layout(set = 0, binding = 27) uniform sampler2DArray terrainMatTex;
const int kTmGrass = 0, kTmForest = 1, kTmRock = 2, kTmSnow = 3, kTmSand = 4, kTmDirt = 5;
const float kTmTileM = 4.0;   // metres per texture repeat; divides the 2048-m anchor cell (world-fixed)

// One material, biplanar in ECEF on the anchored lattice (p = anchorRel + the ECEF offset, in tiles):
// the two projections along n's two largest axes, weighted by |n| on them. Explicit LOD from the
// footprint (derivatives are undefined in this divergent branch). out: ratio rgb + height (a),
// and the normal's tangential perturbation in ECEF (xyz) + AO (w).
void tmSample(int m, vec3 p, vec3 n, float lod, out vec4 alb, out vec4 nrm) {
    vec3  an = abs(n);
    ivec3 ma = (an.x > an.y && an.x > an.z) ? ivec3(0, 1, 2) : (an.y > an.z) ? ivec3(1, 2, 0) : ivec3(2, 0, 1);
    ivec3 mi = (an.x < an.y && an.x < an.z) ? ivec3(0, 1, 2) : (an.y < an.z) ? ivec3(1, 2, 0) : ivec3(2, 0, 1);
    ivec3 me = ivec3(3) - mi - ma;
    vec2  uvA = vec2(p[ma.y], p[ma.z]), uvB = vec2(p[me.y], p[me.z]);
    vec2  w   = clamp((vec2(an[ma.x], an[me.x]) - 0.5773) / (1.0 - 0.5773), 0.0, 1.0);
    w = w * w * w * w;
    w /= max(w.x + w.y, 1e-4);
    float la = float(2 * m), ln = float(2 * m + 1);
    vec4  aA = textureLod(terrainMatTex, vec3(uvA, la), lod), aB = textureLod(terrainMatTex, vec3(uvB, la), lod);
    vec4  nA = textureLod(terrainMatTex, vec3(uvA, ln), lod), nB = textureLod(terrainMatTex, vec3(uvB, ln), lod);
    alb = aA * w.x + aB * w.y;
    alb.rgb *= 2.5;                                   // ratio x 0.4 -> ratio
    // Tangent normal -> ECEF: the texture's x/y run along the projection's two axes; mirrored faces
    // (n negative on the projection axis) flip x so the relief keeps its handedness.
    vec2  tA = nA.xy * 2.0 - 1.0, tB = nB.xy * 2.0 - 1.0;
    tA.x *= sign(n[ma.x] + 1e-6);
    tB.x *= sign(n[me.x] + 1e-6);
    vec3  dA = vec3(0.0), dB = vec3(0.0);
    dA[ma.y] = tA.x; dA[ma.z] = tA.y;
    dB[me.y] = tB.x; dB[me.z] = tB.y;
    nrm = vec4(dA * w.x + dB * w.y, nA.w * w.x + nB.w * w.y);
}

// ── Procedural city lights (.plans/CITIES_PLAN.md, phase 1) ────────────────────────────────────
// The night map (5 km texels) says WHERE the lights are and how bright; this says how they are laid
// out: a street grid per district, lamps along the streets, arterials on the district borders, and
// the real major roads (Natural Earth, binding 28). The result is a MULTIPLIER with mean 1 over any
// area much larger than a block, so from orbit the night map is unchanged, and every feature is
// filtered analytically to the pixel footprint (lamps merge into lines, lines into a uniform glow):
// no aliasing, and no cost once it is uniform.
layout(std430, set = 0, binding = 28) readonly buffer CityRoadsBuf { uint roadWords[]; };

const float kCityDistrictM = 2048.0;   // the anchor cell (world-fixed, exact); districts are two of them
// Street lighting is lamp posts, not glowing streets (the user, 2026-09-29): each post lights a pool on
// the road (kCityPoolM) with a small bright head (kCityHeadM, kCityHeadShare of its light); posts every
// kCityPostM along a street (kCityPostArtM on arterials). Lamps stay separate until the pixel cannot
// resolve them, and only then merge into lines, then into a uniform glow.
const float kCityPoolM     = 6.0;
const float kCityHeadM     = 1.2;
const float kCityHeadShare = 0.3;
const float kCityPostM     = 35.0;
const float kCityPostArtM  = 30.0;
const float kCityArtBoost  = 3.5;      // an arterial's light per metre vs a residential street's (5 saturated
                                       // into solid lines: the posts merged in the tonemap)

float cityG1(float d, float sg) { return exp(-0.5 * d * d / (sg * sg)) * (0.39894228 / sg); }

// Distance (metres) from pC to the major roads near uv, as the summed filtered line light.
// Everything is taken in the anchor CELL's frame: a stored float ECEF endpoint minus the cell origin
// (a multiple of 2048 m, exact in float) is exact, so the lines are world-fixed.
float cityRoadLight(vec2 uv, vec3 pC, float foot) {
    uint W = roadWords[1], H = roadWords[2];
    if (W == 0u) return 0.0;
    uvec2 c   = uvec2(clamp(uv * vec2(float(W), float(H)), vec2(0.0), vec2(float(W) - 1.0, float(H) - 1.0)));
    uint cell = c.y * W + c.x;
    uint offBase = 4u, idxBase = 4u + W * H + 1u, segBase = idxBase + roadWords[4u + W * H];
    uint i0 = roadWords[offBase + cell], i1 = min(roadWords[offBase + cell + 1u], i0 + 64u);
    vec3  org = vec3(cloud.terrainAnchorCell.xyz) * kTdBaseCellM;
    float e = 0.0;
    for (uint i = i0; i < i1; ++i) {
        uint  sg = segBase + roadWords[idxBase + i] * 8u;
        vec3  a  = vec3(uintBitsToFloat(roadWords[sg]), uintBitsToFloat(roadWords[sg + 1u]),
                        uintBitsToFloat(roadWords[sg + 2u])) - org;
        vec3  b  = vec3(uintBitsToFloat(roadWords[sg + 4u]), uintBitsToFloat(roadWords[sg + 5u]),
                        uintBitsToFloat(roadWords[sg + 6u])) - org;
        float cls = uintBitsToFloat(roadWords[sg + 3u]);
        float wid = uintBitsToFloat(roadWords[sg + 7u]);
        vec3  ab = b - a;
        float len = max(length(ab), 1.0);
        float t  = clamp(dot(pC - a, ab) / (len * len), 0.0, 1.0);
        float d  = length(pC - (a + t * ab));
        float sgm = sqrt(0.25 * wid * wid + 0.25 * foot * foot);
        if (d > 4.0 * sgm) continue;
        // Light per metre of road, in "area units" (x the metres over which it counts as the city's
        // mean): major highways the brightest. Lit by posts every 40 m (dotted until the pixel cannot
        // resolve them), as the streets are.
        float k   = (cls > 1.5) ? 700.0 : (cls > 0.5 ? 450.0 : 250.0);
        float eLn = k * cityG1(d, sgm);
        float sgP = sqrt(kCityPoolM * kCityPoolM + 0.25 * foot * foot);
        float tLn = smoothstep(0.3 * 40.0, 0.55 * 40.0, sgP);
        float ePt = 0.0;
        if (tLn < 1.0) {
            float al = t * len, m = floor(al / 40.0 + 0.5) * 40.0;
            for (int j = -1; j <= 1; ++j) {
                float da = al - m - float(j) * 40.0;
                ePt += exp(-0.5 * (d * d + da * da) / (sgP * sgP));
            }
            ePt *= k * 40.0 * 0.15915494 / (sgP * sgP);
        }
        e = max(e, mix(ePt, eLn, tLn));
    }
    return e;
}

// Lamp colours (luminance ~1): high-pressure sodium, warm-white LED (4000 K), cool LED (5000 K) and
// metal halide; neon for the rare commercial signs.
const vec3 kLampSodium = vec3(1.28, 0.90, 0.42);
const vec3 kLampLedW   = vec3(1.04, 0.99, 0.90);
const vec3 kLampLedC   = vec3(0.90, 0.98, 1.14);
const vec3 kLampHalide = vec3(1.02, 1.00, 0.96);
vec3 cityLampColor(float h, float ledP) {
    if (h < 1.0 - ledP) return kLampSodium;
    float t = (h - (1.0 - ledP)) / max(ledP, 1e-3);
    return t < 0.55 ? kLampLedW : (t < 0.85 ? kLampLedC : kLampHalide);
}
vec3 cityLampMean(float ledP) {
    return (1.0 - ledP) * kLampSodium + ledP * (0.55 * kLampLedW + 0.30 * kLampLedC + 0.15 * kLampHalide);
}

// One street direction at coordinate x (base spacing 1; an even line exists with probability keepE, an
// odd one keepO — both fall continuously with density, so streets thin out gradually instead of the
// grid switching stride at a density contour; every artN-th line an arterial that always exists),
// along-coordinate y. Returns the coloured lamp light (resolved posts) mixed toward
// lines by tL, in "weight x 1/m^2 per base-unit^2" before the caller's normalisation. sgPoolU / sgHeadU:
// sigmas in base units; postU / postArtU: post spacing in base units. ledP: the share of LED posts
// (cores and arterials whiter — the user, 2026-09-29); signP: neon signs among arterial posts.
vec3 cityStreetDir(float x, float y, float sgPoolU, float sgHeadU, float postU, float postArtU,
                   float keepE, float keepO, float artN, int salt, float tL, float ledP, float signP) {
    float n0 = floor(x + 0.5);
    vec3  e  = vec3(0.0);
    for (int j = -1; j <= 1; ++j) {
        float ni  = n0 + float(j);
        bool  art = mod(ni, artN) < 0.5;
        if (!art && tdRand3(ivec3(int(ni), salt, 0)).x * 0.5 + 0.5 > (mod(ni, 2.0) > 0.5 ? keepO : keepE)) continue;
        float w   = art ? kCityArtBoost : 1.0;
        float ps  = art ? postArtU : postU;
        float lp  = art ? min(1.0, ledP + 0.25) : ledP;
        float dx  = x - ni;
        // Lines: light per unit length w / ps, spread over the pool sigma, in the street's mean colour.
        vec3  eL  = (w / ps) * cityG1(dx, sgPoolU) * cityLampMean(lp);
        vec3  eP  = vec3(0.0);
        if (tL < 1.0) {
            float m  = floor(y / ps + 0.5) * ps;
            float mi = floor(y / ps + 0.5);
            vec3  pool = vec3(0.0), head = vec3(0.0);
            for (int k = -2; k <= 2; ++k) {
                float dy = y - m - float(k) * ps;
                float r2 = dx * dx + dy * dy;
                // Each post its own: 0.5-1.5x, one in ten dark (mean 1: / 0.9), its own lamp type;
                // on arterials, now and then a neon sign.
                vec3  ph = tdRand3(ivec3(int(ni), int(mi) + k, salt + 17));
                vec3  ph2 = tdRand3(ivec3(int(ni), int(mi) + k, salt + 29)) * 0.5 + 0.5;
                float pw = (ph.x > -0.8) ? (1.0 + 0.5 * ph.y) / 0.9 : 0.0;
                vec3  c  = cityLampColor(ph2.x, lp);
                if (art && ph2.y < signP)
                    c = (ph2.z < 0.33) ? vec3(2.2, 0.35, 0.45) : (ph2.z < 0.66 ? vec3(0.35, 0.8, 2.1) : vec3(0.6, 1.8, 0.8));
                pool += pw * c * exp(-0.5 * r2 / (sgPoolU * sgPoolU));
                head += pw * c * exp(-0.5 * r2 / (sgHeadU * sgHeadU));
            }
            eP = w * 0.15915494 * ((1.0 - kCityHeadShare) * pool / (sgPoolU * sgPoolU)
                                   + kCityHeadShare * head / (sgHeadU * sgHeadU));
        }
        e += mix(eP, eL, tL);
    }
    return e;
}

// World-fixed 2D value noise on a lattice of `cellM` metres (a divisor of the 4096-m district, so the
// lattice index is exact from the district anchor): C1-smooth bilinear of hashed corners, in [-1, 1].
float cityValue2(vec2 p2, ivec2 dAnc, float cellM, int salt) {
    int   per = int(4096.0 / cellM + 0.5);
    vec2  x   = p2 / cellM;
    vec2  fl  = floor(x), fr = x - fl;
    ivec2 i   = dAnc * per + ivec2(fl);
    vec2  w   = fr * fr * (3.0 - 2.0 * fr);
    float a = tdRand3(ivec3(i, salt)).x, b = tdRand3(ivec3(i + ivec2(1, 0), salt)).x;
    float c = tdRand3(ivec3(i + ivec2(0, 1), salt)).x, d = tdRand3(ivec3(i + ivec2(1, 1), salt)).x;
    return mix(mix(a, b, w.x), mix(c, d, w.x), w.y);
}

// ── The city layout, shared by the night lights and the day albedo ──────────────────────────────
// One district's street grid.
struct CityGrid {
    vec2  u;        // street-grid coordinates (base spacing 1)
    float sp;       // base street spacing (m)
    float artN;     // every artN-th base line is an arterial
    int   saltX, saltY;
};

struct CityLayout {
    CityGrid g;     // the pixel's own district's grid
    CityGrid g2;    // the neighbour's, when it differs and the pixel is within kCityBlendM of the border
    float w2;       // g2's weight (0 .. 0.5 at the border): the two cross-fade, nothing clips (night)
    float db;       // metres to the district border
    bool  differ;   // the two districts' grids differ (another region, or independent)
    float keepE, keepO;  // street existence (even / odd base lines), from the density
    float dens;     // 0 suburb .. 1 core: the night map's light AT THE PIXEL (continuous)
    vec2  p2;       // metres in the district-anchored face frame
    ivec2 dAnc;
    int   f;
    float dSeam;    // metres to the projection's face seam
    float voidN;    // < ~-0.45: a dark void / park
    float lum;      // the night map's light (base removed) at the pixel
    vec3  rel;      // metres from the anchor cell origin (ECEF)
};
const float kCityBlendM = 80.0;

// Does base line ni exist: 0 no, 1 street, 2 arterial.
int cityLineKind(float ni, float keepE, float keepO, float artN, int salt) {
    if (mod(ni, artN) < 0.5) return 2;
    return tdRand3(ivec3(int(ni), salt, 0)).x * 0.5 + 0.5 > (mod(ni, 2.0) > 0.5 ? keepO : keepE) ? 0 : 1;
}

// The world-fixed 2D frame the city and farm layouts live in: the erosion's face projection (drop the
// dominant axis of up), anchored in 4096-m cells (2 anchor cells: the index is (anchor + offset) / 2,
// formed as integers — the anchor's low bit carried into the float part — so it stays world-fixed). p2
// = metres from the district-anchor origin dAnc * 4096. Any lattice whose cell divides 4096 m is exact.
void cityFrame(vec3 q, vec3 enuX, vec3 enuY, vec3 enuZ, out vec2 p2, out ivec2 dAnc, out int f,
               out vec3 rel, out vec3 aup) {
    vec3  offS = tdSphereOffset(q);
    rel  = cloud.terrainAnchorRel.xyz + offS.x * enuX + offS.y * enuY + offS.z * enuZ;
    aup  = abs(tdUpECEF(q, enuX, enuY, enuZ));
    f    = (aup.x > aup.y && aup.x > aup.z) ? 0 : (aup.y > aup.z ? 1 : 2);
    ivec2 ax   = tdFaceAxes(f);
    ivec3 anchor = ivec3(cloud.terrainAnchorCell.xyz);
    ivec2 anc2 = ivec2(anchor[ax.x], anchor[ax.y]);
    p2   = vec2(rel[ax.x], rel[ax.y]) + vec2(anc2 & 1) * kCityDistrictM;
    dAnc = anc2 >> 1;
}

// The grid of district `id` (centre cc, hashes r / rB): ONE grid per region of 4x4 districts (16 km)
// measured from the region's origin, so streets run straight through district borders; a fifth of the
// districts lay out their own (independent) grid. Street existence is hashed per REGION (per district
// for independent ones), so it does not change at a border.
CityGrid cityGridOf(ivec2 id, vec2 cc, vec3 r, vec3 rB, vec2 p2, ivec2 dAnc, int f, float dens, out bool indep, out ivec2 regId) {
    const float kD = 2.0 * kCityDistrictM;
    regId = id >> 2;
    vec3  rReg  = tdRand3(ivec3(regId, 6089 + f));
    vec3  rReg2 = tdRand3(ivec3(regId, 6091 + f));
    indep = rB.z > 0.6;
    float ang   = (rReg.x * 0.5 + 0.5) * 1.5708;
    float sp    = 90.0 + 30.0 * (rReg.y * 0.5 + 0.5);                  // base street spacing (m)
    vec2  org   = vec2(regId * 4 - dAnc) * kD;                          // region origin, p2 frame (exact)
    vec2  phase = rReg2.xy * 13.7;
    float artN  = 6.0 + floor(3.0 * (rReg.z * 0.5 + 0.5));              // every 6th-8th base line
    if (indep) {
        ang   = (r.x * 0.5 + 0.5) * 1.5708;
        sp    = 90.0 + 40.0 * (r.z * 0.5 + 0.5);
        org   = cc;
        phase = rB.xy * 17.3;
        artN  = 6.0 + floor(3.0 * (rB.x * 0.5 + 0.5));
    }
    vec2  cs = vec2(cos(ang), sin(ang));
    vec2  dp = p2 - org;
    CityGrid g;
    g.u  = vec2(dot(dp, cs), dot(dp, vec2(-cs.y, cs.x))) / sp + phase;
    // Independent suburbs bend gently (organic layouts).
    if (indep && dens < 0.6)
        g.u += 0.35 * vec2(sin(g.u.y * 0.21 + rB.x * 6.0), sin(g.u.x * 0.17 + rB.y * 6.0));
    g.sp = sp; g.artN = artN;
    g.saltX = 101 + (indep ? id.x * 7 + id.y * 131 : regId.x * 7 + regId.y * 131);
    g.saltY = 202 + (indep ? id.x * 13 + id.y * 71 : regId.x * 13 + regId.y * 71);
    return g;
}

// The layout at a terrain hit: q (terrain_detail.glsl's q) and the night map's light there (base
// removed: the density, per pixel — per district it jumped at every border).
CityLayout cityLayout(vec3 q, vec3 enuX, vec3 enuY, vec3 enuZ, float lum) {
    CityLayout L;
    vec2  p2;
    ivec2 dAnc;
    int   f;
    vec3  rel, aup;
    cityFrame(q, enuX, enuY, enuZ, p2, dAnc, f, rel, aup);
    const float kD = 2.0 * kCityDistrictM;
    float dens = smoothstep(0.004, 0.12, lum);

    // Districts: jittered Voronoi, the nearest two.
    vec2  cf = floor(p2 / kD);
    float d1 = 1e9, d2 = 1e9;
    vec2  c1 = vec2(0.0), c2 = vec2(0.0);
    vec3  r1 = vec3(0.0), r2 = vec3(0.0);
    ivec2 id1 = ivec2(0), id2 = ivec2(0);
    for (int k = 0; k < 9; ++k) {
        vec2  o  = vec2(float(k % 3 - 1), float(k / 3 - 1));
        ivec2 id = dAnc + ivec2(cf + o);
        vec3  r  = tdRand3(ivec3(id, 7717 + f));
        vec2  cc = (cf + o + 0.5 + 0.38 * r.xy) * kD;
        float d  = dot(p2 - cc, p2 - cc);
        if (d < d1) { d2 = d1; c2 = c1; r2 = r1; id2 = id1; d1 = d; c1 = cc; r1 = r; id1 = id; }
        else if (d < d2) { d2 = d; c2 = cc; r2 = r; id2 = id; }
    }
    bool  ind1, ind2;
    ivec2 reg1, reg2;
    L.g  = cityGridOf(id1, c1, r1, tdRand3(ivec3(id1, 3301 + f)), p2, dAnc, f, dens, ind1, reg1);
    L.w2 = 0.0;
    L.g2 = L.g;
    L.differ = false;
    float db = abs(dot(p2 - 0.5 * (c1 + c2), normalize(c2 - c1)));
    L.db = db;
    if (db < kCityBlendM) {
        CityGrid g2 = cityGridOf(id2, c2, r2, tdRand3(ivec3(id2, 3301 + f)), p2, dAnc, f, dens, ind2, reg2);
        if (ind1 || ind2 || reg1 != reg2) {
            L.g2 = g2;
            L.w2 = 0.5 * (1.0 - smoothstep(0.0, kCityBlendM, db));
            L.differ = true;
        }
    }
    float a2 = (f == 0) ? max(aup.y, aup.z) : (f == 1 ? max(aup.x, aup.z) : max(aup.x, aup.y));
    L.dSeam = (aup[f] - a2) * R_EARTH * 0.7071;
    // Streets thin out with density: fewer odd lines first (a coarser grid), then gaps in all of them.
    float drop = mix(0.65, 0.05, dens);
    L.keepE = 1.0 - drop;
    L.keepO = (1.0 - drop) * smoothstep(0.15, 0.45, dens);
    L.dens = dens; L.p2 = p2; L.dAnc = dAnc; L.f = f; L.rel = rel; L.lum = lum;
    // Dark voids (parks, hillsides, rail yards): soft irregular patches over ~15% of the area.
    // (256/512-m cells with a narrow edge read as ink spots.)
    L.voidN = cityValue2(p2, dAnc, 1024.0, 4441 + f) * 0.7 + cityValue2(p2, dAnc, 512.0, 4443 + f) * 0.3;
    return L;
}

// The share of low-density blocks that are FIELDS (the city thins into farmland: patches between the
// houses, not a hard edge — the user, 2026-09-29).
float cityFieldP(float dens) { return 0.8 * (1.0 - smoothstep(0.03, 0.4, dens)); }

// The lamp light of one grid (street posts, traffic lights, block glow) x its commercial-strip macro,
// normalised to mean ~1, in colour. The caller cross-fades two grids at a border.
vec3 cityNightGrid(CityGrid g, CityLayout L, float foot, float ledP, vec3 meanC, out float tUo) {
    vec2  u = g.u;
    float sp = g.sp, artN = g.artN;
    float fh     = 0.25 * foot * foot;
    float sgPool = sqrt(kCityPoolM * kCityPoolM + fh) / sp;
    float sgHead = sqrt(kCityHeadM * kCityHeadM + fh) / sp;
    float postU  = kCityPostM / sp, postArtU = kCityPostArtM / sp;
    float tL     = smoothstep(0.3 * postU, 0.55 * postU, sgPool);       // posts -> lines
    float coarse = mix(2.0, 1.0, L.keepO / max(L.keepE, 1e-3));         // effective line spacing
    float tU     = smoothstep(0.3 * coarse, 0.6 * coarse, sgPool);      // lines -> uniform
    tUo = tU;
    // Mean of the lamp light per base unit^2: two directions x (light per unit length summed over the
    // lines of one base unit). Normalises the grid to mean 1.
    float linesW = kCityArtBoost / (artN * postArtU)
                 + (1.0 - 1.0 / artN) * 0.5 * (L.keepE + L.keepO) / postU;
    vec3  eGridC = meanC;
    if (tU < 1.0) {
        vec3 eg = cityStreetDir(u.x, u.y, sgPool, sgHead, postU, postArtU, L.keepE, L.keepO, artN, g.saltX, tL, ledP, 0.04)
                + cityStreetDir(u.y, u.x, sgPool, sgHead, postU, postArtU, L.keepE, L.keepO, artN, g.saltY, tL, ledP, 0.04);
        eGridC = mix(eg / (2.0 * linesW), meanC, tU);
        // Traffic lights where two arterials cross: four heads at the corners, cycling green / amber / red
        // with sim time (each intersection its own phase). Small and dim: only seen close up.
        vec2 ni = floor(u + 0.5);
        if (tL < 1.0 && mod(ni.x, artN) < 0.5 && mod(ni.y, artN) < 0.5) {
            float ph  = fract(pc.waveTime / 60.0 + tdRand3(ivec3(ivec2(ni), 5003 + L.f)).x);
            vec3  sig = ph < 0.45 ? vec3(0.2, 2.4, 0.9) : (ph < 0.5 ? vec3(2.2, 1.4, 0.1) : vec3(2.6, 0.15, 0.1));
            vec3  sigX = ph < 0.45 ? vec3(2.6, 0.15, 0.1) : (ph < 0.5 ? vec3(2.6, 0.15, 0.1) : vec3(0.2, 2.4, 0.9));
            for (int c = 0; c < 4; ++c) {
                vec2  cr = vec2((c & 1) == 0 ? -1.0 : 1.0, (c & 2) == 0 ? -1.0 : 1.0) * (14.0 / sp);
                vec2  d  = u - ni - cr;
                float gg = exp(-0.5 * dot(d, d) / (sgHead * sgHead)) * 0.15915494 / (sgHead * sgHead);
                eGridC  += ((c == 0 || c == 3) ? sig : sigX) * gg * (0.004 / (2.0 * linesW));
            }
        }
    }
    // Lit windows and lots between the streets: a faint glow varying per block; none in field blocks.
    vec3  bh    = tdRand3(ivec3(ivec2(floor(u)), 911 + L.f));
    float fieldB = (tdRand3(ivec3(ivec2(floor(u)), 919 + L.f)).x * 0.5 + 0.5) < cityFieldP(L.dens) ? 0.0 : 1.0;
    float block = mix(0.2 + 1.6 * (bh.x * 0.5 + 0.5) * (bh.y * 0.5 + 0.5) * 2.0 * fieldB / max(1.0 - cityFieldP(L.dens), 0.2),
                      1.0, smoothstep(0.25, 0.7, sgPool));
    vec3  eC = 0.06 * block * meanC + 0.94 * eGridC;
    // Macro structure (mean ~1): a city from 5-20 km is bright commercial strips along its arterials
    // and dimmer residential between — the street scale alone averages to an even grey mesh. (Per-district
    // levels and a 512-m noise were tried first: a hard-edged patchwork and camouflage blotches.)
    vec2  artD  = abs(u - artN * floor(u / artN + 0.5)) * sp;          // metres to the nearest arterial
    float strip = exp(-0.5 * pow(min(artD.x, artD.y) / 220.0, 2.0));
    float macro = (0.25 + 2.6 * strip) / 1.55;
    return eC * mix(macro, 1.0, smoothstep(200.0, 350.0, foot));
}

float farmsteadLights(vec2 p2, ivec2 dAnc, int f, float foot);

// The night pattern: lamp light at the layout, the pixel footprint (m) and the ground's up-facing
// (nUp). Returns an RGB multiplier for the night map's lights.
vec3 cityLightPattern(CityLayout L, vec2 uv, float foot, float nUp) {
    // Lamp types: white LED dominates city cores (and arterials, +0.25 in cityStreetDir), sodium the
    // suburbs; a regional bias (some cities converted, some not) from a 4-km value noise — continuous
    // (a per-region hash changed the colour mix at every region border).
    float ledP  = clamp(mix(0.25, 0.85, L.dens) + 0.35 * cityValue2(L.p2, L.dAnc, 4096.0, 4129 + L.f), 0.05, 0.95);
    vec3  meanC = cityLampMean(ledP);
    float tU1, tU2;
    vec3  eCityC = cityNightGrid(L.g, L, foot, ledP, meanC, tU1);
    if (L.w2 > 0.0)
        eCityC = mix(eCityC, cityNightGrid(L.g2, L, foot, ledP, meanC, tU2), L.w2);
    float fh = 0.25 * foot * foot;
    // Countryside (the map's faintest lights): farmsteads and hamlets as scattered lights, not street
    // grids. A jittered light on a 512-m lattice (it must divide the 4096-m district anchor, or the
    // lights jump when the observer crosses an anchor cell), present with probability 0.45.
    float rural = 1.0 - smoothstep(0.002, 0.012, L.lum);
    if (rural > 0.0)
        eCityC = mix(eCityC, farmsteadLights(L.p2, L.dAnc, L.f, foot) * kLampSodium, rural);

    // The real major roads; district borders are no longer roads (the grids run through them or
    // cross-fade), only the projection's face seam is (its grid jumps).
    float sgA  = sqrt(25.0 + fh);
    float eArt = mix(1860.0 * cityG1(L.dSeam, sgA), 1.0, smoothstep(300.0, 700.0, sgA));
    vec3  eC   = mix(eCityC, vec3(eArt) * meanC, 0.015);
    eC += cloud.cityRoadsStrength * cityRoadLight(uv, L.rel, foot) * (1.0 - smoothstep(150.0, 350.0, foot)) * kLampLedW;
    float e    = 1.0;
    // Steep ground carries few streets.
    e *= mix(0.3, 1.0, smoothstep(0.75, 0.9, nUp));
    // Dark voids and a mild 2-km variation (the commercial strips are in cityNightGrid).
    float lit   = mix(0.12, 1.0, smoothstep(-0.65, -0.25, L.voidN));
    float macro = lit / 0.88 * exp(0.7 * cityValue2(L.p2, L.dAnc, 2048.0, 5557 + L.f)) / 1.1;
    e *= mix(macro, 1.0, smoothstep(200.0, 350.0, foot));
    // Close up a city is mostly dark between its lights; from far the lights blur into the map's own
    // brightness. The map's city cores, at the night exposure, are a lit sheet: the old detail texture
    // this replaces averaged ~0.1 of the map there (measured, harness cdbg 2026-09-29), which is what
    // read as streets in the dark. The mean ramps to 1 by the footprint the caller stops at (400 m —
    // it has to be complete below a LEO pixel, ~470 m, or orbit views changed: 15% of a Europe frame).
    e *= mix(0.12, 1.0, smoothstep(40.0, 350.0, foot));
    // The lamps' colour fades to neutral (the map's own colour) with the pattern.
    float tc = 0.75 * (1.0 - smoothstep(200.0, 350.0, foot));
    vec3  lum3 = vec3(dot(eC, vec3(0.2126, 0.7152, 0.0722)));
    return e * mix(lum3, eC, tc);
}

// ── Day: the same layout as albedo (cities phase 2) ────────────────────────────────────────────
// Box-filter overlap of the interval [-w/2, w/2] with a box of width F centred at x: the exact
// coverage of a stripe (a street) by a pixel, continuous at any footprint.
float cityBoxCover(float x, float w, float F) {
    F = max(F, 1e-4);
    return max(0.0, min(x + 0.5 * F, 0.5 * w) - max(x - 0.5 * F, -0.5 * w)) / F;
}

// Roof palette (linear albedo), in the order of the hash thresholds below.
vec3 cityRoof(float h, float warm) {
    if (h < 0.24) return vec3(0.19, 0.19, 0.185);                      // concrete / gravel
    if (h < 0.42) return vec3(0.09, 0.09, 0.10);                       // dark membrane / tar
    if (h < 0.52) return vec3(0.45, 0.45, 0.43);                       // white / cool roof
    if (h < 0.52 + 0.2 * warm) return vec3(0.26, 0.15, 0.10);          // terracotta (muted)
    if (h < 0.84) return vec3(0.13, 0.11, 0.09);                       // shingle
    return vec3(0.30, 0.31, 0.33);                                     // metal
}
vec3 cityRoofMean(float warm) {
    return 0.24 * vec3(0.19, 0.19, 0.185) + 0.18 * vec3(0.09, 0.09, 0.10) + 0.10 * vec3(0.45, 0.45, 0.43)
         + 0.2 * warm * vec3(0.26, 0.15, 0.10) + (0.32 - 0.2 * warm) * vec3(0.13, 0.11, 0.09)
         + 0.16 * vec3(0.30, 0.31, 0.33);
}
// A field block's crop (the farm palette, farmCrop, declared below).
vec3 farmCrop(float h, float green);
vec3 farmCropMean(float green);

// One grid's day albedo and its expected mean (the caller cross-fades two grids at a border and takes
// the ratio). Streets are asphalt (arterials wider); each base block is split into 2-4 lots per side
// (large commercial buildings along the arterials), each a roof of its own size and offset in a yard
// (lawn, paved or bare) under individual tree crowns; the voids are parks; at low density many blocks
// are fields. Every part is box-filtered to the footprint and replaced by its expected value once it
// cannot be resolved. (The first cut — equal lots, centred square roofs, grey yards, no trees — read as
// a board game; value-noise canopy repeated visibly.)
void cityDayGrid(CityGrid g, CityLayout L, float foot, float warm, out vec3 albO, out vec3 meanO) {
    vec2  u = g.u;
    float Fu = foot / g.sp;                                  // footprint in base units
    const vec3 kAsphalt = vec3(0.065, 0.065, 0.07);
    float wRes = 12.0 / g.sp, wArt = 24.0 / g.sp;
    // Street coverage, both directions.
    float cov[2];
    for (int d = 0; d < 2; ++d) {
        float x = u[d], n0 = floor(x + 0.5), c = 0.0;
        int   salt = d == 0 ? g.saltX : g.saltY;
        for (int j = -1; j <= 1; ++j) {
            float ni = n0 + float(j);
            int   k  = cityLineKind(ni, L.keepE, L.keepO, g.artN, salt);
            if (k == 0) continue;
            c = max(c, cityBoxCover(x - ni, k == 2 ? wArt : wRes, Fu));
        }
        cov[d] = c;
    }
    float road = 1.0 - (1.0 - cov[0]) * (1.0 - cov[1]);
    float fr1 = wArt / g.artN + (1.0 - 1.0 / g.artN) * 0.5 * (L.keepE + L.keepO) * wRes;
    float roadMean = 1.0 - (1.0 - fr1) * (1.0 - fr1);

    float dens  = L.dens;
    // Lots: per base block 2-4 per side (each axis its own); along the arterials often 1-2 (large).
    vec2  bi   = floor(u + 0.5 * vec2(wRes));                  // base block (offset past the street)
    vec3  bh   = tdRand3(ivec3(ivec2(bi), 7337 + L.f)) * 0.5 + 0.5;
    vec2  artD = abs(bi + 0.5 - g.artN * floor((bi + 0.5) / g.artN + 0.5));   // blocks to the nearest arterial
    float pBig = min(artD.x, artD.y) < 1.0 ? mix(0.35, 0.8, dens) : 0.06 * dens;
    bool  big  = bh.z < pBig;
    vec2  nLot = big ? vec2(1.0 + floor(bh.x * 1.99), 1.0 + floor(bh.y * 1.99))
                     : vec2(2.0 + floor(bh.x * 2.99), 2.0 + floor(bh.y * 2.99));
    vec2  lu   = (u - bi) * nLot;
    vec2  li   = floor(lu), lf = lu - li;
    vec3  lh   = tdRand3(ivec3(ivec2(bi * 4.0 + li), 7331 + L.f)) * 0.5 + 0.5;
    vec3  lh2  = tdRand3(ivec3(ivec2(bi * 4.0 + li), 7333 + L.f)) * 0.5 + 0.5;
    float mBase = big ? 0.06 : mix(0.30, 0.12, dens);
    vec2  mar  = mBase + vec2(0.12, 0.12) * vec2(lh.y, lh2.x) * (big ? 0.3 : 1.0);
    vec2  off  = (vec2(lh2.y, lh.z) - 0.5) * 0.2 * (1.0 - 2.0 * mar);
    vec2  Fl   = Fu * nLot;                                     // footprint in lot units
    float roofCov = cityBoxCover(lf.x - 0.5 - off.x, 1.0 - 2.0 * mar.x, Fl.x)
                  * cityBoxCover(lf.y - 0.5 - off.y, 1.0 - 2.0 * mar.y, Fl.y);
    // Large buildings are flat commercial roofs (concrete, membrane, white, metal): never terracotta.
    vec3  roofC = big ? cityRoof(lh.x < 0.5 ? lh.x * 1.04 : 0.84 + 0.16 * lh.x, 0.0) : cityRoof(lh.x, warm);
    vec3  lawn = vec3(0.065, 0.095, 0.042), paved = vec3(0.15, 0.15, 0.14), bare = vec3(0.19, 0.15, 0.11);
    float pPaved = mix(0.1, 0.5, dens), pBare = 0.1;
    vec3  yard = (lh2.z < pPaved) ? paved : (lh2.z < pPaved + pBare ? bare : lawn);
    vec3  yardMean = pPaved * paved + pBare * bare + (1.0 - pPaved - pBare) * lawn;
    // Trees: individual crowns (a jittered tree per 8-m cell, radius 2.5-6.5 m, each its own shade,
    // darker toward its edge), clustered by a 64-m field. Expected cover past a ~4-m footprint.
    vec3  treeC = vec3(0.035, 0.06, 0.025);
    float pTree = mix(0.55, 0.22, dens);
    float tFrac = clamp(pTree * 0.95, 0.0, 1.0);
    float tTree = smoothstep(3.0, 8.0, foot);
    float canopy = tFrac;
    vec3  crownC = treeC;
    if (tTree < 1.0) {
        float clus = smoothstep(-0.45, 0.35, cityValue2(L.p2, L.dAnc, 64.0, 9155 + L.f)) * 1.6;
        vec2  tc = L.p2 / 8.0;
        vec2  ti = floor(tc);
        float cv = 0.0, shade = 0.0;
        for (int k = 0; k < 9; ++k) {
            ivec2 o = ivec2(k % 3 - 1, k / 3 - 1);
            vec3  r = tdRand3(ivec3(L.dAnc * 512 + ivec2(ti) + o, 9151 + L.f));
            vec3  r2 = tdRand3(ivec3(L.dAnc * 512 + ivec2(ti) + o, 9153 + L.f)) * 0.5 + 0.5;
            if (r2.x > pTree * clus) continue;
            vec2  pp  = (ti + vec2(o) + 0.5 + 0.45 * r.xy) * 8.0;
            float rad = 2.5 + 4.0 * r2.y;
            float d   = length(L.p2 - pp);
            float c   = clamp((rad - d) / max(foot, 0.3) + 0.5, 0.0, 1.0);
            if (c > cv) { cv = c; shade = (0.7 + 0.6 * r2.z) * (0.72 + 0.28 * sqrt(max(0.0, 1.0 - (d / rad) * (d / rad)))); }
        }
        canopy = mix(cv, tFrac, tTree);
        crownC = mix(treeC * shade, treeC, tTree);
    }
    float roofShareS = (1.0 - 2.0 * (mix(0.30, 0.12, dens) + 0.06));
    float roofShareB = (1.0 - 2.0 * (0.06 + 0.018));
    float roofShare  = mix(roofShareS * roofShareS, roofShareB * roofShareB, 0.25 * dens);
    vec3  roofMean   = cityRoofMean(warm);
    vec3  groundMean = mix(yardMean, treeC, tFrac);
    vec3  lotMean    = mix(groundMean, roofMean, roofShare);
    float tLot = smoothstep(0.25, 0.6, max(Fl.x, Fl.y));
    vec3  ground = mix(yard, crownC, canopy);
    vec3  lot  = mix(mix(ground, roofC, roofCov), lotMean, tLot);
    // Field blocks (low density): a crop, its own shade per block.
    float pField = cityFieldP(dens);
    vec3  fieldM = farmCropMean(0.5);
    if (pField > 0.0) {
        float fh = tdRand3(ivec3(ivec2(bi), 919 + L.f)).x * 0.5 + 0.5;
        if (fh < pField)
            lot = mix(farmCrop(tdRand3(ivec3(ivec2(bi), 923 + L.f)).y * 0.5 + 0.5, 0.5), lotMean, 0.0);
        lot = mix(lot, mix(lotMean, fieldM, pField), smoothstep(0.35, 0.7, Fu));
    }
    lotMean = mix(lotMean, fieldM, pField);
    // Parks where the voids are: lawn and trees.
    float park  = 1.0 - smoothstep(-0.65, -0.25, L.voidN);
    vec3  parkC = mix(lawn, crownC, mix(canopy, tFrac, tLot));
    vec3  parkM = mix(lawn, treeC, tFrac);
    lot = mix(lot, parkC, park);
    vec3  lotAvg = mix(lotMean, parkM, park);
    // Street trees overhang the residential streets a little (not arterials).
    vec3  alb  = mix(lot, kAsphalt, road * (1.0 - 0.35 * canopy * (1.0 - dens)));
    vec3  mean = mix(lotAvg, kAsphalt, roadMean * (1.0 - 0.35 * tFrac * (1.0 - dens)));
    // Past the street spacing everything is its mean: ratio exactly 1.
    float coarse = mix(2.0, 1.0, L.keepO / max(L.keepE, 1e-3));
    float tU = smoothstep(0.35 * coarse, 0.7 * coarse, Fu);
    albO  = mix(alb, mean, tU);
    meanO = mean;
}

// The day albedo at the layout as a RATIO to the pattern's own mean (so it multiplies the day map and
// converges to exactly it as the footprint grows: a seamless hand-off, orbit unchanged).
vec3 cityDayAlbedo(CityLayout L, float foot) {
    // Terracotta regions, from a 4-km value noise (continuous across region borders).
    float warm = smoothstep(-0.2, 0.4, cityValue2(L.p2, L.dAnc, 4096.0, 8123 + L.f));
    vec3  a1, m1;
    cityDayGrid(L.g, L, foot, warm, a1, m1);
    // Where two different grids meet, a road runs along the border (surfaces cannot cross-fade: an
    // 80-m blend of two street grids read as a ghosted double exposure; the night's lights can).
    if (L.differ) {
        float road = cityBoxCover(L.db, 14.0, foot) * (1.0 - smoothstep(60.0, 150.0, foot));
        a1 = mix(a1, vec3(0.065, 0.065, 0.07), road);
    }
    return a1 / max(m1, vec3(1e-3));
}

// Farmsteads: a jittered light on a 512-m lattice (it divides the 4096-m anchor: world-fixed), present
// with probability 0.45; mean 1 over its area, filtered to the footprint. Used by the city pattern's
// faintest (rural) end and by the farmland light floor.
float farmsteadLights(vec2 p2, ivec2 dAnc, int f, float foot) {
    vec2  fc = p2 / 512.0;
    vec2  fi = floor(fc);
    float sgF = sqrt(64.0 + 0.25 * foot * foot) / 512.0;
    if (sgF >= 0.35) return 1.0;
    float eF = 0.0;
    for (int k = 0; k < 9; ++k) {
        ivec2 o  = ivec2(k % 3 - 1, k / 3 - 1);
        vec3  r  = tdRand3(ivec3(dAnc * 8 + ivec2(fi) + o, 6601 + f));
        if (r.z * 0.5 + 0.5 > 0.45) continue;
        vec2  pp = fi + vec2(o) + 0.5 + 0.4 * r.xy;
        vec2  d  = fc - pp;
        eF += exp(-0.5 * dot(d, d) / (sgF * sgF)) * 0.15915494 / (sgF * sgF);
    }
    return mix(eF / 0.45, 1.0, smoothstep(0.2, 0.35, sgF));
}

// Beaches (terrain v2 P3 follow-up): sand along LOW, GENTLE shores, 40-140 m wide, varying along the
// coast. The shoreline is the height function's own (the water map filtered by hand near the observer
// as tdDemAt does, plus the coves of tdShoreOffset), so the sand meets the water exactly. h = the ground
// height (DEM part), nUp = its up-facing. out dLand = metres inland from the waterline.
float beachAt(vec3 q, vec3 enuX, vec3 enuY, vec3 enuZ, vec2 uv, float h, float nUp, out float dLand) {
    dLand = 1e9;
    if (!tdEnabled()) return 0.0;
    vec2 wm;
    if (dot(q.xy, q.xy) < kTdExactDemM * kTdExactDemM) {
        ivec2 wsz = textureSize(earthSpecTex, 0);
        vec2  tw  = uv * vec2(wsz) - 0.5;
        vec2  fl  = floor(tw), fr = tw - fl;
        int   x0  = ((int(fl.x) % wsz.x) + wsz.x) % wsz.x, x1 = (x0 + 1) % wsz.x;
        int   y0  = clamp(int(fl.y), 0, wsz.y - 1), y1 = clamp(int(fl.y) + 1, 0, wsz.y - 1);
        wm = mix(mix(texelFetch(earthSpecTex, ivec2(x0, y0), 0).rg, texelFetch(earthSpecTex, ivec2(x1, y0), 0).rg, fr.x),
                 mix(texelFetch(earthSpecTex, ivec2(x0, y1), 0).rg, texelFetch(earthSpecTex, ivec2(x1, y1), 0).rg, fr.x), fr.y);
    } else {
        wm = textureLod(earthSpecTex, uv, 0.0).rg;
    }
    if (wm.g < 14.5 / 255.0) return 0.0;
    float d = (wm.r - 0.5) * 2.0 * kShoreSdfMaxM;
    if (d < -1500.0 || d > 1500.0) return 0.0;
    d += tdShoreOffset(q, enuX, enuY, enuZ, wm);
    if (d > 0.0) return 0.0;
    dLand = -d;
    float level = max(0.0, wm.g * kElevRange - kElevOffset);
    // Width varies along the coast (a 600-m value noise on the anchored lattice).
    vec3  offS = tdSphereOffset(q);
    vec3  rel  = cloud.terrainAnchorRel.xyz + offS.x * enuX + offS.y * enuY + offS.z * enuZ;
    float wn   = tdNoised(rel / 512.0, ivec3(cloud.terrainAnchorCell.xyz) * 4 + ivec3(911, 377, 53)).x;
    float width = 40.0 + 100.0 * (wn * 0.5 + 0.5);
    float b = 1.0 - smoothstep(0.6 * width, width, dLand);
    b *= 1.0 - smoothstep(3.0, 9.0, h - level);       // low: a beach, not a cliff top
    b *= smoothstep(0.93, 0.985, nUp);                  // gentle
    return b;
}

// ── Farmland (cities phase 2 follow-up, the user: rural areas should be more farmy) ──────────────
// Fields as a RATIO to their own expected mean x the day map, like the city albedo (seamless, orbit
// unchanged). Two regional styles: a surveyed GRID (the Americas: 1024-m sections split into 1-4 fields,
// gravel roads on the section lines, centre-pivot circles in dry regions) and a PATCHWORK (elsewhere:
// warped Voronoi fields of ~256 m with hedgerows where it is green). The crop palette leans green or
// dry with the day map.
vec3 farmCrop(float h, float green) {
    // green crop, dark green, fallow grass, yellow grain, stubble, bare soil
    float g = 0.25 + 0.35 * green;
    if (h < g * 0.6)  return vec3(0.060, 0.100, 0.035);
    if (h < g)        return vec3(0.040, 0.070, 0.030);
    if (h < g + 0.12) return vec3(0.090, 0.100, 0.050);
    if (h < g + 0.12 + 0.35 * (1.0 - g)) return vec3(0.220, 0.190, 0.090);
    if (h < 0.90)     return vec3(0.260, 0.230, 0.150);
    return vec3(0.130, 0.100, 0.070);
}
vec3 farmCropMean(float green) {
    float g = 0.25 + 0.35 * green;
    float y = 0.35 * (1.0 - g);
    return g * 0.6 * vec3(0.060, 0.100, 0.035) + g * 0.4 * vec3(0.040, 0.070, 0.030) + 0.12 * vec3(0.090, 0.100, 0.050)
         + y * vec3(0.220, 0.190, 0.090) + max(0.90 - g - 0.12 - y, 0.0) * vec3(0.260, 0.230, 0.150)
         + 0.10 * vec3(0.130, 0.100, 0.070);
}

vec3 farmDayAlbedo(vec3 q, vec3 enuX, vec3 enuY, vec3 enuZ, float lonDeg, float green, float dry, float foot) {
    vec2  p2;
    ivec2 dAnc;
    int   f;
    vec3  rel, aup;
    cityFrame(q, enuX, enuY, enuZ, p2, dAnc, f, rel, aup);
    ivec2 big  = dAnc >> 5;                                           // ~130-km areas share a style
    vec3  rA   = tdRand3(ivec3(big, 7101 + f));
    bool  gridStyle = (lonDeg < -30.0) ? (rA.z > -0.6) : (rA.z > 0.7);
    const vec3 kGravel = vec3(0.20, 0.18, 0.15), kHedge = vec3(0.03, 0.055, 0.025);
    vec3  meanC = farmCropMean(green);
    vec3  alb;
    vec3  mean;
    if (gridStyle) {
        float ang = rA.x * 0.35;                                      // nearly the same over the area
        vec2  cs  = vec2(cos(ang), sin(ang));
        vec2  dp  = p2 - vec2(big * 32 - dAnc) * 4096.0;
        vec2  u   = vec2(dot(dp, cs), dot(dp, vec2(-cs.y, cs.x))) / 1024.0;   // sections
        vec2  si  = floor(u), sf = u - si;
        vec3  sh  = tdRand3(ivec3(ivec2(si) + big * 256, 7103 + f)) * 0.5 + 0.5;
        vec2  nF  = vec2(sh.x < 0.35 ? 1.0 : 2.0, sh.y < 0.35 ? 1.0 : 2.0);
        vec2  fi  = floor(sf * nF), ff = sf * nF - fi;
        vec3  fh  = tdRand3(ivec3(ivec2(si * 2.0 + fi) + big * 512, 7105 + f)) * 0.5 + 0.5;
        vec3  crop = farmCrop(fh.x, green);
        float Fu  = foot / 1024.0;
        // Centre pivots in dry regions: a green circle in the field, bare corners.
        if (dry > 0.0 && fh.y < 0.7 * dry * dry) {
            vec2  fsz = 1.0 / nF;
            float r   = 0.47 * min(fsz.x, fsz.y);
            float d   = length((ff - 0.5) * fsz);
            float inC = clamp((r - d) / max(Fu, 1e-4) + 0.5, 0.0, 1.0);
            crop = mix(vec3(0.15, 0.12, 0.085), vec3(0.055, 0.10, 0.035), inC);
        }
        // Gravel roads on the section lines (10 m).
        float w    = 10.0 / 1024.0;
        float road = max(cityBoxCover(sf.x - 0.5 * w, w, Fu) + cityBoxCover(sf.x - 1.0 + 0.5 * w, w, Fu),
                         cityBoxCover(sf.y - 0.5 * w, w, Fu) + cityBoxCover(sf.y - 1.0 + 0.5 * w, w, Fu));
        float tF   = smoothstep(0.15, 0.4, Fu * max(nF.x, nF.y));    // fields unresolved -> mean
        alb  = mix(mix(crop, meanC, tF), kGravel, road * (1.0 - tF));
        mean = mix(meanC, kGravel, 2.0 * w * (1.0 - tF));
    } else {
        // Patchwork: Voronoi fields on a 256-m lattice, stretched ~2.2:1 along an area's own direction
        // (European fields are strips, not the pentagons an isotropic Voronoi draws), warped, hedgerows
        // (3 m) where it is green. The stretch is a linear map of the anchored coordinate: still exact.
        float ang = rA.x * 3.1416;
        vec2  cs  = vec2(cos(ang), sin(ang));
        // Measured from the ~130-km area's origin (fixed as the observer moves: a rotated lattice anchored
        // at the district origin would jump whenever the district anchor changes).
        vec2  dpA = p2 - vec2(big * 32 - dAnc) * 4096.0;
        vec2  pr  = vec2(dot(dpA, cs), dot(dpA, vec2(-cs.y, cs.x)));
        vec2  c  = pr / vec2(420.0, 190.0);
        c += 0.25 * vec2(sin(c.y * 0.9 + rA.x * 5.0), sin(c.x * 0.8 + rA.y * 5.0));
        vec2  ci = floor(c);
        float d1 = 1e9, d2 = 1e9;
        vec2  c1 = vec2(0.0), c2 = vec2(0.0);
        vec3  h1 = vec3(0.0);
        for (int k = 0; k < 9; ++k) {
            ivec2 o = ivec2(k % 3 - 1, k / 3 - 1);
            vec3  r = tdRand3(ivec3(ivec2(ci) + o + big * 4096, 7107 + f));
            vec2  cc = ci + vec2(o) + 0.5 + 0.42 * r.xy;
            float d  = dot(c - cc, c - cc);
            if (d < d1) { d2 = d1; c2 = c1; d1 = d; c1 = cc; h1 = r; }
            else if (d < d2) { d2 = d; c2 = cc; }
        }
        // Metres to the boundary: the bisector is a line in both spaces; its metric normal is the stretched
        // direction's, mapped.
        vec2  nS   = normalize(c2 - c1);
        vec2  tM   = normalize(vec2(-nS.y, nS.x) * vec2(420.0, 190.0));
        float edge = abs(dot((c - 0.5 * (c1 + c2)) * vec2(420.0, 190.0), vec2(-tM.y, tM.x)));
        vec3  crop = farmCrop(h1.z * 0.5 + 0.5, green);
        float Fu   = foot / 190.0;
        float tF   = smoothstep(0.15, 0.4, Fu);
        float hw   = mix(1.0, 3.0, green);                             // hedge / track width (m)
        vec3  bnd  = mix(kGravel, kHedge, green);
        float hcov = clamp((0.5 * hw - edge) / max(foot, 0.3) + 0.5, 0.0, 1.0) * (1.0 - tF);
        alb  = mix(mix(crop, meanC, tF), bnd, hcov);
        mean = mix(meanC, bnd, (hw / 280.0) * 2.2 * (1.0 - tF));
    }
    return alb / max(mean, vec3(1e-3));
}

// ── Milky Way surface-brightness anchors ─────────────────────────────────────────────────────
// File scope, not block-local, because TWO places gate this panorama on them: the direct sky view
// and the ocean sky-reflection. Two copies would be free to drift and the symptom — a reflection
// fading at a different sky brightness than the sky it reflects — would be subtle and confusing.
//
// Measured, not guessed. The panorama is VK_FORMAT_R8G8B8A8_SRGB, so what the sampler delivers is
// linear; its luminance histogram runs ~0.0006 for empty sky between stars, ~0.003 faint band,
// ~0.015 mid-band, 0.05-0.115 for the galactic core. Anchoring 0.05 -> 21.0 mag/arcsec^2 puts the
// core at the real Milky Way core's surface brightness.
//
// kMwMagSpread is well under the physical 2.5 mag/decade because this is a stretched photograph:
// its faint regions sit relatively brighter than the real sky. It is also half of the gate's
// contrast-stretch control (kVisSharpness is the other half) — at the original 1.6 the band's own
// 1.2-decade range covered essentially the whole response window, so the gate doubled as a hard
// contrast stretch and the Milky Way got SHARPER as it faded. See darksky.glsl before retuning.
const float kMwRefLum    = 0.05;
const float kMwRefMag    = 21.0;
const float kMwMagSpread = 0.85;

// ── Cloud noise domain frequencies ─────────────────────────────────────────────
// Cloud procedural noise (cloudNoiseTex) is sampled by TRUE 3D unit-sphere position
// (dirECEF = normalize(pECEF)), not by lat/lon UV. A sphere embedded in R^3 carries its
// natural induced metric, so this has no pole singularity and no latitude-dependent scale
// distortion — unlike an equirectangular UV, whose atan2/asin derivatives blow up at the
// poles (causing both the visible polar noise compression and a real perf hit, since the
// raymarch's empty-air skip gets defeated by aliased density near the poles).
// dirECEF is also altitude-invariant by construction (same value straight up/down at a
// given lat/lon), so cloud "presence" shape naturally has no unwanted Z-sweep with no hack
// needed. Frequencies are ~(old UV-space tile count)/(2*PI), since dirECEF isn't normalized
// to a 0-1 globe fraction the way pUV was — retune visually, these are starting points.
// (kCloudHorizFreq / kCloudColFreq are defined in common.glsl — same values, same rationale.)

// ── Domain warp / city upwelling constants: MOVED, not deleted ────────────────────────────
// kWarpFreq/kWarpStrength/kWarpDriftRate/kWarpEvolveRate and kCityUpwellStrength used to live
// here alongside this file's own cloudWarpOffset()/cloudDensity()/cloudMarch(). Those marches
// moved to cloud_march.comp (session 23); the constants and functions were left behind and had
// zero call sites in this file ever since. Removed in the pipeline-unification pass. The live
// definitions — including the full rationale comments this block used to carry — are in
// cloud_march.comp; anything shared migrates to shaders/include/ from there, not from here.

// City-brightness response curve, shared by the cloud upwelling (below) and the atmospheric
// city-glow term (see kNightGlowScale) so both read the same "how bright is this city" signal
// consistently. earthNightTex luminance varies enormously between a small town and a major
// metro core — a LINEAR response (the old cityMask = max(0,cityLum-kNightFloor)) means bright
// cities dominate completely while small towns barely clear the floor and contribute nothing.
// Reinhard-style compression (raw/(raw+k)) has a steep slope near 0 (small towns get a real,
// visible response) and naturally saturates toward 1.0 for large raw values (major metros can't
// run away and blow out) — compresses the huge input dynamic range into a much narrower, more
// even output range. Smaller k = more aggressive compression (steeper low-end boost, earlier
// high-end saturation).
const float kNightFloor    = 0.002;
const float kCityCompressK = 0.08;
float cityBrightness(float lum) {
    float raw = max(0.0, lum - kNightFloor);
    return raw / (raw + kCityCompressK);
}
// Atmospheric city-glow strength (Step 7 / C10 in TERRAIN_PLAN.md — previously deferred,
// implemented here alongside the cloud upwelling fix so both read the same brightness curve
// and sell as one consistent light source instead of bright clouds over a flat-black sky).
// First-pass value, deliberately conservative — kCityUpwellStrength's first guess (50) blew
// out badly, so start low here and raise if the glow reads as too subtle.
const float kNightGlowScale = 0.0000002;

// ── Airglow (C15, TERRAIN_PLAN.md Phase E) ──────────────────────────────────────
// Three altitude-banded emissive nightglow layers, riding the N_VIEW atmosphere loop
// where their peak altitude falls inside it (green/sodium), or a small supplemental
// march where it doesn't (red — peaks at 275km, well past N_VIEW's ~100km ceiling;
// see the airglowRed march after the N_VIEW loop in main()). Density per layer is a
// Gaussian in altitude: exp(-((h-peakAltM)/halfWidthM)^2). Real airglow altitudes are
// near-constant physical constants (not scene-dependent), so they're hardcoded here
// rather than exposed as CloudParams sliders — only per-band brightness (which is a
// legitimate first-pass visual guess, unlike the altitudes) is user-tunable.
const float kAirglowGreenPeakM      = 96000.0;   // O I 557.7nm — dominant visible band
const float kAirglowGreenHalfWidthM = 9000.0;
const vec3  kAirglowGreenColor      = vec3(0.35, 1.0, 0.25);
const float kAirglowSodiumPeakM      = 90000.0;  // Na D 589.3nm — sharp/thin
const float kAirglowSodiumHalfWidthM = 6500.0;
const vec3  kAirglowSodiumColor      = vec3(1.0, 0.65, 0.15);
// kAirglowRedPeakM/HalfWidthM moved to cloud_march.comp with the red band's march itself —
// kAirglowRedColor stays here too, still used by auroraSampleAt's color blend below.
const vec3  kAirglowRedColor      = vec3(1.0, 0.12, 0.05);
// Horizontal patchiness so the bands don't read as a perfectly flat, featureless ring
// around the sky (a pure function of altitude alone has zero horizontal variation).
// Reuses the analytic warpPerlin3 noise already used for cloud domain warp — no new
// texture/binding, matches the "reuse existing noise infra" C15 design directive
// (which pre-dates the cloud warp's migration from a noiseTex lookup to this analytic
// evaluator — see cloudWarpOffset's comment; follow the current code, not the stale plan).
const float kAirglowNoiseFreq = 4.0;
const float kAirglowDriftRate = 0.015; // wall-clock rad/s (pc.waveTime), slow independent drift
// First-pass brightness scale, same convention as kNightGlowScale/kCityUpwellStrength
// above: raw accumulation (density × segLen, summed over qualifying march samples) is
// a large unnormalized number, this brings it into visible range. Deliberately
// conservative — real airglow is famously faint. Tune via cloud.airglowGain (settings
// slider) rather than editing this constant.
const float kAirglowScale = 0.0000005;

// ── 3D volumetric ↔ flat 2D crossfade band ─────────────────────────────────────
// cloudMarch (expensive per-sample 3D shell march) and evalCloudLayer (cheap flat-texture
// paste at layers[0]/[1]'s shellAltM, i.e. the same physical cloud base/top) render the SAME
// shell at two different fidelities. Below kCloud3DFadeStart: pure 3D. Above kCloud3DFadeEnd:
// pure flat 2D (cheap enough for orbit). Both sides read this same pair of constants so the
// crossfade is symmetric — previously the flat paste used a hard `obsEffH < 8000` boolean while
// the volumetric fade didn't reach zero until 180 km, so 8-180 km altitude showed both the flat
// shell AND the still-near-full-strength 3D volume composited at once (visible as the flat
// texture "shell" intersecting the volumetric clouds).
const float kCloud3DFadeStart = 800000.0;
const float kCloud3DFadeEnd   = 3000000.0;

// ── View-march step count vs. altitude ──────────────────────────────────────────
// Step count needed for a glitchless march scales with the shell's ANGULAR size on screen,
// which shrinks with observer altitude — LEO (400-600 km) can look correct with far fewer
// steps than ground level needs. This band is intentionally separate from kCloud3DFadeStart
// (800 km): that constant now sits above typical LEO, so opacity fading alone doesn't reduce
// cost anywhere satellites actually orbit — the reported "LEO cloud perf is awful" case sits
// entirely inside the always-full-3D zone below kCloud3DFadeStart. This band supplies the
// actual LEO-perf lever: steps ramp from full ground quality down to a floor well before 800 km.
const float kMarchStepsAltStart = 2000.0;    // below this: full cloud.marchSteps (ground/aircraft)
const float kMarchStepsAltEnd   = 200000.0;   // at/above this: kMarchStepsFloor (LEO and up)
const float kMarchStepsFloor    = 12.0;       // minimum steps once the shell is angularly tiny

// ── Rayleigh scattering (wavelength-dependent: R=650nm, G=510nm, B=440nm) ─────
// (BETA_R / H_R are defined in common.glsl.)

// ── Mie scattering (aerosols, wavelength-independent) ─────────────────────────
// (BETA_M / H_M / G_MIE are defined in common.glsl.)

// ── Lighting / tone mapping ────────────────────────────────────────────────────
// (SUN_INTENSITY is defined in common.glsl.)
const float EXPOSURE_DAY   =  1.8;   // sun at zenith -- prevents white washout
const float EXPOSURE_NIGHT = 10.0;   // below horizon -- amplifies dim twilight glow

// ── Ray march quality ──────────────────────────────────────────────────────────
// Was fixed const (124/12) — perf follow-up (session 24): the main atmosphere loop runs
// unconditionally on every pixel (terrain, ocean, cloud, satellite, or empty space) before any
// surface-specific work, so this is the single most-paid-for cost in the whole shader. Now
// UBO-tunable ("View samples"/"Light samples" sliders) so the user can empirically test how much
// of the ground-level frame budget this actually costs before investing in a transmittance LUT.
// Defaults (124/12) preserve prior behavior exactly.

// (phaseR / phaseM are defined in common.glsl.)
// (phaseCloud lived here — dead since the cloud march moved to cloud_march.comp, removed in the
// pipeline-unification pass. The live copy is cloud_march.comp's.)
// (raySphere, with its full planetary-scale precision rationale, is in common.glsl.)
// Marches N_LIGHT steps from point p toward direction d over distance segTotal
// and returns (Rayleigh optical depth, Mie optical depth) — i.e. ∫ρ(h) ds for each species.
// Multiply by BETA_R / BETA_M in the caller to convert to actual extinction coefficients.
// Called once per view sample to accumulate the sun-side transmittance at that altitude.
// Kept local, NOT shared with cloud_march.comp's copy of the same integral. See that file's
// optDepth comment for why: sharing forces a runtime trip count, and cloud_march's is a
// compile-time constant it needs to keep. This copy's count is settings-tunable, so it has a
// runtime bound either way and loses nothing by staying here.
vec2 optDepth(vec3 p, vec3 d, float segTotal) {
    // Perf knockout: zero optical depth = "sun ray unattenuated", the same fallback the
    // callers already use when tSun.y <= 0 (no atmosphere intersection) — a safe, already-
    // exercised code path, not a new one. Isolates every optDepth() call site (main N_VIEW
    // loop, ocean sky-reflection loop, and both fixed-count supplemental marches) at once.
    if (dbgSkipSunOD())
        return vec2(0.0);
    int   N_LIGHT = int(max(2.0, cloud.lightSamples));
    float sLen = segTotal / float(N_LIGHT);  // length of each sun-ray sub-step
    float odR = 0.0, odM = 0.0;
    for (int i = 0; i < N_LIGHT; ++i) {
        float h = max(0.0, length(p + d * (float(i) + 0.5) * sLen) - R_EARTH);  // altitude at sub-step midpoint
        odR += exp(-h / H_R);  // Rayleigh density (exponential profile, scale height H_R)
        odM += exp(-h / H_M);  // Mie density (exponential profile, scale height H_M)
    }
    return vec2(odR, odM) * sLen;  // summed densities x step length -> optical depth units
}

// (rotateZ lived here — dead since the cloud march moved to cloud_march.comp, removed in the
// pipeline-unification pass. The live copy is cloud_march.comp's.)

// (remap is defined in common.glsl — still used here by the aurora shell/fold code.)

// ── Analytic 3D gradient noise for the cloud domain warp ───────────────────────
// The warp used to read cloudNoiseTex (a 192³ DISCRETELY STORED texture) at kWarpFreq=0.1,
// which spans only ~38 texels across the whole visible range. Trilinear filtering between
// stored texel values is piecewise-multilinear, not truly smooth — each grid cell interpolates
// as a flat-ish shard, not a curved surface. That's invisible at the texture's intended dense
// sampling rate (kCloudHorizFreq=480+), but reading it this sparsely exposed the underlying
// voxel grid directly as faceted, straight-edged geometry — the reported "tessellating"
// artifacts, baked into the cloud edge wherever the warp perturbed the presence threshold.
// Fix: evaluate gradient noise ANALYTICALLY at the exact continuous query point instead of
// interpolating a coarse discrete grid — same hash/gradient technique cloud_noise.comp uses to
// bake the volume, just run live here instead of pre-baked to a fixed low resolution. No
// texture, no discretization, no grid to facet against, and (bonus) no REPEAT-wrap seam class
// of bug possible at all, since there's no stored tile to wrap.
uvec3 warpHashU(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
vec3 warpGradHash(ivec3 c) {
    return normalize(-1.0 + 2.0 * (vec3(warpHashU(uvec3(c))) * (1.0 / 4294967296.0)));
}
float warpPerlin3(vec3 p) {
    ivec3 i = ivec3(floor(p));
    vec3  f = fract(p);
    vec3  u = f * f * (3.0 - 2.0 * f);   // smoothstep — gives C1-continuous interpolation
    float v000 = dot(warpGradHash(i),                   f              );
    float v100 = dot(warpGradHash(i + ivec3(1,0,0)), f - vec3(1,0,0));
    float v010 = dot(warpGradHash(i + ivec3(0,1,0)), f - vec3(0,1,0));
    float v110 = dot(warpGradHash(i + ivec3(1,1,0)), f - vec3(1,1,0));
    float v001 = dot(warpGradHash(i + ivec3(0,0,1)), f - vec3(0,0,1));
    float v101 = dot(warpGradHash(i + ivec3(1,0,1)), f - vec3(1,0,1));
    float v011 = dot(warpGradHash(i + ivec3(0,1,1)), f - vec3(0,1,1));
    float v111 = dot(warpGradHash(i + ivec3(1,1,1)), f - vec3(1,1,1));
    return mix(mix(mix(v000, v100, u.x), mix(v010, v110, u.x), u.y),
               mix(mix(v001, v101, u.x), mix(v011, v111, u.x), u.y), u.z);
}

// Airglow coverage patchiness (cloud.airglowCoverageGain) for the green/sodium bands — a
// lower-frequency companion to kAirglowNoiseFreq's mild +-40% shimmer above, remapped through a
// threshold (same idiom as cloud_march.comp's own copy / auroraCoverage) so there are real dim
// gaps between brighter patches instead of a gentle wobble. gain=0 reproduces the old uniform-
// minus-shimmer look exactly; gain=1 is full patchiness. Duplicated in cloud_march.comp (which
// needs its own copy for the red band's supplemental march) rather than shared — matches this
// file's standing convention for small per-shader noise helpers, see the aurora functions above.
float airglowCoverageMask(vec3 dirECEF, float t, vec3 seedOffset) {
    float n = warpPerlin3(dirECEF * (kAirglowNoiseFreq * 0.5)
                          + vec3(t * kAirglowDriftRate * 0.6, 0.0, 0.0) + seedOffset);
    return smoothstep(-0.2, 0.5, n);
}

// (cloudWarpOffset lived here — dead since the cloud march moved to cloud_march.comp, removed in
// the pipeline-unification pass. Worth knowing if you go looking for it: this copy still used
// THREE LIVE warpPerlin3 evaluations, while cloud_march.comp's live version reads the baked
// cloudWarpNoiseTex instead (session 31). They were genuinely different algorithms producing
// different values — the drift was invisible only because this copy was already unreachable.)

// cirrusWindAngleAt/cirrusDomainWarp moved to shaders/cloud_march.comp (C15-perf, half-res cloud
// pass) — they were exclusive to cirrusMarch, which moved there too.

// ── Aurora (C16, TERRAIN_PLAN.md Phase E) ──────────────────────────────────────
// Emissive-only "curtain primitive" centered on the geomagnetic pole (NOT the geographic pole —
// see TERRAIN_PLAN.md C16 for why this matters). Geomagnetic poles are antipodal under a dipole
// model, so one ECEF constant covers both hemispheres (negate for south). Derived with the same
// cos(lat)cos(lon)/cos(lat)sin(lon)/sin(lat) formula the fixed observer ECEF constant in
// CLAUDE.md uses. North geomagnetic pole ≈ 80.7°N, 72.7°W (current epoch; drift ~0.05-0.1°/yr is
// negligible at sim epoch 2036).
const vec3  kGeomagPoleECEF      = vec3(0.0481, -0.1543, 0.9868);
const float kAuroraOvalColatDeg  = 20.0;     // oval centerline, degrees from the geomagnetic pole
const float kAuroraOvalWidthDeg  = 6.0;      // base half-width before storm expansion
const float kAuroraShellInnerM   = 95000.0;  // curtain base altitude (m)
const float kAuroraShellOuterM   = 300000.0; // red-fringe top altitude (m)
const vec3  kAuroraBaseColor     = vec3(0.15, 1.0, 0.35);  // green O I 557.7nm — matches airglow green family
const vec3  kAuroraTopColor      = vec3(0.65, 0.15, 0.45); // red/magenta upper fringe
const float kAuroraRingWarpFreq  = 3.0;   // oval-edge ripple spatial frequency (around the ring)
const float kAuroraOvalWarpDeg   = 4.0;   // oval-edge ripple amplitude, degrees of colatitude
const float kAuroraOvalDriftRate = 0.003; // wall-clock rad/s ripple drift (pc.waveTime) — 10x slower
                                           // than first pass per user feedback (evolution read as
                                           // too fast/frantic for something the size of a continent)
// These three frequencies are NOT comparable as raw numbers — colat/az are RADIANS multiplied by
// Earth's radius (~6.57e6 m) to get physical arc length, while altitude is already METERS. A colat
// frequency of 6 looks "low" next to altitude's 0.00001, but 1/6 radian × 6.57e6 m ≈ 1100 km — a
// physically enormous cell, ~10x longer than altitude's own ~100km cell at the time this was first
// (wrongly) tuned. That's why swapping which noise-space SLOT held colat vs altitude didn't fix
// the "streaks point at the pole" bug: colat was still the physically longest axis by a wide
// margin. Values below are chosen so all three land in the same physical ballpark (~50-170km per
// noise cell) with altitude deliberately the largest (long vertical streaks), tangent/radial both
// short (many separate folds, no unwanted radial elongation).
const float kAuroraTangentFreq   = 40.0;     // ~2π/40 rad cell × R·sin(colat)(~2.2e6 m) ≈ 55 km
const float kAuroraRadialFreq    = 70.0;     // ~1/70 rad cell × R (~6.57e6 m) ≈ 94 km
const float kAuroraAltFreq       = 0.000006; // ~1/0.000006 m cell ≈ 167 km — the long/coherent axis
// Evolution speed is user-tunable — see cloud.auroraShimmerRate (settings window "Fold shimmer
// rate"), mixed into the tangent/azimuthal axis in auroraCurtainNoise below, not here.
// Raw accumulation → visible range, same convention as kAirglowScale. Same order of magnitude as
// kAirglowScale (5e-7) despite aurora being a much brighter phenomenon than nightglow — the segLen
// (meters) × sample-count accumulation this multiplies is the same order for both, so the
// brightness difference belongs on cloud.auroraGain (a much higher default than airglowGain), not
// here. First pass used 0.02 (1e4× too large) and blew out to solid white even at auroraGain=0.01 —
// EXPOSURE_NIGHT (10x) compounds the error further downstream, and once any channel's post-exposure
// value is large the Reinhard-style tonemap saturates every channel to 1.0 together, which reads as
// white instead of an overbright green/red.
const float kAuroraScale         = 0.000001;

// Colatitude + azimuth of a direction relative to a geomagnetic pole, plus the local
// tangent (azimuthal, "around the ring") and radial (colatitude, "toward/away from pole")
// basis vectors so the curtain noise can be stretched anisotropically along each axis.
void auroraFrame(vec3 dirECEF, vec3 poleDir, out float colat, out float az,
                  out vec3 radialT, out vec3 tangentT) {
    colat = acos(clamp(dot(dirECEF, poleDir), -1.0, 1.0));
    vec3 ref = abs(poleDir.z) < 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    tangentT = normalize(cross(poleDir, ref));
    radialT  = normalize(cross(tangentT, poleDir));
    vec3 localDir = dirECEF - poleDir * dot(dirECEF, poleDir);
    az = atan(dot(localDir, tangentT), dot(localDir, radialT));
}

// Large-scale "coverage" gate — breaks the oval into discrete arcs/patches instead of a solid,
// uniformly lit ring. MUCH lower frequency than auroraCurtainNoise's fold texture (which only
// varies the brightness WITHIN an already-lit patch); this decides whether a whole multi-degree
// stretch of the ring has any aurora at all, which is what actually reads as "twisty curves that
// come and go" rather than fine internal ray structure. storm strength lowers the threshold (fills
// in gaps) — a strong substorm brightens/fills the whole oval, a quiet aurora is patchy — matching
// real auroral behavior (calm-period aurora genuinely does look like broken arcs, not a full ring).
//
// Varies PRIMARILY with COLATITUDE, only mildly with azimuth — a first version did the opposite
// (noise sampled purely as a function of az, constant across colat) and that was backwards: a
// field that's constant along colat and varies with az has its threshold CROSSINGS at fixed-az
// contours, and constant-azimuth lines are meridians — they point straight at the geomagnetic
// pole by definition. At any real frequency that reads as "cranking up frequency, lines tracing up
// to the pole" — exactly the reported bug. Swapping which coordinate dominates makes the threshold
// crossings fall on near-constant-colatitude contours instead, which run parallel to latitude
// circles — "large tracks... parallel with latitude lines", the explicit ask. The azimuthal term
// is kept small purely to keep the boundary from being a perfect circle (a gentle wave instead).
// kAuroraCoverageSoftness stays a fixed constant (edge softness isn't worth its own slider);
// frequency/az-frequency/drift-rate are user-tunable — see cloud.auroraCoverageFreq/AzFreq/
// DriftRate below (settings window "Coverage freq"/"Coverage az freq"/"Coverage drift").
const float kAuroraCoverageSoftness  = 0.45;  // smoothstep width of each patch's edge
float auroraCoverage(float colat, float az, float t, float storm) {
    // Time is mixed into the AZIMUTHAL embedding (x,y), NOT the colatitude axis (z) — an earlier
    // version added t directly to the colat coordinate, which made the whole pattern visibly
    // TRANSLATE toward/away from the pole over time (reported as "waves marching to the poles").
    // colat here is a pure, time-independent spatial axis. (cos(az)+t·rate, sin(az)+t·rate) is no
    // longer on the unit circle once translated, but that's fine — x,y are just an embedding of az
    // chosen to avoid a seam at az=±π, not a meaningful physical direction, so drifting them
    // doesn't create a directionally-biased slide the way drifting colat did; the noise value at
    // any fixed (az,colat) point instead evolves/shimmers over time.
    vec2  azWarp = vec2(cos(az), sin(az)) * cloud.auroraCoverageAzFreq + t * cloud.auroraCoverageDriftRate;
    float n = warpPerlin3(vec3(azWarp, degrees(colat) * cloud.auroraCoverageFreq));
    float threshold = mix(0.2, -0.6, clamp(storm, 0.0, 1.0));
    return smoothstep(threshold, threshold + kAuroraCoverageSoftness, n);
}

// Ripple-displaces the oval's centerline colatitude as a function of azimuth + time, so the
// band isn't a perfect circle. Sampled on (cos az, sin az) rather than az directly — avoids a
// seam at az=±π, same reason cloudWarpOffset feeds a 3D point into warpPerlin3 instead of a
// raw angle. stormStrength widens the band and pushes it equatorward (larger colatitude),
// matching real substorm behavior. Multiplied by auroraCoverage so the band itself is patchy,
// not just internally textured — this is the "erosion" that turns a solid ring into broken arcs.
float auroraOvalMask(float colat, float az, float t, float storm) {
    // Perf: cheap conservative pre-filter before the two warpPerlin3 calls below (ripple +
    // auroraCoverage's own internal one) — auroraSampleAt calls this once per march step, so
    // these 2 evaluations are paid by every sample whose colatitude is even plausibly near the
    // oval, which is the majority of samples along a long oblique ray (the reason the aurora
    // march dominated frame cost — see this session's profiling). centerDeg's worst-case range
    // is [20-6, 20+6+8] = [14,34] (ripple bounded generously at ±1.5*kAuroraOvalWarpDeg=±6°,
    // storm*8.0 up to +8° at storm's slider-enforced max of 1.0); the fade zone reaches
    // widthDeg*2, worst case 2*(6*(1+1.5))=30° at storm=1. So no parameter combination can light
    // any colatitude beyond 34+30=64°, worst case — 70° below is that bound plus margin, kept as
    // a plain arithmetic comparison (no noise, no branches inside a loop already paid for) so it
    // costs nothing on the samples that DO need the real calculation.
    if (degrees(colat) > 70.0)
        return 0.0;
    vec2  ringP  = vec2(cos(az), sin(az)) * kAuroraRingWarpFreq;
    float ripple = warpPerlin3(vec3(ringP, t * kAuroraOvalDriftRate)) * kAuroraOvalWarpDeg;
    float centerDeg = kAuroraOvalColatDeg + ripple + storm * 8.0;
    float widthDeg  = kAuroraOvalWidthDeg * (1.0 + storm * 1.5);
    // Full brightness out to half of widthDeg, then a WIDE gradual fade out to 2x widthDeg —
    // previously the whole 0..widthDeg span was the falloff, which hit exactly zero right at the
    // oval's nominal edge. Airglow (green/sodium, similar altitude) has no such cutoff at all, so
    // that hard zero read as a visible seam where the aurora clipped against it. Widening the fade
    // zone (without changing the bright "core" size) blends the two smoothly instead.
    float distDeg = abs(degrees(colat) - centerDeg);
    float band = smoothstep(widthDeg * 2.0, widthDeg * 0.5, distDeg);
    return band * auroraCoverage(colat, az, t, storm);
}

// Curtain fold structure: many thin vertical sheets standing up off the surface, distributed
// around the ring — NOT rays radiating toward/away from the pole (two earlier attempts produced
// exactly that "spokes pointing at the pole" look: first by anisotropically stretching the wrong
// pair of axes, then by picking a colat frequency that LOOKED small as a raw number but was still
// physically enormous once multiplied by Earth's radius — see the frequency constants' comment
// above for the physical-cell-size reasoning that fixed it for real). Built from two anisotropic
// warpPerlin3 samples: HIGH frequency along the tangent (azimuthal) axis gives many separate folds
// distributed around the ring; comparably HIGH frequency along colatitude keeps the band's
// cross-section from adding unwanted radial coherence; LOW frequency along ALTITUDE is the one
// genuinely long axis, keeping each fold an unbroken streak running vertically. storm increases
// fold frequency (more chaotic structure).
//
// Time (cloud.auroraShimmerRate) drives the TANGENT/azimuthal axis, NOT altitude — a first version
// added it directly to the altitude coordinate, which made every fold visibly scroll monotonically
// top-to-bottom (reported as "columns flicker from top to bottom" — the same axis-conflation
// mistake auroraCoverage's colat/time bug was, one layer down). Altitude is a real physical
// direction (up), so translating it with time reads as a directional slide.
//
// It's mixed in via a WARP PHASE (a separate warpPerlin3 evaluation with t as one of its inputs),
// NOT added directly as `+ t*rate` — a second version did the direct-add version, which fixed the
// top-to-bottom slide but was reported as looking like "a spinning texture moving east to west":
// still a rigid, linear translation, just along a different (harmless-direction) axis instead of a
// wrong one. A pure additive shift is mechanical regardless of which axis it's on — real curtains
// don't slide sideways at constant velocity, they morph. Routing time through its own noise
// evaluation first (same technique cloudWarpOffset already uses for cloud shape) means the phase
// itself changes non-monotonically over time, and — since colat/altM feed the SAME warp evaluation
// — varies smoothly across the curtain instead of shifting every fold by an identical amount in
// lockstep, so it reads as evolving structure rather than the whole pattern sliding as one rigid
// sheet.
float auroraCurtainNoise(float colat, float az, float altM, float t, float storm) {
    float shimmerPhase = warpPerlin3(vec3(colat * kAuroraRadialFreq * 0.3,
                                           altM * kAuroraAltFreq * 0.3,
                                           t * cloud.auroraShimmerRate)) * 2.5;
    vec3 p = vec3(az * kAuroraTangentFreq * (1.0 + storm * 0.8) + shimmerPhase,
                  altM * kAuroraAltFreq,
                  colat * kAuroraRadialFreq);
    // Perf (this session): "base" is now a texture read against auroraNoiseTex's R channel
    // (baked once by aurora_noise.comp) instead of a live warpPerlin3 call — the biggest single
    // piece of "why is aurora so much more expensive than clouds, which look more complex"
    // (clouds' noise was already baked in a prior session; aurora's never was). shimmerPhase above
    // stays LIVE and cheap (1 call) — it's the animation driver, offsetting the sample coordinate
    // into the static baked texture, same "warp the lookup, bake the detail" split cloudWarpOffset
    // already uses for clouds. (Also dropped the "detail" octave earlier this session — one fewer
    // call — before this replaced the remaining "base" call entirely.)
    //
    // U wraps at the TRUE physical period (kAuroraTangentFreq*2*PI≈251.3), independent of the
    // bake's own internal resolution (256 cells/loop — a power-of-2 approximation of that period,
    // required for correct tiling, see aurora_noise.comp). fract() here handles both the wrap AND
    // storm's frequency-scaling of p.x correctly: a p.x that completes more physical cycles per
    // loop (storm scales the az term) just wraps U more times, which reads as "more folds" — the
    // same visual effect storm produced before, now via repetition of the baked pattern rather
    // than genuinely new higher-frequency noise (an accepted approximation).
    // V/W are clamped to the bake's fixed ranges (see aurora_noise.comp): p.y in [0.24, 2.46]
    // (= [kAuroraMarchInnerM, kAuroraMarchOuterM] * kAuroraAltFreq), p.z in [0, 91.6]
    // (= [0, 75deg] * kAuroraRadialFreq).
    const float kAuroraCurtainPeriod = kAuroraTangentFreq * 2.0 * PI;
    float texU = fract(p.x / kAuroraCurtainPeriod);
    float texV = clamp((p.y - 0.24) / (2.46 - 0.24), 0.0, 1.0);
    float texW = clamp(p.z / 91.6, 0.0, 1.0);
    float base = texture(auroraNoiseTex, vec3(texU, texV, texW)).r * 2.0 - 1.0;
    return remap(base, -0.3, 0.9, 0.0, 1.0); // bias toward bright folds over dark gaps
}

// Combined density + color sample at a point given in the observer's local ENU-scaled frame
// (rp; same frame obsPos/dir/main()'s march points use), converted to a true ECEF direction via
// the enuX/enuY/enuZ basis — mirrors the rDirECEF idiom the airglowRed march already uses.
// Picks whichever geomagnetic pole (north or south) the point is nearer to, so one code path
// covers both hemispheres.
//
// Day-gated per-SAMPLE on that point's own geographic day/night state (same twilight window
// airglowRed's rDayness/rNight uses), NOT on the observer's local sky brightness as originally
// planned. The observer-based gate (a single smoothstep on pc.sunDirENU.w, applied once outside
// the march) was wrong for an orbital view near the terminator: an observer whose own local sun
// angle reads "daylight" can still be looking at a geographically dark limb with a large visible
// night-side portion, and the old gate blacked out the aurora there entirely instead of letting
// it fade in over the genuinely dark samples along that same ray.
vec3 auroraSampleAt(vec3 rp, vec3 enuX, vec3 enuY, vec3 enuZ, vec3 sunDirECEF, float t, float storm) {
    vec3 pDirECEF = normalize(rp.x * enuX + rp.y * enuY + rp.z * enuZ);
    float dayness = clamp((dot(pDirECEF, sunDirECEF) + 0.15) / 0.3, 0.0, 1.0);
    float night   = 1.0 - dayness;
    if (night <= 0.001) return vec3(0.0); // cheapest test first: this patch of sky is in daylight
    vec3 poleDir  = (dot(pDirECEF, kGeomagPoleECEF) > 0.0) ? kGeomagPoleECEF : -kGeomagPoleECEF;
    float colat, az; vec3 radialT, tangentT;
    auroraFrame(pDirECEF, poleDir, colat, az, radialT, tangentT);
    float oval = auroraOvalMask(colat, az, t, storm);
    if (oval <= 0.001) return vec3(0.0); // cheap early-out before the pricier fold noise below
    float altM = length(rp) - R_EARTH;
    // Inner/outer edges kept as SEPARATE terms (not pre-multiplied into one `vert`) so both the
    // fold-contrast fade and the color blend below can use each edge's own weight independently —
    // innerVert also does double duty as the early-out gate via the combined `vert` product.
    // SIGMOID falloff, not smoothstep — smoothstep(edge0,edge1,x) is EXACTLY zero at and below
    // edge0 no matter how far apart edge0/edge1 are; widening the transition just moves where that
    // hard floor sits; it can never remove it. That's why the previous fix (widening to 80-110km)
    // just relocated the visible cut from 95km to exactly 80km instead of eliminating it. A sigmoid
    // asymptotically approaches 0/1 without ever exactly reaching either — no floor to hit at any
    // altitude, so there's nothing left to read as a hard edge. `kAuroraInnerFalloffM`/
    // `kAuroraOuterFalloffM` set the transition's rough WIDTH (effective ~4x this value from ~12%
    // to ~88%), analogous to smoothstep's old span but without the hard endpoint. The march's own
    // bounds (main()) were extended further to match — a sigmoid's tail is still finite in practice
    // (the vert<=0.001 early-out below still culls it eventually), but that cull point needs to
    // actually be reachable by the march, not clipped off before the tail gets sampled at all.
    const float kAuroraInnerFalloffM = 7500.0;
    const float kAuroraOuterFalloffM = 15000.0;
    float innerVert = 1.0 / (1.0 + exp(-(altM - kAuroraShellInnerM) / kAuroraInnerFalloffM));
    float outerVert = 1.0 / (1.0 + exp((altM - kAuroraShellOuterM) / kAuroraOuterFalloffM));
    float vert = innerVert * outerVert;
    if (vert <= 0.001) return vec3(0.0);
    // Fold contrast is blended toward a flat 1.0 as `vert` approaches its edges (mix(1.0,fold,vert),
    // NOT raw fold) — softening `vert` alone (the earlier fix) made the DENSITY/ALPHA transition
    // gradual, but the fold NOISE's own structure stayed at full contrast right up until vert hit
    // zero, so the curtain's sharply-textured folds still snapped straight to airglow's smooth,
    // uniform glow at the boundary — a texture/character discontinuity, not a brightness one, and
    // exactly why softening the density alone didn't read as a real blend. Fading the structure
    // itself alongside the density means the curtain smooths out into a uniform glow BEFORE it
    // fades away, so it hands off to airglow's own uniform character instead of cutting to it.
    // Per-column elevation window: without this, every column spans the FULL inner-to-outer shell
    // (vert/innerVert/outerVert above are the same at every colat/az), so every fold shows the same
    // complete base->top gradient — reads as suspiciously uniform. Real curtains vary in height: some
    // barely lift off the ~95km base, others tower to the full ~300km extent. Sampled as a function of
    // (colat, az) ONLY — no altitude, no time — so it's constant all the way up a given column (that's
    // the definition of "this column's own height range") and stable frame to frame rather than
    // flickering. Low frequency (kAuroraColumnFreq, well below the fold texture's own tangent/radial
    // frequencies) so one "column" here bundles many individual fold-noise folds together, matching
    // the real scale where dozens of thin folds share one taller or shorter structure.
    //
    // Deliberately kept SEPARATE from vert/innerVert/outerVert rather than replacing them — those two
    // still drive the color blend and the airglow hand-off at the TRUE shell bounds (kAuroraShellInnerM/
    // OuterM), which must stay physically anchored there regardless of any one column's random window.
    // This only gates final visibility/opacity on top.
    const float kAuroraColumnFreq = 9.0;
    // Perf (this session): colA/colB are now a single texture read against auroraNoiseTex's G/B
    // channels (baked by aurora_noise.comp) instead of 2 separate live warpPerlin3 calls — no
    // per-frame animation here to preserve (the live version had none either: no altitude, no
    // time — constant all the way up a given column, see the comment above), so this is a
    // straightforward bake with no runtime coordinate warp needed. az's column-specific frequency
    // (kAuroraColumnFreq) cancels out of the wrap-period division below (az*freq / (freq*2*PI) =
    // az/(2*PI)), so the U mapping is independent of it; W reuses the exact same colatitude-
    // fraction formula as the curtain sample above (colat / radians(75)) — both channels were
    // baked over the identical [0,75deg] colatitude range, just at different internal frequencies.
    float colTexU = fract(az / (2.0 * PI));
    float colTexW = clamp(colat / radians(75.0), 0.0, 1.0);
    vec2  colSample = texture(auroraNoiseTex, vec3(colTexU, 0.5, colTexW)).gb;
    float colA = colSample.x;
    float colB = colSample.y;
    // Sorting two decorrelated samples into lo/hi (instead of deriving lo/hi from one center+halfwidth)
    // naturally produces the full requested spread: when colA/colB land close together the window is
    // narrow (low OR high depending on where), when they land far apart (near 0 and near 1) the window
    // covers nearly the whole shell — "some just low, some just high, others span the full distance"
    // falls out of this without needing separate special cases.
    float colLoFrac = clamp(min(colA, colB) - 0.08, 0.0, 1.0);
    float colHiFrac = clamp(max(colA, colB) + 0.08, 0.0, 1.0);
    float colLoM = mix(kAuroraShellInnerM, kAuroraShellOuterM, colLoFrac);
    float colHiM = mix(kAuroraShellInnerM, kAuroraShellOuterM, colHiFrac);
    const float kAuroraColumnFalloffM = 12000.0;
    float columnWindow = (1.0 / (1.0 + exp(-(altM - colLoM) / kAuroraColumnFalloffM)))
                        * (1.0 / (1.0 + exp((altM - colHiM) / kAuroraColumnFalloffM)));
    if (columnWindow <= 0.001) return vec3(0.0);
    float fold = mix(1.0, auroraCurtainNoise(colat, az, altM, t, storm), vert);
    vec3  col  = mix(kAuroraBaseColor, kAuroraTopColor,
                      clamp(remap(altM, kAuroraShellInnerM, kAuroraShellOuterM, 0.0, 1.0), 0.0, 1.0));
    // Hue also blends toward the nearby airglow band's own color in each edge zone — green/sodium
    // near the inner edge (matching their real 83-105km presence), red near the outer edge (matching
    // its real 200-350km presence) — instead of aurora's own base/top gradient just stopping short
    // and handing off to a completely differently-colored airglow with no shared transition at all.
    vec3 innerAirglowCol = mix(kAirglowSodiumColor, kAirglowGreenColor, 0.5);
    col = mix(innerAirglowCol, col, innerVert);
    col = mix(kAirglowRedColor, col, outerVert);
    return col * (oval * fold * vert * columnWindow * night);
}

// Representative altitude for auroraGlowAt's fold-noise texture — the ground-glow term doesn't
// march a real altitude, it just needs *a* fixed altitude to sample the curtain's horizontal
// structure at (mid-shell, roughly where the green base is brightest).
const float kAuroraGroundGlowAltM = 150000.0;

// Local aurora ambient light AT A GIVEN GEOGRAPHIC POINT (terrain/ocean hit point, ocean-reflection
// sample, etc.) — evaluates the SAME oval mask + curtain-fold noise the sky curtain itself uses,
// but keyed on that point's own geographic location instead of the observer's. This is what makes
// aurora ground-lighting properly local, the same way moonlight is: a patch of ground directly
// under an active curtain lights up regardless of where the observer is standing, and moving the
// observer somewhere else doesn't turn it off. (The first implementation computed a single CPU-side
// value from the OBSERVER's position and applied it to everything in view — from LEO that lit the
// entire visible Earth uniformly green whenever the observer's orbit passed over the oval, and shut
// off instantly the moment it didn't, regardless of what was actually under the curtain. See
// TERRAIN_PLAN.md session 28 follow-up #5.)
vec3 auroraGlowAt(vec3 posDirECEF, vec3 sunDirECEF, float t, float storm) {
    float dayness = clamp((dot(posDirECEF, sunDirECEF) + 0.15) / 0.3, 0.0, 1.0);
    float night   = 1.0 - dayness;
    if (night <= 0.001) return vec3(0.0);
    vec3 poleDir  = (dot(posDirECEF, kGeomagPoleECEF) > 0.0) ? kGeomagPoleECEF : -kGeomagPoleECEF;
    float colat, az; vec3 radialT, tangentT;
    auroraFrame(posDirECEF, poleDir, colat, az, radialT, tangentT);
    float oval = auroraOvalMask(colat, az, t, storm);
    if (oval <= 0.001) return vec3(0.0);
    float fold = auroraCurtainNoise(colat, az, kAuroraGroundGlowAltM, t, storm);
    return kAuroraBaseColor * oval * fold * night;
}

// ── Lens flare (adapted from "Lens Flare Example" by peterekepeter, public domain)
// ─────────────────────────────────────────────────────────────────────────────
// Produces the visible corona/bloom around the source AND the reflected ghost
// artifacts that appear along the flare axis (source -> screen centre -> beyond).
// Diffraction spikes are intentionally omitted; instead the irregular corona
// shape (human-eye / dirty-lens airy-disk pattern) is produced entirely by the
// noise texture lookup on f0.
//
// Coordinate space: ShaderToy-style UV.
//   x in [-0.5*aspect, +0.5*aspect],  y in [-0.5, +0.5].
//
// Parameters:
//   uv     -- current fragment position in flare UV space
//   pos    -- source (satellite / sun) position in flare UV space
//   intens -- normalised brightness [0,1]; controls f0 scale and ghost strength
//
// Returns an HDR additive RGB contribution.
// The call site multiplies by a tint and an overall scale factor.
// ─────────────────────────────────────────────────────────────────────────────
// bokehMult: independent brightness scalar for the ghost/bokeh elements (f2–f6).
// Use a small value (e.g. 0.3) for satellites, larger (e.g. 2.0) for the sun.
// Separates corona brightness (intens) from artifact brightness (bokehMult).
vec3 lensFlare(vec2 uv, vec2 pos, float intens, float bokehMult) {

    // uvd: radially distorted UV -- uv * |uv|.
    // Near screen centre uvd ~= 0; toward edges it bends outward.
    // Ghost artifacts use uvd so their positions follow the curved optical path
    // of real multi-element lens reflections.
    vec2 uvd = uv * length(uv);

    // d: displacement from current fragment to source.
    vec2 d = uv - pos;

    // dist: radius^0.1 -- nearly 1.0 everywhere, dips to 0 right at the source.
    // Used as a small radial term in f0's shimmer modulation.
    float dist = pow(length(d), 0.1);

    // ang: polar angle [-pi, +pi] around the source.
    // Used to sample the noise texture angularly so the corona has irregular lobes.
    float ang = atan(d.y, d.x);

    // ── Angular corona noise via texture lookup ────────────────────────────────
    // Replicates the original ShaderToy formula:
    //   noise(sin(ang*4 + pos.x)*4 - cos(ang*3 + pos.y))
    //
    // The argument is a smoothly-varying scalar that changes both with the angle
    // around the source (ang) and with the source's screen position (pos.x, pos.y).
    // This means each satellite at a different screen position has a unique corona
    // shape -- the lobes don't align between adjacent satellites.
    //
    // Mapping the scalar to a UV coordinate for noiseTex:
    //   We use a 1D slice along the texture's x-axis (v = 0.5, middle row).
    //   The u coordinate wraps via the REPEAT sampler so any float value is valid.
    //   The noise value (red channel) is then passed into sin(...*16)*0.1 which
    //   creates fine angular variation (+/-10%) around the corona rim.
    // noiseSeed is a smoothly-varying float that changes with angle and source position.
    // We map it into [0,1] UV space by dividing by the expected range (~8) and adding
    // 0.5 to centre it, then rely on REPEAT wrapping for values outside [0,1].
    // Using fract() explicitly makes the wrapping behaviour unambiguous.
    // The v coordinate is fixed at 0.25 (upper quarter of texture, away from the
    // edge to avoid any border artifacts on some hardware).
    float noiseSeed = sin(ang * 4.0 + pos.x) * 4.0 - cos(ang * 3.0 + pos.y);
    float noiseU    = fract(noiseSeed * 0.125 + 0.5); // map [-8,+8] -> [0,1], wrapping
    float angNoise  = texture(noiseTex, vec2(noiseU, 0.25)).r;

    // ── Source glow: Lorentzian corona centered on the source ─────────────────
    // The Lorentzian  1/(r * scale + 1)  is wider and softer than a Gaussian,
    // matching real lens-coating scatter on a bright point source.
    //
    float scale = 1200.0; // corona radius: higher = tighter. 60 = wide (visible at 200px), 1200 = tight (visible at ~15px)
    //   r = 0.005 (~5px at 1080p):  f0 = 1/(0.005*60+1) = 0.77
    //   r = 0.02  (~22px):          f0 = 1/(0.02 *60+1) = 0.45
    //   r = 0.05  (~54px):          f0 = 1/(0.05 *60+1) = 0.25
    //   r = 0.10  (~108px):         f0 = 1/(0.10 *60+1) = 0.14
    //   r = 0.20  (~216px):         f0 = 1/(0.20 *60+1) = 0.077
    // This gives a wide, visible corona that extends well past the satellite dot
    // and fades naturally without a hard edge.  The old scale of 200 fell to
    // <0.05 at only 50px, making the corona invisible at our additive blend scale.
    //
    // The modulation line applies the noise-driven angular shimmer:
    //   sin(angNoise * 16) * 0.1  -- fine ripple from texture (+/- 10% per lobe)
    //   dist * 0.1                -- barely-there radial taper (~constant ~1)
    //   + 0.8                     -- base boost so the corona is always bright
    // sin(noise*16) oscillates rapidly around the corona, creating 8-16 irregular
    // bright lobes -- the airy-disk / human-eye diffraction pattern.
    float f0 = 1.0 / (length(d) * scale + 1.0);
    f0 = f0 + f0 * (sin(angNoise * 16.0) * 20.8 + dist);
    // Scale by intensity so dimmer satellites have a proportionally smaller corona.
    f0 *= 0.1;// + intens * 0.5);

    // ── Large near-source bloom: soft blob mirrored through screen centre ──────
    // Placed at -1.2*pos (reflected slightly beyond centre).
    // Represents light that bounced backward through the lens and re-emerged near
    // the entrance pupil.  Multiplier 4.0 (reduced from original 7.0) and
    // contribution capped below to prevent peripheral over-saturation.
    float f1 = max(0.01 - pow(length(uv + 1.2 * pos), 1.9), 0.0) * 4.0;
    f1 *= 0.6;

    // ── Ghost artifacts: fade when source is near screen centre ───────────────
    // When pos ~= (0,0) (looking directly at the source), uvd + k*pos ~= uvd,
    // which is nearly zero everywhere near centre.  The Lorentzian denominator
    // (1 + 32*r^2) then approaches 1 everywhere, lighting up the entire screen.
    //
    // ghostFade = smoothstep(0.03, 0.12, |pos|):
    //   source within ~3% screen height of centre: ghosts = 0
    //   source more than 12% screen height off-centre: ghosts full
    // This also makes physical sense: looking directly at the source means ghost
    // reflection paths don't form visible off-axis elements.
    float ghostFade = smoothstep(0.03, 0.12, length(pos));

    // ── Bokeh halos: large circular rings reflected through screen centre ──────
    // Classic rainbow-ringed bokeh circles opposite the source.
    // Lorentzian  1/(1 + 32*r^2)  matches wide, soft real ghost disc profiles.
    // Three slightly offset RGB positions produce chromatic aberration fringing.

    float f2  = max(1.0/(1.0 + 32.0*pow(length(uvd + 0.80*pos), 2.0)), 0.0) * 0.25 * bokehMult;
    float f22 = max(1.0/(1.0 + 32.0*pow(length(uvd + 0.85*pos), 2.0)), 0.0) * 0.23 * bokehMult;
    float f23 = max(1.0/(1.0 + 32.0*pow(length(uvd + 0.90*pos), 2.0)), 0.0) * 0.21 * bokehMult;

    // ── Star-shaped secondary bokeh (between source and centre) ───────────────
    // uvx = 1.5*uv - 0.5*uvd.  The 2.4 exponent gives a slightly star-shaped
    // profile (intermediate between circle and square).
    // RGB variants at 0.40/0.45/0.50*pos create a second tier of chromatic split.
    vec2 uvx = mix(uv, uvd, -0.5);
    float f4  = max(0.01 - pow(length(uvx + 0.40*pos), 2.4), 0.0) * 6.0;
    float f42 = max(0.01 - pow(length(uvx + 0.45*pos), 2.4), 0.0) * 5.0;
    float f43 = max(0.01 - pow(length(uvx + 0.50*pos), 2.4), 0.0) * 3.0;

    // ── Compact sparkle dots along the flare axis ─────────────────────────────
    // High exponent (5.5) = sharp dropoff = tight bright pinpoints at 0.2/0.4/0.6*pos.
    uvx = mix(uv, uvd, -0.4);
    float f5  = max(0.01 - pow(length(uvx + 0.20*pos), 5.5), 0.0) * 2.0;
    float f52 = max(0.01 - pow(length(uvx + 0.40*pos), 5.5), 0.0) * 2.0;
    float f53 = max(0.01 - pow(length(uvx + 0.60*pos), 5.5), 0.0) * 2.0;

    // ── Broad streaks on the camera-side of centre ────────────────────────────
    // Negative multiplier places these between centre and the source.
    // Low exponent (1.6) = broad, diffuse -- reads as a smear on the front element.
    uvx = mix(uv, uvd, -0.5);
    float f6  = max(0.01 - pow(length(uvx - 0.300*pos), 1.6), 0.0) * 6.0;
    float f62 = max(0.01 - pow(length(uvx - 0.325*pos), 1.6), 0.0) * 3.0;
    float f63 = max(0.01 - pow(length(uvx - 0.350*pos), 1.6), 0.0) * 5.0;

    // (A "radial ray fan" sunburst stand-in for screen-space godrays lived here briefly,
    // 2026-07-29 — reverted same day per user feedback: too sharply-defined/star-like for the
    // soft, astigmatism-like human-eye look this flare is going for, and made the many-satellite
    // Reflect-Orbital case look worse, not better. The prior streak/bokeh terms above (f4-f6) are
    // the preferred "spike" look. Screen-space godrays remain a real, unimplemented want — see
    // TERRAIN_PLAN.md's follow-up log for the "threshold + depth-subtract + radial blur" direction
    // proposed instead.)

    // ── Assemble ──────────────────────────────────────────────────────────────
    vec3 c = vec3(0.0);

    // Source corona -- achromatic (warm white set by call-site tint).
    c += vec3(f0);
    c += vec3(f1 * 0.5);  // bloom at -1.2*pos

    // Ghost terms: chromatic, gated by ghostFade to prevent centre blowout.
    // bokehMult independently scales all ghost/reflection artifacts from the corona (f0).
    c.r += (f2  + f4  + f5  + f6)  * 0.4 * ghostFade * bokehMult;
    c.g += (f22 + f42 + f52 + f62) * 0.4 * ghostFade * bokehMult;
    c.b += (f23 + f43 + f53 + f63) * 0.4 * ghostFade * bokehMult;

    // Slight vignette: outer screen positions have more lens distortion.
    c = c * 1.3 - vec3(length(uvd) * 0.05);

    return max(c, vec3(0.0));
}

// ── Ocean wave functions (adapted from "Seascape" by Alexander Alekseev aka TDM, 2014)
// License: CC-BY-NC-SA 3.0 — tdmaav@gmail.com
// posM = ENU East/North metres + geographic phase offset (observer-relative, ~Earth-fixed);
// pHeight = metres above R_EARTH; seaTime = 1.0 + pc.waveTime * kSeaSpeed.

const mat2  kOctaveM       = mat2(1.6, 1.2, -1.2, 1.6);
const float kSeaFreq       = 0.056;
const float kSeaHeight     = 2;
const float kSeaChoppy     = 3.0;   // 4.0 → 2.0: rounder crests, less plateau cliffs
const float kSeaSpeed      = 1.5;
const vec3  kSeaBase = vec3(0.01, 0.04, 0.08);   // dark, desaturated blue
const vec3  kSeaWaterColor = vec3(0.2, 0.50, 0.85) * 0.1;

// Hash without Sine (Dave Hoskins, MIT): stable for all float input magnitudes.
// The original fract(sin(dot(p, large_vec))*large_num) loses GPU sin() precision
// once the dot product exceeds ~10^4 (happens at 4th-5th octave where kOctaveM
// doubles UV scale each iteration), producing the angular banding artifact.
float seaHash(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973) * 0.1); //vec3(0.1031, 0.1030, 0.0973)
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}
float seaNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return -1.0 + 2.0 * mix(
        mix(seaHash(i + vec2(0.0, 0.0)), seaHash(i + vec2(1.0, 0.0)), u.x),
        mix(seaHash(i + vec2(0.0, 1.0)), seaHash(i + vec2(1.0, 1.0)), u.x),
        u.y);
}

float seaOctave(vec2 uv, float choppy) {
    uv += seaNoise(uv);
    vec2 wv  = 1.0 - abs(sin(mod(uv, vec2(2.0 * PI, 2.0 * PI))));
    vec2 swv = abs(cos(mod(uv, vec2(2.0 * PI, 2.0 * PI))));
    wv = mix(wv, swv, wv);
    return pow(1.0 - pow(wv.x * wv.y, 0.65), choppy);
}

// Geometry pass (3 octaves default): used in height-map trace. Octave count is UBO-tunable
// (cloud.oceanSeaOctaves, perf session 24) — this is called up to 10x per ocean pixel by
// heightMapTracing's secant refinement, so it's a direct multiplicative cost lever.
float seaMap(vec2 posM, float pHeight, float seaTime) {
    float freq = kSeaFreq, amp = kSeaHeight, choppy = kSeaChoppy;
    vec2  uv   = posM; uv.x *= 0.75;
    float h    = 0.0;
    int   nOct = int(max(1.0, cloud.oceanSeaOctaves));
    for (int i = 0; i < nOct; i++) {
        float d  = seaOctave((uv + seaTime) * freq, choppy);
              d += seaOctave((uv - seaTime) * freq, choppy);
        h  += d * amp;
        uv *= kOctaveM; freq *= 1.9; amp *= 0.22;
        choppy = mix(choppy, 1.0, 0.2);
    }
    return pHeight - h;
}

// Fragment pass (5 octaves default): used for high-quality normal computation. Octave count is
// UBO-tunable (cloud.oceanDetailOctaves, perf session 24).
float seaMapDetail(vec2 posM, float pHeight, float seaTime) {
    float freq = kSeaFreq, amp = kSeaHeight, choppy = kSeaChoppy;
    vec2  uv   = posM; uv.x *= 0.75;
    float h    = 0.0;
    int   nOct = int(max(1.0, cloud.oceanDetailOctaves));
    for (int i = 0; i < nOct; i++) {
        float d  = seaOctave((uv + seaTime) * freq, choppy);
              d += seaOctave((uv - seaTime) * freq, choppy);
        h  += d * amp;
        uv *= kOctaveM; freq *= 1.9; amp *= 0.22;
        choppy = mix(choppy, 1.0, 0.2);
    }
    return pHeight - h;
}

// ── Thin-shell cloud layer evaluator ─────────────────────────────────────────
// Intersects a sphere shell at R_EARTH + shellAltM, samples earthCloudsTex at the
// hit point's geographic lat/lon (Earth-fixed UV + per-layer longitude drift), and
// blends the result into `color`.
//
// Lighting uses dot(normalize(cloudPointECEF), sunDirECEF) — the sun angle at the
// cloud's own geographic location, NOT the observer's sun elevation.  This ensures
// clouds on the dark side of Earth are dark regardless of where the observer is.
void evalCloudLayer(
    vec3  obsPos,  vec3 dir,  float tSurface,
    vec3  enuX,    vec3 enuY, vec3  enuZ,
    vec3  sunDirECEF,
    float odRcam,  float odMcam,
    float coverage, float density, float sunGain, float sunGainZenith,
    float shellAltM, float driftMult, float alphaMax, float mipLod,
    float cloudPhase,
    float obsEffH, float volumetricPair,
    vec3  skyOnlyColor,
    inout vec3 color)
{
    // Runtime-tunable scattering strength — shadows the physical base constants (common.glsl)
    // with the user-facing "Rayleigh gain"/"Mie/haze gain" sliders. See cloud_params.glsl's
    // atmosRayleighGain/atmosMieGain comment for what each one does perceptually.
    //
    // flatRayleighGain ("2D Rayleigh gain") stacks on top of the global Rayleigh gain and applies
    // to THIS function only — the flat 2D paste — so the 3D->2D crossfade can be matched without
    // moving the sky/volumetric look. It lands on both uses of BETA_R below, which is the point:
    // sunColorFlat (the sun's own colour arriving AT the cloud) and attn/airlight (the
    // camera->cloud path) together are what set how red and how washed-out the flat layer reads
    // relative to the volumetric shell it fades into. 1.0 = the previous coupled behaviour
    // exactly. See cloud_params.glsl's flatRayleighGain comment.
    vec3  BETA_R = BETA_R_BASE * cloud.atmosRayleighGain * cloud.flatRayleighGain;
    float BETA_M = BETA_M_BASE * cloud.atmosMieGain;

    vec2  tc = raySphere(obsPos, dir, R_EARTH + shellAltM);
    float t  = (tc.x > 0.001) ? tc.x : tc.y;
    if (t <= 0.001) return;
    if (tSurface > 0.0 && t >= tSurface) return;

    // Crossfade against the volumetric pass, for the two layers it also renders (0 and 1). This
    // has to be computed HERE rather than at the call site, because it depends on t — the
    // distance to the shell along THIS ray — and the call site does not have it. That is the whole
    // point of moving to a distance-keyed fade: it varies across the screen, so near clouds can be
    // volumetric while horizon clouds on the same frame are flat. The weight is the exact
    // complement of cloudMarchCS's, so the two always sum to one.
    if (volumetricPair > 0.5) {
        float altFade  = 1.0 - smoothstep(kCloud3DFadeStart, kCloud3DFadeEnd, obsEffH);
        float distFade = 1.0 - smoothstep(cloud.cloudDistFadeStartM, cloud.cloudDistFadeEndM, t);
        alphaMax *= 1.0 - min(altFade, distFade);
        if (alphaMax < 0.001) return;
    }

    // Hit point in ENU → convert to ECEF for geographic UV and sun-dot
    vec3  hitENU = obsPos + t * dir;
    vec3  cECEF  = hitENU.x * enuX + hitENU.y * enuY + hitENU.z * enuZ;
    float cL     = length(cECEF);
    float cLon   = atan(cECEF.y, cECEF.x);
    float cLat   = asin(clamp(cECEF.z / cL, -1.0, 1.0));

    // The map at this Earth-fixed direction, turned by the per-layer longitude drift into the weather
    // cube's (drifted) frame. The cube's mip 0 is the equirect map's mip 1 (5 km texels).
    float dph   = cloudPhase * driftMult;
    vec3  dE    = cECEF / cL;
    vec3  dMap  = vec3(dE.x * cos(dph) - dE.y * sin(dph), dE.x * sin(dph) + dE.y * cos(dph), dE.z);
    float raw   = textureLod(earthCloudsCube, dMap, max(mipLod - 1.0, 0.0)).r;
    float alpha = clamp((raw - (1.0 - coverage)) * density, 0.0, alphaMax);
    if (alpha <= 0.0) return;

    // Sun angle at the cloud's geographic position — independent of observer location
    float cloudSunDot  = dot(normalize(cECEF), sunDirECEF);
    float cloudDayFrac = smoothstep(-0.1, 0.15, cloudSunDot);
    // See cloud_march.comp's cloudMarchCS sunGainCurve comment — same horizon/zenith blend.
    float sunGainCurve = mix(sunGain, sunGainZenith,
                             smoothstep(0.0, max(cloud.sunGainElevBand, 0.02),
                                        clamp(cloudSunDot, 0.0, 1.0)));
    // Soft-compressed rather than the old raw product. This term fed straight into the composite
    // unbounded, so a sunGain tuned to give the VOLUMETRIC path good sunsets drove the flat layer
    // hard into pure white — the two paths respond to the same slider completely differently,
    // because the volumetric accumulates through transmittance while this is a single multiply.
    // 1-exp(-x) is ~x for small x (dim clouds unchanged) and asymptotes to 1 instead of clipping,
    // which is also about right physically: a fully lit cloud's albedo is ~0.7-0.9, not unbounded.
    // Spectral sun colour at the CLOUD's own geographic position. This term used to be a pure
    // white vec3, which is precisely why raising sun gain only ever made 2D clouds BRIGHTER and
    // never orange: the flat path had no wavelength-dependent factor anywhere in it, while the
    // volumetric path multiplies by sunColorCloud — its own optDepth-derived sun transmittance at
    // shell entry. This is the same integral, evaluated in ECEF from the cloud point toward the
    // sun, so at a grazing sun the long air path extinguishes blue and green and what actually
    // reaches the cloud is genuinely orange/red rather than white scaled up.
    //
    // The soft 1-exp(-x) compression below then works per channel, so at high gain red saturates
    // first and the colour survives instead of washing to white the way a scaled grey did.
    //
    // dbgSkipSunOD() makes optDepth return 0, hence vec3(1.0) — exactly the old white behaviour —
    // so the existing sun-OD knockout bit isolates this cleanly at zero extra plumbing.
    // Two details here are copied deliberately from cloudMarchCS's sunColorCloud block rather
    // than reinvented, because getting either one wrong shifts the HUE relative to the volumetric
    // path — which is exactly what the first version of this did (2D read distinctly redder):
    //
    //   * BETA_M * 1.1, not BETA_M. Mie extinction is wavelength-neutral, so it is the term that
    //     dilutes Rayleigh's red with grey. Using a 10% weaker Mie coefficient than the volumetric
    //     leaves proportionally less grey in the mix, i.e. a MORE saturated red, for the same
    //     geometry.
    //   * The Earth-shadow gate. raySphere against R_EARTH first: if the sun ray from this cloud
    //     point enters the planet, the cloud is in Earth's shadow and gets no sunlight at all.
    //     Without it, optDepth happily integrates a chord that passes underground — where its
    //     max(0, length-R_EARTH) altitude clamp reads maximum air density the whole way — yielding
    //     an enormous, extremely red-shifted colour just past the terminator. The volumetric has
    //     already gone to zero there, so that band was the "dark red" tail with no 3D counterpart.
    //     cloudDayFrac's smoothstep only fades this out by cloudSunDot = -0.1, leaving the whole
    //     sunset band running on the ungated value.
    //
    // SUN_INTENSITY is deliberately NOT copied — the flat path's magnitude calibration lives in
    // sunGain * flatSunGainScale instead, and folding in a second large constant would just move
    // where those sit. Only the wavelength RATIO has to match, and it now does.
    vec3  sunColorFlat = vec3(0.0);
    vec2  tSunEarth    = raySphere(cECEF, sunDirECEF, R_EARTH);
    if (!(tSunEarth.x > 0.0 && tSunEarth.y > 0.0)) {
        vec2 tSunAtm = raySphere(cECEF, sunDirECEF, R_ATMOS);
        if (tSunAtm.y > 0.0) {
            vec2 odSun = optDepth(cECEF, sunDirECEF, tSunAtm.y);
            sunColorFlat = exp(-(BETA_R * odSun.x + BETA_M * 1.1 * odSun.y));
        }
    }
    vec3  cloudLit     = sunColorFlat * (max(0.0, cloudSunDot + 0.1) * sunGainCurve) * cloudDayFrac;

    // Moonlight — this flat path had no night-side light source at all (cloudDayFrac zeroes
    // cloudLit once the sun sets), unlike the volumetric shell's moonContrib (cloud_march.comp)
    // or terrain's own moonContribTerrain. Same geographic-dot gate as those, and reuses
    // cloud.moonGain so all three stay calibrated to the same brightness.
    vec3  moonDir3     = normalize(moonDirENU.xyz);
    vec3  moonDirECEF  = moonDir3.x * enuX + moonDir3.y * enuY + moonDir3.z * enuZ;
    float cloudMoonDot = dot(normalize(cECEF), moonDirECEF);
    float moonLit      = max(0.0, cloudMoonDot) * moonDirENU.w;
    vec3  moonContrib  = vec3(0.92, 0.95, 1.0) * moonLit * cloud.moonGain;

    // ── Twilight sky ambient (flat 2D path) ──────────────────────────────────────────────────
    // Until this was added the flat layer had NO ambient term of any kind — its only light
    // sources were direct sun (cloudLit, which cloudDayFrac drives to zero at the terminator) and
    // moonlight. The volumetric shell has had a twilight ambient since session 28: sky-lit cloud
    // at dusk/dawn, so clouds read consistently against the sky-lit TERRAIN beside them instead
    // of dropping dark while the ground is still visibly blue. Across the 3D->2D crossfade that
    // asymmetry read as flat clouds going black through twilight while the volumetric ones next
    // to them stayed blue — the airlight term below is NOT a substitute, since that is light
    // scattered in FRONT of the cloud (so it grows with distance and vanishes on near clouds),
    // not downwelling sky light falling ON it.
    //
    // Deliberately a near-verbatim copy of cloudMarchCS's twilightAmbient + skyAmbientBase blocks
    // (same bell, same fixed widths, same UBO edges, same 6-step zenith integral, same
    // cloud-anchored p0) — matching the crossfade is the entire purpose of this term, so anything
    // reinvented here would just have to be re-matched by hand afterwards. Keep the two in sync;
    // same standing rule as the rest of the duplicated cloud code in this file.
    //
    // Three deliberate differences from the volumetric copy:
    //   * The volumetric's `mix(0.3, 0.9, hNorm)` becomes the constant kFlatAmbientHeightMix.
    //     That factor is its vertical shading ramp across a cloud COLUMN, and a flat shell has no
    //     column — 0.6 is that ramp's own midpoint, i.e. what a full column averages to.
    //   * The anchor is this layer's own shell hit point rather than a march entry point, and
    //     needs no altitude clamp: a cloud shell sits at 2-11 km by construction, decades inside
    //     R_ATMOS, so the guard the volumetric keeps has nothing to guard against here.
    //   * The whole block is gated on twilightWeight > 0. That is EXACT, not an approximation —
    //     the weight is a bell, zero in full daylight and zero deep into night — and it keeps six
    //     optDepth calls off every daylit cloud pixel, which at full resolution matters.
    //
    // BETA_R here is the flat-scaled one (flatRayleighGain), so this term's colour tracks the 2D
    // Rayleigh slider the same way sunColorFlat and attn do. That is intended: all three are "how
    // this path sees the atmosphere", and splitting them would make the 2D slider shift hue.
    vec3 twilightAmbient = vec3(0.0);
    {
        // Widths are fixed; only the edges move. Both constants match cloudMarchCS exactly.
        const float kTwilightRiseWidth = 0.2;
        const float kTwilightFallWidth = 0.3;
        const float kFlatAmbientHeightMix = 0.6;
        float twiHi = cloud.twilightBandHi;
        float twiLo = cloud.twilightBandLo;
        float twilightRise = 1.0 - smoothstep(twiHi - kTwilightRiseWidth, twiHi, cloudSunDot);
        float twilightFall = smoothstep(twiLo, twiLo + kTwilightFallWidth, cloudSunDot);
        float twilightWeight = twilightRise * twilightFall;
        float twiGain = cloud.cloudTwilightAmbientGain * cloud.flatTwilightAmbientGain;

        if (twilightWeight > 0.0 && twiGain > 0.0) {
            // Downwelling sky light AT the cloud: a short zenith-ray single-scattering integral
            // anchored on the cloud point itself, not on the observer. Anchoring at the observer
            // is only equivalent when the two are co-located (true from the ground, badly false
            // from orbit, where you can be over the night side looking at a cloud that is still
            // in full twilight) — see the long note on this in cloud_march.comp.
            vec3 skyAmbientBase = vec3(0.0);
            vec3 p0     = cECEF;
            vec3 zenith = normalize(p0);
            vec2 tSA    = raySphere(p0, zenith, R_ATMOS);
            if (tSA.y > 0.0) {
                const int N_Z = 6;
                float zSeg   = tSA.y / float(N_Z);
                float cosAUp = dot(zenith, sunDirECEF);
                float pR_up  = 0.75 * (1.0 + cosAUp * cosAUp);
                float odR_z = 0.0, odM_z = 0.0;
                float skyAmbientBaseM = 0.0;
                for (int zi = 0; zi < N_Z; ++zi) {
                    vec3  sp = p0 + zenith * ((float(zi) + 0.5) * zSeg);
                    float h  = max(0.0, length(sp) - R_EARTH);
                    float dR = exp(-h / H_R) * zSeg;
                    float dM = exp(-h / H_M) * zSeg;
                    odR_z += dR;  odM_z += dM;
                    vec2 tSE = raySphere(sp, sunDirECEF, R_EARTH);
                    if (tSE.x > 0.0 && tSE.y > 0.0) continue;   // this step is in Earth's shadow
                    vec2 tSun  = raySphere(sp, sunDirECEF, R_ATMOS);
                    vec2 sunOD = (tSun.y > 0.0) ? optDepth(sp, sunDirECEF, tSun.y) : vec2(0.0);
                    vec3 tau      = BETA_R * (odR_z + sunOD.x) + BETA_M * 1.1 * (odM_z + sunOD.y);
                    vec3 attnStep = exp(-tau);
                    skyAmbientBase  += attnStep * dR;
                    skyAmbientBaseM += dot(attnStep, vec3(1.0 / 3.0)) * dM;
                }
                float pM_up = phaseM(cosAUp);
                skyAmbientBase = SUN_INTENSITY * (pR_up * BETA_R * skyAmbientBase
                                                + vec3(pM_up * BETA_M * skyAmbientBaseM));
            }
            twilightAmbient = skyAmbientBase * kFlatAmbientHeightMix * twilightWeight * twiGain;
        }
    }

    // Compression applied to LUMINANCE, then scaled back onto the original colour, rather than
    // per channel. This is what caused the reported yellow -> orange -> red -> dark red march
    // across a single sunset while the volumetric only went orange -> dark orange.
    //
    // Per-channel 1-exp(-x) is a saturating curve, so it compresses BRIGHT channels harder than
    // dim ones. On a sunset colour like (1.0, 0.5, 0.2) it lifts green and blue relative to red —
    // it DESATURATES. How much depends on absolute magnitude: near x=0 the curve is the identity
    // (no desaturation at all, full sunset red), and at large x every channel pins near 1 (heavy
    // desaturation, toward yellow/white). So as the sun sets and cloudLit's magnitude falls, the
    // flat layer slides continuously from the compressed end of that curve to the linear end,
    // traversing the whole hue range on the way down. The volumetric never does this because it
    // has no per-channel compressor at all — it accumulates linearly through transmittance.
    // flatSunGainScale (~4x) put this path even deeper into the compressed regime to begin with,
    // which is why its bright end read as yellow rather than orange.
    //
    // Compressing the luminance and rescaling preserves chromaticity exactly, so the hue is now
    // whatever sunColorFlat says it is at every brightness — the same thing the volumetric does.
    // The magnitude roll-off (the reason this compressor exists: an unbounded product drove the
    // flat layer to pure white at volumetric-tuned sun gains) is unchanged.
    //
    // Note this can leave an individual channel above 1.0 for a strongly tinted colour, where the
    // per-channel form could not. That is correct for an HDR value feeding the tone map below, but
    // it is a real change in what this function can return.
    vec3  litSum       = cloudLit + moonContrib + twilightAmbient;
    float litLum       = dot(litSum, vec3(0.2126, 0.7152, 0.0722));
    vec3  cloudColor   = litSum * ((1.0 - exp(-litLum)) / max(litLum, 1e-4));

    // Aerial perspective. `color` at this point already holds the atmosphere's own inscattered
    // light along this ray (the N_VIEW loop that ran before this function), the same value the
    // terrain/ocean surfAttn path adds its own lit color on top of. Without this, a straight
    // mix() to cloudColor*attn at high alpha throws that inscattered light away and replaces it
    // with a plain Beer-Lambert-dimmed cloud color, which trends to black/dark-red at grazing
    // angles (attn shrinks, and BETA_R/BETA_M dim blue harder than red) instead of fading into
    // the horizon haze the way the volumetric clouds do via their additive cloudA/cloudB
    // composite. Blending toward `color` by (1-attn) — the fraction of light scattered INTO the
    // path between the cloud and the camera — fixes that.
    //
    // BUT `color` here is the FULL accumulated ray color, not just sky inscatter — over a terrain
    // hit it already has the ground's own lit surface (including night-lights, `surfColor *
    // surfAttn`, added earlier in main()) folded in. Blending toward it by a flat `(1-attn)`
    // therefore leaked ground light through this layer via a path that has nothing to do with
    // `alpha`/`alphaMax` at all — reported as "opacity scale has zero effect once alpha already
    // saturates," and worst from orbital altitude specifically because `attn` (camera-to-cloud
    // atmosphere) is naturally lower over that much longer path, so the leaked fraction is larger
    // exactly where this layer is doing the most work (the 3D->2D crossfade's flat regime).
    // Gating the leak by `(1-alpha)` fixed THAT leak, but it over-corrected: `(1-alpha)` also
    // zeroes out the airlight (atmosphere-scattered sunlight added INTO the camera-to-cloud
    // segment) for exactly the fully-opaque case, which is the one case that most needs it — a
    // solid horizon-hugging deck at real distance (this is the common case at the true horizon:
    // the shell's own curvature-limited entry point is already 100+ km out at ground level) has
    // `attn` crushed toward zero by BETA_R/BETA_M over that path, so with no airlight term
    // `cloudColor * attn` alone collapses toward black and, because BETA_R/BETA_M extinguish
    // blue/green harder than red, does so through an increasingly saturated yellow/red before
    // going dark — reported as horizon clouds rendering "too yellow, red, and dark" instead of
    // fading into the same orange sunset glow the sky around them already correctly shows.
    //
    // Fix: split `color` into its sky-only part (`skyOnlyColor`, captured in main() right after
    // the N_VIEW atmosphere loop and before any ground/surface light is added) and whatever
    // ground contributed on top (`groundLeak = color - skyOnlyColor`). Airlight drawn from the
    // sky-only part is legitimate regardless of alpha — it represents light scattered in FRONT of
    // the cloud, not whatever is behind it — so it is weighted only by `(1-attn)` and, to match
    // the volumetric shell's own airlight (cloud_march.comp's `airlight`, weighted by how opaque
    // the cloud itself is), by `alpha`: a fully opaque cloud gets the full sky-glow substitute for
    // its own extinguished light, a clear pixel gets none. `groundLeak` keeps exactly the previous
    // `(1-alpha)`-gated treatment, so the original leak fix is untouched.
    vec3 attn       = exp(-(BETA_R * odRcam + BETA_M * 1.1 * odMcam));
    vec3 groundLeak = color - skyOnlyColor;
    vec3 airlight   = skyOnlyColor * (1.0 - attn) * alpha;
    vec3 cloudSeen  = cloudColor * attn + airlight + groundLeak * (1.0 - attn) * (1.0 - alpha);
    color = mix(color, cloudSeen, alpha);
}

// (cloudDensity lived here — dead since the cloud march moved to cloud_march.comp, removed in
//  the pipeline-unification pass. Worth knowing if you go looking for it: this copy blended only
//  TWO fixed Z-slices (0.0 / 0.5) and used a fixed 0.2*erosion floor, while cloud_march.comp's
//  live version blends THREE (0.0 / 0.28 / 0.56) piecewise and takes its erosion floor from the
//  UBO. Another genuine drift that stayed invisible because this copy was already unreachable.)

// ── Cloud raymarch diagnostics ────────────────────────────────────────────────
// Set CLOUD_DEBUG to 1-5 to replace cloud output with a diagnostic overlay.
// Set to 0 for normal rendering.
//
//  1 = 2D coverage at column entry — white=overcast, black=clear.
//      Question: Is the coverage map causing a solid overcast everywhere?
//
//  2 = hNorm of FIRST cloud hit — red=hit near base, green=hit near top, dark-blue=no hit.
//      Question: Are all clouds at the same altitude (should show varying colour if 3D)?
//
//  3 = Fraction of march steps where d>0 — white=solid cloud in this column, black=clear.
//      Question: Is the march mostly in cloud (overcast) or mostly clear (scattered)?
//
//  4 = noiseUVW at march midpoint — R=X, G=Y, B=Z noise coords.
//      Question: Is the noise actually varying in 3D, or is one axis stuck constant?
//
//  5 = posZ value at first cloud hit — greyscale [0,1].
//      Question: Is posZ (the Z anti-banding offset) actually varying across the image?
//      UNIFORM GREY = posZ is constant for all visible pixels = geographic UV barely changes
//      within the visible cloud footprint from ground level. This confirms the Z-layer
//      banding is caused by posZ not spanning enough geographic range from the surface.
#define CLOUD_DEBUG 0

// NOTE: the description below is HISTORY. cloud_shadow.comp no longer exists — the shadow is now
// marched per pixel inside cloud_march.comp from this pixel's own terrain hit point (see
// cloudGroundShadow there) and arrives in cloudTargetB.a. Kept because the reasoning about why
// full-res per-pixel shadowing was too expensive in session 23 still explains why the current
// version runs at half resolution rather than here.
//
// cloudShadowFactor() removed (C15-perf follow-up, session 23) — it was the dominant
// remaining surface-level cloud cost (full-res, ~every terrain/ocean pixel, up to 64
// steps each). Its CloudParams UBO slot reverted to `pad0`, and the CLOUD_ISOLATE_COLH/SHADOW
// debug switches that existed only to isolate this function's seam bugs went with it.
// Cloud shadowing on terrain/ocean is BACK as of C12 (session 32+) via a different mechanism:
// cloud_shadow.comp precomputes the same shadow transmittance once per low-res grid texel
// (128x128) instead of once per screen pixel, sampled here as an O(1) texture read — see
// directSun below.

// cloudMarch()/cirrusMarch() moved to shaders/cloud_march.comp (half-res compute pass —
// C15-perf). main() below now samples cloudTargetA/cloudTargetB (bindings 10/11) instead
// of calling these directly. See TERRAIN_PLAN.md session 23 log for the design.

#ifdef SKY_ENV
// The aurora along this ray: the main renderer marches it in cloud_march.comp (screen space), so the
// env variant runs its own short march through the curtain shell with the same auroraSampleAt(),
// the ocean reflection's recipe with more steps. Handles an observer above, inside or below the shell.
vec3 envAurora(vec3 obsPos, vec3 dir, float tSurface, vec3 enuX, vec3 enuY, vec3 enuZ, vec3 sunDirECEF)
{
    if (cloud.auroraGain <= 0.0) return vec3(0.0);
    vec2 tOut = raySphere(obsPos, dir, R_EARTH + kAuroraShellOuterM);
    if (tOut.y <= 0.0) return vec3(0.0);
    vec2 tIn = raySphere(obsPos, dir, R_EARTH + kAuroraShellInnerM);
    float obsR = length(obsPos);
    float t0 = max(tOut.x, 0.0), t1 = tOut.y;
    if (obsR > R_EARTH + kAuroraShellInnerM) {
        if (tIn.x > 0.0) t1 = tIn.x;              // from above or inside: stop at the curtain base
    } else if (tIn.y > 0.0) {
        t0 = max(t0, tIn.y);                       // from below: start where the ray leaves the base
    }
    if (tSurface > 0.0) t1 = min(t1, tSurface);
    if (t1 <= t0) return vec3(0.0);
    const int N = 16;
    float seg = (t1 - t0) / float(N);
    vec3 acc = vec3(0.0);
    for (int i = 0; i < N; ++i) {
        vec3 rp = obsPos + dir * (t0 + (float(i) + 0.5) * seg);
        acc += auroraSampleAt(rp, enuX, enuY, enuZ, sunDirECEF, pc.waveTime, cloud.stormStrength);
    }
    float ext = pow(10.0, -0.4 * atmExtinctionMag(obsPos, dir, 0.0, cloud.extinctionCoeff));
    return acc * seg * kAuroraScale * cloud.auroraGain * ext;
}

// Cell of a unit direction on the star grid's cube (SatelliteSim::starEnvCell mirrors it).
uint starEnvCell(vec3 d, uint G)
{
    vec3 a = abs(d);
    uint face;
    vec2 uv;
    if (a.x >= a.y && a.x >= a.z) { face = d.x > 0.0 ? 0u : 1u; uv = d.yz / a.x; }
    else if (a.y >= a.z)         { face = d.y > 0.0 ? 2u : 3u; uv = d.xz / a.y; }
    else                         { face = d.z > 0.0 ? 4u : 5u; uv = d.xy / a.z; }
    uvec2 ij = uvec2(clamp(ivec2((uv * 0.5 + 0.5) * float(G)), ivec2(0), ivec2(int(G) - 1)));
    return (face * G + ij.y) * G + ij.x;
}

// The stars along ECEF direction dEcef, as the main view draws them (point_style.glsl: the same
// display flux D per magnitude), at a pixel angle pAng: each a Gaussian of the main view's PSF width in
// pixels of THIS render, carrying the energy it has on the main screen — so in a probe texel a faint
// star averages away as it would, and in a mirror (pixels the size of the screen's) it is the star.
// Display units (the caller divides by the viewer's exposure).
vec3 envStars(vec3 dEcef, float pAng)
{
    float th = pc.gmst; // ECEF → ECI (the sim's Earth rotation angle)
    vec3 d = vec3(cos(th) * dEcef.x - sin(th) * dEcef.y, sin(th) * dEcef.x + cos(th) * dEcef.y, dEcef.z);
    uint G = uint(starHdr1.z + 0.5);
    uint cell = starEnvCell(d, G);
    uint a = starCells[cell], b = starCells[cell + 1u];
    const float k = 1.3287712; // 0.4 · log2(10)
    float dl = exp2(-k * starHdr0.y * (starHdr0.z - starHdr0.x));
    float s0 = starHdr0.w, pRef = starHdr1.x;
    vec3 acc = vec3(0.0);
    for (uint n = a; n < b; ++n) {
        uint i = starCells[n];
        vec4 r0 = starRec[2u * i];
        vec3 e = d - r0.xyz;
        float th2 = dot(e, e); // chord² ≈ angle² at these sizes
        float D = max(exp2(-k * starHdr0.y * (r0.w - starHdr0.x)) - dl, 0.0);
        float sPx = clamp(s0 * sqrt(max(D, 1.0)), s0, starHdr1.y);
        float sig = sPx * max(pAng, 1e-6);
        if (D <= 0.0 || th2 > 9.0 * sig * sig) continue;
        // Energy D·2π(s0·pRef)² spread over this Gaussian.
        float peak = D * (s0 * pRef) * (s0 * pRef) / (sig * sig);
        acc += starRec[2u * i + 1u].rgb * peak * exp(-0.5 * th2 / (sig * sig));
    }
    return acc;
}

// A direction in this (satellite's) ENU frame, in the MAIN observer's ENU frame — the frame of the
// Milky Way and zodiacal bases (cloud.envMainObsDir).
vec3 envMainEnuDir(vec3 d, vec3 enuX, vec3 enuY, vec3 enuZ)
{
    vec3 e = d.x * enuX + d.y * enuY + d.z * enuZ;
    vec3 mX, mY, mZ;
    enuBasis(cloud.envMainObsDir.xyz, mX, mY, mZ);
    return vec3(dot(e, mX), dot(e, mY), dot(e, mZ));
}
#endif

void main() {
    // Runtime-tunable scattering strength — shadows the physical base constants (common.glsl)
    // with the user-facing "Rayleigh gain"/"Mie/haze gain" sliders, visible to every use of
    // BETA_R/BETA_M below (atmosphere loop, terrain ambient, ocean reflection, moon/sun
    // attenuation). See cloud_params.glsl's atmosRayleighGain/atmosMieGain comment.
    vec3  BETA_R = BETA_R_BASE * cloud.atmosRayleighGain;
    float BETA_M = BETA_M_BASE * cloud.atmosMieGain;

#ifdef SKY_ENV
    sunDirENU = vec4(sunDirENUIn.xyz, sunDirENUIn.z);
    float envViewExposure = max(floor(sunDirENUIn.w) * 0.01, 0.01); // the viewer's (header note)
    float envGlareVis     = fract(sunDirENUIn.w);                   // its sun-glare gate
#endif
#ifdef SKY_REFL
    // This instance's mirror-smooth pixels only (the G-buffer's w = slot + 1, pc.aspect = ours).
    uvec4 reflG = imageLoad(reflGbuf, ivec2(gl_FragCoord.xy));
    if (reflG.w == 0u || reflG.w != uint(pc.aspect + 0.5)) discard;
    vec2 reflOct = unpackUnorm2x16(reflG.x) * 2.0 - 1.0;
    vec3 reflEcef = vec3(reflOct, 1.0 - abs(reflOct.x) - abs(reflOct.y)); // octahedral decode
    if (reflEcef.z < 0.0) reflEcef.xy = (1.0 - abs(reflEcef.yx)) * vec2(reflEcef.x >= 0.0 ? 1.0 : -1.0,
                                                                      reflEcef.y >= 0.0 ? 1.0 : -1.0);
    reflEcef = normalize(reflEcef);
    vec3 reflWeight = vec3(unpackHalf2x16(reflG.y), unpackHalf2x16(reflG.z).x);
#endif

    // ENU→ECEF rotation built from observer ECEF direction (needed early for terrain UV).
    vec3 enuZ = normalize(pc.obsECEFDir.xyz); // observer Up in ECEF
    vec3 enuX = normalize(cross(vec3(0.0, 0.0, 1.0), enuZ)); // East
    vec3 enuY = cross(enuZ, enuX);            // North

#ifdef SKY_REFL
    vec3 dir    = vec3(dot(reflEcef, enuX), dot(reflEcef, enuY), dot(reflEcef, enuZ));
#else
    vec3 dir    = normalize(enuDir);
#endif
    vec3 sunDir = normalize(sunDirENU.xyz);

#if CLOUD_DEBUG == 6
    // Minimal, direct test: sample cloudNoiseTex straight from the view direction, bypassing
    // the entire raymarch/threshold/lighting pipeline entirely. If the seam still appears here,
    // it's unambiguously baked into the volume itself (or in this one-line coordinate build);
    // if it's clean, something between here and the raymarch is still the culprit.
    {
        vec3 dirECEFView = normalize(dir.x * enuX + dir.y * enuY + dir.z * enuZ);
        outColor = vec4(texture(cloudNoiseTex, dirECEFView * kCloudHorizFreq).rgb, 1.0);
        return;
    }
#endif

    // Sun direction in ECEF — used by evalCloudLayer for per-cloud-point illumination.
    // Transforms ENU sunDir into ECEF so cloud day/night is geographically correct,
    // not relative to the observer's view of the sun.
    vec3 sunDirECEF = sunDir.x * enuX + sunDir.y * enuY + sunDir.z * enuZ;

    // Elevation encoding constants (kElevRange / kMaxTerrain / kElevOffset) and the GPU-side
    // observer ground-height lookup both live in terrain.glsl now — see that file's header for
    // the DEM encoding, which is the single most re-broken piece of knowledge in this project.
#ifdef SKY_ENV
    // A probe renders from a satellite: the main observer's value below is not its eye.
    float obsEffH = observerEffHeight(earthElevTex, earthSpecTex, pc.obsECEFDir);
#else
    // The observer's ground with terrain detail — scene_depth.comp computed it once this frame,
    // unless knockout bit 1024 skipped that pass (then the buffer is stale: compute it here).
    float obsEffH = ((cloud.dbgDisableMask & 1024u) != 0u)
                  ? observerEffHeightDetailed(earthElevTex, earthSpecTex, pc.obsECEFDir)
                  : terrainFrame.x;
#endif

    // Observer position: +2 m eye height above ground.
    vec3 obsPos = observerPos(obsEffH);

    // For elevated observers the visible region extends below the geometric horizon.
    // limbZ = sin(Earth-limb depression angle) — negative, approaches 0 at sea level.
    float obsR  = length(obsPos);
    float limbZ = (obsR > R_EARTH) ? -sqrt(max(0.0, 1.0 - (R_EARTH / obsR) * (R_EARTH / obsR))) : 0.0;
    float hClip = smoothstep(limbZ - 0.02, limbZ + 0.03, dir.z);

    // ── Phase 1: terrain march (runs before atmosphere so we can truncate tEnd) ─

    vec2 tBase  = raySphere(obsPos, dir, R_EARTH);
    vec2 tShell = raySphere(obsPos, dir, R_EARTH + kMaxTerrain);

    // March for rays that could plausibly intersect terrain (up to ~44° above horizon).
    // Beyond that angle no terrain on Earth is geometrically reachable from any altitude.
    float tHit      = -1.0;
    float tSeaLvl   = (tBase.x > 0.0) ? tBase.x : -1.0;
    // Terrain v2 P2: a LAKE hit (the march met a water body's flat surface above sea level, see
    // terrain.glsl waterAdjustHeight) takes the ocean path at its own level: tSeaLvl becomes the
    // hit's distance and the wave code measures heights from waterLevelM instead of sea level.
    float waterLevelM = 0.0;
    // Is the surface pixel water? 1 / 0 once the march decided (its shoreline has the coves the
    // raw map lacks, tdShoreOffset), -1 = not known here (no march): the water map's own test.
    int   waterPx = -1;
    vec2  hitUV     = vec2(0.0);
    vec3  terrainNorm = vec3(0.0, 0.0, 1.0); // overwritten on terrain hit

    // Procedural detail state at the hit, kept for the material/AO/shadow terms and debug views.
    vec3     terrainDebugColor = vec3(0.0);
    // Set together with terrainDebugColor by the terrain debug block, tested at the end of main.
    // A separate flag because a SIGN test on the colour is not enough: view 23 (geodot) is
    // (geoSunDot, sunDot, tHit/4000), whose x component is negative on every night pixel — exactly
    // the pixels that view exists for — so the old `terrainDebugColor.x >= 0.0` gate silently kept
    // the beauty frame there. Caught 2026-09-25 by diffing the capture against t_beauty: 159 867 of
    // 160 200 sampled pixels byte-identical, the other 333 differing by <= 3/255 (PNG rounding).
    bool     terrainDebugActive = false;
    int      terrainSteps  = 0;
    TdSample terrainDet;
    terrainDet.h = 0.0; terrainDet.grad = vec3(0.0); terrainDet.rough = 0.0; terrainDet.amp = 0.0;
    terrainDet.hEro = 0.0; terrainDet.hC3 = 0.0;
    float    terrainH0     = 0.0;   // DEM height at the hit
    vec3     tmRatio       = vec3(1.0); // close-up material albedo ratio (terrain v2 P3), 1 = none
    float    tmAO          = 1.0;
    float    tmFade        = 0.0;
    vec3     terrainQ      = vec3(0.0); // hit, observer-relative (terrain_detail.glsl's q)
    vec3     terrainUpE    = vec3(0.0, 0.0, 1.0);
    float    pixAngle      = pc.fovYRad / max(cloud.skyScreenH, 1.0);
    const int kTerrainMaxSteps = 224;

    if (!dbgSkipTerrain() && dir.z < 0.7 && tShell.y > 0.0) {
        float tExit = (tBase.x > 0.0) ? tBase.x
                    : (tShell.y > 0.0  ? tShell.y : 0.0);
        // Cap scales with observer altitude so terrain is visible from LEO (250 km on the ground,
        // 3600 km from 400 km up); limb-grazing rays stay bounded.
        float tCap = mix(250000.0, 3600000.0, clamp(obsEffH / 400000.0, 0.0, 1.0));
        tExit = min(tExit, tCap);
        // S4: past terrainDistFadeEndM the relief is sub-pixel; skip the march and let tSeaLvl
        // stand in (the smooth textured Earth). The detail march's own cost no longer scales with
        // tExit — it steps on the height gap (terrain_detail.glsl) — so the old kN fade is gone and
        // only this cut-off remains.
        //
        // 2026-09-25: the march is terrain_detail.glsl's terrainMarchDetailed(), shared with
        // scene_depth.comp: DEM + procedural detail, stepped by the gap to the surface, no screen
        // jitter. It replaced a fixed quadratic 64-164-step schedule with a per-pixel jittered start,
        // whose jitter was the salt-and-pepper speckle along every silhouette.
        // Seed from the shared depth (scene_depth.comp, earlier this frame): it marched the same
        // surface at half resolution, so start just short of the nearest of the 3x3 half-res
        // texels around this pixel (conservative at silhouettes), and skip the march where none
        // of them found terrain or sea (sky). This is what makes the full-resolution march cheap:
        // it starts where the coarse surface + the bound on the fine octaves was met, usually a
        // few steps from the hit, instead of at the eye. Knockout bit 1024 fills
        // the depth with kNoSurfaceT, so the seed is ignored then (tSeed stays 2).
        float tSeed = 2.0;
        bool  seedSky = false;
#ifndef SKY_ENV
        if ((cloud.dbgDisableMask & 1024u) == 0u) {
            // The four half-res texels around this pixel (the bilinear footprint): a full-res pixel
            // sits between their ray samples, so their nearest is a conservative start. A 3x3 min
            // pulled foreground distances far across silhouettes and cost long marches there.
            ivec2 dSize = textureSize(sceneDepthTex, 0);
            vec2  dp    = gl_FragCoord.xy / vec2(cloud.skyScreenW, cloud.skyScreenH) * vec2(dSize) - 0.5;
            ivec2 d0    = ivec2(floor(dp));
            float dMin  = kNoSurfaceT;
            for (int sy = 0; sy <= 1; ++sy)
                for (int sx = 0; sx <= 1; ++sx)
                    dMin = min(dMin, texelFetch(sceneDepthTex, clamp(d0 + ivec2(sx, sy), ivec2(0), dSize - 1), 0).r);
            seedSky = dMin >= kNoSurfaceT * 0.5;
            // The depth pass marched the same surface with the same geometry LOD (its 1.2%-of-t floor
            // dominates the pixel term at any normal FOV); its larger footprint makes it stop EARLY,
            // never late. So the seed needs only float slack: at 4% the seeded march measured 3.8 ms of
            // extra sky pass at ground level, at 0.5% most pixels resolve in one or two evaluations.
            tSeed = max(2.0, dMin * 0.995 - 0.5);
        }
#endif
        if (tExit < cloud.terrainDistFadeEndM && !seedSky) {
            float hEye = obsEffH + 2.0;
            tHit = terrainMarchDetailed(earthElevTex, earthSpecTex, hEye, dir, enuX, enuY, enuZ,
                                        tSeed, tExit, pixAngle, kTerrainMaxSteps, 1.0, false,
                                        kTdCoarseOctaves, terrainSteps);
            if (tHit > 0.0) {
                terrainQ = vec3(0.0, 0.0, hEye) + tHit * dir;
                vec3 phE = terrainQ.x * enuX + terrainQ.y * enuY + (R_EARTH + terrainQ.z) * enuZ;
                hitUV = posToUV(phE);
                float hMip3;
                tdDemAt(earthElevTex, earthSpecTex, terrainQ, enuX, enuY, enuZ, terrainH0, hMip3);

                // ── The sea is not terrain ───────────────────────────────────────────────
                // Over water tdDemAt reports h0 = 0 (its sea branch) and nothing the march adds on
                // top is non-zero there (tdAmp0's smoothstep(0, 80, h0), and the erosion scales
                // with it), so the surface this hit lies on IS the sea-level sphere — the same
                // sphere tExit bounds the march by. Landing exactly on it is step-size luck: the
                // ray has to land in the sub-metre band the march accepts (rayH >= -1 in the loop
                // and then gap = rayH - 0 < 0, the regula-falsi branch) while minStep alone is
                // 0.5 m, and a satellite's near-tangential crossing has to win by a fraction of a
                // millimetre before tExit. Which rays win flips with distance, in arcs concentric
                // about the nadir: the ocean rings (harness ocean_rings_sea, `debugview normals`
                // over open water — 15% of the frame repainted at 60 m AGL, 0% at 1200 m).
                // A hit whose surface is the sea is not a terrain hit: void it so the pixel takes
                // the ocean branch below (its own mask test agrees with this h0) instead of the
                // flat, detail-free sea-level land shading this branch draws for it today.
                // h0 alone is the test: it is the march's own height function, so the two cannot
                // disagree, and land cannot read 0 here — the DEM's land baseline (16/255) is 35 m
                // above the sea baseline, and the water mask only forces a sea height below
                // kWaterMaskMaxM (tdDemAt, terrain.glsl).
                bool hitWater = hMip3 <= kTdWaterMark;   // tdDemAt's water mark
                waterPx = hitWater ? 1 : 0;
                if (hitWater && terrainH0 > 0.0) {
                    // A lake: its surface is the flat level the march hit. Water, not terrain.
                    tSeaLvl     = tHit;
                    waterLevelM = terrainH0;
                    tHit        = -1.0;
                } else if (terrainH0 <= 0.0 || hitWater) {
                    tHit = -1.0;   // sea (or land at exactly sea level), not terrain: see above
                } else {
                    // Normal: the DEM gradient over +-1 texel (bilinear central differences are
                    // continuous — the old +-0.69-texel offsets, written for a 21600-wide DEM, gave a
                    // gradient constant per texel, i.e. faceted shading), plus the detail gradient at
                    // the shading LOD (finer than the geometry's: small octaves are normal-mapped).
                    vec2  demSize = vec2(textureSize(earthElevTex, 0));
                    vec2  du = vec2(1.0 / demSize.x, 0.0), dv = vec2(0.0, 1.0 / demSize.y);
                    float hE2 = max(0.0, textureLod(earthElevTex, hitUV + du, 0.0).r * kElevRange - kElevOffset);
                    float hW2 = max(0.0, textureLod(earthElevTex, hitUV - du, 0.0).r * kElevRange - kElevOffset);
                    float hN2 = max(0.0, textureLod(earthElevTex, hitUV - dv, 0.0).r * kElevRange - kElevOffset);
                    float hS2 = max(0.0, textureLod(earthElevTex, hitUV + dv, 0.0).r * kElevRange - kElevOffset);
                    float hitLat2 = PI * 0.5 - hitUV.y * PI;
                    float texLon  = max(100.0, 2.0 * PI * R_EARTH * abs(cos(hitLat2)) / demSize.x);
                    float texLat  = PI * R_EARTH / demSize.y;
                    float dE2     = (hE2 - hW2) / (2.0 * texLon);
                    float dN2     = (hN2 - hS2) / (2.0 * texLat);
                    terrainUpE    = normalize(phE);
                    vec3 hEsE     = normalize(vec3(-terrainUpE.y, terrainUpE.x, 0.0)); // East in ECEF
                    vec3 hNrE     = cross(terrainUpE, hEsE);                            // North in ECEF
                    vec3 slopeE   = dE2 * hEsE + dN2 * hNrE;
                    if (tdEnabled()) {
                        terrainDet = terrainDetailLinEro(terrainQ, enuX, enuY, enuZ, tdShadeLodM(tHit, pixAngle),
                                                         terrainH0, hMip3);   // the march's erosion, as a plane
                        slopeE += terrainDet.grad;
                    }
                    vec3 nECEF  = normalize(terrainUpE - slopeE);
                    if (tdEnabled()) {
                        // Snow in the day map (the material block's test) smooths the micro relief.
                        vec3  dayS  = textureLod(earthDayTex, hitUV, 0.0).rgb;
                        float lumS  = dot(dayS, vec3(0.2126, 0.7152, 0.0722));
                        float mxS   = max(dayS.r, max(dayS.g, dayS.b));
                        float satS  = (mxS - min(dayS.r, min(dayS.g, dayS.b))) / max(mxS, 1e-3);
                        float snowS = smoothstep(0.40, 0.62, lumS) * (1.0 - smoothstep(0.10, 0.28, satS));
                        nECEF = normalize(nECEF - tdMicroBump(terrainQ, nECEF, enuX, enuY, enuZ,
                                                              tdShadeLodM(tHit, pixAngle), terrainDet.rough, snowS));
                    }
                    // ── Close-up materials (terrain v2 P3) ─────────────────────────────────────────
                    // Within a few metres per pixel, texture sets take over the ground's structure: the
                    // day map (5 km texels) picks WHAT the ground is — green = grass (dark green =
                    // forest floor), bright and unvegetated = sand, else dirt; steep = rock; its white =
                    // snow — and the texture's ratio to its own mean multiplies the map's colour, so the
                    // hue at a distance is unchanged. The two strongest materials are sampled, blended
                    // by their height maps (rock pokes through grass instead of a cross-fade).
                    float tmFoot = pixAngle * tHit / max(abs(dot(dir, vec3(dot(nECEF, enuX), dot(nECEF, enuY), dot(nECEF, enuZ)))), 0.25);
                    // Not inside cities (the night map's lights): their ground is the city layout.
                    vec3  nTm    = max(textureLod(earthNightTex, hitUV, 1.0).rgb - vec3(0.006, 0.006, 0.0132), vec3(0.0));
                    float cityTm = smoothstep(0.002, 0.03, dot(nTm, vec3(0.2126, 0.7152, 0.0722))) * cloud.cityLightsStrength;
                    tmFade = cloud.terrainTextureStrength * cloud.terrainMaterialStrength
                           * (1.0 - smoothstep(0.6, 3.0, pixAngle * tHit)) * (1.0 - cityTm);
                    if (tdEnabled() && tmFade > 0.001) {
                        vec3  dayT  = textureLod(earthDayTex, hitUV, 0.0).rgb;
                        float lumT  = dot(dayT, vec3(0.2126, 0.7152, 0.0722));
                        float mxT   = max(dayT.r, max(dayT.g, dayT.b));
                        float satT  = (mxT - min(dayT.r, min(dayT.g, dayT.b))) / max(mxT, 1e-3);
                        float snowT = smoothstep(0.40, 0.62, lumT) * (1.0 - smoothstep(0.10, 0.28, satT));
                        float nUpT  = dot(nECEF, terrainUpE);
                        float steep = smoothstep(0.28, 0.55, 1.0 - nUpT);
                        float veg   = smoothstep(0.0, 0.10, (dayT.g - dayT.r) / (dayT.g + dayT.r + 1e-3));
                        float dark  = 1.0 - smoothstep(0.035, 0.09, lumT);
                        float sand  = (1.0 - veg) * smoothstep(0.22, 0.40, lumT);
                        float wT[6];
                        wT[kTmGrass]  = veg * (1.0 - dark);
                        wT[kTmForest] = veg * dark;
                        wT[kTmSand]   = sand;
                        wT[kTmDirt]   = (1.0 - veg) * (1.0 - sand);
                        wT[kTmRock]   = 0.0;
                        wT[kTmSnow]   = 0.0;
                        for (int i = 0; i < 6; ++i) wT[i] *= (1.0 - steep) * (1.0 - snowT);
                        wT[kTmRock] = steep;
                        wT[kTmSnow] = snowT * (1.0 - steep);
                        {   // beaches (beachAt): the sand set
                            float dL;
                            float bw = beachAt(terrainQ, enuX, enuY, enuZ, hitUV, terrainH0, nUpT, dL);
                            wT[kTmSand] = max(wT[kTmSand], bw);
                            for (int bi = 0; bi < 6; ++bi) if (bi != kTmSand) wT[bi] *= 1.0 - bw;
                        }
                        int m0 = 0, m1 = 1;
                        if (wT[1] > wT[0]) { m0 = 1; m1 = 0; }
                        for (int i = 2; i < 6; ++i) {
                            if (wT[i] > wT[m0]) { m1 = m0; m0 = i; }
                            else if (wT[i] > wT[m1]) { m1 = i; }
                        }
                        float w0 = wT[m0], w1 = wT[m1];
                        vec3  offE = terrainQ.x * enuX + terrainQ.y * enuY + terrainQ.z * enuZ;
                        vec3  pT   = (cloud.terrainAnchorRel.xyz + offE) / kTmTileM;
                        float lodT = log2(max(tmFoot / (kTmTileM / 1024.0), 1.0));
                        vec4  a0, n0, a1, n1;
                        tmSample(m0, pT, nECEF, lodT, a0, n0);
                        tmSample(m1, pT, nECEF, lodT, a1, n1);
                        // Height blend: the material standing higher (its height + its weight) wins,
                        // over a narrow band.
                        float b1 = (w1 > 0.02) ? smoothstep(-0.12, 0.12, (a1.a + w1) - (a0.a + w0)) : 0.0;
                        vec4  aT = mix(a0, a1, b1);
                        vec4  nT = mix(n0, n1, b1);
                        // A second read of the main material at 4x the tile, multiplied in, breaks the
                        // 4-m repeat (its mean ratio is 1, so the colour is kept).
                        vec4  aF, nF;
                        tmSample(m0, pT * 0.25, nECEF, max(lodT - 2.0, 0.0), aF, nF);
                        tmRatio = aT.rgb * mix(vec3(1.0), sqrt(max(aF.rgb, vec3(0.0))), 0.6);
                        tmAO    = nT.w;
                        vec3 dn = nT.xyz - nECEF * dot(nT.xyz, nECEF);   // tangential only
                        nECEF   = normalize(nECEF + dn * (0.9 * tmFade));
                    } else {
                        tmFade = 0.0;
                    }
                    terrainNorm = normalize(vec3(dot(nECEF, enuX), dot(nECEF, enuY), dot(nECEF, enuZ)));
                }
            }
            // A ray that passed the march and met the sea sphere: water or land by the MARCH's own
            // shoreline (tdShoreOffset's coves), not the raw map's — landing on the sea level is
            // step-size luck (see above), so most sea pixels come this way. One height evaluation.
            if (tHit < 0.0 && waterPx < 0 && tSeaLvl > 0.0) {
                vec3  qs = vec3(0.0, 0.0, hEye) + tSeaLvl * dir;
                float hs0, hs3;
                tdDemAt(earthElevTex, earthSpecTex, qs, enuX, enuY, enuZ, hs0, hs3);
                waterPx = (hs3 <= kTdWaterMark) ? 1 : 0;
            }
        }
    }

    // Effective surface distance: terrain if found, else sea level
    float tSurface = (tHit > 0.0) ? tHit : tSeaLvl;

    // ── Phase 4c: a satellite mesh nearer than terrain/ocean is this pixel's surface ──────────
    // The atmosphere march below then stops at it (so a daytime satellite is washed out by the blue
    // sky in front of it, and one in orbit above the observer gets the full-column extinction), and
    // every term gated on tSurface — sun and moon discs, Milky Way, stars via depth — is hidden
    // behind it, exactly as behind terrain. Indexed through normalized screen UV so the renderScale
    // prepass (a smaller target) reads the same texel.
#ifdef SKY_ENV
    const bool meshHit = false; // the mesh targets are the main view's; a probe sees no meshes
    const float tMesh  = 0.0;
    const ivec2 meshPx = ivec2(0);
#else
    ivec2 meshTexSize = imageSize(meshDistImg);
    ivec2 meshPx  = min(ivec2(gl_FragCoord.xy / vec2(cloud.skyScreenW, cloud.skyScreenH) * vec2(meshTexSize)),
                        meshTexSize - 1);
    float tMesh   = imageLoad(meshDistImg, meshPx).r;
    bool  meshHit = tMesh > 0.0 && (tSurface <= 0.0 || tMesh < tSurface);
    if (meshHit)
        tSurface = tMesh;
#endif

    // ── Half-resolution cloud composite sample (hoisted early) ─────────────────
    // Sampled here — ahead of the moon disc below — so the moon can be occluded by opaque
    // cloud the same way it's occluded by terrain. The actual multiplicative/additive
    // composite (`color = color * cloudB.rgb + cloudA.rgb`) still applies later, after the
    // 2D flat cloud-layer overlay and satellite glow so those get attenuated too; this early
    // sample only reads the alpha channels needed for occlusion tests.
#ifdef SKY_ENV
    // No half-res cloud targets for another viewpoint: neutral composite (A adds nothing, B passes
    // everything, no opaque-cloud distance, no ground shadow). The flat layers carry the clouds.
    vec2  cloudUV        = vec2(0.0);
    vec4  cloudACenter   = vec4(0.0, 0.0, 0.0, -1.0);
    vec4  cloudBCenter   = vec4(1.0);
    vec4  cloudA         = cloudACenter;
    vec4  cloudB         = cloudBCenter;
#else
    vec2  cloudUV        = gl_FragCoord.xy / vec2(cloud.skyScreenW, cloud.skyScreenH);
    vec4  cloudACenter   = texture(cloudTargetA, cloudUV);
    vec4  cloudBCenter   = texture(cloudTargetB, cloudUV);
    // 3x3 box-blurred over cloudTargetA/B's own half-res texels — same idiom as the
    // cloudGroundShadow 5x5 blur further down. A cloud edge is a single evaluation per half-res
    // texel with no spatial supersampling, so its silhouette is genuinely stair-stepped at that
    // resolution; ordinarily hidden by the natural softness of a sky-color-to-cloud-color
    // transition, but a beam's glow riding inside cloudA.rgb (B_total, see cloud_march.comp's
    // beam pointing-ray loop) turns that same stair-step into a hard, jagged edge wherever a
    // cloud silhouette passes in front of a beam (reported in-app — the edge tracks the cloud's
    // real shape and is fixed in world space, i.e. genuine spatial aliasing of the half-res
    // alpha field, not a screen-space or depth-gate artifact). Only .rgb is blurred — .a
    // (tCloudOcclude / the shadow channel, which gets its own separate 5x5 blur below) keeps its
    // single-tap value, since occlusion tests want the exact per-pixel distance, not a blend of
    // neighbors.
    vec3  cloudARgb = vec3(0.0);
    vec3  cloudBRgb = vec3(0.0);
#ifdef SKY_LITE
    // The 3x3 rgb blur below exists only to de-jag a beam's glow riding inside cloudA.rgb where a
    // cloud silhouette crosses it — and SKY_LITE cuts beams. Single tap: −16 texture samples on
    // every pixel (this blur is not terrain-gated, so it was the largest unconditional sample cost
    // left at the Planetarium tier). SKY_OPTIMIZATION_PLAN.md Phase 1.
    cloudARgb = cloudACenter.rgb;
    cloudBRgb = cloudBCenter.rgb;
#else
    // The clouds' temporal resolve (cloud_v2_resolve.comp) already anti-aliases their silhouettes
    // (each half-res pixel accumulates jittered samples), so v1's 3x3 blur here would only soften
    // them: the bilinear tap.
    cloudARgb = cloudACenter.rgb;
    cloudBRgb = cloudBCenter.rgb;
    // ...except where a SURFACE edge crosses the half-res grid (a ridge against the sky, a cloud
    // cut by a mountain): there the bilinear tap mixes a texel marched to the terrain with one
    // marched to the sky, and the cloud's edge along the ridge came out in half-res stair-steps.
    // Joint-bilateral: each of the four texels is weighted by how well its depth (the shared
    // half-res scene depth) matches this full-resolution pixel's surface.
    {
        ivec2 hs = textureSize(cloudTargetA, 0);
        vec2  hp = cloudUV * vec2(hs) - 0.5;
        ivec2 i0 = ivec2(floor(hp));
        vec2  fr = hp - vec2(i0);
        float lF = log(max(tSurface > 0.0 ? tSurface : kNoSurfaceT, 1.0));
        ivec2 tp[4] = ivec2[4](i0, i0 + ivec2(1, 0), i0 + ivec2(0, 1), i0 + ivec2(1, 1));
        float lD[4];
        float dMin = 1e9, dMax = -1e9;
        for (int k = 0; k < 4; ++k) {
            tp[k] = clamp(tp[k], ivec2(0), hs - 1);
            lD[k] = log(max(texelFetch(sceneDepthTex, tp[k], 0).r, 1.0));
            dMin = min(dMin, lD[k]); dMax = max(dMax, lD[k]);
        }
        if (dMax - dMin > 0.1) {
            float bw[4] = float[4]((1.0 - fr.x) * (1.0 - fr.y), fr.x * (1.0 - fr.y),
                                   (1.0 - fr.x) * fr.y, fr.x * fr.y);
            vec3  sa = vec3(0.0), sb = vec3(0.0);
            float sw = 0.0;
            for (int k = 0; k < 4; ++k) {
                float w = bw[k] * exp(-abs(lD[k] - lF) * 6.0) + 1e-5;
                sa += w * texelFetch(cloudTargetA, tp[k], 0).rgb;
                sb += w * texelFetch(cloudTargetB, tp[k], 0).rgb;
                sw += w;
            }
            cloudARgb = sa / sw;
            cloudBRgb = sb / sw;
        }
    }
#endif
    vec4  cloudA         = vec4(cloudARgb, cloudACenter.a);
    vec4  cloudB         = vec4(cloudBRgb, cloudBCenter.a);
#endif
    // Target A's alpha is a signed distance in km (include/cloud_occlusion.glsl): >= 0 opaque.
    float tCloudOcclude  = (cloudA.a >= 0.0) ? cloudA.a * 1000.0 : -1.0;
    // cloudB.a used to carry tEnterCombined, the fused entry distance this shader compared
    // against tSurface to suppress the whole composite. Every volumetric layer is now clamped to
    // the shared scene depth inside cloud_march.comp instead, so there is nothing to test here.
    // The channel is free; the next step gives it the per-pixel cloud shadow.
    //
    // Local cloud opacity at THIS pixel's view ray (0 = clear sky/no cloud, 1 = fully opaque),
    // same formula `cloudBlock` derives from cloudB.rgb further below — computed here too so the
    // night-lights blur-through-cloud blend (city detail block below) can use it before that
    // later point. A soft edge, not a hard cutoff on purpose: even a thin/wispy cloud should
    // diffuse city light a little, not switch abruptly from sharp to blurred.
    float localCloudOpacity = 1.0 - clamp(dot(cloudB.rgb, vec3(1.0 / 3.0)), 0.0, 1.0);

    // ── Phase 2: atmosphere integration, truncated at the surface ─────────────
    vec2  tAtmos = raySphere(obsPos, dir, R_ATMOS);
    // Clamped to 0: when the observer is above R_ATMOS (reachable via the uncapped "Raise
    // Elevation" control) and looking outward/away from Earth, raySphere's forward root
    // (tAtmos.y) goes negative — the 100km shell is now entirely behind the camera. Without
    // this clamp, segLen would go negative and the whole loop below would march backward from
    // the observer instead of contributing nothing, corrupting the sky colour along that ray.
    float tEnd   = max(0.0, (tSurface > 0.0) ? min(tAtmos.y, tSurface) : tAtmos.y);
    // The march starts where the ray ENTERS the atmosphere (0 from inside it). It started at the eye:
    // from orbit nearly every step fell in the vacuum above R_ATMOS (zero density, but each one still
    // paid the city-glow fetch, the trig and the airglow noise), and the few left in the air
    // undersampled it. From 2000 km this loop was 13 of the sky pass's 16.6 ms (harness_runs/impostor).
    float tStart = clamp(tAtmos.x, 0.0, tEnd);

    // Adaptive N_VIEW (perf follow-up, session 24 round 2): a FIXED sample count over segLen =
    // tEnd/N_VIEW badly serves this loop, because tEnd itself varies enormously with viewing
    // geometry, not just altitude — a straight-up ray from the ground has tEnd~100km, but a
    // near-horizon ray (the same geometry as looking toward a low sun near the terminator) can
    // graze a chord of 2000+ km through the thin atmosphere shell, and most rays from orbit hit
    // the shell at a similarly grazing angle. A fixed low N_VIEW gives a razor-thin, accurate
    // step for the short case but a step many times the ~8km Rayleigh scale height (H_R) for the
    // long case — under-resolving the exponential falloff exactly where it's most sensitive,
    // which is what read as rainbow banding near the terminator and a vanishing atmosphere past
    // MEO. This is undersampling, not a floating-point precision problem (precision loss
    // wouldn't recover just by raising the sample count the way this does).
    //
    // Fix: derive a target step length from the user's validated "looks convincing" ground-level
    // case (cloud.viewSamplesMin steps over the reference ~100km straight-up path — kAtmosRefTEnd
    // below is exactly R_ATMOS-R_EARTH), then scale N_VIEW to hold that SAME step length for
    // whatever tEnd this particular ray actually has, clamped to cloud.viewSamplesMax so a
    // pathologically long grazing ray can't balloon the cost unboundedly.
    const float kAtmosRefTEnd = 100000.0; // R_ATMOS - R_EARTH: ground-level straight-up reference path
    float viewSamplesMin = max(2.0, cloud.viewSamplesMin);
    float viewSamplesMax = max(viewSamplesMin, cloud.viewSamplesMax);
    float targetStepLen  = kAtmosRefTEnd / viewSamplesMin;
    int   N_VIEW = dbgSkipAtmosphere() ? 0
                 : int(clamp(ceil((tEnd - tStart) / targetStepLen), viewSamplesMin, viewSamplesMax));
    float segLen = (tEnd - tStart) / float(N_VIEW);
    float cosA   = dot(dir, sunDir);
    float pR     = phaseR(cosA);
    float pM     = phaseM(cosA);

    vec3  accumR  = vec3(0.0);
    float accumM  = 0.0;
    float accumCity = 0.0;
    vec3  accumAirglow = vec3(0.0); // green + sodium bands (C15) — ride these same samples
    float odR_cam = 0.0;
    float odM_cam = 0.0;

    // ── Orbital terminator gate (artistic) ───────────────────────────────────
    // Strength of the per-sample terminator suppression applied to accumR/accumM below, faded in
    // by observer altitude so ground level is bit-identical to having no gate at all. Reuses the
    // same 40-100 km fade constants as the Milky Way block further down; below 40 km this is 0 and
    // the whole feature compiles out to a multiply by 1.0.
    //
    // Why altitude-gated rather than global: the gate suppresses samples sitting over night-side
    // ground, which from orbit is exactly the unwanted twilight wash, but from the GROUND is the
    // post-sunset sky itself. Measured at sun 2 degrees below the horizon, applying it globally
    // took the western afterglow down 4-8x and removed the Belt of Venus entirely. It is an
    // orbital art knob, not a scattering correction, and it is scoped to say so.
    float atmTermSpace = clamp((obsEffH - 40000.0) / 60000.0, 0.0, 1.0)
                       * clamp(cloud.atmosTermStrength, 0.0, 1.0);
    float atmTermW     = max(1e-4, cloud.atmosTermWidth);

    // ── Single-scattering atmosphere integration (Rayleigh + Mie) ────────────
    // N_VIEW uniform steps from the observer toward the atmosphere exit (or truncated at
    // the surface).  At each step the scattered sunlight is accumulated using:
    //   - Two running totals (odR_cam, odM_cam): optical depth from the CAMERA to this step.
    //     These are also read back after the loop to attenuate the surface/moon/cloud colours.
    //   - Per-step sun optical depth (sunOD): optical depth from THIS STEP to the SUN,
    //     computed by calling optDepth along the sun direction.
    //   - Phase functions pR/pM: angular weighting of how much scatter points toward the camera.
    for (int i = 0; i < N_VIEW; ++i) {
        vec3  sp  = obsPos + dir * (tStart + (float(i) + 0.5) * segLen);  // midpoint of this atmosphere step
        float len = length(sp);
        if (len < R_EARTH) sp *= R_EARTH / len;  // clamp underground samples to Earth surface
        float h = max(0.0, length(sp) - R_EARTH);  // altitude above sea level (metres)

        // Running camera-side optical depth: accumulated from step 0 to this step.
        // densR/densM = density × step length = optical depth contribution of this step alone.
        float densR = exp(-h / H_R) * segLen;   // Rayleigh: peaks at sea level, scale height H_R
        float densM = exp(-h / H_M) * segLen;   // Mie: concentrated near surface, scale height H_M
        odR_cam += densR;
        odM_cam += densM;

        // sin(sun elevation) at THIS SAMPLE's own geographic point — the same quantity the cloud
        // march gates on (cloudSunDotRaw), which is the whole point: it is what lets the
        // atmosphere cut off on the same variable the clouds already do. Set inside the block
        // below (which computes spDirECEF for city glow / airglow anyway) and consumed at the
        // accumulation, so it costs one dot product that was already being taken.
        float sampleSunDotGeo = 1.0;

        // City light-pollution upwelling (Step 7 / C10, TERRAIN_PLAN.md). An INDEPENDENT light
        // source, not derived from sunlight, so it's computed here — BEFORE the sun-shadow test
        // below, which specifically triggers when this sample is in Earth's shadow (i.e. at
        // night, exactly when city glow matters). Uses camera-side attenuation only (no
        // sun-side optical depth term — irrelevant for a non-solar light source). densR weights
        // near-surface atmosphere heavily, so an observer directly over a city gets strong
        // zenith glow while a distant observer only picks up dim glow from low horizon samples
        // — both fall out naturally from the same accumulation used for Rayleigh/Mie above.
        //
        // SKY_LITE (Planetarium-tier): the green/sodium airglow bands (two warpPerlin3-based
        // coverage masks per step) are cut entirely — that was the bulk of the cost and this tier
        // has no nightglow. City-glow upwelling is KEPT (the sim reads hollow at night without it)
        // but only on the near steps: densR = exp(-h/8km) makes everything past ~3 steps a rounding
        // error, so the per-step earthNightTex fetch + asin/atan is paid ~3× instead of ~10×.
        // sampleSunDotGeo (the orbital terminator gate) needs every step, split out and cheapened:
        // dot(normalize(sp), sunDir) is frame-invariant, skipping the ECEF matrix transform.
#ifdef SKY_LITE
        sampleSunDotGeo = dot(normalize(sp), sunDir);
        if (i < 3) {
            vec3  spDirECEF = normalize(sp.x * enuX + sp.y * enuY + sp.z * enuZ);
            float spLat     = asin(clamp(spDirECEF.z, -1.0, 1.0));
            float spLon     = atan(spDirECEF.y, spDirECEF.x);
            vec2  spUV      = vec2((spLon + PI) / (2.0 * PI), (0.5 * PI - spLat) / PI);
            float spLum     = dot(textureLod(earthNightTex, spUV, 4.0).rgb, vec3(0.2126, 0.7152, 0.0722));
            vec3  attnCam   = exp(-(BETA_R * odR_cam + BETA_M * 1.1 * odM_cam));
            accumCity += cityBrightness(spLum) * densR * dot(attnCam, vec3(1.0 / 3.0));
        }
#else
        {
            vec3  spECEF    = sp.x * enuX + sp.y * enuY + sp.z * enuZ;
            float spLen     = length(spECEF);
            vec3  spDirECEF = spECEF / spLen;
            float spLat     = asin(clamp(spDirECEF.z, -1.0, 1.0));
            float spLon     = atan(spDirECEF.y, spDirECEF.x);
            vec2  spUV      = vec2((spLon + PI) / (2.0*PI), (0.5*PI - spLat) / PI);
            float spLum     = dot(textureLod(earthNightTex, spUV, 4.0).rgb, vec3(0.2126, 0.7152, 0.0722));
            vec3  attnCam   = exp(-(BETA_R * odR_cam + BETA_M * 1.1 * odM_cam));
            accumCity += cityBrightness(spLum) * densR * dot(attnCam, vec3(1.0 / 3.0));

            // Airglow (C15): green (96km) + sodium (90km) bands both fall inside this loop's
            // own altitude range (h spans 0..~100km along an open-sky ray), so they ride these
            // existing samples for free — no dedicated march needed (unlike red, see below the
            // loop). Gated by the SAMPLE's own geographic day/night (not the observer's), same
            // dot-product test cloud lighting uses (evalCloudLayer/cloudMarch sampleDayness) —
            // physically correct since the glow originates at that geographic point, not at the
            // observer. Horizontal patchiness from a slow analytic domain warp avoids a flat,
            // featureless ring (a pure function of altitude alone has none).
            sampleSunDotGeo  = dot(spDirECEF, sunDirECEF);
            float airDayness = clamp((sampleSunDotGeo + 0.15) / 0.3, 0.0, 1.0);
            float airNight   = 1.0 - airDayness;
            if (airNight > 0.001) {
                float airPatch = 0.6 + 0.4 * warpPerlin3(spDirECEF * kAirglowNoiseFreq
                                    + vec3(pc.waveTime * kAirglowDriftRate, 17.0, -5.0));
                // Coverage patchiness — independent noise samples per band so green and sodium
                // don't brighten/dim in perfect lockstep (real thermospheric density variation
                // doesn't affect both emission layers identically).
                float coverageG = airglowCoverageMask(spDirECEF, pc.waveTime, vec3(3.0, 29.0, -11.0));
                float coverageS = airglowCoverageMask(spDirECEF, pc.waveTime, vec3(-37.0, 6.0, 44.0));
                float covGainC  = clamp(cloud.airglowCoverageGain, 0.0, 1.0);
                float dzG = (h - kAirglowGreenPeakM) / kAirglowGreenHalfWidthM;
                float dzS = (h - kAirglowSodiumPeakM) / kAirglowSodiumHalfWidthM;
                float densAirG = exp(-dzG * dzG) * segLen;
                float densAirS = exp(-dzS * dzS) * segLen;
                accumAirglow += (kAirglowGreenColor  * cloud.airglowGreenGain  * densAirG * mix(1.0, coverageG, covGainC)
                                + kAirglowSodiumColor * cloud.airglowSodiumGain * densAirS * mix(1.0, coverageS, covGainC))
                                * airNight * airPatch;
            }
        }
#endif

        // Shadow test: skip samples in Earth's shadow.
        // If the sun-ray from this point has TWO positive intersections with R_EARTH, the sun
        // is behind Earth from here → no direct sunlight → no in-scatter contribution.
        vec2 tSunEarth = raySphere(sp, sunDir, R_EARTH);
        if (tSunEarth.x > 0.0 && tSunEarth.y > 0.0) continue;

        // Compute sun-side optical depth from this sample to the atmosphere boundary.
        vec2 tSun  = raySphere(sp, sunDir, R_ATMOS);
        vec2 sunOD = (tSun.y > 0.0) ? optDepth(sp, sunDir, tSun.y) : vec2(0.0);

        // Combined transmittance τ = BETA × (cam_depth + sun_depth):
        //   cam_depth: how much atmosphere light must traverse from here to the camera.
        //   sun_depth: how much atmosphere sunlight must traverse from the sun to here.
        // Mie multiplied by 1.1 to account for aerosol absorption (σ_ext > σ_scat).
        vec3 tau  = BETA_R       * (odR_cam + sunOD.x)
                  + BETA_M * 1.1 * (odM_cam + sunOD.y);
        vec3 attn = exp(-tau);  // total transmittance: sun → this sample → camera

        // Orbital terminator gate. Deliberately NOT physical: the scattering integral itself was
        // measured (against a clean-room reimplementation of this exact loop) to fall about one
        // decade per 6 degrees across the terminator, which is real twilight's rate. The problem
        // it solves is a tone-mapping mismatch, not a scattering error — this renderer composites
        // an eyeballed HDR scene rather than shooting at a fixed exposure the way the orbital
        // photography it is being compared against does, so the physically-correct twilight tail
        // survives tone mapping far more visibly than a camera would record it. That leaves the
        // clouds (which cut off hard on their own geographic sun angle) reading as oddly dark
        // patches against a still-bright atmosphere. Suppressing the atmosphere is the correct
        // direction to close that gap; raising the clouds' twilight ambient to meet the
        // atmosphere, which was tried first, drives them to full-bright neon seen from the ground.
        //
        // Weighting by the SAMPLE's own geographic sun elevation is what makes this free of
        // daylight cost: samples over daylit ground never enter the rolloff at all, so the day
        // side is untouched to six decimal places while SZA 92 drops ~23x at width 0.08.
        float atmTermW8 = mix(1.0, smoothstep(-atmTermW, atmTermW, sampleSunDotGeo), atmTermSpace);

        // Accumulate in-scattered radiance for each particle type.
        // Multiplying by density (densR/densM) weights by how many particles are at this altitude.
        // accumCity and accumAirglow above are deliberately NOT gated — they are independent
        // emissive sources, not scattered sunlight, and suppressing them past the terminator is
        // the exact opposite of what they exist to do.
        accumR += attn * densR * atmTermW8;                  // Rayleigh: wavelength-dependent (blue sky)
        accumM += dot(attn, vec3(1.0 / 3.0)) * densM * atmTermW8; // Mie: wavelength-neutral (white haze/corona)
    }

    vec3 color = SUN_INTENSITY * (pR * BETA_R * accumR + vec3(pM * BETA_M * accumM));

    // City light-pollution glow dome, composited once here (see accumCity comment in the loop
    // above). nightFactor fades it out through the day — cheap local gate rather than reusing
    // any later-computed day/night variable, since none exists yet at this point in main().
    float nightFactor = 1.0 - smoothstep(-0.05, 0.1, sunDirENU.w);
    color += accumCity * vec3(1.0, 0.72, 0.42) * nightFactor * kNightGlowScale;

    // C12 follow-up #41: replaced the directional (azimuth-sector-dome-based) wash from #39/#40
    // with a simple non-directional "sky is brighter near an active beam" term. The directional
    // version read as a narrow rising pillar from one bearing (right for a city's broad glow dome,
    // wrong for one concentrated, often low-angle beam) and measured distance to the beam's GROUND
    // TARGET only, incorrectly fading out as the observer climbed up alongside a beam away from the
    // ground while staying right next to its actual line. cloud.beamProximityGlow (CPU-computed in
    // SatelliteSim.cpp from true point-to-segment distance to the nearest active beam's line,
    // one-frame-stale like every other reflectBeamsBuf readback) already carries the complete
    // [0,1] falloff — applied equally regardless of view direction, so standing near a beam
    // brightens the WHOLE visible sky, not one patch of it. beamGlowDomeBuf itself is untouched —
    // still used below (Milky Way section) and in sat_flare.comp/updateStars() for its original
    // purpose, suppressing other sky objects near an active beam.
    const float kBeamSkyGlowScale = 1.0; // C12 follow-up #40 — see that follow-up's note on why
                                          // this needed to be O(1), not O(1e-6): it multiplies an
                                          // already-[0,1]-normalized value, unlike kNightGlowScale.
    // Reuses the SAME nightFactor just computed for city glow above — an accepted first-pass
    // simplification (an active beam's target is always night-side by construction, but this
    // doesn't separately check the OBSERVER's own day/night).
#ifndef SKY_ENV
    color += vec3(1.0, 0.95, 0.9) * cloud.beamProximityGlow * cloud.beamGlowBleedGain * kBeamSkyGlowScale
           * nightFactor;
#endif

    // ── Airglow (C15) ─────────────────────────────────────────────────────────
    // Green + sodium bands (accumAirglow) rode the N_VIEW loop above for free.
    color += accumAirglow * kAirglowScale * cloud.airglowGain;

    // Red band (630nm) supplemental march: peaks at 275km, well past N_VIEW's ~100km ceiling
    // (R_ATMOS), so it never rode those samples the way green/sodium do above. Perf (this
    // session): the dedicated march itself moved to cloud_march.comp (airglowRedMarchCS),
    // alongside aurora — half resolution instead of full-res. Rides along inside cloudA.rgb
    // (B_total) the same way aurora does now; no separate handling needed here. See that
    // function for the full march logic and the entry/exit classification history.

    // ── Aurora (C16, TERRAIN_PLAN.md Phase E) ──────────────────────────────────
    // Perf (this session): the sky curtain march itself moved to cloud_march.comp, alongside
    // clouds/cirrus, so it runs at half resolution instead of full-res (one of the two big
    // remaining levers from "why is aurora so much more expensive than clouds" — the other,
    // baking its noise, is done too — see aurora_noise.comp). Its result now arrives already
    // folded into cloudTargetA's B_total (additive radiance), composited below alongside
    // cirrus/cloud with no separate handling needed here. auroraFrame/auroraCoverage/
    // auroraOvalMask/auroraCurtainNoise/auroraSampleAt (above) and auroraGlowAt (below) STAY here
    // — still used by terrain/ocean ambient lighting and the ocean sky-reflection's aurora sample,
    // both full-resolution. dbgSkipAurora() now lives in CloudMarchPC (mirrors debugDisableMask)
    // since that's where the actual march runs; see cloud_march.comp's auroraMarchCS.

    // ── Moon disc ─────────────────────────────────────────────────────────────
    // kMoonTexRotDeg: rotates the texture CW in the UV plane to align the image's
    // north pole with the physical lunar north pole as seen from the observer.
    // Tune this until the terminator's shadow boundary matches the image poles.
    const float kMoonTexRotDeg = 180.0;
    const float kMoonAngR      = 0.004578 * 3.0;
    const float kMoonBright    = 0.54;
    // Set below on an actual ray-disc hit; used later to block the Milky Way skybox (and
    // nothing else — stars are culled per-vertex in star_point.vert) from showing through the
    // Moon's opaque disc on a clear-sky ray. Terrain/cloud occluding the Moon itself already
    // separately block the Milky Way via their own existing visibility terms, so this only
    // needs to be the pure geometric hit, not discFade.
    bool moonDiscHit = false;
    if (moonDirENU.z > limbZ - kMoonAngR * 2.0) {
        vec3  moonDir3 = normalize(moonDirENU.xyz);

        // (An atmospheric-refraction "squish" — compressing the disc vertically near the horizon
        // via a Bennett-formula differential-refraction term — used to live here. It was disabled
        // (`squish = 0`) but still ran two tan / a radians / an asin every moon-region pixel to
        // compute a value that fed the identity `dir.z * (1.0 + 0.0)`. Removed, Phase 0 —
        // SKY_OPTIMIZATION_PLAN.md. Re-add from git history if the effect is ever wanted back.)
        vec3  oc    = -moonDir3;
        float bm    = dot(oc, dir);
        float cm    = 1.0 - kMoonAngR * kMoonAngR;
        float discm = bm * bm - cm;
        float tm    = -bm - sqrt(max(discm, 0.0));
        if (discm >= 0.0 && tm > 0.0) {
            moonDiscHit = true;
            vec3  hp = tm * dir;
            vec3  n  = normalize(hp - moonDir3);
            float diffuse  = max(0.0, dot(n, sunDir)) * moonDirENU.w;
            float mu       = max(0.0, dot(n, -moonDir3));
            float limbDark = 0.35 + 0.65 * sqrt(mu);
            // Earthshine inversely follows moon phase: new moon (full Earth) = maximum.
            float earthshine = 0.0008 * mu * (1.0 - moonDirENU.w);

            // Build the moon's local face frame: moonZ points toward the observer
            // (tidally locked near side), moonX/moonY span the visible face plane.
            // refUp = celestial north pole in ENU: converts ECEF (0,0,1) to observer ENU
            // by dotting with the ENU basis vectors (enuX/Y/Z are in ECEF-space).
            // This correctly rotates the texture with parallactic angle as the observer
            // moves across Earth, instead of always aligning north with local zenith.
            vec3 moonZ = -moonDir3;
            vec3 northCelENU = vec3(enuX.z, enuY.z, enuZ.z);
            vec3 refUp = (abs(dot(northCelENU, moonZ)) < 0.99) ? northCelENU : vec3(1.0, 0.0, 0.0);
            vec3 moonX = normalize(cross(refUp, moonZ));
            vec3 moonY = cross(moonZ, moonX);

            // Orthographic projection of the surface normal onto the face plane.
            // At the disc centre n == moonZ → UV (0.5, 0.5); at the limb UV spans [0,1].
            vec2 moonUV = vec2(dot(n, moonX), dot(n, moonY)) * 0.5 + 0.5;

            // Rotate UV around disc centre by kMoonTexRotDeg to align image north pole
            // with the physical lunar north pole. Positive = CCW rotation of the texture.
            float rotRad = radians(kMoonTexRotDeg);
            float cosR = cos(rotRad), sinR = sin(rotRad);
            vec2  uvc  = moonUV - 0.5;
            moonUV = vec2(cosR * uvc.x - sinR * uvc.y,
                          sinR * uvc.x + cosR * uvc.y) + 0.5;

            vec3 texColor = texture(moonTex, moonUV).rgb;

            // Occluded by terrain OR by opaque cloud (tCloudOcclude, ≥90% opaque along this ray —
            // same threshold satellite/star depth occlusion uses below). Without the cloud term
            // the moon's raw disc brightness survived the later multiplicative cloud attenuation
            // visibly intact even under a thick deck — the composite dims it but doesn't blank
            // the fine albedo detail the way a genuinely opaque cloud should.
            float discFade = (tSurface > 0.0 || tCloudOcclude >= 0.0) ? 0.0 : 1.0;
            vec3 moonColor = texColor * (diffuse + earthshine) * limbDark * kMoonBright;
            vec3 moonAttn  = exp(-(BETA_R * odR_cam + BETA_M * 1.1 * odM_cam));
            color += discFade * moonColor * moonAttn;
        }
    }

    // ── Satellite constellation sky glow (pre-tonemap) ────────────────────────
    // Wide Gaussian (kSig = 0.90 rad ~= 51 deg) over 64 sky bins.
    // Each occupied bin represents the brightest satellite in that 45°×11.25° cell.
    // Runs pre-tonemap so the exposure system scales it: invisible at noon,
    // visible at dusk, prominent at night.
    // Knockout bit 65536 (2026-08-10): 64 iterations with an acos() each, on EVERY full-res pixel,
    // unconditionally — the only fixed-cost loop in this shader with no knockout and no quality
    // slider behind it, so its share of the "sky background draw" bucket was previously
    // unmeasurable. Skipping just leaves `color` without the glow term, which is exactly what an
    // all-empty glowBuf already produces.
#if !defined(SKY_LITE) && !defined(SKY_ENV)   // Planetarium-tier / env: drop the 64-iteration satellite sky-glow loop
    if ((cloud.dbgDisableMask & 65536u) == 0u) {
        const float TWO_PI = 6.28318530718;
        vec3  flareAttn = exp(-(BETA_R * odR_cam + BETA_M * 1.1 * odM_cam));
        const float kSig = 0.90;
        for (int gi = 0; gi < 64; ++gi) {
            uint fluxBits = glowBuf.bins[gi];
            if (fluxBits == 0u) continue;
            float flux   = uintBitsToFloat(fluxBits);
            // Derive bin-centre ENU direction from bin index.
            // azBin=0 is North, increasing toward East (matches atan(x,y) convention).
            float az     = (float(gi / 8) + 0.5) * (TWO_PI / 8.0);
            float elSin  = (float(gi % 8) + 0.5) / 8.0; // z = sin(elevation)
            float elCos  = sqrt(max(0.0, 1.0 - elSin * elSin));
            vec3  fd     = vec3(sin(az) * elCos, cos(az) * elCos, elSin);
            if (fd.z < limbZ - 0.05) continue;
            float angle  = acos(clamp(dot(dir, fd), -1.0, 1.0));
            float glow   = exp(-angle * angle / (2.0 * kSig * kSig)) * 0.01;
            float gElev  = smoothstep(-0.08, 0.02, fd.z);
            float intens = clamp(log2(max(flux, 1.0)) / 4.0, 0.0, 1.5);
            float atmosW = 1.0 - exp(-odR_cam / 5000.0);
            color += hClip * gElev * glow * intens * 0.06 * vec3(1.0, 0.96, 0.88) * flareAttn * atmosW;
        }
    }
#endif

    // ── Phase 3: ground / terrain composite ──────────────────────────────────
    // The atmosphere was truncated at tSurface, so odR_cam/odM_cam represent
    // optical depth from the observer to the surface. Transmittance = e^(-tau).
    // We ADD attenuated surface colour to the atmosphere scatter already in `color`.
    if (meshHit) {
        // Phase 4c: the mesh's own radiance, lit in sat_mesh.frag, through the air in front of it.
        vec3 meshAttn = exp(-(BETA_R * odR_cam + BETA_M * 1.1 * odM_cam));
        color += imageLoad(meshColorImg, meshPx).rgb * meshAttn;
    } else if (tSurface > 0.0) {
        vec3 surfAttn = exp(-(BETA_R * odR_cam + BETA_M * 1.1 * odM_cam));

        vec2 uvSurf;
        vec3 hitPt;
        if (tHit > 0.0) {
            uvSurf = hitUV;
            hitPt  = obsPos + tHit * dir;
        } else {
            hitPt        = obsPos + tSeaLvl * dir;
            vec3  hE     = hitPt.x * enuX + hitPt.y * enuY + hitPt.z * enuZ;
            float geoLat = asin(clamp(hE.z / R_EARTH, -1.0, 1.0));
            float geoLon = atan(hE.y, hE.x);
            uvSurf = vec2((geoLon + PI) / (2.0*PI), (0.5*PI - geoLat) / PI);
        }

        vec3  shadingN   = (tHit > 0.0) ? terrainNorm : normalize(hitPt);
        float sunDot     = dot(shadingN, sunDir);
        // Geographic horizon gate: dot(normalize(hitPt), sunDir) is the sun's elevation above
        // the local horizon at the TERRAIN POINT's geographic location.  The slope normal
        // (shadingN) cannot be used for this — a steep slope can face toward the sun even
        // when the terrain point is on the night side of Earth.  Gate dayFrac by the radial
        // direction so no illumination leaks past the terminator regardless of slope angle.
        // Margin [-0.03, 0.02]: thin alpenglow zone for mountain peaks that see the sun just
        // past the flat horizon; anything below -1.7° geographic is forced to zero.
        float geoSunDot   = dot(normalize(hitPt), sunDir);
        float horizonGate = smoothstep(-0.03, 0.02, geoSunDot);
        // Day vs night is the GEOGRAPHIC gate alone. It was also gated by the shading normal
        // (smoothstep(-0.15, -0.12, sunDot)), which sent any face turned more than ~8 degrees past
        // edge-on to the Sun to the NIGHT branch — no skylight at all, in daylight. DEM normals
        // rarely got there; the terrain-detail normals do all the time, and every such facet
        // rendered pure black (a harness flight into a glacier). Direct sun has its own Lambert
        // term (sunLit below); a face turned away still sees the sky.
        float dayFrac     = horizonGate;
        // directSun combines day/night blend for all sun-driven contributions.
        float directSun   = dayFrac;
        // ── Surface twilight gate ──────────────────────────────────────────────────────────────
        // Civil twilight runs until the Sun is 6 degrees below the horizon (sin = -0.105): the
        // ground is still visibly lit by the sky the whole way down, and the sky is at its most
        // orange while the Sun is only a degree or two under. dayFrac above is the DIRECT-SUN
        // gate — it spans 1.5 degrees of Sun altitude and is the right gate for the Sun's disc,
        // but it was also used to blend the whole surface colour, which handed the city-lights
        // map 83% of every ground pixel at Sun -1 degree. The near ground (neutral snow albedo,
        // this terrain) then came out dead grey — 63,63,63 with r=g=b — at the exact instant the
        // sky was at its brightest, and its light was neutral while the peaks kept only a dimmed
        // red from the 17% left over (2026-09-25 twilight-terrain harness captures). Albedo is
        // dimmed by light, not replaced by it: one gate per job.
        const float kCivilTwilightSin = -0.105; // sin of -6 degrees of geographic sun altitude
        float twilightFrac = smoothstep(kCivilTwilightSin, 0.02, geoSunDot);
        // Cloud shadow: cloud_march.comp already marched sunward from this pixel's own terrain
        // hit point and stored the transmittance in cloudB.a (sampled earlier, alongside the rest
        // of the composite).
        //
        // This replaced a 128x128 observer-centred tangent-plane grid, which needed all three of
        // those things plus a texel-snapping residual to stop shadows swimming as the observer
        // moved, and which silently stopped shadowing anything past cloudShadowRangeM. The
        // replacement has no range limit and nothing to snap, because the value is a function of
        // the world point being shaded rather than of where the camera happens to be.
        //
        // Box-blurred over cloudTargetB's own half-res texels rather than a single tap.
        // cloudGroundShadow (cloud_march.comp) is a 12-step raymarch dithered by one noise-texture
        // lookup per half-res pixel, with no temporal accumulation to average it away (single frame
        // in flight) — so the raw value reads as the noise texture itself stamped onto the ground,
        // worst on the ocean where there's no other high-frequency detail to hide it in. A small
        // spatial blur here is the same fix the light-pollution dome already uses.
        //
        // Phase 0 (SKY_OPTIMIZATION_PLAN.md): was a 5x5 (25 texture taps on every ground pixel — the
        // single heaviest sample count in this shader on GCN1). Now a 3x3 at kShadowBlurSpread texel
        // spacing so the FOOTPRINT still ~matches radius-2 (was 2.0, now 1.7) for 9 taps. If ocean
        // graininess returns, strengthen cloudGroundShadow's dither in cloud_march.comp rather than
        // widening this back out.
        const float kShadowBlurSpread = 1.7;
        float cloudShadowT = cloudBCenter.a;   // centre tap — already sampled above, don't re-fetch
#ifndef SKY_ENV
        {
            vec2 shadowTexel = kShadowBlurSpread / vec2(textureSize(cloudTargetB, 0));
            for (int sy = -1; sy <= 1; ++sy)
                for (int sx = -1; sx <= 1; ++sx)
                    if ((sx | sy) != 0)
                        cloudShadowT += texture(cloudTargetB, cloudUV + vec2(sx, sy) * shadowTexel).a;
            cloudShadowT *= (1.0 / 9.0);
        }
#endif
        directSun *= cloudShadowT;
        // Antimeridian seam fix: longitude wraps at ±PI so dFdx(uvSurf.x) jumps by ~1.0
        // across that boundary. The GPU would pick the highest mip level, blurring a
        // vertical strip. Clamp the derivative to the small expected value instead.
        vec2 uvd_dx = dFdx(uvSurf);
        vec2 uvd_dy = dFdy(uvSurf);
        if (uvd_dx.x >  0.5) uvd_dx.x -= 1.0;
        if (uvd_dx.x < -0.5) uvd_dx.x += 1.0;
        if (uvd_dy.x >  0.5) uvd_dy.x -= 1.0;
        if (uvd_dy.x < -0.5) uvd_dy.x += 1.0;
        vec3 dayColor   = textureGrad(earthDayTex,   uvSurf, uvd_dx, uvd_dy).rgb;
        vec3 nightColor = textureGrad(earthNightTex, uvSurf, uvd_dx, uvd_dy).rgb;
        // Blur city lights toward a coarser mip under cloud (see localCloudOpacity above and
        // cloud.cityLightBlurLod) — real light passing through cloud droplets is diffused, not a
        // clean pass-through of whatever's behind it, so a sharp copy of earthNightTex's city
        // silhouette bleeding through a hazy/thin cloud reads as an artifact even when the
        // OPACITY itself is physically reasonable. Skipped below the horizon (dayFrac path has no
        // night lights anyway) is unnecessary — nightColor is cheap and already computed for the
        // dayFrac mix regardless of whether it's actually night at this point.
        if (cloud.cityLightBlurLod > 0.01 && localCloudOpacity > 0.001) {
            vec3 nightColorBlur = textureLod(earthNightTex, uvSurf, cloud.cityLightBlurLod).rgb;
            nightColor = mix(nightColor, nightColorBlur, localCloudOpacity);
        }

        // ── City detail texture blend ───────────────────────────────────────────
        // Fades in a tileable high-frequency detail texture over bright earthNightTex pixels
        // (cities) within a fixed distance of the observer: dayDetail replaces dayColor,
        // nightDetail replaces the night emissive term. Beyond kCityFadeFarM, or over
        // non-city terrain, this is a no-op and dayColor/nightColor pass through unchanged.
        {
            const float kCityDetailTileM = 20000.0;  // metres per texture tile repeat
            const float kCityMaskLo      = 0.01;   // nightColor luminance where detail starts
            const float kCityMaskHi      = 0.3;   // luminance where detail is fully blended in
            const float kCityFadeNearM   = 30000.0; // full detail strength inside this distance
            const float kCityFadeFarM    = 300000.0; // detail fully faded out beyond this distance
            float cityDistFade = 1.0 - smoothstep(kCityFadeNearM, kCityFadeFarM, tSurface);
            if (cityDistFade > 0.001)
            {
                // Cheap reject using the already-fetched, full-detail nightColor (no extra texture
                // fetch) before paying for the blurred sample + noise below — most terrain within
                // kCityFadeFarM isn't near a city at all.
                float cityLumFast = dot(nightColor, vec3(0.2126, 0.7152, 0.0722));
                float cityMask = 0.0;
                vec2  worldXY  = vec2(0.0);
                if (cityLumFast > kCityMaskLo - 0.05)
                {
                    // World-fixed ground coordinate — see the detailUV comment below for why this
                    // (not hitPt.xy directly) is what stays glued to the terrain as the observer
                    // moves. Computed here too so the edge-jitter noise below can share it.
                    worldXY = hitPt.xy + vec2(cloud.pad1, cloud.pad2);

                    // earthNightTex is only ~8K across the whole globe (~5 km/texel) — city
                    // silhouettes are inherently blocky at that resolution, and no amount of
                    // filtering recovers detail that was never captured. Two cheap tricks disguise
                    // it instead of trying to resolve it:
                    //   1. Sample luminance at a deliberately coarser LOD than nightColor's own
                    //      (already-mip-selected) sample, so the mask transition isn't chasing the
                    //      raw texel grid — a wider, softer step instead of a hard mip-block edge.
                    //   2. Jitter the smoothstep threshold with the same analytic 3D Perlin noise
                    //      the clouds use (warpPerlin3 — pure ALU, no texture, no tiling seam at any
                    //      zoom), sampled in the world-fixed ground plane at a frequency well above
                    //      the source texture's resolution. This breaks the mip-grid-aligned
                    //      blockiness into an organic, irregular wobble. It can't recover the TRUE
                    //      city boundary (there's no more data than the low-res mask has) — it's
                    //      purely a masking/edge-shape disguise, independent of the (already
                    //      world-fixed) detail texture UV below.
                    const float kCityMaskLod          = 1.5;
                    const float kCityEdgeNoiseFreqInv = 1.0 / 8000.0; // ~800 m noise period
                    const float kCityEdgeNoiseAmt     = 0.05;        // threshold jitter (luminance units)
                    float cityLumBlur = dot(textureLod(earthNightTex, uvSurf, kCityMaskLod).rgb,
                                             vec3(0.2126, 0.7152, 0.0722));
                    float edgeNoise = warpPerlin3(vec3(worldXY * kCityEdgeNoiseFreqInv, 0.0));
                    float jitter    = edgeNoise * kCityEdgeNoiseAmt;
                    // max(sharp, blurred), not blurred alone: blurring dilutes peak brightness (a
                    // city core averaged with its darker surroundings at LOD 1.5 may never cross
                    // kCityMaskHi on its own), which was capping cityMask well below 1.0 even deep
                    // in bright cores — nightColor could never fully hand off to nightDetail, so the
                    // detail pattern was always fighting a still-visible base underneath. The sharp
                    // sample lets genuinely bright pixels reach full mask strength; the blurred+
                    // jittered sample still governs the soft, noisy edge in between.
                    float cityLumForMask = max(cityLumFast, cityLumBlur);
                    // Once kCityEdgeNoiseAmt exceeds kCityMaskLo, jitter can push the lower
                    // threshold below zero — and a negative lower threshold means truly-dark
                    // (cityLumForMask≈0) pixels read as "above threshold", producing spurious light
                    // patches in genuinely unpopulated areas. Shift the whole [lo,hi] band up
                    // together when that would happen (rather than clamping loT alone, which would
                    // narrow or invert the transition width) — preserves the organic per-edge jitter
                    // everywhere it's safe, without ever letting true darkness read as lit.
                    float loT = kCityMaskLo + jitter;
                    float hiT = kCityMaskHi + jitter;
                    const float kCityMaskLoFloor = kCityMaskLo * 0.5;
                    if (loT < kCityMaskLoFloor)
                    {
                        float shift = kCityMaskLoFloor - loT;
                        loT += shift;
                        hiT += shift;
                    }
                    cityMask = smoothstep(loT, hiT, cityLumForMask) * cityDistFade;
                }
                if (cityMask > 0.001)
                {
                    // hitPt.xy (observer-local ENU tangent plane) is a real orthogonal projection —
                    // a physical square patch of ground always looks square in it, at any latitude,
                    // any distance — unlike a lon/lat-derived (plate-carrée-style) UV, which is only
                    // exactly square-scale at the one latitude its metric was evaluated at and
                    // visibly skews elsewhere (tried; no derivative fix rescues it, it's the wrong
                    // coordinate system). So a local ENU tangent plane is the right shape. hitPt.xy's
                    // only flaw is being tied to the observer's live position (drifts as the observer
                    // moves). A fixed-basis anchor was tried to cancel that exactly, but re-deriving
                    // the basis at each grid-snap silently rotated the axes a little, not just
                    // translated them — a visible pop at every snap instead of a seamless jump.
                    //
                    // The actual fix is simpler: hitPt.xy's drift, for any point near the observer,
                    // is to leading order just a uniform shift equal to the observer's OWN north/east
                    // motion (shifting the reference frame doesn't rotate nearby points relative to
                    // each other, it moves them all together). So track the observer's cumulative
                    // north/east displacement on the CPU (cityOffsetEastM/NorthM in
                    // SatelliteSim.cpp, packed into cloud.pad1/pad2) and add it straight back —
                    // a plain per-frame-constant translation, no basis, no trig, no snap events.
                    // worldXY (computed above) is this same hitPt.xy + cloud.pad1/pad2 offset —
                    // reused here rather than recomputed.
                    vec2 detailUV = worldXY / kCityDetailTileM;
                    vec2 duv_dx = dFdx(detailUV);
                    vec2 duv_dy = dFdy(detailUV);
                    vec3 dayDetail   = textureGrad(cityDayDetailTex,   detailUV, duv_dx, duv_dy).rgb;
                    vec3 nightDetail = textureGrad(cityNightDetailTex, detailUV, duv_dx, duv_dy).rgb;
                    // Same blur-through-cloud treatment as the base nightColor sample above —
                    // this is the HIGHER-frequency of the two textures, so left sharp it would be
                    // the more visible half of the "sharp texture cutting through cloud" artifact.
                    if (cloud.cityLightBlurLod > 0.01 && localCloudOpacity > 0.001) {
                        vec3 nightDetailBlur = textureLod(cityNightDetailTex, detailUV, cloud.cityLightBlurLod).rgb;
                        nightDetail = mix(nightDetail, nightDetailBlur, localCloudOpacity);
                    }
                    dayColor   = mix(dayColor,   dayDetail,   cityMask * (1.0 - cloud.cityLightsStrength));
                    nightColor = mix(nightColor, nightDetail, cityMask * (1.0 - cloud.cityLightsStrength));
                }
            }
        }

        // ── Procedural city layout (.plans/CITIES_PLAN.md): computed once, the day albedo here and
        // the night lights below. On land, and on land AT sea level (the march's hit voided to the sea
        // path, waterPx 0): much of a coastal basin like LA's is DEM 0-35 m. Where the night map has
        // lights; the day albedo is weighted by how much (city presence).
        vec3  cityLights = max(nightColor - vec3(0.006, 0.006, 0.0132), vec3(0.0));
        float cityLum    = dot(cityLights, vec3(0.2126, 0.7152, 0.0722));
        bool  cityLand   = tHit > 0.0 || (waterPx == 0 && tSeaLvl > 0.0);
        CityLayout cityL;
        bool  cityLOk    = false;
        float cFoot      = 1e9;
        if (cityLand && cloud.cityLightsStrength > 0.0 && cityLum > 1e-5 && tdEnabled()) {
            float cT = tHit > 0.0 ? tHit : tSeaLvl;
            vec3  cQ = tHit > 0.0 ? terrainQ : vec3(0.0, 0.0, obsEffH + 2.0) + tSeaLvl * dir;
            cFoot = pixAngle * cT / max(abs(dot(dir, shadingN)), 0.2);
            if (cFoot < 400.0) {   // past it both patterns are uniform: orbit sees the maps as before
                cityL   = cityLayout(cQ, enuX, enuY, enuZ, cityLum);
                cityLOk = true;
                float presence = smoothstep(0.002, 0.03, cityLum) * cloud.cityLightsStrength;
                dayColor *= mix(vec3(1.0), clamp(cityDayAlbedo(cityL, cFoot), vec3(0.0), vec3(4.0)), presence);
            }
        }

        // ── Farmland (rural, where the day map is cultivated: not forest, desert, snow, steep, city) ──
        float farmW = 0.0, farmFoot = 1e9;
        vec3  farmQ = vec3(0.0);
        if (cityLand && cloud.cityLightsStrength > 0.0 && tdEnabled()) {
            float fT    = tHit > 0.0 ? tHit : tSeaLvl;
            float fFoot = pixAngle * fT / max(abs(dot(dir, shadingN)), 0.2);
            if (fFoot < 400.0) {
                vec3  dm    = textureLod(earthDayTex, uvSurf, 1.0).rgb;
                float lumD  = dot(dm, vec3(0.2126, 0.7152, 0.0722));
                float vegD  = (dm.g - dm.r) / (dm.g + dm.r + 1e-3);
                float mxD   = max(dm.r, max(dm.g, dm.b));
                float satD  = (mxD - min(dm.r, min(dm.g, dm.b))) / max(mxD, 1e-3);
                float forest = 1.0 - smoothstep(0.03, 0.055, lumD);        // dark green = forest
                float desert = smoothstep(0.22, 0.34, lumD) * (1.0 - smoothstep(-0.02, 0.06, vegD));
                float snowD  = smoothstep(0.40, 0.62, lumD) * (1.0 - smoothstep(0.10, 0.28, satD));
                float flatG  = smoothstep(0.93, 0.98, dot(shadingN, normalize(hitPt)));
                float cityP  = smoothstep(0.002, 0.03, cityLum);   // the city's own presence: they cross-fade
                float high   = 1.0 - smoothstep(2500.0, 3500.0, tHit > 0.0 ? terrainH0 : 0.0);
                // Not near water: the 5-km day map blends the sea's blue into coastal land (a flat shelf at
                // Big Sur drew lavender fields) — keep ~0.5 km back from the shore and off water-tinted texels.
                float shoreD = -(textureLod(earthSpecTex, uvSurf, 0.0).r - 0.5) * 2.0 * kShoreSdfMaxM;
                float bluish = smoothstep(0.0, 0.04, dm.b - max(dm.r, dm.g));
                float farm   = (1.0 - forest) * (1.0 - 0.8 * desert) * (1.0 - snowD) * flatG * (1.0 - cityP) * high
                             * smoothstep(300.0, 800.0, shoreD) * (1.0 - bluish);
                if (farm > 0.01) {
                    vec3  fQ    = tHit > 0.0 ? terrainQ : vec3(0.0, 0.0, obsEffH + 2.0) + tSeaLvl * dir;
                    float green = smoothstep(-0.02, 0.12, vegD);
                    float dry   = smoothstep(0.1, 0.25, lumD) * (1.0 - green);
                    vec3  fr = farmDayAlbedo(fQ, enuX, enuY, enuZ, uvSurf.x * 360.0 - 180.0, green, dry, fFoot);
                    float tFade = 1.0 - smoothstep(200.0, 350.0, fFoot);    // unchanged past 350 m (orbit)
                    dayColor *= mix(vec3(1.0), clamp(fr, vec3(0.0), vec3(4.0)), farm * tFade * cloud.cityLightsStrength);
                    farmW = farm * cloud.cityLightsStrength;
                    farmFoot = fFoot;
                    farmQ = fQ;
                }
            }
        }

        // ── Beaches: sand on low, gentle shores (beachAt) — dry sand, wet near the water ─────────
        if (cityLand && cloud.terrainMaterialStrength > 0.0 && tdEnabled()) {
            float bT    = tHit > 0.0 ? tHit : tSeaLvl;
            float bFoot = pixAngle * bT / max(abs(dot(dir, shadingN)), 0.2);
            if (bFoot < 350.0) {
                vec3  bQ = tHit > 0.0 ? terrainQ : vec3(0.0, 0.0, obsEffH + 2.0) + tSeaLvl * dir;
                float dLand;
                float bw = beachAt(bQ, enuX, enuY, enuZ, uvSurf, tHit > 0.0 ? terrainH0 : 0.0,
                                   dot(shadingN, normalize(hitPt)), dLand);
                if (bw > 0.0) {
                    vec3 sand = mix(vec3(0.21, 0.18, 0.13), vec3(0.43, 0.37, 0.26), smoothstep(4.0, 25.0, dLand));
                    dayColor = mix(dayColor, sand, bw * cloud.terrainMaterialStrength * (1.0 - smoothstep(200.0, 350.0, bFoot)));
                }
            }
        }

        // ── Procedural terrain material (terrain_detail.glsl, 2026-09-25) ──────────
        // The day map is ~4.9 km/texel: from the ground it is one flat colour per hillside. The
        // day map stays the authority on WHAT the ground is (its colour is the biome, its white is
        // snow); this adds what it cannot resolve: a world-fixed mottle (tdMicro), rock on steep
        // faces (dark rock where the map says snow), snow shed from steep faces with a noisy edge,
        // and crevice darkening from the detail height. All of it fades out as the pixel footprint
        // passes ~60-400 m, so from orbit the day map is untouched. terrainMaterialStrength 0 = the
        // day map alone. (A latitude snowline was tried first: it painted the Tibetan plateau white.)
        float terrainAO = 1.0;
        float terrainMatSteep = 0.0, terrainMatSnow = 0.0;
        if (tHit > 0.0 && cloud.terrainMaterialStrength > 0.0) {
            float foot = pixAngle * tHit;
            float ms   = cloud.terrainMaterialStrength * (1.0 - smoothstep(60.0, 400.0, foot));
            if (ms > 0.0) {
                vec3  nE   = terrainNorm.x * enuX + terrainNorm.y * enuY + terrainNorm.z * enuZ;
                float nUp  = dot(nE, terrainUpE);
                float dn   = terrainDet.amp > 1.0 ? clamp(terrainDet.h / terrainDet.amp, -1.0, 1.0) : 0.0;
                float m    = tdMicro(terrainQ, enuX, enuY, enuZ, foot);
                vec3  day  = dayColor;
                float lum  = dot(day, vec3(0.2126, 0.7152, 0.0722));
                float mx   = max(day.r, max(day.g, day.b));
                float sat  = (mx - min(day.r, min(day.g, day.b))) / max(mx, 1e-3);
                float snowMap = smoothstep(0.40, 0.62, lum) * (1.0 - smoothstep(0.10, 0.28, sat));
                terrainMatSteep = smoothstep(0.30, 0.62, 1.0 - nUp + 0.10 * m);
                vec3  rockBare = mix(vec3(lum), day, 0.35) * (0.72 + 0.18 * m);
                vec3  rock     = mix(rockBare, vec3(0.20, 0.19, 0.17) * (0.85 + 0.25 * m), snowMap);
                vec3  alb      = day * (1.0 + 0.32 * m) * (1.0 + 0.12 * dn);
                alb = mix(alb, rock, terrainMatSteep * 0.85);
                terrainMatSnow = snowMap * (1.0 - smoothstep(0.22, 0.48, 1.0 - nUp + 0.12 * m - 0.06 * dn));
                alb = mix(alb, vec3(0.80, 0.82, 0.86) * (0.96 + 0.06 * m), terrainMatSnow);
                dayColor  = mix(dayColor, alb, ms);
                terrainAO = mix(1.0, 0.68 + 0.32 * smoothstep(-0.9, 0.6, dn), ms * terrainDet.rough);
            }
        }
        // Close-up material textures (terrain v2 P3, computed with the shading normal above).
        if (tmFade > 0.0) {
            dayColor  *= mix(vec3(1.0), clamp(tmRatio, vec3(0.0), vec3(3.0)), tmFade);
            terrainAO *= mix(1.0, tmAO, tmFade);
        }

        // ── The Sun's shadow line at this hit point, and the colour of the light on it ────────
        // Two things come out of this block, and they are one geometric fact seen twice:
        //
        //   1. `sunDiscVis`: does the Sun's DISC clear this point's OWN horizon? An elevated point
        //      sees further over the Earth's curve, so its horizon is DIPPED by
        //      sin(dip) = sqrt(1 - (R/|hitPt|)^2) — 0.96 degrees at 900 m. The code below always
        //      had this test (the raySphere against R_EARTH) but used it only to CHOOSE A TINT, so
        //      ground that cannot see the Sun at all still received 17% of full direct sunlight
        //      (dayFrac at Sun -0.96 degrees) in WHITE, because the tint fell back to vec3(1.0)
        //      inside the shadow. Measured at the twilight-terrain view (harness tw_terms_fixed,
        //      2026-09-25): the shadowed ground's tDirectSun was (6.0, 6.2, 6.0)e-3 — dead neutral,
        //      and 4x the whole sky ambient (1.4e-3) — a flat grey slab whose edge was exactly the
        //      shadow line, so it appeared to CHASE the red-lit slopes above it as the Sun moved.
        //      0.0046 is sin(0.27 deg), the Sun's own angular radius, so that soft edge is the
        //      physical width of the solar limb crossing the terrain's horizon.
        //   2. `sunSpecTint`: the hue of the light arriving along the Sun's direction, with that
        //      direction clamped to (never below) the same local horizon. Below the shadow line the
        //      ground therefore gets the deep sunset red that grazes the horizon toward the Sun's
        //      azimuth instead of a WHITE fallback, so the tint is continuous ACROSS the line:
        //      warm alpenglow on the slopes above it, its dimmed continuation below, no colour
        //      cliff between them.
        // Where the Sun IS up nothing changes: sunDiscVis is 1, the tint is the same computation
        // from the true sunDir, and tDirectSun's 0.05 sunLit floor and 0.15 terrain-shadow floor
        // (both there for faces turned from a DAYTIME Sun) keep applying.
        vec3  hitUp    = normalize(hitPt);
        float dipCos   = R_EARTH / length(hitPt);
        float dipSin   = sqrt(max(0.0, 1.0 - dipCos * dipCos)); // sin of the horizon dip at this altitude
        float sunDiscVis = smoothstep(-dipSin - 0.0046, -dipSin + 0.0046, geoSunDot);
        vec3  sunDirLocal = (geoSunDot > -dipSin) ? sunDir : normalize(sunDir - hitUp * geoSunDot);
        vec3 sunSpecTint = vec3(1.0);
        {
            vec2 tSAT = raySphere(hitPt, sunDirLocal, R_ATMOS);
            if (tSAT.y > 0.0) {
                vec2 sODT    = optDepth(hitPt, sunDirLocal, tSAT.y);
                vec3 attnT   = exp(-(BETA_R * sODT.x + BETA_M * 1.1 * sODT.y));
                float maxAttn = max(max(attnT.r, attnT.g), max(attnT.b, 0.001));
                sunSpecTint  = attnT / maxAttn;  // hue-normalized: max channel → 1.0
            }
        }

        // ── Sky ambient at terrain hit ─────────────────────────────────────────
        // 4-step zenith integration gives the scattered sky color illuminating terrain
        // faces: blue during day, warm-orange during twilight. Mirrors the cloud skyAmbientBase.
        vec3 skyAmbientTerrain = vec3(0.0);
        {
            vec3  zenT = normalize(hitPt);  // terrain zenith (radially outward)
            vec2  tSAT = raySphere(hitPt, zenT, R_ATMOS);
            if (tSAT.y > 0.0) {
#ifdef SKY_LITE
                const int N_ZT = 2;   // Planetarium-tier: halve the optDepth zenith integration
#else
                const int N_ZT = 4;
#endif
                float zSegT    = tSAT.y / float(N_ZT);
                float cosAT    = dot(zenT, sunDir);
                float pR_upT   = 0.75 * (1.0 + cosAT * cosAT);
                float pM_upT   = phaseM(cosAT);
                float odR_zt = 0.0, odM_zt = 0.0;
                float skyAmbTM = 0.0;
                for (int zi = 0; zi < N_ZT; ++zi) {
                    vec3  sp = hitPt + zenT * ((float(zi) + 0.5) * zSegT);
                    float h  = max(0.0, length(sp) - R_EARTH);
                    float dR = exp(-h / H_R) * zSegT;
                    float dM = exp(-h / H_M) * zSegT;
                    odR_zt += dR;  odM_zt += dM;
                    vec2 tSE  = raySphere(sp, sunDir, R_EARTH);
                    if (tSE.x > 0.0 && tSE.y > 0.0) continue;
                    vec2 tSun = raySphere(sp, sunDir, R_ATMOS);
                    vec2 sunOD = (tSun.y > 0.0) ? optDepth(sp, sunDir, tSun.y) : vec2(0.0);
                    vec3 tau      = BETA_R * (odR_zt + sunOD.x) + BETA_M * 1.1 * (odM_zt + sunOD.y);
                    vec3 attnStep = exp(-tau);
                    skyAmbientTerrain += attnStep * dR;
                    skyAmbTM         += dot(attnStep, vec3(1.0 / 3.0)) * dM;
                }
                skyAmbientTerrain = SUN_INTENSITY * (pR_upT * BETA_R * skyAmbientTerrain
                                                   + vec3(pM_upT * BETA_M * skyAmbTM));
            }
        }

        // ── Moonlight at terrain hit ────────────────────────────────────────────
        // Mirrors the sun's own direct-light pattern above (shadingN·dir Lambertian +
        // geographic horizon gate) rather than cloud_march.comp's self-shadow/phase model —
        // terrain has no volumetric self-occlusion to model, so the sun scaffolding already
        // in this block is the closer fit. cloud.moonGain is shared with cloud_march.comp's
        // moonContrib so terrain and moonlit clouds stay calibrated to the same brightness.
        vec3  moonDir3t        = normalize(moonDirENU.xyz);
        float moonDot          = dot(shadingN, moonDir3t);
        float geoMoonDot       = dot(normalize(hitPt), moonDir3t);
        float moonHorizonGate  = smoothstep(-0.03, 0.02, geoMoonDot);
        float moonLitTerrain   = max(0.0, moonDot) * moonHorizonGate * moonDirENU.w;
        vec3  moonContribTerrain = dayColor * vec3(0.92, 0.95, 1.0) * moonLitTerrain * cloud.moonGain;

        // Aurora ground-glow: soft ambient wash from the curtain overhead, evaluated LOCALLY at
        // this hit point (auroraGlowAt — same oval mask + fold noise the sky curtain itself uses)
        // rather than a single observer-position proxy, so lighting is properly local like
        // moonlight: only ground actually under an active curtain lights up. Modulated by how much
        // the surface faces "up" (toward the glow), same spirit as skyAmbientTerrain's fill above.
        //
        // auroraGlowAt needs a TRUE ECEF direction (it compares against the fixed geomagnetic-pole
        // ECEF constant) — hitPt itself is in the observer-local ENU-ish frame (same convention
        // rp/obsPos/dir all use), so it must go through the enuX/enuY/enuZ basis first, same as
        // every other geographic lookup in this file (e.g. the terrain-hit lat/lon UV above). A
        // first version passed normalize(hitPt) directly, which is a fine "local up" vector for the
        // Lambertian dot product just below (shadingN is in the SAME local frame, so that part was
        // already correct) but wrong for auroraGlowAt specifically — it made the computed
        // "geographic" position track the OBSERVER's own frame instead of the terrain point's true
        // location, so the noise pattern appeared to follow the observer instead of the ground.
#ifdef SKY_LITE
        vec3  auroraContribTerrain = vec3(0.0);   // Planetarium-tier: aurora surface glow cut (auroraGlowAt's fold-noise fetch + oval mask)
#else
        vec3  hitDirECEF           = normalize(hitPt.x * enuX + hitPt.y * enuY + hitPt.z * enuZ);
        vec3  auroraGlowTerrain    = auroraGlowAt(hitDirECEF, sunDirECEF, pc.waveTime, cloud.stormStrength);
        // Same cloud-awareness gap as the ocean reflection fix (see that block's comment) — this
        // is a plain ambient wash, so the simplest gate is reusing cloudB, already sampled for
        // THIS pixel's own camera ray up top: an overcast view of this ground point dims its
        // aurora glow along with everything else, no new march or texture read needed.
        float auroraGroundCloudOccl = dot(cloudB.rgb, vec3(1.0 / 3.0));
        vec3  auroraContribTerrain = dayColor * auroraGlowTerrain
                                    * max(dot(shadingN, normalize(hitPt)), 0.0)
                                    * cloud.auroraGroundGain * auroraGroundCloudOccl;
#endif

        // cloudShadowT gates only the direct-sun term (real cloud shadows block the sun, not the
        // diffuse skylight) — skyAmbientTerrain stays outside it, same split the ocean branch
        // below already uses (directSun on the sun specular/diffuse terms, plain dayFrac on the
        // sky reflection). Previously this term used dayFrac alone with no cloud-shadow factor at
        // all, so cloud shadows never appeared on land — only on the ocean/sea-level branch, which
        // is the only place `directSun` (dayFrac * cloudShadowT) was actually consumed.
        // Terrain sun shadow (terrain_detail.glsl terrainSunShadow): a soft march toward the Sun over
        // DEM + detail. Only where the Sun is up at this point and the face can see it.
        float terrainShadow = 1.0;
#if !defined(SKY_LITE) && !defined(SKY_ENV)
        if (tHit > 0.0 && cloud.terrainShadowStrength > 0.0 && dayFrac > 0.0 && sunDot > -0.05) {
            float sh = terrainSunShadow(earthElevTex, earthSpecTex, terrainQ, terrainNorm, sunDir, enuX, enuY, enuZ,
                                        tdGeomLodM(tHit, pixAngle), terrainH0 + terrainDet.hC3);
            terrainShadow = mix(1.0, sh, cloud.terrainShadowStrength);
        }
#endif
        // Direct sun: 1.5x Lambert with a 0.05 floor. The floor used to be a hard clamp; with the
        // detail normals turning many small facets away from a low Sun its kink drew hard contour
        // rims around every one, so it is now reached smoothly (unchanged above sunDot*1.5 = 0.2).
        float sunLit = clamp(sunDot * 1.5, 0.0, 1.0) + 0.05 * (1.0 - smoothstep(0.0, 0.2, sunDot * 1.5));
        // Ground bounce (procedural material only, terrainMaterialStrength): light reflected off the
        // sunlit ground around a face, proportional to that ground's own albedo — the day map's
        // luminance here — and the Sun's height over the local horizon. Snow reflects ~0.8 and lit
        // it strongly: without this a snowfield's faces turned from a low Sun went near-black
        // (harness flight into a glacier); forest (~0.1) barely changes.
        float bounceK = 0.0;
        if (tHit > 0.0 && cloud.terrainMaterialStrength > 0.0)
            bounceK = 0.3 * cloud.terrainMaterialStrength * dot(dayColor, vec3(0.2126, 0.7152, 0.0722))
                    * max(dot(normalize(hitPt), sunDir), 0.0);
        // The terrain's own albedo IS the surface all through twilight — dimmed and reddened by
        // the sky, never swapped out for another map — and the city-lights texture is only an
        // emissive ADD on top of it, faded in as the twilight closes (and back out above, the
        // same way). Deep night is therefore unchanged by design: albedo lit by nothing is
        // black, so only the lights and the Moon are left there (no terrain night ambient term —
        // decided in session 25), and the night detail texture stays the city layout at close
        // range, on top of black rather than under a grey wash.
        // Each contribution is named, so the harness debug channels below (`debugview terms`,
        // `direct`, `skyamb`, `night`, `moon`, `aurora`) can report which one actually lights a
        // pixel instead of leaving it to arithmetic on the beauty frame. Behaviour is unchanged:
        // the sum below is the same expression the unnamed version evaluated.
        // mix(0.15, 1, shadow): light bounced off the sunlit terrain around a shadowed face — without it terrain shadows went near-black.
        // * sunDiscVis: no direct sun reaches ground that cannot see the Sun's disc (see the tint
        // block above) — without it the 0.05 sunLit floor, the 0.15 terrain-shadow floor and
        // dayFrac's 1.5-degree soft gate lit the whole night side with a NEUTRAL wash 4x brighter
        // than the sky ambient. That was the grey slab of 2026-09-25.
        vec3 tDirectSun = dayColor * sunSpecTint * (sunLit * mix(0.15, 1.0, terrainShadow) + bounceK)
                        * cloudShadowT * dayFrac * sunDiscVis;
        // Sky view (terrain v2 P1): the share of the sky dome a face sees, 1 flat, 1/2 vertical. The
        // zenith integral above is the light of a horizontal face; x "Terrain sky light"
        // (cloud.terrainErosion.w) scales it toward the hemisphere's real irradiance — at 0.4 the
        // ground in cloud shadow was near-black by day.
        float skyView   = 0.5 + 0.5 * dot(shadingN, hitUp);
        vec3 tSkyAmb    = dayColor * skyAmbientTerrain * (0.4 * cloud.terrainErosion.w * skyView) * twilightFrac;
        // City lights only (terrain v2 P1). The night map is a Black Marble composite with a BLUE
        // base under every texel (land sRGB ~(12,13,25), ocean ~(5,7,22)); x 0.12 that base out-shone
        // full-moon terrain, so the whole night ground was a flat blue 5 km texture. Subtracting the
        // base (linear 0.006, blue x 2.2) zeroes ~99% of texels and keeps ~all of the lights' energy
        // (measured on the map, 2026-09-29). A plain subtraction, no knee: it is linear above the
        // base, so subtracting after the texture filter is ~subtracting before it — a knee on the
        // filtered value drew every 5 km texel as a hard-edged square from orbit.
        // The procedural street lights on the same layout (computed above, with the day albedo).
        if (cityLOk && twilightFrac < 1.0) {
            vec3 pat = cityLightPattern(cityL, uvSurf, cFoot, dot(shadingN, normalize(hitPt)));
            cityLights *= mix(vec3(1.0), pat, cloud.cityLightsStrength);
        }
        vec3 tNight     = cityLights * (0.12 * (1.0 - twilightFrac));       // city lights: emissive only, never the surface
        // Farmsteads in farmland (a floor: the night map is ~0 over real farmland — 0.0006 in rural Iowa —
        // so its own lights never showed). Equivalent to a map value of 0.004; faded out before orbital
        // footprints like every other pattern (orbit unchanged).
        if (farmW > 0.0 && twilightFrac < 1.0 && farmFoot < 350.0) {
            vec2  fp2;
            ivec2 fdA;
            int   ff;
            vec3  frl, fau;
            cityFrame(farmQ, enuX, enuY, enuZ, fp2, fdA, ff, frl, fau);
            tNight += kLampSodium * (farmsteadLights(fp2, fdA, ff, farmFoot) * 0.004 * 0.12)
                    * farmW * (1.0 - twilightFrac) * (1.0 - smoothstep(150.0, 350.0, farmFoot));
        }
        // Night light ON the albedo (terrain v2 P1): the moonlit sky (moonlight scattered by the air —
        // ~0.15 of the direct Moon on a flat face, like the Sun's diffuse share) and the moonless
        // night sky (starlight + airglow), "Night sky light" (cloud.terrainErosion.z) as a fraction of
        // the full Moon overhead. Slightly cool, never the old blue. Both are negligible by day.
        float moonSkyK  = 0.15 * moonDirENU.w * smoothstep(-0.05, 0.3, geoMoonDot);
        vec3 tNightSky  = dayColor * vec3(0.88, 0.93, 1.0) * (cloud.moonGain * (moonSkyK + cloud.terrainErosion.z) * skyView);
        vec3 surfColor  = (tDirectSun + tSkyAmb + tNightSky) * terrainAO  // terrainAO darkens only the map/sky-lit surface
                        + tNight
                        + moonContribTerrain
                        + auroraContribTerrain;

        // Terrain debug views (harness `debugview`, cloud.terrainDebugView) — override the pixel.
        if (cloud.terrainDebugView > 0.5 && tHit > 0.0) {
            int dv = int(cloud.terrainDebugView + 0.5);
            vec3 dbg = vec3(0.0);
            if (dv == 1) dbg = terrainNorm * 0.5 + 0.5;
            else if (dv == 2) dbg = vec3(0.5 + 0.25 * (terrainDet.amp > 1.0 ? clamp(terrainDet.h / terrainDet.amp, -2.0, 2.0) : 0.0));
            else if (dv == 3) {
                float f = clamp(float(terrainSteps) / float(kTerrainMaxSteps), 0.0, 1.0);
                dbg = (f < 0.5) ? mix(vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 0.0), f * 2.0)
                                : mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), f * 2.0 - 1.0);
            }
            else if (dv == 4) dbg = dayColor;
            else if (dv == 5) dbg = vec3(terrainShadow * max(sunDot, 0.0));
            else if (dv == 6) dbg = vec3(terrainDet.rough, terrainMatSteep, terrainMatSnow);
            // Zebra views, from the `erosion` branch: stripes at a fixed real-world interval show small
            // jitter a normalized heatmap cannot — broken or jagged stripes are height (7: every 25 m of
            // elevation) or hit-distance (8: every 100 m along the ray) instability, not texture.
            else if (dv == 7) dbg = vec3(mix(0.05, 0.95, mod(floor(tdAltitude(terrainQ) / 25.0), 2.0)));
            else if (dv == 8) dbg = vec3(mix(0.05, 0.95, mod(floor(tHit / 100.0), 2.0)));
            // 9: the erosion octaves alone (grey = none; bright = ridge, dark = gully), relative to
            // their bound; blue where the slope is too gentle for any.
            else if (dv == 9) {
                float eb = terrainDet.amp * cloud.terrainErosion.x * 0.875;
                dbg = eb > 1.0 ? vec3(0.5 + 0.5 * clamp(terrainDet.hEro / eb, -1.0, 1.0)) : vec3(0.1, 0.2, 0.5);
            }
            // 10-24: the terrain LIGHTING, term by term. These are LINEAR radiance values written
            // straight into the frame buffer (this block bypasses the exposure/tonemap at the end of
            // main), so a capture's own pixels are the numbers: the value the eye gets is
            // srgb(v * gain) / 255, i.e. invert the sRGB curve to read v. Radiance terms are scaled
            // x100 (and per-channel, never luminance, so a term's COLOUR shows too); the gates and
            // factors of 16/17/21 are raw. twilight_terrain's ground sits near 8e-3 total, so x100
            // puts the interesting range inside 0..1 with ~1e-4 resolution per 8-bit step.
            else if (dv == 10) dbg = surfColor * 100.0;        // total terrain light, x100
            else if (dv == 11) dbg = tDirectSun * 100.0;       // direct sun (Lambert x dayFrac x cloud shadow)
            else if (dv == 12) dbg = tSkyAmb * 100.0;          // sky ambient x twilightFrac
            else if (dv == 13) dbg = tNight * 100.0;           // city-lights emissive add
            else if (dv == 14) dbg = moonContribTerrain * 100.0;
            else if (dv == 15) dbg = auroraContribTerrain * 100.0;
            else if (dv == 16) dbg = vec3(dayFrac, twilightFrac, terrainShadow);  // the gates
            else if (dv == 17) dbg = vec3(sunLit, bounceK * 10.0, dot(skyAmbientTerrain, vec3(1.0 / 3.0)) * 100.0);
            else if (dv == 18) dbg = skyAmbientTerrain * 100.0;  // its hue: blue day .. orange dusk
            else if (dv == 19) dbg = sunSpecTint;                // sun hue at the hit point (max channel 1)
            else if (dv == 20) dbg = vec3(dot(dayColor, vec3(0.2126, 0.7152, 0.0722)), terrainAO, cloudShadowT);
            else if (dv == 21) dbg = vec3(clamp((sunDir.z + 0.2) / 1.2, 0.0, 1.0), sunDir.z, dot(normalize(hitPt), sunDir));
            else if (dv == 22) dbg = nightColor * 20.0;          // the city-lights map itself here
            else if (dv == 25) dbg = tNightSky * 100.0;          // night sky + moonlit sky on the albedo
            else if (dv == 26) dbg = cityLights * 20.0;          // the night map with its base removed
            else if (dv == 23) dbg = vec3(geoSunDot, sunDot, tHit / 4000.0);
            // 24: the Sun's shadow line at this hit point. R = sunDiscVis (0 = the Sun's disc is
            // below this point's own horizon, so no direct sun reaches it), G = geoSunDot + dipSin
            // (>= 0 exactly when the disc is clear of that horizon, 0 on the line), B = dipSin at
            // this altitude. R is what tDirectSun is multiplied by, so one capture shows both the
            // gate and its margin over the line.
            else if (dv == 24) dbg = vec3(sunDiscVis, geoSunDot + dipSin, dipSin);
            terrainDebugColor  = dbg;
            terrainDebugActive = true;
        }

        // ── Ocean wave material (sea-level hits only, not terrain) ─────────────
        // ShaderToy "Seascape" by TDM adapted to Earth ENU/ECEF space.
        // heightMapTracing: 8-step secant refinement around the sea-sphere hit.
        // getNormal: central differences on seaMapDetail (5 octaves).
        // getSeaColor: kSeaBase refraction + atmosphere reflection + specular.
        float oceanMask = textureGrad(earthSpecTex, uvSurf, uvd_dx, uvd_dy).r;
        if ((waterPx >= 0 ? waterPx == 1 : oceanMask > 0.5) && tHit < 0.0) {
            vec3  surfUp  = normalize(hitPt);
            float dist    = tSeaLvl;
            float seaTime = 1.0 + pc.waveTime * kSeaSpeed;

            

            // Altitude fade: full 3D waves at low altitude, smooth specular from orbit.
            float altFade = 1.0 - smoothstep(3000.0, 8000.0, obsEffH - waterLevelM);

            // Wave UV strategy:
            //   posM = hitPt.xy (ENU East/North metres from observer nadir) — always small,
            //   so the 0.5 m normal epsilon is hundreds of float steps above ULP.
            //   An observer-geographic phase offset (modulo first-octave wave period ≈ 39.3 m)
            //   is added so the pattern is approximately Earth-fixed without accumulating
            //   large absolute coordinates. Derived entirely from enuZ (observer ECEF unit vec).
            vec2 obsPhase = vec2(0.0);
            if (altFade > 0.01) {
                const float wvScale = 2.0 * PI / kSeaFreq;
                float oLat  = asin(clamp(enuZ.z, -1.0, 1.0));
                float oLon  = atan(enuZ.y, enuZ.x);
                obsPhase.x  = fract(oLon * R_EARTH * cos(oLat) / wvScale) * wvScale;
                obsPhase.y  = fract(oLat * R_EARTH           / wvScale) * wvScale;
            }
            vec2 posM = hitPt.xy + obsPhase;

            // ── heightMapTracing (low altitude only) ──────────────────────────
            // Bracket: ±2.5 m vertical around the sea-sphere intersection.
            // hm > 0 at the near end (above waves), hx < 0 at the far end (inside).
            if (altFade > 0.01 && dist < 5000.0) {
                float cosEl  = max(0.05, abs(dot(dir, surfUp)));
                float traceR = min(60.0, 2.5 / cosEl);
                float tm     = tSeaLvl - traceR;
                float tx     = tSeaLvl + traceR;

                // Height above sea level computed as obsEffH + 2 + t*dir.z — avoids
                // catastrophic cancellation in length(p)-R_EARTH at sea level (float
                // ULP at 6.37 M m is 0.76 m, which quantises 1.5 m waves into ~2 steps).
                vec3  plo = obsPos + tm * dir;
                float hm  = seaMap(plo.xy + obsPhase, obsEffH + 2.0 - waterLevelM + tm * dir.z, seaTime);
                vec3  phi = obsPos + tx * dir;
                float hx  = seaMap(phi.xy + obsPhase, obsEffH + 2.0 - waterLevelM + tx * dir.z, seaTime);

                if (hx < 0.0) {
                    for (int i = 0; i < 8; i++) {
                        float tmid = mix(tm, tx, hm / (hm - hx));
                        vec3  pm   = obsPos + tmid * dir;
                        float hmid = seaMap(pm.xy + obsPhase, obsEffH + 2.0 - waterLevelM + tmid * dir.z, seaTime);
                        if (hmid < 0.0) { tx = tmid; hx = hmid; }
                        else             { tm = tmid; hm = hmid; }
                        if (abs(hmid) < 0.001) break;
                    }
                    float tWave = mix(tm, tx, hm / (hm - hx));
                    hitPt  = obsPos + tWave * dir;
                    surfUp = normalize(hitPt);
                    posM   = hitPt.xy + obsPhase;
                    dist   = tWave;
                }
            }

            // Same precision fix: obsEffH + 2 + dist*dir.z instead of length(hitPt)-R_EARTH.
            float pHeight = obsEffH + 2.0 - waterLevelM + dist * dir.z;
            vec3  viewDir = normalize(-dir);

            // ── getNormal (central differences on seaMapDetail) ───────────────
            // posM is in ENU East/North metres, so +eps in x = East, +eps in y = North.
            // Normal in ENU = normalize(East_slope, North_slope, Up_component).
            //
            // Perf (session 24 round 3, low-angle/horizon views): the blend factor below only
            // needs `dist`/`altFade`, both already known here — compute it FIRST and skip the
            // three seaMapDetail calls (15 octave evaluations total) entirely when it's already
            // ~1 (result blends back to flat `surfUp` regardless). The original version always
            // paid full detail cost then discarded most of it for distant ocean — exactly the
            // case that dominates horizon views, where most visible ocean is far past the 8km
            // distance fade. Bitwise-identical result for blend<0.99; below that threshold the
            // discarded detail was already imperceptible (>99% blended to flat).
            vec3 waveN = surfUp;
            if (altFade > 0.01) {
                float distFade = smoothstep(3000.0, 8000.0, dist);
                float blend    = max(distFade, 1.0 - altFade);  // 0 = full detail, 1 = flat
                if (blend < 0.99) {
                    float eps = max(0.5, dist * 0.0008);
                    float n0  = seaMapDetail(posM,                    pHeight, seaTime);
                    float nX  = seaMapDetail(posM + vec2(eps, 0.0),  pHeight, seaTime) - n0;
                    float nY  = seaMapDetail(posM + vec2(0.0,  eps), pHeight, seaTime) - n0;
                    waveN = normalize(vec3(nX, nY, 0.0) + eps * surfUp);
                    waveN = normalize(mix(waveN, surfUp, blend));
                }
            }

            // ── getSeaColor ────────────────────────────────────────────────────
            // Fresnel: cubic ramp, capped at 0.5 (ShaderToy formula)
            float fresnel = min(pow(clamp(1.0 - dot(waveN, viewDir), 0.0, 1.0), 3.0), 0.5);

            // Sky reflection — 6-sample atmosphere, distance-gated
            vec3 reflDir   = reflect(dir, waveN);
            vec3 reflColor = vec3(0.12, 0.28, 0.50) * dayFrac;
            float reflStr  = fresnel * exp(-dist / 40000.0);

            // Feature-reflection fade. Clouds, aurora and the Milky Way only reflect believably
            // off resolved 3D wave facets — the close-up detail waveN carries. As the surface
            // flattens toward a perfect mirror, either with observer altitude (altFade, shared
            // with the wave-normal detail budget) or with distance to this ocean point, those
            // discrete sky features should drop out: a mirror-flat sea seen from orbit rendering
            // a crisp inverted Milky Way / aurora band reads as plain wrong. What survives at
            // distance is the smooth scattered-sky reflection (reflColor's atmosphere march) plus
            // the sun and moon glints below — those are broad specular lobes, physically correct
            // on any surface roughness, and are deliberately NOT gated by this.
            float featureReflFade = altFade * (1.0 - smoothstep(3000.0, 8000.0, dist));

            if (!dbgSkipOceanRefl() && dot(reflDir, surfUp) > 0.0 && reflStr > 0.005) {
                vec2 tAR = raySphere(hitPt, reflDir, R_ATMOS);
                if (tAR.y > 0.0) {
                    int   N_REFL = int(max(1.0, cloud.oceanReflSamples)); // perf session 24, was const 6
                    float rStart = max(0.0, tAR.x);
                    float rSeg   = (tAR.y - rStart) / float(N_REFL);
                    float rcosA  = dot(reflDir, sunDir);
                    float rpR    = phaseR(rcosA);
                    float rpM    = phaseM(rcosA);
                    vec3  rAccR  = vec3(0.0);
                    float rAccM  = 0.0;
                    float rodR   = 0.0, rodM = 0.0;
                    for (int ri = 0; ri < N_REFL; ++ri) {
                        vec3  rp   = hitPt + reflDir * (rStart + (float(ri) + 0.5) * rSeg);
                        float rh   = max(0.0, length(rp) - R_EARTH);
                        float rdR  = exp(-rh / H_R) * rSeg;
                        float rdM  = exp(-rh / H_M) * rSeg;
                        rodR += rdR; rodM += rdM;
                        vec2 tSE  = raySphere(rp, sunDir, R_EARTH);
                        if (tSE.x > 0.0 && tSE.y > 0.0) continue;
                        vec2 tSun = raySphere(rp, sunDir, R_ATMOS);
                        vec2 sOD  = (tSun.y > 0.0) ? optDepth(rp, sunDir, tSun.y) : vec2(0.0);
                        vec3 rtau  = BETA_R * (rodR + sOD.x) + BETA_M * 1.1 * (rodM + sOD.y);
                        vec3 rattn = exp(-rtau);
                        rAccR += rattn * rdR;
                        rAccM += dot(rattn, vec3(1.0 / 3.0)) * rdM;
                    }
                    reflColor = SUN_INTENSITY * (rpR * BETA_R * rAccR + vec3(rpM * BETA_M * rAccM));
                }

                // Screen-space occlusion lookup for the REFLECTED ray, shared by the aurora and
                // Milky Way reflections below. Same technique as the satellite/sun lens-flare
                // occlusion above: project reflDir with the same skyView transform and sample what
                // the compute passes already wrote for that direction, rather than re-marching
                // cloud density or terrain along reflDir here. Hoisted out of the aurora block
                // when the Milky Way reflection was added — both want it, and it is one texture
                // fetch each instead of two identical ones.
                //
                // reflCam.z only proves the direction is in FRONT of the camera, not inside the
                // frustum; an off-screen reflDir clamps to an edge texel. Pre-existing limitation
                // of this technique here, not introduced by the hoist.
                float reflCloudOccl   = 1.0;
                float reflTerrainOccl = 1.0;
#ifndef SKY_ENV
                vec3  reflCam = mat3(pc.skyView) * reflDir;
                if (reflCam.z < -0.01) {
                    float tanHFRefl    = tan(pc.fovYRad * 0.5);
                    vec2  reflUV       = vec2(reflCam.x, -reflCam.y) / (-reflCam.z * tanHFRefl * 2.0);
                    vec2  reflScreenUV = vec2(reflUV.x / pc.aspect + 0.5, reflUV.y + 0.5);
                    reflCloudOccl   = dot(texture(cloudTargetB, reflScreenUV).rgb, vec3(1.0 / 3.0));
                    reflTerrainOccl = (texture(sceneDepthTex, reflScreenUV).r >= kNoSurfaceT * 0.5) ? 1.0 : 0.0;
                }
#endif

                // Aurora reflection: literally march the curtain shell along the REFLECTED ray
                // instead of the camera ray — reuses the exact same auroraSampleAt() the primary
                // sky view uses, so the aurora shows up as a genuine mirror-like glint on the
                // water (visible in the reflection itself, not just a flat ambient wash) whenever
                // a wave happens to reflect toward it. hitPt is always below the shell here (ocean
                // surface), so only the "observer below shell" entry/exit case applies — no need
                // for the full obsEffH-keyed classification the primary march uses.
                vec2 rAuroraFar = raySphere(hitPt, reflDir, R_EARTH + kAuroraShellOuterM);
                vec2 rAuroraIn  = raySphere(hitPt, reflDir, R_EARTH + kAuroraShellInnerM);
                if (featureReflFade > 0.001 && rAuroraIn.y > 0.0 && rAuroraIn.y < rAuroraFar.y) {
                    const int N_AURORA_REFL = 6;
                    float raSeg = (rAuroraFar.y - rAuroraIn.y) / float(N_AURORA_REFL);
                    vec3  accumAuroraRefl = vec3(0.0);
                    for (int ai = 0; ai < N_AURORA_REFL; ++ai) {
                        vec3 rap = hitPt + reflDir * (rAuroraIn.y + (float(ai) + 0.5) * raSeg);
                        accumAuroraRefl += auroraSampleAt(rap, enuX, enuY, enuZ, sunDirECEF,
                                                           pc.waveTime, cloud.stormStrength);
                    }
                    // Same atmospheric extinction as the primary sky march (see that block's
                    // comment) — using the REFLECTED ray's own elevation, since that's the
                    // direction the aurora's light actually traveled through the atmosphere before
                    // bouncing off the water toward the camera. Ocean views skew toward low-angle
                    // reflections by construction (Fresnel favors grazing angles), so this matters
                    // here at least as much as it does for the direct view.
                    float extinctionAuroraRefl =
                        pow(10.0, -0.4 * atmExtinctionMag(hitPt, reflDir, 0.0, cloud.extinctionCoeff));

                    // Cloud occlusion (reflCloudOccl, hoisted above): this march reused
                    // auroraSampleAt() (the raw curtain function) directly rather than going
                    // through cloud_march.comp's auroraMarchCS, so it had none of that pass's
                    // cloud-suppression — the water mirrored the aurora right through an overcast
                    // sky. Deliberately does NOT take reflTerrainOccl, which is new with the Milky
                    // Way reflection: leaving this term's behaviour exactly as it was.
                    reflColor += accumAuroraRefl * raSeg * kAuroraScale * cloud.auroraGain
                               * extinctionAuroraRefl * reflCloudOccl * featureReflFade;
                }

                // ── Milky Way reflection ──────────────────────────────────────────────────────
                // Same panorama, same galactic basis and same dark-sky exposure gate as the direct
                // view far above, evaluated along reflDir instead of dir — so on calm water under
                // a dark sky the band genuinely shows up in the reflection, and it dims with
                // skyglow and twilight coherently with the sky it is reflecting. Far cheaper than
                // the aurora reflection directly above it (one texture fetch, no shell march).
                //
                // SKY_LITE (Planetarium tier) cuts it for the same reason the direct view is cut
                // there: the equirect projection's atan/asin plus the panorama fetch is the exact
                // combination that does not fit that tier's budget. A tier that does not draw the
                // Milky Way must not draw its reflection either.
#ifndef SKY_LITE
                if (featureReflFade > 0.001) {
                    // Forced mip, NOT a plain texture(): the reflected direction comes off a
                    // noise-perturbed wave normal, so neighbouring pixels sample points far apart
                    // on a high-contrast panorama and the band scintillates. Sampling a blurred
                    // level is also the physically right answer — a rough surface reflects a
                    // blurred image, which is why the ocean already blurs city lights through
                    // cloud.cityLightBlurLod.
                    const float kOceanMwReflLod = 3.0;
                    vec3 rDirGal = vec3(dot(reflDir, cloud.mwBasisRow0.xyz),
                                        dot(reflDir, cloud.mwBasisRow1.xyz),
                                        dot(reflDir, cloud.mwBasisRow2.xyz));
                    float rLonGal = atan(rDirGal.y, rDirGal.x);
                    float rLatGal = asin(clamp(rDirGal.z, -1.0, 1.0));
                    vec2  rMwUV   = vec2(0.5 + rLonGal / (2.0 * PI), 0.5 + rLatGal / PI);
                    vec3  rMwColor = textureLod(milkyWayTex, rMwUV, kOceanMwReflLod).rgb
                                   * cloud.mwBasisRow0.w;

                    // Exposure gate against reflDir's OWN sky background — the reflection shows
                    // the patch of sky the wave points at, which can be a very different
                    // brightness from the patch the camera is looking at directly (the twilight
                    // anisotropy alone spans ~3 magnitudes across the sky).
                    float rMwSkyBgMag = darkSkySkyMag(reflDir, sunDirENU);
                    float rMwLum      = dot(rMwColor, vec3(0.2126, 0.7152, 0.0722));
                    float rMwObjMag   = darkSkySurfMag(rMwLum, kMwRefLum, kMwRefMag,
                                                       kMwMagSpread);

                    // Extinction along the REFLECTED ray's own elevation, matching the aurora
                    // reflection's reasoning: that is the path this light actually took through
                    // the atmosphere before bouncing off the water. Matters here more than for the
                    // direct view, since Fresnel biases ocean reflections toward grazing angles.
                    float extinctionMwRefl =
                        pow(10.0, -0.4 * atmExtinctionMag(hitPt, reflDir, 0.0, cloud.extinctionCoeff));

                    reflColor += rMwColor * darkSkyVis(rMwObjMag, rMwSkyBgMag)
                               * extinctionMwRefl * reflCloudOccl * reflTerrainOccl
                               * cloud.oceanMwReflGain * featureReflFade;
                }
#endif
            }

            // Refracted subsurface color (SEA_BASE + diffuse * SEA_WATER_COLOR)
            // directSun replaces dayFrac for all sun-driven contributions so clouds shadow the ocean.
            float diff    = pow(max(0.0, dot(waveN, sunDir)) * 0.4 + 0.6, 80.0) * directSun;
            vec3 refracted = kSeaBase * directSun + diff * kSeaWaterColor * 0.12;

            // Fresnel blend (distance-attenuated to prevent orbit-scale glowing ring)
            surfColor = mix(refracted, reflColor, reflStr);

            // Wave-height crest shading: raised crests catch more water-color light
            float atten = max(1.0 - dist * dist * 1e-5, 0.0);
            surfColor += kSeaWaterColor * max(pHeight - kSeaHeight, 0.0) * 0.18 * atten * directSun;

            // Specular: shininess narrows close-up, broadens with distance
            float specPow = clamp(600.0 / max(1.0, sqrt(dist)), 8.0, 600.0);
            float nrm     = (specPow + 8.0) / (PI * 8.0);
            surfColor    += pow(max(0.0, dot(reflect(dir, waveN), sunDir)), specPow) * nrm * directSun;

            // Moon glint on ocean — nighttime only, dims with phase (new moon = brightest Earth).
            if (moonDirENU.z > limbZ && moonDirENU.w > 0.01) {
                vec3  moonDir3o = normalize(moonDirENU.xyz);
                float mSpecPow  = 120.0;
                float mNrm      = (mSpecPow + 8.0) / (PI * 8.0);
                surfColor += pow(max(0.0, dot(reflect(dir, waveN), moonDir3o)), mSpecPow)
                           * mNrm * moonDirENU.w * clamp(moonDirENU.z, 0.0, 1.0)
                           * 0.006 * (1.0 - dayFrac);
            }

            // Aurora ground-glow: soft ambient tint from the curtain overhead, evaluated LOCALLY at
            // this ocean point (auroraGlowAt — same function terrain uses) rather than a single
            // observer-position proxy, distance-attenuated by the same `atten` the wave-crest
            // shading above uses so it doesn't glow uniformly out to the horizon.
            //
            // surfUp is the observer-local "up" (same frame as hitPt/obsPos/dir) — auroraGlowAt
            // needs a TRUE ECEF direction instead (see the terrain block's own comment on this same
            // bug), so it goes through enuX/enuY/enuZ first rather than being passed straight in.
#ifndef SKY_LITE   // Planetarium-tier: aurora glow on the ocean surface cut (matches the terrain-hit cut above)
            vec3 surfUpECEF = normalize(surfUp.x * enuX + surfUp.y * enuY + surfUp.z * enuZ);
            // Same cloud gate as the terrain ground-glow and the aurora reflection above — this
            // is why the water kept turning aurora-green straight through an overcast sky.
            float auroraGroundCloudOcclOcean = dot(cloudB.rgb, vec3(1.0 / 3.0));
            surfColor += auroraGlowAt(surfUpECEF, sunDirECEF, pc.waveTime, cloud.stormStrength)
                       * cloud.auroraGroundGain * 0.5 * atten * auroraGroundCloudOcclOcean;
#endif
            // Mirror satellite flare glints — own small independent capped atomic-append list
            // (flare architecture overhaul), decoupled from the deleted per-pixel corona system.
            // Now also occlusion-aware (previously had NONE at all): sampled at each entry's own
            // screen position, the same technique already proven this session for the corona loop.
#ifndef SKY_ENV
            {
                uint fCount = min(oceanGlintBuf.oceanGlintCount, kOceanGlintMax);
                float tanHFg = tan(pc.fovYRad * 0.5);
                for (uint fi = 0u; fi < fCount; ++fi) {
                    float flux = oceanGlintBuf.oceanGlintEntries[fi].w;
                    // Floor and gain are the Ocean tab's "Flare refl floor" / "Ocean flare refl"
                    // (CloudParams UBO, 2026-09-26). Both default to the constants that were
                    // hardcoded here before then: 2.0 was this cutoff, and a gain of 1.0 is the old
                    // look. Raising the floor is the surgical control for "a mild satellite lights
                    // up the water" — the entry is dropped before any per-pixel work, where the gain
                    // dims the spectacular glints by the same factor.
                    if (flux < cloud.oceanGlintMinFlux) continue;
                    vec3 fe = normalize(oceanGlintBuf.oceanGlintEntries[fi].xyz);
                    if (fe.z < limbZ - 0.02) continue;
                    vec3 feCam = mat3(pc.skyView) * fe;
                    if (feCam.z >= -0.01) continue;
                    vec2 feUV = vec2(feCam.x, -feCam.y) / (-feCam.z * tanHFg * 2.0);
                    vec2 feScreenUV = vec2(feUV.x / pc.aspect + 0.5, feUV.y + 0.5);
                    float feCloudOccl = dot(texture(cloudTargetB, feScreenUV).rgb, vec3(1.0 / 3.0));
                    float feTerrainOccl = (texture(sceneDepthTex, feScreenUV).r >= kNoSurfaceT * 0.5) ? 1.0 : 0.0;
                    float fSpecPow = 80.0;
                    float fNrm     = (fSpecPow + 8.0) / (PI * 8.0);
                    float fIntens  = clamp(log2(max(flux, 1.0)) / 10.0, 0.0, 1.0);
                    surfColor += pow(max(0.0, dot(reflect(dir, waveN), fe)), fSpecPow)
                               * fNrm * fIntens * 0.008 * vec3(1.2, 1.1, 1.0) * (1.0 - dayFrac) * altFade
                               * feCloudOccl * feTerrainOccl * cloud.oceanGlintGain;
                }
            }
#endif
        }

        // ── Reflect-Orbital beam ground-spot (C12) ──────────────────────────────────────────
        // Applies uniformly to whichever branch above produced surfColor (terrain or ocean) —
        // deliberately placed after both, not inside either. Physically a different quantity
        // from cloud_march.comp's volumetric in-scatter term above the surface: this is direct
        // irradiance landing ON the ground, so unlike the volumetric term it does NOT get
        // dimmer in fully clear air — the shadow lookup below only accounts for intervening
        // cloud, not "is there scattering medium to see the beam in" (there's no beam here to
        // see, just a lit patch of ground).
        // (Kept in SKY_LITE — the ground-spot loop is bounded by groundBeamCount and measured
        // cheap; beams are a wanted feature and run fine at the Planetarium tier.)
#ifndef SKY_ENV   // the ground-beam list is in the MAIN observer's ENU frame
        if ((cloud.dbgDisableMask & 128u) == 0u) {
            const float kBeamGroundScale = 4e-8;
            // Normalized against the slider's default (0.05, see SatelliteSim.h) so existing
            // footprint brightness is unchanged at default gain, while still scaling together
            // with cloud_march.comp's sky glow (C12 follow-up #18) — one shared control instead
            // of two independently-tuned pieces, so raising/lowering it visually reads as one
            // continuous beam rather than a mismatched ground patch under an unrelated sky ray.
            float skyGlowNorm = cloud.beamSkyGlowGain / 0.05;
            // Site-referenced (C12 follow-up #5): beams are now written unconditionally by any
            // satellite above the OBSERVER's own orbital horizon, not gated by the ground
            // target's local horizon — so cloud.beamMaxRangeM (settings-tunable, follow-up #6) is
            // the render-time "is the observer close enough to this site" cutoff. Perf follow-up:
            // that cutoff is now applied ONCE, CPU-side, when GroundBeamsBuf is built each frame
            // (see its declaration above) rather than redone here per ground-hit pixel against
            // the full raw list — this loop's trip count is the real cost, not the comparison.
            // 2026-08-10: the loop body is now almost entirely per-pixel work. Everything that
            // did not vary across the screen — the range fade keyed to the chosen target's site,
            // the 5-degree elevation fade, the per-beam cloud shadow attenuation, and the
            // obsPos+satENU / raySphere solve for the beam's REAL ray/ground intersection (two
            // sqrts) — was hoisted to the CPU, which already visits these entries once per frame
            // when it builds GroundBeamsBuf. That loop measured 1.59 ms of this shader at Medium in
            // the Anchorage worst-case sweep, on a list sitting at its full GROUND_BEAM_MAX cap, so
            // every ground-hit pixel paid all of it 256 times.
            //
            // The squared-distance reject is now FIRST rather than last: a pixel nowhere near a
            // landing spot costs one 2D subtract, one dot and one compare per beam, and the two
            // Gaussians (the only genuinely per-pixel maths left) are reached only by pixels that
            // are actually inside a footprint. Working in squared distance also drops the
            // length() sqrt the old reject needed.
            int activeBeamCount = int(min(groundBeamCount, GROUND_BEAM_MAX));
            for (int bi = 0; bi < activeBeamCount; ++bi) {
                vec2  d  = hitPt.xy - groundBeams[bi].groundHitXY;
                float d2 = dot(d, d);
                if (d2 > groundBeams[bi].cutoffSq) continue;
                float w = groundBeams[bi].weight;
                if (w <= 0.0) continue;

                // The spot is the Sun's limb-darkened DISK seen in the mirror (radius = the footprint,
                // edge blurred over the mirror's width), with the same total energy as the Gaussian of
                // sigma = footprint (plus a mirror-sized hotspot) it replaces (2026-09-28): that drew a
                // glow ~4x the real spot, cut off hard at 4 sigma — the blobby, sharp-edged beams.
                float rho2 = d2 * groundBeams[bi].invFootprintSq;
                // Pass 13: the soft dome of cloud_v2_march.comp's beamDisk (same energy): the 2%-edged
                // disk read as huge, hard-edged spotlights.
                float disk = (1.0 - smoothstep(0.25, 1.15, sqrt(rho2))) * 1.885;

                surfColor += vec3(kBeamGroundScale * w * 2.0 * disk * skyGlowNorm);
            }
        }
#endif

        color += surfColor * surfAttn;
    }

    // Sky-only snapshot for evalCloudLayer's aerial-perspective term below — everything folded
    // into `color` up to this point is atmosphere/sky (Rayleigh/Mie inscatter, city/beam glow,
    // airglow, moon disc/corona) with no ground/terrain/ocean surface light yet, since that was
    // just added immediately above. See evalCloudLayer's own comment for why this needs to be
    // kept separate from the ground-inclusive `color`.
    vec3 skyOnlyColor = color;

    // ── Cloud layers (C3/C4 unified: thin-shell 2D overlays) ─────────────────
    // Layers 0/1 double as the volumetric shell's base/top (same shellAltM values cloudMarch
    // reads below), so their flat paste here must crossfade against cloudMarch's own fade using
    // the SAME kCloud3DFadeStart/End band — not an independent threshold — or the two renders
    // overlap. Layers 2/3 (e.g. a standalone high cirrus deck) are always flat, at full weight.
    //
    // Iterate HIGH INDEX -> LOW INDEX: layers are conventionally ordered by increasing altitude
    // (layer0 = low/near, layer1 = cirrus/far, and any future layer2/3 should follow the same
    // convention). evalCloudLayer composites each call ON TOP of whatever `color` already holds,
    // so the farthest-from-a-ground-observer shell must be drawn FIRST (as background) and the
    // nearest drawn LAST (on top) for correct back-to-front compositing — ascending-index order
    // had this backwards (cirrus drew over the low deck regardless of which was actually nearer).
#ifdef SKY_LITE
    for (int li = 1; li >= 0; --li) {   // Planetarium-tier: low/mid deck only, drop cirrus + high layers
#else
    for (int li = 3; li >= 0; --li) {
#endif
        if (cloud.layers[li].enabled < 0.5) continue;
#ifndef SKY_ENV
        // The volumetric clouds draw the low/mid clouds at every distance, so no flat stand-in —
        // unless their march is knocked out (Planetarium/Potato), which leaves the flat layer.
        if (li == 0 && (cloud.dbgDisableMask & 32768u) == 0u) continue;
#endif
        // The 3D->2D weight used to be computed here from observer altitude alone — one value for
        // the entire screen. It now lives inside evalCloudLayer, which knows this ray's own
        // distance to the shell; see the note there.
#ifdef SKY_ENV
        float volumetricPair = 0.0; // no volumetric pass for this viewpoint: every layer flat, full weight
#else
        float volumetricPair = (li < 2) ? 1.0 : 0.0;
#endif
        evalCloudLayer(
            obsPos, dir, tSurface, enuX, enuY, enuZ, sunDirECEF,
            odR_cam, odM_cam,
            // flatCoverageScale / flatSunGainScale calibrate the shared sliders onto this path.
            // Without them one set of values could only ever suit the volumetric OR the flat
            // layer, never both — which is what made the 3D->2D crossfade untunable and forced
            // kCloud3DFadeStart out to 800 km to hide the mismatch.
            cloud.coverage * cloud.layers[li].coverageMult * cloud.flatCoverageScale,
            // flatDensityScale decouples the flat layer's opacity from the volumetric one. They
            // reach opacity by completely different routes — the volumetric accumulates
            // transmittance over many samples, so lowering `density` to soften its shading
            // necessarily thins it; the flat layer multiplies once, so the same value drops
            // straight out as translucency. One shared slider could only ever satisfy one of them.
            cloud.density  * cloud.layers[li].densityMult * cloud.flatDensityScale,
            cloud.sunGain      * cloud.flatSunGainScale,
            cloud.sunGainZenith * cloud.flatSunGainScale,
            cloud.layers[li].shellAltM,
            cloud.layers[li].driftMult,
            cloud.layers[li].alphaMax,
            cloud.layers[li].mipLod,
            cloud.cloudPhase,
            obsEffH, volumetricPair,
            skyOnlyColor,
            color);
    }

    // ── Half-resolution cloud composite (C15-perf) ───────────────────────────────
    // cirrusMarch/cloudMarch ran in shaders/cloud_march.comp at half resolution; sample the
    // precomputed result here instead of marching per full-res pixel. Target A: rgb=B_total
    // (combined additive radiance), a=tCloudOcclude (m, -1=none, only set when the cloud is
    // ≥90% opaque — used below for satellite/star depth occlusion, NOT for terrain suppression).
    // Target B: rgb=A_total (combined multiplicative attenuation), a=per-pixel cloud shadow
    // (currently 1.0 — the channel was freed by deleting tEnterCombined and is claimed in the
    // next step).
    // Perf (session 29, resolution-scaling fix): was `gl_FragCoord.xy / (textureSize(
    // cloudTargetA,0)*2.0)`, silently assuming gl_FragCoord always spans the full swap extent —
    // true before renderScale existed, false once sat_sky.frag can render into a SMALLER low-res
    // framebuffer (recordPrePass) while cloudTargetA/B stay sized off the TRUE swap extent
    // (cloud_march.comp's own dispatch is unaffected by renderScale). Dividing by the wrong,
    // larger denominator compressed cloudUV into a shrinking corner of [0,1] as renderScale
    // dropped — reported as clouds drifting off-center and distorting. the sky render-target size (cloud.skyScreenW/H) is always
    // this draw's OWN actual target size, so this now maps to [0,1] correctly regardless of scale.
    // cloudUV/cloudA/cloudB/tCloudOcclude were sampled earlier (right after tSurface), so the
    // moon disc above can be occluded by opaque cloud too — not resampled here.
    // cloudBlock (post-tonemap sun-disc dimming, used below) derived from A_total's luminance
    // rather than a separate stored scalar — A_total already tracks combined opacity closely
    // (→0 when opaque, →1 when clear).
    float cloudBlock    = dot(cloudB.rgb, vec3(1.0/3.0));
    // No terrain-suppression test any more. cloud_march.comp now clamps every layer (cloud,
    // cirrus, aurora, airglow-red) and every beam to the shared scene depth at march time, so
    // whatever reached this composite is already correctly occluded — per layer, and with real
    // partial truncation where a ridge pokes into a shell, which the old single-scalar gate
    // could not express at all.
    // Aurora rides along inside cloudA.rgb (B_total) now — folded in by cloud_march.comp's
    // auroraMarchCS, with its own cloud-suppression already applied there using the local cloud
    // opacity. No separate aurora term needed here; it is terrain-occluded at march time along
    // with everything else in the composite.
    color = color * cloudB.rgb + cloudA.rgb;
#ifdef SKY_ENV
    // From orbit the curtains are in front of every cloud deck: added on top.
    color += envAurora(obsPos, dir, tSurface, enuX, enuY, enuZ, sunDirECEF);
#endif

    // ── Auto-exposure tone mapping ─────────────────────────────────────────────
    float dayness  = clamp((sunDirENU.w + 0.2) / 1.2, 0.0, 1.0);
    float exposure = mix(EXPOSURE_NIGHT, EXPOSURE_DAY, pow(dayness, 0.4));
#ifndef SKY_ENV
    exposure *= cloud.exposureScale;              // "Exposure (EV)"; SatelliteSim::skyExposure() mirrors it
#endif
#ifdef SKY_ENV
    // HDR out: keep the radiance; the display-space terms below accumulate separately and are
    // divided back by the exposure they were tuned against (linear at their small values).
    vec3 envHdr = color;
    color = vec3(0.0);
    float nightAmt = 1.0 - clamp(dayness * 5.0, 0.0, 1.0);
#else
    // "Highlight roll-off" blends toward 1 - 1/(1 + x + x^2/2): equal to 1 - exp(-x) to second
    // order (same toe and midtones) but with a long shoulder — at x = 4 it is 0.92 where the old
    // curve is 0.98 — so sunlit cloud and the sky around the Sun keep their gradations.
#ifndef SKY_ENV
    // "White balance": chromatic adaptation to the sunlight arriving at the observer (the Chapman
    // column, as the clouds are lit). A 15-degree Sun is cream-yellow and everything it lights read
    // as beige; an eye or a camera adapts to its illuminant. Luminance-preserving, faded out as the
    // Sun sets (twilight keeps its colour).
    if (cloud.whiteBalance > 0.0) {
        float rW = R_EARTH + max(obsEffH, 0.0);
        float cW = sunDirENU.z;
        vec3  sunW = exp(-(BETA_R * (atmColumnInf(rW, cW, H_R) * H_R)
                          + BETA_M * 1.1 * (atmColumnInf(rW, cW, H_M) * H_M)));
        vec3  wb = sunW / max(dot(sunW, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
        // Backed off near the horizon (from 11.5 deg down to 1 deg): at a grazing Sun its colour at the
        // eye swings with a kilometre of altitude, and dividing the frame by it (up to x5) flipped the
        // same clouds from gold to white as the observer climbed 1 km near sunset (user snaps 9-10,
        // pass 13). The low Sun's colour is the look; adaptation is for a Sun well up.
        float k  = cloud.whiteBalance * smoothstep(0.02, 0.2, cW);
        color /= mix(vec3(1.0), clamp(wb, vec3(0.4), vec3(2.5)), k);
    }
#endif
    {
        vec3 xe = exposure * color;
        color = mix(vec3(1.0) - exp(-xe), vec3(1.0) - 1.0 / (vec3(1.0) + xe + 0.5 * xe * xe),
                    cloud.highlightRolloff);
    }

    // ── Night ambient floor ────────────────────────────────────────────────────
    float nightAmt = 1.0 - clamp(dayness * 5.0, 0.0, 1.0);
    color += vec3(0.0008, 0.001, 0.002) * nightAmt;
#endif

    // ── Milky Way skybox ───────────────────────────────────────────────────────
    // Diffuse galactic-plane glow behind the discrete star catalog (star_point.vert/frag).
    // Visible only from truly dark sites or space, using the same gating shape as CPU's
    // updateStars(): sun-elevation/space detection, moonlight, directional light-pollution dome,
    // and atmospheric extinction. Added post-tonemap like the ambient terms around it (comparably
    // faint) rather than folded into the HDR atmosphere accumulation above.
    //
    // SKY_LITE (Planetarium-tier): cut entirely — the equirect projection's atan2/asin plus the
    // panorama texture fetch are disproportionately expensive on weak GPUs (this is the exact term
    // that sank the Potato experiment). The discrete star catalogue still renders.
#ifndef SKY_LITE
    {
        // Space detection: mirrors CPU's updateStars()/atmFrac — linear fade over the last
        // stretch of the simulated atmosphere shell (40-100km, R_ATMOS-R_EARTH=100km) rather
        // than an 80km-scale-height exponential, which decayed too fast and leaked Milky Way
        // brightness into a clear daytime sky by cloud-deck altitude. Keep in sync with that copy.
        const float kMWSpaceFadeStartM = 40000.0;
        const float kMWSpaceFadeEndM   = 100000.0;
        float atmFracSky = 1.0 - clamp((obsEffH - kMWSpaceFadeStartM)
                                        / (kMWSpaceFadeEndM - kMWSpaceFadeStartM), 0.0, 1.0);
        float nightFactorSky = clamp(-sunDirENU.w * 5.0, 0.0, 1.0);
        // cloud.skyGlareVisibility (CPU-eased sun-glare gate) replaces
        // the old flat 1.0 space target — matches the same replacement in CPU's updateStars().
#ifdef SKY_ENV
        float nightFactorEffSky = mix(envGlareVis, nightFactorSky, atmFracSky); // the viewer's glare
#else
        float nightFactorEffSky = mix(cloud.skyGlareVisibility, nightFactorSky, atmFracSky);
#endif

        // Moonlight suppression — same shape as CPU's moonBrightStar.
        float tm = clamp(moonDirENU.z / 0.5, 0.0, 1.0);
        float moonBrightSky = tm * tm * moonDirENU.w;
        const float kMWMoonMaxDim = 0.95;

        // Directional dome geometry — sector lookup shared with beamDomeVal below and with the
        // dark-sky sky-background estimate. Interpolated between the two nearest sector CENTERS,
        // same as every other consumer of this dome.
        float azLP    = mod(atan(dir.x, dir.y) + 6.283185307, 6.283185307);
        float secF    = azLP * (16.0 / 6.283185307) - 0.5;
        int   sec0    = int(floor(secF));
        float secFrac = secF - float(sec0);
        int   sec0w   = ((sec0 % 16) + 16) % 16;
        int   sec1w   = (sec0w + 1) % 16;
        float elevFalloffMW = 0.35 / (max(dir.z, 0.0) + 0.35);

        // Sky background surface brightness for THIS view direction (mag/arcsec^2). Replaces
        // cloud.mwSuppressEased, a single non-directional scalar derived from a MAX over all 16
        // sectors — so a city on one horizon suppressed the Milky Way everywhere, including the
        // darkest part of the sky opposite it. See darksky.glsl for the model.
#ifdef SKY_ENV
        float mwSkyBgMag = kSkyMagPristine; // the pollution dome is the main observer's
#else
        float mwSkyBgMag = darkSkySkyMag(dir, sunDirENU);
#endif

        // C12 follow-up #31: same suppression shape, second independent source — a nearby
        // Reflect-Orbital beam should wash out the Milky Way the same way real light pollution
        // does. beamGlowDome[] holds raw atomicMax'd uint bit-patterns (floatBitsToUint on the
        // write side in sat_orbit.comp) — reinterpret via uintBitsToFloat, unlike lightDome[].
        float beamDomeAz = mix(uintBitsToFloat(beamGlowDome[sec0w]), uintBitsToFloat(beamGlowDome[sec1w]), secFrac);
#ifdef SKY_ENV
        float beamDomeVal = 0.0 * beamDomeAz * elevFalloffMW;
#else
        float beamDomeVal = clamp(beamDomeAz * elevFalloffMW, 0.0, 1.0);
#endif
        const float kMWBeamPollutionMaxDim = 0.99;

        // Atmospheric extinction along THIS ray (atmosphere.glsl) — the same line-of-sight column
        // sat_flare.comp and updateStars() use, so the Milky Way, stars and satellites in one
        // direction dim alike. Deliberately NOT atmFracSky, which is about the bright sky AROUND
        // the observer, not the air this ray crosses.
        float extinctionMW = pow(10.0, -0.4 * atmExtinctionMag(obsPos, dir, 0.0, cloud.extinctionCoeff));

        // Sun glare: even in space (where nightFactorEffSky doesn't suppress anything — there's
        // no atmosphere to scatter sunlight into a uniform "day" sky), staring straight at the
        // sun should still wash out the Milky Way — real eye/camera dazzle, not atmospheric
        // scattering, so this is unconditional rather than atmFracSky-gated. On the ground it's
        // mostly redundant with nightFactorEffSky already zeroing everything once the sun is up,
        // but it also correctly dims the Milky Way near the sun during twilight, when the rest of
        // the sky is still dark enough to show it.
        // Gated on the sun actually being above the spherical horizon (same limbZ test the sun
        // disc's own visibility uses below) — a pure angle-to-sunDir test has no notion of what's
        // along that line of sight, so without this it kept dimming the Milky Way toward the
        // sunset point (or, from orbit, toward wherever the Earth hides the sun) long after the
        // sun itself was fully Earth-occluded and no real glare could exist.
        float sunAngleMW = acos(clamp(dot(dir, sunDir), -1.0, 1.0));
        // In a reflection too: the sky next to the Sun's reflected image dims as it does next to the Sun.
        float sunGlareSuppress = (sunDirENU.w > limbZ) ? smoothstep(0.12, 0.5, sunAngleMW) : 1.0; // 0 within ~7deg, 1 beyond ~29deg or sun occluded

        // Project the view ray into the galactic frame and sample the panorama.
#ifdef SKY_ENV
        vec3 dirMw = envMainEnuDir(dir, enuX, enuY, enuZ);
#else
        vec3 dirMw = dir;
#endif
        vec3 dirGal = vec3(dot(dirMw, cloud.mwBasisRow0.xyz),
                            dot(dirMw, cloud.mwBasisRow1.xyz),
                            dot(dirMw, cloud.mwBasisRow2.xyz));
        float lonGal = atan(dirGal.y, dirGal.x);
        float latGal = asin(clamp(dirGal.z, -1.0, 1.0));
        vec2  mwUV   = vec2(0.5 + lonGal / (2.0 * PI), 0.5 + latGal / PI);
        vec3  mwColor = texture(milkyWayTex, mwUV).rgb * cloud.mwBasisRow0.w;

        // Cloud suppression: CUBED, not linear — same reasoning as the aurora's auroraCloudSuppress
        // above (session 28 follow-up #11). A plain `* cloudBlock` still let the Milky Way show
        // clearly through cloud that reads as visually solid, since a "mostly opaque" transmittance
        // of e.g. 0.25 only cuts brightness to a quarter. Reuses the same opacity scalar the sun
        // disc is already dimmed by (see "Sun/moon disc" above); this term is added post-tonemap
        // (deliberately, see comment above) so it can't be folded into the HDR cloud composite the
        // same way aurora/atmosphere are, but a steeper power curve on the same continuous value
        // works without needing that.
        const float kMWCloudSuppressPower = 3.0;

        // ── Dark-sky exposure gate (per-texel, not a uniform dim) ─────────────────────────────
        // The single most important property here: objMag is derived from THIS TEXEL's own
        // luminance, so as the sky brightens the faint outer arms drop out while the galactic core
        // hangs on. That is the real rural->suburban transition, and it is exactly what the
        // previous whole-texture scalar multiply could not produce at any tuning — a scalar can
        // only make the entire Milky Way uniformly dimmer, never thinner.
        //
        // The surface-brightness anchors live at file scope (search "Milky Way surface-brightness
        // anchors") along with their calibration, because the ocean sky-reflection further down
        // gates the same panorama on the same curve — two copies would be free to drift, and the
        // symptom (a reflection fading at a different sky brightness than the sky it reflects)
        // would be subtle.
        float mwLum = dot(mwColor, vec3(0.2126, 0.7152, 0.0722));
        float mwObjMag = darkSkySurfMag(mwLum, kMwRefLum, kMwRefMag, kMwMagSpread);

        float visibility = nightFactorEffSky
                          * darkSkyVis(mwObjMag, mwSkyBgMag)
                          * (1.0 - beamDomeVal * kMWBeamPollutionMaxDim)
                          * (1.0 - moonBrightSky * kMWMoonMaxDim)
                          * extinctionMW
                          * sunGlareSuppress
                          * (tSurface > 0.0 ? 0.0 : 1.0) // blocked by terrain/ocean
                          * (moonDiscHit ? 0.0 : 1.0)    // blocked by the Moon's own opaque disc
                          * pow(clamp(cloudBlock, 0.0, 1.0), kMWCloudSuppressPower);
#ifndef SKY_ENV
        color += mwColor * visibility * cloud.exposureScale;   // the global exposure (SatelliteSim.h)
#else
        color += mwColor * visibility;
#endif
#ifdef SKY_ENV
        // The stars, under the same gates as the Milky Way bar the dark-sky one (the main view's stars
        // are the point model's alone). A reflection's pixels are the screen's; elsewhere this
        // render's own pixel angle.
#ifdef SKY_REFL
        float starPAng = starHdr1.x;
#else
        float starPAng = length(fwidth(dir)) * 0.70710678;
#endif
        float starVis = nightFactorEffSky * extinctionMW * sunGlareSuppress
                      * (1.0 - moonBrightSky * kMWMoonMaxDim)
                      * (tSurface > 0.0 ? 0.0 : 1.0) * (moonDiscHit ? 0.0 : 1.0)
                      * pow(clamp(cloudBlock, 0.0, 1.0), kMWCloudSuppressPower);
        if (starVis > 0.0)
            color += envStars(dir.x * enuX + dir.y * enuY + dir.z * enuZ, starPAng) * starVis;
#endif
    }
#endif

    // ── Zodiacal light ─────────────────────────────────────────────────────────
    // SKY_LITE (Planetarium-tier): cut, same as the Milky Way block above. Cheaper than that one
    // (no panorama fetch, no equirect projection) but not free — two acos, an asin, an atan for
    // the pollution-dome lookup, two exps and two pows per pixel, plus ~100 lines of added
    // fragment code. SKY_LITE exists because this shader's compiled SIZE collapses wavefront
    // occupancy on GCN1-class parts, not only because of what executes, so new post-tonemap
    // sky terms default to being cut there. Flip this to render zodiacal light at that tier.
#ifndef SKY_LITE
    // Sunlight scattered by interplanetary dust in the ecliptic plane — a faint, warm-white
    // diffuse cone brightest a few degrees beyond the sun's own corona, fading with elongation,
    // plus a much fainter "gegenschein" patch directly opposite the sun. Pure analytic falloff,
    // no texture: elongation from the sun needs only sunDir (already in scope); ecliptic latitude
    // needs the one new CPU-computed basis vector, cloud.eclipticPoleENU (see updatePositions()
    // and cloud_params.glsl). Added post-tonemap for the same reason the Milky Way/sun disc/moon
    // corona are — a faint additive term should read at a consistent brightness regardless of
    // whatever exposure the raw HDR atmosphere integral needed that frame, not get compressed or
    // blown out differently by EXPOSURE_DAY/EXPOSURE_NIGHT depending on sky brightness that frame.
    // Deliberately does NOT reuse the Milky Way's sunGlareSuppress above — that exists specifically
    // to dim things NEAR the sun, which is exactly where zodiacal light is brightest; innerFade
    // below handles the seam against the corona for an unrelated reason (avoiding a double-bright
    // ring, not glare). Locals below duplicate the Milky Way block's day/night/moon/extinction/
    // dome shapes rather than reaching across the closed `{}` above — same per-block-local
    // convention this file already uses everywhere else.
    {
        float theta = acos(clamp(dot(dir, sunDir), -1.0, 1.0));
#ifdef SKY_ENV
        float beta  = asin(clamp(dot(envMainEnuDir(dir, enuX, enuY, enuZ), cloud.eclipticPoleENU.xyz), -1.0, 1.0));
#else
        float beta  = asin(clamp(dot(dir, cloud.eclipticPoleENU.xyz), -1.0, 1.0));
#endif

        // Main cone: innerFade clears the sun corona's own falloff (coronaSig maxes at ~0.08 rad
        // above), outerFade closes it out by cloud.zodiacalOuterFadeDeg. The ecliptic-latitude
        // sigma narrows with elongation — wide and low near the sun/horizon, narrowing further
        // out — the real cone shape.
        float innerFade = smoothstep(0.09, 0.17, theta);
        float outerR1   = radians(cloud.zodiacalOuterFadeDeg);
        float outerR0   = outerR1 * 0.75;
        float outerFade = 1.0 - smoothstep(outerR0, outerR1, theta);
        float sigmaNear = radians(cloud.zodiacalWidthDeg);
        float sigmaFar  = sigmaNear * 0.45;
        float sigmaLat  = mix(sigmaNear, sigmaFar, smoothstep(0.0, 1.2, theta));
        float latFalloff = exp(-(beta * beta) / (2.0 * sigmaLat * sigmaLat));
        float zodMain = innerFade * outerFade * latFalloff;

        // Gegenschein: same shape mirrored around the antisolar direction, tight and dim, no
        // separate slider — real zodiacal light's opposition brightening is subtle.
        float thetaAnti = acos(clamp(dot(dir, -sunDir), -1.0, 1.0));
        float outerFadeAnti  = 1.0 - smoothstep(radians(15.0), radians(25.0), thetaAnti);
        float sigmaAnti       = sigmaNear * 0.6;
        float latFalloffAnti  = exp(-(beta * beta) / (2.0 * sigmaAnti * sigmaAnti));
        const float kZodGegenscheinRatio = 0.07;
        float gegenschein = outerFadeAnti * latFalloffAnti * kZodGegenscheinRatio;
        float zodShape = zodMain + gegenschein;

        // Color: pale warm-white, far less saturated than the sun disc/corona (vec3(1.8,0.7,0.2)
        // above) — a faint wash, not a second sun. Mirrors the sun disc's own sunsetT shape for a
        // consistent warm shift near the horizon; the gegenschein (night-side) stays unwarmed.
        float sunsetTZod = clamp(1.0 - (sunDirENU.w - limbZ) / 0.15, 0.0, 1.0);
        vec3  zodColMain = mix(vec3(1.00, 0.98, 0.92), vec3(1.05, 0.88, 0.68), sunsetTZod * 0.5);
        vec3  zodColAnti = vec3(1.0, 0.98, 0.95);
        vec3  zodCol = (zodMain * zodColMain + gegenschein * zodColAnti) / max(zodShape, 1e-4);

        // Visibility chain — same day/night, moonlight, terrain/cloud/moon-disc occlusion shape
        // the Milky Way uses above, minus sunGlareSuppress (see header comment). Pollution now
        // goes through the SAME dark-sky exposure gate the Milky Way does (darksky.glsl), reading
        // the same eased dome half: the two features are lit by the same sky, so they should be
        // gated by one model. Their different real-world visibility falls out of their different
        // surface-brightness anchors below, not out of separately-tuned max-dim ceilings.
        float atmFracSkyZ    = 1.0 - clamp((obsEffH - 40000.0) / (100000.0 - 40000.0), 0.0, 1.0);
        float nightFactorSkyZ = clamp(-sunDirENU.w * 5.0, 0.0, 1.0);
        // cloud.skyGlareVisibility, not pc.: this field moved into the CloudParams UBO when
        // SatDrawPC was trimmed to the 128-byte maxPushConstantsSize floor. The Milky Way
        // block above reads it the same way.
#ifdef SKY_ENV
        float nightFactorEffZ = mix(envGlareVis, nightFactorSkyZ, atmFracSkyZ);
#else
        float nightFactorEffZ = mix(cloud.skyGlareVisibility, nightFactorSkyZ, atmFracSkyZ);
#endif

        float tmZ = clamp(moonDirENU.z / 0.5, 0.0, 1.0);
        float moonBrightZ = tmZ * tmZ * moonDirENU.w;
        const float kZodMoonMaxDim = 0.9;

        // This ray's own line-of-sight extinction (atmosphere.glsl), as for the Milky Way above.
        float extinctionZ = pow(10.0, -0.4 * atmExtinctionMag(obsPos, dir, 0.0, cloud.extinctionCoeff));

        // ── Dark-sky exposure gate ────────────────────────────────────────────────────────────
        // Same model as the Milky Way above (darksky.glsl), replacing the old
        // (1 - domeValZ * 0.85) linear dim with its own hand-picked ceiling. Per-sample here too:
        // zodShape falls off with elongation and ecliptic latitude, so the cone's bright inner
        // part survives a sky that its faint outer edge and the gegenschein do not — the cone
        // shrinks toward the sun as skies brighten instead of the whole wedge fading uniformly.
        //
        // Anchor: peak output is zodShape(<=1) * zodiacalGain (0.01 by default), so 0.01 is the
        // brightest this feature gets. Mapping that to 21.6 mag/arcsec^2 puts it just below the
        // Milky Way's core, matching reality — zodiacal light is comparable in surface brightness
        // to the Milky Way but is generally considered the harder of the two to see, and it
        // correspondingly drops out one step earlier on the Bortle ladder here. Same 0.85 spread
        // as the Milky Way, for the same reason (a tuned artistic falloff, not a radiometric one,
        // and half of the contrast-stretch control — see darksky.glsl).
#ifdef SKY_ENV
        float zodSkyBgMag = kSkyMagPristine;
#else
        float zodSkyBgMag = darkSkySkyMag(dir, sunDirENU);
#endif

        const float kZodRefLum    = 0.01;
        const float kZodRefMag    = 21.6;
        const float kZodMagSpread = 0.85;
        float zodLum = zodShape * cloud.eclipticPoleENU.w * dot(zodCol, vec3(0.2126, 0.7152, 0.0722));
        float zodObjMag = darkSkySurfMag(zodLum, kZodRefLum, kZodRefMag, kZodMagSpread);

        const float kZodCloudSuppressPower = 2.0;
        float visibilityZod = nightFactorEffZ
                             * (1.0 - moonBrightZ * kZodMoonMaxDim)
                             * extinctionZ
                             * (tSurface > 0.0 ? 0.0 : 1.0) // blocked by terrain/ocean
                             * (moonDiscHit ? 0.0 : 1.0)    // blocked by the Moon's own opaque disc
                             * pow(clamp(cloudBlock, 0.0, 1.0), kZodCloudSuppressPower)
                             * darkSkyVis(zodObjMag, zodSkyBgMag);

        color += zodCol * zodShape * cloud.eclipticPoleENU.w * visibilityZod * cloud.exposureScale;
    }
#endif

    // ── Moonlight ambient ──────────────────────────────────────────────────────
    float moonEl    = clamp(moonDirENU.z, 0.0, 1.0);
    float moonIllum = moonDirENU.w;
    // Atmosphere weight: glow and ambient fade to zero above the atmosphere.
    float atmosWeight = 1.0 - exp(-odR_cam / 5000.0);
    color += vec3(0.0025, 0.003, 0.004) * moonIllum * moonEl * nightAmt * atmosWeight;

    // ── Moon glow: tight corona + wide diffuse halo (atmosphere-only) ─────────
    if (moonDirENU.z > limbZ - 0.05) {
        vec3  moonDir3  = normalize(moonDirENU.xyz);
        float moonAngle = acos(clamp(dot(dir, moonDir3), -1.0, 1.0));
        float moonFade  = smoothstep(limbZ - 0.006, limbZ + 0.002, moonDirENU.z);

        // Tight inner corona — peaks at disc edge, falls off quickly.
        float corona = exp(-moonAngle * moonAngle / (2.0 * 0.012 * 0.012)) * nightAmt;
        color += hClip * moonFade * corona * vec3(0.92, 0.94, 1.00) * moonIllum * 0.04 * atmosWeight;

        // Wide diffuse halo — scattered moonlight glow, atmosphere-only.
        float scale = 100.0;
        float halo  = exp(-moonAngle * moonAngle / (2.0 * 0.018 * 0.018 * scale * scale));
        color += hClip * moonFade * halo * vec3(0.88, 0.90, 1.00) * moonIllum * 0.012 * atmosWeight;
    }

#ifdef SKY_ENV
    // No sun disc or lens flare (the mesh's GGX sun lobe is the glint) and no depth attachment. The
    // display-space terms go back to radiance through the VIEWER's exposure (header note).
    vec3 envOut = max(envHdr + color / envViewExposure, vec3(0.0));
#ifdef SKY_REFL
    outColor = vec4(envOut * reflWeight, 1.0);
#else
    outColor = vec4(envOut, 1.0);
#endif
#else
    // ── Sun disc + atmospheric corona ─────────────────────────────────────────
    if (sunDirENU.w > limbZ - 0.1) {
        float angle      = acos(clamp(cosA, -1.0, 1.0));
        const float kSunAngR = 0.00466; // solar angular radius (~0.267°)
        // Geometric fade: smooth transition as sun centre crosses the geometric limb.
        float geomFade   = smoothstep(limbZ - kSunAngR, limbZ + kSunAngR, sunDirENU.w);
        // Hard gate: terrain/ocean OR genuinely-opaque (>=90%) cloud on THIS fragment's own view
        // ray — same ray the sun disc/corona are drawn along, and the same tSurface/tCloudOcclude
        // pair the moon disc gates on (discFade above). Fixed 2026-07-29: previously `corona` (the
        // wide atmospheric halo, ~10-20x the disc's radius) had NO gate at all — only `discVis`
        // checked tSurface, and neither checked tCloudOcclude — so a mountain or a genuinely opaque
        // cloud deck correctly hid the disc but left its halo glowing right through. The only
        // attenuation either term got was `cloudBlock` (A_total's soft luminance dimming) below,
        // which fades but never reaches zero for real cloud, so it read as "doesn't occlude."
        float sunGate    = (tSurface > 0.0 || tCloudOcclude >= 0.0) ? 0.0 : 1.0;
        // Disc pixel: hard-clipped by terrain/ocean/opaque-cloud hit for this fragment direction.
        float discVis    = (1.0 - smoothstep(0.007, 0.010, angle)) * sunGate;
        // Sunset shift: redden and widen corona as sun approaches the limb.
        float sunsetT    = clamp(1.0 - (sunDirENU.w - limbZ) / 0.15, 0.0, 1.0);
        vec3  sunCol     = mix(vec3(1.5, 1.3, 1.0), vec3(1.8, 0.7, 0.2), sunsetT * 0.7);
        float coronaSig  = mix(0.035, 0.08, sunsetT * sunsetT);
        float corona     = exp(-angle * angle / (2.0 * coronaSig * coronaSig)) * sunGate;
        // Bright inner glare bridging the disc edge to the *0.12 wide corona. Without it the disc
        // (full `sunCol`) dropped straight to the dim corona at its edge — a hard brightness step
        // that reads, especially post-tonemap, as a dark ring / "cutout" around the sun. Three
        // stacked pow lobes peaking at ~disc brightness (sum ≈ 1.0), same shape as
        // sat_sky_minimal.frag's Potato corona so the two tiers match. `g = max(cosA,0)`;
        // pow(g,N) ≈ exp(-N·angle²/2), so N = 1800/320/55 → σ ≈ 0.024/0.056/0.135 rad.
        float g          = max(cosA, 0.0);
        float glare      = (pow(g, 1800.0) * 0.85 + pow(g, 320.0) * 0.13 + pow(g, 55.0) * 0.03) * sunGate;
        // Remaining soft dimming (cloudBlock, A_total's luminance) still applies on top for thin/
        // translucent cloud the hard gate above doesn't trip on — a hazy, dimmed disc through mist
        // is correct; sunGate only handles the "actually opaque" case that dimming alone can't.
        color += (discVis * geomFade * sunCol
                  + glare  * geomFade * sunCol
                  + corona * geomFade * sunCol * 0.12) * cloudBlock;
    }

    // ── Camera lens flares (post-tonemap) ─────────────────────────────────────
    // Applied after all physics-based rendering so they read as pure camera
    // optical artifacts on top of the scene.
    //
    // UV space: x in [-0.5*aspect, +0.5*aspect], y in [-0.5, +0.5].
    //
    // Fragment projection:
    //   fragCamDir = mat3(skyView) * enuDir  (camera-space ray, z ~= -1)
    //   fragUV = vec2(camDir.x, -camDir.y) * invTanHF2
    //   No perspective divide since z ~= -1 throughout the fullscreen tri.
    //
    // Source projection (satellite or sun):
    //   satCam = mat3(skyView) * normalize(enu)
    //   satUV  = vec2(satCam.x, -satCam.y) / (-satCam.z * tanHF * 2)
    //   Perspective divide by -satCam.z is required here.
    {
        float tanHF     = tan(pc.fovYRad * 0.5);
        float invTanHF2 = 1.0 / (tanHF * 2.0);

        vec3 fragCamDir = mat3(pc.skyView) * enuDir;
        vec2 fragUV     = vec2(fragCamDir.x, -fragCamDir.y) * invTanHF2;

        vec3 flareAccum = vec3(0.0);

        // (Satellite lens flares — the per-pixel loop over glowBuf.flareEntries — lived here.
        // Deleted in the flare architecture overhaul: satellites now get their soft glow+godray
        // treatment from a render-to-texture + blur/streak pipeline, composited separately in
        // flare_composite.frag. Only the sun keeps a hand-authored lensFlare() ghost/corona call,
        // per explicit user decision — see FlareSourcePC's comment in SatelliteSim.h.)

        // ── Sun lens flare ──────────────────────────────────────────────────────
        // Gate on limbZ (sin of geometric limb depression, already accounts for observer
        // altitude) so the flare correctly persists when the sun is visible past the
        // curved Earth from orbit — not just when sunDirENU.w > 0.
        if (pc.sunDirENU.w > limbZ - 0.05) {
            float above        = pc.sunDirENU.w - limbZ;
            float sunIntensity = 10.0 * clamp(above / 0.5, 0.0, 1.0);
            vec3 sunCam = mat3(pc.skyView) * normalize(pc.sunDirENU.xyz);
            if (sunCam.z < -0.01) {
                vec2 sunUV    = vec2(sunCam.x, -sunCam.y) / (-sunCam.z * tanHF * 2.0);
                float sunFade = clamp(above * 8.0, 0.0, 1.0);
                vec3  sunTint = vec3(1.4, 1.2, 0.9);
                // Same cloud-occlusion fix as the satellite flares above — sampled at the SUN's
                // own screen position, not this fragment's (`cloudBlock`, used for the sun disc
                // itself right above, is deliberately not reused here for the same reason).
                vec2  sunScreenUV  = vec2(sunUV.x / pc.aspect + 0.5, sunUV.y + 0.5);
                float sunCloudOccl = dot(texture(cloudTargetB, sunScreenUV).rgb, vec3(1.0/3.0));
                // Terrain occlusion, same technique/reasoning as the satellite loop above — a
                // local ridge or mountain blocking the sun's own direction (distinct from limbZ's
                // Earth-curvature horizon test above) now correctly hides its flare too.
                float sunTerrainOccl = (texture(sceneDepthTex, sunScreenUV).r >= kNoSurfaceT * 0.5) ? 1.0 : 0.0;
                flareAccum += lensFlare(fragUV, sunUV, sunIntensity, 2.0) * sunTint * sunFade * 0.45
                            * sunCloudOccl * sunTerrainOccl;
            }
        }

        // Lens flares are a screen-space camera-optics artifact, not light literally travelling
        // to each pixel — source visibility is already handled per-source above (sun: limbZ
        // gate at its `if`; satellites: satDir.z horizon cull + camera-facing check). Do NOT
        // gate flareAccum by this fragment's OWN terrain hit (tHit): that tests whether THIS
        // pixel's unrelated view ray hit land, not whether the source is occluded. The old
        // `tHit > 0.0 ? 0.0 : 1.0` mask zeroed the flare's additive glow on every terrain pixel
        // anywhere on screen — invisible at ground level (terrain only fills the lower frame),
        // but at LEO twilight, where terrain fills most of the screen under a large sun flare,
        // it hard-clipped the raymarched terrain silhouette out of the middle of the glow.
        color += flareAccum;
    }

    outColor = vec4(color, 1.0);
    if (terrainDebugActive) outColor = vec4(terrainDebugColor, 1.0);

    // Unified scene depth (include/depth.glsl) for the passes that follow: the TRUE distance to the
    // first opaque surface — terrain, else ocean (tSeaLvl covers ocean pixels with no terrain
    // hit), else opaque cloud (cloud is above terrain, so tCloudOcclude only applies when nothing
    // nearer was hit) — at any range; 1.0 for sky. Points write their own range, so a satellite in
    // front of the distant Earth seen from orbit passes and one behind a mountain does not.
    float tOcclude = (tHit >= 0.0) ? tHit : tSeaLvl;
    if (tOcclude < 0.0 && tCloudOcclude >= 0.0) tOcclude = tCloudOcclude;
    if (meshHit) tOcclude = (tOcclude >= 0.0) ? min(tOcclude, tMesh) : tMesh;
    gl_FragDepth = (tOcclude >= 0.0) ? sceneDepthFromDistance(tOcclude) : 1.0;
#endif
}
