#ifndef SATLIGHTSIM_BEAM_CLOUD_LIGHTS_GLSL
#define SATLIGHTSIM_BEAM_CLOUD_LIGHTS_GLSL

// Moved verbatim out of cloud_march.comp (2026-09-27) so clouds v2 (cloud_v2_march.comp) lights its
// clouds from the same list, tile cull and lighting function as v1. The only change: the cull takes
// the cloud shell's outer radius as a parameter (v1 passes its layer-1 shell, v2 its highest top).
// Needs common.glsl, cloud_params.glsl, and BEAM_CLOUD_LIGHTS_BINDING defined; the including shader
// must be dispatched with 16x16 workgroups (the shared-memory list is per workgroup).

// ── Reflect-Orbital beam->cloud light sources (2026-08-09, fourth design) ────────────────────
// First was a per-target CPU aggregation anchored at the idealized targetENU — retired once
// beam_self_march.comp started marching the beam's REAL path and made per-beam values genuinely
// differ, which a horizontal-only Gaussian around an idealized target position could never read
// as directional. Second was a true per-beam glow folded into main()'s per-pixel debug-ray loop,
// using the CAMERA's own view-ray-to-beam-line distance for proximity — cheap, but geometrically
// wrong: that distance's iso-contours are not soft blobs, they're hyperbola-like curves, and it
// showed up in-app as large white rings intersecting the beams. Third restored the correct
// architecture — evaluated PER CLOUD SAMPLE inside the march (`inScatter`, right alongside
// `sunColorCloud`/`moonContrib`), exactly like sun/moon — with each list entry as one individual
// real beam, no aggregation. That was affordable at a 16-slot cap but visually starved; getting
// "appreciable" coverage needed raising the cap to 512, which tanked frame rate (this loop's cost
// is linear in the cap, evaluated per IN-CLOUD SAMPLE, not per pixel).
//
// This fourth version keeps the third's per-sample architecture and geometric accuracy (`posENU`
// is the beam's REAL ground intersection, satENU + reflectDirENU traced to R_EARTH; `dirToSource`
// is its REAL direction, ground toward satellite) but clusters: a beam LOCKED onto its target
// (small `aimErrorRad`, checked on the CPU) is folded into a shared per-target light via
// intensity-weighted average — dozens of satellites converged on one site produce nearly
// identical geometry, so representing them as one combined light recovers full visual coverage at
// a fraction of the cost. A beam still SLEWING keeps its own individual slot, since its geometry
// is genuinely unique right now. See `GpuBeamCloudLights`' own comment (SatelliteSim.h) for the
// CPU-side clustering logic. Cap dropped back to 256 (from 512) — same real coverage, since the
// effective distinct-light count is now bounded near the active-target count, not total
// active-satellite count.
// Struct must match GpuBeamCloudLight/GpuBeamCloudLights in SatelliteSim.h exactly — including
// kMaxCloudBeamLights itself, hand-duplicated here (not shared via a header) same as this
// project's other CPU/GPU struct-layout pairs; a mismatch silently truncates or wastes capacity,
// not a crash, so it will NOT show up as a compile or validation error if missed.
const uint kMaxCloudBeamLights = 512u;
struct BeamCloudLight {
    vec3  posENU;       // meters, observer-relative — REAL ground intersection of this ONE beam
    float intensity;    // groundIrradiance * beamGain for this satellite
    vec3  dirToSource;  // unit direction from posENU toward the satellite (real beam direction)
    float footprintRadM;
    float blockAltM;    // altitude (m) where THIS beam's own path drops below 50% transmittance
    float blockOpacity; // 0 = clear column, 1 = fully opaque
    float pad0, pad1;   // std430 array-of-vec4-pairs alignment
};
layout(std430, set = 0, binding = BEAM_CLOUD_LIGHTS_BINDING) readonly buffer BeamCloudLightBuf {
    uint            beamLightCount;
    uint            beamLightPad0, beamLightPad1, beamLightPad2;
    BeamCloudLight  beamLights[kMaxCloudBeamLights];
};

// Perf: fixed-sigma cutoff BEFORE the exp() call — see the retired version's own comment for
// why this bounds cost the same way regardless of a light's brightness. At file scope (2026-08-10)
// because cullCloudLightsForTile below must use the identical value; if the two ever disagree the
// cull stops being conservative and beam glow pops on a 16x16-texel grid.
const float kBeamCutoffSigma = 4.0;

// ── Per-tile cloud-light culling (2026-08-10) ────────────────────────────────────────────────────
// beamCloudLighting() is called from cloudMarchCS's innermost loop — once per in-cloud SAMPLE, so
// ~128 march steps x 484,800 texels — and walked all beamLightCount entries every time. That list
// is capped at kMaxCloudBeamLights = 512 (not 16; CLAUDE.md said 16 and was stale), which is why
// knockout bit 128 measured 3.48 ms inside the cloud_march bucket at Medium in the Anchorage sweep.
//
// Same fix, same workgroup, same reasoning as cullBeamsForTile above: one 16x16-texel tile per
// workgroup, 256 threads cooperatively reduce 512 lights to the handful whose influence cylinder
// can reach any ray in this tile, and the per-sample loop walks that shared list instead. The cost
// of the test moves from (lights x samples x texels) to (lights / 256) per workgroup.
//
// CONSERVATISM. beamCloudLighting accepts a sample when its perpendicular distance to the light's
// infinite line (posENU, dirToSource) is within footprintRadM * kBeamCutoffSigma. Every sample lies
// on this tile's view rays somewhere inside the cloud shell, so the tile test is the minimum of
// that same perpendicular distance over the tile-centre ray restricted to [0, tRangeMax], widened
// by tRangeMax * tileHalfAngle to cover rays at the tile's edge. tRangeMax is the tile-centre ray's
// own far crossing of the cloud-top sphere, which upper-bounds where cloudMarchCS can march
// (tExit is min'd against exactly that shell exit), scaled by kTileRangeMargin so a tile-edge ray
// whose own shell crossing runs slightly longer is still covered.
//
// Distances are formed observer-relative throughout (w0 = -posENU, never obsPos - lightWorldPos),
// the same cancellation-avoidance every other beam computation in this file uses.
const uint  kTileCloudLightMax = 128u;
const float kTileRangeMargin   = 1.25;
shared uint sTileLightCount;
shared uint sTileLightOverflow;
shared uint sTileLightIdx[kTileCloudLightMax];

void cullCloudLightsForTile(vec3 cAxis, float tileHalfAngle, vec3 obsPos, float shellTopR) {
    if (gl_LocalInvocationIndex == 0u) {
        sTileLightCount = 0u;
        // Bit 131072 turns this cull off alongside the pointing-ray one — same A/B, same
        // requirement that the image be pixel-identical either way.
        sTileLightOverflow = ((cloud.dbgDisableMask & 131072u) != 0u) ? 1u : 0u;
    }
    barrier();

    if ((cloud.dbgDisableMask & 128u) == 0u && cloud.beamSkyGlowGain > 0.0
        && (cloud.dbgDisableMask & 131072u) == 0u) {
        vec2  shellT    = raySphere(obsPos, cAxis, shellTopR);
        float tRangeMax = max(shellT.y, 0.0) * kTileRangeMargin;
        float slack     = tRangeMax * tileHalfAngle;
        uint  n         = min(beamLightCount, kMaxCloudBeamLights);
        for (uint bli = gl_LocalInvocationIndex; bli < n; bli += gl_WorkGroupSize.x * gl_WorkGroupSize.y) {
            if (beamLights[bli].intensity <= 0.0) continue;
            vec3  lp   = beamLights[bli].posENU;   // observer-relative
            vec3  dirS = beamLights[bli].dirToSource;
            vec3  w0   = -lp;                      // observer - light origin
            float b  = dot(cAxis, dirS);
            float dd = dot(cAxis, w0);
            float e  = dot(dirS, w0);
            float den = 1.0 - b * b;
            float t = (den > 1e-6) ? (b * e - dd) / den : 0.0;
            t = clamp(t, 0.0, tRangeMax);
            vec3  toP  = cAxis * t - lp;           // P(t) - lightWorldPos, small-magnitude by construction
            vec3  perp = toP - dirS * dot(toP, dirS);
            float cutoff = max(beamLights[bli].footprintRadM, 100.0) * kBeamCutoffSigma + slack;
            if (dot(perp, perp) > cutoff * cutoff) continue;

            uint slot = atomicAdd(sTileLightCount, 1u);
            if (slot < kTileCloudLightMax) sTileLightIdx[slot] = bli;
            else                            sTileLightOverflow = 1u;
        }
    }
    barrier();
}

// Takes the evaluation point (`p`/`h`) and `sampleDayness` (caller's own per-sample geographic
// day/night gate) — same signature shape the retired function used, still cloud-only (fog no
// longer carries a beam term at all, removed with the second design).
vec3 beamCloudLighting(vec3 p, float h, vec3 dir, vec3 obsPos, float sampleDayness) {
    vec3 beamLit = vec3(0.0);
    if ((cloud.dbgDisableMask & 128u) != 0u || cloud.beamSkyGlowGain <= 0.0 || beamLightCount == 0u)
        return beamLit;

    const float kBeamCloudGlowScale = 1e-6;
    const float kCloudFeatherM = 500.0; // soft edge width for the height cutoff below
    // 2026-08-10: walk the workgroup's culled list, not all beamLightCount entries. Overflow past
    // kTileCloudLightMax (or bit 131072) falls back to the full scan, which is exactly the old
    // behaviour — slow on a pathological tile, never wrong.
    bool lightOverflow = (sTileLightOverflow != 0u);
    uint iterCount = lightOverflow ? min(beamLightCount, kMaxCloudBeamLights)
                                   : min(sTileLightCount, kTileCloudLightMax);
    vec3 mag = vec3(0.0);
    for (uint li = 0u; li < iterCount; ++li) {
        uint bli = lightOverflow ? li : sTileLightIdx[li];
        // 2026-08-09 (in-app finding: cloud bottoms near a busy target read as uniformly lit,
        // independent of illumination/density sliders): this used to be a HORIZONTAL-only
        // distance from the sample to the light's ground point, with brightness gated purely
        // on/off by the height cutoff below — i.e. no real falloff from a source position at
        // all, just a flat disc. Replaced with true 3D perpendicular distance from the sample to
        // the light's actual LINE (posENU + dirToSource) — the same point-to-line shape
        // beam_self_march.comp/the debug ray already use elsewhere, just applied here for the
        // first time. Samples near where the beam actually crosses the shell now read brighter
        // than samples merely near the ground point at any height, which is what actually
        // produces a top-bright/bottom-dark gradient instead of a wash.
        vec3  lightWorldPos = obsPos + beamLights[bli].posENU;
        vec3  toP  = p - lightWorldPos;
        vec3  dirS = beamLights[bli].dirToSource;
        vec3  perp = toP - dirS * dot(toP, dirS);
        float distSq = dot(perp, perp);
        float radius = max(beamLights[bli].footprintRadM, 100.0);
        float cutoff = radius * kBeamCutoffSigma; // same sigma cullCloudLightsForTile bounds against
        if (distSq > cutoff * cutoff) continue;
        float g = beamLights[bli].intensity * exp(-distSq / (2.0 * radius * radius));
        float hCut = smoothstep(beamLights[bli].blockAltM - kCloudFeatherM,
                                  beamLights[bli].blockAltM, h);
        float hFadeBeam = mix(1.0, hCut, beamLights[bli].blockOpacity);
        // Directional shading using THIS beam's own real direction (2026-08-09) — not a shared
        // local-zenith approximation, since an oblique beam's true source direction can differ
        // substantially from straight up. Reuses the same phaseCloud() Henyey-Greenstein lobe the
        // sun/moon terms already use, so looking up along a beam reads as a brighter forward-
        // scattered shaft — now genuinely toward where the light is actually coming from.
        float ph = phaseCloud(dot(dir, beamLights[bli].dirToSource));
        mag += vec3(1.0, 0.97, 0.92) * (g * hFadeBeam * ph);
    }

    beamLit = mag * cloud.beamSkyGlowGain * (1.0 - sampleDayness) * kBeamCloudGlowScale;
    return beamLit;
}

#endif // SATLIGHTSIM_BEAM_CLOUD_LIGHTS_GLSL
