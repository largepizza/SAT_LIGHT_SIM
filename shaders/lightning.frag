#version 450
// Lightning (lightning.vert): a channel's antialiased core + halo, hidden by the cloud in front of it; or the cloud
// lit around a flash (the resolved cloud composite's distance and opacity: in-cloud diffusion ~5 km, then the
// inverse square — lightning_draw.glsl's boltGlow), or a red sprite's head glowing in the thin air.
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
    if (vSeg.z > 1.5) {
        // ── The engine lights (2026-10-20): burning rocket engines, a ship's entry glow, the pads' floodlights ──
        // Each a point light of intensity I (suns x m^2): on the cloud composite (clouds and fog: albedo 0.9, facing it,
        // the light floored at its core radius; from the far side of the cloud the diffuse transmission, 0.35), on the
        // ground (the scene depth: albedo 0.15 x the cosine to the light; not on the vehicle's own hull) and on the haze
        // along the ray (single scattering, in closed form: the integral of 1 / (d^2 + s^2) is an arctangent).
        float tanH = tan(pc.fovYRad * 0.5);
        vec3  dc   = normalize(vec3((2.0 * uv.x - 1.0) * tanH * pc.aspect, -(2.0 * uv.y - 1.0) * tanH, -1.0));
        vec3  dir  = transpose(mat3(pc.skyView)) * dc;
        vec4  ga = textureGather(cloudTargetA, uv, 3);
        vec4  gt = textureGather(cloudTargetB, uv, 1);
        vec4  gd = textureGather(sceneDepthTex, uv, 0);
        vec2  fw = fract(uv * vec2(textureSize(cloudTargetA, 0)) - 0.5);
        vec4  w4 = vec4((1.0 - fw.x) * fw.y, fw.x * fw.y, fw.x * (1.0 - fw.y), (1.0 - fw.x) * (1.0 - fw.y));
        const float RE = 6371000.0;
        float hE  = max(pc.spare.x, 0.0);
        vec3  ctr = vec3(0.0, 0.0, RE + hE);   // p + ctr: a point ENU about the eye, from the Earth's centre
        // The cloud's opacity from the FILTERED transmittance (smooth across the half-res texels: per gathered texel,
        // gated on a valid distance, the lit cloud's outline was a staircase); its distance and air from the texels
        // that have one, weighted bilinearly among themselves.
        // A 3x3-texel tent (four bilinear taps half a texel out): one bilinear tap still drew the half-res texels as
        // 2-px steps where the light is strong.
        vec2  hT  = 0.5 / vec2(textureSize(cloudTargetB, 0));
        float opF = 1.0 - clamp(0.25 * (texture(cloudTargetB, uv + vec2(-hT.x, -hT.y)).g + texture(cloudTargetB, uv + vec2(hT.x, -hT.y)).g +
                                         texture(cloudTargetB, uv + vec2(-hT.x, hT.y)).g + texture(cloudTargetB, uv + hT).g),
                                0.0, 1.0);
        vec4  wk4, tC4, air4, tr4, airG4;
        float wS = 0.0, opS = 0.0, tOp = 0.0, tGround = 1e30, tCmin = 1e30;
        for (int k = 0; k < 4; ++k) {
            tr4[k]  = clamp(gt[k], 0.0, 1.0);
            float op = 1.0 - tr4[k];
            // (not a texel whose opacity is a plume's: cloud_march lit it by the engine lights already, cloudAlphaIsEmit)
            bool ok = op >= 0.005 && abs(ga[k]) < 5000.0 && !cloudAlphaIsEmit(ga[k]);
            wk4[k]  = ok ? w4[k] : 0.0;
            tC4[k]  = abs(ga[k]) * 1000.0;
            air4[k] = ok ? boltAir(dir * tC4[k], tC4[k]) : 0.0;
            wS  += wk4[k];
            opS += op * wk4[k];
            tOp += op * wk4[k] * tC4[k];
            if (ok) tCmin = min(tCmin, tC4[k]);
            bool okG = gd[k] < 1e29;
            airG4[k] = okG ? boltAir(dir * gd[k], gd[k]) : 0.0;
            if (okG) tGround = min(tGround, gd[k]);
        }
        wk4 *= wS > 0.0 ? 1.0 / wS : 0.0;
        float tCbar = opS > 0.0 ? tOp / opS : 0.0;
        // The cloud's distance for its LIGHT: a [1 2 1] tent over the 3x3 half-res texels (four gathers half a texel
        // out), weighted by opacity (2026-10-20). The light goes as 1 / r^2 of that distance, and the composite's
        // distances step from texel to texel (the sparse march's blocks, a near cloud's edge over a far one): taken per
        // gathered texel, a lit deck's outline and every gap in it were drawn as a staircase of 4-8 px steps.
        float tLs = 0.0, wLs = 0.0;
        {
            vec2 hT2 = 0.5 / vec2(textureSize(cloudTargetA, 0));
            for (int j = 0; j < 4; ++j) {
                vec2 o2 = vec2((j & 1) == 0 ? -hT2.x : hT2.x, (j & 2) == 0 ? -hT2.y : hT2.y);
                vec4 a4 = textureGather(cloudTargetA, uv + o2, 3);
                vec4 t4 = textureGather(cloudTargetB, uv + o2, 1);
                for (int k = 0; k < 4; ++k) {
                    float op = 1.0 - clamp(t4[k], 0.0, 1.0);
                    if (op < 0.005 || abs(a4[k]) >= 5000.0 || cloudAlphaIsEmit(a4[k])) continue;
                    tLs += op * abs(a4[k]) * 1000.0;
                    wLs += op;
                }
            }
            tLs = wLs > 0.0 ? tLs / wLs : 0.0;
        }
        float airLs = wLs > 0.0 ? boltAir(dir * tLs, tLs) : 0.0;
        // (No surface orientation: the screen derivatives of the half-res distance drew a 2-px crosshatch. The relief
        // that shows comes from the opacity: thin edges and wisps take less of the light, as the cloud's own does.)
        float opK = opF * sqrt(opF);
        float trBar = dot(tr4, w4);
        // The haze is lit up to the ground, and in the cloud's share of the pixel only up to the cloud (a threshold on
        // the opacity drew the cloud's half-res outline into the haze as a staircase).
        float tEndC = min(tGround, 4.0e5);
        float tEndK = min(tEndC, tCbar > 0.0 ? tCbar : tEndC);
        x = vec3(0.0);
        uint n = min(uint(pc.params2.z + 0.5), 32u);
        for (uint i = 0u; i < n; ++i) {
            EngineLight L = elList[kEngineLightBase + i];
            vec3  Lp = L.c.xyz;
            float reach = L.col.z;
            float rE = length(Lp);
            if (rE > reach && dot(dir, Lp) < rE * sqrt(1.0 - (reach * reach) / (rE * rE))) continue;
            float r02 = L.ia.w * L.ia.w, reach2 = reach * reach;
            float hL  = length(Lp + ctr) - RE;
            float g = 0.0;
            // Clouds and fog (at the tent-smoothed distance above).
            if (wLs > 0.0 && wS > 0.0) {
                vec3  C  = dir * tLs;
                vec3  dl = Lp - C;
                float r2 = dot(dl, dl);
                if (r2 <= reach2) {
                    float hC  = length(C + ctr) - RE;
                    float far = (hL - hC) * (hE - hC) < 0.0 ? smoothstep(150.0, 800.0, min(abs(hL - hC), abs(hE - hC))) : 0.0;
                    g += opK * airLs * 0.85 * mix(1.0, 0.35, far) * reachWin(r2, reach2) / (r2 + r02);
                }
            }
            // The ground (and the sea, the pad's meshes), behind the clouds' transmittance.
            if (L.col.y < 0.5) {
                for (int k = 0; k < 4; ++k) {
                    if (gd[k] >= 1e29) continue;
                    vec3  P  = dir * gd[k];
                    vec3  dl = Lp - P;
                    float r2 = dot(dl, dl);
                    if (r2 > reach2) continue;
                    float ax = -dot(dl, L.ib.xyz);
                    vec3  rd = -dl - ax * L.ib.xyz;
                    if (ax > -5.0 && ax < L.ib.w && dot(rd, rd) < L.col.x * L.col.x) continue;   // the hull itself
                    float cosL = max(dot(normalize(P + ctr), dl), 0.0) * inversesqrt(max(r2, 1.0));
                    g += w4[k] * tr4[k] * airG4[k] * 0.15 * cosL * reachWin(r2, reach2) / (r2 + 0.25 * r02);
                }
            }
            // The haze along the ray: sigma(h) / 4 x the integral of I / (d^2 + (t - t0)^2) over [0, tEnd].
            {
                float t0 = dot(dir, Lp);
                float d  = max(length(Lp - dir * t0), L.ia.w);
                if (d < reach) {
                    float hh  = max(hL, 0.0);
                    float sig = 1.2e-5 * exp(-hh / 8000.0) + 2.5e-5 * exp(-hh / 1500.0);
                    float a0  = atan(-t0 / d);
                    float ang = mix(atan((tEndC - t0) / d), atan((tEndK - t0) / d), opF) - a0;
                    float tc  = clamp(t0, 0.0, tEndC);
                    g += 0.25 * sig * max(ang, 0.0) / d * boltAir(dir * tc, tc) * (tc > tCmin ? trBar : 1.0) *
                         reachWin(d * d, reach2);
                }
            }
            x += L.ia.rgb * (L.c.w * g);
        }
        x *= pc.params.x;
        if (max(x.r, max(x.g, x.b)) < 1e-4) discard;
    } else if (vSeg.z > 0.5) {
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
            bool ok = op4[k] >= 0.005 && abs(ga[k]) < 5000.0 && !cloudAlphaIsEmit(ga[k]);
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
