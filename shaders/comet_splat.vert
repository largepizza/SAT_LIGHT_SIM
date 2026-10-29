#version 450
// ── comet_splat.vert — comet tail particles (2026-10-28; SatelliteSimComets.cpp, Comets.h, CLAUDE.md "Comets") ───────
// One instance per particle (dust grain or ion parcel), six vertices: a quad round its projection in the half-res splat
// target. The particle is carried from its build's time to the frame's by a Taylor step (the acceleration from its own
// heliocentric position: the Sun's gravity x (1 - beta)), its light is its flux weight / (its distance from the
// observer)^2, x the dust's compound Henyey-Greenstein phase at its own scattering angle / (its distance from the Sun)^2
// for dust (ions fluoresce in sunlight: / (their distance from the Sun)^2 too, saturated as the head's law), spread as
// a Gaussian of its spread (the grains it stands for) or ~a pixel. A dust grain also carries its local (emission time,
// ln beta) field (Comets.h GpuGrain d / e): projected to the screen here, the fragment shader maps each pixel of its
// Gaussian to the emission time and size of the dust there (the tail's zoom detail, comet_splat.frag).
#include "comet_frame.glsl"
layout(push_constant) uniform PC {
    vec4 a, b, c;
    vec4 screen;   // w: 1 = the field pass (comet_field.frag), 0 = the colour pass
} pc;

layout(location = 0) out vec2 vOff;       // the corner in units of the Gaussian's sigma
layout(location = 1) out vec3 vLight;     // colour x surface brightness at the centre (lum units)
layout(location = 2) flat out vec4 vTe;
layout(location = 3) flat out vec4 vGrad;
layout(location = 4) flat out vec4 vRect;     // the atlas level's rectangle (px): the quad never spills past it

const float kMuSun = 2.9591220828e-4;     // AU^3 / day^2
const float kLumPerFluxSr = 2.952e-4;
const float kQuadSig = 2.6;                // the quad's half-size in sigmas
const float kLevels = 6.0;                 // atlas levels (comet_composite.frag)

float hg(float g, float c) { return (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * c, 1.5); }
// The Halley-Marcus compound phase (Comets.cpp dustPhase), normalised to 1 at 90 deg; c = cos(scattering angle),
// 1 = straight forward.
float dustPhase(float c)
{
    float p90 = 0.95 * hg(0.9, 0.0) + 0.05 * hg(-0.7, 0.0);
    return (0.95 * hg(0.9, c) + 0.05 * hg(-0.7, c)) / p90;
}

void cull()
{
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0); vOff = vec2(0.0); vLight = vec3(0.0);
    vTe = vec4(0.0); vGrad = vec4(0.0); vRect = vec4(0.0);
}

// A direction from the observer (ecliptic) to splat pixels.
vec2 screenPx(vec3 dEcl)
{
    vec3 d = normalize(dEcl);
    vec3 enu = vec3(dot(F.eclX.xyz, d), dot(F.eclY.xyz, d), dot(F.eclZ.xyz, d));
    vec3 cam = (F.skyView * vec4(enu, 0.0)).xyz;
    float w = max(-cam.z, 1e-4);
    vec2 ndc = vec2(cam.x / (F.proj.x * F.proj.y * w), -cam.y / (F.proj.x * w));
    return ndc * 0.5 * F.proj.zw;
}

void main()
{
    Grain G = grains[gl_InstanceIndex];
    uint tag = uint(G.q.w + 0.5);
    uint kind = tag & 3u, cs = tag >> 2;
    vec4 C = F.comet[2u * cs];
    vec4 A4 = F.comet[2u * cs + 1u];
    vec3 A = A4.xyz;
    float dt = C.w;
    float age = G.q.z + dt;
    if (age < 0.0 || G.p.w <= 0.0) { cull(); return; }
    vec3 p0 = G.p.xyz;
    vec3 h0 = A + p0;
    float r0 = length(h0);
    vec3 acc = -kMuSun * G.q.x * h0 / (r0 * r0 * r0);
    vec3 pt = p0 + G.v.xyz * dt + 0.5 * acc * dt * dt;
    vec3 D = C.xyz + pt;                 // the particle from the observer (ecliptic AU)
    float dist = length(D);
    vec3 hel = A + pt;
    float rg = length(hel);
    float flux = G.p.w / (dist * dist);
    vec3 col;
    if (kind == 0u) {
        float c = dot(hel / rg, -D / dist);
        flux *= dustPhase(c) / (rg * rg);
        col = vec3(1.05, 0.98, 0.86);    // sunlight off dust, a little reddened (luminance ~1)
    } else {
        // CO+ fluorescence (the comet-tail bands at 400-430 nm, + a little H2O+): deep blue. Excited by sunlight at the
        // ion's own distance from the Sun, saturated as the head's law (A4.w = rSat^2).
        flux *= (1.0 + A4.w) / (rg * rg + A4.w);
        col = vec3(0.16, 0.42, 1.95);
    }
    vec3 dirEcl = D / dist;
    vec3 enu = vec3(dot(F.eclX.xyz, dirEcl), dot(F.eclY.xyz, dirEcl), dot(F.eclZ.xyz, dirEcl));
    vec3 cam = (F.skyView * vec4(enu, 0.0)).xyz;
    float w = -cam.z;
    if (w < 0.02) { cull(); return; }
    float tanH = F.proj.x, aspect = F.proj.y;
    float pixAng = F.misc.z;
    float sigAU = max(G.v.w + G.q.y * dt, 0.0);
    float sAng = sqrt((sigAU / dist) * (sigAU / dist) + (0.7 * pixAng) * (0.7 * pixAng));
    // the projection's local scale (radians -> splat pixels), larger off the axis
    float pxPerRad = 0.5 * F.proj.w / (tanH * w);
    float sPx = sAng * pxPerRad;
    // Surface brightness from the TRUE spread. A particle wider than the quad cap draws into the coarsest atlas level
    // that holds it within the cap (level k: 2^-k of the splat target's resolution; SatelliteSimComets.cpp), so its fill
    // is bounded and zoomed in the whole tail in view still draws; past the last level the old fade (a near grain
    // passing the eye is a faint disc, not a bright one).
    float lum = kLumPerFluxSr * flux / (6.2831853 * sAng * sAng);
    vec2 ndc = vec2(cam.x / (tanH * aspect * w), -cam.y / (tanH * w));
    bool fieldPass = pc.screen.w > 0.5;

    // The tail's zoom detail: the (emission time, ln beta) field's screen Jacobian at this grain (level-0 pixels).
    vTe = vec4(0.0); vGrad = vec4(0.0);
    if (fieldPass) {
        vec2 u0 = unpackHalf2x16(G.d.x), u1 = unpackHalf2x16(G.d.y), u2 = unpackHalf2x16(G.d.z);
        vec3 dG = vec3(u0, u1.x), dB = vec3(u1.y, u2);
        float lg = length(dG), lb = length(dB);
        float on = 0.0;
        if (kind == 0u && F.pad[0].x > 0.0 && lg > 0.0 && lb > 0.0) {
            vec2 s0 = ndc * 0.5 * F.proj.zw;
            float hG = 1e-3 * dist / lg, hB = 1e-3 * dist / lb;
            vec2 gs = (screenPx(D + dG * hG) - s0) / hG; // px per day of emission time
            vec2 bs = (screenPx(D + dB * hB) - s0) / hB; // px per e-fold of beta
            float det = gs.x * bs.y - gs.y * bs.x;
            float cond = abs(det) / max(length(gs) * length(bs), 1e-30);
            // where the synchrones and syndynes run together on screen (the tail seen edge-on) the field is undefined
            on = smoothstep(0.06, 0.25, cond) * min(F.pad[0].x, 1.0);
            if (on > 0.0) {
                vec2 rT = vec2(bs.y, -bs.x) / det;  // d te / d px
                vec2 rB = vec2(-gs.y, gs.x) / det;  // d ln beta / d px
                vTe = vec4(uintBitsToFloat(G.d.w) - F.coma[3u * cs + 2u].w, log(max(1.0 - G.q.x, 1e-6)), 0.0, on);
                vGrad = vec4(rT, rB) * sPx;         // per sigma (vOff's unit)
            }
        }
        if (on <= 0.0) { cull(); return; }
    }

    // The pass's atlas: the colour pass's (F.pad[1], splat px) or the field pass's (F.pad[2], half of them: the field
    // is smooth, and RGBA32F blending costs ~4x the colour's per pixel).
    vec4 geo = fieldPass ? F.pad[2] : F.pad[1];
    float sP = sPx * geo.x / F.pad[1].x;
    float capPx = F.misc.y / kQuadSig;
    float lvl = 0.0;
    if (sP > capPx) {
        lvl = min(ceil(log2(sP / capPx)), kLevels - 1.0);
        float maxS = capPx * exp2(kLevels - 1.0);
        if (sP > maxS) lum *= smoothstep(4.0, 1.5, sP / maxS);
    }
    if (lum < F.misc.x) { cull(); return; }
    // the level's rectangle in the atlas (px): level 0 at (0, 0), level k >= 1 at (W, the levels 1..k-1 above it)
    float sc = exp2(-lvl);
    vec2 W0 = geo.xy, AT = geo.zw;
    vec2 wh = max(floor(W0 * sc), vec2(1.0));
    vec2 org = lvl < 0.5 ? vec2(0.0) : vec2(W0.x, 0.0);
    for (float j = 1.0; j < lvl; j += 1.0) org.y += max(floor(W0.y * exp2(-j)), 1.0);
    vRect = vec4(org, org + wh);
    const vec2 corner[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 k = corner[gl_VertexIndex % 6];
    float sL = min(sP * sc, capPx);                          // the Gaussian in the level's pixels
    vec2 pcen = org + (ndc * 0.5 + 0.5) * wh + k * (kQuadSig * sL);
    gl_Position = vec4(pcen / AT * 2.0 - 1.0, 0.5, 1.0);
    vOff = k * kQuadSig;
    vLight = col * lum;
}
