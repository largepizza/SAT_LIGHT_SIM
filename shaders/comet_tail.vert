#version 450
// ── comet_tail.vert — comet tails and comae (2026-10-27; SatelliteSimComets.cpp, CLAUDE.md "Comets") ───────────────
// The CPU writes each comet's dust sheet, ion ribbon and coma as grids of sky directions (ENU from the eye), each
// vertex carrying its surface brightness in the Milky Way's anchor units (lum 0.05 = 21 mag/arcsec^2, linear in the
// physical surface brightness). One draw per grid ("patch", gl_InstanceIndex), six vertices per cell, no vertex
// buffer. Drawn at infinity (kDepthFar) with w = the camera depth, so cells reaching behind the eye clip correctly
// and the coma's tangent-plane coordinates interpolate perspective-correctly.
// Per vertex: the sky's own surface brightness in this direction (light pollution, twilight, daylight with its
// aureole round the Sun, moonlight: darksky.glsl's model, magnitudes per square arcsecond) and the extinction along
// the line of sight (atmosphere.glsl) — both smooth over a cell.

#include "depth.glsl"
#include "common.glsl"
#define CLOUD_PARAMS_BINDING 4
#include "cloud_params.glsl"
layout(std430, set = 0, binding = 5) readonly buffer LightDomeBuf {
    float lightDome[16];
    float lightDomeEased[16];
};
#include "atmosphere.glsl"
#include "darksky.glsl"

struct Patch { uvec4 a; vec4 col; };      // a: first vertex, nu, nv, kind
struct Vert { vec4 dirLum; vec4 uv; };
layout(std430, set = 0, binding = 0) readonly buffer CometBuf {
    Patch patches[16];
    Vert verts[];
};

layout(push_constant) uniform PC {
    mat4  skyView;
    float fovYRad, aspect, screenW, screenH;
    vec4  sunDirENU;  // xyz, w = sin(elevation)
    vec4  obs;        // x observer radius (m), y extinction k, z atmFrac (the air's share round the eye), w moonBright
    vec4  misc;       // x display gain, y manual terrain test, z the Sun above the apparent horizon, w glare gate
} pc;

layout(location = 0) out vec3  vUV;
layout(location = 1) out float vLum;
layout(location = 2) out float vSkyMag;
layout(location = 3) out float vExt;
layout(location = 4) flat out uint vKind;
layout(location = 5) flat out vec4 vCol;

// The sky's surface brightness (mag/arcsec^2) in direction d: city glow (the physical form, as the aurora reads it)
// + twilight + daylight (+ its aureole round the Sun) + moonlight, added as light. Twilight and day only with air round
// the eye; the glare round the Sun (the eye's, the camera's) from anywhere it is above the horizon.
float cometSkyMag(vec3 d)
{
    const float kM = 0.4 * 2.302585093;
    // city glow (darkSkySkyMagLinear's first half)
    float az      = mod(atan(d.x, d.y) + 6.283185307, 6.283185307);
    float secF    = az * (16.0 / 6.283185307) - 0.5;
    int   sec0    = int(floor(secF));
    float secFrac = secF - float(sec0);
    int   sec0w   = ((sec0 % 16) + 16) % 16;
    int   sec1w   = (sec0w + 1) % 16;
    float raw     = mix(lightDome[sec0w], lightDome[sec1w], secFrac) * cloud.auroraSky.x;
    float dd      = max(raw, 0.0) * 0.08 / max(1.0 - raw, 0.02);
    float elevTerm = 0.4 + 0.6 * (0.35 / (max(d.z, 0.0) + 0.35));
    float mCity   = kSkyMagPristine - 2.5 * log(1.0 + dd * elevTerm / 0.0241) / log(10.0);
    float L = exp(-kM * mCity);
    float atm = pc.obs.z;
    // twilight
    L += atm * exp(-kM * darkSkyTwilightMag(d, pc.sunDirENU.xyz, pc.sunDirENU.w));
    // daylight: ~11 mag/arcsec^2 with the Sun on the horizon to ~4 with it 30 deg up, brighter toward the Sun (the
    // aureole ~ 1/theta^2 inside a few degrees)
    float cosS = clamp(dot(d, pc.sunDirENU.xyz), -1.0, 1.0);
    float thDeg = degrees(acos(cosS));
    if (pc.sunDirENU.w > -0.02) {
        float s = smoothstep(-0.02, 0.5, pc.sunDirENU.w);
        float mDay = 11.0 - 7.0 * sqrt(s);
        float aur = 1.0 + 9.0 / max(thDeg * thDeg, 0.09);
        L += atm * exp(-kM * mDay) * aur;
    }
    // the glare round the Sun from anywhere (eye / camera dazzle)
    if (pc.misc.z > 0.5)
        L += exp(-kM * 9.0) * 4.0 / max(thDeg * thDeg, 0.09);
    // moonlight (a full Moon high: ~18 mag/arcsec^2)
    L += atm * pc.obs.w * exp(-kM * 18.0);
    return -(1.0 / kM) * log(max(L, 1e-30));
}

void main()
{
    Patch P = patches[gl_InstanceIndex];
    uint nu = P.a.y;
    uint q = uint(gl_VertexIndex) / 6u, c = uint(gl_VertexIndex) % 6u;
    uint qi = q % (nu - 1u), qj = q / (nu - 1u);
    const uvec2 corner[6] = uvec2[6](uvec2(0, 0), uvec2(1, 0), uvec2(1, 1), uvec2(0, 0), uvec2(1, 1), uvec2(0, 1));
    uint i = qi + corner[c].x, j = qj + corner[c].y;
    Vert V = verts[P.a.x + j * nu + i];

    vec3 cam = (pc.skyView * vec4(V.dirLum.xyz, 0.0)).xyz;
    float tanH = tan(pc.fovYRad * 0.5);
    float w = -cam.z;
    gl_Position = vec4(cam.x / (tanH * pc.aspect), -cam.y / tanH, kDepthFar * w, w);

    vec3 d = normalize(V.dirLum.xyz);
    vUV = V.uv.xyz;
    vLum = V.dirLum.w;
    vKind = P.a.w;
    vCol = P.col;
    vSkyMag = cometSkyMag(d);
    vExt = pow(10.0, -0.4 * atmExtinctionMag(vec3(0.0, 0.0, pc.obs.x), d, 0.0, pc.obs.y));
}
