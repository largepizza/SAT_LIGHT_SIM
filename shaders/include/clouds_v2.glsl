#ifndef SATLIGHTSIM_CLOUDS_V2_GLSL
#define SATLIGHTSIM_CLOUDS_V2_GLSL

// ── Clouds v2: THE cloud field (.plans/CLOUDS_V2_PLAN.md) ────────────────────────────────────────
// One density function for every consumer: the view march, its light march, the ground shadow and
// the beam occlusion march. Anything that needs "is there cloud here" calls cv2Field() — there are
// no hand-kept copies to drift apart (v1 had one in cloud_march.comp and one in beam_self_march.comp).
//
// Needs common.glsl. Define before including (CV2_PARAMS_ONLY: just the UBO block):
//   CV2_PARAMS_BINDING, CV2_WEATHER_BINDING, CV2_SHAPE_BINDING, CV2_MESO_BINDING
//   CV2_DETAIL_BINDING   optional; without it the field has no erosion detail (the cheap form, for
//                        consumers that only need optical depth at a coarse scale)
//
// Coverage is built in three scales:
//   weather cube (5 km texels, the real map, classified into a cloud TYPE)  x
//   mesoscale fields on the sea-level sphere (cumulus cells ~2 km, closed cells ~30 km, clusters)  x
//   a real 3D shape noise, eroded by a 3D detail noise near the surface.
// Tops rise with local presence, so a cell becomes a dome and a dense cell a tower.
//
// COORDINATES. Everything is metric and relative to the observer's SEA-LEVEL point; no absolute
// ECEF position is ever formed in float. The CPU gives each noise volume an anchor, frac(the sea
// point / period) in double, plus the wind offset; a sample adds its small relative offset. Altitude
// and the sea-level projection are formed without cancellation (see cv2Pos). This is the terrain
// detail's scheme, and the reason nothing swims or pixelates far from the origin.

const int CV2_NUM_TYPES = 5;   // stratus, stratocumulus, cumulus, congestus, cumulonimbus

struct CV2Type {
    vec4 alt;    // x base altitude (m); y top at full presence (m, at tropopause scale 1);
                 // z top as a fraction of the span at zero presence (1 = flat-topped deck);
                 // w base-height variation (m)
    vec4 shape;  // x extinction at density 1 (1/m); y erosion strength; z billow (0 wispy .. 1
                 // cauliflower); w anvil (spread of the top beyond the core)
    vec4 look;   // x multiple-scattering brightness; y ambient multiplier; z convective (0 = the
                 // stratiform field, 1 = the cell field); w precipitation loading of the base
};

// std140; mirrored by GpuCloudV2Params in SatelliteSim.h (offsetof static_asserts there).
layout(std140, set = 0, binding = CV2_PARAMS_BINDING) uniform CloudV2Params {
    mat4  prevSkyView;    // previous frame's ENU -> camera rotation
    vec4  prevObs;        // xyz previous observer ECEF unit vector, w previous tan(fovY/2)
    vec4  obsDelta;       // xyz eye(now) - eye(prev), ECEF metres (double on the CPU); w previous aspect
    vec4  anchorShape;    // xyz = frac(anchor / period) in [0,1), w = 1 / period (1/m)
    vec4  anchorDetail;
    vec4  anchorCluster;  // the mesoscale anchors are sampled on the sea-level sphere
    vec4  anchorCell;
    vec4  frame;          // x frame index, y history valid (0/1), zw this frame's pixel in each 2x2
    vec4  march;          // x base step (m), y step growth per metre of distance, z max step (m),
                          // w max distance (m)
    vec4  light;          // x light-march length (m), y light steps, z MS extinction ratio per
                          // octave, w MS contribution ratio per octave
    vec4  look;           // x coverage scale, y density scale, z detail strength, w ambient gain
    vec4  look2;          // x ground-bounce gain, y sun gain, z powder, w new-sample weight (resolve)
    vec4  phase;          // x/y forward lobe g/weight, z/w backward lobe g/weight
    vec4  shell;          // x lowest base (m), y highest top (m), z debug view, w distance LOD start (m)
    vec4  extra;          // x edge sharpness (density gain after erosion, clamped at 1),
                          // y weather-lookup warp (rad), zw unused
    CV2Type types[CV2_NUM_TYPES];
} cv2;

#ifndef CV2_PARAMS_ONLY   // the resolve pass needs only the UBO
layout(set = 0, binding = CV2_WEATHER_BINDING) uniform samplerCube cv2WeatherTex;
layout(set = 0, binding = CV2_SHAPE_BINDING)   uniform sampler3D   cv2ShapeTex;
layout(set = 0, binding = CV2_MESO_BINDING)    uniform sampler3D   cv2MesoTex;
#ifdef CV2_DETAIL_BINDING
layout(set = 0, binding = CV2_DETAIL_BINDING)  uniform sampler3D   cv2DetailTex;
#endif

// A point, in the forms the field needs.
struct CV2Pos {
    vec3  rSeaE;     // offset from the observer's sea-level point, ECEF axes (m)
    vec3  seaProjE;  // its radial projection onto the sea-level sphere, same origin/axes (m)
    vec3  dirE;      // unit ECEF direction from the Earth's centre (weather lookup)
    float h;         // altitude above sea level (m)
};

// relENU: the point relative to the EYE, ENU axes. eyeH: the eye's altitude above sea level.
CV2Pos cv2Pos(vec3 relENU, float eyeH, mat3 enuToEcef)
{
    vec3  s      = vec3(relENU.xy, relENU.z + eyeH);          // relative to the sea-level point
    float horiz2 = dot(s.xy, s.xy);
    float rz     = R_EARTH + s.z;
    float pl     = sqrt(horiz2 + rz * rz);
    CV2Pos o;
    // |p|^2 - R^2 = horiz2 + s.z (2R + s.z): no cancellation, unlike length(p) - R.
    o.h        = (horiz2 + s.z * (2.0 * R_EARTH + s.z)) / (pl + R_EARTH);
    // (R + s.z) R/|p| - R = -R horiz2 / (|p| (R + s.z + |p|)), again cancellation-free.
    vec3 proj  = vec3(s.xy * (R_EARTH / pl), -R_EARTH * horiz2 / (pl * (rz + pl)));
    o.rSeaE    = enuToEcef * s;
    o.seaProjE = enuToEcef * proj;
    o.dirE     = enuToEcef * (vec3(s.xy, rz) / pl);
    return o;
}

CV2Type cv2TypeAt(float t)
{
    float fi = clamp(t, 0.0, 1.0) * float(CV2_NUM_TYPES - 1);
    int   i0 = min(int(fi), CV2_NUM_TYPES - 2);
    float f  = fi - float(i0);
    CV2Type r;
    r.alt   = mix(cv2.types[i0].alt,   cv2.types[i0 + 1].alt,   f);
    r.shape = mix(cv2.types[i0].shape, cv2.types[i0 + 1].shape, f);
    r.look  = mix(cv2.types[i0].look,  cv2.types[i0 + 1].look,  f);
    return r;
}

struct CV2Field {
    float sigma;     // extinction (1/m)
    float hf;        // height fraction within this column's cloud (0 base .. 1 top)
    float msBright;  // the type's multiple-scattering brightness
    float ambient;   // the type's ambient multiplier
};

// Mip level of a noise volume for a footprint of fpM metres (128 texels per period, w = 1/period).
float cv2Lod(float fpM, float invPeriod) { return max(0.0, log2(max(fpM * invPeriod * 128.0, 1e-6))); }

// detailAmt: > 0 = erosion from the detail volume at that strength (faded with distance by the
// caller); 0 = the MEAN erosion, no fetch — for lighting, shadows and beams, whose optical depth
// must match the eroded clouds on average (v1's cone learned this: an uneroded field is far denser
// than what is drawn, and shadowed everything); < 0 = no erosion at all, the upper bound the
// coarse march steps on (conservative: erosion only removes).
// fpM: the footprint (m) the sample stands for — the pixel's width at this distance, or a light
// step's length. The noise volumes are mip-mapped and read at that footprint, so a field that is
// finer than a pixel averages instead of aliasing (from orbit a 2 km cumulus cell is one pixel).
CV2Field cv2Field(CV2Pos q, float detailAmt, float fpM)
{
    CV2Field f;
    f.sigma = 0.0; f.hf = 0.0; f.msBright = 1.0; f.ambient = 1.0;
    if (q.h < cv2.shell.x || q.h > cv2.shell.y) return f;

    // Early out on a coarse, unwarped read (conservative: its texels span the warp below).
    if (textureLod(cv2WeatherTex, q.dirE, 2.0).r * cv2.look.x < 0.01) return f;

    vec4 cl = textureLod(cv2MesoTex, cv2.anchorCluster.xyz + q.seaProjE * cv2.anchorCluster.w,
                         cv2Lod(fpM, cv2.anchorCluster.w));
    vec4 ce = textureLod(cv2MesoTex, cv2.anchorCell.xyz + q.seaProjE * cv2.anchorCell.w,
                         cv2Lod(fpM, cv2.anchorCell.w));

    // The weather map's texels are 5 km: thresholding them bilinearly draws their squares, which
    // read as a grid from altitude. Warping the lookup by a few km of mesoscale noise hides it.
    vec3  warp = (vec3(ce.a, cl.a, ce.b) - vec3(0.5, 0.5, 0.6)) * cv2.extra.y;
    vec4  w    = textureLod(cv2WeatherTex, q.dirE + warp, 0.0);
    float cov  = clamp(w.r * cv2.look.x, 0.0, 1.0);
    if (cov < 0.02) return f;

    CV2Type ty     = cv2TypeAt(w.g);
    float   tropo  = w.a + 0.5;
    float   topMax = ty.alt.x + (ty.alt.y - ty.alt.x) * tropo;
    if (q.h < ty.alt.x - ty.alt.w || q.h > topMax) return f;

    // Where cloud may be, in 2D: a field thresholded by the coverage. Convective types use the
    // cumulus cells (clustered); stratiform types a high baseline broken along closed-cell rims.
    float fieldConv  = sqrt(ce.g) * mix(0.55, 1.05, cl.r) + (ce.a - 0.5) * 0.3;
    // Closed-cell rims (cl.b) fade out with the footprint: from orbit they read as cracked ice.
    float rimAmt     = 0.25 * (1.0 - smoothstep(0.5, 2.5, cv2Lod(fpM, cv2.anchorCell.w)));
    float fieldStrat = 0.5 + rimAmt * (cl.b - 0.5) + 0.125 + 0.3 * (ce.a - 0.5) + 0.2 * ce.g;
    float field      = mix(fieldStrat, fieldConv, ty.look.z);
    float P          = clamp((field - (1.0 - cov)) / max(cov, 0.05), 0.0, 1.0);
    // Sub-pixel cells: thresholding the mip-averaged field would erase them (the average sits
    // below the threshold), so where a cell is smaller than the footprint the presence becomes the
    // fraction of area covered — a haze of the right opacity instead of nothing, or of aliasing dots.
    float subPix = smoothstep(1.5, 3.5, cv2Lod(fpM, cv2.anchorCell.w));
    P = mix(P, cov * 0.75, subPix * ty.look.z);

    float base  = ty.alt.x + (cl.a - 0.5) * 2.0 * ty.alt.w;
    float hfMax = (q.h - base) / max(topMax - base, 1.0);
    // Anvil: near the tropopause a cumulonimbus spreads beyond its core, over the cluster.
    float anvil = ty.shape.w * smoothstep(0.76, 0.88, hfMax) * (1.0 - smoothstep(0.95, 1.0, hfMax))
                * smoothstep(0.45, 0.85, cl.r * cov);
    if (P <= 0.0 && anvil <= 0.0) return f;

    // Tops rise with presence: a cell is a dome, a dense cell a tower.
    float top  = base + (topMax - base) * mix(ty.alt.z, 1.0, P);
    float hf   = (q.h - base) / max(top - base, 1.0);
    float prof = (hf >= 0.0 && hf <= 1.0)
               ? smoothstep(0.0, 120.0, q.h - base) * (1.0 - smoothstep(0.35, 1.0, hf)) : 0.0;
    float Ph   = max(P * prof, anvil);
    if (Ph <= 0.0) return f;

    vec4  s  = textureLod(cv2ShapeTex, cv2.anchorShape.xyz + q.rSeaE * cv2.anchorShape.w,
                          cv2Lod(fpM, cv2.anchorShape.w));
    float bs = s.r * 0.65 + s.g * 0.35;
    float d  = clamp((bs - (1.0 - Ph)) / max(Ph, 1e-3), 0.0, 1.0);   // remap(bs, 1 - Ph, 1, 0, 1)
    if (d <= 0.0) return f;

    if (detailAmt >= 0.0) {
        float df = 0.45;   // the detail's mean (inverted-Worley fBm), when it is not fetched
        float amt = 1.0;
#ifdef CV2_DETAIL_BINDING
        if (detailAmt > 0.0) {
            vec3  dc = cv2.anchorDetail.xyz + q.rSeaE * cv2.anchorDetail.w
                     + (vec3(s.a, s.g, s.b) - vec3(0.5, 0.45, 0.45)) * 0.35;
            vec4  dn = textureLod(cv2DetailTex, dc, cv2Lod(fpM, cv2.anchorDetail.w));
            df  = dn.r * 0.55 + dn.g * 0.45;   // inverted Worley: high in the lumps
            amt = detailAmt;
        }
#endif
        // Base: bite the lumps (torn, wispy). Higher up: bite the cracks, keep the lumps (billows).
        float billow = ty.shape.z * smoothstep(0.05, 0.4, clamp(hf, 0.0, 1.0));
        float erode  = mix(df, 1.0 - df, billow) * ty.shape.y * cv2.look.z * amt;
        d = clamp((d - erode) / max(1.0 - erode, 1e-3), 0.0, 1.0);
        if (d <= 0.0) return f;
    }

    // Edge sharpness (the reference's noiseEdgeHardness): real cumulus surfaces are abrupt, but a
    // remapped noise ramps in over hundreds of metres and reads as fog. Applied AFTER erosion, so
    // the eroded lumps are what gets sharpened, and clamped so the interior saturates.
    // Relaxed toward 1 as the footprint grows: a surface a pixel cannot resolve should not be a
    // hard cut-out edge (orbit views read as paper cut-outs otherwise).
    d = min(d * mix(cv2.extra.x, 1.0, smoothstep(150.0, 1500.0, fpM)), 1.0);

    // Denser upward (cloud water grows with height); precipitation loads the base.
    d *= mix(0.6, 1.0, smoothstep(0.0, 0.4, hf)) * (1.0 + w.b * ty.look.w * (1.0 - clamp(hf, 0.0, 1.0)));
    f.sigma    = d * ty.shape.x * cv2.look.y;
    f.hf       = clamp(hf, 0.0, 1.0);
    f.msBright = ty.look.x;
    f.ambient  = ty.look.y;
    return f;
}

// Henyey-Greenstein normalised so the isotropic value is 1 (this codebase's phase convention —
// phaseR/phaseM/phaseCloud in common.glsl average to 1, so radiance = SUN_INTENSITY x phase).
float cv2HG(float cosA, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cosA, 1e-4), 1.5);
}

// The ray's in-shell intervals: [lowest base, highest top] minus whatever lies inside the inner
// sphere. No below/inside/above special cases — the observer's position only changes which roots
// are negative. Returns the number of segments (0..2).
int cv2ShellSegments(vec3 eye, vec3 dir, out vec2 seg0, out vec2 seg1)
{
    seg0 = vec2(0.0); seg1 = vec2(0.0);
    vec2 tO = raySphere(eye, dir, R_EARTH + cv2.shell.y);
    if (tO.y <= 0.0 || tO.x > tO.y) return 0;
    float a0 = max(tO.x, 0.0), a1 = tO.y;
    vec2 tI = raySphere(eye, dir, R_EARTH + cv2.shell.x);
    if (tI.y > 0.0 && tI.x < tI.y) {
        int n = 0;
        if (tI.x > a0) { seg0 = vec2(a0, min(a1, tI.x)); n = 1; }
        if (tI.y < a1) {
            vec2 s = vec2(max(a0, tI.y), a1);
            if (n == 0) seg0 = s; else seg1 = s;
            ++n;
        }
        return n;
    }
    seg0 = vec2(a0, a1);
    return 1;
}

#endif // CV2_PARAMS_ONLY

#endif // SATLIGHTSIM_CLOUDS_V2_GLSL
