#version 450
// Lightning (lightning.vert): a channel's antialiased core + halo, hidden by the cloud in front of it; or the cloud
// lit around a flash (the resolved cloud composite's distance and opacity: in-cloud diffusion ~5 km, then the
// inverse square — cloud_lightning.glsl's cv2FlashGlow), or a red sprite's head glowing in the thin air.
#include "lightning_draw.glsl"
#include "cloud_occlusion.glsl"

layout(location = 0) in vec2 vLocal;
layout(location = 1) flat in vec4 vCore;
layout(location = 2) flat in vec4 vHalo;
layout(location = 3) flat in vec4 vSeg;
layout(location = 4) flat in vec4 vGlow;
layout(location = 0) out vec4 outColor;

void main()
{
    vec2 uv = gl_FragCoord.xy / vec2(pc.screenW, pc.screenH);
    vec3 x;
    if (vSeg.z > 0.5) {
        // The ray through this pixel, and the cloud composite's four texels about it (own distance and opacity each,
        // blended bilinearly: one filtered distance mixes a cloud's with the no-cloud marker).
        float tanH = tan(pc.fovYRad * 0.5);
        vec3  dc   = normalize(vec3((2.0 * uv.x - 1.0) * tanH * pc.aspect, -(2.0 * uv.y - 1.0) * tanH, -1.0));
        vec3  dir  = transpose(mat3(pc.skyView)) * dc;
        vec4  ga = textureGather(cloudTargetA, uv, 3);
        vec4  gt = textureGather(cloudTargetB, uv, 1);
        vec2  fw = fract(uv * vec2(textureSize(cloudTargetA, 0)) - 0.5);
        vec4  w4 = vec4((1.0 - fw.x) * fw.y, fw.x * fw.y, fw.x * (1.0 - fw.y), (1.0 - fw.x) * (1.0 - fw.y));
        vec4  op4, tC4, air4;
        bool  any = false;
        for (int k = 0; k < 4; ++k) {
            op4[k]  = 1.0 - clamp(gt[k], 0.0, 1.0);
            bool ok = op4[k] >= 0.005 && abs(ga[k]) < 5000.0;
            op4[k] *= ok ? w4[k] : 0.0;
            tC4[k]  = abs(ga[k]) * 1000.0;
            air4[k] = ok ? boltAir(dir * tC4[k], tC4[k]) : 0.0;
            any = any || ok;
        }
        x = vec3(0.0);
        uint n = uint(pc.params2.y + 0.5);
        for (uint fi = 0u; fi < n; ++fi) {
            BoltFlash F = bFlash[fi];
            if (F.o.w <= 0.01) continue;
            if (F.t1.w > 1.5 && F.t1.w < 2.5) {
                // A sprite's head: a flattened red glow ~15 km across, seen through the air (not lit cloud).
                vec3  H  = F.e[0].xyz;
                float tH = dot(H, dir);
                if (tH <= 0.0) continue;
                vec3  hp = H - dir * tH;
                float hv = dot(hp, F.u.xyz), hh = length(hp - F.u.xyz * hv);
                float g  = exp(-(hh * hh) / 8.0e7 - (hv * hv) / 1.6e7);
                if (g < 1e-3) continue;
                float occ = cloudPointVisibilityAt(cloudTargetA, cloudTargetB, uv, length(H), 1.0);
                x += vec3(0.55, 0.008, 0.02) * (pc.params.x * pc.params.y * 0.08 * F.o.w * g * occ);
                continue;
            }
            if (!any) continue;
            for (int e = 0; e < 4; ++e) {
                vec4 em = F.e[e];
                if (em.w <= 0.0) continue;
                // Outside the cone of the emitter's reach (kBoltGlowR): nothing to add.
                float rE = length(em.xyz);
                if (rE > kBoltGlowR && dot(dir, em.xyz) < rE * sqrt(1.0 - (kBoltGlowR * kBoltGlowR) / (rE * rE))) continue;
                float g = 0.0;
                for (int k = 0; k < 4; ++k)
                    g += op4[k] * air4[k] * boltGlow(length(dir * tC4[k] - em.xyz));
                x += vec3(0.80, 0.86, 1.0) * (pc.params.x * pc.params.z * 2.0 * F.o.w * em.w * g);
            }
        }
        if (max(x.r, max(x.g, x.b)) < 1e-4) discard;
    } else {
        float s    = clamp(vLocal.x, 0.0, vSeg.x);
        float d    = length(vec2(vLocal.x - s, vLocal.y));
        float core = clamp(0.5 * vCore.a + 0.5 - d, 0.0, 1.0);
        float halo = vHalo.a > 0.0 ? exp(-(d * d) / (vHalo.a * vHalo.a)) : 0.0;
        x = vCore.rgb * core + vHalo.rgb * halo;
        if (max(x.r, max(x.g, x.b)) < 1e-4) discard;
        // Hidden by the cloud (and rain) in front of it; inside a cloud it shows only through the glow.
        x *= cloudPointVisibilityAt(cloudTargetA, cloudTargetB, uv, vSeg.y, 1.0);
    }
    outColor = vec4(boltTonemap(x), 0.0);
}
