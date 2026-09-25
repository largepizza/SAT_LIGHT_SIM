#ifndef SATLIGHTSIM_TERRAIN_DETAIL_GLSL
#define SATLIGHTSIM_TERRAIN_DETAIL_GLSL

// ── Procedural terrain detail: a real 3D heightfield on top of the DEM ─────────────────────────
//
// Requires common.glsl, cloud_params.glsl (the `cloud` UBO) and terrain.glsl, in that order, and
// `#extension GL_EXT_control_flow_attributes : require` in the including shader (the erosion's loops
// are [[dont_unroll]]: unrolled, their inlined copies bloated every pass that includes this file).
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
// Erosion octaves (tdErosion, 2026-09-25): between the coarse and fine octaves, three octaves of
// slope-aligned stripe noise (512/256/128 m) cut gullies that run DOWNHILL and branch — the
// clayjohn 2018 / Fewes 2023 "eroded terrain noise" the `erosion` branch used, but here part of the
// marched surface (they notch ridgelines and change silhouettes), not a correction after the hit.
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
    vec3  flow;   // the slope the erosion follows: the gradient after the two largest octaves
    bool  ero;    // the erosion octaves have been added
    bool  lodDone; // the value octaves stopped at the LOD (none finer will run)
    float hEro;   // metres of h that came from them (debug view, the march's refinement)
    vec3  gEro;   // their tangential gradient (ECEF, m/m)
    float hC3;    // h after the first three octaves: the terrain shadow's surface (terrainSunShadow)
};

TdState tdBegin(vec3 q, vec3 enuX, vec3 enuY, vec3 enuZ, float h0, float hMip3) {
    TdState s;
    s.h = 0.0; s.grad = vec3(0.0); s.slope = vec3(0.0);
    s.ero = false; s.hEro = 0.0; s.gEro = vec3(0.0); s.flow = vec3(0.0); s.lodDone = false; s.hC3 = 0.0;
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

// ── Erosion octaves ───────────────────────────────────────────────────────────────────────────
// Each octave is a cell grid of jittered points, each drawing a cosine stripe whose phase runs
// ACROSS the local slope, so its ridges and grooves run down it; the weighted average over the 3x3
// neighbourhood gives gullies that wander and end. Each octave's direction follows the slope PLUS the
// octaves before it (tributaries run down the walls of the gully they feed — the branching; the
// terrainErosion.y slider scales that feedback).
//
// Differences from the source, each for a reason:
//  - a compact kernel (1 - d^2/R^2)^3, R = 1.25 cells, with the points jittered within the middle
//    half of their cell: every point that can reach p is in the 3x3, so cell borders have no seam
//    (the Gaussian of the source needs a 4x4 and still truncates);
//  - the value and its gradient are exact (weights' derivatives included): the gradient is the
//    shading normal and the branching input, not only an approximation;
//  - the 2D domain is world-fixed and anchored like the value noise: the ECEF plane perpendicular to
//    the dominant axis of up (dropping one coordinate keeps the anchor's integer cell exact), blended
//    between the two dominant axes near a cube-face edge. The projection stretches spacing up to
//    1.7x near the corners but not the direction: the stripes vary across the PROJECTED slope, and
//    the plane projection is a linear bijection on the tangent plane, so grooves run exactly downhill.
const int   kTdErosionOctaves = 2;
const int   kTdErosionFirstK  = 2;       // first cell 2048 / 2^2 = 512 m
const float kTdErosionSum     = 0.75;    // 0.5 + 0.25: |sum| of the octave weights

// Upper bound on the erosion's height (|stripe average| <= 1, slope factor <= 1).
float tdErosionBound(float amp0) { return amp0 * cloud.terrainErosion.x * kTdErosionSum; }

// Three values in [-1, 1] from one hash (tdRand's mix): 11 + 11 + 10 bits.
vec3 tdRand3(ivec3 c) {
    uvec3 v = uvec3(c);
    uint h = v.x * 0x8da6b343u ^ v.y * 0xd8163841u ^ v.z * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return vec3(float(h >> 21) * (2.0 / 2047.0), float((h >> 10) & 0x7FFu) * (2.0 / 2047.0),
                float(h & 0x3FFu) * (2.0 / 1023.0)) - 1.0;
}

// One octave at p (lattice units, lattice offset `off`), stripes varying along the unit 2D `dir`.
// Returns (value in [-1, 1], d/dp) per lattice unit. Each point draws its stripe at its own
// frequency (0.75-1.25 per cell): with one frequency, a far slope where only the 512-m octave is
// left read as regular parallel ripples (harness, the Andes from 30 km).
vec3 tdErosionCell(vec2 p, ivec2 off, int salt, vec2 dir) {
    const float kTwoPi = 6.2831853;
    const float kInvR2 = 1.0 / 1.5625;   // kernel radius 1.25 cells
    vec2  fl = floor(p);
    vec2  f  = p - fl;
    ivec2 i  = ivec2(fl) + off;
    float wsum = 0.0, vsum = 0.0;
    vec2  dwsum = vec2(0.0), dvsum = vec2(0.0);
    [[dont_unroll]] for (int c = 0; c < 9; ++c) {
        {
            int   x  = c % 3 - 1, y = c / 3 - 1;
            vec3  r  = tdRand3(ivec3(i + ivec2(x, y), salt));
            vec2  pp = f - (vec2(float(x), float(y)) + 0.5 + 0.25 * r.xy);
            float s  = 1.0 - dot(pp, pp) * kInvR2;
            if (s <= 0.0) continue;
            float w  = s * s * s;
            vec2  dw = (-6.0 * kInvR2 * s * s) * pp;
            float fr = kTwoPi * (1.0 + 0.25 * r.z);
            float ph = fr * dot(pp, dir);
            float cv = cos(ph), sv = sin(ph);
            wsum  += w;
            dwsum += dw;
            vsum  += w * cv;
            dvsum += dw * cv - (w * sv * fr) * dir;
        }
    }
    float v = vsum / wsum;               // wsum > 0: p's own cell point is always within R
    return vec3(v, (dvsum - v * dwsum) / wsum);
}

// Face axes: the two ECEF axes left when the dominant one (f) is dropped.
ivec2 tdFaceAxes(int f) { return f == 0 ? ivec2(1, 2) : (f == 1 ? ivec2(2, 0) : ivec2(0, 1)); }

// The erosion octaves on one face: vec4(height in amplitude units, gradient in ECEF per metre, not
// yet projected onto the tangent plane). g = the slope they follow (ECEF, m/m); amp = the metres
// one amplitude unit is (for the branching feedback, which is in real slope).
vec4 tdErosionFace(vec3 rel, int f, vec3 g, float amp, float lodM) {
    ivec2 ax     = tdFaceAxes(f);
    vec2  rel2   = vec2(rel[ax.x], rel[ax.y]);
    ivec3 anchor = ivec3(cloud.terrainAnchorCell.xyz);
    ivec2 anc2   = ivec2(anchor[ax.x], anchor[ax.y]);
    vec2  g2     = vec2(g[ax.x], g[ax.y]);
    float h = 0.0, a = 0.5;
    vec2  dh = vec2(0.0);                // per metre, face coordinates
    float cell = kTdBaseCellM / float(1 << kTdErosionFirstK);
    [[dont_unroll]] for (int o = 0; o < kTdErosionOctaves; ++o) {
        // Twice the value noise's margin: a stripe period is one cell, so a cell of 2 LODs is a
        // stripe pattern at the resolution limit — the ripples above.
        float fade = smoothstep(2.0 * lodM, 4.0 * lodM, cell);
        if (fade <= 0.0) break;
        vec2 gs  = g2 + cloud.terrainErosion.y * amp * dh;   // slope + the gullies so far
        vec2 dir = vec2(-gs.y, gs.x) / max(length(gs), 1e-6);
        vec3 e   = tdErosionCell(rel2 / cell, anc2 << (kTdErosionFirstK + o), 4099 + 17 * f + o, dir);
        h  += a * fade * e.x;
        dh += (a * fade / cell) * e.yz;
        a *= 0.5;
        cell *= 0.5;
    }
    vec3 G = vec3(0.0);
    G[ax.x] = dh.x;
    G[ax.y] = dh.y;
    return vec4(h, G);
}

// The DEM's slope at up (ECEF, m/m): central differences over two texels of the bilinear DEM, which
// is continuous across texel borders (a one-sided difference of the bilinear jumps there, and the
// stripes would break along every texel edge) and smooth enough to steer by: stripes cross-cut the
// slope, so where its direction turns fast (a crest, a valley floor) they crowd into fringes — seen as
// thin parallel lines along the ridges with a one-texel difference (harness, Alps from 3.5 km). Only
// the erosion needs it, so only it pays the fetches.
vec3 tdDemGrad(sampler2D elevTex, vec3 up) {
    vec2  sz = vec2(textureSize(elevTex, 0));
    vec2  uv = dirToUV(up);
    vec2  du = vec2(1.0 / sz.x, 0.0), dv = vec2(0.0, 1.0 / sz.y);
    float hE = textureLod(elevTex, uv + du, 0.0).r, hW = textureLod(elevTex, uv - du, 0.0).r;
    float hN = textureLod(elevTex, uv - dv, 0.0).r, hS = textureLod(elevTex, uv + dv, 0.0).r;
    float cosLat = max(length(up.xy), 0.02);
    float dE = (hE - hW) * kElevRange / (4.0 * PI * R_EARTH * cosLat / sz.x);
    float dN = (hN - hS) * kElevRange / (2.0 * PI * R_EARTH / sz.y);
    vec3  east  = normalize(vec3(-up.y, up.x, 0.0) + vec3(1e-7, 0.0, 0.0));
    vec3  north = cross(up, east);
    return dE * east + dN * north;
}

// Add the erosion octaves to s (once). They follow the DEM slope plus the two largest octaves'
// (s.flow): the 512/256-m octaves turn too fast to steer 512-m gullies by (fringes on every crest).
void tdErosion(sampler2D elevTex, inout TdState s, float lodM) {
    s.ero = true;
    float strength = cloud.terrainErosion.x;
    if (strength <= 0.0 || s.amp0 <= 0.0) return;
    if (kTdBaseCellM / float(1 << kTdErosionFirstK) <= 2.0 * lodM) return;
    vec3  g   = tdDemGrad(elevTex, s.up) + s.grad;
    g -= s.up * dot(g, s.up);
    // Gullies need a slope to run down: none on flats, valley floors and crests (where the slope's
    // direction flips, the other source of fringes).
    float amp = s.amp0 * strength * smoothstep(0.03, 0.25, length(g));
    if (amp <= 0.0) return;
    vec3 au = abs(s.up);
    int  f0 = (au.x >= au.y && au.x >= au.z) ? 0 : (au.y >= au.z ? 1 : 2);
    int  f1 = (f0 == 0) ? (au.y >= au.z ? 1 : 2) : (f0 == 1 ? (au.x >= au.z ? 0 : 2) : (au.x >= au.y ? 0 : 1));
    float w0 = 0.5 + 0.5 * smoothstep(0.0, 0.02, au[f0] - au[f1]);
    // One call site for both faces (a second site doubled the inlined code, and GLSL inlines all of
    // it: the shader's registers are sized for its largest path, so code that never runs still cost
    // every terrain pixel ~40% — measured with the harness).
    vec4 e  = vec4(0.0);
    int  nf = (w0 < 1.0) ? 2 : 1;
    [[dont_unroll]] for (int fi = 0; fi < nf; ++fi)
        e += (fi == 0 ? w0 + (1.0 - w0) * float(nf == 1) : 1.0 - w0) * tdErosionFace(s.rel, fi == 0 ? f0 : f1, g, amp, lodM);
    vec3 G = e.yzw - s.up * dot(e.yzw, s.up);
    s.h     += amp * e.x;
    s.hEro   = amp * e.x;
    s.gEro   = amp * G;
    s.grad  += amp * G;
    s.slope += amp * G;
}

// What the octaves not yet run can still add (the march's bounds).
float tdRemainingBound(TdState s) {
    return (s.k < kTdOctaves && !s.lodDone ? tdTailBound(s.amp) : 0.0) + (s.ero ? 0.0 : tdErosionBound(s.amp0));
}

// Run octaves s.k .. kEnd-1 (and stop at the LOD: octaves with a cell below lodM are dropped,
// faded in between lodM and 2*lodM). The erosion octaves run once, between the coarse and the fine
// octaves, whenever the call reaches past the coarse ones — also when the value octaves stopped at
// the LOD before that (the erosion's first cell is larger than theirs): a stop below the coarse
// octaves jumps to them with lodDone set, so the erosion still comes up. ONE call site for the
// erosion, and allowEro is a literal at every call site so the coarse-only paths compile without it.
void tdValueOctaves(inout TdState s, float lodM, int kEnd) {
    ivec3 anchor = ivec3(cloud.terrainAnchorCell.xyz);
    for (; s.k < kEnd; ++s.k) {
        float fade = s.lodDone ? 0.0 : smoothstep(lodM, 2.0 * lodM, s.cell);
        if (fade <= 0.0 || s.amp <= 0.0) {
            s.lodDone = true;
            if (s.k < kTdCoarseOctaves) { s.k = kTdCoarseOctaves - 1; continue; }
            s.k = kTdOctaves;
            break;
        }
        vec4 n  = tdNoised(s.rel / s.cell, anchor << s.k);
        vec3 g  = n.yzw / s.cell;
        g -= s.up * dot(g, s.up);                         // tangential part only
        s.slope += g * s.amp;
        float damp = 1.0 / (1.0 + cloud.terrainDetailErode * dot(s.slope, s.slope));
        s.h    += s.amp * fade * damp * n.x;
        s.grad += s.amp * fade * damp * g;
        if (s.k == 1)
            s.flow = s.grad;
        if (s.k <= kTdCoarseOctaves - 2)
            s.hC3 = s.h;
        s.amp  *= cloud.terrainDetailGain;
        s.cell *= 0.5;
    }
}
// The erosion call sits OUTSIDE the octave loop: inside it, an unrolled loop that starts at a
// runtime s.k cannot know which iteration reaches the coarse boundary, so the compiler copied the
// whole erosion into every iteration.
void tdOctaves(sampler2D elevTex, inout TdState s, float lodM, int kEnd, bool allowEro) {
    if (!allowEro || kEnd <= kTdCoarseOctaves) {
        tdValueOctaves(s, lodM, kEnd);
        return;
    }
    tdValueOctaves(s, lodM, kTdCoarseOctaves);
    if (!s.ero)
        tdErosion(elevTex, s, lodM);
    tdValueOctaves(s, lodM, kEnd);
}

// The full detail at q (for shading, AO, and the debug views).
struct TdSample {
    float h;     // metres added to the DEM
    vec3  grad;  // tangential gradient of h (ECEF, metres per metre)
    float rough; // 0..1 roughness used (material/AO)
    float amp;   // the octave-0 amplitude used (metres)
    float hEro;  // metres of h from the erosion octaves
    float hC3;   // metres of h from the first three octaves (the terrain shadow's surface)
};

// Shading-only micro relief: octaves 8..13 (8 m down to 0.25 m cells) — see tdMicroBump.
const int kTdMicroFirst = 8;
const int kTdMicroLast  = 13;
TdSample terrainDetail(sampler2D elevTex, vec3 q, vec3 enuX, vec3 enuY, vec3 enuZ, float lodM, float h0,
                       float hMip3, int kEnd) {
    TdState st = tdBegin(q, enuX, enuY, enuZ, h0, hMip3);
    tdOctaves(elevTex, st, lodM, kEnd, true);
    TdSample s;
    s.h = st.h; s.grad = st.grad; s.rough = st.rough; s.amp = st.amp0; s.hEro = st.hEro; s.hC3 = st.hC3;
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
// smoothness (0..1) flattens it: snow (the caller's day-map snow mask) is far smoother at metre scale
// than rock or turf. At full slope a snowfield under a 20-degree Sun turned half its metre-scale
// facets away from the Sun — near-black blobs across a glacier seen from the ground (harness
// flight). The floor on smooth ground came down from 0.35 to 0.25 of the rough value for the
// same reason.
vec3 tdMicroBump(vec3 q, vec3 nE, vec3 enuX, vec3 enuY, vec3 enuZ, float lodM, float rough, float smoothness) {
    vec3  rel = tdSolidRel(q, enuX, enuY, enuZ);
    ivec3 anchor = ivec3(cloud.terrainAnchorCell.xyz);
    vec3  gsum = vec3(0.0);
    float slopeAmp = 0.22 * max(rough, 0.25) * mix(1.0, 0.3, smoothness) * cloud.terrainDetailStrength;
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

// Full terrain height (DEM + detail) at q. h0 returns the DEM part. terrainHeightCoarse: the same
// up to kEnd <= kTdCoarseOctaves, without the erosion (shadows, the tail march) — a separate
// function so those paths do not carry its code.
float terrainHeightDetailed(sampler2D elevTex, sampler2D specTex, vec3 q, vec3 enuX, vec3 enuY, vec3 enuZ,
                            float lodM, int kEnd, out float h0) {
    float hMip3;
    tdDemAt(elevTex, specTex, q, enuX, enuY, enuZ, h0, hMip3);
    if (!tdEnabled()) return h0;
    TdState st = tdBegin(q, enuX, enuY, enuZ, h0, hMip3);
    tdOctaves(elevTex, st, lodM, kEnd, true);
    return h0 + st.h;
}
float terrainHeightCoarse(sampler2D elevTex, sampler2D specTex, vec3 q, vec3 enuX, vec3 enuY, vec3 enuZ,
                          float lodM, int kEnd, out float h0) {
    float hMip3;
    tdDemAt(elevTex, specTex, q, enuX, enuY, enuZ, h0, hMip3);
    if (!tdEnabled()) return h0;
    TdState st = tdBegin(q, enuX, enuY, enuZ, h0, hMip3);
    tdOctaves(elevTex, st, lodM, kEnd, false);
    return h0 + st.h;
}
// The full height with the erosion taken as a plane through (qB, hEroB, slope gEroB) — for the
// march's refinement inside a bracket a few metres to tens of metres long, where the 256/512-m
// gullies are flat to a few centimetres. The erosion was ~4 ms of the terrain passes at ground
// level and the refinement was half of its evaluations (harness perf, 2026-09-25).
float terrainHeightLinEro(sampler2D elevTex, sampler2D specTex, vec3 q, vec3 enuX, vec3 enuY, vec3 enuZ,
                          float lodM, vec3 qB, float hEroB, vec3 gEroB) {
    float h0, hMip3;
    tdDemAt(elevTex, specTex, q, enuX, enuY, enuZ, h0, hMip3);
    if (!tdEnabled()) return h0;
    TdState st = tdBegin(q, enuX, enuY, enuZ, h0, hMip3);
    tdValueOctaves(st, lodM, kTdCoarseOctaves);
    vec3 d = q - qB;
    float he = hEroB + dot(gEroB, d.x * enuX + d.y * enuY + d.z * enuZ);
    st.h += he; st.grad += gEroB; st.slope += gEroB; st.ero = true;
    tdValueOctaves(st, lodM, kTdOctaves);
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

// The erosion at the hit point of the last terrainMarchDetailed(), for the caller's shading
// (terrainDetailLinEro): xyz = its gradient, w = its height, at gTdHitEroQ. Zero for hits that
// carry no erosion (DEM-only, or the coarse tail march — consistent with their geometry). A module
// variable rather than out-parameters so the depth passes, which ignore it, are unchanged.
vec4 gTdHitEro  = vec4(0.0);
vec3 gTdHitEroQ = vec3(0.0);

// The full detail at q with the erosion as the plane the march left (see terrainHeightLinEro): the
// shading normal, at the shading LOD. Saves one full erosion evaluation per terrain pixel.
TdSample terrainDetailLinEro(vec3 q, vec3 enuX, vec3 enuY, vec3 enuZ, float lodM, float h0, float hMip3) {
    TdState st = tdBegin(q, enuX, enuY, enuZ, h0, hMip3);
    tdValueOctaves(st, lodM, kTdCoarseOctaves);
    vec3 d = q - gTdHitEroQ;
    st.hEro = gTdHitEro.w + dot(gTdHitEro.xyz, d.x * enuX + d.y * enuY + d.z * enuZ);
    st.gEro = gTdHitEro.xyz;
    st.h += st.hEro; st.grad += st.gEro; st.slope += st.gEro; st.ero = true;
    tdValueOctaves(st, lodM, kTdOctaves);
    TdSample s;
    s.h = st.h; s.grad = st.grad; s.rough = st.rough; s.amp = st.amp0; s.hEro = st.hEro; s.hC3 = st.hC3;
    return s;
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
    gTdHitEro = vec4(0.0);
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
        float allBound = detail ? tdTailBound(amp0) + tdErosionBound(amp0) : 0.0;
        if (rayH > h0 + allBound) {
            gap = rayH - h0 - allBound;
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
            tdOctaves(elevTex, st, lod, coarseK, false);
            float fineBound = tdRemainingBound(st);
            gap = rayH - h0 - st.h - fineBound;
            if (envelope && armed && gap < pixAngle * t)
                return (gap >= 0.0) ? t : tPrev;   // conservative: the last sample before, if inside
            if (gap > 0.0)
                armed = true;
            if (gap <= 0.0) {
                tdOctaves(elevTex, st, lod, kTdOctaves, true);
                gap = rayH - h0 - st.h;
                // Within a pixel footprint of the surface is a hit: a ray skimming a slope or a
                // plain would otherwise creep along it in ever smaller relaxation steps (and a ray
                // that runs out of steps is a MISS). IQ's "Elevated" terminates the same way.
                if (gap >= 0.0 && gap < pixAngle * t) {
                    gTdHitEro = vec4(st.gEro, st.hEro); gTdHitEroQ = q;
                    return t;
                }
                if (gap < 0.0) {
                    // Bracket [tPrev, t]: regula falsi on the same height function. The bracket is
                    // a fraction of the gap one step back, so three secant steps land well inside a
                    // pixel; bisection took eight full evaluations for the same answer.
                    // One evaluation site (j = -1 fills in the gap at tPrev when the last step did
                    // not compute it): each inlined full-height call is the whole detail + erosion.
                    float tA = tPrev, gA = max(prevGap, 1e-3), tB = t, gB = gap;
                    for (int j = prevGapValid ? 0 : -1; j < 3; ++j) {
                        float tM = (j < 0) ? tA : tA + (tB - tA) * gA / (gA - gB);
                        vec3  qm = vec3(0.0, 0.0, hEye) + tM * dir;
                        float gM = tdAltitude(qm) - terrainHeightLinEro(elevTex, specTex, qm, enuX, enuY, enuZ,
                                                                        tdGeomLodM(tM, pixAngle) * lodScale,
                                                                        q, st.hEro, st.gEro);
                        if (j < 0)            gA = max(gM, 1e-3);
                        else if (gM < 0.0)  { tB = tM; gB = gM; }
                        else                { tA = tM; gA = max(gM, 1e-3); }
                    }
                    gTdHitEro = vec4(st.gEro, st.hEro); gTdHitEroQ = q;
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
            float gap = tdAltitude(q) - terrainHeightCoarse(elevTex, specTex, q, enuX, enuY, enuZ,
                                                            tdGeomLodM(t, pixAngle) * lodScale, coarseK, h0);
            if (gap < 0.0) {
                float tLo = tPrev, tHi = t;
                for (int j = 0; j < 8; ++j) {
                    float tM = 0.5 * (tLo + tHi);
                    vec3  qm = vec3(0.0, 0.0, hEye) + tM * dir;
                    float m0;
                    float mT = terrainHeightCoarse(elevTex, specTex, qm, enuX, enuY, enuZ,
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
                       vec3 enuX, vec3 enuY, vec3 enuZ, float lodBase, float hCoarseAtQ) {
    float res  = 1.0;
    float bias = 1.0 + 1.5 * lodBase;
    // The ray is tested against the COARSE octaves only, but q lies on the full surface: where the
    // fine octaves dip below the coarse ones the ray began inside the surface it is tested against
    // and the point shadowed itself — black blobs with grey rims tracing the fine octaves' contours
    // on any gentle sunlit slope seen from the ground (found with a harness flight into a glacier).
    // Start from the coarse surface instead where it is higher. hCoarseAtQ = DEM + the first three
    // octaves at q, which the caller's shading-normal evaluation already has (TdSample::hC3):
    // recomputing it here cost ~0.5 ms at ground level.
    float lift = max(0.0, hCoarseAtQ - tdAltitude(q));
    vec3  q0   = q + normalize(vec3(q.xy, R_EARTH + q.z)) * lift + n * bias;
    float t    = bias;
    // 16 steps, three octaves, the step floor at 8% of t: measured 2-4 ms at ground level with 24
    // steps / four octaves / 5% (harness perf, clouds off), for no visible difference at the
    // shadow's own scale.
    for (int i = 0; i < 16; ++i) {
        vec3  qs   = q0 + sunDir * t;
        float rayH = tdAltitude(qs);
        if (rayH > kMaxTerrain + 600.0) break;
        float h0;
        float H = terrainHeightCoarse(elevTex, specTex, qs, enuX, enuY, enuZ, max(lodBase, 0.05 * t),
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
