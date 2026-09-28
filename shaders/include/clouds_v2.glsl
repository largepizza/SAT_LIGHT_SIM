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
                 // z flatness: 0 = the column top is a dome over each cell, 1 = a flat deck;
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
                          // y weather-lookup warp (rad), zw cos/sin of the drift since last frame
    CV2Type types[CV2_NUM_TYPES];
    vec4  cover;          // x map value that is clear sky, y map value that is overcast,
                          // zw cos/sin of the map's longitude drift (v1's cloudPhase x driftMult)
    vec4  form;           // x wobble (3D noise on the column edge, fraction of the span),
                          // y lean (m of horizontal shift per m of height), z interior erosion
                          // kept (0..1), w column-edge softness (1/fraction of the span)
    vec4  misc;           // x march budget (iterations), y moon key-light gain, z full rate: every
                          // half-res pixel marched this frame (0/1), w mid-layer amount (Ac/As)
    vec4  storm;          // x storm feature scale (deep convection's lobes and erosion are this much
                          // larger), y erosion kept on deep convection, z anvil amount, w base roughness
    vec4  anchorStorm;    // the shape volume at period x storm scale
    vec4  anchorStormDetail; // the detail volume at period x storm scale
    vec4  motion;         // x how far the noise volumes slid against each other since last frame (m):
                          // the evolution the resolve cannot reproject; yzw unused
    vec4  high;           // the high layer: x amount, y 1/along-wind period (m^-1, divides the equator),
                          // z along-wind offset (periods, the jet), w 1/across-wind period (m^-1)
    vec4  high2;          // x high-layer density; yzw unused
    vec4  rain;           // x rain amount (0 = none), y optics strength (halos, sundogs, rainbows),
                          // z rain streaks at the eye, w unused
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

// An Earth-fixed vector turned into the DRIFTED frame the whole field is read in: the weather cube
// and the noise volumes alike, so the clouds move with the map as one body (and the resolve can
// reproject that motion exactly — cv2.extra.zw).
vec3 cv2Drift(vec3 v)
{
    return vec3(v.x * cv2.cover.z - v.y * cv2.cover.w, v.x * cv2.cover.w + v.y * cv2.cover.z, v.z);
}
vec3 cv2WeatherDir(vec3 d) { return cv2Drift(d); }

struct CV2Field {
    float sigma;     // extinction (1/m)
    float hf;        // height fraction within this column's cloud (0 base .. 1 top)
    float msBright;  // the type's multiple-scattering brightness
    float ambient;   // the type's ambient multiplier
    float deck;      // 0..1: a sheet wide enough that light at a grazing angle crosses its whole
                     // width (the march shadows it by the path to its top, not the light march alone)
    float topH;      // altitude of this column's top (m)
    float thin;      // share of the extinction that is the optically thin high layer (cheap lighting)
    float rain;      // share of the extinction that is rain (its phase carries the rainbows)
};

void cv2Add(inout CV2Field f, float s, float hf, float topH, float deck)
{
    if (f.sigma <= 0.0) { f.hf = hf; f.msBright = 1.0; f.ambient = 1.0; f.topH = topH; f.deck = deck; f.thin = 0.0; f.rain = 0.0; }
    else                { f.topH = max(f.topH, topH); f.deck = max(f.deck, deck); }
    f.sigma += s;
}

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
CV2Field cv2FieldLow(CV2Pos q, float detailAmt, float fpM)
{
    CV2Field f;
    f.sigma = 0.0; f.hf = 0.0; f.msBright = 1.0; f.ambient = 1.0; f.deck = 0.0; f.topH = 0.0; f.thin = 0.0;
    f.rain = 0.0;
    if (q.h < cv2.shell.x || q.h > cv2.shell.y) return f;

    // The map drifts in longitude exactly as v1's flat layer and the ambience driver read it
    // (cloudPhase x driftMult, rotated about the pole on the CPU into cover.zw).
    vec3  wd      = cv2Drift(q.dirE);
    float covSpan = max(cv2.cover.y - cv2.cover.x, 1e-3);

    // Early out on a coarse, unwarped read. Conservative: its 20 km texels span the warp below, and
    // their box average keeps at least a sixteenth of one cloudy 5 km texel.
    if (textureLod(cv2WeatherTex, wd, 2.0).r * cv2.look.x < cv2.cover.x * 0.25) return f;

    // The noise volumes are read in the same drifted frame.
    vec3 rS = cv2Drift(q.rSeaE);
    vec3 sP = cv2Drift(q.seaProjE);

    // Towers lean downwind with height: the mesoscale fields are read that far upwind (east is the
    // stand-in wind until the weather evolves, phase 5).
    vec3 eastW = vec3(-wd.y, wd.x, 0.0) * inversesqrt(max(dot(wd.xy, wd.xy), 1e-6));
    vec3 mp    = sP - eastW * (cv2.form.y * max(q.h - 800.0, 0.0));
    vec4 cl = textureLod(cv2MesoTex, cv2.anchorCluster.xyz + mp * cv2.anchorCluster.w,
                         cv2Lod(fpM, cv2.anchorCluster.w));
    vec4 ce = textureLod(cv2MesoTex, cv2.anchorCell.xyz + mp * cv2.anchorCell.w,
                         cv2Lod(fpM, cv2.anchorCell.w));

    // The weather map's texels are 5 km: thresholding them bilinearly draws their squares, which
    // read as a grid from altitude. Warping the lookup by a few km of mesoscale noise hides it.
    vec3  warp = (vec3(ce.a, cl.a, ce.b) - vec3(0.5, 0.5, 0.6)) * cv2.extra.y;
    vec4  w    = textureLod(cv2WeatherTex, wd + warp, 0.0);
    // Coverage is the map's brightness remapped: thin cloud is grey in the imagery, an overcast
    // deck is not pure white, and the raw value used as a fraction never let anywhere close over.
    float cov  = clamp((w.r * cv2.look.x - cv2.cover.x) / covSpan, 0.0, 1.0);
    if (cov <= 0.0) return f;

    CV2Type ty     = cv2TypeAt(w.g);
    float   strat  = 1.0 - ty.look.z;                  // 1 = the stratiform field, 0 = the cells
    float   tropo  = w.a + 0.5;
    float   topMax = ty.alt.x + (ty.alt.y - ty.alt.x) * tropo;
    // Nimbostratus: a stratiform deck where the map says it rains thickens by up to 3.5 km (its
    // base is already darkened by the precipitation loading below).
    topMax += w.b * strat * 3500.0;

    // A deck is not a slab: its thickness follows the closed cells and a km-scale Perlin field, and
    // a thicker part hangs lower (an overcast seen from below was one flat, featureless plane).
    // Closed-cell rims (cl.b) fade out with the footprint: from orbit they read as cracked ice.
    float rimAmt  = 0.25 * (1.0 - smoothstep(0.5, 2.5, cv2Lod(fpM, cv2.anchorCell.w)));
    float deckVar = clamp((ce.a - 0.5) * 3.0 + (cl.b - 0.5) * rimAmt * 4.0, -1.0, 1.0);
    float base    = ty.alt.x + (cl.a - 0.5) * 2.0 * ty.alt.w - strat * 150.0 * deckVar;
    // The base is bumped in 3D by the lobes below, by up to ~2.2 x baseAmp: a deck's underside is
    // lumpy (rolls, pouches), a cumulus base nearly flat (the condensation level).
    float baseAmp = mix(40.0, 220.0, strat) * cv2.storm.w;
    if (q.h > topMax) return f;
    // Below the (lowest possible) base: only rain, where the map says it precipitates.
    bool  below   = q.h <= base - 2.2 * baseAmp;
    // The bake's precipitation channel keys on the RAW map brightness (> 0.55-0.9), which few
    // storms reach: most towering fields came out dry. Deep convection at near-full (remapped)
    // coverage rains too.
    // Full overcast rains lightly whatever its type (a thick tropical deck read as stratiform).
    float rainAmt = max(w.b, smoothstep(0.75, 1.0, cov) * (0.3 + 0.7 * smoothstep(0.55, 0.9, w.g))) * cv2.rain.x;
    if (below && rainAmt < 0.02) return f;
    float z = max(q.h - base, 0.0) / max(topMax - base, 1.0);   // height through the type's full span

    // Where cloud may be, in 2D: a field thresholded by the coverage. Convective types use the
    // cumulus cells (clustered); stratiform types a high baseline broken along closed-cell rims.
    // The convective field has a floor, so at full coverage the gaps between cells close into a
    // low deck (cumulus merging into stratocumulus) instead of staying clear.
    // Deep types (congestus, Cb) read the cells two mips coarser as well: neighbouring cells merge
    // into one tower several km wide, with the fine cells as turrets on it. On the 1-2 km cells
    // alone a 7 km tower was a 3:1 spire.
    // No fine 2D noise term here: anything 2D at the km scale is extruded up the column's side
    // into vertical flutes. The 3D lobes below do that job.
    float deep       = smoothstep(0.45, 0.85, w.g);   // cumulus (0.5) ~0, congestus (0.75) ~0.8
    float cellsC     = textureLod(cv2MesoTex, cv2.anchorCell.xyz + mp * cv2.anchorCell.w,
                                  cv2Lod(fpM, cv2.anchorCell.w) + 2.0).g;
    float cells      = mix(ce.g, max(ce.g * 0.6, min(cellsC * 1.8, 1.0)), deep);
    float fieldConv  = 0.15 + 0.85 * sqrt(cells) * mix(0.6, 1.0, cl.r);
    float fieldStrat = 0.5 + rimAmt * (cl.b - 0.5) + 0.125 + 0.3 * (ce.a - 0.5) + 0.2 * ce.g;
    float field      = mix(fieldStrat, fieldConv, ty.look.z);
    // How far into the cloud this column is: 0 at the edge, 1 well inside. Not normalised by the
    // coverage, so sparse fair-weather cells stay small and low; only a dense field builds towers.
    float e          = clamp((field - (1.0 - cov)) / 0.55, 0.0, 1.0);

    // ── Rain shafts: under the column's cloud, heavier under convective cores than under a
    // nimbostratus deck. Curtains = the shape volume compressed along the vertical (streaks that
    // fall), slanted downwind with depth below the base. ~0.3-1.5 /km: a few km of visibility in a
    // heavy shower. Lit through the cloud above by the light march: dark under a Cb, bright at its
    // sunlit edge — which is where rainbows show (the march's rain phase). ──
    if (below) {
        float rate = rainAmt * smoothstep(0.3, 0.8, e) * mix(0.4, 1.0, ty.look.z);
        if (rate <= 0.0) return f;
        vec3  rr = rS + eastW * ((base - q.h) * 0.25);
        rr -= wd * (dot(rr, wd) * 0.85);
        vec4  sr = textureLod(cv2ShapeTex, cv2.anchorShape.xyz + rr * cv2.anchorShape.w,
                              cv2Lod(fpM, cv2.anchorShape.w));
        float shaft = clamp((sr.g * 0.6 + sr.a * 0.4 - 0.42) * 4.0 + 0.3, 0.0, 1.0);
        shaft = mix(shaft, 0.45, smoothstep(300.0, 2000.0, fpM));   // sub-pixel curtains: their mean
        f.sigma    = rate * shaft * 0.0012;
        f.msBright = 0.6;
        f.ambient  = 0.8;
        f.rain     = 1.0;
        f.topH     = base;
        return f;
    }

    // THE SHAPE IS A 3D MARGIN, not a heightfield. v1 and the first v2 cuts made cloud a column:
    // a 2D strength set a top, and the cloud filled everything below it down to one flat base. A
    // heightfield cannot bulge or undercut, so wherever its top rose steeply the side was a wall,
    // and the base ran flat to the edge: every cloud, down to the smallest puff, was a flat-bottomed
    // can with vertical sides and a fluffy top. Here cloud exists where
    //     m = e(x,y) + lobes(x,y,z) - g(z) - gBase(h) > 0
    //   e       how far inside the thresholded 2D field this column is (above)
    //   lobes   multi-scale inverted-Worley noise, in the SAME units as e, so it moves the side
    //           surface sideways as much as up: cauliflower lobes, overhangs, necks
    //   g(z)    the strength needed to reach height z of the type's span: small cells are low mounds,
    //           strong cores towers (convective); decks fill most of their (varying) thickness
    //           with a lumpy top
    //   gBase   extra strength needed just above the (bumped) base: flat under the core, curling up
    //           to the edge — no square corner
    // form.x scales the lobes, form.w is the softness of the surface (1 / its width in e units).
    float hb     = q.h - base;
    // (Deep towers were tried with straighter walls, 0.9 z^1.8: that extruded the 2D field into
    // vertical flutes again. All convection shares z^1.2; deep towers are wide via the coarse cells.)
    float gConv  = pow(z, 1.2);
    // A deck's top, in its span: thin parts glow from below, thick parts go dark (from below an
    // overcast is seen by the light it transmits, so its thickness IS its texture).
    float zTop   = clamp(0.75 + 0.3 * deckVar, 0.4, 1.0);
    float gStrat = 1.2 * smoothstep(0.3, 1.0, z / zTop);
    float g      = mix(gConv, gStrat, ty.alt.z);
    float fpFade = mix(1.0, 0.3, smoothstep(100.0, 800.0, fpM));   // sub-pixel lobes average away
    float lobeK  = cv2.form.x * mix(0.5, 1.0, ty.look.z) * fpFade;
    // Sub-pixel cells: thresholding the mip-averaged field would erase them (the average sits
    // below the threshold), so where a cell is smaller than the footprint the presence becomes the
    // fraction of area covered — a haze of the right opacity instead of nothing, or aliasing dots.
    // Starts at a 2x footprint (it began at 3x: from orbit the barely resolved cells read as salt).
    float subPix = smoothstep(1.0, 3.0, cv2Lod(fpM, cv2.anchorCell.w)) * ty.look.z;
    {
        // Before the shape fetch, bounded as if the base bumps lifted this point all the way (the
        // gBase and the lobe damping only fall with height): the lobes add at most ~0.6 lobeK.
        float hbMax = hb + 2.2 * baseAmp;
        float gB    = 0.3 * (1.0 - smoothstep(0.0, 350.0, hbMax));
        if (e - g - gB * gB / 0.3 + lobeK * 0.6 <= 0.0 && subPix <= 0.0) return f;
    }

    vec4  s  = textureLod(cv2ShapeTex, cv2.anchorShape.xyz + rS * cv2.anchorShape.w,
                          cv2Lod(fpM, cv2.anchorShape.w));
    // Deep convection's lobes are storm.x times larger: at the cumulus scale a 12 km storm was a
    // pile of small bubbles, a "mountain" rather than towers.
    if (deep > 0.02) {
        vec4 s2 = textureLod(cv2ShapeTex, cv2.anchorStorm.xyz + rS * cv2.anchorStorm.w,
                             cv2Lod(fpM, cv2.anchorStorm.w));
        s = mix(s, s2, deep);
    }
    // Inverted Worley at 4/8/16 cells (G: 1.75 km, 875 m, 440 m lobes for the 7 km period) and
    // 8/16/32 (B), with a little Perlin (A) so the lobes are not all spheres. ~zero mean.
    float lobes = (s.g * 0.6 + s.b * 0.25 + s.a * 0.15) - 0.47;
    float hbE   = hb + baseAmp * lobes * 4.0;          // height above the bumped base
    if (hbE <= 0.0 && subPix <= 0.0) return f;
    float gBase = 0.3 * (1.0 - smoothstep(0.0, 350.0, hbE));
    // Lobes are strong on convective types, gentler on decks, and damped near the base (cumulus
    // bases are flat: they are the condensation level, not a surface the turbulence shapes).
    float A     = lobeK * mix(0.3, 1.0, smoothstep(0.0, 500.0, hbE));
    float m     = e + A * lobes * 2.0 - g - gBase * gBase / 0.3;
    float Ph    = clamp(m * cv2.form.w, 0.0, 1.0) * smoothstep(0.0, 60.0, hbE);
    float PhSub = cov * 0.8 * clamp((mix(0.2 + 0.4 * cov, 1.0, ty.alt.z) - z) * cv2.form.w, 0.0, 1.0)
                * smoothstep(0.0, 150.0, hb);
    Ph = mix(Ph, PhSub, subPix);
    if (Ph <= 0.0) return f;
    // Height within THIS column (for the lighting terms and billowing): the column's own top.
    float Htop = mix(pow(e, 0.83), zTop * 0.85, ty.alt.z);
    float hf   = clamp(z / max(Htop, 0.05), 0.0, 1.0);

    // Inside, a little of the macro noise for density variation (the surface is the margin).
    float d = Ph * mix(0.75, 1.0, s.r);

    if (detailAmt >= 0.0) {
        float df = 0.45;   // the detail's mean (inverted-Worley fBm), when it is not fetched
        float amt = 1.0;
#ifdef CV2_DETAIL_BINDING
        if (detailAmt > 0.0) {
            vec3  dist = (vec3(s.a, s.g, s.b) - vec3(0.5, 0.45, 0.45)) * 0.35;
            vec4  dn   = textureLod(cv2DetailTex, cv2.anchorDetail.xyz + rS * cv2.anchorDetail.w + dist,
                                    cv2Lod(fpM, cv2.anchorDetail.w));
            if (deep > 0.02) {
                vec4 dn2 = textureLod(cv2DetailTex,
                                      cv2.anchorStormDetail.xyz + rS * cv2.anchorStormDetail.w + dist,
                                      cv2Lod(fpM, cv2.anchorStormDetail.w));
                dn = mix(dn, dn2, deep);
            }
            df  = dn.r * 0.55 + dn.g * 0.45;   // inverted Worley: high in the lumps
            amt = detailAmt;
        }
#endif
        // Base: bite the lumps (torn, wispy). Higher up: bite the cracks, keep the lumps (billows).
        // Deep convection keeps storm.y of it: its large towers read best with little erosion.
        float billow = ty.shape.z * smoothstep(0.05, 0.4, hf);
        float erode  = mix(df, 1.0 - df, billow) * ty.shape.y * cv2.look.z * amt * mix(1.0, cv2.storm.y, deep);
        // Erosion shapes the SURFACE. Deep inside (d high) only form.z of it applies: at full
        // strength it punched holes through the interior, and from inside a cloud the sky showed.
        erode *= mix(1.0, cv2.form.z, smoothstep(0.35, 0.85, d));
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
    d *= mix(0.6, 1.0, smoothstep(0.0, 0.4, hf)) * (1.0 + w.b * ty.look.w * (1.0 - hf));
    f.sigma    = d * ty.shape.x * cv2.look.y;
    f.hf       = hf;
    f.msBright = ty.look.x;
    f.ambient  = ty.look.y;
    f.deck     = strat * smoothstep(0.6, 1.0, e);
    // The deck's upper envelope (a lumpy top reaches ~zTop): the grazing-light shadow measures the
    // depth below it, and a mean top made every trough between the lumps black at sunrise.
    f.topH     = base + mix(Htop, zTop, ty.alt.z) * (topMax - base);
    return f;
}

// ── The mid-level layer: altocumulus / altostratus (2.8-7 km) ────────────────────────────────────
// A second, independent field above the low system, so clouds overlap in altitude and cover: the
// low field alone put every cloud on the same flat base. Present only where the map has cloud (the
// map's brightness is the column's total) and a mid-level "moisture regime" (the cluster field's
// Perlin channel, 8-64 km) says so. Altocumulus — rounded cloudlets from the shape volume's 3D
// Worley octaves (220-875 m) in a thin lens-shaped sheet — where the low cloud is broken;
// altostratus — a smooth translucent sheet the sun shows through — where it is stratiform.
float cv2MidSigma(CV2Pos q, float fpM, out float hfMid, out float topMid, out float deckMid)
{
    hfMid = 0.0; topMid = 0.0; deckMid = 0.0;
    if (q.h < 2800.0 || q.h > 7200.0) return 0.0;
    vec4  w   = textureLod(cv2WeatherTex, cv2WeatherDir(q.dirE), 1.0);   // mid decks are broad
    float cov = clamp((w.r * cv2.look.x - cv2.cover.x) / max(cv2.cover.y - cv2.cover.x, 1e-3), 0.0, 1.0);
    if (cov <= 0.0) return 0.0;
    vec4  cl  = textureLod(cv2MesoTex, cv2.anchorCluster.xyz + cv2Drift(q.seaProjE) * cv2.anchorCluster.w,
                           cv2Lod(fpM, cv2.anchorCluster.w));
    float regime = smoothstep(0.5, 0.68, cl.a + 0.15 * (cov - 0.5)) * cv2.misc.w;
    if (regime <= 0.0) return 0.0;
    float base  = 3000.0 + 2600.0 * cl.r;
    float thick = mix(1200.0, 450.0, cl.r);
    float zm    = (q.h - base) / thick;
    if (zm <= 0.0 || zm >= 1.0) return 0.0;
    float strat = 1.0 - smoothstep(0.3, 0.55, w.g);                 // stratiform below -> As
    vec4  sh    = textureLod(cv2ShapeTex, cv2.anchorShape.xyz + cv2Drift(q.rSeaE) * cv2.anchorShape.w,
                             cv2Lod(fpM, cv2.anchorShape.w));
    // A lens: thin at its edges and top and bottom, the cloudlets' own lobes shaping both faces.
    float lens  = smoothstep(0.0, 0.3, zm + (sh.a - 0.5) * 0.4) * (1.0 - smoothstep(0.55, 1.0, zm + (sh.b - 0.45) * 0.5));
    float ac    = clamp((sh.b * 0.7 + sh.g * 0.3 - (1.0 - regime * 0.8)) * 5.0, 0.0, 1.0);
    // Cloudlets far below a pixel become the haze of their coverage (as the low cells do).
    ac          = mix(ac, regime * 0.45, smoothstep(400.0, 3000.0, fpM));
    float as_   = clamp(regime * 1.3, 0.0, 1.0) * mix(0.7, 1.0, sh.a);
    hfMid       = zm;
    topMid      = base + thick;
    deckMid     = strat;
    return lens * mix(ac * 0.035, as_ * 0.004, strat) * cv2.look.y;
}

// ── The high layer: cirrus / cirrostratus / cirrocumulus (0.7-0.9 of the tropopause) ─────────────
// Ice cloud, optically thin (tau ~0.1-3). Present where a high-level moisture regime (the cluster
// field's Perlin read at an offset, so it is independent of the mid layer's) says so, a little more
// where the map is cloudy (the imagery includes thin cirrus as grey veils). Three forms:
//   cirrus        fibrous streaks: the shape volume read with its coordinate STRETCHED along the
//                 wind (east, the jet), meandered by the cluster Perlin, sheared with height so the
//                 falling ice trails (fall streaks)
//   cirrostratus  a smooth veil, where the map is stratiform (fronts: Cs rides ahead of As/Ns)
//   cirrocumulus  small rippled cloudlets, in patches of the cluster field
// The stretch needs coordinates that follow the wind everywhere, so the along-wind coordinate is
// the (drifted) LONGITUDE x R, with a period that divides the equator an integer number of times
// (high.y, no seam at the antimeridian); across it, latitude x R. Absolute angles in float resolve
// ~1 m here, plenty for km-scale fibres. Near the poles the streaks shorten (longitude converges).
float cv2HighSigma(CV2Pos q, float fpM, out float hfH, out float topH)
{
    hfH = 0.0; topH = 0.0;
    if (cv2.high.x <= 0.0 || q.h < 5500.0 || q.h > 14500.0) return 0.0;
    vec3  wd     = cv2Drift(q.dirE);
    vec4  wc     = textureLod(cv2WeatherTex, wd, 2.0);
    float tropTop = cv2.types[4].alt.x + (cv2.types[4].alt.y - cv2.types[4].alt.x) * (wc.a + 0.5);
    float base0  = 0.74 * tropTop;
    if (q.h < base0 - 700.0 || q.h > base0 + 2400.0) return 0.0;
    vec4  cl     = textureLod(cv2MesoTex, cv2.anchorCluster.xyz + cv2Drift(q.seaProjE) * cv2.anchorCluster.w
                              + vec3(0.41, 0.17, 0.69), cv2Lod(fpM, cv2.anchorCluster.w));
    float covRaw = clamp(wc.r * cv2.look.x, 0.0, 1.0);
    // The baked Perlin fBm is narrow: 0.50 +- 0.057 (1-99%: 0.37-0.63, measured by replicating the
    // bake). Thresholds are set against that spread; the first cut used 0.5-0.7 and drew no cirrus.
    // Above 1 the amount widens the regime instead (2 = cirrus over most of the sky).
    float widen  = 0.06 * max(cv2.high.x - 1.0, 0.0);
    float regime = smoothstep(0.51 - widen, 0.57 - widen, cl.a + 0.1 * (covRaw - 0.25)) * min(cv2.high.x, 1.0);
    if (regime <= 0.0) return 0.0;
    float zb     = base0 + (cl.r - 0.5) * 1200.0;
    float thick  = mix(500.0, 1600.0, regime);
    float z      = (q.h - zb) / thick;
    if (z <= 0.0 || z >= 1.0) return 0.0;

    float lon = atan(wd.y, wd.x), lat = asin(clamp(wd.z, -1.0, 1.0));
    // Fall streaks: the ice lower in the layer lags the generating heads above it.
    float shear = (zb + thick - q.h) * 2.0;
    vec3  cc  = vec3((lon * R_EARTH - shear) * cv2.high.y + cv2.high.z,
                     // meander: the cluster Perlin normalised by its spread (0.057), +-~0.7 period
                     lat * R_EARTH * cv2.high.w + (cl.a - 0.5) * 12.0,
                     q.h * cv2.high.w);
    float lod = cv2Lod(fpM, cv2.high.w);
    vec4  s   = textureLod(cv2ShapeTex, cc, lod);
    vec4  s2  = textureLod(cv2ShapeTex, cc * 3.0 + vec3(0.31, 0.73, 0.17), lod + 1.585);
    float strat = 1.0 - smoothstep(0.3, 0.55, wc.g);
    float ccK   = smoothstep(0.5, 0.7, cl.g) * (1.0 - strat) * 0.8;

    float fibN = (s.a - 0.5) * 0.6 + (s2.a - 0.5) * 0.4;      // ~0 +- 0.04
    float ci   = clamp((fibN - mix(0.07, -0.01, regime)) / 0.06, 0.0, 1.0);
    // Hair: a 9x finer read (still an integer multiple of the along-wind period, so no seam) combs
    // each streak into fibres; without it they were smooth ribbons, like lenticular strands.
    vec4  s3   = textureLod(cv2ShapeTex, cc * 9.0 + vec3(0.57, 0.11, 0.83), lod + 3.17);
    ci *= mix(1.0, mix(0.3, 1.4, clamp((s3.a - 0.5) / 0.12 + 0.5, 0.0, 1.0)), 1.0 - smoothstep(1.0, 3.0, lod));
    float cs   = regime * mix(0.6, 1.0, clamp(fibN / 0.08 + 0.5, 0.0, 1.0));
    float cu   = clamp((s2.g - mix(0.75, 0.55, regime)) * 5.0, 0.0, 1.0);
    // Fibres and cloudlets far below a pixel become the haze of their coverage (as the low cells do).
    float far = smoothstep(1.5, 4.0, lod);
    ci = mix(ci, regime * 0.3, far);
    cu = mix(cu, regime * 0.25, far);
    float prof = smoothstep(0.0, 0.25, z) * (1.0 - smoothstep(0.55, 1.0, z));
    float d    = prof * mix(mix(ci, cs, strat), cu, ccK);
    hfH  = z;
    topH = zb + thick;
    return d * mix(mix(0.0008, 0.0003, strat), 0.0016, ccK) * cv2.high2.x;
}

// ── The cumulonimbus anvil ───────────────────────────────────────────────────────────────────────
// Deep convection's outflow spreads under the tropopause into a flat, smooth-topped ice shield tens
// to hundreds of km across, carried downwind of the towers that feed it; the strongest towers
// overshoot it in domes (the Cb column reaches ~250 m above the lid). From orbit that shield IS the
// storm: without it storms were piles of towers, "mountains". Where: the weather cube read coarse
// (~30 km texels), a little upwind, so it spans the storm region rather than each tower. Ice: a
// lower extinction than the water cloud below; the lid is sharp, the underside lumpy, the outline
// frayed by the cluster Perlin.
float cv2AnvilSigma(CV2Pos q, float fpM, out float hfA, out float topA)
{
    hfA = 0.0; topA = 0.0;
    if (cv2.storm.z <= 0.0 || q.h < 7000.0) return 0.0;
    vec3  wd     = cv2Drift(q.dirE);
    vec3  eastW  = vec3(-wd.y, wd.x, 0.0) * inversesqrt(max(dot(wd.xy, wd.xy), 1e-6));
    vec4  wc     = textureLod(cv2WeatherTex, wd - eastW * (25000.0 / R_EARTH), 2.5);
    float cov    = clamp((wc.r * cv2.look.x - cv2.cover.x) / max(cv2.cover.y - cv2.cover.x, 1e-3), 0.0, 1.0);
    float stormA = smoothstep(0.62, 0.88, wc.g) * smoothstep(0.35, 0.8, cov);
    if (stormA <= 0.0) return 0.0;
    float top    = cv2.types[4].alt.x + (cv2.types[4].alt.y - cv2.types[4].alt.x) * (wc.a + 0.5) - 250.0;
    if (q.h > top || q.h < top - 3600.0) return 0.0;
    vec4  cl     = textureLod(cv2MesoTex, cv2.anchorCluster.xyz + cv2Drift(q.seaProjE) * cv2.anchorCluster.w,
                              cv2Lod(fpM, cv2.anchorCluster.w));
    float a0     = stormA + (cl.a - 0.5) * 0.7;
    if (a0 <= -0.15) return 0.0;                        // the finer fray below adds at most ~0.2
    vec4  sh     = textureLod(cv2ShapeTex, cv2.anchorStorm.xyz + cv2Drift(q.rSeaE) * cv2.anchorStorm.w,
                              cv2Lod(fpM, cv2.anchorStorm.w));
    // The outline frayed at the storm-lobe scale too: on the cluster Perlin alone it was a smooth
    // ellipse, one weather texel's blur.
    float a      = a0 + (sh.b - 0.45) * 0.45;
    if (a <= 0.05) return 0.0;
    float thick  = 3000.0 * clamp(a, 0.0, 1.0);        // thickest over the storm, thin at its edge
    float za     = (q.h - (top - thick)) / thick;
    if (za <= -0.2) return 0.0;
    float under  = smoothstep(0.0, 0.35, za + (sh.g - 0.45) * 0.8);
    // The lid is flat but not blank: low domes of the storm-lobe scale, highest over the core
    // (overshooting tops), ~ +-8% of the thickness.
    float lid    = 1.0 - smoothstep(0.93, 1.0, za - (sh.g - 0.45) * 0.3 * a);
    float edge   = smoothstep(0.05, 0.35, a);
    hfA  = clamp(za, 0.0, 1.0);
    topA = top;
    return edge * under * lid * mix(0.6, 1.0, sh.r) * 0.01 * cv2.storm.z * cv2.look.y;
}

// THE cloud field: the low system (stratus .. cumulonimbus, nimbostratus), the mid layer, the anvils
// and the high layer (cirrus family).
CV2Field cv2Field(CV2Pos q, float detailAmt, float fpM)
{
    CV2Field f = cv2FieldLow(q, detailAmt, fpM);
    float hfX, topX, deckX;
    float sm = cv2MidSigma(q, fpM, hfX, topX, deckX);
    if (sm > 0.0) cv2Add(f, sm, hfX, topX, deckX);
    float sa = cv2AnvilSigma(q, fpM, hfX, topX);
    if (sa > 0.0) cv2Add(f, sa, hfX, topX, 1.0);
    float sh = cv2HighSigma(q, fpM, hfX, topX);
    if (sh > 0.0) {
        cv2Add(f, sh, hfX, topX, 0.0);
        f.thin = sh / f.sigma;
    }
    return f;
}

// ── Atmospheric optics (.plans/ATMOS_OPTICS_AND_STORMS.md) ──────────────────────────────────────
// Added to the single-scattering phase of ice (the high layer) and rain samples. Every POSITION is
// Snell's law on the real shape, per colour channel (R 650 / G 550 / B 450 nm), so the colour order
// and the dependence on the light's elevation are exact; the intensity profiles are analytic
// stand-ins (a sharp inner edge and an outward tail for halos; a caustic peak, a bright interior and
// Alexander's dark band for the bows), to be replaced by baked phase tables later. v and s are unit
// vectors in the observer's ENU frame (z up): the view ray and the direction TO the light (Sun, or
// the Moon at night — moon halos and moonbows come free).
const vec3 CV2_N_ICE   = vec3(1.3069, 1.3108, 1.3165);
const vec3 CV2_N_WATER = vec3(1.3314, 1.3350, 1.3403);

float cv2Sq(float x) { return x * x; }

// Ice crystals: the 22 and 46 degree halos (randomly oriented hexagonal prisms: minimum deviation of
// the 60 and 90 degree prisms), and from plates falling flat the sundogs (at the light's elevation,
// through the effective index n' = sqrt(n^2 - sin^2 e) / cos e — 22 deg out at the horizon, 36 at
// 40 deg, none above ~61), the parhelic circle and the circumzenithal arc (only below 32 deg).
vec3 cv2IceOptics(vec3 v, vec3 s)
{
    float a = acos(clamp(dot(v, s), -1.0, 1.0));
    vec3  D22 = 2.0 * asin(CV2_N_ICE * 0.5) - radians(60.0);
    vec3  D46 = 2.0 * asin(CV2_N_ICE * 0.7071068) - radians(90.0);
    vec3  av  = vec3(a);
    vec3  h22 = smoothstep(D22 - radians(0.25), D22 + radians(0.15), av) * exp(-max(av - D22, 0.0) / radians(1.8));
    vec3  h46 = smoothstep(D46 - radians(0.4), D46 + radians(0.3), av) * exp(-max(av - D46, 0.0) / radians(3.0));

    float e   = asin(clamp(s.z, -1.0, 1.0));
    float ev  = asin(clamp(v.z, -1.0, 1.0));
    float daz = abs(atan(v.y, v.x) - atan(s.y, s.x));
    daz = min(daz, 2.0 * PI - daz);
    float ce = cos(e), se = sin(e);
    vec3  np  = sqrt(CV2_N_ICE * CV2_N_ICE - se * se) / max(ce, 1e-3);
    vec3  arg = np * 0.5;
    vec3  Dp  = 2.0 * asin(min(arg, vec3(1.0))) - radians(60.0);
    vec3  dz  = vec3(daz);
    vec3  dogs = step(arg, vec3(0.999)) * smoothstep(Dp - radians(0.25), Dp + radians(0.15), dz)
               * exp(-max(dz - Dp, 0.0) / radians(3.0)) * exp(-cv2Sq((ev - e) / radians(1.3)));
    float circle = exp(-cv2Sq((ev - e) / radians(0.35))) * smoothstep(radians(8.0), radians(25.0), daz) * 0.6;
    vec3  cz2 = CV2_N_ICE * CV2_N_ICE - ce * ce;
    vec3  hZ  = radians(90.0) - acos(sqrt(clamp(cz2, 0.0, 1.0)));
    vec3  cza = step(cz2, vec3(1.0)) * exp(-(vec3(ev) - hZ) * (vec3(ev) - hZ) / cv2Sq(radians(0.6)))
              * (1.0 - smoothstep(radians(40.0), radians(80.0), daz)) * smoothstep(-0.02, 0.1, se);
    float up = smoothstep(-0.03, 0.02, se);          // plates need the light above the horizon
    return 4.0 * h22 + 0.7 * h46 + up * (12.0 * dogs + vec3(circle) + 3.0 * cza);
}

// Rain drops: the primary bow (one internal reflection, 42.3 deg red .. 41.0 blue from the antisolar
// point, red outside), the secondary (two reflections, 50.5 .. 52.8, colours reversed), the bright
// sky inside the primary and Alexander's dark band between them.
vec3 cv2RainOptics(vec3 v, vec3 s)
{
    float a  = acos(clamp(dot(v, -s), -1.0, 1.0));
    vec3  av = vec3(a);
    vec3  i1 = acos(sqrt((CV2_N_WATER * CV2_N_WATER - 1.0) / 3.0));
    vec3  R1 = 4.0 * asin(sin(i1) / CV2_N_WATER) - 2.0 * i1;
    vec3  i2 = acos(sqrt((CV2_N_WATER * CV2_N_WATER - 1.0) / 8.0));
    vec3  R2 = 2.0 * i2 - 6.0 * asin(sin(i2) / CV2_N_WATER) + PI;
    vec3  d1 = (av - R1) / radians(0.7);
    vec3  prim = exp(-d1 * d1)
               + 0.22 * (1.0 - smoothstep(R1 - radians(0.5), R1 + radians(0.3), av)) * exp(min(av - R1, 0.0) / radians(12.0));
    vec3  d2 = (av - R2) / radians(0.9);
    vec3  sec  = 0.4 * exp(-d2 * d2)
               + 0.08 * smoothstep(R2 - radians(0.3), R2 + radians(0.5), av) * exp(-max(av - R2, 0.0) / radians(15.0));
    return prim + sec;
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
