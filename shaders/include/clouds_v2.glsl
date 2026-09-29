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
    vec4  high2;          // x high-layer density, y 1 / cirrus field size (m^-1), z cirrus flow (x the low
                          // flow), w cumulus base flatness (0..1: the lobes damped at the base)
    vec4  rain;           // x rain amount (0 = none), y optics strength (halos, sundogs, rainbows),
                          // z rain streaks at the eye, w wind (m/s, east) for the streaks' slant
    vec4  beam;           // Reflect-Orbital beams as light (.plans/BEAMS_V2_PLAN.md): x shaft gain (0 =
                          // the old drawn pointing ray in cloud_march.comp), y aerosol (haze/dust)
                          // multiplier for the shafts, z beam light on cloud (x the physical
                          // irradiance), w 1 / (1361 x beamGain): intensity -> reflecting area (m^2)
    vec4  anchorMid;      // the shape volume at the mid layer's own period (its cloudlets)
    vec4  atmo;           // x Rayleigh gain on the SUNLIGHT reaching cloud (independent of the sky's
                          // atmosRayleighGain), y twilight sky light, z mid-layer density, w beam lines
    vec4  anchorFlow;     // the mesoscale volume at the flow period (the large swirls that bend the noise)
    vec4  flow;           // x flow displacement (m per unit of the normalised field), y layer spread
    vec4  anchorCol;      // the Cb column lattice: xyz frac(sea point / period), w 1 / period (period =
                          // 64 lattice cells, so a cell id mod 64 is stable as the observer moves)
    vec4  column;         // x Cb column amount (0 = the pass-14 deep cores in the low layer), y lattice
                          // cell (m), z column base radius (m), w the low layer's top in storm regions (m)
    vec4  column2;        // x waist (x the base radius), y head flare (x the base radius), z head drift
                          // downwind (m), w lobe strength (fraction of the radius)
    vec4  anvil2;         // x anvil thickness (m), y anvil hang around a tower's head (m), z tower
                          // sparsity (0 = every candidate in a storm, 1 = only the strongest cores),
                          // w overshooting top: how far a tower's dome rises above the anvil lid (m)
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

// Tropopause (cloud-top scale): ~16 km in the tropics, ~9 km at the poles; the type table's tops are
// written for the mid-latitudes (scale 1.0 at ~11 km). A function of latitude only, so it is computed
// here (the weather cube's alpha used to store it and now carries the ground height).
float cv2Tropo(vec3 wd)
{
    float latAbs = abs(asin(clamp(wd.z, -1.0, 1.0))) * (180.0 / PI);
    return mix(1.35, 0.8, smoothstep(15.0, 65.0, latAbs));
}

// The ground under a column, smoothed over ~20 km (the weather cube's alpha: DEM height / 8 km,
// baked per mip). Read at the EARTH-FIXED direction: the map drifts, the terrain does not.
bool  gCv2GroundSet = false;   // cv2Field reads the ground once per sample for every layer
float gCv2Ground    = 0.0;
float cv2Ground(vec3 dirE)
{
    if (gCv2GroundSet) return gCv2Ground;
    return textureLod(cv2WeatherTex, dirE, 1.5).a * 8000.0;
}

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
    // The deck flag by density share: with max(), a thin anvil sheet crossing a dense tower gave the
    // whole sample the deck's path-to-top shadow at the tower's density — a black band across the
    // tower at the anvil's height (user snap 2, pass 18).
    else                { f.topH = max(f.topH, topH); f.deck = (f.deck * f.sigma + deck * s) / max(f.sigma + s, 1e-9); }
    f.sigma += s;
}

// Mip level of a noise volume for a footprint of fpM metres (128 texels per period, w = 1/period).
float cv2Lod(float fpM, float invPeriod) { return max(0.0, log2(max(fpM * invPeriod * 128.0, 1e-6))); }

// The mesoscale volume's G (the cells) at a COARSE mip, C1-smooth: the fractional texel coordinate is
// smoothstep-remapped before one hardware fetch, so the field has no creases along the texel grid.
// Linear filtering of a 4-16 texel-per-period mip is piecewise-linear — flat facets and straight
// creases — and extruded up a 10 km storm column those were planar walls and fins (user snaps 7-8,
// pass 13; the v1 warp bake met the same faceting). A fractional lod blends two integer mips.
float cv2MesoSmoothAtG(vec3 uvw, float lodI)
{
    vec3 sz = vec3(textureSize(cv2MesoTex, int(lodI)));
    vec3 u  = uvw * sz - 0.5;
    vec3 i  = floor(u), f = u - i;
    f = f * f * (3.0 - 2.0 * f);
    return textureLod(cv2MesoTex, (i + f + 0.5) / sz, lodI).g;
}
// The weather cube at mip 0, C1-smooth (the same remap, per cube face). Its 5 km texels, linearly
// filtered, made the coverage a patchwork of flat bilinear facets with straight creases (and the
// JPEG's block edges), and a storm column extruded them 10 km up into planar walls and fins
// (user snaps 7-10, pass 13).
vec4 cv2WeatherSmooth(vec3 d)
{
    vec3  a  = abs(d);
    float sz = float(textureSize(cv2WeatherTex, 0).x);
    // The face's two in-plane coordinates in [-1, 1], and the major axis.
    vec2 uv; vec3 axis;
    if (a.x >= a.y && a.x >= a.z)      { uv = d.yz / a.x; axis = vec3(sign(d.x), 0.0, 0.0); }
    else if (a.y >= a.z)               { uv = d.xz / a.y; axis = vec3(0.0, sign(d.y), 0.0); }
    else                               { uv = d.xy / a.z; axis = vec3(0.0, 0.0, sign(d.z)); }
    vec2 t = (uv * 0.5 + 0.5) * sz - 0.5;
    vec2 i = floor(t), f = t - i;
    f  = f * f * (3.0 - 2.0 * f);
    uv = ((i + f + 0.5) / sz) * 2.0 - 1.0;
    vec3 ds = (axis.x != 0.0) ? vec3(axis.x, uv.x, uv.y)
            : (axis.y != 0.0) ? vec3(uv.x, axis.y, uv.y)
                              : vec3(uv.x, uv.y, axis.z);
    return textureLod(cv2WeatherTex, ds, 0.0);
}

// The shape volume at an integer mip, C1-smooth (the same remap as cv2WeatherSmooth, one fetch). At
// its finest mip (150 m texels at the storm scale) a threshold cutting through linear filtering drew
// the texel lattice on a lit anvil wall as a regular waffle of cubes (user snap 2, pass 17).
vec4 cv2ShapeSmooth(vec3 uvw, float lod)
{
    float l  = floor(lod + 0.5);
    vec3  sz = vec3(textureSize(cv2ShapeTex, int(l)));
    vec3  t  = uvw * sz - 0.5;
    vec3  i  = floor(t), f = t - i;
    f = f * f * (3.0 - 2.0 * f);
    return textureLod(cv2ShapeTex, (i + f + 0.5) / sz, l);
}

// The weather cube at an integer mip, bilinear in FLOAT (C1: smoothstep weights) from four texel-centre
// fetches. Hardware filtering blends with 8-bit sub-texel weights, so a mip-3 read (~40 km texels)
// moves in ~150 m steps; a tower's 11 km radius scaled by it came out as vertical flutes every few
// hundred metres up the whole wall (user snaps, pass 18). The four texels stay on this face (clamped):
// towers already fade out near cube-face edges.
vec4 cv2WeatherBilinear(vec3 d, float lod)
{
    vec3  a  = abs(d);
    float sz = float(textureSize(cv2WeatherTex, int(lod)).x);
    vec2 uv; int fc;
    if (a.x >= a.y && a.x >= a.z)      { uv = d.yz / a.x; fc = 0; }
    else if (a.y >= a.z)               { uv = d.xz / a.y; fc = 1; }
    else                               { uv = d.xy / a.z; fc = 2; }
    float sg = (fc == 0) ? sign(d.x) : ((fc == 1) ? sign(d.y) : sign(d.z));
    vec2 t = (uv * 0.5 + 0.5) * sz - 0.5;
    vec2 i = clamp(floor(t), vec2(0.0), vec2(sz - 2.0)), f = clamp(t - i, 0.0, 1.0);
    f = f * f * (3.0 - 2.0 * f);
    vec4 r[4];
    for (int k = 0; k < 4; ++k) {
        vec2 c  = ((i + vec2(k & 1, k >> 1) + 0.5) / sz) * 2.0 - 1.0;
        vec3 ds = (fc == 0) ? vec3(sg, c.x, c.y) : ((fc == 1) ? vec3(c.x, sg, c.y) : vec3(c.x, c.y, sg));
        r[k] = textureLod(cv2WeatherTex, ds, lod);
    }
    return mix(mix(r[0], r[1], f.x), mix(r[2], r[3], f.x), f.y);
}

float cv2MesoSmoothG(vec3 uvw, float lod)
{
    float l0 = floor(lod), fl = lod - l0;
    float a  = cv2MesoSmoothAtG(uvw, l0);
    return fl < 0.001 ? a : mix(a, cv2MesoSmoothAtG(uvw, l0 + 1.0), fl);
}

// FLOW: a large, smooth displacement (m, tangent to the sphere) that the low cloud's mesoscale
// fields are read through, and half of it the weather map itself, so systems bend into curved bands
// and swirls. ANALYTIC since pass 13: the curl of a potential of eight smooth waves on the drifted
// sphere, evaluated in float. It was the Perlin channel of the (RGBA8) mesoscale volume at a coarse
// mip, and hardware filtering interpolates with 8-bit sub-texel weights: at ~375 km texels the
// displacement moved in steps every ~1.5 km, each step shifting the clouds by hundreds of metres —
// blocks of shifted cloud with straight seams, aligned to the texture grid (anchored at the observer),
// worse the stronger the warp (user, pass 12; the v1 distortion showed the same). A curl field is also
// divergence-free: it swirls the noise around, without the converging flow that bunched it into
// folds. The shear (|grad D|) is ~flow.x x 2 pi / wavelength: keep it well under 1.
// wd: the drifted direction. flow.x = the rms displacement (m); anchorFlow.w = 1 / the period (1/m).
// cv2Field evaluates the flow ONCE per sample and every layer reads it (four layers each paid the
// eight waves, and the Cb columns a fifth time). Outside cv2Field (debug views, the eye's rain) the
// flag is off and each call evaluates its own.
bool gCv2FlowSet = false;
vec3 gCv2FlowD   = vec3(0.0);
vec3 cv2FlowDisp(vec3 wd, vec3 sP)
{
    if (gCv2FlowSet) return gCv2FlowD;
    if (cv2.flow.x <= 0.0) return vec3(0.0);
    // Wave vectors (unit directions x k), fixed; k = 2 pi R / period (radians of the sphere per rad).
    const vec3 kd[8] = vec3[8](vec3( 0.62,  0.48,  0.62), vec3(-0.71,  0.30,  0.64), vec3( 0.18, -0.93,  0.32),
                               vec3( 0.85,  0.21, -0.48), vec3(-0.34, -0.55, -0.76), vec3( 0.05,  0.77, -0.64),
                               vec3(-0.90, -0.40,  0.17), vec3( 0.44, -0.29,  0.85));
    const float ks[8] = float[8](1.0, 1.13, 0.87, 1.31, 0.74, 1.52, 0.93, 1.21);
    const float ph[8] = float[8](0.3, 2.1, 4.4, 1.7, 5.6, 3.2, 0.9, 2.7);
    float k0 = 6.2831853 * R_EARTH * cv2.anchorFlow.w;
    vec3  g  = vec3(0.0);
    for (int i = 0; i < 8; ++i) {
        vec3 kv = normalize(kd[i]) * (k0 * ks[i]);
        g += kv * cos(dot(kv, wd) + ph[i] + cv2.anchorFlow.x * 6.2831853) / ks[i];   // grad of sin / k
    }
    // Rotate the tangential gradient by 90 degrees about the vertical: the curl, i.e. the stream
    // function's flow. Normalised so each component is ~N(0,1): 8 waves, gradient ~k0 each.
    vec3 v = cross(wd, g) / (k0 * 2.0);
    return v * cv2.flow.x;
}
// The weather-map direction the low cloud reads at q (half the flow). Every layer that follows the
// low cloud's weather (mid, high, anvils, the far-field early outs) must read it here too, or with
// the flow on those layers sit up to ~100 km from the systems they belong to.
vec3 cv2FlowWeatherDir(vec3 wd, vec3 flowD) { return normalize(wd + flowD * (1.0 / R_EARTH)); }
vec3 cv2FlowWeatherDirAt(CV2Pos q)
{
    vec3 wd = cv2Drift(q.dirE);
    return cv2FlowWeatherDir(wd, cv2FlowDisp(wd, cv2Drift(q.seaProjE)));
}

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

    // The noise volumes are read in the same drifted frame.
    vec3 rS = cv2Drift(q.rSeaE);
    vec3 sP = cv2Drift(q.seaProjE);
    // The flow (cv2FlowDisp) moves the weather map and the cluster field by flowD (not the cells).
    vec3 flowD = cv2FlowDisp(wd, sP);
    vec3 wdF   = cv2FlowWeatherDir(wd, flowD);

    // Early out on a coarse read AT THE FLOWED position. Conservative: its 20 km texels span the
    // few-km warp below, and their box average keeps at least a sixteenth of one cloudy 5 km texel.
    // (Pass 11 tested the UNflowed position at mip 4 while the lookup moved up to ~200 km: every
    // cloud displaced past an 80 km texel was cut off along that texel's straight edge — the
    // "discontinuities cutting the clouds up" at any flow setting.)
    if (textureLod(cv2WeatherTex, wdF, 2.0).r * cv2.look.x < cv2.cover.x * 0.25) return f;

    // Towers lean downwind with height: the mesoscale fields are read that far upwind (east is the
    // stand-in wind until the weather evolves, phase 5).
    vec3 eastW = vec3(-wd.y, wd.x, 0.0) * inversesqrt(max(dot(wd.xy, wd.xy), 1e-6));
    // The flow bends the weather map and the cluster field (the systems, what shows from space) but
    // NOT the cells or the lobes: any displacement field stretches everything read through it by its
    // shear, whatever the field's scale, and the 2 km cells stretched into combed ridges that the
    // shape extruded into vertical fins (user snaps 7-10, pass 13, flow 0.2). Local convection is not
    // stretched by the synoptic flow; it is only placed by it (the map).
    vec3 mp    = sP - eastW * (cv2.form.y * max(q.h - 800.0, 0.0));
    vec3 mpF   = mp + flowD;
    vec4 cl = textureLod(cv2MesoTex, cv2.anchorCluster.xyz + mpF * cv2.anchorCluster.w,
                         cv2Lod(fpM, cv2.anchorCluster.w));
    vec4 ce = textureLod(cv2MesoTex, cv2.anchorCell.xyz + mp * cv2.anchorCell.w,
                         cv2Lod(fpM, cv2.anchorCell.w));

    // The weather map's texels are 5 km: thresholding them bilinearly draws their squares, which
    // read as a grid from altitude. Warping the lookup by a few km of mesoscale noise hides it.
    // Smooth Perlin channels only. The third component was the closed-cell RIM field (ce.b: 0 on thin
    // Voronoi rims, 1 inside — a near step) - 0.6: x 7 km the lookup jumped ~4 km across every rim, the
    // map FOLDED along straight Voronoi edges, and storms extruded those folds into planar walls and
    // fins (user snaps 7-10, pass 13; project_cloud_warp_shear: a warp's shear must stay < 1).
    vec3  warp = (vec3(ce.a, cl.a, 0.5 * (ce.a + cl.a)) - 0.5) * cv2.extra.y;
    vec4  w    = cv2WeatherSmooth(wdF + warp);
    // Coverage is the map's brightness remapped: thin cloud is grey in the imagery, an overcast
    // deck is not pure white, and the raw value used as a fraction never let anywhere close over.
    float cov  = clamp((w.r * cv2.look.x - cv2.cover.x) / covSpan, 0.0, 1.0);
    if (cov <= 0.0) return f;

    CV2Type ty     = cv2TypeAt(w.g);
    float   strat  = 1.0 - ty.look.z;                  // 1 = the stratiform field, 0 = the cells
    float   tropo  = cv2Tropo(wd);
    float   topMax = ty.alt.x + (ty.alt.y - ty.alt.x) * tropo;
    // Nimbostratus: a stratiform deck where the map says it rains thickens by up to 3.5 km (its
    // base is already darkened by the precipitation loading below).
    topMax += w.b * strat * 3500.0;
    // Bases follow the (smoothed) terrain: a convective base is the condensation level, about the
    // same height above the ground everywhere; decks follow it less (they hug and bank against
    // slopes). With every base above sea level the Tibetan plateau and the Andes stood inside the
    // cloud, and every ridge cut through it. Tops rise with the base, but not past the tropopause.
    float lift   = cv2Ground(q.dirE) * mix(0.6, 0.9, ty.look.z);
    float topCap = cv2.types[4].alt.x + (cv2.types[4].alt.y - cv2.types[4].alt.x) * tropo;
    topMax = min(topMax + lift, max(topCap, ty.alt.x + lift + 500.0));
    // With the Cb COLUMN layer on (cv2ColumnSigma) the towers and their heads are that layer's: here a
    // storm region is only its cumulus/congestus field, capped at column.w. A low-layer core running
    // the full span was a mountain sloping up into the anvil: a margin field thresholded per height
    // cannot narrow and flare again (user snap 4, pass 14).
    bool colOn = cv2.column.x > 0.0;
    // A TARGET, not a ceiling: as min() it only ever lowered the tops, and the storm regions' type
    // (mostly congestus) already topped out below it, so the slider did nothing upward (user, pass 19).
    // Near a storm, not only in it: convective cells within ~80 km of one (the weather's type at mip 4,
    // float-bilinear: this scales kilometres of height) take the storm cumulus top too, so cumulus
    // is taller on average around storms (user, pass 20). Decks keep their own tops.
    if (colOn) {
        float wS = smoothstep(0.5, 0.75, w.g);
        // (The map types only a storm's core as Cb, and the towers cover that; the cloud around it is
        // typed stratocumulus, so the threshold is well below the Cb class, and stratocumulus rises
        // halfway. At the Cb threshold the slider changed 0.2% of a storm view, pass 20.)
        if (ty.look.z > 0.01 && wS < 1.0)
            wS = max(wS, smoothstep(0.3, 0.6, cv2WeatherBilinear(wdF, 4.0).g) * min(ty.look.z * 2.0, 1.0));
        topMax = mix(topMax, cv2.column.w + lift, wS);
    }

    // A deck is not a slab: its thickness follows the closed cells and a km-scale Perlin field, and
    // a thicker part hangs lower (an overcast seen from below was one flat, featureless plane).
    // Closed-cell rims (cl.b) fade out with the footprint: from orbit they read as cracked ice.
    float rimAmt  = 0.25 * (1.0 - smoothstep(0.5, 2.5, cv2Lod(fpM, cv2.anchorCell.w)));
    float deckVar = clamp((ce.a - 0.5) * 3.0 + (cl.b - 0.5) * rimAmt * 4.0, -1.0, 1.0);
    float base    = ty.alt.x + lift + (cl.a - 0.5) * 2.0 * ty.alt.w - strat * 150.0 * deckVar;
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
    // At a FIXED coarse mip (or the footprint's, once coarser): these set a cell's width and height,
    // and at footprint + 2 (+ 4 below) they changed with the viewing distance, so every storm was
    // stretched along the view rays into fins radiating from the camera (user snaps, pass 13).
    float lodCell    = cv2Lod(fpM, cv2.anchorCell.w);
    float cellsC     = cv2MesoSmoothG(cv2.anchorCell.xyz + mp * cv2.anchorCell.w, max(lodCell, 2.0));
    // Deep types: the coarse cells only. The fine cells' 2 km edges, extruded up a 10 km column, were
    // vertical grooves on every storm wall (user snaps 7-10, pass 13): the 3D lobes shape a tower.
    float cells      = mix(ce.g, min(cellsC * 1.8, 1.0), deep);
    float fieldConv  = 0.15 + 0.85 * sqrt(cells) * mix(0.6, 1.0, cl.r);
    float fieldStrat = 0.5 + rimAmt * (cl.b - 0.5) + 0.125 + 0.3 * (ce.a - 0.5) + 0.2 * ce.g;
    float field      = mix(fieldStrat, fieldConv, ty.look.z);
    // ── Mesoscale structure, 3-90 km: the cluster Perlin at 1x and 4x, normalised (~N(0,1)). From
    // orbit the cells are far below a pixel and average away, and the decks' rims fade out, so the
    // presence was the map's own 5-10 km blobs, warped: the "blobby" clouds from space. Real imagery
    // there shows broken fields, holes and fractal edges at 3-100 km. The far field below is this
    // noise thresholded to the map's coverage (area fraction ~ cov: z = 2.6 (0.5 - cov) approximates
    // the normal quantile), and a little of it clusters the near field too, so the holes seen from
    // orbit are still there on the way down instead of the two looks morphing into each other.
    vec4  cl4    = textureLod(cv2MesoTex, (cv2.anchorCluster.xyz + mpF * cv2.anchorCluster.w) * 4.0
                              + vec3(0.37, 0.61, 0.13), cv2Lod(fpM, cv2.anchorCluster.w * 4.0));
    float fz     = ((cl.a - 0.5) * 0.6 + (cl4.a - 0.5) * 0.4) / 0.041;
    float zThr   = 2.6 * (0.5 - cov);
    float farK   = smoothstep(0.5, 2.5, cv2Lod(fpM, cv2.anchorCell.w));
    field = mix(field + 0.08 * clamp(fz, -2.5, 2.5), (1.0 - cov) + 0.385 * (fz - zThr), farK);
    // How far into the cloud this column is: 0 at the edge, 1 well inside. Not normalised by the
    // coverage, so sparse fair-weather cells stay small and low; only a dense field builds towers.
    float e          = clamp((field - (1.0 - cov)) / 0.55, 0.0, 1.0);
    // The same strength at the CELL scale (the cells two mips coarser: a cell's neighbourhood mean;
    // four for deep types, whose cells are already the coarse ones). It sets each cell's height and
    // its peak, so every column of a cell shares one profile (see THE CONVECTIVE PROFILE below).
    float cellsB     = deep > 0.02
                     ? mix(cellsC, cv2MesoSmoothG(cv2.anchorCell.xyz + mp * cv2.anchorCell.w, max(lodCell, 4.0)), deep)
                     : cellsC;
    float fieldB     = 0.15 + 0.85 * sqrt(min(cellsB * 1.8, 1.0)) * mix(0.6, 1.0, cl.r)
                     + 0.08 * clamp(fz, -2.5, 2.5);
    float eB         = clamp((fieldB - (1.0 - cov)) / 0.55, 0.0, 1.0);

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
    // THE CONVECTIVE PROFILE (pass 12). Until now the strength needed to reach a height rose with
    // it (z^1.2) and each COLUMN's top came from its own strength: the surface was e(x,y) = g(z), a
    // cone over every cell, whatever the lobes did on it — gumdrops and spiky towers, never a head
    // wider than its stem, and a strong core (e ~ 1 over a wide area) ran into the type's top and was
    // cut flat there. Now each CELL is one cloud with a vertical profile (the reference's per-type
    // coverage curve, applied per cell):
    //   T     the cell's top, from its cell-scale strength eB (shared by its columns), at most
    //         flow.w of the span (headroom: the lobes still fit under the type's top)
    //   req   the strength a column needs at height zeta = z / T: parabolic about zm, the cloud's
    //         widest point, high up — reqMin there (the gaps between cells stay clear), more at the
    //         base (a narrower stem, flow.z "Top-heavy"), 1 at the top. With e ~ 1 - r^2 over a
    //         cell the body is a rounded, top-heavy ellipsoid. The profile must slope everywhere: a
    //         flat stretch (smoothstep) extruded the 2D field straight up into vertical walls and
    //         flutes (tried first, pass 12). Normalising e by the cell's peak was tried too: it
    //         lifted the weak gaps between cells over the threshold and dense fields became slabs.
    //   eS    e capped by the cells' own dome: in a dense field e saturates (the coverage threshold
    //         is far below the cells), and the merged mass would end in a flat mesa at T; capped,
    //         it keeps a turret over each cell.
    // (Deep towers were tried with straighter walls, 0.9 z^1.8: that extruded the 2D field into
    // vertical flutes. They are wide via the coarse cells; the profile shapes them.)
    // flow.w (Tower top) 0 = the pass-11 shape (cones: g = z^1.2, each column its own top), for A/B.
    bool  legacy = cv2.flow.w <= 0.0;
    float T      = legacy ? max(pow(e, 0.83), 0.05)
                          : cv2.flow.w * clamp(max(pow(eB, 0.83), 0.6 * pow(e, 0.83)), 0.25, 1.0);
    // Deep convection runs the full span, up to the anvil (cv2AnvilSigma sits under the same top):
    // capped at "Tower top" the anvil floated free of its column (user, pass 12).
    // Only strong cores: every deep cell at full height filled the storm with cloud (56 ms frames
    // inside a Cb field, 15 before).
    float deepCol = (legacy || colOn) ? 0.0 : deep * smoothstep(0.5, 0.9, eB);
    T = mix(T, 1.0, deepCol);
    float zeta   = z / T;
    // (A smooth min: min() creased the surface where the two cross, and sqrt(cells) is a cliff at
    // each blob's rim — both drew flat, rock-like facets.)
    float capS   = 0.3 + 0.7 * cells;
    float hS     = max(0.2 - abs(e - capS), 0.0) / 0.2;
    float eS     = min(e, capS) - hS * hS * 0.05;
    float zmC    = mix(0.55, 0.65, deep);
    // reqMin keeps the gaps between cells in a dense field; a weak, isolated cell (small eB) needs
    // almost none, or the fair-weather puffs vanish (e barely clears the coverage there).
    float reqMin = 0.2 * smoothstep(0.1, 0.6, eB);
    // Only TALL cells get the narrower stem: on every cell it rounded the shallow cumulus' undersides
    // into balls, and a field seen from below read as mammatus (user snap, pass 13).
    float tallC  = smoothstep(0.4, 0.8, T / max(cv2.flow.w, 0.3));
    float req0   = reqMin + cv2.flow.z * mix(0.15, 0.3, deep) * smoothstep(0.05, 0.4, eB) * tallC;
    float uC     = min(zeta / zmC, 1.0) - 1.0;                  // -1 at the base .. 0 at zm
    float vC     = max(zeta - zmC, 0.0) / (1.0 - zmC);          // 0 at zm .. 1 at the top
    float reqC   = legacy ? pow(z, 1.2)
                          : reqMin + (req0 - reqMin) * uC * uC + (1.0 - reqMin) * vC * vC;
    // A cumulonimbus is concave: wide at the base (the inflow), narrowing to a waist through the
    // middle, flaring out again under the tropopause into the anvil, flat-lidded at the top. The
    // waist's requirement is where only the core stands; the flare's is low, so the column's upper
    // part spreads toward the anvil's extent. "Top-heavy" scales the waist.
    if (deepCol > 0.0) {
        float rWaist = 0.55 + 0.2 * cv2.flow.z;
        float reqD   = 0.12 + (rWaist - 0.12) * smoothstep(0.0, 0.45, zeta)
                     - (rWaist - 0.25) * smoothstep(0.62, 0.9, zeta)
                     + 4.0 * max(zeta - 0.97, 0.0) / 0.03;
        reqC = mix(reqC, reqD, deepCol);
        // The HEAD (pass 13, the user's mushroom reference): above the waist the column is compared
        // against a much wider strength (the cells ~6 mips coarse, ~the storm's extent) read up to
        // 15 km upwind, i.e. displaced downwind: the requirement falls from the waist to the flare as
        // it rises, so the head widens upward past its own column and overhangs downwind — a curved
        // underside sloping up and out. Against the column's own field the flare could not spread
        // beyond its cell, and the storms were towers without heads.
        float headW = smoothstep(0.55, 0.8, zeta) * deepCol;
        if (headW > 0.0) {
            vec3  mh  = mp - eastW * (15000.0 * smoothstep(0.55, 1.0, zeta));
            float cW  = cv2MesoSmoothG(cv2.anchorCell.xyz + mh * cv2.anchorCell.w, max(lodCell, 5.0));
            float eW  = clamp((0.15 + 0.85 * sqrt(min(cW * 2.2, 1.0)) * mix(0.6, 1.0, cl.r) - (1.0 - cov)) / 0.55,
                              0.0, 1.0);
            eS = mix(eS, max(eS, eW), headW);
        }
    }
    // A deck's top, in its span: thin parts glow from below, thick parts go dark (from below an
    // overcast is seen by the light it transmits, so its thickness IS its texture).
    float zTop   = clamp(0.75 + 0.3 * deckVar, 0.4, 1.0);
    float gStrat = 1.2 * smoothstep(0.3, 1.0, z / zTop);
    // In e units (the stratiform margin keeps the raw strength: a deck's thickness follows it).
    float m0     = mix((legacy ? e : eS) - reqC, e - gStrat, ty.alt.z);
    float fpFade = mix(1.0, 0.3, smoothstep(100.0, 800.0, fpM));   // sub-pixel lobes average away
    // Convective lobes are 2.5x stronger with the profile: the reference's shapes come from its 3D
    // noise thresholded by a soft coverage, and here the 2D field's steep edge dominated — the lobes
    // moved a cell's side by ~100 m, so every cloud was its 2D outline extruded (flat faces).
    float lobeK  = cv2.form.x * mix(0.5, 1.0, ty.look.z) * fpFade
                 * (legacy ? 1.0 : mix(1.0, 2.5, ty.look.z * (1.0 - ty.alt.z)));
    // Sub-pixel cells: thresholding the mip-averaged field would erase them (the average sits
    // below the threshold), so where a cell is smaller than the footprint the presence becomes the
    // fraction of area covered — a haze of the right opacity instead of nothing, or aliasing dots.
    // Starts at a 2x footprint (it began at 3x: from orbit the barely resolved cells read as salt).
    float subPix = smoothstep(1.0, 3.0, cv2Lod(fpM, cv2.anchorCell.w)) * ty.look.z;
    {
        // Before the shape fetch, bounded as if the base bumps lifted this point all the way (the
        // gBase and the lobe damping only fall with height): the lobes add at most ~0.6 x 1.25 lobeK.
        float hbMax = hb + 2.2 * baseAmp;
        float gB    = 0.3 * (1.0 - smoothstep(0.0, 350.0, hbMax));
        if (m0 - gB * gB / 0.3 + lobeK * (legacy ? 0.6 : 0.75) <= 0.0 && subPix <= 0.0) return f;
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
    // Convective lobes grow up the cloud (a cauliflower head over a smoother stem).
    // (With the profile, faded in from the cell's edge: at 2.5x the lobes alone grew cloud in the
    // clear gaps between cells.)
    // "Base flatness" (high2.w): how little of the lobes survives at the base (0.3 of them at 0; at
    // the default 0.8, 0.1 — the lobes are 2.5x on convection, and the bases read as mammatus).
    float A     = lobeK * mix(legacy ? 0.3 : mix(0.3, 0.05, cv2.high2.w), 1.0, smoothstep(0.0, legacy ? 500.0 : 700.0, hbE))
                * (legacy ? 1.0 : mix(mix(0.4, 1.0, smoothstep(0.0, 0.25, e)), 1.0, ty.alt.z))
                * (legacy ? 1.0 : mix(mix(0.8, 1.25, smoothstep(0.2, 0.75, zeta)), 1.0, ty.alt.z));
    float m     = m0 + A * lobes * 2.0 - gBase * gBase / 0.3;
    float Ph    = clamp(m * cv2.form.w, 0.0, 1.0) * smoothstep(0.0, 60.0, hbE);
    // The sub-pixel cells' haze, in the mesoscale presence's patches (their mean is still ~cov).
    float presFar = smoothstep(-0.3, 0.3, fz - zThr);
    float PhSub = presFar * 0.8 * clamp((mix(0.2 + 0.4 * cov, 1.0, ty.alt.z) - z) * cv2.form.w, 0.0, 1.0)
                * smoothstep(0.0, 150.0, hb);
    Ph = mix(Ph, PhSub, subPix);
    if (Ph <= 0.0) return f;
    // Height within THIS column (for the lighting terms and billowing): the column's own top.
    // (Convective: the CELL's top T, so hf runs 0..1 up the cloud, not up each column's own cone.)
    float Htop = mix(T, zTop * 0.85, ty.alt.z);
    float hf   = clamp(z / max(Htop, 0.05), 0.0, 1.0);

    // Inside, a little of the macro noise for density variation (the surface is the margin).
    float d = Ph * mix(0.75, 1.0, s.r);

    if (detailAmt >= 0.0) {
        float df = 0.45;   // the detail's mean (inverted-Worley fBm), when it is not fetched
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
            // Mostly the coarse octave: the fine one (~125 m cells) made every dense cumulus
            // surface a uniform popcorn grain.
            // Faded with distance TOWARD THE MEAN (the value used where it is not fetched), at full
            // strength. It used to fade the erosion itself toward none, then switch to the mean at
            // full strength where detailAmt reached 0: a band of uneroded, denser cloud just inside
            // that distance, seen from above as a ring (~220 km out, 143 km up).
            df = mix(0.45, dn.r * 0.75 + dn.g * 0.25, detailAmt);
        }
#endif
        // Base: bite the lumps (torn, wispy). Higher up: bite the cracks, keep the lumps (billows).
        // Deep convection keeps storm.y of it: its large towers read best with little erosion.
        // Deep convection bites far fewer cracks: a Cb top is bubbly domes, and the crack network the
        // billow term carves read as dark veins over every storm (with storm erosion turned up).
        float billow = ty.shape.z * smoothstep(0.05, 0.4, hf) * (1.0 - 0.7 * deep);
        float erode  = mix(df, 1.0 - df, billow) * ty.shape.y * cv2.look.z * mix(1.0, cv2.storm.y, deep);
        // Erosion shapes the SURFACE. Deep inside (d high) only form.z of it applies: at full
        // strength it punched holes through the interior, and from inside a cloud the sky showed.
        erode *= mix(1.0, cv2.form.z, smoothstep(0.35, 0.85, d));
        // A cumulus base is the condensation level: flat. The base erosion ("bite the lumps") dimpled
        // every underside, and a field seen from below read as mammatus (user snap 1, pass 13; the
        // pass-11 shape too). "Base flatness" keeps 1 - 0.7 x it of the erosion in the lowest ~400 m.
        if (!legacy) erode *= mix(mix(1.0, 1.0 - 0.7 * cv2.high2.w, ty.look.z), 1.0, smoothstep(0.0, 400.0, hbE));
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
    f.topH     = base + mix(legacy ? T : T * 1.1, zTop, ty.alt.z) * (topMax - base);
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
    if (q.h < 2800.0 || q.h > 12000.0) return 0.0;
    float gl  = cv2Ground(q.dirE) * 0.8;                                  // follows the terrain
    if (q.h < 2800.0 + gl || q.h > 7200.0 + gl) return 0.0;
    vec3  wF  = cv2FlowWeatherDirAt(q);                                 // the low cloud's (flowed) map
    vec4  w   = textureLod(cv2WeatherTex, wF, 1.0);                     // mid decks are broad
    float cov = clamp((w.r * cv2.look.x - cv2.cover.x) / max(cv2.cover.y - cv2.cover.x, 1e-3), 0.0, 1.0);
    // (The spread below can place mid cloud where the local low coverage is 0, so no early out on cov
    // alone when it is on.)
    if (cov <= 0.0 && cv2.flow.y <= 0.0) return 0.0;
    vec4  cl  = textureLod(cv2MesoTex, cv2.anchorCluster.xyz + cv2Drift(q.seaProjE) * cv2.anchorCluster.w,
                           cv2Lod(fpM, cv2.anchorCluster.w));
    // Fades in with the coverage: the source map is a JPEG, and in clear areas its 8x8 blocks
    // (~40 km) sit just above or below the clear threshold. A full altostratus sheet wherever the
    // coverage was above 0 drew those blocks as straight-edged translucent rectangles.
    // Layer spread (cv2.flow.y): mid-level cloud keys on the weather over ~80 km (mip 4), not only the
    // low cloud under it, and thins over a low overcast. It used to appear only where (and wherever)
    // the low coverage was high: every layer stacked on the same spot.
    float covW   = clamp((textureLod(cv2WeatherTex, wF, 4.0).r * cv2.look.x - cv2.cover.x)
                         / max(cv2.cover.y - cv2.cover.x, 1e-3), 0.0, 1.0);
    float covM   = mix(cov, max(cov, covW * 0.85), cv2.flow.y);
    float regime = smoothstep(0.5, 0.68, cl.a + 0.15 * (covM - 0.5)) * cv2.misc.w * smoothstep(0.0, 0.35, covM)
                 * mix(1.0, 0.55, cv2.flow.y * smoothstep(0.75, 1.0, cov));
    if (regime <= 0.0) return 0.0;
    // The baked Perlin is 0.50 +- 0.057: read raw, the base spanned ~300 m, and the layer was one flat
    // sheet at ~4.5 km around the planet — seen edge-on from near its height, a dead-straight line
    // across every storm, with the towers below it seen dimmed through it (user snap 1, pass 17).
    // Stretched to +-2 sigma it ranges 3-5.6 km over the cluster scale.
    float cr    = clamp(0.5 + (cl.r - 0.5) * 4.4, 0.0, 1.0);
    float base  = 3000.0 + gl + 2600.0 * cr;
    float thick = mix(1200.0, 450.0, cr);
    float zm    = (q.h - base) / thick;
    if (zm <= 0.0 || zm >= 1.0) return 0.0;
    float strat = 1.0 - smoothstep(0.3, 0.55, w.g);                 // stratiform below -> As
    // Its own period (anchorMid): on the cumulus shape period, enlarging the cumulus lobes made the
    // altocumulus cloudlets huge too.
    vec4  sh    = textureLod(cv2ShapeTex, cv2.anchorMid.xyz + cv2Drift(q.rSeaE) * cv2.anchorMid.w,
                             cv2Lod(fpM, cv2.anchorMid.w));
    // A lens: thin at its edges and top and bottom, the cloudlets' own lobes shaping both faces.
    float lens  = smoothstep(0.0, 0.3, zm + (sh.a - 0.5) * 0.4) * (1.0 - smoothstep(0.55, 1.0, zm + (sh.b - 0.45) * 0.5));
    float ac    = clamp((sh.b * 0.7 + sh.g * 0.3 - (1.0 - regime * 0.8)) * 5.0, 0.0, 1.0);
    // Cloudlets far below a pixel become the haze of their coverage (as the low cells do).
    ac          = mix(ac, regime * 0.45, smoothstep(400.0, 3000.0, fpM));
    float as_   = clamp(regime * 1.3, 0.0, 1.0) * mix(0.7, 1.0, sh.a);
    hfMid       = zm;
    topMid      = base + thick;
    deckMid     = strat;
    return lens * mix(ac * 0.035, as_ * 0.004, strat) * cv2.atmo.z;
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
    float tropTop = cv2.types[4].alt.x + (cv2.types[4].alt.y - cv2.types[4].alt.x) * cv2Tropo(wd);
    float base0  = 0.74 * tropTop;
    if (q.h < base0 - 700.0 || q.h > base0 + 2400.0) return 0.0;
    vec3  flowL  = cv2FlowDisp(wd, vec3(0.0));
    vec3  wF     = cv2FlowWeatherDir(wd, flowL);              // the low cloud's (flowed) map
    // The high layer's own frame: the low flow x "Cirrus flow" (the jet bends the upper cloud more;
    // until pass 13 cirrus ignored the flow entirely).
    vec3  wdH    = normalize(wd + flowL * (cv2.high2.z / R_EARTH));
    vec4  wc     = textureLod(cv2WeatherTex, wF, 2.0);
    // Cirrus lives with the weather systems: in the jet ahead of fronts and in the outflow of deep
    // convection; the subtropical highs are mostly free of it. The regime therefore follows the map's
    // coverage over the surrounding ~150 km (mip 4) as much as its own noise. On the noise alone the
    // high layer covered the globe uniformly, and from orbit it was an even field of splotches.
    vec4  wb     = textureLod(cv2WeatherTex, wF, 4.0);
    float span   = max(cv2.cover.y - cv2.cover.x, 1e-3);
    float covL   = clamp((wc.r * cv2.look.x - cv2.cover.x) / span, 0.0, 1.0);
    float covS   = clamp((wb.r * cv2.look.x - cv2.cover.x) / span, 0.0, 1.0);
    float sys    = max(covS, 0.7 * covL);
    vec4  cl     = textureLod(cv2MesoTex, cv2.anchorCluster.xyz + cv2Drift(q.seaProjE) * cv2.anchorCluster.w
                              + vec3(0.41, 0.17, 0.69), cv2Lod(fpM, cv2.anchorCluster.w));
    // The baked Perlin fBm is narrow: 0.50 +- 0.057 (1-99%: 0.37-0.63, measured by replicating the
    // bake). Thresholds are set against that spread; the first cut used 0.5-0.7 and drew no cirrus.
    // A clear system shifts it by ~-1 sd (~13% of the area has some cirrus), a cloudy one by ~+1.2 sd
    // (~85%). Above 1 the amount widens the regime instead (2 = cirrus over most of the sky).
    float widen  = 0.06 * max(cv2.high.x - 1.0, 0.0);
    // Layer spread loosens the tie to the low systems (cirrus is often over clear low levels).
    // The regime's own noise at the cirrus FIELD size (high2.y, ~1200 km; two octaves for a ragged
    // edge), read on the flowed sphere: at the cluster scale it was splotches (user, pass 12). Absolute
    // coordinates are fine here: R / 1200 km ~ 5 periods, float resolves ~1 m.
    vec3  rgc    = wdH * (R_EARTH * cv2.high2.y);
    float rgN    = (textureLod(cv2MesoTex, rgc + vec3(0.23, 0.71, 0.37), 0.0).a - 0.5) * 0.75
                 + (textureLod(cv2MesoTex, rgc * 3.1 + vec3(0.61, 0.13, 0.89), 0.0).a - 0.5) * 0.45
                 + (cl.a - 0.5) * 0.2;
    float regime = smoothstep(0.51 - widen, 0.57 - widen, 0.5 + rgN + 0.12 * (1.0 - 0.5 * cv2.flow.y) * (sys - 0.45))
                 * min(cv2.high.x, 1.0);
    if (regime <= 0.0) return 0.0;
    float zb     = base0 + (cl.r - 0.5) * 1200.0;
    float thick  = mix(500.0, 1600.0, regime);
    float z      = (q.h - zb) / thick;
    if (z <= 0.0 || z >= 1.0) return 0.0;

    float lon = atan(wdH.y, wdH.x), lat = asin(clamp(wdH.z, -1.0, 1.0));
    // Fall streaks: the ice lower in the layer lags the generating heads above it.
    float shear = (zb + thick - q.h) * 2.0;
    vec3  cc  = vec3((lon * R_EARTH - shear) * cv2.high.y + cv2.high.z,
                     // meander: the cluster Perlin normalised by its spread (0.057), +-~0.7 period
                     lat * R_EARTH * cv2.high.w + (cl.a - 0.5) * 12.0,
                     q.h * cv2.high.w);
    float lod = cv2Lod(fpM, cv2.high.w);
    // Bundles: the same volume read 4x coarser (swaths ~30 km across and a few hundred km long; the
    // CPU keeps the along-wind period's count around the equator a multiple of 4, so no seam).
    // Streaks come in bundles that converge and part, not as an even comb: at one scale, thresholded,
    // they read as regular sand ripples. From orbit, where the fibres are far below a pixel, the
    // bundles are what shows: long bands along the jet.
    vec4  sb     = textureLod(cv2ShapeTex, cc * 0.25 + vec3(0.13, 0.47, 0.29), max(lod - 2.0, 0.0));
    float bundle = smoothstep(0.43, 0.6, sb.a);
    cc.y += (sb.g - 0.45) * 1.2;                          // streaks crowd together and fan apart
    vec4  s   = textureLod(cv2ShapeTex, cc, lod);
    vec4  s2  = textureLod(cv2ShapeTex, cc * 3.0 + vec3(0.31, 0.73, 0.17), lod + 1.585);
    float strat = 1.0 - smoothstep(0.3, 0.55, wc.g);
    float ccK   = smoothstep(0.5, 0.7, cl.g) * (1.0 - strat) * 0.8;

    float fibN = (s.a - 0.5) * 0.6 + (s2.a - 0.5) * 0.4;      // ~0 +- 0.04
    float lo   = mix(0.06, -0.02, regime);
    float ci   = smoothstep(lo, lo + 0.07, fibN) * mix(0.2, 1.0, bundle);
    // Hair: a 9x finer read (still an integer multiple of the along-wind period, so no seam) combs
    // each streak into fibres; without it they were smooth ribbons, like lenticular strands.
    vec4  s3   = textureLod(cv2ShapeTex, cc * 9.0 + vec3(0.57, 0.11, 0.83), lod + 3.17);
    ci *= mix(1.0, mix(0.3, 1.4, clamp((s3.a - 0.5) / 0.12 + 0.5, 0.0, 1.0)), 1.0 - smoothstep(1.0, 3.0, lod));
    float cs   = regime * mix(0.6, 1.0, clamp(fibN / 0.08 + 0.5, 0.0, 1.0)) * mix(0.6, 1.0, bundle);
    float cu   = clamp((s2.g - mix(0.75, 0.55, regime)) * 5.0, 0.0, 1.0);
    // Fibres and cloudlets far below a pixel become the haze of their coverage (as the low cells
    // do) — in their bundles, so from orbit the high layer is banded, not an even veil.
    float far = smoothstep(1.5, 4.0, lod);
    // Mostly a soft veil at a distance: a stretched noise is parallel streaks at every scale, and
    // thresholded far away it read as evenly spaced ripples from orbit.
    // From orbit: the bundles' streaks (squared: thin bands), not the regime's blob, and thinner —
    // cirrus from space is a translucent veil the ground shows through.
    ci = mix(ci, regime * bundle * bundle * 0.45, far);
    cs *= mix(1.0, mix(0.35, 1.0, bundle), far);
    cu = mix(cu, regime * 0.25 * bundle, far);
    float prof = smoothstep(0.0, 0.25, z) * (1.0 - smoothstep(0.55, 1.0, z));
    float d    = prof * mix(mix(ci, cs, strat), cu, ccK);
    hfH  = z;
    topH = zb + thick;
    return d * mix(mix(0.0008, 0.0003, strat), 0.0016, ccK) * cv2.high2.x * mix(1.0, 0.6, far);
}

// ── The cumulonimbus anvil ───────────────────────────────────────────────────────────────────────
// Deep convection's outflow spreads under the tropopause into a flat, smooth-topped ice shield tens
// to hundreds of km across, carried downwind of the towers that feed it; the strongest towers
// overshoot it in domes (the Cb column reaches ~250 m above the lid). From orbit that shield IS the
// storm: without it storms were piles of towers, "mountains". Where: the weather cube read coarse
// (~30 km texels), a little upwind, so it spans the storm region rather than each tower. Ice: a
// lower extinction than the water cloud below; the lid is sharp, the underside lumpy, the outline
// frayed by the cluster Perlin.
// -- Cumulonimbus COLUMNS (pass 15) ---------------------------------------------------------------
// A storm is three layers: the cumulus field around it (the low layer, capped at column.w), discrete
// towers rising from that field's base to the tropopause (this), and the anvil shield they feed
// (cv2AnvilSigma, which hangs lower around each tower's head). A margin field thresholded per height
// (the low layer) cannot be concave - a column narrowing to a waist and flaring again - so the Cb
// grew as a mountain sloping up into the anvil; a column with its own axis and radius profile can.
// Columns stand on a jittered 3D lattice (cells of column.y metres) sliced by the sea-level sphere:
// each cell holds one candidate centre, kept where it lies within 0.45 of a cell of the sphere, and
// present where the weather at the centre is cumulonimbus and covered. Jitter within the middle half
// of a cell keeps the 2 x 2 x 2 neighbourhood exact out to 0.75 of a cell.
float cv2SqC(float x) { return x * x; }
uvec3 cv2Pcg3(uvec3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

struct CV2Col {
    float sigma;   // extinction of the column (1/m)
    float hf;      // height fraction within the column
    float topH;    // the column's top (m)
    float near;    // 0..1: under a full-height column's head (the anvil thickens and hangs lower there)
    float storm;   // 0..1: the storm strength here (~40 km smooth); the mid layer gives way to it
};

CV2Col cv2ColumnSigma(CV2Pos q, float detailAmt, float fpM)
{
    CV2Col o; o.sigma = 0.0; o.hf = 0.0; o.topH = 0.0; o.near = 0.0; o.storm = 0.0;
    if (cv2.column.x <= 0.0 || q.h < 400.0) return o;
    vec3  wd    = cv2Drift(q.dirE);
    float tropo = cv2Tropo(wd);
    float top   = cv2.types[4].alt.x + (cv2.types[4].alt.y - cv2.types[4].alt.x) * tropo;
    if (q.h > top - 50.0 + cv2.anvil2.w) return o;
    // Coarse gate: no cumulonimbus within ~20 km (weather mip 2), no columns.
    vec3 sP    = cv2Drift(q.seaProjE);
    vec3 flowD = cv2FlowDisp(wd, sP);
    vec3 wdF   = cv2FlowWeatherDir(wd, flowD);
    // The storm strength here, ~40 km smooth (mip 3): one read per sample. It varies slowly across a
    // tower, so a column fades as a whole at a storm's edge. (Reading it at each candidate centre cost
    // up to eight scattered cube fetches per sample; storm views went 10 -> 29 ms.)
    vec4  wk  = cv2WeatherBilinear(wdF, 3.0);
    if (wk.g < 0.55) return o;
    float ck  = clamp((wk.r * cv2.look.x - cv2.cover.x) / max(cv2.cover.y - cv2.cover.x, 1e-3), 0.0, 1.0);
    float strW = smoothstep(0.6, 0.8, wk.g) * smoothstep(0.15, 0.45, ck);
    o.storm = strW;
    if (strW <= 0.02) return o;

    float Rb     = cv2.column.z;
    // The lattice cell is at least a tower's full reach / 1.2, so the 3x3 search covers every part of
    // every tower. "Cb spacing" is a minimum: 12 km towers flared 2.6x reach ~55 km, and on the user's
    // 20 km cells everything past 1.22 cells (25 km) was dropped — heads and the anvil's hang cut off
    // on arcs around each candidate (user snap 1, pass 17). Uniforms only (the tropopause's maximum
    // span, not the local one): a cell that varied with position would scramble the lattice.
    float reachFull = Rb * 1.3 * max(cv2.column2.y, 1.0) + cv2.column2.z + Rb * 0.6
                    + 0.25 * cv2.form.y * max(cv2.types[4].alt.y - cv2.types[4].alt.x, 0.0);
    float cellM  = max(cv2.column.y, reachFull / 1.2);
    vec3  eastW  = vec3(-wd.y, wd.x, 0.0) * inversesqrt(max(dot(wd.xy, wd.xy), 1e-6));
    float lift   = cv2Ground(q.dirE) * 0.9;
    float base   = cv2.types[4].alt.x + lift;
    float hb     = q.h - base;
    if (hb < -300.0) return o;

    // The lattice is 2D, on an equal-angle cube map of the drifted sphere: every candidate lies on the
    // sphere, so a 2x2 (3x3) search does what the 3D lattice's 2x2x2 (3x3x3) did, most of whose
    // candidates were then rejected as off the sphere — and the search runs on every sample of a storm
    // region, the light march's included (towers at near-zero density cost MORE than dense ones: 74 ms).
    // Towers cannot straddle a cube-face edge (a point searches only its own face), so those whose
    // reach crosses one fade out: a tower-free band along the twelve edges, never a cut.
    vec3  aw = abs(wd);
    int   fc = (aw.x >= aw.y && aw.x >= aw.z) ? 0 : ((aw.y >= aw.z) ? 1 : 2);
    vec3  ax = (fc == 0) ? vec3(sign(wd.x), 0.0, 0.0) : ((fc == 1) ? vec3(0.0, sign(wd.y), 0.0) : vec3(0.0, 0.0, sign(wd.z)));
    vec3  e1 = (fc == 0) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3  e2 = (fc == 2) ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
    float mj = dot(wd, ax);
    float cellA = cellM / R_EARTH;                      // a lattice cell, in radians of the face's angles
    vec2  u  = vec2(atan(dot(wd, e1) / mj), atan(dot(wd, e2) / mj)) / cellA;
    vec2  cf = floor(u), fr = u - cf;
    ivec2 c0 = ivec2(cf);
    ivec2 dn = ivec2(fr.x < 0.5 ? -1 : 1, fr.y < 0.5 ? -1 : 1);
    uint  faceId = uint(fc * 2) + ((dot(wd, ax) < 0.0) ? 1u : 0u);
    // How far from its axis a tower (and the anvil hang it drives) reaches: its widest head, blown
    // downwind, lobes and lean. The 2x2 search is exact to 0.75 of a cell; past that, 3x3 (exact to
    // 1.25). With 12 km towers and a 2.6x flare the heads reached ~45 km on 36 km cells and were cut
    // along the lattice's cell faces (user snaps 1 and 3, pass 15): straight seams through storms.
    float reachM = Rb * 1.3 * max(cv2.column2.y, 1.0) + cv2.column2.z + Rb * 0.6
                 + 0.25 * cv2.form.y * max(top - base, 0.0);
    float lidB   = top - 250.0;
    // Below mid-height only the body (base radius, lean, lobes) can reach a point: no flared head,
    // no head drift, no anvil hang there — the 8-cell search always suffices.
    if (q.h < base + 0.5 * (lidB - base))
        reachM = Rb * 1.3 * 1.6 + 0.25 * cv2.form.y * max(top - base, 0.0);
    bool  wide   = reachM > 0.72 * cellM;
    float limM   = wide ? 1.22 * cellM : 0.72 * cellM;   // anything beyond is not searched: taper to 0
    int   nK     = wide ? 9 : 4;
    float lid    = top - 250.0;                          // the anvil's lid (cv2AnvilSigma)
    float hc     = lid - 1800.0;                         // a head rounds off from here to 300 m under the lid
    float osM    = cv2.anvil2.w;                         // overshooting top above the lid (m)
    bool  inAnvil = q.h > lid - cv2.anvil2.x * 1.2 - 600.0 - cv2.anvil2.y;   // where the hang can reach
    float best = -1e9, bestZeta = 0.0, bestTop = 0.0, bestCap = 0.0, bestRc = 1.0;
    float near = 0.0;
    for (int k = 0; k < nK; ++k) {
        ivec2 off = wide ? ivec2(k % 3 - 1, k / 3 - 1)
                         : ivec2((k & 1) != 0 ? dn.x : 0, (k & 2) != 0 ? dn.y : 0);
        ivec2 ci = c0 + off;
        uvec3 hv = cv2Pcg3(uvec3(uvec2(ci + 8192), faceId));
        vec2  ca = (vec2(ci) + 0.25 + 0.5 * vec2(hv.xy & 0xFFFFu) / 65535.0) * cellA;   // the centre's angles
        float edgeM = (0.7853982 - max(abs(ca.x), abs(ca.y))) * R_EARTH;              // to the face's edge
        if (edgeM < limM) continue;
        vec3  cDir = normalize(ax + tan(ca.x) * e1 + tan(ca.y) * e2);
        vec3  dv = (wd - cDir) * R_EARTH;                // this point minus the centre, on the sphere (m)
        vec3  dh = dv - wd * dot(dv, wd);                // horizontal offset from the axis at the base
        float hS = float(hv.x >> 16u) / 65535.0;         // size
        float hP = float(hv.z & 0xFFFFu) / 65535.0;      // presence
        float hO = float(hv.y >> 16u) / 65535.0;         // overshoot
        float dhL = length(dh);
        if (dhL > limM) continue;
        float str = strW * smoothstep(0.0, 0.25, strW - cv2.anvil2.z * hP)   // sparser at a storm's edge
                  * smoothstep(limM, 1.3 * limM, edgeM);                            // the face-edge band
        if (str <= 0.02) continue;
        // A weaker tower is SHORTER, not thinner: its top sinks from under the anvil (full strength)
        // to its base (at the 0.02 cutoff), so at a storm's edge towers settle into the cumulus as
        // short, wide mounds. Floored at 0.7 of its radius at full height it vanished at the cutoff (a
        // flat crescent wall, pass 18); shrunk in radius instead it stood as a thin straw (user, pass 19).
        // Only the ones reaching the anvil get the waist, the flared head and the dome.
        float hK   = smoothstep(0.02, 0.6, str);
        float topC = base + (lid - 300.0 - base) * hK;   // this tower's top
        float full = smoothstep(0.8, 1.0, hK);
        if (q.h > ((full > 0.0) ? lid + osM + 200.0 : topC + 200.0)) continue;
        float Rc   = Rb * mix(0.7, 1.3, hS) * mix(0.8, 1.0, smoothstep(0.1, 0.6, str));
        float fl   = smoothstep(0.1, 0.35, str) * full;  // only anvil-reaching towers flare
        float zeta = hb / max(topC - base, 300.0);
        // Lean downwind with height, and the head blown further downwind under the lid.
        // (Head drift for anvil-reaching towers only: a short tower's zeta passes 0.55 below the
        // search's mid-height switch, and its drifted top was cut along a horizontal seam, pass 20.)
        vec3  dhz  = dh - eastW * (0.25 * max(hb, 0.0) * cv2.form.y + cv2.column2.z * smoothstep(0.55, 1.0, zeta) * full);
        float d    = length(dhz);
        // The profile: a wide flat base (the inflow), a waist through the middle, a head flaring under
        // the lid that closes over INSIDE the anvil (by hc + 500 m), and a small overshooting dome over
        // the core. The head used to close over the last 5% of the span, right at the lid: a plate tens
        // of km wide and ~500 m high standing above the anvil — flat enough that the march steps drew
        // contour rings on it, and from orbit every tower was its own dome on the shield (user snaps 3-4).
        float rW   = Rc * mix(1.0, cv2.column2.x, full);
        float r    = mix(Rc, rW, smoothstep(0.03, 0.5, zeta));
        r += (Rc * cv2.column2.y - rW) * smoothstep(0.62, 0.94, zeta) * fl;
        // The head rounds off over 1.5 km and stays 300 m inside the anvil: only the anvil's own lid,
        // lit as a deck, shows from above. A head closing over 500 m at the lid stood proud of it as a
        // flat plate tens of km wide: contour rings from the march steps, a moat around it, and dark
        // from orbit (a grazing Sun's light-march ray skims through a plate; user snaps 1, 3 and 4).
        float rnd  = min(1500.0, 0.6 * max(topC - base, 1.0));   // the top rounds off over this
        float zh   = (q.h - (topC - rnd)) / rnd;
        r *= sqrt(max(1.0 - cv2SqC(max(zh, 0.0)), 0.0));
        // Only the stronger cores overshoot (about a third), each to its own height: with every tower
        // doming through, the shield from orbit was an even grid of dots (user snap 4, pass 16).
        float osS  = smoothstep(0.6, 0.9, hO) * mix(0.5, 1.0, smoothstep(0.3, 0.8, str));
        float osT  = osM * osS;
        // The dome's apex: 400 m inside the anvil unless this tower overshoots. It sat exactly AT the
        // lid for every tower, so from above each one showed a dense 4-5 km disc in the anvil's soft
        // lid, on the lattice (user snap 3, pass 17).
        float apex = mix(lid - 300.0, lid + osT, smoothstep(0.0, 0.3, osS));
        float zo   = (q.h - (apex - 1000.0)) / 1000.0;
        float rOs  = 0.35 * Rc * sqrt(max(1.0 - zo * zo, 0.0)) * step(0.0, zo) * full;
        r = max(r, rOs);
        r *= 0.9 + 0.1 * smoothstep(0.0, 0.04, zeta);       // the base's rim curls up
        float cap  = smoothstep(0.0, 1.0, zo);
        float mC   = (r - d) / Rc;
        // ITS top over THIS point (the dome's surface here, else the head's): with the apex, every
        // anvil sample in a dome's footprint was shaded as if under the whole dome (dark craters with
        // bright rims from orbit, user snap 3, pass 17; lid + osM for every tower made dark pits).
        float topL = max(topC, (full > 0.0) ? (apex - 1000.0) + 1000.0 * sqrt(max(1.0 - cv2SqC(d / max(0.35 * Rc, 1.0)), 0.0)) : 0.0);
        if (mC > best) { best = mC; bestZeta = zeta; bestTop = topL; bestCap = cap; bestRc = Rc; }   // ITS top: lid + osM shaded every tower's patch of anvil as if under 800 m more cloud (dark pits)
        // Under the head: the anvil thickens toward the column (a wide, downwind-stretched footprint),
        // tapered to zero before the search's limit (or it too would be cut along the lattice).
        if (inAnvil) {
            vec3  dn2  = dh - eastW * (cv2.column2.z + 4000.0);
            float al   = dot(dn2, eastW);
            float ac   = length(dn2 - eastW * al);
            // Sized to fall to ~1% by the search's limit: at 4 radii along the wind it was ~48 km for
            // a 12 km tower, and a 3.6 km hang dropped to nothing over the last few km before limM —
            // a cliff in the anvil's underside (user snap 1, pass 17).
            float sa2  = cv2SqC(min(4.0 * Rc, max(limM - cv2.column2.z - 4000.0, 0.3 * limM) / 2.2));
            float sc2  = cv2SqC(min(2.0 * Rc, limM / 2.2));
            near = max(near, fl * str * exp(-(al * al) / sa2 - (ac * ac) / sc2)
                             * (1.0 - smoothstep(0.7 * limM, limM, dhL)));
        }
    }
    o.near = near;
    float fpFade = mix(1.0, 0.3, smoothstep(300.0, 2000.0, fpM));
    // Cauliflower all the way up; flatter at the base (the condensation level).
    // A head's top under the anvil is smooth: its lobes and erosion fade out over its last ~1.2 km. Under
    // the anvil's thin lid (300 m) the lumps and the cracks between them showed through as a cell
    // pattern over the whole shield (user snap 3, pass 17). Sinking the heads 900 m hid them too, but
    // rays then crossed 600 m more anvil before stopping: +15 ms from above.
    float headTop = smoothstep(lid - 1500.0, lid - 500.0, q.h) * (1.0 - bestCap);
    float A  = cv2.column2.w * fpFade * mix(0.25, 1.0, smoothstep(0.0, 0.12, bestZeta)) * (1.0 - 0.75 * bestCap)
             * (1.0 - 0.85 * headTop);
    // Before the fetches: the lobes add at most ~0.75 x 2A in practice (the low layer's bound). A fixed
    // -0.6 fetched the storm noise through a shell 0.6 radii deep around every tower (7 km at 12 km).
    if (best + A * 1.5 <= 0.0) return o;
    vec4  s  = cv2ShapeSmooth(cv2.anchorStorm.xyz + cv2Drift(q.rSeaE) * cv2.anchorStorm.w,
                              cv2Lod(fpM, cv2.anchorStorm.w));
    float lobes = (s.g * 0.6 + s.b * 0.25 + s.a * 0.15) - 0.47;
    float m  = best + A * lobes * 2.0;
    // The surface ramps in over ~300 m whatever the tower's size (x the radius, 0.2 of it: 2.4 km of
    // thin fog around a 12 km tower, which every ray marched through lit sample by sample before it
    // turned opaque — the towers cost ~20 ms inside a storm, and at any density).
    float Ph = clamp(m * bestRc / 300.0, 0.0, 1.0) * smoothstep(0.0, 120.0, hb + 150.0 * lobes);
    if (Ph <= 0.0) return o;
    float d  = Ph * mix(0.75, 1.0, s.r);
    if (detailAmt >= 0.0) {
        float df = 0.45;
#ifdef CV2_DETAIL_BINDING
        if (detailAmt > 0.0) {
            vec4 dd = textureLod(cv2DetailTex, cv2.anchorStormDetail.xyz + cv2Drift(q.rSeaE) * cv2.anchorStormDetail.w,
                                 cv2Lod(fpM, cv2.anchorStormDetail.w));
            df = mix(0.45, dd.r * 0.75 + dd.g * 0.25, detailAmt);
        }
#endif
        // Bite the cracks between the lumps, on the surface (deep inside, form.z of it).
        float erode = (1.0 - df) * 0.5 * cv2.look.z * cv2.storm.y * mix(1.0, cv2.form.z, smoothstep(0.35, 0.85, d))
                    * (1.0 - headTop);
        d = clamp((d - erode) / max(1.0 - erode, 1e-3), 0.0, 1.0);
        if (d <= 0.0) return o;
    }
    d = min(d * mix(cv2.extra.x, 1.0, smoothstep(150.0, 1500.0, fpM)), 1.0);
    o.hf    = clamp(bestZeta, 0.0, 1.0);
    o.topH  = bestTop;
    o.sigma = d * mix(0.6, 1.0, smoothstep(0.0, 0.3, o.hf)) * cv2.types[4].shape.x * cv2.look.y * cv2.column.x;
    return o;
}

// presA: 0..1, how much anvil shield this column holds (its outline strength, height-independent):
// the high layer gives way to it (the anvil IS the high cloud around a storm).
float cv2AnvilSigmaP(CV2Pos q, float fpM, float colNear, bool wantPres, out float hfA, out float topA, out float presA)
{
    hfA = 0.0; topA = 0.0; presA = 0.0;
    if (cv2.storm.z <= 0.0 || q.h < 5000.0) return 0.0;
    vec3  wd     = cv2Drift(q.dirE);
    float top    = cv2.types[4].alt.x + (cv2.types[4].alt.y - cv2.types[4].alt.x) * cv2Tropo(wd) - 250.0;
    // Around a column's head the shield hangs lower (the mushroom's underside): up to 1.2 km more.
    // (3 km, and the 3 km shield, swallowed the towers: from the side a shield on short stubs.)
    float hang   = cv2.anvil2.y * colNear;
    // The shield's band; below it, down through the cirrus layer's band, only its PRESENCE is wanted
    // (the high layer gives way under an anvil, cv2Field).
    bool  inBand = q.h <= top && q.h >= top - cv2.anvil2.x * 1.2 - 600.0 - hang;
    if (q.h > top || (!inBand && (!wantPres || q.h < 0.66 * (top + 250.0)))) return 0.0;
    // Fed by the towers: the weather read at ~10 km (mip 1), where it is cumulonimbus and well covered,
    // here and 12 / 25 / 40 km upwind (the outflow is blown downwind, so a point downwind of a core is
    // under its anvil). The first cut read one 30 km-blurred texel 25 km upwind: only a storm region
    // hundreds of km across got an anvil (typically one on Earth), and it did not sit on its towers.
    vec3  eastW  = vec3(-wd.y, wd.x, 0.0) * inversesqrt(max(dot(wd.xy, wd.xy), 1e-6));
    float span   = max(cv2.cover.y - cv2.cover.x, 1e-3);
    float stormA = 0.0;
    vec3  wF     = cv2FlowWeatherDirAt(q);                    // the towers' (flowed) map
    for (int k = 0; k < 4; ++k) {
        float off  = (k == 0) ? 0.0 : (k == 1) ? 12000.0 : (k == 2) ? 25000.0 : 40000.0;
        float fall = (k == 0) ? 1.0 : (k == 1) ? 0.95 : (k == 2) ? 0.85 : 0.65;
        vec4  wk   = textureLod(cv2WeatherTex, wF - eastW * (off / R_EARTH), 1.0);
        float ck   = clamp((wk.r * cv2.look.x - cv2.cover.x) / span, 0.0, 1.0);
        stormA = max(stormA, smoothstep(0.72, 0.9, wk.g) * smoothstep(0.3, 0.7, ck) * fall);
    }
    stormA = max(stormA, colNear);
    if (stormA <= 0.0) return 0.0;
    vec4  cl     = textureLod(cv2MesoTex, cv2.anchorCluster.xyz + cv2Drift(q.seaProjE) * cv2.anchorCluster.w,
                              cv2Lod(fpM, cv2.anchorCluster.w));
    float a0     = stormA + (cl.a - 0.5) * 0.7;
    if (a0 <= -0.15) return 0.0;                        // the finer fray below adds at most ~0.2
    vec4  sh     = cv2ShapeSmooth(cv2.anchorStorm.xyz + cv2Drift(q.rSeaE) * cv2.anchorStorm.w,
                                  cv2Lod(fpM, cv2.anchorStorm.w));
    // The outline frayed at the storm-lobe scale too: on the cluster Perlin alone it was a smooth
    // ellipse, one weather texel's blur.
    float a      = a0 + (sh.b - 0.45) * 0.45;
    presA = smoothstep(0.05, 0.35, a);
    if (a <= 0.05 || !inBand) return 0.0;
    float thick  = cv2.anvil2.x * clamp(a, 0.0, 1.0) + hang; // thickest over the storm, thin at its edge
    float za     = (q.h - (top - thick)) / thick;
    if (za <= -0.2) return 0.0;
    float under  = smoothstep(0.0, 0.35, za + (sh.g - 0.45) * 0.8);
    // The lid is flat but not blank: low domes of the storm-lobe scale, highest over the core
    // (overshooting tops), ~ +-8% of the thickness.
    float lid    = 1.0 - smoothstep(0.93, 1.0, za - (sh.g - 0.45) * 0.3 * a);
    float edge   = presA;
    hfA  = clamp(za, 0.0, 1.0);
    topA = top;
    return edge * under * lid * mix(0.6, 1.0, sh.r) * 0.01 * cv2.storm.z;   // "Anvils" is its density
}

float cv2AnvilSigma(CV2Pos q, float fpM, float colNear, out float hfA, out float topA)
{
    float presA;
    return cv2AnvilSigmaP(q, fpM, colNear, false, hfA, topA, presA);
}

// THE cloud field: the low system (stratus .. cumulonimbus, nimbostratus), the mid layer, the anvils
// and the high layer (cirrus family).
CV2Field cv2Field(CV2Pos q, float detailAmt, float fpM)
{
    gCv2FlowSet = false;
    gCv2FlowD   = cv2FlowDisp(cv2Drift(q.dirE), vec3(0.0));
    gCv2FlowSet = true;
    gCv2GroundSet = false;
    gCv2Ground    = cv2Ground(q.dirE);
    gCv2GroundSet = true;
    CV2Field f = cv2FieldLow(q, detailAmt, fpM);
    CV2Col col = cv2ColumnSigma(q, detailAmt, fpM);
    float hfX, topX, deckX;
    float sm = cv2MidSigma(q, fpM, hfX, topX, deckX);
    // The mid layer (a thin Ac/As lens) gives way over a storm: drawn through it, it sliced every
    // tower with a flat plate at ~6 km, seen edge-on as a hard line across the storm (user snap 1,
    // pass 17). Only where the towers are drawn (the storm's strength is read for them).
    // Wherever towers can stand (a weak storm-edge tower cut by a mid-layer belt, pass 20).
    if (cv2.column.x > 0.0) sm *= 1.0 - smoothstep(0.0, 0.15, col.storm);
    if (sm > 0.0) cv2Add(f, sm, hfX, topX, deckX);
    float hfH, topHh;
    float sh = cv2HighSigma(q, fpM, hfH, topHh);
    float presA;
    float sa = cv2AnvilSigmaP(q, fpM, col.near, false, hfX, topX, presA);
    if (col.sigma > 0.0) cv2Add(f, col.sigma, col.hf, col.topH, 0.0);
    if (sh > 0.0 && cv2.storm.z > 0.0) {
        // Where the storms that feed anvils are (the anvil's own test, one ~20 km read): running the
        // anvil's evaluation through the whole cirrus band cost ~10 ms from orbit.
        vec4  wa = textureLod(cv2WeatherTex, cv2FlowWeatherDirAt(q), 2.0);
        float ca = clamp((wa.r * cv2.look.x - cv2.cover.x) / max(cv2.cover.y - cv2.cover.x, 1e-3), 0.0, 1.0);
        presA = max(presA, smoothstep(0.65, 0.85, wa.g) * smoothstep(0.25, 0.6, ca));
    }
    if (sa > 0.0) cv2Add(f, sa, hfX, topX, 1.0);
    // The anvil is the high cloud around a storm: the cirrus layer (0.69-0.93 of the tropopause, the
    // anvil ~0.7-0.98) gives way to it, and continues outward from its edge. Both drawn, cirrus sheets
    // cut straight through the anvils' sides (user snap 3, pass 15).
    sh *= 1.0 - presA;
    hfX = hfH; topX = topHh;
    if (sh > 0.0) {
        cv2Add(f, sh, hfX, topX, 0.0);
        f.thin = sh / f.sigma;
    }
    gCv2FlowSet = false;
    gCv2GroundSet = false;
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

// Crystal habits, per region (the cluster field, tens of km): which ice optics a patch of cirrus can
// show at all. A halo needs crystals of the right shape; sundogs and the arcs need plates falling
// flat, which calm air allows and turbulence scrambles. Real skies show a 22 degree halo in maybe a
// third of cirrostratus, sundogs less often, the circumzenithal arc and parhelic circle rarely and the
// 46 degree halo very rarely. The first cut drew every arc on every cirrus, which made the rare
// look commonplace. x = 22 deg halo, y = sundogs (flat plates), z = circumzenithal arc + parhelic
// circle (very well aligned plates), w = 46 deg halo. Evaluated once per RAY (the march caches it).
vec4 cv2IceHabit(CV2Pos q)
{
    vec3  m = cv2.anchorCluster.xyz + cv2Drift(q.seaProjE) * cv2.anchorCluster.w;
    float a = textureLod(cv2MesoTex, m + vec3(0.73, 0.29, 0.11), 1.0).a;
    float b = textureLod(cv2MesoTex, m + vec3(0.17, 0.83, 0.47), 1.0).a;
    float c = textureLod(cv2MesoTex, m * 2.0 + vec3(0.61, 0.07, 0.35), 1.0).a;
    // fBm 0.50 +- 0.057: P(> 0.53) ~ 30%, P(> 0.565) ~ 13%, P(> 0.6) ~ 4%
    float halo = smoothstep(0.51, 0.55, a) * mix(0.35, 1.0, smoothstep(0.44, 0.56, c));
    float dogs = smoothstep(0.545, 0.585, b) * mix(0.5, 1.0, smoothstep(0.44, 0.56, c));
    float arcs = smoothstep(0.545, 0.585, b) * smoothstep(0.53, 0.58, c);
    float h46  = smoothstep(0.585, 0.615, a);
    return vec4(halo, dogs, arcs, h46);
}

// Ice crystals: the 22 and 46 degree halos (randomly oriented hexagonal prisms: minimum deviation of
// the 60 and 90 degree prisms), and from plates falling flat the sundogs (at the light's elevation,
// through the effective index n' = sqrt(n^2 - sin^2 e) / cos e — 22 deg out at the horizon, 36 at
// 40 deg, none above ~61), the parhelic circle and the circumzenithal arc (only below 32 deg).
// hab: cv2IceHabit's weights for this region.
vec3 cv2IceOptics(vec3 v, vec3 s, vec4 hab)
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
    return 4.0 * hab.x * h22 + 0.7 * hab.w * h46 + up * (12.0 * hab.y * dogs + hab.z * (vec3(circle) + 3.0 * cza));
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
