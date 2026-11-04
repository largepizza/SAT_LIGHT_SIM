#version 450
// The turbulence warp (2026-10-28, issues 20261028_193115 "Detail Stars should flicker with turbulence" and
// 20261028_191408; the eddy band, sim time and the field in the air 2026-11-03, issue 20261103_191106 "Turbulence"):
// atmospheric seeing as a distortion of the WHOLE picture, after the main pass (sky, Milky Way and its star-cloud points,
// the Moon, the Sun, stars, planets, satellites) and before the UI. The frame is copied (SatelliteSimTurbulence.cpp)
// and resampled through a smooth displacement field plus a blur:
//  - the rms per axis at each pixel is the refraction table's angle-of-arrival rms at that pixel's APPARENT elevation
//    (Refraction.cpp: 2.91 D^-1/3 int Cn^2 ds), x the share of the turbulent column in front of the first surface the
//    pixel shows (terrain / sea / mesh from the scene depth, cloud from the composite: 1 - exp(-d / aoaS), aoaS the
//    Cn^2-weighted mean distance along that ray), faded out above the air;
//  - that variance is spread evenly over the octaves of the eddy band D / aoaS .. L0 / aoaS (aperture 1 cm .. outer
//    scale 0.4 x the air's height there, 1-50 m, x "Turbulence cell size"); six octaves of vector value noise on fixed angular cells carry the part
//    from ~2 px up as displacement (each at most 1/8 of its cell: it never folds), the finer part is a blur;
//  - each octave is a frozen 3D lattice in the air (ECEF metres, anchored on the CPU in double): fixed on the scene
//    when the camera turns, re-drawn when the eye moves, drifting with sim time.
// The Sun's and the Moon's discs do not jitter in sat_sky.frag: the warp moves them like everything else.

layout(set = 0, binding = 0) uniform TurbWarp {
    vec4  camR;      // camera right / up / forward in ECEF (xyz)
    vec4  camU;
    vec4  camF;
    vec4  up;        // the eye's up in ECEF
    vec4  screen;    // x W, y H (px), z tan(fov/2) x aspect, w tan(fov/2)
    vec4  params;    // x the pixel's angle (rad / px), y gain (the altitude fade), z 1 / (the field's rms), w 1 / (table n - 1)
    vec4  aniso;     // x horizontal amplitude, y vertical amplitude, z table eLo (rad), w table span (rad)
    vec4  band;      // x log2(aperture m x cell size), y log2(cell size), z log2(rad) where the octaves take over, w eye height (m)
    vec4  outer;     // the outer scale: x per metre of height, y min (m), z max (m)
    ivec4 anc[6];    // per octave: lattice anchor (mod 4096) xyz, seed w
    vec4  frac[6];   // xyz the anchor's fraction, w cells / rad
    vec4  oct[6];    // x log2(cell rad), y on-screen fade, z cell (px)
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

// The length of [a0, a1] inside [b0, b1].
float overlap(float a0, float a1, float b0, float b1) { return max(min(a1, b1) - max(a0, b0), 0.0); }

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

    vec2  ts  = tableAt(asin(clamp(dot(dir, u.up.xyz), -1.0, 1.0)));
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
    float sigPx = ts.x * sqrt(var) * u.params.y / u.params.x;   // total rms per axis (px)
    if (sigPx < 0.004) { outColor = vec4(texelFetch(frameTex, ip, 0).rgb, 1.0); return; }
    float sig2 = sigPx * sigPx;

    // The eddy band in log2(rad) at this pixel: aperture / aoaS .. outer scale / aoaS. Equal variance per octave; the
    // share above band.z is displacement, spread over the displayed octaves by their overlap with the band (x their
    // on-screen fade); the rest is blur.
    // the outer scale from the height of the air aoaS out along the ray (the eye's height + the climb + the Earth's drop)
    float sEl = dot(dir, u.up.xyz);
    float zT  = max(u.band.w + ts.y * sEl + ts.y * ts.y * 7.85e-8, 0.0);   // 7.85e-8 = 1 / (2 x 6371 km)
    float L0  = clamp(u.outer.x * zT, u.outer.y, u.outer.z);
    float la  = log2(max(ts.y, 1.0));
    float bLo = u.band.x - la, bHi = u.band.y + log2(L0) - la;
    float nB  = max(bHi - bLo, 1.0);
    float vShown = sig2 * overlap(u.band.z, 1e9, bLo, bHi) / nB;
    float wk[6];
    float wSum = 0.0;
    for (int i = 0; i < 6; ++i) {
        float lc = u.oct[i].x;
        // the coarsest displayed octave also stands for the band above it
        float w = overlap(lc - 0.5, i == 5 ? 1e9 : lc + 0.5, bLo, bHi) * u.oct[i].y;
        wk[i] = w;
        wSum += w;
    }
    vec2  n = vec2(0.0);
    float shown2 = 0.0;
    if (wSum > 1e-4 && vShown > 0.0) {
        for (int i = 0; i < 6; ++i) {
            if (wk[i] <= 0.0) continue;
            // rms (px) of this octave, capped at 1/8 of its cell (a field displaced by more than that folds)
            float a = min(sqrt(vShown * wk[i] / wSum), 0.125 * u.oct[i].z);
            shown2 += a * a;
            n += a * vnoise2(u.anc[i].xyz, u.frac[i].xyz + dd * u.frac[i].w, uint(u.anc[i].w));
        }
    }
    // Near the horizon the jitter is mostly vertical (the long, layered path); the CPU blends aniso.xy by elevation.
    vec2 dispPx = n * u.params.z * u.aniso.xy;
    // Screen axes: x right, y down.
    vec2 src = gl_FragCoord.xy + vec2(dispPx.x, -dispPx.y);
    vec3 col = sampleCR(src);
    // The rest of the variance (the sub-pixel eddies: the seeing disc) blurs: a ring of six bilinear taps at r around
    // the centre, equal weights (per-axis variance 3/7 r^2), at most 1.5 px (wider rings would show as copies).
    float blur = sqrt(max(sig2 - shown2, 0.0));
    if (blur > 0.3) {
        float r = 1.528 * min(blur, 1.5);
        vec3 acc = col;
        for (int k = 0; k < 6; ++k) {
            float an = 1.0471976 * float(k) + 0.2617994;
            acc += texture(frameTex, (src + r * vec2(cos(an), sin(an))) / u.screen.xy).rgb;
        }
        col = acc / 7.0;
    }
    outColor = vec4(col, 1.0);
}
