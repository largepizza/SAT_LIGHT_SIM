#ifndef SATLIGHTSIM_CV2_LAYER_MAP_GLSL
#define SATLIGHTSIM_CV2_LAYER_MAP_GLSL
// ── The layer shadow map (2026-10-14, .plans/LAYER_SHADOW_MAP_PLAN.md) ──────────────────────────────────────────────
// The mid layer (altocumulus / altostratus) and the high layer (cirrus) shade everything under them over hundreds of km
// at a low Sun, far past the light march's ~8 km. The near Sun-shadow lattices (fine grid, shaft rings) cannot carry
// those shadows: any lattice confined to a region about the eye draws that region (a grey disc under altocumulus that
// followed the observer, issue 20261013_192258). This map exists wherever the layers do.
//
// Two LEVELS of a 2D lattice on a gnomonic plane tangent at an ANCHOR fixed in the DRIFTED cloud frame (the clouds stand
// still in it apart from their evolution), 256 x 256 cells each, stored toroidally about the eye: level 0 with cells of
// cv2.layA.w (1 km), level 1 of cv2.layE.w (6 km, +-768 km). A point's cell coordinate = R x (d.E, d.N) / (d.A) / cell,
// d its direction from the Earth's centre (observer ENU axes; the CPU turns the anchor's axes into the frame each frame).
//
//  * COLUMN map (cloud_v2_layers.comp, mode 0): per cell, running means over jittered points of the cell of each layer's
//    vertical optical depth tau and tau^2 (the variance makes the slant pass's mean transmittance right: a broken
//    altocumulus field passes more light than its mean tau says), and tau x h, tau x h^2 (h km relative to the layer's
//    reference height cv2.layRef.xy): the layer's height and thickness over the cell. The field is read at a 100-m
//    footprint: coarse reads do not conserve the layers' optical depth (plan F2: altocumulus 2x too thick, cirrus 1/3).
//  * SLANT map (mode 1): per cell X of the layer's reference sphere, the Sun's transmittance through that layer along the
//    whole Sun ray through X, integrated over the columns the ray crosses (their own heights and thicknesses), with the
//    gamma model E[exp(-m tau)] = (1 + m var / mean)^(-mean^2 / var); and the layer's band at X (for the lookup's share).
//  * LOOKUP (the march's key light, cv2LayerShadowT): where a sample's light march ends, the Sun ray's crossing of each
//    layer's reference sphere, four slant cells bilinear, T^(share of the layer above the point). What the light march
//    saw it counts; the map counts the rest.
//
// Every value carries a STAMP (which window wrap of its slot it was written for, and the anchor's epoch): a slot that
// changed hands as the eye moved, or a value from before a re-anchor, reads as invalid (cloud_lightning.glsl's rings).
// Define CV2_LAYER_BINDING (and CV2_LAYER_WRITE for the bake) after including clouds_v2.glsl.
const int   kCv2LayN     = 256;
const int   kCv2LayCells = kCv2LayN * kCv2LayN;
const int   kCv2LayOff   = 4096;     // keeps absolute cell indices positive (the CPU re-anchors past 1500 km, SatelliteSimCloudsV2.cpp)
const float kCv2LayR     = 6371000.0;
layout(std430, set = 0, binding = CV2_LAYER_BINDING)
#ifndef CV2_LAYER_WRITE
readonly
#endif
buffer CV2LayerBuf {
    // [(level * cells + slot) * 2]: x half2(E[tauMid], E[tauMid^2]), y half2(E[tauHigh], E[tauHigh^2]),
    // z half2(E[tauMid h], E[tauMid h^2]), w the same for the high layer (h in km from the layer's reference height);
    // [.. * 2 + 1]: x the stamp | bakes << 16, yzw spare.
    uvec4 cv2LayCol[2 * kCv2LayCells * 2];
    // [level * cells + slot]: x half2(T mid, T high), y half2(mid band bottom, top), z the same for the high layer
    // (km from the reference height), w the stamp.
    uvec4 cv2LaySlant[2 * kCv2LayCells];
};

float cv2LayCell(int lv)  { return lv == 0 ? cv2.layA.w : cv2.layE.w; }
vec2  cv2LayEyeC(int lv)  { return lv == 0 ? cv2.layEye.xy : cv2.layEye.zw; }
// A direction (observer ENU, any length) -> its absolute cell coordinate on level lv.
vec2 cv2LayCoord(vec3 d, int lv)
{
    return vec2(dot(d, cv2.layE.xyz), dot(d, cv2.layN.xyz)) * (kCv2LayR / (dot(d, cv2.layA.xyz) * cv2LayCell(lv)));
}
int  cv2LaySlot(ivec2 c)  { return ((c.y & (kCv2LayN - 1)) << 8) | (c.x & (kCv2LayN - 1)); }
// Bit 15 set (a cleared buffer reads invalid), 5 + 5 bits of the window wrap, 4 of the anchor's epoch.
uint cv2LayStamp(ivec2 c)
{
    return 0x8000u | (uint((c.x + kCv2LayOff) >> 8) & 31u) | ((uint((c.y + kCv2LayOff) >> 8) & 31u) << 5u)
         | ((uint(cv2.layBake.z) & 15u) << 10u);
}
// The reference spheres: the slant map's cells lie on them, and the lookup maps a Sun ray's crossing there.
float cv2LayRefH(int L) { return L == 0 ? cv2.layRef.x : cv2.layRef.y; }
// The heights each layer can occupy anywhere (cv2MidSigma's / cv2HighSigma's own gates).
vec2  cv2LayBand(int L) { return L == 0 ? vec2(2800.0, 14000.0) : vec2(5500.0, 14500.0); }

#ifdef CV2_LAYER_BAKE
// ── The columns: each layer's vertical optical depth at one point, with its first two height moments ───────────────
// Hand-kept copies of cv2MidSigma / cv2HighSigma (clouds_v2.glsl: keep in step) with the 2D terms evaluated ONCE per
// column and the 3D ones at kLayK stratified heights through the layer's own [base, base + thick]. The flow and the
// ground are set once (gCv2FlowSet / gCv2GroundSet) by the caller. The march's two suppressions are reproduced: the mid
// layer gives way where Cb towers can stand (cv2ColumnSigma's storm strength), cirrus where anvils' storms are.
const int kLayK = 6;
struct CV2LayCol { float tau, th, th2; };   // tau, tau x h, tau x h^2 (h km from the reference height)

CV2LayCol cv2MidColumn(vec3 dirENU, float eyeH, mat3 enuToEcef, float jz, float fpM)
{
    CV2LayCol o; o.tau = 0.0; o.th = 0.0; o.th2 = 0.0;
    vec3  eyeP = vec3(0.0, 0.0, kCv2LayR + eyeH);
    CV2Pos q = cv2Pos(dirENU * (kCv2LayR + 5000.0) - eyeP, eyeH, enuToEcef);
    float gl  = cv2Ground(q.dirE) * 0.8;
    vec3  wF  = cv2FlowWeatherDirAt(q);
    vec4  w   = textureLod(cv2WeatherTex, wF, 1.0);
    float cov = clamp((w.r * cv2.look.x - cv2.cover.x) / max(cv2.cover.y - cv2.cover.x, 1e-3), 0.0, 1.0);
    if (cov <= 0.0 && cv2.flow.y <= 0.0) return o;
    vec4  cl  = textureLod(cv2MesoTex, cv2.anchorCluster.xyz + cv2Drift(q.seaProjE) * cv2.anchorCluster.w,
                           cv2Lod(fpM, cv2.anchorCluster.w));
    float covW   = clamp((textureLod(cv2WeatherTex, wF, 4.0).r * cv2.look.x - cv2.cover.x)
                         / max(cv2.cover.y - cv2.cover.x, 1e-3), 0.0, 1.0);
    float covM   = mix(cov, max(cov, covW * 0.85), cv2.flow.y);
    float clA    = cl.a, uMid = -1.0;
    if (cv2.morph.x > 0.0) {
        vec3  wdM = cv2Drift(q.dirE), aw = abs(wdM);
        vec3  aM  = cv2.farTune2.yzw + cv2Drift(q.seaProjE) * (cv2.anchorMorph.w * 1.7);
        vec2  uvM = (aw.x >= aw.y && aw.x >= aw.z) ? aM.yz : (aw.y >= aw.z ? aM.xz + vec2(0.37) : aM.xy + vec2(0.71));
        float uM  = textureLod(cv2MorphTex, uvM, cv2Lod(fpM, cv2.anchorMorph.w * 1.7) + 3.0).r;
        clA = mix(cl.a, 0.5 + 0.057 * 2.6 * (uM - 0.5) * 1.2, cv2.morph.x * 0.75);
        uMid = uM;
    }
    if (cv2.mid3.x > 0.0) {
        vec3 rgm = wF * (R_EARTH * cv2.mid3.x);
        clA = 0.5 + (textureLod(cv2MesoTex, rgm + vec3(0.47, 0.19, 0.83), cv2Lod(fpM, cv2.mid3.x)).a - 0.5) * 0.85
            + (clA - 0.5) * 0.4;
    }
    float regime = smoothstep(0.5 - cv2.mid2.w, 0.68 - cv2.mid2.w, clA + 0.15 * (covM - 0.5)) * cv2.misc.w * smoothstep(0.0, 0.35, covM)
                 * mix(1.0, 0.55, cv2.flow.y * smoothstep(0.75, 1.0, cov));
    if (regime <= 0.0) return o;
    // cv2Field: the mid layer gives way where towers can stand (the column search's storm strength, mip 3).
    float keep = 1.0;
    if (cv2.column.x > 0.0) {
        vec4 wk = cv2WeatherBilinear(wF, 3.0);
        if (wk.g >= 0.55) {
            float ck = clamp((wk.r * cv2.look.x - cv2.cover.x) / max(cv2.cover.y - cv2.cover.x, 1e-3), 0.0, 1.0);
            keep = 1.0 - smoothstep(0.0, 0.15, smoothstep(0.6, 0.8, wk.g) * smoothstep(0.15, 0.45, ck));
        }
    }
    if (keep <= 0.0) return o;
    float cr    = clamp(0.5 + (cl.r - 0.5) * 4.4, 0.0, 1.0);
    float base  = 3000.0 + gl + 2600.0 * cr;
    float thick = mix(1200.0, 450.0, cr) * cv2.mid2.y;
    float strat = 1.0 - smoothstep(0.3, 0.55, w.g);
    float haze  = regime * 0.45;
    if (uMid >= 0.0)
        haze = mix(haze, 0.55 * smoothstep(0.88 - regime, 1.12 - regime, uMid), cv2.morph.x);
    float dz = thick / float(kLayK);
    [[dont_unroll]] for (int k = 0; k < kLayK; ++k) {
        float zm = (float(k) + jz) / float(kLayK);
        float h  = base + thick * zm;
        CV2Pos qk = cv2Pos(dirENU * (kCv2LayR + h) - eyeP, eyeH, enuToEcef);
        vec4  sh  = textureLod(cv2ShapeTex, cv2.anchorMid.xyz + cv2Drift(qk.rSeaE) * cv2.anchorMid.w,
                               cv2Lod(fpM, cv2.anchorMid.w));
        float lens = smoothstep(0.0, 0.3, zm + (sh.a - 0.5) * 0.4) * (1.0 - smoothstep(0.55, 1.0, zm + (sh.b - 0.45) * 0.5));
        float ac   = clamp((sh.b * 0.7 + sh.g * 0.3 - (1.0 - regime * 0.8)) * 5.0, 0.0, 1.0);
        ac         = mix(ac, haze, smoothstep(400.0, 3000.0, fpM));
        float as_  = clamp(regime * 1.3, 0.0, 1.0) * mix(0.7, 1.0, sh.a);
        float s    = lens * mix(ac * 0.035, as_ * 0.004 * cv2.mid2.x, strat) * cv2.atmo.z * keep;
        float hk   = (h - cv2.layRef.x) * 0.001;
        o.tau += s * dz;
        o.th  += s * dz * hk;
        o.th2 += s * dz * hk * hk;
    }
    return o;
}

CV2LayCol cv2HighColumn(vec3 dirENU, float eyeH, mat3 enuToEcef, float jz, float fpM)
{
    CV2LayCol o; o.tau = 0.0; o.th = 0.0; o.th2 = 0.0;
    if (cv2.high.x <= 0.0) return o;
    vec3  eyeP = vec3(0.0, 0.0, kCv2LayR + eyeH);
    CV2Pos q = cv2Pos(dirENU * (kCv2LayR + 9000.0) - eyeP, eyeH, enuToEcef);
    vec3  wd     = cv2Drift(q.dirE);
    float tropTop = cv2.types[4].alt.x + (cv2.types[4].alt.y - cv2.types[4].alt.x) * cv2Tropo(wd);
    float base0  = 0.74 * tropTop;
    vec3  flowL  = cv2FlowDisp(wd, vec3(0.0));
    vec3  wF     = cv2FlowWeatherDir(wd, flowL);
    vec3  wdH    = normalize(wd + flowL * (cv2.high2.z / R_EARTH));
    vec4  wc     = textureLod(cv2WeatherTex, wF, 2.0);
    vec4  wb     = textureLod(cv2WeatherTex, wF, 4.0);
    float span   = max(cv2.cover.y - cv2.cover.x, 1e-3);
    float covL   = clamp((wc.r * cv2.look.x - cv2.cover.x) / span, 0.0, 1.0);
    float covS   = clamp((wb.r * cv2.look.x - cv2.cover.x) / span, 0.0, 1.0);
    float sys    = max(covS, 0.7 * covL);
    vec4  cl     = textureLod(cv2MesoTex, cv2.anchorCluster.xyz + cv2Drift(q.seaProjE) * cv2.anchorCluster.w
                              + vec3(0.41, 0.17, 0.69), cv2Lod(fpM, cv2.anchorCluster.w));
    float widen  = 0.06 * max(cv2.high.x - 1.0, 0.0);
    vec3  rgc    = wdH * (R_EARTH * cv2.high2.y);
    float rgN    = (textureLod(cv2MesoTex, rgc + vec3(0.23, 0.71, 0.37), 0.0).a - 0.5) * 0.75
                 + (textureLod(cv2MesoTex, rgc * 3.1 + vec3(0.61, 0.13, 0.89), 0.0).a - 0.5) * 0.45
                 + (cl.a - 0.5) * 0.2;
    float regime = smoothstep(0.51 - widen, 0.57 - widen, 0.5 + rgN + 0.12 * (1.0 - 0.5 * cv2.flow.y) * (sys - 0.45))
                 * min(cv2.high.x, 1.0);
    if (regime <= 0.0) return o;
    // cv2Field: the cirrus gives way to the anvils over storms (their one ~20 km read).
    float keep = 1.0;
    if (cv2.storm.z > 0.0) {
        vec4  wa = textureLod(cv2WeatherTex, wF, 2.0);
        float ca = clamp((wa.r * cv2.look.x - cv2.cover.x) / span, 0.0, 1.0);
        keep = 1.0 - smoothstep(0.65, 0.85, wa.g) * smoothstep(0.25, 0.6, ca);
    }
    if (keep <= 0.0) return o;
    float zb     = base0 + (cl.r - 0.5) * 1200.0;
    float thick  = mix(500.0, 1600.0, regime);
    float lon = atan(wdH.y, wdH.x), lat = asin(clamp(wdH.z, -1.0, 1.0));
    float latW  = lat * R_EARTH;
    if (cv2.tune.w > 0.0)
        latW += cv2.tune.w * 0.45 * R_EARTH * (sin(7.0 * lon + 3.0 * lat + 1.3) / 7.0 + sin(13.0 * lon - 5.0 * lat + 4.1) / 13.0
                                               + sin(23.0 * lon + 8.0 * lat + 2.2) / 23.0 + sin(41.0 * lon - 13.0 * lat + 5.7) / 41.0
                                               + sin(67.0 * lon + 29.0 * lat + 0.4) / 67.0);
    float strat = 1.0 - smoothstep(0.3, 0.55, wc.g);
    float ccK   = smoothstep(0.5, 0.7, cl.g) * (1.0 - strat) * 0.8;
    float lod   = cv2Lod(fpM, cv2.high.w);
    float far   = smoothstep(1.5, 4.0, lod);
    float lo    = mix(0.06, -0.02, regime);
    float dz    = thick / float(kLayK);
    [[dont_unroll]] for (int k = 0; k < kLayK; ++k) {
        float z  = (float(k) + jz) / float(kLayK);
        float h  = zb + thick * z;
        float shear = (zb + thick - h) * 2.0;
        vec3  cc  = vec3((lon * R_EARTH - shear) * cv2.high.y + cv2.high.z,
                         latW * cv2.high.w + (cl.a - 0.5) * 12.0,
                         h * cv2.high.w);
        vec4  sb     = textureLod(cv2ShapeTex, cc * 0.25 + vec3(0.13, 0.47, 0.29), max(lod - 2.0, 0.0));
        float bundle = smoothstep(0.43, 0.6, sb.a);
        bundle *= mix(1.0, smoothstep(-1.2, 0.6, (cl.a - 0.5) / 0.057), min(cv2.tune.w * 2.0, 1.0));
        cc.y += (sb.g - 0.45) * 1.2;
        vec4  s   = textureLod(cv2ShapeTex, cc, lod);
        vec4  s2  = textureLod(cv2ShapeTex, cc * 3.0 + vec3(0.31, 0.73, 0.17), lod + 1.585);
        float fibN = (s.a - 0.5) * 0.6 + (s2.a - 0.5) * 0.4;
        float ci   = smoothstep(lo, lo + 0.07, fibN) * mix(0.2, 1.0, bundle);
        vec4  s3   = textureLod(cv2ShapeTex, cc * 9.0 + vec3(0.57, 0.11, 0.83), lod + 3.17);
        ci *= mix(1.0, mix(0.3, 1.4, clamp((s3.a - 0.5) / 0.12 + 0.5, 0.0, 1.0)), 1.0 - smoothstep(1.0, 3.0, lod));
        float cs   = regime * mix(0.6, 1.0, clamp(fibN / 0.08 + 0.5, 0.0, 1.0)) * mix(0.6, 1.0, bundle);
        float cu   = clamp((s2.g - mix(0.75, 0.55, regime)) * 5.0, 0.0, 1.0);
        ci = mix(ci, regime * bundle * bundle * 0.45, far);
        cs *= mix(1.0, mix(0.35, 1.0, bundle), far);
        cu = mix(cu, regime * 0.25 * bundle, far);
        float prof = smoothstep(0.0, 0.25, z) * (1.0 - smoothstep(0.55, 1.0, z));
        float d    = prof * mix(mix(ci, cs, strat), cu, ccK);
        float sg   = d * mix(mix(0.0008, 0.0003, strat), 0.0016, ccK) * cv2.high2.x * mix(1.0, 0.6, far) * keep;
        float hk   = (h - cv2.layRef.y) * 0.001;
        o.tau += sg * dz;
        o.th  += sg * dz * hk;
        o.th2 += sg * dz * hk * hk;
    }
    return o;
}
#endif

#ifdef CV2_LAYER_LOOKUP
// The Sun's transmittance through the mid and high layers BEYOND a point: relEnd the point (from the eye, observer ENU;
// where the sample's light march ends), hEnd its height above sea level, sunDir the Sun (ENU), highK how much of the
// high layer's own shadow a point inside it takes ("Cirrus self-shadow"; 1 below it). One call site in the march.
float cv2LayerShadowT(vec3 relEnd, float hEnd, vec3 sunDir, float eyeH, float highK)
{
    if (cv2.layN.w <= 0.0) return 1.0;
    vec3  P   = relEnd + vec3(0.0, 0.0, kCv2LayR + eyeH);
    float b   = dot(P, sunDir);
    float lnT = 0.0;
    [[dont_unroll]] for (int L = 0; L < 2; ++L) {
        vec2  band = cv2LayBand(L);
        float href = cv2LayRefH(L);
        if (hEnd >= band.y) continue;
        // The ray's crossing of the reference sphere on the Sun's side of its lowest point (the slant cells' rays).
        float c    = (hEnd - href) * (2.0 * kCv2LayR + hEnd + href);
        float disc = b * b - c;
        if (disc < 0.0) continue;
        float sq   = sqrt(disc);
        float t    = (b > 0.0) ? -c / (b + sq) : sq - b;
        vec3  X    = P + sunDir * t;
        // Level 0 (1-km cells) where it reaches and is baked, level 1 (6 km) beyond and in its place where it is not.
        // vec4 per level: T, band bottom, band top (weighted), the valid share of its four cells.
        vec4  acc = vec4(0.0);
        float wTot = 0.0;
        [[dont_unroll]] for (int lv = 0; lv < 2; ++lv) {
            vec2  g  = cv2LayCoord(X, lv) - 0.5;
            vec2  ge = abs(g + 0.5 - cv2LayEyeC(lv));
            float wl = 1.0 - smoothstep(lv == 0 ? 100.0 : 110.0, lv == 0 ? 120.0 : 124.0, max(ge.x, ge.y));
            if (wl <= 0.0) continue;
            ivec2 g0 = ivec2(floor(g));
            vec2  gf = g - vec2(g0);
            vec4  a  = vec4(0.0);
            [[dont_unroll]] for (int k = 0; k < 4; ++k) {
                ivec2 cc = g0 + ivec2(k & 1, k >> 1);
                uvec4 e  = cv2LaySlant[lv * kCv2LayCells + cv2LaySlot(cc)];
                if (e.w != cv2LayStamp(cc)) continue;
                float bw = ((k & 1) != 0 ? gf.x : 1.0 - gf.x) * ((k >> 1) != 0 ? gf.y : 1.0 - gf.y);
                vec2  tt = unpackHalf2x16(e.x);
                vec2  bd = unpackHalf2x16(L == 0 ? e.y : e.z);
                a += vec4(L == 0 ? tt.x : tt.y, bd, 1.0) * bw;
            }
            // The near level takes its valid share of what is left; the far level the rest.
            float wv = (1.0 - wTot) * (lv == 0 ? wl * a.w : wl * step(1e-4, a.w));
            acc  += vec4(a.xyz / max(a.w, 1e-4), 0.0) * wv;
            wTot += wv;
        }
        if (wTot <= 1e-4) continue;
        float Tl = acc.x / wTot, lo = acc.y / wTot, hi = acc.z / wTot;
        float wSum = wTot;
        // The share of the layer's slant path above the point: 1 below the layer, 0 above it (the light march has seen
        // what is between). Its band is the slant cell's (where the ray crosses the reference sphere).
        float share = clamp((hi - (hEnd - href) * 0.001) / max(hi - lo, 0.05), 0.0, 1.0);
        if (L == 1) share *= (hEnd > href + lo * 1000.0) ? highK : 1.0;
        lnT += log(max(Tl, 1e-4)) * share * min(wSum, 1.0);
    }
    return mix(1.0, exp(lnT), cv2.layN.w);
}
#endif
#endif
