// ── cloud_params.glsl ─────────────────────────────────────────────────────────────────────────
// The CloudParams uniform block, shared by every stage that reads it.
//
// This block used to be hand-copied into each consumer. It has grown from 176 to 336 bytes across
// sessions, and every growth meant editing each copy plus the C++ GpuCloudParams in lockstep — a
// silent, whole-buffer corruption if any one of them was missed, with no compiler to catch it.
// One definition removes that failure mode entirely.
//
// Consumers bind it at different indices, so define CLOUD_PARAMS_BINDING before including:
//     #define CLOUD_PARAMS_BINDING 4
//     #include "cloud_params.glsl"
//
// The C++ side (GpuCloudParams in src/simulations/SatelliteSim.h) still has to mirror this by
// hand — GLSL and C++ cannot share a declaration. THAT pairing is the one remaining place a
// mismatch can hide, so any field added here must be added there in the same change.
//
// Not every consumer reads every field. Fields are kept in full regardless, because the layout is
// what matters: dropping one a given shader does not use would shift every field after it.
#ifndef CLOUD_PARAMS_BINDING
#error "define CLOUD_PARAMS_BINDING before including cloud_params.glsl"
#endif

struct CloudLayer {
    float shellAltM;
    float driftMult;
    float alphaMax;
    float mipLod;
    float coverageMult;
    float densityMult;
    float enabled;
    float pad;
};

layout(set = 0, binding = CLOUD_PARAMS_BINDING) uniform CloudParams {
    float coverage;
    float density;
    float driftRate;
    float sunGain;
    float ambientGain;
    float hgG;
    float marchSteps;
    float lightSteps;
    float cloudPhase;
    float extinctionCoeff;
    float cirrusWindAngle;
    float cirrusStretch;
    float airglowGain;
    float airglowGreenGain;
    float airglowRedGain;
    float airglowSodiumGain;
    float shadowMaxDistM;
    float maxRenderDistM;
    float viewSamplesMin;
    float lightSamples;
    float oceanSeaOctaves;
    float oceanDetailOctaves;
    float oceanReflSamples;
    float viewSamplesMax;
    float sunGainZenith;
    float moonGain;           // shared moonlight brightness master — replaces this file's old
    float pad1;               // reserved
    float pad2;               // reserved
    vec4 mwBasisRow0;
    vec4 mwBasisRow1;
    vec4 mwBasisRow2;
    CloudLayer layers[4];
    float stormStrength;
    float auroraGain;
    float auroraCloudGain;
    float auroraGroundGain;
    float auroraCoverageFreq;
    float auroraCoverageAzFreq;
    float auroraCoverageDriftRate;
    float auroraShimmerRate;
    // Struct grew 320->336 (session 30, see GpuCloudParams in SatelliteSim.h for why this was
    // appended here instead of reusing pad1/pad2 above).
    float cloudTwilightAmbientGain;
    float cloudBaseVariance;
    float cloudErosionEdge;
    float cloudErosionCore;
    // Grew 336->352. sunGainElevBand: sin(sun elevation) at which sunGainZenith fully replaces
    // sunGain. Was a hardcoded smoothstep(0,1,...), i.e. only half-way to the zenith value at 30
    // degrees elevation, so a gain tuned for sunset stayed dominant most of the morning.
    float sunGainElevBand;
    // Edges of the twilight ambient bell, in sin(sun elevation) at the cloud sample.
    // Hi: above this the term is zero (raise to start it EARLIER, before the sun has set).
    // Lo: below this the term is zero (lower to carry it deeper into night).
    float twilightBandHi;
    float twilightBandLo;
    // Mip level the VOLUMETRIC march reads the 2D coverage map at. Was hardcoded 4.0, which on
    // the 8K source is ~78 km per texel — every small feature the flat 2D layer shows was already
    // filtered away before the density function saw it, so no coverage value could bring it back.
    float coverageMipLod;
    // Flat-2D-layer calibration. The volumetric and flat paths need DIFFERENT numbers to look the
    // same, because they are structurally different: the volumetric thresholds the coverage map
    // and then erodes it, applies a height profile and accumulates through transmittance, while
    // the flat layer thresholds the same map once and multiplies. Driving both from one set of
    // sliders is what made the 3D->2D crossfade impossible to match. These map the shared slider
    // values onto the flat layer's equivalent, measured against the volumetric at MIP 0.
    float flatCoverageScale;   // volumetric coverage 1.00 matched flat coverage 0.69
    float flatSunGainScale;    // volumetric sun gain 0.46 matched flat sun gain 1.84 (~4x)
    // fogTopAltM/fogDensity (C11, repurposed from pad10/pad11 — zero size change): the ground fog
    // shell (sea level to fogTopAltM) and its density scale. See fogMarchCS in cloud_march.comp.
    float fogTopAltM;
    float fogDensity;
    // Distance-based 3D->2D crossfade, replacing the altitude-only one as the effective control.
    // Keyed on the distance to the cloud shell along THIS ray, so it bounds the march directly —
    // unlike maxRenderDistM, which caps march LENGTH from the shell entry and therefore does
    // nothing from orbit (there the span is just the ~9 km shell crossing, already far under any
    // cap). Also gives a natural horizon transition from the ground, which altitude cannot.
    float cloudDistFadeStartM; // fully volumetric nearer than this
    float cloudDistFadeEndM;   // fully flat-2D beyond this
    // fogCoverage/fogSunGain (C11, repurposed from pad12/pad13 — zero size change): global
    // coverage gate for the ground fog shell's patchiness, and its own sun-lit brightness gain
    // (separate from cloud.sunGain per [[feedback_shared_gain_sliders]]). See fogMarchCS.
    float fogCoverage;
    float fogSunGain;
    // Terrain march distance fade (S4, RELEASE_v1_1_PLAN.md session 31): mirrors
    // cloudDistFadeStartM/EndM above but gates the per-pixel terrain-relief raymarch in
    // sat_sky.frag rather than a volumetric/flat crossfade. Keyed on the ray's own march reach
    // (tExit, which already grows with observer altitude via tCap) — grazing/high-altitude rays
    // fade OUT the march's step budget and, beyond terrainDistFadeEndM, skip it outright, falling
    // back to the already-computed tSeaLvl (a smooth textured Earth, not "nothing" — the same
    // result "terrain off" debug knockout already produces). Below terrainDistFadeStartM, full
    // detail — chosen so the old always-250km ground-level reach is entirely unaffected.
    float terrainDistFadeStartM;
    float terrainDistFadeEndM;
    float pad14; // reserved
    float pad15; // reserved
} cloud;
