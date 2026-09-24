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

layout(location = 0) out vec4 outColor;

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

    const float a2    = max(mat.roughness * mat.roughness, 1e-8);
    const bool  beck  = mat.beckmann != 0u;
    const vec3  diffC = mat.color * mat.albedo;
    const vec3  specC = (mat.f0 >= 0.5) ? mat.color : vec3(1.0); // metals tint their reflection
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
            float lod = log2(max(2.0 * mat.roughness * h / 4900.0, 1.0)) + frame.params.z;
            vec3  env = earthEnv(ro, Rd, frame.sunDir.xyz, lod, frame.earthCenter.w);
            float Fr  = mat.f0 + (max(1.0 - mat.roughness, mat.f0) - mat.f0) * pow(1.0 - nv, 5.0);
            L += env * specC * Fr;
        }
    }

    // Viewer output: the sky's own exposure curve, straight into the display-format target.
    vec3 col = vec3(1.0) - exp(-frame.camPos.w * L);
    outColor = vec4(col, 1.0);
}
