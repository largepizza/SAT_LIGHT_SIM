#version 450
// Resolved planet discs (CLAUDE.md "Planets": the resolved discs). Per pixel, 4 samples (rotated grid) of:
//   the ball: an oblate spheroid (pole, polar / equatorial ratio), its map (layer x0.w, sqrt-encoded ratio to the map's
//     mean, tools/make_planet_maps.py) or a flat albedo, lit by a Lommel-Seeliger (rocky) or Minnaert (clouds) law;
//     the light per unit law / <law> (lin) makes the disc's total the planet's own magnitude (the CPU's integral);
//   Jupiter: the Galilean moons' shadows; Saturn: the rings (ray / plane, a radial profile layer: albedo, optical depth),
//     their shadow on the ball, the ball's shadow on them, the lit or the unlit face;
//   Venus: the atmosphere's rim, forward-scattered sunlight round the limb (a full ring near inferior conjunction).
// The four samples' linear light is averaged, tonemapped as the sky is (AgX, the tone settings), x the share and the
// suppression; the point part (1 - share, the point style's own Gaussian) is added, and the cloud composite in front
// dims both as it dims the stars.
#include "planet_disc.glsl"

layout(set = 0, binding = 1) uniform sampler2D cloudTargetA;
layout(set = 0, binding = 2) uniform sampler2D cloudTargetB;
layout(set = 0, binding = 3) uniform sampler2D sceneDepthTex;
layout(set = 0, binding = 4) uniform sampler2DArray planetMaps;

layout(push_constant) uniform PC {   // PointDrawPC
    mat4  skyView;
    float fovYRad;
    float aspect;
    float waveTime;
    float noTwinkle;
    vec4  moonDirENU;
    vec4  obsECEFDir;
    vec2  screenSizePx;
    uint  debugDisableMask;
    float manualTerrainTest;
} pc;

layout(location = 0) in vec2 vQ;
layout(location = 1) flat in int vDisc;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265;

vec3 agx(vec3 v)
{
    const mat3 kIn  = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                           0.0784335999999992, 0.878468636469772, 0.0784336,
                           0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 kOut = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                           -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                           -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    v = clamp(log2(max(kIn * v, vec3(1e-10))), minEv, maxEv);
    v = (v - minEv) / (maxEv - minEv);
    vec3 v2 = v * v, v4 = v2 * v2;
    v = 15.5 * v4 * v2 - 40.14 * v4 * v + 31.96 * v4 - 6.868 * v2 * v + 0.4298 * v2 + 0.1191 * v - 0.00232;
    float l = dot(v, vec3(0.2126, 0.7152, 0.0722));
    v = pow(max(v, vec3(0.0)), vec3(pd.tone.z));
    v = vec3(pow(max(l, 0.0), pd.tone.z)) + pd.tone.y * (v - vec3(pow(max(l, 0.0), pd.tone.z)));
    return pow(clamp(kOut * v, 0.0, 1.0), vec3(2.2));
}
vec3 tonemap(vec3 x)
{
    if (pd.tone.x > 0.5) return agx(x);
    return mix(vec3(1.0) - exp(-x), vec3(1.0) - 1.0 / (vec3(1.0) + x + 0.5 * x * x), pd.tone.w);
}

// The nearest intersection (largest z) of the line x = q (along z) with the spheroid |p|^2 + k (p.P)^2 = 1.
bool spheroidHit(vec2 q, vec3 P, float k, out float z)
{
    float s0 = q.x * P.x + q.y * P.y;
    float A = 1.0 + k * P.z * P.z, B = 2.0 * k * P.z * s0, C = dot(q, q) + k * s0 * s0 - 1.0;
    float disc = B * B - 4.0 * A * C;
    z = (-B + sqrt(max(disc, 0.0))) / (2.0 * A);
    return disc >= 0.0;
}
// Does the ray o + t d (t > 0) meet the spheroid?
bool spheroidRay(vec3 o, vec3 d, vec3 P, float k)
{
    float od = dot(o, P), dd = dot(d, P);
    float A = dot(d, d) + k * dd * dd, B = 2.0 * (dot(o, d) + k * od * dd), C = dot(o, o) + k * od * od - 1.0;
    float disc = B * B - 4.0 * A * C;
    if (disc < 0.0) return false;
    float t = (-B + sqrt(disc)) / (2.0 * A);
    return t > 0.0;
}

vec4 ringProfile(float rho, float layer, float lod)
{
    float u = (rho - 1.10) / 1.25;
    if (u <= 0.0 || u >= 1.0) return vec4(0.0);
    vec4 t = textureLod(planetMaps, vec3(u, 0.5, layer), lod);
    return vec4(t.rgb, t.a * 4.0);
}

vec3 shade(PlanetDisc D, vec2 q, float pxR, float lodBias)
{
    vec3 P = D.pole.xyz, s = D.sun.xyz;
    float qa = D.pole.w, k = 1.0 / (qa * qa) - 1.0;
    vec3 L = vec3(0.0);
    float z;
    bool hit = spheroidHit(q, P, k, z);
    float ringZ = -1e9;
    vec3 ringL = vec3(0.0);
    float ringT = 1.0;
    float ringLayer = float(pd.head.z);
    // Saturn's rings: where the line meets the ring plane.
    if (D.ring.x > 0.0 && abs(P.z) > 1e-5) {
        float zr = -(q.x * P.x + q.y * P.y) / P.z;
        vec3 pr = vec3(q, zr);
        float rho = length(pr);
        if (rho > 1.10 && rho < D.ring.x) {
            float muV = abs(P.z), sP = dot(s, P), mu0 = abs(sP);
            // The profile's level from the footprint across the rings (radial, foreshortened by the opening).
            float lodR = log2(max(pxR / max(muV, 0.02) * 1024.0 / 1.25, 1.0)) + lodBias;
            vec4 pf = ringProfile(rho, ringLayer, lodR);
            float tau = pf.a;
            bool litFace = (sP > 0.0) == (P.z > 0.0);
            float IF;
            if (litFace)
                IF = 2.4 * mu0 / max(mu0 + muV, 1e-4) * (1.0 - exp(-tau * (1.0 / max(mu0, 0.02) + 1.0 / muV)));
            else   // the unlit face: light diffusing through (strongest at moderate optical depth)
                IF = 0.35 * (tau / max(mu0, 0.02)) * exp(-tau / max(mu0, 0.02)) * exp(-0.5 * tau / muV);
            // The ball's shadow on the rings.
            float shadow = spheroidRay(pr, s, P, k) ? 0.0 : 1.0;
            ringL = pf.rgb * IF * shadow * D.ring.y;   // (pf.rgb: the particles' albedo x colour)
            ringT = exp(-tau / muV);
            ringZ = zr;
        }
    }
    if (hit) {
        vec3 p = vec3(q, z);
        vec3 n = normalize(p + k * dot(p, P) * P);
        float mu = max(n.z, 1e-3), mu0 = dot(n, s);
        float law = 0.0;
        if (mu0 > 0.0) {
            if (D.sun.w < 0.5) law = 2.0 * mu0 / (mu0 + mu);
            else law = pow(mu0, D.tint.w) * pow(mu, D.tint.w - 1.0);
        }
        vec3 alb = D.tint.rgb;
        if (D.x0.w >= 0.0 && law > 0.0) {
            vec3 pn = normalize(p);
            vec3 X0 = D.x0.xyz, Y0 = cross(P, X0);
            float lat = asin(clamp(dot(pn, P), -1.0, 1.0));
            float lon = atan(dot(pn, Y0), dot(pn, X0));
            vec2 uv = vec2(0.5 + lon / (2.0 * PI) + D.pt.w, 0.5 - lat / PI);
            float lod = log2(max(pxR / max(mu, 0.12) * 512.0 / PI, 1.0)) + lodBias;
            vec3 t = textureLod(planetMaps, vec3(uv, D.x0.w), lod).rgb;
            alb *= t * t * 8.0;
        }
        L = D.lin.rgb * alb * law;
        // Jupiter: the Galilean moons' shadows (soft by the Sun's disc at their distance, and the pixel).
        if (int(pd.head.y) == vDisc && law > 0.0) {
            for (int m = 0; m < 4; ++m) {
                vec3 dm = pd.moons[m].xyz - p;
                float al = dot(dm, s);
                if (al > 0.0) {
                    float perp = length(dm - s * al);
                    float rm = pd.moons[m].w, pw = 0.00093 * al + 0.5 * pxR;
                    L *= mix(0.03, 1.0, smoothstep(rm - pw, rm + pw, perp));
                }
            }
        }
        // Saturn: the rings' shadow on the ball.
        if (D.ring.x > 0.0 && law > 0.0) {
            float sP = dot(s, P);
            if (abs(sP) > 1e-4) {
                float t = -dot(p, P) / sP;
                if (t > 0.0) {
                    float rho = length(p + t * s);
                    vec4 pf = ringProfile(rho, ringLayer, log2(max(pxR * 1024.0 / 1.25 / max(abs(sP), 0.02), 1.0)));
                    L *= exp(-pf.a / max(abs(sP), 0.02));
                }
            }
        }
    }
    // Compose: the rings in front of the ball, or behind it.
    if (ringZ > -1e8) {
        if (!hit) L = ringL;
        else if (ringZ > z) L = ringL + L * ringT;
    }
    // Venus: the atmosphere's rim, forward-scattered round the limb; a full ring as the Sun gets behind the planet.
    if (!hit && D.ring.z > 0.0) {
        float r = length(q);
        float dl = r - 1.0;
        float sig = max(0.6 * pxR, 0.0035);
        vec2 sxy = s.xy;
        float lenS = length(sxy);
        float cosA = lenS > 1e-4 ? dot(q / max(r, 1e-6), sxy / lenS) : 1.0;
        float behind = smoothstep(0.94, 0.995, -s.z);   // near inferior conjunction the ring closes
        float along = mix(pow(max(0.5 + 0.5 * cosA, 0.0), 2.0), 1.0, behind);
        L += D.ring.y * D.ring.z * (0.0035 / sig) * exp(-0.5 * dl * dl / (sig * sig)) * along;
    }
    return L;
}

void main() {
    PlanetDisc D = pd.disc[vDisc];
    float pxR = D.by.w;
    float lodBias = log2(max(D.misc.x, 1.0));
    vec3 Lsum = vec3(0.0);
    if (D.lin.w > 0.0) {
        const vec2 o[4] = vec2[4](vec2(-0.125, -0.375), vec2(0.375, -0.125), vec2(0.125, 0.375), vec2(-0.375, 0.125));
        for (int i = 0; i < 4; ++i)
            Lsum += shade(D, vQ + o[i] * pxR * max(D.misc.x, 1.0), pxR, lodBias);
        Lsum *= 0.25;
    }
    // Not x the suppression (pt.z): that is the point model's stand-in for the sky's glare and glow, which a resolved
    // surface meets in the frame itself (it is added to the sky as the Moon's disc is).
    vec3 col = tonemap(Lsum) * D.lin.w;
    // The point it hands over from (the point style's Gaussian, pt.x already x (1 - share)).
    if (D.pt.x > 0.0) {
        float rpx = length(vQ) / pxR;
        vec3 tc = D.tint.rgb / max(max(D.tint.r, max(D.tint.g, D.tint.b)), 1e-4);
        col += tc * D.pt.x * exp(-rpx * rpx / (2.0 * D.pt.y * D.pt.y));
    }
    // The cloud composite in front (as star_point.frag), and terrain where the depth buffer cannot (render scale < 1).
    vec2 uv = gl_FragCoord.xy / pc.screenSizePx;
    vec4 cA = texture(cloudTargetA, uv);
    vec4 cB = texture(cloudTargetB, uv);
    float vis = (cA.a >= 0.0 ? 0.0 : 1.0) * pow(clamp(dot(cB.rgb, vec3(1.0 / 3.0)), 0.0, 1.0), 2.0);
    if (pc.manualTerrainTest >= 0.5 && texture(sceneDepthTex, uv).r < 5.0e29) vis = 0.0;
    col *= vis;
    outColor = vec4(col, dot(col, vec3(0.2126, 0.7152, 0.0722)));
}
