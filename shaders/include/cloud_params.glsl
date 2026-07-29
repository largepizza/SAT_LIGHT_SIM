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
                           // now genuinely occupied there; kept named/positioned for layout parity
    float cirrusWindAngle;
    float cirrusStretch;
    float airglowGain;
    float airglowGreenGain;
    float airglowRedGain;
    float airglowSodiumGain;
    float shadowMaxDistM;
    float maxRenderDistM;
    float viewSamplesMin;
                              // sat_sky.frag) — kept for layout parity, was pad2
    float lightSamples;
    float oceanSeaOctaves;
                              // shader) — kept for layout parity
    float oceanDetailOctaves;
    float oceanReflSamples;
    float viewSamplesMax;
    float sunGainZenith;
                               // near zenith, blended against `sunGain` (now effectively the
                               // near-horizon/sunset value) by sun elevation. Was a single flat
                               // gain for all elevations, which meant a value tuned to look good
                               // at sunset (high) overexposed clouds at midday, or vice versa.
    float moonGain;           // shared moonlight brightness master — replaces this file's old
                              // hardcoded 0.015 moonContrib scalar; sat_sky.frag's terrain
                              // moonlight term reads the same field
    float pad1;               // reserved
    float pad2;               // reserved
    vec4 mwBasisRow0;
    vec4 mwBasisRow1;
    vec4 mwBasisRow2;
    CloudLayer layers[4];
    float stormStrength;
                               // sat_sky.frag's storm-driven widening/equatorward shift)
    float auroraGain;
    float auroraCloudGain;
    float auroraGroundGain;
    float auroraCoverageFreq;
    float auroraCoverageAzFreq;
    float auroraCoverageDriftRate;
    float auroraShimmerRate;
    // Struct grew 320->336 (session 30, see GpuCloudParams in SatelliteSim.h for why this was
    // appended here instead of reusing pad1/pad2 above).
    float cloudNightAmbientGain;
                                  // floor added to skyAmbient in cloudMarchCS
    float cloudBaseVariance;
                               // in hNorm units (0 = old perfectly flat base); see cloudMarchCS
    float cloudErosionEdge;
                               // silhouette edge (base near 0)
    float cloudErosionCore;
                               // dense core (base near 1); kept lower than cloudErosionEdge so
                               // cores stay comparatively solid while edges fray
} cloud;
