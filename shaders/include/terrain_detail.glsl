#ifndef SATLIGHTSIM_TERRAIN_DETAIL_GLSL
#define SATLIGHTSIM_TERRAIN_DETAIL_GLSL

// ── Procedural terrain detail: a real 3D heightfield on top of the DEM ─────────────────────────
//
// Requires common.glsl, cloud_params.glsl (the `cloud` UBO) and terrain.glsl, in that order.
// Shared by every pass that needs to agree on where the ground is: sat_sky.frag (the march that
// draws it), scene_depth.comp (the shared depth every volumetric clamps to) and cloud_march.comp
// (the observer's own height). Docs: CLAUDE.md "Procedural terrain detail".
//
// The DEM is 14999x7500 (2.67 km/texel) and 8-bit (~37 m steps). From the ground it is a set of
// smooth blobs. This adds eight octaves of value noise, 2048 m down to 16 m cells, to the HEIGHT
// the march intersects — silhouettes, ridgelines and occlusion change, not only shading:
//
//   H(p) = DEM(p) + D(p),  D = sum_k amp_k * noise(p / cell_k) / (1 + erode * |slope so far|^2)
//
// The slope damping (after IQ's "Elevated") keeps steep flanks smooth and lets fine detail gather
// on ridges and in valleys — the look of eroded terrain without a simulation. amp_0 scales with
// the local relief of the DEM (|DEM - DEM at mip 3|, i.e. relative to ~21 km around it) and with
// elevation, so plains stay nearly flat and mountains become rugged; it fades to zero at the coast.
//
// World-fixed without float precision loss: the noise domain is ECEF metres on the sea-level
// sphere, but never formed as an absolute coordinate (6.4e6 m has 0.5 m float steps). The CPU
// (double) passes the observer's sea-level point as an integer 2048-m cell (terrainAnchorCell) and
// the offset inside it (terrainAnchorRel); the shader adds each point's small offset from the
// observer to the latter and the cell index to the lattice as INTEGERS (<< k per octave). Nothing
// swims as the observer moves, anywhere on the globe. Every height is formed the same way from the
// observer-relative offset q (see tdAltitude), for the same reason.
//
// LOD: an octave is geometry only while its cell is at least ~2x the march's resolution at that
// distance (tdGeomLodM: max(2 px footprint, 1.2% of the distance)); finer octaves still reach the
// shading normal (tdShadeLodM, ~1.5 px). So near ground is fully 3D, far ridgelines keep the large
// octaves, and nothing aliases.
//
// Cost (the first cut was 15-28 ms of extra sky pass at ground level, measured with the harness):
// the octaves are evaluated in two stages. The march steps on the COARSE surface (DEM + the first
// kTdCoarseOctaves) and only runs the fine octaves on samples within their own amplitude bound of
// it; above the local bound of all the detail it steps on the DEM alone.

const int   kTdOctaves       = 8;
const int   kTdCoarseOctaves = 4;
const float kTdBaseCellM     = 2048.0;

// Off in the sky shader's environment-probe and Planetarium variants: a probe renders from a
// satellite (the anchor is the MAIN observer's, and the relief is sub-texel there anyway), and
// SKY_LITE exists to be cheap.
bool tdEnabled() {
#if defined(SKY_ENV) || defined(SKY_LITE)
    return false;
#else
    return cloud.terrainDetailStrength > 0.0;
#endif
}

// lowbias32 (two multiply-xorshift rounds) on a linear combination of the lattice coordinates.
// Do NOT drop to one round to save ALU: that was tried (2026-09-25) and the lattice values became
// correlated along one direction — the detail streaked into screen-wide stripes and lost its zero
// mean (the "detail" debug view read saturated everywhere).
float tdRand(ivec3 c) {
    uvec3 v = uvec3(c);
    uint h = v.x * 0x8da6b343u ^ v.y * 0xd8163841u ^ v.z * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return float(h >> 8) * (2.0 / 16777215.0) - 1.0;
}

// 3D value noise with its analytic gradient (per unit of x). x is the local lattice coordinate,
// cellOff the integer lattice offset of the anchor.
vec4 tdNoised(vec3 x, ivec3 cellOff) {
    vec3  fl = floor(x);
    ivec3 i  = ivec3(fl) + cellOff;
    vec3  w  = x - fl;
    vec3  u  = w * w * w * (w * (w * 6.0 - 15.0) + 10.0);
    vec3  du = 30.0 * w * w * (w * (w - 2.0) + 1.0);
    float a = tdRand(i);
    float b = tdRand(i + ivec3(1, 0, 0));
    float c = tdRand(i + ivec3(0, 1, 0));
    float d = tdRand(i + ivec3(1, 1, 0));
    float e = tdRand(i + ivec3(0, 0, 1));
    float f = tdRand(i + ivec3(1, 0, 1));
    float g = tdRand(i + ivec3(0, 1, 1));
    float h = tdRand(i + ivec3(1, 1, 1));
    float k1 = b - a, k2 = c - a, k3 = e - a, k4 = a - b - c + d;
    float k5 = a - c - e + g, k6 = a - b - e + f, k7 = -a + b + c - d + e - f - g + h;
    float v = a + k1 * u.x + k2 * u.y + k3 * u.z + k4 * u.x * u.y + k5 * u.y * u.z + k6 * u.z * u.x
            + k7 * u.x * u.y * u.z;
    vec3 gr = du * vec3(k1 + k4 * u.y + k6 * u.z + k7 * u.y * u.z,
                        k2 + k5 * u.z + k4 * u.x + k7 * u.z * u.x,
                        k3 + k6 * u.x + k5 * u.y + k7 * u.x * u.y);
    return vec4(v, gr);
}

// 3D gradient (Perlin) noise with its analytic gradient, same lattice convention. Used for the
// shading-only micro octaves: value noise's lattice shows up in the lighting at pebble scale as
// sheared boxes (seen in a 3x harness crop of the Grand Canyon foreground); gradient noise does not.
vec3 tdGrad(ivec3 c) {
    uvec3 v = uvec3(c);
    uint h = v.x * 0x8da6b343u ^ v.y * 0xd8163841u ^ v.z * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    uint h2 = h * 0x9e3779b9u;
    return normalize(vec3(float(h & 1023u), float((h >> 10) & 1023u), float(h2 >> 22)) - 511.5 + 1e-3);
}
vec4 tdGradNoised(vec3 x, ivec3 cellOff) {
    vec3  fl = floor(x);
    ivec3 i  = ivec3(fl) + cellOff;
    vec3  w  = x - fl;
    vec3  u  = w * w * w * (w * (w * 6.0 - 15.0) + 10.0);
    vec3  du = 30.0 * w * w * (w * (w - 2.0) + 1.0);
    vec3 ga = tdGrad(i), gb = tdGrad(i + ivec3(1, 0, 0)), gc = tdGrad(i + ivec3(0, 1, 0)), gd = tdGrad(i + ivec3(1, 1, 0));
    vec3 ge = tdGrad(i + ivec3(0, 0, 1)), gf = tdGrad(i + ivec3(1, 0, 1)), gg = tdGrad(i + ivec3(0, 1, 1)), gh = tdGrad(i + ivec3(1, 1, 1));
    float va = dot(ga, w), vb = dot(gb, w - vec3(1, 0, 0)), vc = dot(gc, w - vec3(0, 1, 0)), vd = dot(gd, w - vec3(1, 1, 0));
    float ve = dot(ge, w - vec3(0, 0, 1)), vf = dot(gf, w - vec3(1, 0, 1)), vg = dot(gg, w - vec3(0, 1, 1)), vh = dot(gh, w - vec3(1, 1, 1));
    float k1 = vb - va, k2 = vc - va, k3 = ve - va, k4 = va - vb - vc + vd;
    float k5 = va - vc - ve + vg, k6 = va - vb - ve + vf, k7 = -va + vb + vc - vd + ve - vf - vg + vh;
    float v = va + k1 * u.x + k2 * u.y + k3 * u.z + k4 * u.x * u.y + k5 * u.y * u.z + k6 * u.z * u.x + k7 * u.x * u.y * u.z;
    vec3 g = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.z * (ge - ga) + u.x * u.y * (ga - gb - gc + gd)
           + u.y * u.z * (ga - gc - ge + gg) + u.z * u.x * (ga - gb - ge + gf)
           + u.x * u.y * u.z * (-ga + gb + gc - gd + ge - gf - gg + gh)
           + du * vec3(k1 + k4 * u.y + k6 * u.z + k7 * u.y * u.z,
                       k2 + k5 * u.z + k4 * u.x + k7 * u.z * u.x,
                       k3 + k6 * u.x + k5 * u.y + k7 * u.x * u.y);
    return vec4(v * 1.6, g * 1.6); // ~[-1, 1] like the value noise
}

// ── Observer-relative geometry, all without forming a 6.4e6-m coordinate ─────────────────────
// q = point - (the observer's sea-level point), in the observer's ENU axes. The march builds it as
// vec3(0, 0, hEye) + t * dir.

// Height above the sea-level sphere. |p|^2 - R^2 = 2 R q.z + |q|^2, divided by |p| + R.
float tdAltitude(vec3 q) {
    float len = length(vec3(q.xy, R_EARTH + q.z));
    return (2.0 * R_EARTH * q.z + dot(q, q)) / (len + R_EARTH);
}

// The point on the sea-level sphere below q, as an offset from the observer's sea-level point
// (ENU axes). R*p/|p| - (0,0,R) with its z formed as -R |q.xy|^2 / ((R+q.z+|p|) |p|): exact.
vec3 tdSphereOffset(vec3 q) {
    float rz  = R_EARTH + q.z;
    float len = length(vec3(q.xy, rz));
    float s   = R_EARTH / len;
    float r2  = dot(q.xy, q.xy);
    return vec3(q.x * s, q.y * s, -R_EARTH * r2 / ((rz + len) * len));
}

// ECEF unit up at q.
vec3 tdUpECEF(vec3 q, vec3 enuX, vec3 enuY, vec3 enuZ) {
    return normalize(q.x * enuX + q.y * enuY + (R_EARTH + q.z) * enuZ);
}

vec2 tdUV(vec3 q, vec3 enuX, vec3 enuY, vec3 enuZ) {
    return posToUV(q.x * enuX + q.y * enuY + (R_EARTH + q.z) * enuZ);
}

// The DEM texel coordinate of q (texel-centre convention), relative to the observer's texel from
// the CPU, with the lon/lat offsets formed from the SMALL ECEF offset of q — no cancellation. The
// float UV route (atan of an absolute ECEF position, then *W) resolves only ~2.4 m on the ground, and
// the hardware bilinear filter adds 8-bit sub-texel weights (~10 m steps): together they built
// metre-high shelves on a steep wall seen from 10 m (found with the harness, a valley-wall close-up).
//
// Only used within kTdExactDemM of the observer, so the offset is the observer's local projection
// (east/north metres over R cos(lat) and R): smooth, a few metres off true lon/lat at 4 km, which is
// invisible — quantization STEPS are what show, and this has none. (Exact spherical angles via two
// atans cost measurably more in the depth pass for no visible difference.)
void tdDemTexel(vec3 q, vec3 enuX, vec3 enuY, vec3 enuZ, vec2 size, out ivec2 base, out vec2 frac) {
    float cosLat = max(length(enuZ.xy), 1e-3);
    float dLon = q.x / (R_EARTH * cosLat);
    float dLat = q.y / R_EARTH;
    vec2  t = cloud.terrainObsTexel.zw + vec2(dLon * size.x / (2.0 * PI), -dLat * size.y / PI);
    vec2  fl = floor(t);
    base = ivec2(cloud.terrainObsTexel.xy + fl);
    frac = t - fl;
}

float tdDemBilinear(sampler2D elevTex, ivec2 base, vec2 f, ivec2 sz) {
    int x0 = ((base.x % sz.x) + sz.x) % sz.x, x1 = (x0 + 1) % sz.x;
    int y0 = clamp(base.y, 0, sz.y - 1), y1 = clamp(base.y + 1, 0, sz.y - 1);
    float a = texelFetch(elevTex, ivec2(x0, y0), 0).r, b = texelFetch(elevTex, ivec2(x1, y0), 0).r;
    float c = texelFetch(elevTex, ivec2(x0, y1), 0).r, e = texelFetch(elevTex, ivec2(x1, y1), 0).r;
    return mix(mix(a, b, f.x), mix(c, e, f.x), f.y);
}

// DEM at q: mip 0 (the height) and mip 3 (roughness — the ~21 km mean needs no precision).
// Water-masked like terrain.glsl. Within kTdExactDemM (horizontally) of the observer the height is
// the exact bilinear at the observer-relative texel (tdDemTexel); beyond it, where 2.4 m and 10 m
// steps are far below a pixel, the hardware filter at the float UV (1 fetch instead of 4 plus the
// angle math — the exact path everywhere measured +1 ms in the depth pass). The choice depends on q
// alone, so every pass makes the same one for the same point.
const float kTdExactDemM = 4000.0;
void tdDemAt(sampler2D elevTex, sampler2D specTex, vec3 q, vec3 enuX, vec3 enuY, vec3 enuZ,
             out float h0, out float hMip3) {
    ivec2 sz = textureSize(elevTex, 0);
    vec2  uv;
    if (dot(q.xy, q.xy) < kTdExactDemM * kTdExactDemM) {
        ivec2 base;
        vec2  f;
        tdDemTexel(q, enuX, enuY, enuZ, vec2(sz), base, f);
        uv = (vec2(base) + f + 0.5) / vec2(sz);
        h0 = max(0.0, tdDemBilinear(elevTex, base, f, sz) * kElevRange - kElevOffset);
    } else {
        uv = tdUV(q, enuX, enuY, enuZ);
        h0 = max(0.0, textureLod(elevTex, uv, 0.0).r * kElevRange - kElevOffset);
    }
    hMip3 = max(0.0, textureLod(elevTex, uv, 3.0).r * kElevRange - kElevOffset);
    if (h0 < kWaterMaskMaxM && textureLod(specTex, uv, 0.0).r > 0.5) { h0 = 0.0; hMip3 = 0.0; } // sea (terrain.glsl)
}

// Roughness from the DEM around a point: SIGNED relief against the ~21 km mean (mip 3) — ridges and
// peaks (above their surroundings) are rugged, valley floors (below them) are the flattest ground
// there is — plus elevation. The first cut used |relief| and dug 250 m pits into every Alpine valley
// floor, where the observer stands.
float tdRoughness(float h0, float hMip3) {
    float relief = h0 - hMip3;
    return clamp(0.12 + 0.88 * smoothstep(-250.0, 450.0, relief) * (0.35 + 0.65 * smoothstep(400.0, 2500.0, h0)),
                 0.06, 1.0);
}

// The octave-0 amplitude at a point (0 on the sea, fading in over the first 80 m of land).
float tdAmp0(float h0, float hMip3) {
    return cloud.terrainDetailAmpM * cloud.terrainDetailStrength * tdRoughness(h0, hMip3) * smoothstep(0.0, 80.0, h0);
}

// Upper bound on what octaves k.. can still add, given octave k's amplitude.
float tdTailBound(float ampK) { return ampK / max(1.0 - cloud.terrainDetailGain, 0.05); }

// ── The detail field, evaluated in stages ─────────────────────────────────────────────────────
struct TdState {
    float h;      // metres added to the DEM so far
    vec3  grad;   // tangential gradient of h (ECEF, metres per metre)
    vec3  slope;  // accumulated slope driving the damping
    float amp;    // amplitude of the NEXT octave
    float cell;   // cell size of the next octave
    int   k;      // next octave
    float rough;  // 0..1 roughness (material/AO)
    float amp0;   // octave-0 amplitude
    vec3  rel;    // noise-domain position (ECEF metres from the anchor cell origin)
    vec3  up;     // ECEF up
};

TdState tdBegin(vec3 q, vec3 enuX, vec3 enuY, vec3 enuZ, float h0, float hMip3) {
    TdState s;
    s.h = 0.0; s.grad = vec3(0.0); s.slope = vec3(0.0);
    s.rough = tdRoughness(h0, hMip3);
    s.amp0  = tdAmp0(h0, hMip3);
    s.amp   = s.amp0;
    s.cell  = kTdBaseCellM;
    s.k     = 0;
    vec3 off = tdSphereOffset(q);
    s.rel = cloud.terrainAnchorRel.xyz + off.x * enuX + off.y * enuY + off.z * enuZ;
    s.up  = tdUpECEF(q, enuX, enuY, enuZ);
    return s;
}

// Run octaves s.k .. kEnd-1 (and stop at the LOD: octaves with a cell below lodM are dropped,
// faded in between lodM and 2*lodM).
void tdOctaves(inout TdState s, float lodM, int kEnd) {
    ivec3 anchor = ivec3(cloud.terrainAnchorCell.xyz);
    for (; s.k < kEnd; ++s.k) {
        float fade = smoothstep(lodM, 2.0 * lodM, s.cell);
        if (fade <= 0.0 || s.amp <= 0.0) { s.k = kTdOctaves; break; }
        vec4 n  = tdNoised(s.rel / s.cell, anchor << s.k);
        vec3 g  = n.yzw / s.cell;
        g -= s.up * dot(g, s.up);                         // tangential part only
        s.slope += g * s.amp;
        float damp = 1.0 / (1.0 + cloud.terrainDetailErode * dot(s.slope, s.slope));
        s.h    += s.amp * fade * damp * n.x;
        s.grad += s.amp * fade * damp * g;
        s.amp  *= cloud.terrainDetailGain;
        s.cell *= 0.5;
    }
}

// The full detail at q (for shading, AO, and the debug views).
struct TdSample {
    float h;     // metres added to the DEM
    vec3  grad;  // tangential gradient of h (ECEF, metres per metre)
    float rough; // 0..1 roughness used (material/AO)
    float amp;   // the octave-0 amplitude used (metres)
};

// Shading-only micro relief: octaves 8..13 (8 m down to 0.25 m cells) — see tdMicroBump.
const int kTdMicroFirst = 8;
const int kTdMicroLast  = 13;
TdSample terrainDetail(vec3 q, vec3 enuX, vec3 enuY, vec3 enuZ, float lodM, float h0, float hMip3, int kEnd) {
    TdState st = tdBegin(q, enuX, enuY, enuZ, h0, hMip3);
    tdOctaves(st, lodM, kEnd);
    TdSample s;
    s.h = st.h; s.grad = st.grad; s.rough = st.rough; s.amp = st.amp0;
    return s;
}

// The TRUE 3D point in the noise domain (not projected to the sea-level sphere like the heightfield's
// octaves): solid noise for the surface texture, so a steep face seen edge-on is not a vertical smear
// of one ground-plan value (the first cut's "curtains" on every hillside facing the observer).
vec3 tdSolidRel(vec3 q, vec3 enuX, vec3 enuY, vec3 enuZ) {
    return cloud.terrainAnchorRel.xyz + q.x * enuX + q.y * enuY + q.z * enuZ;
}

// Shading-only micro relief (rock and turf in the lighting within a few hundred metres, which the
// fractal alone left as smooth plastic): gradient noise at a constant SLOPE (amplitude proportional
// to the cell), solid in 3D, returned as the part of its gradient tangent to the macro normal nE
// (ECEF) — subtract it from nE. Gradient noise, not value noise: value noise's lattice showed as
// sheared boxes in the lighting at pebble scale.
vec3 tdMicroBump(vec3 q, vec3 nE, vec3 enuX, vec3 enuY, vec3 enuZ, float lodM, float rough) {
    vec3  rel = tdSolidRel(q, enuX, enuY, enuZ);
    ivec3 anchor = ivec3(cloud.terrainAnchorCell.xyz);
    vec3  gsum = vec3(0.0);
    float slopeAmp = 0.22 * max(rough, 0.35) * cloud.terrainDetailStrength;
    for (int k = kTdMicroFirst; k <= kTdMicroLast; ++k) {
        float cell = kTdBaseCellM / float(1 << k);
        float fade = smoothstep(lodM, 2.0 * lodM, cell);
        if (fade <= 0.0) break;
        vec4 n = tdGradNoised(rel / cell, anchor << k);
        gsum += fade * slopeAmp * n.yzw;               // amplitude slopeAmp*cell, per metre: cancels
    }
    return gsum - nE * dot(gsum, nE);
}

// World-fixed albedo mottle in [-1, 1] at q: four octaves of the same anchored value noise
// (32, 8, 2 and 0.5 m cells, decorrelated from the height field by a lattice offset), solid in 3D,
// each faded out once the pixel footprint `foot` reaches its cell size, so it never shimmers.
float tdMicro(vec3 q, vec3 enuX, vec3 enuY, vec3 enuZ, float foot) {
    vec3  rel = tdSolidRel(q, enuX, enuY, enuZ);
    ivec3 anchor = ivec3(cloud.terrainAnchorCell.xyz);
    float m = 0.0, wsum = 0.0;
    for (int j = 0; j < 4; ++j) {
        int   k    = 6 + 2 * j;               // 32, 8, 2, 0.5 m
        float cell = kTdBaseCellM / float(1 << k);
        float w    = 1.0 - smoothstep(0.12 * cell, 0.4 * cell, foot);
        if (w <= 0.0) break;
        m    += w * tdNoised(rel / cell, (anchor << k) + ivec3(911, 373, 1297)).x;
        wsum += 1.0;
    }
    return wsum > 0.0 ? m / wsum : 0.0;
}

// Geometry and shading LODs for a point at distance t along a ray with the given pixel angle.
// lodScale > 1 coarsens the geometry (scene_depth.comp, which only needs occlusion).
float tdGeomLodM(float t, float pixAngle) { return max(2.0 * pixAngle * t, 0.012 * t); }
// 3 px: finer normal detail than that aliases into a paper-like grain at a few km.
float tdShadeLodM(float t, float pixAngle) { return max(3.0 * pixAngle * t, 0.25); }

// Full terrain height (DEM + detail) at q. h0 returns the DEM part.
float terrainHeightDetailed(sampler2D elevTex, sampler2D specTex, vec3 q, vec3 enuX, vec3 enuY, vec3 enuZ,
                            float lodM, int kEnd, out float h0) {
    float hMip3;
    tdDemAt(elevTex, specTex, q, enuX, enuY, enuZ, h0, hMip3);
    if (!tdEnabled()) return h0;
    TdState st = tdBegin(q, enuX, enuY, enuZ, h0, hMip3);
    tdOctaves(st, lodM, kEnd);
    return h0 + st.h;
}

// The observer's ground: DEM + full detail right under the eye, so the eye (observerPos, +2 m) can
// never sit inside a detail bump. The one definition for every pass that shares the depth buffer.
float observerEffHeightDetailed(sampler2D elevTex, sampler2D specTex, vec4 obsECEFDir) {
    vec3 enuX, enuY, enuZ;
    enuBasis(obsECEFDir.xyz, enuX, enuY, enuZ);
    float h0;
    float g = terrainHeightDetailed(elevTex, specTex, vec3(0.0), enuX, enuY, enuZ, 0.0, kTdOctaves, h0);
    return max(max(g, 0.0), max(0.0, obsECEFDir.w));
}

// ── The march ─────────────────────────────────────────────────────────────────────────────────
// Returns the distance to the first terrain hit along dir, or -1. hEye = observer height above
// the sea-level sphere (obsEffH + 2). tExit bounds the search (sea-level hit, shell exit, reach).
//
// Per sample, from cheapest to dearest:
//   - above DEM + the local bound on ALL the detail: step on that gap (DEM fetches only);
//   - else run the coarse octaves; above the coarse surface + the bound on the fine ones: step on
//     that gap;
//   - else run the fine octaves too and step on the true gap.
// Steps are a fraction of the gap (relaxation), never below minStep(t); a sign change is refined
// by bisection on the same height function; passing within one pixel footprint of the surface is
// a hit. Grazing rays are what cost: that, the step floor (1% of t, rising with the step count)
// and maxSteps bound them. lodScale coarsens the geometry LOD.
//
// envelope = true (scene_depth.comp): return where the ray first meets the coarse surface PLUS the
// bound on every octave it did not evaluate, never running the fine octaves. That is always at or
// before the true surface, whatever the LOD — which is what makes it a safe seed for sat_sky.frag's
// full-resolution march (it starts there) — and cheap. Consumers of the shared depth (cloud and
// beam clamps, the cloud-shadow start point) see a surface at most that bound (~30 m on the most
// rugged terrain) above the real one.
float terrainMarchDetailed(sampler2D elevTex, sampler2D specTex, float hEye, vec3 dir,
                           vec3 enuX, vec3 enuY, vec3 enuZ, float tStart, float tExit, float pixAngle,
                           int maxSteps, float lodScale, bool envelope, int coarseK, out int stepsUsed) {
    stepsUsed = 0;
    bool  detail = tdEnabled();
    float t = max(tStart, 2.0), tPrev = t;
    // An envelope hit only counts once the ray has been OUTSIDE the envelope: an eye 2 m above the
    // ground starts inside it (the fine bound is tens of metres), and without this every ground-
    // level ray would "hit" at t = 2 — a useless seed.
    bool armed = false;
    float prevGap = 0.0;        // the TRUE gap at tPrev, when it was computed (regula falsi)
    bool  prevGapValid = false;
    for (int i = 0; i < maxSteps; ++i) {
        if (t > tExit) break;
        stepsUsed = i + 1;
        vec3  q    = vec3(0.0, 0.0, hEye) + t * dir;
        float rayH = tdAltitude(q);
        if (rayH < -1.0) break;
        float h0, hMip3;
        tdDemAt(elevTex, specTex, q, enuX, enuY, enuZ, h0, hMip3);
        float grow    = float(i) / float(maxSteps);
        float minStep = max(0.5, t * mix(0.015, 0.05, grow * grow));
        // The cap only binds far above the terrain (0.6 * gap is safe for any DEM slope below ~0.6):
        // at 2.5 km, rays leaving the terrain shell a few degrees above the horizon took 160-220
        // steps in the half-res depth pass (harness probe) — most of that pass's cost at ground level.
        float maxStep = clamp(t * 0.3, 20.0, 12000.0);
        float gap;
        float amp0 = detail ? tdAmp0(h0, hMip3) : 0.0;
        if (rayH > h0 + tdTailBound(amp0)) {
            gap = rayH - h0 - tdTailBound(amp0);
            armed = true;
        } else if (!detail) {
            gap = rayH - h0;
            if (gap < 0.0) {
                float tLo = tPrev, tHi = t;
                for (int j = 0; j < 10; ++j) {
                    float tM = 0.5 * (tLo + tHi);
                    vec3  qm = vec3(0.0, 0.0, hEye) + tM * dir;
                    float m0, m3;
                    tdDemAt(elevTex, specTex, qm, enuX, enuY, enuZ, m0, m3);
                    if (tdAltitude(qm) < m0) tHi = tM; else tLo = tM;
                }
                return 0.5 * (tLo + tHi);
            }
        } else {
            TdState st = tdBegin(q, enuX, enuY, enuZ, h0, hMip3);
            float lod = tdGeomLodM(t, pixAngle) * lodScale;
            tdOctaves(st, lod, coarseK);
            float fineBound = (st.k < kTdOctaves) ? tdTailBound(st.amp) : 0.0;
            gap = rayH - h0 - st.h - fineBound;
            if (envelope && armed && gap < pixAngle * t)
                return (gap >= 0.0) ? t : tPrev;   // conservative: the last sample before, if inside
            if (gap > 0.0)
                armed = true;
            if (gap <= 0.0) {
                tdOctaves(st, lod, kTdOctaves);
                gap = rayH - h0 - st.h;
                // Within a pixel footprint of the surface is a hit: a ray skimming a slope or a
                // plain would otherwise creep along it in ever smaller relaxation steps (and a ray
                // that runs out of steps is a MISS). IQ's "Elevated" terminates the same way.
                if (gap >= 0.0 && gap < pixAngle * t)
                    return t;
                if (gap < 0.0) {
                    // Bracket [tPrev, t]: regula falsi on the same height function. The bracket is
                    // a fraction of the gap one step back, so three secant steps land well inside a
                    // pixel; bisection took eight full evaluations for the same answer.
                    float tA = tPrev, gA = prevGap, tB = t, gB = gap;
                    if (!prevGapValid) {
                        vec3 qa = vec3(0.0, 0.0, hEye) + tA * dir;
                        float a0;
                        gA = tdAltitude(qa) - terrainHeightDetailed(elevTex, specTex, qa, enuX, enuY, enuZ,
                                                                    tdGeomLodM(tA, pixAngle) * lodScale, kTdOctaves, a0);
                    }
                    gA = max(gA, 1e-3);
                    for (int j = 0; j < 3; ++j) {
                        float tM = tA + (tB - tA) * gA / (gA - gB);
                        vec3  qm = vec3(0.0, 0.0, hEye) + tM * dir;
                        float m0;
                        float gM = tdAltitude(qm) - terrainHeightDetailed(elevTex, specTex, qm, enuX, enuY, enuZ,
                                                                          tdGeomLodM(tM, pixAngle) * lodScale, kTdOctaves, m0);
                        if (gM < 0.0) { tB = tM; gB = gM; } else { tA = tM; gA = max(gM, 1e-3); }
                    }
                    return tA + (tB - tA) * gA / (gA - gB);
                }
                prevGap = gap;
                prevGapValid = true;
                tPrev = t;
                t += clamp(0.6 * gap, minStep, maxStep);
                continue;
            }
        }
        prevGapValid = false;
        tPrev = t;
        t += clamp(0.6 * gap, minStep, maxStep);
    }
    // Out of budget before tExit: a long grazing ray (the far side of a valley, a horizon seen from
    // a summit). Finish it in at most 48 steps sized to reach tExit, on the DEM plus the coarse
    // octaves (those still in LOD range). Returning a miss here punched sky-coloured holes through
    // terrain — the half-res depth said "sky", so the full-res pass skipped the pixel. A DEM-only
    // tail was the first fix and still missed hills that exist only in the detail (found with the
    // harness `probe`: seed "SKY", but a march from the eye hit at 2.6 km after 150 steps).
    if (t < tExit) {
        float tailStep = max((tExit - t) / 48.0, 50.0);
        for (int i = 0; i < 48 && t < tExit; ++i) {
            vec3  q  = vec3(0.0, 0.0, hEye) + t * dir;
            float h0;
            float gap = tdAltitude(q) - terrainHeightDetailed(elevTex, specTex, q, enuX, enuY, enuZ,
                                                              tdGeomLodM(t, pixAngle) * lodScale, coarseK, h0);
            if (gap < 0.0) {
                float tLo = tPrev, tHi = t;
                for (int j = 0; j < 8; ++j) {
                    float tM = 0.5 * (tLo + tHi);
                    vec3  qm = vec3(0.0, 0.0, hEye) + tM * dir;
                    float m0;
                    float mT = terrainHeightDetailed(elevTex, specTex, qm, enuX, enuY, enuZ,
                                                     tdGeomLodM(tM, pixAngle) * lodScale, coarseK, m0);
                    if (tdAltitude(qm) < mT) tHi = tM; else tLo = tM;
                }
                return 0.5 * (tLo + tHi);
            }
            tPrev = t;
            t += clamp(0.6 * gap, 0.2 * tailStep, tailStep);
        }
    }
    return -1.0;
}

// Soft sun shadow from the terrain at q (a hit point): 0 = in shadow, 1 = lit. sunDir and n (the
// shading normal) in ENU. Coarse octaves only (the fine ones are normal-mapped at shading distances
// anyway) and a penumbra that widens with distance. The ray starts a distance-proportional bias off
// the surface along n: the shadow's surface is a coarser LOD than the one drawn, and starting on it
// shadowed every sun-facing facet in texel-sized blocks (acne) — found with the harness's shadow view.
float terrainSunShadow(sampler2D elevTex, sampler2D specTex, vec3 q, vec3 n, vec3 sunDir,
                       vec3 enuX, vec3 enuY, vec3 enuZ, float lodBase) {
    float res  = 1.0;
    float bias = 1.0 + 1.5 * lodBase;
    vec3  q0   = q + n * bias;
    float t    = bias;
    // 16 steps, three octaves, the step floor at 8% of t: measured 2-4 ms at ground level with 24
    // steps / four octaves / 5% (harness perf, clouds off), for no visible difference at the
    // shadow's own scale.
    for (int i = 0; i < 16; ++i) {
        vec3  qs   = q0 + sunDir * t;
        float rayH = tdAltitude(qs);
        if (rayH > kMaxTerrain + 600.0) break;
        float h0;
        float H = terrainHeightDetailed(elevTex, specTex, qs, enuX, enuY, enuZ, max(lodBase, 0.05 * t),
                                        kTdCoarseOctaves - 1, h0);
        float gap = rayH - H;
        res = min(res, 6.0 * gap / t);
        if (res < 0.0) break;
        t += clamp(0.7 * gap, max(6.0, 0.08 * t), 5000.0);
        if (t > 50000.0) break;
    }
    return smoothstep(0.0, 1.0, clamp(res, 0.0, 1.0));
}

#endif // SATLIGHTSIM_TERRAIN_DETAIL_GLSL
