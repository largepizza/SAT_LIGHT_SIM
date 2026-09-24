#version 450
// Satellite mesh shading (lighting overhaul Phase 4, .plans/SAT_RENDERER_PHASE4.md).
//
// Units: sat_sky.frag's pre-exposure radiance, "π × radiance per unit solar irradiance" — a sunlit
// white Lambertian face reads its albedo × cos, as terrain does. The BRDF is the photometry's
// (modelFlux / lobeIntensity): Lambert + GGX (or Beckmann) · Schlick · Smith, with the sun's disc
// folded into α (SUN_ALPHA), so summing a render's pixels reproduces the lobe model's intensity.
//   sun          direct, × litFactor (Earth shadow) × tint, shadowed by the model's own primitives
//   earthshine   diffuse from the effective earthshine source (same table as the photometry)
//   reflections  the specular lobe of the reflected ray into earth_env.glsl (Earth, atmosphere,
//                clouds, city lights) — replaces the earthshine specular lobe, which it contains
//   moonlight    diffuse, directional

#include "common.glsl"
#include "terrain.glsl"
#include "sat_mesh_common.glsl"
#include "earth_env.glsl"

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) flat in uint vMaterial;
layout(location = 4) flat in uint vComponent;
layout(location = 5) flat in uint vInstance;
layout(location = 6) in vec3 vRest;
layout(location = 7) flat in uint vGroup;

layout(location = 0) out vec4 outColor;
layout(location = 1) out float outDist; // scene mode: TRUE distance from the camera (m); other modes
                                        // have no attachment at location 1 and the write is dropped

const float SUN_ALPHA2 = 0.0023 * 0.0023; // the sun's disc as a roughness (sat_orbit.comp SUN_ALPHA)

// π·f_spec·(n·l) for unit N, V, L (both above the surface) — sat_orbit.comp's lobe formula per pixel.
float specCos(vec3 N, vec3 V, vec3 L, float a2, float f0, bool beckmann)
{
    float nl = dot(N, L), nv = dot(N, V);
    if (nl <= 0.0 || nv <= 0.0) return 0.0;
    vec3  H  = normalize(L + V);
    float nh = max(dot(N, H), 0.0);
    vec3  cx = cross(N, H);
    float sin2 = dot(cx, cx); // exact near the peak, where 1 − nh² cancels
    float F  = f0 + (1.0 - f0) * pow(1.0 - max(dot(L, H), 0.0), 5.0);
    if (beckmann) {
        if (nh <= 0.0) return 0.0;
        float nh2 = nh * nh;
        float D = exp(-sin2 / (nh2 * a2)) / (PI * a2 * nh2 * nh2);
        float alpha = sqrt(a2);
        float cL = nl / (alpha * sqrt(max(1e-12, 1.0 - nl * nl)));
        float cV = nv / (alpha * sqrt(max(1e-12, 1.0 - nv * nv)));
        float gL = cL >= 1.6 ? 1.0 : (3.535 * cL + 2.181 * cL * cL) / (1.0 + 2.276 * cL + 2.577 * cL * cL);
        float gV = cV >= 1.6 ? 1.0 : (3.535 * cV + 2.181 * cV * cV) / (1.0 + 2.276 * cV + 2.577 * cV * cV);
        return PI * D * F * gL * gV / (4.0 * nv);
    }
    float den = nh * nh * a2 + sin2;
    float D = a2 / (PI * den * den);
    float gL = 2.0 * nl / (nl + sqrt(a2 + (1.0 - a2) * nl * nl));
    float gV = 2.0 * nv / (nv + sqrt(a2 + (1.0 - a2) * nv * nv));
    return PI * D * F * gL * gV / (4.0 * nv);
}

// Mirror of rayHitsOccluder() in SatModel.cpp / sat_orbit.comp: the ray org + t·dir (t > tMin) in
// the occluder's LOCAL frame.
bool rayHitsOccluder(uint kind, vec3 hf, vec3 org, vec3 dir)
{
    const float tMin = 1e-5;
    if (kind == 0u) {
        if (abs(dir.z) < 1e-9) return false;
        float t = -org.z / dir.z;
        if (t <= tMin) return false;
        vec2 p = org.xy + t * dir.xy;
        return abs(p.x) <= hf.x && abs(p.y) <= hf.y;
    }
    if (kind == 1u) {
        float tNear = -1e30, tFar = 1e30;
        for (int a = 0; a < 3; ++a) {
            if (abs(dir[a]) < 1e-12) {
                if (abs(org[a]) > hf[a]) return false;
                continue;
            }
            float t0 = (-hf[a] - org[a]) / dir[a];
            float t1 = ( hf[a] - org[a]) / dir[a];
            tNear = max(tNear, min(t0, t1));
            tFar  = min(tFar,  max(t0, t1));
        }
        return tNear <= tFar && tFar > tMin;
    }
    if (kind == 4u) {
        float b = dot(org, dir), c = dot(org, org) - hf.x * hf.x;
        float disc = b * b - c;
        return disc >= 0.0 && (-b + sqrt(disc)) > tMin;
    }
    float r = hf.x, hh = hf.z;
    float a = dot(dir.xy, dir.xy);
    if (a > 1e-12) {
        float b = 2.0 * dot(org.xy, dir.xy);
        float c = dot(org.xy, org.xy) - r * r;
        float disc = b * b - 4.0 * a * c;
        if (disc >= 0.0) {
            float sq = sqrt(disc);
            float t1 = (-b - sq) / (2.0 * a), t2 = (-b + sq) / (2.0 * a);
            if (t1 > tMin && abs(org.z + t1 * dir.z) <= hh) return true;
            if (t2 > tMin && abs(org.z + t2 * dir.z) <= hh) return true;
        }
    }
    if (abs(dir.z) > 1e-12) {
        for (int k = 0; k < 2; ++k) {
            float t = ((k == 0 ? -hh : hh) - org.z) / dir.z;
            if (t > tMin) {
                vec2 p = org.xy + t * dir.xy;
                if (dot(p, p) <= r * r) return true;
            }
        }
    }
    return false;
}

// True if the ray from world point p toward dir is blocked by another component of this instance
// (a primitive never occludes its own surface — the photometry's rule, exact for convex/flat parts).
bool rayBlocked(MeshInstance inst, vec3 p, vec3 dir)
{
    for (uint i = 0u; i < inst.occluderCount; ++i) {
        if (i == vComponent) continue;
        MeshOccluder O = occluders[inst.firstOccluder + i];
        mat3 R = instGroupRot(inst, O.group);
        vec3 pr = transpose(R) * (p - inst.origin.xyz - inst.trans[O.group].xyz) - O.center;
        vec3 dr = transpose(R) * dir;
        vec3 org = vec3(dot(pr, O.axisX.xyz), dot(pr, O.axisY.xyz), dot(pr, O.axisZ.xyz));
        vec3 dl  = vec3(dot(dr, O.axisX.xyz), dot(dr, O.axisY.xyz), dot(dr, O.axisZ.xyz));
        if (rayHitsOccluder(O.kind, O.halfExt, org, dl)) return true;
    }
    return false;
}

// ── Procedural surface detail (SatSurfacePattern, SatModel.h) ────────────────────────────────
// Visual only, and photometrically neutral: each pattern scales the diffuse albedo by a factor whose
// AREA-WEIGHTED MEAN is 1, so the lobe model's scalar albedo stays the average of what is drawn. The
// specular part is left alone (the cover glass over solar cells is continuous). Every pattern fades
// to the plain material once its features shrink below a pixel (`resolve`), so a distant model
// shades exactly like the photometry.
uint hashU(uvec3 v)
{
    uint h = v.x * 0x8da6b343u ^ v.y * 0xd8163841u ^ v.z * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}
vec3 hash3(uvec3 v)
{
    uint h = hashU(v);
    return vec3(h & 0x3FFu, (h >> 10) & 0x3FFu, (h >> 20) & 0x3FFu) / 1023.0;
}
vec3 valueNoise3(vec3 p) // three decorrelated channels of trilinear value noise, 0..1
{
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    ivec3 b = ivec3(i);
    vec3 r = vec3(0.0);
    for (int k = 0; k < 8; ++k) {
        ivec3 o = ivec3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        vec3 w = mix(1.0 - f, f, vec3(o));
        r += w.x * w.y * w.z * hash3(uvec3(b + o + ivec3(4096)));
    }
    return r;
}

struct Surface {
    vec3  N;        // shading normal (world)
    vec3  tint;     // diffuse colour
    float albedoM;  // albedo multiplier (area mean 1)
    float rough;    // roughness actually used
};

// Fraction of a pixel footprint covered by periodic lines: each period (length 1, in period units)
// starts with a line of width w (0..1); x is the position, f the footprint width, both in periods.
// The exact box filter of the pattern (after Inigo Quilez's filtered grid): its average over any
// area is the true line fraction, so the pattern keeps its mean at every distance and never aliases.
float lineCover(float x, float w, float f)
{
    f = max(f, 1e-4);
    float a = x + 0.5 * f, b = x - 0.5 * f;
    float ia = floor(a) * w + min(fract(a), w);
    float ib = floor(b) * w + min(fract(b), w);
    return clamp((ia - ib) / f, 0.0, 1.0);
}
// Grid of lines in both directions: p and fw (= fwidth(p)) in period units, w the line fraction.
float gridCover(vec2 p, vec2 w, vec2 fw)
{
    vec2 c = vec2(lineCover(p.x, w.x, fw.x), lineCover(p.y, w.y, fw.y));
    return 1.0 - (1.0 - c.x) * (1.0 - c.y);
}
// Tilts N by a small random rotation seeded by `cell` (rest-frame jitter, posed with the group).
vec3 jitterNormal(vec3 N, mat3 R, ivec2 cell, float amount)
{
    vec3 j = hash3(uvec3(ivec3(cell + ivec2(8192), int(vMaterial)))) - 0.5;
    vec3 t = R * j;
    return normalize(N + amount * (t - dot(t, N) * N));
}

Surface applyPattern(MeshMaterial mat, MeshInstance inst, vec3 N)
{
    Surface s = Surface(N, mat.color, 1.0, mat.roughness);
    if (frame.params.z < 0.5)
        return s;
    const vec2  fwUv   = max(fwidth(vUv), vec2(1e-6));       // metres per pixel along u, v
    const float pxRest = max(length(fwidth(vRest)), 1e-6);
    const mat3  R      = instGroupRot(inst, vGroup);
    if (mat.pattern == 1u) {
        // Solar array: 8 x 4 cm cells with 2.5 mm gaps, grouped into 0.4 m modules with 1 cm gaps;
        // both gaps show the lighter substrate. Each module sits ~0.25 deg off the panel plane (each
        // cell ~0.5 deg once resolved), so a glint breaks up module by module, then cell by cell.
        const vec2 cellP = vec2(0.080, 0.040), modP = vec2(0.40, 0.40);
        const vec2 cellW = vec2(0.0025) / cellP, modW = vec2(0.010) / modP;
        float gapCells = gridCover(vUv / cellP, cellW, fwUv / cellP);
        float gapMods  = gridCover(vUv / modP, modW, fwUv / modP);
        float gap = 1.0 - (1.0 - gapCells) * (1.0 - gapMods);
        // Mean-preserving: the area mean of the multiplier is exactly 1.
        vec2  fc = 1.0 - cellW, fm = 1.0 - modW;
        float gFrac = 1.0 - fc.x * fc.y * fm.x * fm.y;
        const float mGap = 4.0;
        float mCell = (1.0 - gFrac * mGap) / (1.0 - gFrac);
        s.albedoM = mix(mCell, mGap, gap);
        s.tint    = mix(mat.color, vec3(0.85, 0.82, 0.70), gap);
        s.N = jitterNormal(N, R, ivec2(floor(vUv / modP)), 0.009);
        float cellRes = clamp((0.5 * cellP.y - max(fwUv.x, fwUv.y)) / (0.3 * cellP.y), 0.0, 1.0);
        s.N = jitterNormal(s.N, R, ivec2(floor(vUv / cellP)) + ivec2(3000, 0), 0.017 * cellRes);
    } else if (mat.pattern == 2u) {
        // MLI blanket: quilting seams every ~0.35 m (darker stitch lines) and crinkled film as explicit
        // micro-facets (12 cm + 4 cm) with a smaller residual roughness, so it breaks into glints
        // instead of one blurred lobe; statistically about the same spread.
        const float quiltP = 0.35, quiltW = 0.006 / 0.35;
        vec2  q  = vUv / quiltP;
        float st = gridCover(q, vec2(quiltW), fwUv / quiltP);
        const float mSt = 0.45;
        float qFrac = 1.0 - (1.0 - quiltW) * (1.0 - quiltW);
        s.albedoM = mix((1.0 - qFrac * mSt) / (1.0 - qFrac), mSt, st);
        const float scale = 0.12;
        float resolve = clamp((scale - 3.0 * pxRest) / (0.5 * scale), 0.0, 1.0);
        vec3 n3 = (valueNoise3(vRest / scale) - 0.5) + 0.5 * (valueNoise3(vRest / (scale / 3.0) + 17.0) - 0.5);
        vec3 t  = R * n3;
        s.N = normalize(N + resolve * 0.7 * (t - dot(t, N) * N));
        s.rough = mix(mat.roughness, mat.roughness * 0.4, resolve);
    } else if (mat.pattern == 3u) {
        // Panel seams every 0.5 m, 4 mm wide, darker.
        const float pitch = 0.5, w = 0.004 / 0.5;
        float seam = gridCover(vUv / pitch, vec2(w), fwUv / pitch);
        const float mSeam = 0.4;
        float sFrac = 1.0 - (1.0 - w) * (1.0 - w);
        s.albedoM = mix((1.0 - sFrac * mSeam) / (1.0 - sFrac), mSeam, seam);
    }
    return s;
}

void main()
{
    MeshInstance inst = instances[vInstance];
    MeshMaterial mat  = materials[vMaterial];
    vec3 N = normalize(vNormal);
    vec3 V = normalize(frame.camPos.xyz - vWorld);
    float nv = dot(N, V);
    if (nv < 0.0) { // smooth-normal silhouettes: keep the lit side's normal just past grazing
        N = normalize(N - V * (nv - 1e-3));
        nv = 1e-3;
    }

    // Photometric check (frame.params.w): sun only, scalar (the photometry has no colour), no
    // patterns; output L·d² so the CPU's Σ L·d²·Ω/π is the model's radiant intensity toward the camera.
    // params.w is a MODE (0 viewer, 1 check, 2 scene), not a flag: the first scene-pass cut tested
    // `> 0.5` here, so the scene rendered check output (red L·d², distance 0) — a glowing red
    // silhouette in the bloom and no mesh in the sky.
    const bool check = abs(frame.params.w - 1.0) < 0.5;
    Surface sf = check ? Surface(N, vec3(1.0), 1.0, mat.roughness) : applyPattern(mat, inst, N);
    N  = sf.N;
    nv = max(dot(N, V), 1e-3);

    const float a2    = max(sf.rough * sf.rough, 1e-8);
    const bool  beck  = mat.beckmann != 0u;
    const vec3  diffC = sf.tint * (mat.albedo * sf.albedoM);
    const vec3  specC = (mat.f0 >= 0.5 && !check) ? mat.color : vec3(1.0); // metals tint their reflection
    const bool  shadows = frame.params.x > 0.5;

    vec3 L = vec3(0.0);

    // ── Sun ──────────────────────────────────────────────────────────────────
    vec3  S  = inst.sun.xyz;
    float ns = dot(N, S);
    if (inst.sun.w > 0.0 && ns > 0.0) {
        bool blocked = shadows && inst.occluderCount > 0u && rayBlocked(inst, vWorld, S);
        if (!blocked) {
            vec3 E = inst.sunColor.rgb * inst.sun.w;
            L += E * (diffC * ns + specC * specCos(N, V, S, a2 + SUN_ALPHA2, mat.f0, beck));
        }
    }

    if (check) {
        vec3 d = vWorld - frame.camPos.xyz;
        outColor = vec4(L.r * dot(d, d), 0.0, 0.0, 1.0);
        outDist  = 0.0;
        return;
    }

    // ── Earthshine (diffuse; its specular part is the reflection below) ─────
    L += inst.earthshine.w * diffC * max(dot(N, inst.earthshine.xyz), 0.0);

    // ── Moonlight ────────────────────────────────────────────────────────────
    L += frame.moonDir.w * diffC * max(dot(N, frame.moonDir.xyz), 0.0);

    // ── Reflection of the Earth / atmosphere / clouds ────────────────────────
    if (frame.params.y > 0.5) {
        vec3 Rd = reflect(-V, N);
        bool blocked = shadows && inst.occluderCount > 0u && rayBlocked(inst, vWorld, Rd);
        if (!blocked && dot(Rd, N) > 0.0) {
            vec3  ro = vWorld - frame.earthCenter.xyz;
            // Texture lod from the reflected footprint on the ground: a lobe ~2α wide seen from the
            // satellite's altitude, against a ~4.9 km texel at the base level (8K equirect).
            float h   = max(length(ro) - R_EARTH, 1000.0);
            float lod = log2(max(2.0 * sf.rough * h / 4900.0, 1.0));
            vec3  env = earthEnv(ro, Rd, frame.sunDir.xyz, lod, frame.earthCenter.w);
            float Fr  = mat.f0 + (max(1.0 - sf.rough, mat.f0) - mat.f0) * pow(1.0 - nv, 5.0);
            L += env * specC * Fr;
        }
    }

    // Scene output (4c): pre-exposure radiance (sat_sky.frag applies the atmosphere in front of it
    // and the frame's exposure) scaled by the sprite→mesh hand-off fade, and the distance for the
    // shared scene depth.
    if (frame.params.w > 1.5) {
        outColor = vec4(L * inst.origin.w, 1.0);
        outDist  = length(vWorld - frame.camPos.xyz);
        return;
    }

    // Viewer output: the sky's own exposure curve, straight into the display-format target.
    vec3 col = vec3(1.0) - exp(-frame.camPos.w * L);
    outColor = vec4(col, 1.0);
    outDist  = 0.0;
}
