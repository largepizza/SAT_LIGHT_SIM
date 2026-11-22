#ifndef SATLIGHTSIM_LIGHTNING_DRAW_GLSL
#define SATLIGHTSIM_LIGHTNING_DRAW_GLSL
// Lightning channels and glow (lightning.vert / .frag, SatelliteSimLightning.cpp, 2026-10-05).
layout(push_constant) uniform BoltPC {
    mat4  skyView;
    float fovYRad, aspect, screenW, screenH;
    vec4  params;    // x the sky's exposure, y "Lightning bolts", z "Lightning glow", w mode (0 channels, 1 glow)
    vec4  params2;   // x highlight roll-off
    vec4  spare;     // x the eye's altitude (m)
} pc;

struct BoltFlash {     // == GpuBoltFlash (SatelliteSim.h)
    vec4 o;            // xyz origin (ENU about the eye, m), w intensity now
    vec4 u;            // xyz up, w age (s)
    vec4 t1;           // xyz, w kind: 0 in cloud, 1 to ground, 2 red sprite, 3 spider under the base
    vec4 t2;           // xyz, w the tree's longest arc (m)
    vec4 m;            // x revealed arc (m), y the branches' share, z 1 during the stepped leader, w its emitters' light
                       // volumes: 4 bits each, emitter e in bits 4e .. 4e+3 (a slot of flash_volume.glsl, 15 = none)
    vec4 e[4];         // glow emitters: xyz (ENU about the eye), w weight
};
struct BoltSeg { vec3 p0; float w; vec3 p1; uint packed; };
layout(std430, set = 0, binding = 0) readonly buffer BoltFlashBuf { BoltFlash bFlash[]; };
layout(std430, set = 0, binding = 1) readonly buffer BoltSegBuf   { BoltSeg   bSeg[]; };
layout(set = 0, binding = 2) uniform sampler2D cloudTargetA;   // a: signed cloud distance (km, cloud_occlusion.glsl)
layout(set = 0, binding = 3) uniform sampler2D cloudTargetB;   // rgb: cloud transmittance
// The engine lights (2026-10-20, mode 2): the half-res scene depth (m to the first terrain / ocean / mesh surface,
// 1e30 for sky) and the dust list, whose tail after its 1024 entries holds the lights (SatelliteSim::engineLightAdd):
//   c   xyz the light (ENU about the eye, m), w its intensity (suns x m^2, lifted at night)
//   ia  rgb its colour (luminance 1), w the core radius r0 (m: 1 / (r^2 + r0^2))
//   ib  xyz the vehicle's nose (unit), w the hull's length from the light (m; 0 = none)
//   col x the hull's radius (m), y 1 = no light on the ground, z its reach (m)
struct EngineLight { vec4 c; vec4 ia; vec4 ib; vec4 col; vec4 cone; };
layout(set = 0, binding = 4) uniform sampler2D sceneDepthTex;
layout(std430, set = 0, binding = 5) readonly buffer EngineLightBuf { uint elDustCount; uint elCount; uint elPad0, elPad1; EngineLight elList[]; };
const uint kEngineLightBase = 1024u;   // == SatelliteSim::kMaxDustClouds
// The flashes' light on the cloud (2026-11-21, issue_20261121_163317; include/flash_volume.glsl): the cloud's optical
// depth around each emitter, and the cloud lighting's own numbers (the march's, copied by the host each frame).
layout(set = 0, binding = 7) uniform sampler3D boltFvTex;
layout(std140, set = 0, binding = 8) uniform BoltLight {
    vec4 phase;   // == cv2.phase: x / y the forward lobe's g / weight, z / w the backward lobe's
    vec4 ms;      // x 1 = the flashes light the cloud through the volumes (0 = the glow by distance alone, the A/B),
                  // y the diffuse field's g (cv2.ms.y), z its gain (cv2.ms.z), w spare (cv2.ms.w)
    vec4 tune;    // x the diffuse field's side escape per unit of scaled depth, y the single scattering's gain,
                  // z the emitter's own radius (m: a stretch of channel, not a point), w the level (1 / a lit thick face)
    vec4 tune2;   // x the most N may be (flash_volume.glsl: a flash deep in a cloud shows through its thinnest side)
} bl;
#define FV_SAMPLER boltFvTex
#include "flash_volume.glsl"
// Fades a light out over the outer 40% of its reach (a hard cut drew its reach as a circle from orbit).
float reachWin(float r2, float reach2) { return 1.0 - smoothstep(0.36 * reach2, reach2, r2); }

// The air between the eye and a point at `range` (ENU about the eye): the transmittance of its column of air
// (8 km scale height, ~0.15 optical depth per scale height in the visible), so a flash seen from orbit crosses only
// the air below it. A flat 90 km haze length dimmed everything seen from 350 km to ~5%.
float boltAir(vec3 p, float range)
{
    const float H = 8000.0;
    float hE = max(pc.spare.x, 0.0);
    float hP = max(length(p + vec3(0.0, 0.0, 6371000.0 + hE)) - 6371000.0, 0.0);
    float c  = abs(hP - hE) / max(range, 1.0);
    float col = (c > 0.02) ? H * abs(exp(-hE / H) - exp(-hP / H)) / c
                           : range * exp(-min(hE, hP) / H);
    return exp(-0.15 * min(col, range * exp(-min(hE, hP) / H)) / H);
}

// The light of a flash leaving the cloud around it, per unit intensity, at distance r: diffusion through the cloud
// (a broad core ~4 km: from orbit a flash lights the tops over 10-20 km, as in the ISS photographs) and a faint
// long tail (the light reaching the cloud and rain around it).
// r is floored at ~3.5 km (the light has diffused through at least that much cloud wherever it leaves it: without
// it a cloud surface right at the flash drew a hard point, a lamp rather than lightning), and the tail is windowed to
// 0 by kBoltGlowR, inside the quad lightning.vert draws (its edges showed as squares from orbit).
const float kBoltGlowR = 45000.0;
float boltGlow(float r)
{
    float q2 = (r * r + 1.2e7) / 1.6e7;
    return (pow(1.0 + q2, -1.5) + 0.04 / (1.0 + r * r / 2.25e8)) * (1.0 - smoothstep(0.45 * kBoltGlowR, 0.8 * kBoltGlowR, r));
}

// The sky's tonemap (sat_sky.frag): the channels are added after it, screen-blended (lightning pipeline).
vec3 boltTonemap(vec3 x)
{
    return mix(vec3(1.0) - exp(-x), vec3(1.0) - 1.0 / (vec3(1.0) + x + 0.5 * x * x), pc.params2.x);
}

// ── The colour of a stroke (2026-11-22; SatelliteSim::boltColourFill, harness `lightning color`) ──
// What a photograph shows around a bolt, by where the light comes from. The return stroke's channel is ~30,000 K
// plasma: singly ionised nitrogen's lines in the blue-violet (399.5, 444.7, 463.0, 500.5, 568.0 nm) over a continuum,
// and H-alpha: blue-violet, and white only where the image clips (boltTonemap works per channel: a near trunk goes
// to white, a thin or far piece keeps its hue). Round the channel, and along leaders and branches, a cold corona
// sheath a few metres across glows in molecular nitrogen's violet bands (N2 second positive, N2+ first negative
// 391 / 428 nm): most of a leader's or a twig's light, a few percent of a trunk's. The air scatters the stroke's
// light: the irradiance round a line of emitters falls as 1 / distance (each piece's as 1 / distance^2, summed),
// Rayleigh's lambda^-4 leaves the violet lines (indigo), rain and cloud in the way scatter all of it (neutral,
// wider). The air between the stroke and the eye takes the blue first. Every colour here arrives mixed by the host
// from the look of 2026-11-21 (gain 0: these are that look's constants and every new term is 0) to the physical one
// (gain 1) and past it, at the luminance of the colour it replaces.
layout(std140, set = 0, binding = 12) uniform BoltColour {
    vec4 core;      // rgb the channel's colour; a the gain ("Lightning glow colour")
    vec4 halo;      // rgb the Gaussian halo of 2026-10-05 (x 1 - gain: the scattered glow replaces it); a spare
    vec4 sheath;    // rgb the corona sheath's colour (the core's luminance); a its radius round a main channel (m)
    vec4 sheathK;   // x its share of a main channel's light (x gain), y x this per branch order, z a stepped leader's
                    // share (x gain), w its radius x this per branch order
    vec4 glow;      // rgb the scattered glow in clear air x its share (the halo's luminance and energy); a spare
    vec4 glowWet;   // rgb the same through rain / cloud x its share; a how many times wider its core is
    vec4 wet;       // x the wet share of the glow anywhere, y + this x the share of the stroke's light the cloud
                    // composite in front of it takes (rain shafts included); zw spare
    vec4 air;       // rgb the air's optical depth per channel over the grey one's (boltAir), - 1, x gain; a spare
    vec4 cloud;     // rgb the flash's light on the cloud (mode 1); a spare
} bc;
// What the air leaves of each channel, over boltAir's grey transmittance `air` (1 at gain 0).
vec3 boltAirTint(float air) { return pow(vec3(max(air, 1e-6)), bc.air.rgb); }
#endif
