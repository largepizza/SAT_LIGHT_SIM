#version 450
// The turbulence warp (2026-10-28, issues 20261028_193115 "Detail Stars should flicker with turbulence" and
// 20261028_191408): atmospheric seeing as a distortion of the WHOLE picture, after the main pass (sky, Milky Way and its
// star-cloud points, the Moon, the Sun, stars, planets, satellites) and before the UI. The frame is copied
// (SatelliteSimTurbulence.cpp) and resampled through a smooth displacement field:
//  - its rms per axis at each pixel is the refraction table's angle-of-arrival rms at that pixel's APPARENT elevation
//    (Refraction.cpp: 2.91 D^-1/3 int Cn^2 ds), x the share of the turbulent column in front of the first surface the
//    pixel shows (terrain / sea / mesh from the scene depth, cloud from the composite: 1 - exp(-d / aoaS), aoaS the
//    Cn^2-weighted mean distance along that ray), x "Turbulence warp", faded out above the air;
//  - in pixels it is that angle / the pixel's angle, so it is invisible at a wide field and boils when zoomed;
//  - the field is vector value noise on the sky (ENU directions, anchored on the CPU in double so it neither swims
//    when the camera turns nor loses precision), cells ~12" or 6 px, whichever is larger (power-of-two levels
//    cross-faded, so a zoom does not rescale a field), twice as wide horizontally; it drifts with a wind and boils
//    (three phases cross-faded, each re-drawn at its zero weight: a constant rate of change), in REAL time (frame seconds): seeing is a property
//    of the moment, not of the simulated clock.
// The Sun's and the Moon's discs no longer jitter in sat_sky.frag while this pass is on (refrSun.z = 0): the warp moves
// them like everything else.

layout(set = 0, binding = 0) uniform TurbWarp {
    vec4  camR;      // camera right / up / forward in ENU (xyz)
    vec4  camU;
    vec4  camF;
    vec4  screen;    // x W, y H (px), z tan(fov/2) x aspect, w tan(fov/2)
    vec4  params;    // x the pixel's angle (rad / px), y gain (x the altitude fade), z 1 / (the field's rms), w 1 / (table n - 1)
    vec4  mScale;    // xyz level 0's coordinate scale (cells / rad) on E, N, U; w unused
    ivec4 anc[6];    // the lattice anchors: [phase k level o] at k x 2 + o, three phases (mod 4096)
    vec4  frac[6];   // their fractions (xyz); w = the weight (phase x level, each normalised to unit rms)
    vec4  aniso;     // x horizontal amplitude, y vertical amplitude, z table eLo (rad), w table span (rad)
    vec4  tab[64];   // x aoa rms (rad), y aoaS (m)
} u;

layout(set = 0, binding = 1) uniform sampler2D frameTex;   // the composed frame (copy of the swapchain image)
layout(set = 0, binding = 2) uniform sampler2D cloudA;     // the cloud composite: alpha = signed distance (km)
layout(set = 0, binding = 3) uniform sampler2D cloudB;     // rgb = its transmittance
layout(set = 0, binding = 4) uniform sampler2D sceneDepth; // R32F metres to the first terrain / sea / mesh surface

layout(location = 0) out vec4 outColor;

uvec3 pcg3d(uvec3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

// Vector value noise at lattice anchor a + q (q small, float-exact), uniform corners in [-1, 1], quintic interpolation.
vec2 vnoise2(ivec3 a, vec3 q, uint seed)
{
    vec3  fl = floor(q);
    vec3  f  = q - fl;
    ivec3 c  = a + ivec3(fl);
    vec3  s  = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    vec2  acc = vec2(0.0);
    for (int k = 0; k < 8; ++k) {
        ivec3 o = ivec3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        uvec3 h = pcg3d((uvec3(c + o) & 4095u) + uvec3(seed, seed * 7u, seed * 13u));
        vec2  v = vec2(h.xy) * (2.0 / 4294967295.0) - 1.0;
        vec3  w = mix(1.0 - s, s, vec3(o));
        acc += v * (w.x * w.y * w.z);
    }
    return acc;
}

// Catmull-Rom with 5 bilinear taps (the sky TAA's form): sharper than one bilinear tap, so a resampled star stays a star.
vec3 sampleCR(vec2 px)
{
    vec2 texSize = u.screen.xy;
    vec2 pos = px - 0.5;
    vec2 tp1 = floor(pos);
    vec2 f = pos - tp1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 tc12 = (tp1 + 0.5 + w2 / w12) / texSize;
    vec2 tc0 = (tp1 - 0.5) / texSize;
    vec2 tc3 = (tp1 + 2.5) / texSize;
    vec3 r = texture(frameTex, vec2(tc12.x, tc0.y)).rgb * (w12.x * w0.y)
           + texture(frameTex, vec2(tc0.x, tc12.y)).rgb * (w0.x * w12.y)
           + texture(frameTex, tc12).rgb * (w12.x * w12.y)
           + texture(frameTex, vec2(tc3.x, tc12.y)).rgb * (w3.x * w12.y)
           + texture(frameTex, vec2(tc12.x, tc3.y)).rgb * (w12.x * w3.y);
    float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / wsum, vec3(0.0));
}

vec2 tableAt(float e)
{
    float x = sqrt(clamp((e - u.aniso.z) / u.aniso.w, 0.0, 1.0)) / u.params.w;
    int   i = min(int(x), 62);
    return mix(u.tab[i].xy, u.tab[i + 1].xy, x - float(i));
}

// The share of the jitter in front of a surface d metres away.
float shareAt(float d, float s) { return s > 1.0 ? 1.0 - exp(-d / s) : 1.0; }

void main()
{
    ivec2 ip = ivec2(gl_FragCoord.xy);
    vec2  uv = gl_FragCoord.xy / u.screen.xy;
    // The view ray: camera offsets v from the forward axis (dir - fwd is formed without cancellation).
    vec2  ndc = uv * 2.0 - 1.0;
    vec3  v   = u.camR.xyz * (ndc.x * u.screen.z) - u.camU.xyz * (ndc.y * u.screen.w);
    float v2  = dot(v, v);
    float L   = sqrt(1.0 + v2);
    vec3  dir = (u.camF.xyz + v) / L;
    vec3  dd  = (v - u.camF.xyz * (v2 / (L + 1.0))) / L;   // dir - fwd

    vec2  ts  = tableAt(asin(clamp(dir.z, -1.0, 1.0)));
    if (ts.x <= 0.0) { outColor = vec4(texelFetch(frameTex, ip, 0).rgb, 1.0); return; }

    // The turbulent column in front of what the pixel shows.
    float dS  = texture(sceneDepth, uv).r;                       // metres; kNoSurfaceT for sky
    float shS = dS >= 1e29 ? 1.0 : shareAt(dS, ts.y);
    vec4  ga  = textureGather(cloudA, uv, 3);
    vec4  gt  = textureGather(cloudB, uv, 1);
    float wc = 0.0, dc = 0.0;
    for (int k = 0; k < 4; ++k) {
        float o = clamp(1.0 - gt[k], 0.0, 1.0);
        wc += o;
        dc += o * abs(ga[k]) * 1000.0;
    }
    float var = shS;
    if (wc > 1e-3) {
        float opa = wc * 0.25;
        float shC = shareAt(min(dc / wc, dS), ts.y);
        var = mix(shS, shC, opa);
    }
    float amp = ts.x * sqrt(var) * u.params.y;   // rms (rad) per axis
    float ampPx = amp / u.params.x;
    if (ampPx < 0.004) { outColor = vec4(texelFetch(frameTex, ip, 0).rgb, 1.0); return; }

    // The field: two boiling phases x two cell sizes (a power-of-two LOD of the cell, cross-faded as the field of view
    // changes, so zooming never rescales a field: it would scramble). frac[i].w = the phase's x the level's weight.
    vec3 q0 = dd * u.mScale.xyz;   // level 0's cells; level 1's are twice as large
    // Three phases a third of a cycle apart (2026-10-28, issue 20261028_223915: with two the field paused twice a cycle).
    vec2 n = vec2(0.0);
    for (int i = 0; i < 6; ++i)
        n += u.frac[i].w * vnoise2(u.anc[i].xyz, u.frac[i].xyz + ((i & 1) != 0 ? 0.5 : 1.0) * q0, 0u);
    // Near the horizon the jitter is mostly vertical (the long, layered path); the CPU blends aniso.xy by elevation.
    vec2 dispPx = n * u.params.z * ampPx * u.aniso.xy;
    // Screen axes: x right, y down.
    vec2 src = gl_FragCoord.xy + vec2(dispPx.x, -dispPx.y);
    outColor = vec4(sampleCR(src), 1.0);
}
