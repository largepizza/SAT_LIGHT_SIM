#version 450
// ── comet_splat.vert — comet tail particles (2026-10-28; SatelliteSimComets.cpp, Comets.h, CLAUDE.md "Comets") ───────
// One instance per particle (dust grain or ion parcel), six vertices: a quad round its projection in the half-res splat
// target. The particle is carried from its build's time to the frame's by a Taylor step (the acceleration from its own
// heliocentric position: the Sun's gravity x (1 - beta)), its light is its flux weight / (its distance from the
// observer)^2, x the dust's compound Henyey-Greenstein phase at its own scattering angle / (its distance from the Sun)^2
// for dust (ions fluoresce: no phase), spread as a Gaussian of its spread (the grains it stands for) or ~a pixel.
#include "comet_frame.glsl"

layout(location = 0) out vec2 vOff;       // the corner in units of the Gaussian's sigma
layout(location = 1) out vec3 vLight;     // colour x surface brightness at the centre (lum units)

const float kMuSun = 2.9591220828e-4;     // AU^3 / day^2
const float kLumPerFluxSr = 2.952e-4;
const float kQuadSig = 2.6;                // the quad's half-size in sigmas

float hg(float g, float c) { return (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * c, 1.5); }
// The Halley-Marcus compound phase (Comets.cpp dustPhase), normalised to 1 at 90 deg; c = cos(scattering angle),
// 1 = straight forward.
float dustPhase(float c)
{
    float p90 = 0.95 * hg(0.9, 0.0) + 0.05 * hg(-0.7, 0.0);
    return (0.95 * hg(0.9, c) + 0.05 * hg(-0.7, c)) / p90;
}

void cull() { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); vOff = vec2(0.0); vLight = vec3(0.0); }

void main()
{
    Grain G = grains[gl_InstanceIndex];
    uint tag = uint(G.q.w + 0.5);
    uint kind = tag & 3u, cs = tag >> 2;
    vec4 C = F.comet[2u * cs];
    vec3 A = F.comet[2u * cs + 1u].xyz;
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
        col = vec3(0.30, 0.66, 1.75);    // CO+ / H2O+ fluorescence, blue
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
    // Surface brightness from the TRUE spread; a particle wider than the cap is drawn within it at that (low)
    // brightness: its light is lost rather than concentrated (a near grain passing the eye at closest approach drew
    // as a bright out-of-focus disc when its light was squeezed into the capped size).
    float lum = kLumPerFluxSr * flux / (6.2831853 * sAng * sAng);
    float capPx = F.misc.y / kQuadSig;
    if (sPx > capPx) { lum *= smoothstep(4.0, 1.5, sPx / capPx); sPx = capPx; }
    if (lum < F.misc.x) { cull(); return; }
    const vec2 corner[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 k = corner[gl_VertexIndex % 6];
    vec2 ndc = vec2(cam.x / (tanH * aspect * w), -cam.y / (tanH * w));
    vec2 halfNdc = vec2(2.0 / F.proj.z, 2.0 / F.proj.w) * (kQuadSig * sPx);
    gl_Position = vec4(ndc + k * halfNdc, 0.5, 1.0);
    vOff = k * kQuadSig;
    vLight = col * lum;
}
