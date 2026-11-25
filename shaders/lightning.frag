#version 450
// Lightning (lightning.vert): a channel's antialiased core + halo, hidden by the cloud in front of it; or the cloud
// lit around a flash (the resolved cloud composite's distance and opacity: in-cloud diffusion ~5 km, then the
// inverse square — lightning_draw.glsl's boltGlow), or a red sprite's head glowing in the thin air.
#include "lightning_draw.glsl"
#include "cloud_occlusion.glsl"
#include "depth.glsl"

layout(location = 0) in vec2 vLocal;
layout(location = 1) flat in vec4 vCore;
layout(location = 2) flat in vec4 vHalo;
layout(location = 3) flat in vec4 vSeg;
layout(location = 4) flat in vec4 vGlow;
layout(location = 5) flat in vec4 vScat;
layout(location = 6) flat in vec4 vScatW;
layout(location = 7) flat in vec4 vScatK;
layout(location = 0) out vec4 outColor;
// The meshes' distance (m, 0 = none; SatMeshRenderer's scene target, full resolution): a mesh pixel is lit by the
// engine lights in sat_mesh.frag (with its own normals), not by the ground term here (2026-10-21).
layout(set = 0, binding = 6, r32f) uniform readonly image2D meshDistImg;

// ── A flash's light on the cloud a pixel shows (2026-11-21, issue_20261121_163317) ──
// As cloud_v2_march.comp lights a cloud sample under a reflector beam (beamFinish): E (ph e^-od + ms D), with the
// irradiance E the flash's by distance (boltGlow), od the cloud's optical depth from the emitter to the cloud point
// (the emitter's volume, flash_volume.glsl), ph the cloud's two-lobe phase at the angle between the light's path and
// the view, and D the slab's diffuse field (cv2DiffuseField: keep the two in step) with od toward the light and the
// pixel's own depth away from it, x N (the flash's own cloud lets all of its light out somewhere: flash_volume.glsl). So a flash behind thick cloud does not light its near side, a thin edge in front
// of a flash lights up (the forward lobe), and a flash inside a cloud shows where the cloud toward it is thin.
float boltHG(float c, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5);
}
// THE DROPLET PHASE (2026-11-23, step 3 of the lighting audit): the single scattering of the cloud a flash or an engine light
// lights, as the march's key light and beams scatter (cv2MicroShape's fit, Jendersie & d'Eon 2023: HG + Draine, droplet
// diameter d = 2 r_eff), analytic: this pass has no Mie table and no droplet size per pixel. r_eff 10 um x the march's
// droplet size (a storm's cloud a few km up), the ice lobe above the glaciation band at the lit point's height (the march's
// cv2MicroIce), the forward peak capped as the Moon's and the beams' (40: the night view's lift). Normalised to a mean of 1.
float boltDropPhase(float c, float h)
{
    float d   = 2.0 * clamp(10.0 * bl.drop.x, 4.0, 30.0);
    float wD  = exp(-0.599085 / (d - 0.641583) - 0.665888);
    float gHG = exp(-0.0990567 / (d - 1.67154));
    float gD  = exp(-2.20679 / (d + 3.91029) - 0.428934);
    float a   = exp(3.62489 - 8.29288 / (d + 5.52825));
    float pW  = (1.0 - wD) * boltHG(c, gHG) + wD * boltHG(c, gD) * (1.0 + a * c * c) / (1.0 + a * (1.0 + 2.0 * gD * gD) / 3.0);
    float ice = 1.0 - smoothstep(-35.0, -10.0, bl.drop.y - 0.0065 * h);
    return min(mix(pW, 0.45 * boltHG(c, 0.995) + 0.55 * boltHG(c, 0.55), ice), 40.0);
}
// == cv2DiffuseField (cloud_v2_march.comp) without its hV (the caller's wD) and with the side escape passed in: a
// flash radiates all round, so less of its light leaves the line from it to the point than leaves a beam's.
float boltDiffuse(float tauT, float tauB, float cosK, float g, float side)
{
    float f = g * g, gp = g / (1.0 + g);
    float t  = (1.0 - f) * tauT, ts = t + (1.0 - f) * tauB;
    float et = exp(-t);
    float C1 = (5.0 - exp(-ts)) / ((1.0 - gp) * ts + 4.0 / 3.0);
    float I1 = C1 - 3.0 * et;
    float I0 = 5.0 - (2.0 / 3.0) * C1 - (1.0 - gp) * C1 * t - 3.0 * et;
    float J  = max(I0 + 0.3 * g * cosK * I1, 0.0) + (1.0 - f) * max(1.0 + 3.0 * gp * cosK, 0.0) * max(et - exp(-tauT), 0.0);
    return 1.6 * J * exp(-side * t);
}
// ph e^-od + ms D for a light r from a cloud point seen with opacity op (ONE formula: a flash's emitter, boltLightAt, and
// an engine light, engineLightAt). od = the cloud's optical depth from the light to the point, N the light's own-cloud
// factor, cosB the cosine between the light's path to the point and the view, tauP the depth of the pixel's own cloud
// (its caller's reading of the opacity), wD the diffuse light's share of the opacity, src the light's own radius (m),
// single / side the single scattering's gain and the diffuse field's side escape.
float boltCloudShade(float od, float N, float cosB, float r, float op, float tauP, float wD, float src, float single, float side, float h)
{
    // The far side (the slab's depth beyond the point, away from the light): the pixel's own cloud. The point is the
    // first cloud the eye meets, so with the light behind it (cosB 1) nothing lies beyond it, and with the light on the
    // eye's side (cosB -1) the view's own depth does. (Read from the volume 1200 m further out, the depth stepped at the
    // volume's distances: rings round every flash seen through cloud.)
    float ob = tauP * (0.5 - 0.5 * cosB);
    // The light is src across (a stretch of channel, a flame), seen from the cloud point under an angle: the forward
    // lobe's peak is that wide at least.
    float s2   = src * src / (r * r + src * src);
    float cosS = max(cosB - 0.5 * s2, -1.0);
    float ph   = (bl.drop.x > 0.0) ? boltDropPhase(cosS, h)   // (h: the lit point's height, m)
               : bl.phase.y * boltHG(cosS, bl.phase.x) + bl.phase.w * boltHG(cosS, bl.phase.z);
    return single * op * ph * exp(-od) + N * bl.ms.z * wD * boltDiffuse(od, ob, cosB, bl.ms.y, side);
}
// The cloud point C (ENU about the eye) seen along dir with opacity op; wD = the diffuse light's share of that opacity
// (the march's hV, 1 - e^-(1 - f) x over the view's depth x into the cloud, summed over the opacity).
float boltLightAt(uint slot, BoltFlash F, vec3 em, vec3 C, vec3 dir, float op, float wD)
{
    vec3  v  = C - em;
    float r  = max(length(v), 1.0);
    vec3  vl = vec3(dot(v, F.t1.xyz), dot(v, F.t2.xyz), dot(v, F.u.xyz));
    // (the depths blended over the directions by a cubic B-spline, eased inside each distance step: fvAt's bilinear
    // blend of a jittered bake drew the volume's 32 x 32 directions as facets and a starburst round the emitter,
    // issue_20261122_213848)
    float od = fvOdFine(slot, vl, r, true);
    float N  = min(fvN(slot), bl.tune2.x);
    float cosB = -dot(v, dir) / r;
    return boltGlow(r) * bl.tune.w * boltCloudShade(od, N, cosB, r, op, -log(max(1.0 - op, 1e-3)), wD, bl.tune.z, bl.tune.y, bl.tune.x,
                                                  length(C + vec3(0.0, 0.0, 6371000.0 + pc.spare.x)) - 6371000.0);
}
// ── An engine light's light on the cloud a pixel shows, through its volume (2026-11-22, issue_20261122_105107) ──
// The same formula per unit of the light's irradiance at the point (the caller's I / (r^2 + r0^2)): Lp the light now,
// C the cloud point. The depth is read from where the volume was BAKED (bl.fvPos: the light moves
// on between bakes, the cloud's shadows stay where the cloud is), and a light's next volume is mixed in over its
// current one (ev.y, ev.z: baked ahead of a moving light and weighted by how far along it is; no step at a bake).
// What differs from a flash's read, for a reason: an engine light mostly stands OUTSIDE the cloud it lights, and lights
// faces turned to it. The depth from the light to such a face is that of its own skin, a few tens of metres of cloud
// (one free path), which neither the volume (steps of 0.4-2 km, filled to 120 m) nor the composite's distance resolves:
// read at the face, the volume's steps and directions drew rings and spokes on a deck lit from below. So the volume
// answers only for the cloud that stands BETWEEN (read bl.elTune.z m short of the point, toward where it was baked),
// and the point's own cloud is the pixel's: its depth (engineTauBetween), all of it between with the light behind it
// (cosB 1), none with the light on the eye's side (the complement of boltCloudShade's far side).
float engineOd(uint s, vec3 C)
{
    vec3  v  = C - bl.fvPos[s].xyz;
    float rv = length(v);
    vec3  t1 = bl.fvT1[s].xyz, t2 = bl.fvT2[s].xyz;
    // (short by elTune.z, by half the way for a point nearer than twice that: the cloud round a light inside one counts)
    return fvOdFine(s, vec3(dot(v, t1), dot(v, t2), dot(v, cross(t1, t2))), rv - min(bl.elTune.z, 0.5 * rv), false);
}
// The depth of the pixel's own cloud for an engine light, FLAT over the composite's last few per cent of
// transmittance. Those are not a depth (where the march stopped, which tile it ran in), and -ln of them is 3 .. 6.9:
// under a steady light with the lobed look's side escape (e^-0.24 a unit of depth) every level line of that residue
// was a dark contour on the shadowed lobes, and a march tile's edge a straight one (2026-11-22; a 0.2-s flash with
// its side escape of 0.03 shows neither: boltLightAt keeps the plain logarithm). Two readings:
//   engineTauBetween, the cloud between the light and the point with the light BEHIND the pixel's cloud: -ln(1 - op)
// up to an opacity of 0.75, eased to an opaque pixel's 6.9 (= -ln 1e-3, the flashes' cap) by 0.95, flat past it;
//   engineTauFar, the slab's far side for a face the light is in front of: -ln(1 - op) up to 0.93, rounded off to
// 2.945 by 0.97 (a parabola: the slope carried over, zero at the end), flat past it. (Eased to 6.9 like the first, a
// half-transparent cloud counted as a thick slab and was lit 1.8x: from 300 km a launch's lit patch had a bright core.)
// SatelliteSim::recordLightning's level takes the same 2.945 for its thick face.
float engineTauBetween(float op)
{
    return mix(-log(1.0 - min(op, 0.95)), 6.9, smoothstep(0.75, 0.95, op));
}
float engineTauFar(float op)
{
    float u = 0.97 - clamp(op, 0.93, 0.97);
    return op < 0.93 ? -log(1.0 - op) : 2.945 - 178.57 * u * u;
}
vec3 gEngView = vec3(0.0);   // (harness views: the last read's depth in the volume, cosB, N)
float engineLightAt(vec4 ev, vec3 Lp, vec3 C, vec3 dir, float op, float wD, float r0, out float od)
{
    float r  = max(length(C - Lp), 1.0);
    uint  sA = uint(ev.x + 0.5);
    od = engineOd(sA, C);
    float N  = fvN(sA);
    if (ev.z > 0.0) {
        uint  sB = uint(ev.y + 0.5);
        od = mix(od, engineOd(sB, C), ev.z);
        N  = mix(N, fvN(sB), ev.z);
    }
    float cosB = -dot(C - Lp, dir) / r;
    // N (the light's own cloud lets all of its light out somewhere) only on light that crossed cloud: a face the light
    // sees through clear air has its light by distance already (a stage half out of a deck: N 1.2-2 on every lit top).
    N   = mix(1.0, min(N, bl.tune2.x), 1.0 - exp(-od));
    gEngView = vec3(od, cosB, N);
    od += engineTauBetween(op) * (0.5 + 0.5 * cosB);
    return bl.elTune.w * boltCloudShade(od, N, cosB, r, op, engineTauFar(op), wD, r0, bl.elTune.y, bl.elTune.x,
                               length(C + vec3(0.0, 0.0, 6371000.0 + pc.spare.x)) - 6371000.0);
}

void main()
{
    vec2 uv = gl_FragCoord.xy / vec2(pc.screenW, pc.screenH);
    gl_FragDepth = gl_FragCoord.z;   // (the flashes' light at a surface edge writes its cloud's depth: below)
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
        // (and the same texels' second and third moments, about the first one's distance: tLn / tLf below)
        float tRef = -1.0, dM1 = 0.0, dM2 = 0.0, dM3 = 0.0;
        {
            vec2 hT2 = 0.5 / vec2(textureSize(cloudTargetA, 0));
            for (int j = 0; j < 4; ++j) {
                vec2 o2 = vec2((j & 1) == 0 ? -hT2.x : hT2.x, (j & 2) == 0 ? -hT2.y : hT2.y);
                vec4 a4 = textureGather(cloudTargetA, uv + o2, 3);
                vec4 t4 = textureGather(cloudTargetB, uv + o2, 1);
                for (int k = 0; k < 4; ++k) {
                    float op = 1.0 - clamp(t4[k], 0.0, 1.0);
                    if (op < 0.005 || abs(a4[k]) >= 5000.0 || cloudAlphaIsEmit(a4[k])) continue;
                    float tk = abs(a4[k]) * 1000.0;
                    tLs += op * tk;
                    wLs += op;
                    if (tRef < 0.0) tRef = max(tk, 1.0);
                    float dk = tk / tRef - 1.0;
                    dM1 += op * dk;
                    dM2 += op * dk * dk;
                    dM3 += op * dk * dk * dk;
                }
            }
            tLs = wLs > 0.0 ? tLs / wLs : 0.0;
        }
        float airLs = wLs > 0.0 ? boltAir(dir * tLs, tLs) : 0.0;
        // Where the texels' distances differ (a lobe's outline over farther cloud), the smoothed distance is a point in
        // the clear air between the two: no cloud between it and a light, so with an engine light's volume read there
        // every shadowed lobe had a lit outline. The volume is read at TWO distances instead, the two points (tLn, tLf,
        // the far one's weight wLf) that have the texels' mean, spread and skew: for a lobe over farther cloud exactly
        // the two, each with its share, and as smooth from pixel to pixel as the mean is (their nearest and farthest
        // stepped at every texel: a staircase along each shadow's edge). One read where the spread is under 1%.
        float tLn = tLs, tLf = tLs, wLf = -1.0;
        if (wLs > 0.0) {
            dM1 /= wLs; dM2 /= wLs; dM3 /= wLs;
            float var = max(dM2 - dM1 * dM1, 0.0), sd = sqrt(var);
            if (sd * tRef > 0.01 * tLs + 3.0) {
                float gam = clamp((dM3 - 3.0 * dM1 * dM2 + 2.0 * dM1 * dM1 * dM1) / (var * sd), -8.0, 8.0);
                float rho = 0.5 * (sqrt(gam * gam + 4.0) - gam);
                tLn = max(tRef * (1.0 + dM1 - sd * rho), 1.0);
                tLf = tRef * (1.0 + dM1 + sd / rho);
                wLf = rho * rho / (1.0 + rho * rho);
            }
        }
        // (No surface orientation: the screen derivatives of the half-res distance drew a 2-px crosshatch. The relief
        // that shows comes from the opacity: thin edges and wisps take less of the light, as the cloud's own does.)
        float opK = opF * sqrt(opF);
        // (the diffuse light's share of the opacity, as the flashes': boltLightAt's wD)
        float aFe = 1.0 - bl.ms.y * bl.ms.y;
        float wDe = opF - (1.0 - pow(max(1.0 - opF, 0.0), 1.0 + aFe)) / (1.0 + aFe);
        float trBar = dot(tr4, w4);
        // The haze is lit up to the ground, and in the cloud's share of the pixel only up to the cloud (a threshold on
        // the opacity drew the cloud's half-res outline into the haze as a staircase).
        bool  onMesh;
        float md;
        {
            ivec2 ms = imageSize(meshDistImg);
            ivec2 mp = clamp(ivec2(gl_FragCoord.xy * vec2(ms) / vec2(pc.screenW, pc.screenH)), ivec2(0), ms - 1);
            md = imageLoad(meshDistImg, mp).r;
            onMesh = md > 0.0 && md < tGround * 1.05 + 2.0;
        }
        // A mesh pixel sees only the cloud / fog / haze IN FRONT of the mesh (2026-10-21, issue 20261021_174219): the
        // half-res texel a silhouette or an open lattice covers in part carries what lies behind it too (scene_depth.comp
        // clamps only texels the mesh covers whole), and sat_sky.frag keeps that out of the mesh pixel by its own test
        // (kM). Lit here regardless, the fog behind a catch tower lit its members and the lattice's half-res texels drew
        // as blocks. Same test as sat_sky.frag's on the cloud's distance; the haze ends at the mesh.
        float kMesh = 1.0;
        if (md > 0.0) {
            tGround = min(tGround, md);
            kMesh = 1.0 - smoothstep(md, md * 1.25 + 20.0, tLs);
        }
        float tEndC = min(tGround, 4.0e5);
        float tEndK = min(tEndC, tCbar > 0.0 ? tCbar : tEndC);
        x = vec3(0.0);
        float odDbg = 0.0;
        vec3  viewDbg = vec3(0.0);
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
                    // A light that holds a volume (bl.elVol.w its share: launches.engine_light_volume, the host's
                    // engineLightVolumes) lights the cloud through the cloud between, as a flash does; one without
                    // (too many lights, none near cloud, the setting off) by distance alone.
                    vec4 ev = bl.elVol[i];
                    if (ev.w > 0.0) {
                        float odN, odF = 0.0;
                        float gv = wLf < 0.0 ? engineLightAt(ev, Lp, C, dir, opF, wDe, L.ia.w, odN)
                                             : mix(engineLightAt(ev, Lp, dir * tLn, dir, opF, wDe, L.ia.w, odN),
                                                   engineLightAt(ev, Lp, dir * tLf, dir, opF, wDe, L.ia.w, odF), wLf);
                        g += kMesh * airLs * mix(opK * 0.85 * mix(1.0, 0.35, far), gv, ev.w) * reachWin(r2, reach2) / (r2 + r02);
                        // (harness "lightning light e_od= e_od_light=": where that light's cloud depth to the pixel's
                        // cloud is at least e_od, drawn green in place of the lights)
                        if (bl.tune2.y != 0.0 && i == uint(bl.tune2.z + 0.5)) {
                            odDbg = (wLf < 0.0 ? odN : mix(odN, odF, wLf)) >= bl.tune2.y ? opF : 0.0;
                            viewDbg = gEngView;
                        }
                    }
                    else
                        g += kMesh * opK * airLs * 0.85 * mix(1.0, 0.35, far) * reachWin(r2, reach2) / (r2 + r02);
                }
            }
            // The ground (and the sea), behind the clouds' transmittance. Not on a mesh (the pad's, a ship's, a
            // rocket's): sat_mesh.frag lights those with their own normals (it took the local up as every surface's).
            if (L.col.y < 0.5 && !onMesh) {
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
                // The light's own extent softens the 1/d core (2026-10-21): it was floored at the core radius r0 (25 m
                // .. ~100 m for a flame aloft, 40 m for a ship's floods), so the lit haze was a flat disc that size
                // with a rim where the 1/d fall-off began: a round milky halo round a landing burn or a followed
                // stage. A third of r0 in quadrature: a glow concentrated on the light, no edge.
                float dP = length(Lp - dir * t0);
                float d  = sqrt(dP * dP + 0.12 * L.ia.w * L.ia.w);
                if (d < reach) {
                    float hh  = max(hL, 0.0);
                    float sig = 1.2e-5 * exp(-hh / 8000.0) + 2.5e-5 * exp(-hh / 1500.0);
                    float a0  = atan(-t0 / d);
                    float ang = mix(atan((tEndC - t0) / d), atan((tEndK - t0) / d), opF) - a0;
                    float tc  = clamp(t0, 0.0, tEndC);
                    float gH = 0.25 * sig * max(ang, 0.0) / d * boltAir(dir * tc, tc) * (tc > tCmin ? trBar : 1.0) *
                               reachWin(d * d, reach2);
                    // The cloud between the light and the lit haze (2026-11-22): a light that holds a volume lights
                    // the air along the ray through the cloud depth to the ray's nearest point to it (where most of
                    // the integral is). Without it a rocket under a deck still drew its glow in the air above the
                    // deck, and one above a deck in the air under it: 7-19% of the old cloud light there, all that
                    // was left once the cloud itself was shaded through the volume.
                    vec4 evH = bl.elVol[i];
                    if (evH.w > 0.0) {
                        vec3  Ph  = dir * clamp(t0, 0.0, tEndC);
                        float odH = engineOd(uint(evH.x + 0.5), Ph);
                        if (evH.z > 0.0) odH = mix(odH, engineOd(uint(evH.y + 0.5), Ph), evH.z);
                        gH *= mix(1.0, exp(-odH), evH.w);
                    }
                    g += gH;
                }
            }
            x += L.ia.rgb * (L.c.w * g);
        }
        x *= pc.params.x;
        if (bl.tune2.y > 0.0) x = vec3(0.0, 4.0 * odDbg, 0.0);
        // (harness `lightning light e_view=<n>`: in place of the lights, a grey of what the cloud term is made from: 1 the
        // cloud's distance (log2, 100 m .. 25.6 km), 2 its opacity, 3 the strongest light's depth in the volume / 16,
        // 4 its cosB (0.5 + 0.5 c), 5 the two-distance read's far weight (black: one read), 6 its N / 8)
        if (bl.tune2.y < -0.5) {
            int   vw = int(0.5 - bl.tune2.y);
            float v  = vw == 1 ? (wLs > 0.0 ? log2(max(tLs, 100.0) / 100.0) / 8.0 : 0.0) :
                       vw == 2 ? opF :
                       vw == 3 ? viewDbg.x / 16.0 :
                       vw == 4 ? 0.5 + 0.5 * viewDbg.y :
                       vw == 5 ? (wLf < 0.0 ? 0.0 : 0.25 + 0.75 * wLf) : viewDbg.z / 8.0;
            x = vec3(-log(1.0 - 0.98 * clamp(v, 0.0, 1.0)));
        }
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
        // A SURFACE EDGE across the four texels (2026-11-23, TERRAIN_EDGES_PLAN phase 1.2: "lightning flashes show a
        // hard, pixelated boundary between the lit sky and the ground"). A texel marched to the ground carries only the
        // cloud in front of the ground, one marched to the sky the cloud behind the ridge; blended bilinearly, the flash
        // lit cloud bled onto the ridge's pixels and the sky pixels above it came out darker, both on the half-res grid:
        // a 2-px staircase drawn after the sky TAA (nothing smooths it). Where the texels hold both classes, the light is
        // the sky texels' alone (their weights renormalised), drawn just past the ground those texels hit so the main
        // pass's FULL-RES depth (the per-pixel march's own silhouette) cuts it off every ground pixel. Measured
        // (Chamonix, a ridge against flash-lit cloud, 1920 x 1009): the lit edge follows the daylight silhouette's
        // 1-px steps (was 2 rows x 6-8 px on even rows); +0.08 ms in the pass while a flash is up.
        vec4  wC = w4;
        bool  edgeCut = false;
        float tGndMax = 0.0;
        {
            // Each cloud texel's class from the scene depth under its centre (the two grids differ by a texel's
            // rounding at least), and the farthest ground among them (the cut below).
            vec2  cS = vec2(textureSize(cloudTargetA, 0));
            ivec2 dS = textureSize(sceneDepthTex, 0);
            vec2  b0 = floor(uv * cS - 0.5);
            vec4  skyE;
            float nSky = 0.0;
            bool  moved = false;
            // The block itself, then (all four ground) its eight neighbours one texel over: the depth passes count a
            // near miss of a crest as a hit, so the half-res ground stands a texel proud of the full-res silhouette and
            // the sky pixels just over the ridge had only ground texels round them (no light: the 2-px steps). The
            // nearest block holding a sky texel lends its light; the depth cut keeps it off the ground's pixels.
            for (int j = 0; j < 9; ++j) {
                vec2 bo = j == 0 ? vec2(0.0) : vec2(j == 3 || j == 5 || j == 6 ? -1.0 : (j == 4 || j == 7 || j == 8 ? 1.0 : 0.0),
                                                    j == 1 || j == 5 || j == 7 ? -1.0 : (j == 2 || j == 6 || j == 8 ? 1.0 : 0.0));
                nSky = 0.0;
                for (int k = 0; k < 4; ++k) {
                    vec2  ok = vec2(k == 1 || k == 2 ? 1.0 : 0.0, k < 2 ? 1.0 : 0.0);   // textureGather's texel order
                    ivec2 dp = clamp(ivec2((b0 + bo + ok + 0.5) / cS * vec2(dS)), ivec2(0), dS - 1);
                    float dk = texelFetch(sceneDepthTex, dp, 0).r;
                    skyE[k]  = dk >= 1e29 ? 1.0 : 0.0;
                    nSky    += skyE[k];
                    if (dk < 1e29) tGndMax = max(tGndMax, dk);
                }
                if (nSky > 0.5) {
                    if (j > 0) {
                        moved = true;
                        ga = textureGather(cloudTargetA, uv + bo / cS, 3);
                        gt = textureGather(cloudTargetB, uv + bo / cS, 1);
                    }
                    break;
                }
            }
            float wSky = dot(skyE, w4);
            if (nSky > 0.5 && (nSky < 3.5 || moved)) {
                edgeCut = true;
                wC = wSky > 1e-4 ? skyE * w4 / wSky : skyE * 0.25 * (4.0 / nSky);
            }
        }
        vec4  op4, tC4, air4;
        bool  any = false;
        float tCutMin = 1e30;
        for (int k = 0; k < 4; ++k) {
            op4[k]  = 1.0 - clamp(gt[k], 0.0, 1.0);
            bool ok = op4[k] >= 0.005 && abs(ga[k]) < 5000.0 && !cloudAlphaIsEmit(ga[k]) && wC[k] > 0.0;
            op4[k] *= ok ? wC[k] : 0.0;
            tC4[k]  = abs(ga[k]) * 1000.0;
            air4[k] = ok ? boltAir(dir * tC4[k], tC4[k]) : 0.0;
            any = any || ok;
            if (ok) tCutMin = min(tCutMin, tC4[k]);
        }
        // The cut: just past the ground those texels hit (a ground pixel there is at most that far, give or take the
        // slope across a texel), and in front of the cloud the light is on (a sky pixel's depth is its opaque cloud's).
        if (edgeCut && tCutMin < 1e29) gl_FragDepth = sceneDepthFromDistance(min(1.1 * tGndMax, 0.95 * tCutMin));
        // The cloud this pixel shows, as one place: its opacity, and its distance and air weighted by it.
        float opS = 0.0, tS = 0.0, airS = 0.0;
        for (int k = 0; k < 4; ++k) { opS += op4[k]; tS += op4[k] * tC4[k]; airS += op4[k] * air4[k]; }
        bool  phys = bl.ms.x > 0.5 && opS > 1e-4;
        float opP  = clamp(opS, 0.0, 1.0);
        float aF   = 1.0 - bl.ms.y * bl.ms.y;
        float wD   = opP - (1.0 - pow(1.0 - opP, 1.0 + aF)) / (1.0 + aF);
        vec3  Cm   = phys ? dir * (tS / opS) : vec3(0.0);
        float airM = phys ? airS / opS : 0.0;
        // The flash's colour on the cloud (2026-11-22, BoltColour), less the blue the air to that cloud takes.
        vec3  cTint = bc.cloud.rgb * boltAirTint(opS > 1e-4 ? airS / opS : 1.0);
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
                uint  sl = (uint(F.m.w + 0.5) >> uint(4 * e)) & 15u;
                if (phys && sl < 15u)
                    g = airM * boltLightAt(sl, F, em.xyz, Cm, dir, opP, wD);
                else
                    for (int k = 0; k < 4; ++k)
                        g += op4[k] * air4[k] * boltGlow(length(dir * tC4[k] - em.xyz));
                x += cTint * (pc.params.x * pc.params.z * 2.0 * F.o.w * em.w * g);
            }
        }
        if (max(x.r, max(x.g, x.b)) < 1e-4) discard;
    } else {
        float s    = clamp(vLocal.x, 0.0, vSeg.x);
        float d    = length(vec2(vLocal.x - s, vLocal.y));
        float core = clamp(0.5 * vCore.a + 0.5 - d, 0.0, 1.0);
        float halo = vHalo.a > 0.0 ? exp(-(d * d) / (vHalo.a * vHalo.a)) : 0.0;
        // The stroke's colour (2026-11-22, lightning_draw.glsl BoltColour; all 0 at gain 0): the violet corona sheath
        // hugging the channel, and the light the air scatters, each piece's integral of 1 / distance^2 along itself
        // (1 / distance round the channel, not a Gaussian), 0 at the quad's edge: indigo in clear air, neutral and
        // with a wider core where rain or cloud stands in front of the stroke.
        float d2 = d * d;
        float Dc = sqrt(d2 + vScat.a), Dw = sqrt(d2 + vScatW.a);
        vec3  gc = vScat.rgb * max(1.0 / (Dc * Dc * vScatK.x + Dc * 0.31831) - vScatK.y, 0.0);
        vec3  gw = vScatW.rgb * max(1.0 / (Dw * Dw * vScatK.x + Dw * 0.31831) - vScatK.z, 0.0);
        x = vCore.rgb * core + vHalo.rgb * halo + vGlow.rgb * exp(-d2 * vGlow.a);
        vec3  xm = x + max(gc, gw);
        if (max(xm.r, max(xm.g, xm.b)) < 1e-4) discard;
        // Hidden by the cloud (and rain) in front of it; inside a cloud it shows only through the glow.
        float vis = cloudPointVisibilityAt(cloudTargetA, cloudTargetB, uv, vSeg.y, 1.0);
        // (harness `lightning color knock=`, SatelliteSimLightning.cpp g_boltKnock: 2 nothing in front, 4 the core and
        // halo at a fixed level, 8 the core as a readout of the composite in front of it: r the nearest opaque texel's
        // distance / the stroke's range / 2, g the gathered transmittance, b the share of the four texels that are opaque)
        int knock = int(bc.wet.z + 0.5);
        if ((knock & 2) != 0) vis = 1.0;
        if ((knock & 4) != 0) x = vec3(4.0) * max(core, halo);
        if ((knock & 8) != 0) {
            if (core < 0.5) discard;
            vec4 ga = textureGather(cloudTargetA, uv, 3), gt = textureGather(cloudTargetB, uv, 1);
            float tO = 1e9, nO = 0.0;
            for (int k = 0; k < 4; ++k) if (ga[k] >= 0.0) { tO = min(tO, ga[k] * 1000.0); nO += 0.25; }
            outColor = vec4(clamp(tO / vSeg.y * 0.5, 0.0, 1.0), clamp(dot(gt, vec4(0.25)), 0.0, 1.0), nO, 0.0);
            return;
        }
        // Seen through rain alone (vSeg.z < 0: under the cloud's base, from under it): the composite cannot tell the rain
        // from cloud and takes the whole channel behind 1-2 km of it. The terrain, the sea and meshes in front still hide
        // it (the scene depth: this quad skipped the depth test); the rain leaves at least bc.wet.w of its light, and the
        // wet glow's share follows what the composite took.
        float visL = vis;
        if (vSeg.z < -0.5) {
            vec4 gd = textureGather(sceneDepthTex, uv, 0);
            if (min(min(gd.x, gd.y), min(gd.z, gd.w)) < vSeg.y * 0.99 - 30.0) discard;
            // A piece above the base (vScatK.w: where its ray reaches the base): floored only when everything the four
            // texels have in front (transmittance under 0.9) starts short of the base, i.e. is the rain under it; a
            // texel whose cloud starts at or past the base is the cloud itself, and the composite decides as before.
            bool rainFront = true;
            if (vScatK.w < 1e29) {
                vec4 ga = textureGather(cloudTargetA, uv, 3), gt = textureGather(cloudTargetB, uv, 1);
                for (int k = 0; k < 4; ++k)
                    if (gt[k] < 0.9 && abs(ga[k]) < 5000.0 && !cloudAlphaIsEmit(ga[k]) && abs(ga[k]) * 1000.0 >= 0.85 * vScatK.w)
                        rainFront = false;
            }
            if (rainFront) visL = max(vis, bc.wet.w);
        }
        x = (x + mix(gc, gw, clamp(bc.wet.x + bc.wet.y * (1.0 - vis), 0.0, 1.0))) * visL;
    }
    outColor = vec4(boltTonemap(x), 0.0);
}
