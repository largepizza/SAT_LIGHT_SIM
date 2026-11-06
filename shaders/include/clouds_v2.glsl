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

const float kCv2ColFineM  = 500.0;   // the Cb towers' fine lobes: their displacement of the wall (m) at "Cb tower fine lobes" 1
const float kCv2LobeLiftM = 900.0;   // the lobes' lift of a convective cloud's top (m per unit of lobe strength)
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
                          // the evolution the resolve cannot reproject; y the new-sample weight in motion ("History weight moving"), z "Cb head lobes",
                          // w the march pixel / the 100%-render-scale one (the noise LOD footprint scale)
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
    vec4  anchorCol;      // the Cb towers' lobe noise (2026-10-10): xyz the storm shape anchor WITHOUT the wind's
                          // slide (period = anchorStorm.w), w the phase 0..1 of the towers' flow (cv2ColumnSigma)
    vec4  column;         // x Cb column amount (0 = no towers: a storm is tall congestus), y lattice
                          // cell (m), z column base radius (m), w the low layer's top in storm regions (m)
    vec4  column2;        // x waist (x the base radius), y head flare (x the base radius), z head drift
                          // downwind (m), w lobe strength (fraction of the radius)
    vec4  anvil2;         // x anvil thickness (m), y anvil hang around a tower's head (m), z tower
                          // sparsity (0 = every candidate in a storm, 1 = only the strongest cores),
                          // w overshooting top: how far a tower's dome rises above the anvil lid (m)
    vec4  column3;        // x weather mip for the storm cumulus reach (log2 of reach / 5 km texels),
                          // y light LOD footprint (m, 0 = off; cloud_v2_march.comp), z the storm cumulus
                          // top's regional variation (+- fraction), w Cb fill (share of lattice cells with a tower)
    vec4  lightning;      // x flashes per minute of a full-strength tower (0 = off), y the glow in the
                          // cloud, z the cloud-to-ground channels, w sim time (s, wrapped at 1e5)
    vec4  lightVol;       // the light volume (cloud_v2_lightvol.comp): x the first level baked this frame,
                          // y its half-extent (m), z its top (m), w the godrays' strength (0 = off)
    vec4  misc2;          // x red-sprite chance per ground stroke (0 = none), y sprite brightness,
                          // z 1 = the eye moved too far this frame for history (decorrelated ray jitter), w free
    vec4  fog;            // fog + dust (cv2FogDust): x fog amount (0 = none), y fog depth (m above the
                          // smoothed ground), z fog extinction (1/m), w dust amount (0 = none)
    vec4  fog2;           // x dust scale height (m), y dust extinction (1/m), z debug view 11 (the resolve's
                          // adaptive-tile overlay; the march then sees view 0), w free
    vec4  sunE;           // xyz the Sun's direction, Earth-fixed (fog's burn-off), w free
    vec4  rainE, rainN, rainU;  // the rain lattice's frame: xyz its axes (ECEF), w the eye in it (m, mod 1024)
    vec4  precip;         // x the air temperature at the eye (deg C): rain / sleet / snow; y sim time mod 600 s; w the shafts' liquid share (500 m over the ground)
    vec4  anchorMorph;    // review 17: the morphology texture's anchor (xyz frac(anchor / period), w 1 / period)
    vec4  morph;          // x its share of the placement (vs the cluster Perlin), y "Imagery share" (review 19), z "Foveated full rate (radius)" (review 22, cloud_v2_tiles.comp), w 2^-"Far-field sharpness"
    vec4  farLight;       // review 18: the far cloud layer's key-light and sky-light gains (cloud_v2_far.comp);
                          // zw review 22: "Cumulus lobes (bottom / top)", replacing form.x on convective types
    vec4  farTune;        // review 24: the far layer's x slant coverage, y coverage bias (field units), z density, w edge softness
    vec4  farTune2;       // review 24: x the far layer's low-Sun light (0 = a flat slab's mu0); yzw the mid layer's
                          // morphology anchor, frac(anchor x 1.7) reduced on the CPU
    // 2026-10-04: the low cloud's shape volume is read in a ROTATED frame chosen at the observer
    // (SatelliteSimCloudsV2.cpp): shapeRotA = its columns (drifted ECEF -> texture); the anchors in that frame.
    vec4  shapeRotA[3];
    vec4  anchorShapeA, anchorStormA;
    // 2026-10-05, the rain particles' motion, integrated on the CPU over sim time (SatelliteSimRain.cpp): rainMotion
    // x the fall phase (the integral of the reference fall speed / 64, m, mod 1024), y the reference fall speed now
    // (x a drop's terminal speed), zw the rain's wind drift (rain frame E/N, m, mod 1024); rainWind xy the wind now
    // (m/s), zw the snow's wind drift (x "Snow wind", m, mod 1024).
    vec4  rainMotion, rainWind;
    // 2026-10-05: lightning from whole storms. x "Storm lightning (flashes/min/cell)" (cells of ~20 km); yzw the
    // storm DETAIL anchor without the wind's slide (2026-10-10, the towers' erosion; period = anchorStormDetail.w).
    vec4  lightning2;
    // 2026-10-06: the Reflect beams' light at the eye (CPU, SatelliteSim.cpp), for the rain / snow particles: beamEye x the
    // irradiance (Suns) x "Beam light on cloud", y 1 - dayness; beamEyeDir xyz the direction to the satellites (ENU).
    vec4  beamEye, beamEyeDir;
    // 2026-10-06: the snow's own fall: x its phase (as rainMotion.x), y its reference speed now (flakes are not "Rain fall speed").
    vec4  snowMotion;
    // 2026-10-11. tune: x "Small cumulus" (the near field's threshold bias, field units), y "Erosion variation",
    // z "Lobe variation", w "Cirrus direction warp" (the streaks' slope). mid2: x altostratus density (x),
    // y the mid layer's thickness (x), z the share of its extinction left OUT of the shadow it casts
    // (1 - "Mid layer shadow"), w its regime bias (noise units). mid3: x 1 / its field size (m^-1; 0 = the cluster
    // field), y the multiple-scattering octaves (march), z "Lobe occlusion", w "Haze near clouds" (the sky pass's humid air). samp (cloud_v2_resolve.comp): x the accumulation's cap (samples; 0 = off), y new-pixel
    // smoothing, z the motion clamp (sd), w spare.
    vec4  tune, mid2, mid3, samp;
    // 2026-10-13 (issue 20261012_235712), the cumulus shape. cu: x "Cumulus height variation", y "Cumulus break-up"
    // (the lobes' strength at the base), z "Cumulus top headroom", w "Storm cumulus fine lobes". cu2: x "Cb tower fine
    // lobes", y "Cumulus wisps", z "Silver lining" (march), w spare.
    vec4  cu, cu2;
    // The cloud density clipmap's plane (include/cv2_cdc.glsl): layA / layE / layN the anchor's axes (observer ENU; it was the
    // layer shadow map's anchor, 2026-10-14, deleted 2026-10-22 with the other Sun shadow structures for the STV). w spare.
    vec4  layA, layE, layN;
    // 2026-10-21, the cloud density clipmap (EXPERIMENT, include/cv2_cdc.glsl): cdcA xy the eye on the layer map's plane (m),
    // z level 0's cell (m), w 1 = baked and used by the march's coarse steps ("cdc_skip"); cdcB x the epoch, y the bake's read footprint
    // (x the cell), z the bake budget (cells a frame), w the levels the march may use; cdcP0 / cdcP1 each level's sim-time phase (sim time / its period, mod 4096).
    vec4  cdcA, cdcB, cdcP0, cdcP1;
    // 2026-10-22, the Mie phase (tools/make_mie_lut.py, march binding 24): x its share of the single scattering ("Mie
    // phase", 0 = the two-lobe Henyey-Greenstein), y the table row for the droplets' effective radius (v coordinate),
    // z "Mie optics" (the tuned phase x the table's structure), w that radius's structure row.
    vec4  mie;
    // 2026-10-22, the god rays' froxel volume (cloud_v2_froxel.comp, EXPERIMENT "godray_froxels"): x 1 = filled and read by
    // the sky pass, y the first slice's distance (m), z the log ratio per slice.
    vec4  frox;
    // 2026-10-22, S2: x 1 = the light march reads the clipmap's mean light extinction beyond its first two steps, y on the
    // levels below this ("cdc_light_levels": a cell-mean is unbiased only where cells are smaller than the cloud structure);
    // z: coarse steps skip only beyond this distance from the eye (m, "cdc_near_m": the coarse field's ~100-m lobes near the
    // eye are finer than level 0's cells).
    vec4  cdcC;
    // 2026-10-22, S9 (full-resolution clouds): xy the cloud grid (the v2 march / resolve images; half the render extent,
    // or all of it with "Full-resolution clouds"), z log2(cloud grid / scene depth) (0 or 1), w 1 = full-res clouds on
    // (cloud_march.comp composites at half res and writes the derivatives cloud_v2_combine.comp needs).
    vec4  grid;
    // 2026-10-22, the anvils' look: x "Anvil puffiness" (0 = the smooth heads of before: lobes and erosion kept on the head,
    // its lobes stronger, a lift of its top, a softer edge), y the lift (m), zw spare.
    vec4  anvil3;
    // 2026-10-22, S3: the Sun transmittance volume (cloud_lightning.glsl cv2Stv). stvL0..3: xy the eye on level n's lattice
    // (cells, mod 1024), z its cell (m), w its fill weight; stvH: x / y the knots' lowest / highest height (m), z 1 = on (the
    // march's key light, the god rays, the fog and the ground shadow read it), w spare;
    // stvV: xy the eye's motion this frame across the Sun in level 0's cells (the bake takes the leading edge), zw spare.
    vec4  stvL0, stvL1, stvL2, stvL3, stvH, stvV;
    // 2026-10-22, the terrain Sun-horizon map (cloud_lightning.glsl cv2TerrSun, cloud_v2_terrsun.comp): tsh0..3 = level n:
    // xy the eye's fractional position on the level's Earth-fixed lattice across the Sun (cells, 0..1), z its cell (m),
    // w 1 = on (the god rays' froxels, the sky pass's direct path and the fog/dust march read it; not the ground).
    vec4  tsh0, tsh1, tsh2, tsh3;
    // 2026-10-24, the perf pass: x "Near cirrus stride" (1 = off: a pure thin-ice sample within y of footprint stands for
    // x steps; the high layer is a soft haze there), y that footprint (m); z (perf pass 3) "God ray reads": the sky pass's
    // Sun shadow reads a step within 30 km (half that beyond; 8 as built), w spare.
    vec4  perf;
    // 2026-10-25: the aerial-perspective volume (include/sky_ap.glsl, sky_ap.comp): x 1 = it ran this frame (the sky pass
    // reads it), y its first slice depth d0 (m), z the horizon band where the sky pass keeps its own loop (sin), w spare
    vec4  skyAp;
    // 2026-10-26: multiple scattering: x the model (0 = octaves + powder, 1 = delta-Eddington), y the slab's g,
    // z its gain, w how far the march away from the light reaches (m) (cloud_v2_march.comp cv2DiffuseField)
    vec4  ms;
    // 2026-10-26: the sky's light on the clouds: x the model (0 = the zenith x "Sky ambient" + the twilight terms, 1 = the
    // hemispheres' mean radiance with multiple scattering, cloud_v2_sunlut.comp layers 2-3), y its gain (1 = physical),
    // z the ground albedo in the multiple scattering and under the lower hemisphere, w 1 = the sky pass's own multiple
    // scattering (sat_sky.frag, sky_ap.comp: (sigma_R + sigma_M) x Psi per step, in place of the shadowed air's sky light)
    vec4  amb;
    // 2026-10-26: the sky pass's tonemap: x 1 = AgX (0 = 1 - e^-x with "Highlight roll-off"), y its saturation, z its
    // contrast (the look's power about mid grey), w "AgX range" (light3: x the scene's log contrast about mid grey)
    vec4  tone;
    // 2026-10-26: the ground's light under cloud: x 1 = the cloud over the ground (cv2Vt) dims and greys the sky light and
    // adds the sunlight it scatters down (sat_sky.frag); y (2026-10-26) x the wildfire smoke's Angstrom exponent
    // (smoke.glsl smokeSpecK; "Smoke spectral", 1 = physical); z (2026-11-04) the full-res grid's sparse block K (one ray per
    // K x K a frame, "sparse_block"; 2 elsewhere), w the resolve's depth-consistent history (0 = off, "history_depth_test")
    vec4  gnd;
    // 2026-11-04, the droplet phase (cloud_v2_march.comp cv2Micro*, "Droplet phase"): x "Droplet size" (x the adiabatic
    // effective radius; 0 = off: the tuned phase as before), y the sea-level air temperature under the eye (C: the
    // glaciation by height), z 1 / the Mie table's row count (both halves), w the diffuse field's gain in this model
    vec4  micro;
    // 2026-11-04, storms through the cumulus field (cv2StormGeom, cv2FieldLow; clouds_v2.storm_unified): x 1 = on (the
    // towers and shields are a margin the low field's lobes and erosion carve; 0 = cv2ColumnSigma + cv2AnvilSigmaP),
    // y metres of storm surface per margin unit, z the storm surface's density ramp (m), w the storm lobes' gain
    vec4  stormU;
    // 2026-11-04, cumulus turrets (cv2LowA / cv2FieldLow; issue 20261104_181014 "General Cloud Shapes"): x "Cumulus turrets"
    // (0 = off), y "Turret stretch" (the turret noise runs 1 - y as fast upward: towers taller than wide), z "Turret lean"
    // (x form.y, the cells' lean), w "Turret skew" (their positive excursions grow: few tall towers, many low ones)
    vec4  cu3;
    // 2026-11-04 round 2 (issue 20261104_181014): x "Cumulus body" (the adiabatic water profile: extinction x (1 + x 2
    // (height above the base / 1 km)^(2/3)), 0 = off), y "Cumulus edge" (the surface's sharpening x this; 1 = off),
    // z "Showers as cells" (a convective type's deck shaping gives way to cells where the map rains), w "Showers from
    // cores" (a convective type's map rain falls from the strong cells only)
    vec4  cu4;
    // 2026-11-04 (structure round): x "Cumulus bubbles" (the thermal bubbles' share of a convective cloud's margin, m0 ->
    // (m0 + x B) / (1 + x)), y "Bubble relief" (margin units per unit of bubble distance); 2026-11-05 (shape2, clouds.md
    // "Bubble hierarchy"): z "Bubble hierarchy" (the fine bubbles, 4x smaller, on the big ones' surface: 0 off), w packs
    // floor(w) / 1000 "Bubbles follow cloud size" (0 = one size for all) and fract(w) "Bubble base flatness" (0 = uniform).
    // (Packed, not a new member: with a `vec4 shape2` appended after `shade` the app died silently creating
    // rain_particles.vert's graphics pipeline at boot (NVIDIA 596.19, 2026-11-05); packed, it boots. ciHu / ciHu2 were
    // appended the same night and boot, so the member count alone is not the cause.)
    vec4  cu5;
    // 2026-11-04, cloud contrast (cloud_v2_march.comp; clouds.md "Cloud contrast"): x "Sky light keeps its colour" (the
    // sky's light diffused through cloud keeps its hue: cloud scattering is grey; 0 = greyed with depth as before), y the
    // diffuse field's side escape ("Multi-scatter side escape": e^-y t; 0.05 as fitted 2026-10-26), z "Cumulus tower spread" (envelope,
    // 2026-11-05: each cumulus top x(1 - z) .. x(1 + 0.8 z) by a km-scale field; 0 = off), w spare
    vec4  shade;
    // -- cirrus-hurr (2026-11-05, night wave agent D) -- x "Cirrus fibres" (cv2IceFibres' filament contrast), y "Cirrus hooks"
    // (heads and hooked fall streaks), z "Hurricane top smoothing" (the CDO plate's lobes / erosion removed), w "Hurricane top
    // streaks" (the swirl relief on the CDO's lid, m); ciHu2: x "Hurricane top density" (x the plate's ice), y "Cumulus core", z "Cumulus bubble warp" (m), w "Cumulus head width"
    vec4  ciHu;
    vec4  ciHu2;
    // 2026-11-05 (storm3, storms in the cumulus look; cv2AnvilFibre / cv2FieldLow): x "Anvil fibres" (0 = the eroded plate
    // as before), y the fibres' along-wind count round the equator / 2 pi (their period divides it: no seam), z 1 / their
    // across-wind spacing (m^-1), w "Storm bubbles" (the towers' share of the cumulus bubbles and turret skew; 0 = off)
    vec4  storm3a;
    // x the anvil plate's share of the Cb erosion (1 = all of it), y its lid's lobes (x the storm lobes), z its underside's,
    // w spare
    vec4  storm3b;
    // -- vertical (2026-11-05, night wave agent E): varied cloud bases, fractus, mid-level wisps, orographic clouds --
    // vert: x "Cloud base condensation level" (cv2BaseShift's LCL proxy, x1), y "Cloud base cell spread" (m, per cumulus
    // cell), z "Cloud base regional spread" (x the type's alt.w at the cluster scale), w "Fractus" (ragged scud under wet bases)
    // vert2: x "Mid-level wisps" (the mid layer's low thin mode), yzw their anchor (anchorMid's, the drifted z x0.4)
    // oro0: x "Orographic clouds" (0 = off), y the lee wavelength (m, 2 pi U / N), zw the wind's (east, north) unit vector (the
    // way it blows); oro1: x lens moisture (0..1), y x the dryness (faster wind lifts more), zw the regional moisture phase
    vec4  vert;
    vec4  vert2;
    vec4  oro0;
    vec4  oro1;
    // 2026-11-06: the shape volume's PREVIOUS frame while a change of frame cross-fades (SatelliteSimCloudsV2.cpp, cv2Field):
    // [0..2] xyz its rotation's columns (as shapeRotA), [0].w the share of reads still taking it (0 = no fade); [3] xyz its
    // anchorShape, [4] xyz its anchorStorm
    vec4  shapeB[5];
    // Weather v2 (2026-11-06): wind0 = the wind's state (wind_field.glsl ws); wind1: x sim time mod the noise flow map's
    // window (s), y the window (s), z the phases' blend (fraction each side of a reset), w sim seconds since last frame
    vec4  wind0;
    vec4  wind1;
    vec4  wind2;   // x the cirrus fibres' eastward slide (m/s at the equator; cv2HighSigma's high.z), for the resolve
} cv2;
#include "wind_field.glsl"

#ifndef CV2_PARAMS_ONLY   // the resolve pass needs only the UBO
layout(set = 0, binding = CV2_WEATHER_BINDING) uniform samplerCube cv2WeatherTex;
// The cube's finest level is 2048 (~5 km, 2026-10-25); every other read names the LOGICAL mip of the 1024 cube it was
// written against (1 = ~20 km ... ), which is one level up. Only cv2WeatherSmooth's own fine read uses the 2048 level.
#define CV2_WX_LOD(l) ((l) + 1.0)
layout(set = 0, binding = CV2_SHAPE_BINDING)   uniform sampler3D   cv2ShapeTex;
layout(set = 0, binding = CV2_MESO_BINDING)    uniform sampler3D   cv2MesoTex;
#ifdef CV2_DETAIL_BINDING
layout(set = 0, binding = CV2_DETAIL_BINDING)  uniform sampler3D   cv2DetailTex;
#endif
#ifdef CV2_MORPH_BINDING
layout(set = 0, binding = CV2_MORPH_BINDING)   uniform sampler2D   cv2MorphTex;
#endif
// Weather v2: the convection cube (layers 6-11 of the weather image; cloud_v2_weather.comp wxConvection): x convective
// intensity, y the column's top (share of the way to the lid), z the anvil field, w mid-level outflow. Only the march
// set binds it (28); without it there are no storms (cv2StormGeom).
#ifdef CV2_CONV_BINDING
layout(set = 0, binding = CV2_CONV_BINDING)    uniform samplerCube cv2ConvTex;
#endif
#if !defined(SKY_LITE) && !defined(CV2_CONV_BINDING)   // (Weather v2: no CDO swirl texture; hurricanes are rebuilt)
#define CV2_CYC_TEX   // (cv2_storms.glsl: a cyclone CDO's swirl texture reads the shape volume)
#endif
#include "cv2_storms.glsl"   // the storm objects' towers (CV2_STORM_BINDING)

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
// Weather v2 (2026-11-06): there is no drift. The map frame IS the Earth-fixed frame; the clouds move with the wind
// (cv2NoiseDisp below). Kept as the identity so every former map-frame read stays where it was written.
vec3 cv2Drift(vec3 v) { return v; }
vec3 cv2WeatherDir(vec3 d) { return cv2Drift(d); }

// ── Weather v2: the cloud TEXTURE rides the wind (2026-11-06, .plans/WEATHER_V2_PLAN.md) ────────────────────────────
// A flow map: the noise volumes are read at p - W(p) x age, W the layer's steering wind (wind_field.glsl), age the time
// since the texture there last renewed. Two phases half a window (cv2.wind1.y) apart; phase A renews at f = 0 and phase B
// carries the texture across A's renewal (A is used away from it, wA = 1 over 1 - 2 blend of the window). A RAY picks
// one phase (CV2_PHASE_XI, per pixel and frame in the march, so the history averages the cross-fade): one field
// evaluation, not two. The renewal time is staggered by a smooth field (~3000 km), so only ~2 x blend of the sky renews at once.
// Not the sample's own height: a passive tracer in the shear (~3 m/s per km) is smeared ~20x over a 2-h window.
float cv2WindStagger(vec3 d)
{
    return 0.23 * sin(dot(d, vec3(4.1, 2.7, -1.3)) + 0.7) + 0.19 * sin(dot(d, vec3(-1.9, 3.6, 2.4)) + 2.1)
         + 0.11 * sin(dot(d, vec3(2.2, -1.1, 4.3)));
}
vec3 cv2NoiseDispAt(vec3 dirE, float hSteer)
{
    float P = cv2.wind1.y;
    if (P <= 0.0) return vec3(0.0);
    float fT  = cv2.wind1.x / P + cv2WindStagger(dirE);   // windows elapsed here (the stagger shifts them)
    float f   = fract(fT);
    float b   = fract(cv2.wind1.z);                         // (wind1.z = the window count mod 1024 + the blend)
    float wA  = smoothstep(0.0, b, f) * (1.0 - smoothstep(1.0 - b, 1.0, f));
#ifdef CV2_PHASE_XI
    float xi  = CV2_PHASE_XI;   // the march: per pixel and frame, from builtins (a live global put it over the register cliff)
#else
    float xi  = 0.5;            // everyone else: the dominant phase (shadows, beams, probes)
#endif
    bool  pA  = xi < wA;
    float age = pA ? f * P : fract(f + 0.5) * P;
    // Each renewal starts from FRESH texture: a jump of the read position by a hash of the window's index (otherwise
    // every window would rewind the clouds to the same unadvected noise). Constant within a window: no motion.
    float k   = floor(cv2.wind1.z) + floor(pA ? fT : fT + 0.5) + (pA ? 0.0 : 0.5);
    vec3  j   = fract(sin(vec3(k * 12.9898, k * 78.233, k * 37.719)) * 43758.5453) - 0.5;
    return wfWind(dirE, hSteer, cv2.wind0) * age + j * 4.0e5;
}
// Whether the flow map is cross-fading at dirE (within pad of a window's blend): there a ray may read either phase, so
// anything that stands in for the march's reads (the clipmap's occupancy) must hold both.
bool cv2NoiseBlending(vec3 dirE, float pad)
{
    float P = cv2.wind1.y;
    if (P <= 0.0) return false;
    float f = fract(cv2.wind1.x / P + cv2WindStagger(dirE));
    float b = fract(cv2.wind1.z) + pad;
    return f < b || f > 1.0 - b;
}
// A noise-read offset (eye-relative ECEF m) in the moving frame: cv2Field / cv2FarColumn shift q's rSeaE and seaProjE
// by the displacement on entry, so this is the identity (the name marks a read that rides the wind).
vec3 cv2Mat(vec3 v) { return v; }

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
    return textureLod(cv2WeatherTex, dirE, CV2_WX_LOD(1.5)).a * 8000.0;
}

// ── Orographic clouds: the terrain's hold on the passing cloud (2026-11-06, issue 20261105_174850; "Orographic clouds",
// oro0.x) ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Not an object: a modifier of the layers the wind carries over the relief. Air forced over a ridge rises and, downwind of
// it, oscillates in a standing lee wave of wavelength lambda = 2 pi U / N (~12 km at 20 m/s): its streamlines are lifted
// over the ridge and at each crest one, two wavelengths downwind, and sink in the troughs between. Where the lift carries
// the mid layer's moist air past its condensation level it condenses into a smooth lens that stays put while the air
// streams through it; in the troughs it evaporates. The low cloud does not climb a massif: it parts around it.
// gCv2Oro (cv2OroField, per sample, Earth-fixed, from the DEM: binding 25):
//   x the streamlines' lift (m): e(0.1 lambda upwind) + 0.7 e(lambda) - 0.6 max(e(lambda / 2) - e(0.1 lambda), 0), e =
//     the ~6 km relief's excess over the ~19 km massif (DEM mips 1.2 and 2.8: smooth over the lee wave's scale) at the
//     upwind tap: the crest over the ridge, the first lee crest, and the trough half a wavelength downwind of a ridge (not
//     over a summit wider than lambda / 2, whose upwind flank is no trough); saturating, x 1.5 km / (1.5 km + lift);
//   y the lift the air needs to condense (m): mix(1400, 300, "lens moisture" (oro1.x) x the wave-cloud-day gate: two slow
//     waves that open it over ~a fifth of the land at a time, turning over in days) x the wind's dryness factor (oro1.y);
//   z the relief's envelope (the ~5 km DEM, mip 1, + 1.5 x its excess over the massif, m above sea level), w its
//     prominence over the ~20 km ground the low bases ride on (0..1).
// The mid layer reads x / y (cv2MidSigma), the low field z / w (cv2Field). Zero (the default) = no terrain effect, so flat
// land and every shader without the DEM are unchanged. The march computes it ONCE at its main sample (CV2_ORO_EXTERNAL)
// and the light / other reads keep it: computed inside every inlined cv2Field copy it cost the 128-register cliff
// (2026-11-05, the lens object: 251).
vec4 gCv2Oro = vec4(0.0);
#if defined(CV2_ELEV_BINDING) && !defined(SKY_LITE)
#define CV2_ORO 1
layout(set = 0, binding = CV2_ELEV_BINDING) uniform sampler2D cv2ElevTex;
vec4 cv2OroField(CV2Pos q)
{
    if (cv2.oro0.x <= 0.0) return vec4(0.0);
    float gG = cv2Ground(q.dirE);
    if (gG < 250.0 || q.h > gG + 9500.0) return vec4(0.0);
    vec3  d   = q.dirE;
    float lat = asin(clamp(d.z, -1.0, 1.0));
    float lon = atan(d.y, d.x);
    vec2  uv  = vec2((lon + PI) / (2.0 * PI), (0.5 * PI - lat) / PI);
    // the DEM's uv per metre along the wind (upwind = minus)
    vec2  duv = vec2(cv2.oro0.z / (2.0 * PI * R_EARTH * max(cos(lat), 0.05)), -cv2.oro0.w / (PI * R_EARTH)) * cv2.oro0.y;
    const float kEr = 9000.0, kEo = 15.0 / 255.0 * 9000.0;        // terrain.glsl's decode (15/255 = sea level)
    // The lift reads the relief at ~6 km over the ~19 km massif (mips 1.2 / 2.8; 2026-11-06, issue 20261105_201521): the
    // air's response to a ridge is a wave at the lee wavelength's scale, not the crest line. At the 5 km relief (mip 1) the
    // lifted layer bent into a sharp inverted V over each ridge.
    float hRL = textureLod(cv2ElevTex, uv, 2.8).r * kEr;                              // the ~19 km massif
    float eC  = max(0.0, textureLod(cv2ElevTex, uv - duv * 0.1, 1.2).r * kEr - hRL);
    float eT  = max(0.0, textureLod(cv2ElevTex, uv - duv * 0.5, 1.2).r * kEr - hRL);
    float eL  = max(0.0, textureLod(cv2ElevTex, uv - duv,       1.2).r * kEr - hRL);
    // Wave-cloud days (2026-11-06, issue 20261105_201331 "too uniform and persistent"): the moist, stable layer a lens needs
    // is a regional, passing condition, not the climate. Two slow waves (~1500 km, drifting west, turning over in ~3 days)
    // open it over ~a fifth of the land at a time; elsewhere the condensation lift is the dry air's 1400 m, which no relief
    // reaches (the lift saturates near 1.5 km): no lenses. (It was 1 +- 0.5 x one wave: lenses over every range, always.)
    float wv  = 0.6 * sin(lon * 23.0 + cv2.oro1.z) * sin(lat * 29.0 + cv2.oro1.w) + 0.4 * sin(lon * 37.0 - lat * 19.0 - 1.3 * cv2.oro1.z + 2.0);
    float mo  = cv2.oro1.x * smoothstep(0.2, 0.6, wv);
    // The lift saturates (x 1.5 km / (1.5 km + lift)): the Himalaya's 2-7 km excesses lifted a fifth of the range past the
    // condensation level, Fuji's ~2 km a few percent; compressed, ~8% and ~2%.
    float lf  = eC + 0.7 * eL - 0.6 * max(eT - eC, 0.0);
    // The envelope the low cloud parts round is the ~5 km relief (mip 1) + 1.5 x its excess over the ~19 km massif
    // (2026-11-06, issue 20261105_201521): at the 2.7 km DEM + 0.8 x its excess the gap's roof followed every crest line,
    // a sharp inverted V over the ridge seen from under the deck; now a smooth arch, a little higher over the summits (the
    // bare DEM under-reads the peaks the terrain's detail draws; at +0.8 x they poked ~1 km through a deck 900 m over it).
    // Its prominence over the ~20 km ground (w) from the same read: at the 2.7 km DEM the gap's sides followed the spurs.
    float hS  = max(0.0, textureLod(cv2ElevTex, uv, 1.0).r * kEr - kEo);
    return vec4(lf * 1500.0 / (1500.0 + max(lf, 0.0)), mix(1400.0, 300.0, mo) * cv2.oro1.y, hS + 1.5 * max(hS + kEo - hRL, 0.0),
                smoothstep(150.0, 700.0, hS - gG));
}
#endif

// ── Where a base sits (2026-11-05, vertical) ───────────────────────────────────────────────────────────────────────────
// x: the REGIONAL spread, the cluster Perlin (cl.a, 0.50 +- 0.057) normalised as the fog and the mid layer read it, x the
// type's "base variation" (alt.w): +-1.5 alt.w at the 8-64 km scale. Until 2026-11-05 the term was (cl.a - 0.5) x 2 x alt.w,
// so a 300 m setting moved a base by about +-34 m: every base in a view sat on one line.
// y: a lifting-condensation-level proxy (LCL ~ 125 m per K of dewpoint depression: ~500 m over a warm sea, 2-3 km over a
// dry afternoon continent). Lower over the sea (low land counts: the sea fog's own test), in rain and under a full cover;
// higher over dry land by day (the dust's dryness: the ~20 km cover fraction covF, no rain), most on convective types.
// The near field (cv2LowA) and the far field (cv2FarColumn) both take it, from the same inputs, so the bases agree at
// every distance. The top follows (cv2BaseTopShift); anvil3.zw's bound carries the margin (CPU).
vec2 cv2BaseShift(CV2Type ty, float clA, vec4 w, float cov, float covF, float gF, vec3 dirE)
{
    float reg   = clamp((clA - 0.5) * 8.8, -1.5, 1.5) * ty.alt.w * cv2.vert.z;
    float sea   = 1.0 - smoothstep(5.0, 60.0, gF);
    float wet   = clamp(w.b * 3.0, 0.0, 1.0);
    float dry   = (1.0 - smoothstep(0.02, 0.35, covF)) * (1.0 - sea) * (1.0 - wet);
    float day   = smoothstep(0.0, 0.5, dot(dirE, cv2.sunE.xyz)) * (1.0 - sea);
    float conv  = ty.look.z;
    float lcl   = conv * (-350.0 * sea - 250.0 * wet - 200.0 * (cov - 0.5) + 1400.0 * dry * day + 300.0 * day)
                + (1.0 - conv) * (-150.0 * sea - 150.0 * wet);
    return vec2(reg, lcl * cv2.vert.x);
}
// The top moves with it: a lower condensation level (sea, rain) moves the whole cloud down (marine cumulus are low AND
// capped low; only lowering the base deepened every puff and grew the cover seen from above), a higher one raises the top
// by half (desert cumulus are high-based and tall), the regional spread by 0.7 (the cloud keeps most of its depth).
float cv2BaseTopShift(vec2 bSh) { return min(bSh.y, 0.0) + 0.5 * max(bSh.y, 0.0) + 0.7 * bSh.x; }

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
    float soft;      // share that is the mid layer's sheet (its SHADOW is delta-scaled: mid2.z)
};

void cv2Add(inout CV2Field f, float s, float hf, float topH, float deck)
{
    if (f.sigma <= 0.0) { f.hf = hf; f.msBright = 1.0; f.ambient = 1.0; f.topH = topH; f.deck = deck; f.thin = 0.0; f.rain = 0.0; f.soft = 0.0; }
    // The deck flag by density share: with max(), a thin anvil sheet crossing a dense tower gave the
    // whole sample the deck's path-to-top shadow at the tower's density — a black band across the
    // tower at the anvil's height (user snap 2, pass 18).
    else                { f.topH = max(f.topH, topH); f.deck = (f.deck * f.sigma + deck * s) / max(f.sigma + s, 1e-9); }
    f.sigma += s;
}

// Mip level of a noise volume for a footprint of fpM metres (128 texels per period, w = 1/period).
float cv2Lod(float fpM, float invPeriod) { return max(0.0, log2(max(fpM * invPeriod * 128.0, 1e-6))); }

#ifdef CV2_MORPH_BINDING
// Review 17: cloud MORPHOLOGY — how cloud fields organise at 3-100 km (tools/make_cloud_morph.py): R closed
// cells (stratocumulus decks), G open cells (cold-air outbreaks over the sea), B streets (convective rolls),
// A clustered cumulus. Each channel is uniform on [0, 1], so 2.6 (u - 0.5) has the far field's quantile
// scale (zThr = 2.6 (0.5 - cov)): thresholded it covers the coverage the map asks for. Read on the drifted
// sphere through the cube projection, blended near face edges (triplanar, weights ^8). The types pick the
// morphology: decks the closed cells; convective cloud over the sea poleward of ~30 deg the open cells;
// elsewhere streets in some regions (the cluster field), clustered cumulus in the rest. Weighted normal
// deviates are renormalised by sqrt(sum w^2), so a blend keeps unit spread.
float cv2MorphZ(vec3 wd, vec3 mpF, float fpM, float strat, float sea, float region)
{
    vec3  a  = cv2.anchorMorph.xyz + mpF * cv2.anchorMorph.w;
    vec3  nw = abs(wd) / max(length(wd), 1e-6);
    nw *= nw; nw *= nw; nw *= nw;
    nw /= (nw.x + nw.y + nw.z);
    float lod = cv2Lod(fpM, cv2.anchorMorph.w) + 3.0;   // 1024 texels a period, not 128
    vec4  m = vec4(0.0);
    float ws = 0.0;
    if (nw.x > 0.02) { m += nw.x * textureLod(cv2MorphTex, a.yz, lod);               ws += nw.x; }
    if (nw.y > 0.02) { m += nw.y * textureLod(cv2MorphTex, a.xz + vec2(0.37), lod);  ws += nw.y; }
    if (nw.z > 0.02) { m += nw.z * textureLod(cv2MorphTex, a.xy + vec2(0.71), lod);  ws += nw.z; }
    vec4  z    = (m / max(ws, 1e-3) - 0.5) * 2.6;
    // The granular field below ~10 km (review 17, measured against MODIS imagery: the far field's spectrum fell
    // as k^-3.1 where real cloud fields fall as k^-1.4..-2.3, and its outlines were too smooth): the clustered-
    // cumulus channel again at a quarter of the period (~1.3 km puffs in 5-80 km clumps), on the dominant face.
    float zFine;
    {
        vec3  aw = abs(wd);
        vec2  uvF = (aw.x >= aw.y && aw.x >= aw.z) ? a.yz : (aw.y >= aw.z ? a.xz + vec2(0.37) : a.xy + vec2(0.71));
        zFine = (textureLod(cv2MorphTex, uvF * 4.0 + vec2(0.13, 0.57), lod + 2.0).a - 0.5) * 2.6;
    }
    float conv = 1.0 - strat;
    float latA = abs(wd.z) / max(length(wd), 1e-6);              // sin(latitude): the drift turns about z
    float wC   = strat;
    float wO   = conv * sea * smoothstep(0.42, 0.7, latA);       // ~25-45 deg
    float wS   = conv * (1.0 - wO) * smoothstep(0.48, 0.58, region) * (1.0 - 0.5 * sea);
    float wP   = max(conv - wO - wS, 0.0);
    float wF   = mix(0.5, 0.8, conv);
    return (wC * z.r + wO * z.g + wS * z.b + wP * z.a + wF * zFine)
         / sqrt(max(wC * wC + wO * wO + wS * wS + wP * wP + wF * wF, 1e-4));
}
#endif

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
// Review 17: the weather mip the GROUND SHADOW reads (cloud_march.comp sets it; 0 everywhere else, and the
// march never writes it, so its code folds away there). A low Sun's shadow ray runs ~100 km through the shell:
// any coverage over the clear threshold anywhere along it is full shadow, so the shadow became a binary map
// of the source JPEG's 8x8 blocks (~40 km) on the 5-km mip — stair-stepped, blocky shadows on the far sea at
// sunset (they read as blocky reflections). A coarser, C1 read gives them the soft outlines a grazing
// shadow (a penumbra tens of km wide) really has.
float gCv2WxLod = 0.0;
// Review 18: how much a view sample's ground footprint is stretched by a grazing view (1 / |dir . up|, capped;
// the view march sets it, every other consumer leaves 1). The far field's 2D reads are filtered by the
// stretched footprint: near the limb from MEO an isotropic footprint under-filtered them along the view's
// ground direction, and the history drew the aliasing as alternating half-res columns.
float gCv2Stretch = 1.0;
vec4 cv2WeatherSmoothAt(vec3 d, int lod);
vec4 cv2WeatherSmooth(vec3 d)
{
    if (gCv2WxLod > 0.0) {   // (logical mips: the shadow's coarse reads keep the scale they were tuned at)
        float l0 = floor(gCv2WxLod);
        return mix(cv2WeatherSmoothAt(d, int(l0) + 1), cv2WeatherSmoothAt(d, int(l0) + 2), gCv2WxLod - l0);
    }
    return cv2WeatherSmoothAt(d, 0);
}
vec4 cv2WeatherSmoothAt(vec3 d, int lod)
{
    vec3  a  = abs(d);
    float sz = float(textureSize(cv2WeatherTex, lod).x);
    // The face's two in-plane coordinates in [-1, 1], and the major axis.
    vec2 uv; vec3 axis;
    if (a.x >= a.y && a.x >= a.z)      { uv = d.yz / a.x; axis = vec3(sign(d.x), 0.0, 0.0); }
    else if (a.y >= a.z)               { uv = d.xz / a.y; axis = vec3(0.0, sign(d.y), 0.0); }
    else                               { uv = d.xy / a.z; axis = vec3(0.0, 0.0, sign(d.z)); }
    vec2 t = (uv * 0.5 + 0.5) * sz - 0.5;
    vec2 i = floor(t), f = t - i;
#ifdef CV2_WX_BSPLINE
    // Cubic B-spline (C2) from four bilinear fetches: the one-fetch smoothstep remap below flattens the field at every
    // texel centre, and thresholded coverage then draws the 10-km texels as rounded squares and stair-stepped edges
    // from orbit. (Hardware bilinear's 8-bit sub-texel weights step ~40 m here: harmless for coverage.)
    vec2 f2 = f * f, f3 = f2 * f;
    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0, w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0, w3 = f3 / 6.0;
    vec2 g0 = w0 + w1, g1 = w2 + w3;
    vec2 c0 = ((i - 0.5 + w1 / g0 + 0.5) / sz) * 2.0 - 1.0, c1 = ((i + 1.5 + w3 / g1 + 0.5) / sz) * 2.0 - 1.0;
    vec4 acc = vec4(0.0);
    [[dont_unroll]] for (int k = 0; k < 4; ++k) {
        vec2 c = vec2((k & 1) == 0 ? c0.x : c1.x, (k >> 1) == 0 ? c0.y : c1.y);
        float w = ((k & 1) == 0 ? g0.x : g1.x) * ((k >> 1) == 0 ? g0.y : g1.y);
        vec3 ds = (axis.x != 0.0) ? vec3(axis.x, c.x, c.y)
                : (axis.y != 0.0) ? vec3(c.x, axis.y, c.y)
                                  : vec3(c.x, c.y, axis.z);
        acc += w * textureLod(cv2WeatherTex, ds, float(lod));
    }
    return acc;
#else
    f  = f * f * (3.0 - 2.0 * f);
    uv = ((i + f + 0.5) / sz) * 2.0 - 1.0;
    vec3 ds = (axis.x != 0.0) ? vec3(axis.x, uv.x, uv.y)
            : (axis.y != 0.0) ? vec3(uv.x, axis.y, uv.y)
                              : vec3(uv.x, uv.y, axis.z);
    return textureLod(cv2WeatherTex, ds, float(lod));
#endif
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
vec4 cv2WeatherBilinear(vec3 d, float lodL)
{
    float lod = CV2_WX_LOD(lodL);   // (callers name the logical mip)
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
    // Not unrolled (nor the field's other fixed loops): the march inlines the field at every call
    // site, and its binary (225 KB) overflows the GPU's instruction cache — code size is the cost
    // that matters in this shader (perf sprint 2026-09-29, `shaders reload` reports Binary Size).
    [[dont_unroll]] for (int k = 0; k < 4; ++k) {
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
    [[dont_unroll]] for (int i = 0; i < 8; ++i) {
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
    return cv2FlowWeatherDir(wd, cv2FlowDisp(wd, cv2Mat(q.seaProjE)));
}

// detailAmt: > 0 = erosion from the detail volume at that strength (faded with distance by the
// caller); 0 = the MEAN erosion, no fetch — for lighting, shadows and beams, whose optical depth
// must match the eroded clouds on average (v1's cone learned this: an uneroded field is far denser
// than what is drawn, and shadowed everything); < 0 = no erosion at all, the upper bound the
// coarse march steps on (conservative: erosion only removes).
// fpM: the footprint (m) the sample stands for — the pixel's width at this distance, or a light
// step's length. The noise volumes are mip-mapped and read at that footprint, so a field that is
// finer than a pixel averages instead of aliasing (from orbit a 2 km cumulus cell is one pixel).
// stormProx: 0..1, how close the nearest Cb tower is (cv2ColumnSigma's CV2Col.prox): the storm cumulus
// rises around the towers only. 0 where the caller has no tower search (the eye's rain).
// Review 8: the ground shadow sets this: it reads at its step's coarse footprint (it must, or a low Sun's
// long path aliases), where the field would swap its cells for the far field's km-scale blobs, and the
// shadow was those blobs (stair-stepped) under the small puffs the view draws. It takes the cells'
// sub-pixel average instead. Never written by the view march, so it folds away there.
bool gCv2NoFar = false;
// Review 22: the heavy rain under a storm cell's dominant (anvil-reaching) tower, 0..1, set by cv2ColumnSigma for
// cv2FieldLow's rain shafts (cv2Field calls them in that order): a core under the tower's base, drawn out downwind
// under the anvil. The map's precipitation alone put the heaviest rain anywhere its regions said, in stripes.
float gCv2RainCore = 0.0;
// The low field's shape, folded into the scalars the shape fetch and the erosion read (2026-11-04, storms through the
// cumulus field): cv2LowA computes them from the weather map and the cells (the cumulus), a storm object's margin
// (cv2StormGeom) supplies its own, and cv2FieldLow carves the blend of the two with ONE set of fetches.
struct CV2Carve {
    float m0;       // the 2D strength minus the profile's requirement (e units): > 0 inside before the lobes
    float hb;       // height above the (unbumped) base (m)
    float baseAmp;  // the base's bumps (m)
    float lobeRef;  // the lobes' reference height above the base (m; extruded below it)
    float bigL;     // 0..1 the storm-scale lobes' share
    float deep;     // 0..1 deep convection (the storm-scale shape and detail)
    float lobeKL;   // the lobes' strength (e units per unit noise)
    float aBase;    // the lobes' share kept at the base
    float aRamp;    // ... ramping to full over this (m)
    float AshK;     // a shallow cloud's carve
    float phK;      // the surface's density slope (1 / e units)
    float phP;      // its offset
    float subPix;   // 0..1 sub-pixel cells' haze
    float PhSub;    // that haze's presence
    float sigK;     // extinction at full presence (1/m)
    float billow;   // 0..1 billow erosion (bite the cracks)
    float erK;      // erosion strength
    float erBase;   // the erosion kept at the base
    float wispE;    // the wispy edge
    float rV;       // the regional lobe / erosion variation (-1..1)
    float fpFade;   // sub-pixel lobes average away
    float lobeB;    // the lobes' largest possible addition (the bound before the fetch)
    float tur;      // the turrets' amplitude (e units per unit noise; cu3.x on convective cloud thicker than ~250 m)
    float bub;      // 0..1 the thermal bubbles' gate (convective share, thicker than ~250 m, not deep; shape2: no longer the turrets')
    float thk;      // the cloud's thickness (m: the cell's top over its base); the bubbles' size and base ramp follow it (shape2)
};
// A storm object at a point (cv2StormGeom): the margin of its towers and its shield, in metres.
struct CV2Storm {
    float m;        // signed distance-like margin (m): > 0 inside a tower / the shield's plate (-1e9 = none)
    float top;      // the storm cloud's top over this point (m)
    float hf;       // the lighting's height share (0 base .. 1 top)
    float deck;     // 0 a tower .. 1 the shield's plate (lit as a deck)
    float sig;      // the density factor on the congestus type's (1 a tower; an anvil plate thins with age and toward its edge)
    float near;     // under a full-height tower's head (CV2Col.near)
    float storm;    // the storm strength (CV2Col.storm)
    float prox;     // within the storm cumulus reach (CV2Col.prox)
    float cap;      // the lobes' damping share (x 0.75 of them): two thirds at the plate's outline (its edge thins
                    // instead of standing as a wall). (An overshooting dome's quarter until 2026-11-06: a smooth cap.)
};
float gCv2CdoK = 0.0;   // 0..1: inside a tropical cyclone's CDO (cv2StormGeom): its plate is a smooth sheet
CV2Storm cv2StormNone()
{
    CV2Storm g;
    g.m = -1e9; g.top = 0.0; g.hf = 0.0; g.deck = 0.0; g.sig = 0.0;
    g.near = 0.0; g.storm = 0.0; g.prox = 0.0; g.cap = 0.0;
    return g;
}
bool cv2LowA(CV2Pos q, float detailAmt, float fpM, float stormProx, inout CV2Field f, out CV2Carve c)
{
    c.m0 = -1e9; c.hb = 0.0; c.baseAmp = 0.0; c.lobeRef = 0.0; c.bigL = 0.0; c.deep = 0.0; c.lobeKL = 0.0; c.aBase = 1.0;
    c.aRamp = 1.0; c.AshK = 0.0; c.phK = 1.0; c.phP = 0.0; c.subPix = 0.0; c.PhSub = 0.0; c.sigK = 0.0; c.billow = 0.0;
    c.erK = 0.0; c.erBase = 1.0; c.wispE = 0.0; c.rV = 0.0; c.fpFade = 1.0; c.lobeB = 0.0; c.tur = 0.0; c.bub = 0.0; c.thk = 0.0;
    if (q.h < cv2.shell.x || q.h > cv2.shell.y) return false;

    // The map drifts in longitude exactly as v1's flat layer and the ambience driver read it
    // (cloudPhase x driftMult, rotated about the pole on the CPU into cover.zw).
    vec3  wd      = cv2Drift(q.dirE);
    // With the Cb columns on, no low cloud tops out above the congestus top or "Storm cumulus top"
    // (below: topMax, and the storm regions' mix toward column.w), plus the ground's lift: the cirrus
    // and anvil bands sit above that, and their samples paid the weather, cluster and cell reads before
    // the height test at the end (perf sprint 2026-09-29).
    // The low field's EXACT top bound (2026-10-23): over every weather type mix (CPU, anvil3.zw at the tropopause factor's
    // ends; see SatelliteSimCloudsV2.cpp), + the ground (the lift never exceeds it), + the storm cumulus target where a
    // tower is near (stormProx; its value needs the cluster read, below). The test after it took the target's LARGEST
    // value for every sample, and at the user's 12 km x (1 + 0.8) every sample up to ~22 km -- the whole cirrus and anvil
    // band over a storm -- paid the cell, smooth-weather and ground reads before the topMax test at the end returned
    // nothing. (The older test is kept as it was, so nothing it cuts comes back.)
    float hTypB = 1e30;
    if (cv2.column.x > 0.0) {
        float trB = cv2Tropo(wd);
        float cgT = cv2.types[3].alt.x + (cv2.types[3].alt.y - cv2.types[3].alt.x) * trB;
        float stT = cv2.types[1].alt.x + (cv2.types[1].alt.y - cv2.types[1].alt.x) * trB + 3500.0;
        if (q.h > max(max(cgT, stT), cv2.column.w * (1.0 + cv2.column3.z)) + cv2Ground(q.dirE) * 0.9 + 50.0) return false;
        hTypB = mix(cv2.anvil3.z, cv2.anvil3.w, (trB - 0.8) / 0.55) + cv2Ground(q.dirE) + 50.0;
        if (stormProx <= 0.0 && q.h > hTypB) return false;
    }
    float covSpan = max(cv2.cover.y - cv2.cover.x, 1e-3);

    // The noise volumes are read in the same drifted frame.
    vec3 rS = cv2Mat(q.rSeaE);
    vec3 sP = cv2Mat(q.seaProjE);
    // The flow (cv2FlowDisp) moves the weather map and the cluster field by flowD (not the cells).
    vec3 flowD = cv2FlowDisp(wd, sP);
    vec3 wdF   = cv2FlowWeatherDir(wd, flowD);

    // Early out on a coarse read AT THE FLOWED position. Conservative: its 20 km texels span the
    // few-km warp below, and their box average keeps at least a sixteenth of one cloudy 5 km texel.
    // (Pass 11 tested the UNflowed position at mip 4 while the lookup moved up to ~200 km: every
    // cloud displaced past an 80 km texel was cut off along that texel's straight edge — the
    // "discontinuities cutting the clouds up" at any flow setting.)
    float wCoarse = textureLod(cv2WeatherTex, wdF, CV2_WX_LOD(2.0)).r * cv2.look.x;   // also the far field's fraction (review 17)
    if (wCoarse < cv2.cover.x * 0.25) return false;

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
    // ... and near a tower with the storm cumulus target's real value here (cwT below).
    if (q.h > max(hTypB, cv2.column.w * (1.0 + cv2.column3.z * clamp((cl.a - 0.5) * 8.8, -1.0, 1.0))
                         + cv2Ground(q.dirE) + 50.0)) return false;
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
    // Review 18: in the far field the cloud FRACTION over ~20 km (the weather cube's mip 2, which holds the true
    // mean coverage of its fine texels) and the morphology place the clouds; the 5-km map's own zero contour no
    // longer cuts them off (it was every outline seen from orbit: blobs following the map).
    // From "Morphology from (m/px)" (morph.z = its log2) in full, fading in over the two octaves below.
    // Review 19: ONE placement at every distance (tools/harness/climb.py). Review 18 switched, as the footprint
    // grew, from the near field (the cells thresholded by the 5-km map) to a far field (the imagery thresholded
    // to the ~20 km fraction); the two put clouds in different places, and climbing over one spot the clouds
    // were replaced between ~35 and 150 km. Now the placement is the near field PLUS the imagery at a fixed
    // share ("Imagery share", morph.y) everywhere; a wider footprint only filters it, and the cells it can no
    // longer resolve become partial cover (presSub below), not a different placement.
    float covF  = clamp((wCoarse - cv2.cover.x) / covSpan, 0.0, 1.0);
    if (cov <= 0.0 && covF <= 0.0) return false;

    CV2Type ty     = cv2TypeAt(w.g);
    float   strat  = 1.0 - ty.look.z;                  // 1 = the stratiform field, 0 = the cells
    // The threshold's coverage: the 5-km map's, and in its clear texels half the ~20 km fraction, so the
    // imagery can place scattered cloud there and a broken region's outline is not the 5-km texels'.
    float   covT   = max(cov, 0.5 * covF);
    float   tropo  = cv2Tropo(wd);
    float   topMax = ty.alt.x + (ty.alt.y - ty.alt.x) * tropo;
    // Nimbostratus: a stratiform deck where the map says it rains thickens by up to 3.5 km (its
    // base is already darkened by the precipitation loading below).
    topMax += w.b * strat * 3500.0;
    // Bases follow the (smoothed) terrain: a convective base is the condensation level, about the
    // same height above the ground everywhere; decks follow it less (they hug and bank against
    // slopes). With every base above sea level the Tibetan plateau and the Andes stood inside the
    // cloud, and every ridge cut through it. Tops rise with the base, but not past the tropopause.
    // Review 8b: the REGIONAL ground (~80 km, mip 4, never above the local) lifts in full; only the relief
    // above it banks at 0.6-0.9. A plateau is the ground its boundary layer rides on: at 0.6-0.9 of its
    // 5 km the decks over Tibet sat inside the plateau and were cut away (a clear disc with a ring rim).
    float gF     = cv2Ground(q.dirE);
    float gC     = min(gF, textureLod(cv2WeatherTex, q.dirE, CV2_WX_LOD(4.0)).a * 8000.0);
    float lift   = gC + (gF - gC) * mix(0.6, 0.9, ty.look.z);
    float topCap = cv2.types[4].alt.x + (cv2.types[4].alt.y - cv2.types[4].alt.x) * tropo;
    topMax = min(topMax + lift, max(topCap, ty.alt.x + lift + 500.0));
    // With the Cb COLUMN layer on (cv2ColumnSigma) the towers and their heads are that layer's: here a
    // storm region is only its cumulus/congestus field, capped at column.w. A low-layer core running
    // the full span was a mountain sloping up into the anvil: a margin field thresholded per height
    // cannot narrow and flare again (user snap 4, pass 14).
    bool colOn = cv2.column.x > 0.0;
    // A TARGET, not a ceiling (as min() the slider did nothing upward, pass 19).
    // Review 5: the storm cumulus rises AROUND THE TOWERS (stormProx, within "Storm cumulus reach" of one),
    // not over the map's whole storm region. The map types regions hundreds to thousands of km across as
    // Cb, and taking the storm cumulus top over all of it made a continent-wide floor at 5-7 km with the
    // towers standing in it (user snapshots 1-2). Away from towers a Cb-typed region is a field of
    // ordinary cumulus (the cumulus type's top): the storms are the towers, their flanking cumulus and
    // their heads. Decks keep their own tops.
    if (colOn) {
        float wMap = smoothstep(0.8, 0.95, w.g);
        float cuT  = cv2.types[2].alt.x + (cv2.types[2].alt.y - cv2.types[2].alt.x) * tropo + lift;
        topMax = mix(topMax, min(topMax, cuT), wMap);
        // Its height varies by region (the cluster Perlin, ~+-2 sd -> +-"Storm cumulus variation"): one
        // flat target made every storm's cumulus field the same height (user, 2026-09-29).
        float cwT = cv2.column.w * (1.0 + cv2.column3.z * clamp((cl.a - 0.5) * 8.8, -1.0, 1.0));
        topMax = mix(topMax, cwT + lift, stormProx * min(ty.look.z * 2.0, 1.0));
    }

    // A deck is not a slab: its thickness follows the closed cells and a km-scale Perlin field, and
    // a thicker part hangs lower (an overcast seen from below was one flat, featureless plane).
    // Closed-cell rims (cl.b) fade out with the footprint: from orbit they read as cracked ice.
    float rimAmt  = 0.25 * (1.0 - smoothstep(0.5, 2.5, cv2Lod(fpM, cv2.anchorCell.w)));
    float deckVar = clamp((ce.a - 0.5) * 3.0 + (cl.b - 0.5) * rimAmt * 4.0, -1.0, 1.0);
    // The base (2026-11-05, vertical): the type's, + the regional spread and the condensation-level proxy (cv2BaseShift),
    // + each cumulus cell's own offset (ce.g, the cell's strength: a weak, dying cell sits a little higher), - the deck's
    // thickness variation. Kept >= 150 m over the ground and >= 400 m under the top (a thin cloud never inverts); the top
    // follows (cv2BaseTopShift).
    vec2  bSh     = cv2BaseShift(ty, cl.a, w, cov, covF, gF, q.dirE);
    topMax       += cv2BaseTopShift(bSh);
    float base    = ty.alt.x + lift + bSh.x + bSh.y + cv2.vert.y * (0.55 - ce.g) * ty.look.z - strat * 150.0 * deckVar;
    base          = clamp(base, lift + 150.0, max(topMax - 400.0, lift + 150.0));
    // The base is bumped in 3D by the lobes below, by up to ~2.2 x baseAmp: a deck's underside is
    // lumpy (rolls, pouches), a cumulus base nearly flat (the condensation level).
    float baseAmp = mix(40.0, 220.0, strat) * cv2.storm.w;
    // Turrets (cu3) rise on any cloud with a convective share (ty.look.z: half on stratocumulus), up to turH over the
    // type's top. (A shower field typed between stratocumulus and cumulus is shaped mostly as a DECK, flatK ~0.5-0.7: a
    // lumpy sheet with a hump per cell, which is what issue 20261104_181014 saw from 4.7 km.)
    if (q.h > topMax + cv2.cu3.x * ty.look.z * 1200.0) return false;
    // Below the (lowest possible) base: only rain, where the map says it precipitates.
    bool  below   = q.h <= base - 2.2 * baseAmp;
    // The bake's precipitation channel keys on the RAW map brightness (> 0.55-0.9), which few
    // storms reach: most towering fields came out dry. Deep convection at near-full (remapped)
    // coverage rains too.
    // Full overcast rains lightly whatever its type (a thick tropical deck read as stratiform).
    float rainAmt = max(w.b, smoothstep(0.75, 1.0, cov) * (0.3 + 0.7 * smoothstep(0.55, 0.9, w.g))) * cv2.rain.x;
    if (below && rainAmt < 0.02 && gCv2RainCore <= 0.0) return false;
    float z = max(q.h - base, 0.0) / max(topMax - base, 1.0);   // height through the type's full span

    // The cells (1-2 km Worley blobs; deep types two mips coarser, so neighbours merge into towers
    // several km wide) shape each cloud: its top (eB below) and the turret over each cell (capS). Until
    // review 19 they also placed the near field's clouds (a convective field of cells, a stratiform
    // baseline broken along the closed-cell rims, thresholded by the 5-km coverage).
    // ... and any cloud whose span is that of a congestus / tower (2026-10-26, issue 20261026_192322: the storm cumulus
    // round a weak tower, raised to "Storm cumulus top" (12 km) on the fine 1-2 km cells of a cumulus-typed map, stood
    // as rows of narrow spikes; a tall cloud is as wide as it is deep).
    float deep       = max(smoothstep(0.45, 0.85, w.g), smoothstep(3500.0, 8000.0, topMax - base));   // cumulus (0.5) ~0, congestus (0.75) ~0.8
    // At a FIXED coarse mip (or the footprint's, once coarser): these set a cell's width and height,
    // and at footprint + 2 (+ 4 below) they changed with the viewing distance, so every storm was
    // stretched along the view rays into fins radiating from the camera (user snaps, pass 13).
    float lodCell    = cv2Lod(fpM, cv2.anchorCell.w);
    float cellsC     = cv2MesoSmoothG(cv2.anchorCell.xyz + mp * cv2.anchorCell.w, max(lodCell, 2.0));
    // Deep types: the coarse cells only. The fine cells' 2 km edges, extruded up a 10 km column, were
    // vertical grooves on every storm wall (user snaps 7-10, pass 13): the 3D lobes shape a tower.
    float cells      = mix(ce.g, min(cellsC * 1.8, 1.0), deep);
    // The cells' field (convective: cells clustered; stratiform: a high baseline broken along closed-cell rims;
    // with a floor, so at full coverage the gaps between cells close into a low deck).
    float fieldConv  = 0.15 + 0.85 * sqrt(cells) * mix(0.6, 1.0, cl.r);
    float fieldStrat = 0.5 + rimAmt * (cl.b - 0.5) + 0.125 + 0.3 * (ce.a - 0.5) + 0.2 * ce.g;
    float field      = mix(fieldStrat, fieldConv, ty.look.z);
    // ── Mesoscale structure, 3-90 km: the cluster Perlin at 1x, 4x and 16x, normalised (~N(0,1)), and the
    // cloud MORPHOLOGY (real imagery, cv2MorphZ) mixed in by "Morphology" (morph.x). Read at twice the
    // footprint (fpF, review 18): the march's ray lands at another point of its texel each visit, and a field
    // read at the footprint's own mip changed inside it (flicker on scattered puffs from MEO).
    float fpF    = fpM * 2.0 * cv2.morph.w * gCv2Stretch;   // morph.w = 2^-"Far-field sharpness"
    vec4  cl4    = textureLod(cv2MesoTex, (cv2.anchorCluster.xyz + mpF * cv2.anchorCluster.w) * 4.0
                              + vec3(0.37, 0.61, 0.13), cv2Lod(fpF, cv2.anchorCluster.w * 4.0));
    vec4  cl16   = textureLod(cv2MesoTex, (cv2.anchorCluster.xyz + mpF * cv2.anchorCluster.w) * 16.0
                              + vec3(0.71, 0.23, 0.47), cv2Lod(fpF, cv2.anchorCluster.w * 16.0));
    float fz     = ((cl.a - 0.5) * 0.5 + (cl4.a - 0.5) * 0.3 + (cl16.a - 0.5) * 0.2) / 0.035;
    float zMs    = fz;
#ifdef CV2_MORPH_BINDING
    if (cv2.morph.x > 0.0) {
        float seaM = 1.0 - smoothstep(5.0, 120.0, w.a * 8000.0);
        zMs = mix(fz, cv2MorphZ(wd, mpF, fpF, strat, seaM, cl.r), cv2.morph.x);
    }
#endif
    // The blend: the imagery's share is fixed (0.3 x morph.y in field units, ~0.1 sd at the default), the same
    // at every footprint, so the cells' masses and puffs up close and the imagery's structure from orbit are
    // one placement. (Review 17's near clustering was 0.075.)
    field += 0.3 * cv2.morph.y * clamp(zMs, -2.5, 2.5);
    float thr        = 1.0 - covT;
    // How far into the cloud this column is: 0 at the edge, 1 well inside. Not normalised by the
    // coverage, so sparse fair-weather cells stay small and low; only a dense field builds towers.
    // "Small cumulus" (tune.x, 2026-10-11): the field is lifted by tune.x at the threshold, tapering to nothing 3 x tune.x
    // above it (monotone: low puffs appear, formed cloud is unchanged). The sub-pixel cover below (presFar) counts cloud
    // down to about that far under the threshold, so from orbit a region of low coverage was a field of small
    // puffs that were not there on the way down (climb at the user's snapshot: cover 20-30% from 450 km up,
    // ~1% below 300 km).
    float xe         = field - thr;
    float e          = clamp(max(xe, min(xe + cv2.tune.x, cv2.tune.x + 0.6667 * xe)) / 0.55, 0.0, 1.0);
    // The same strength at the CELL scale (the cells two mips coarser: a cell's neighbourhood mean;
    // four for deep types, whose cells are already the coarse ones). It sets each cell's height and
    // its peak, so every column of a cell shares one profile (see THE CONVECTIVE PROFILE below).
    float cellsB     = deep > 0.02
                     ? mix(cellsC, cv2MesoSmoothG(cv2.anchorCell.xyz + mp * cv2.anchorCell.w, max(lodCell, 4.0)), deep)
                     : cellsC;
    float fieldB     = 0.15 + 0.85 * sqrt(min(cellsB * 1.8, 1.0)) * mix(0.6, 1.0, cl.r)
                     + 0.08 * clamp(fz, -2.5, 2.5);
    // Its HEIGHT keys on the 5-km map's own coverage (a cloudy texel builds tall cells, a clear one only
    // low puffs), not on the placement's fraction: that flattened the big cumulus masses (review 19).
    float eB         = clamp((fieldB - thr) / 0.55, 0.0, 1.0);

    // ── Rain shafts: under the column's cloud, heavier under convective cores than under a
    // nimbostratus deck. Curtains = the shape volume compressed along the vertical (streaks that
    // fall), slanted downwind with depth below the base. ~0.3-1.5 /km: a few km of visibility in a
    // heavy shower. Lit through the cloud above by the light march: dark under a Cb, bright at its
    // sunlit edge — which is where rainbows show (the march's rain phase). ──
    // 2026-10-22 (issue 20261022_174757): rain falls from INSIDE the cloud. Above the shafts' old top
    // (base - 2.2 baseAmp) the body path carried no rain at all, so the shafts ended a few hundred metres under a
    // visible base and the drops at the eye vanished as it rose into that band. Up to kCv2RainIntoM above the base
    // the same shaft is a RAIN FLOOR the body's samples start from (its early returns return it; inside the cloud
    // the rain is added to the body's extinction as its rain share), thickening toward the base and fading out
    // inside the cloud. One shaft fetch, as before.
    const float kCv2RainIntoM = 400.0;
    float rainLo = base - 2.2 * baseAmp;
    if (below || (q.h <= base + kCv2RainIntoM && (rainAmt >= 0.02 || gCv2RainCore > 0.0))) {
        // Review 22: the map's rain is the light, intermittent background; the heavy rain is under the dominant
        // towers' cores (gCv2RainCore), denser (to ~2x) and more uniform there.
        float core = gCv2RainCore * cv2.rain.x;
        // The rain falls FROM the cloud drawn over this column (2026-11-04, issue 20261104_020642): the body's margin
        // at its base (below, before the lobes: m0 at zeta = 0, the convective profile's req0 against the cell-capped
        // strength eS; a deck's raw strength). The map's rain keyed on the 2D strength e alone, so it hung under the
        // gaps between a storm's cells, where the profile leaves no cloud at the base: a curtain, and the drops at
        // the eye, in clear air under open sky. Sub-pixel cells keep their mean (the shaft's own fade below).
        float hVarR  = 1.0 + cv2.cu.x * (0.5 * clamp((cl16.a - 0.5) * 9.0, -1.0, 1.0) + 0.9 * (cells - 0.55) * (1.0 - deep));
        float flatR  = ty.alt.z * (1.0 - (1.0 - smoothstep(0.25, 0.65, covT)) * cv2.cu.y * smoothstep(0.1, 0.5, ty.look.z));
        float TR     = max(cv2.flow.w, 0.05) * clamp(max(pow(eB, 0.83), 0.6 * pow(e, 0.83)) * max(hVarR, 0.3),
                                                     mix(0.25, 0.12, cv2.cu.x), 1.0);
        float capR   = 0.3 + 0.7 * cells;
        float hSR    = max(0.2 - abs(e - capR), 0.0) / 0.2;
        float req0R  = 0.2 * smoothstep(0.1, 0.6, eB) + cv2.flow.z * mix(0.15, 0.3, deep) * smoothstep(0.05, 0.4, eB)
                     * smoothstep(0.4, 0.8, TR / max(cv2.flow.w, 0.3));
        float mBase  = mix(min(e, capR) - hSR * hSR * 0.05 - req0R, e, flatR);
        float under  = mix(smoothstep(0.0, 0.08, mBase), 1.0, smoothstep(300.0, 2000.0, fpM));
        // "Showers from cores" (cu4.w): on a convective type the map's rain falls from the strong cells only (the cell-scale
        // strength eB: the big, tall clouds), as shafts under them; at 0 every cloud base of a showery region trailed a
        // translucent curtain that wrapped the whole field in grainy haze (issue 20261104_181014: the "soft blobs").
        float rate = max(rainAmt * smoothstep(0.3, 0.8, e) * mix(0.4, 1.0, ty.look.z) * under
                         * mix(1.0, smoothstep(0.55, 0.9, eB), cv2.cu4.w * ty.look.z), core);
        if (rate <= 0.0 && below) return false;
        if (rate > 0.0) {
        vec3  rr = rS + eastW * ((base - q.h) * 0.25);
#if !defined(SKY_LITE) && defined(CV2_FRACTUS)
        // (Fractus, below: through its band the curtains' vertical squeeze eases off. Only in the march and the occupancy
        // clipmap's bake (CV2_FRACTUS): in the coarse volumes that also read cv2Field -- the froxels, the aerial-perspective
        // volume, the Sun volume -- the thin band under every raining base became blue-grey rectangles low on the horizon,
        // 2026-11-05 integration check.)
        float fDep = mix(250.0, 550.0, clamp(rate * 2.0, 0.0, 1.0));
        float frK  = cv2.vert.w > 0.0 ? smoothstep(base - fDep - 150.0, base - fDep + 50.0, q.h) * (1.0 - smoothstep(base - 60.0, base, q.h))
                                        * (1.0 - smoothstep(600.0, 1500.0, fpM))
                                        * (1.0 - smoothstep(3000.0, 7000.0, R_EARTH * length(cross(q.dirE, cv2.prevObs.xyz)))) : 0.0;
        // (... and only within ~3-7 km of the eye, 2026-11-05 integration fix: further out the band under low (sea) bases,
        // 100-500 m up and seen nearly edge-on from the ground, drew rows of flat-topped blue-grey blobs a degree or two
        // over the horizon (towers region -5 163, all within ~15 km: a 6-15 km fade left them). Scud is a detail overhead.)
        rr -= wd * (dot(rr, wd) * 0.85 * (1.0 - frK));
#else
        rr -= wd * (dot(rr, wd) * 0.85);
#endif
        // The curtains read 1.5 mips coarser with a softer threshold (review 22): at the shape period's finest
        // octaves and x4 they were a striped on/off pattern a still observer saw repeat ("perliny").
        vec4  sr = textureLod(cv2ShapeTex, cv2.anchorShape.xyz + rr * cv2.anchorShape.w,
                              cv2Lod(fpM, cv2.anchorShape.w) + 1.5);
        float shaft = clamp((sr.g * 0.6 + sr.a * 0.4 - 0.42) * 2.2 + 0.42, 0.0, 1.0);
        shaft = mix(shaft, max(shaft, 0.75), core);
        shaft = mix(shaft, 0.45, smoothstep(300.0, 2000.0, fpM));   // sub-pixel curtains: their mean
        // Above the old top: denser toward the base (the shaft gathering under it), gone kCv2RainIntoM into the cloud.
        float into = (1.0 + 1.5 * smoothstep(rainLo, base, q.h)) * (1.0 - smoothstep(base, base + kCv2RainIntoM, q.h));
        f.sigma    = rate * shaft * 0.0012 * (1.0 + core) * (below ? 1.0 : into);
        f.msBright = 0.6;
        f.ambient  = 0.8;
        f.rain     = 1.0;
        f.topH     = base;
#if !defined(SKY_LITE) && defined(CV2_FRACTUS)
        // Fractus / scud (2026-11-05, vertical; "Fractus", vert.w): ragged cloud fragments in the saturated air under a
        // raining base, in a band hanging 60 m to ~250-550 m under it (deeper in heavier rain), fading with the footprint
        // (sub-pixel scraps would only grey the rain). From the shaft's own texel (a second fetch here put the march at 249
        // registers): through the band the shaft's vertical compression eases off (frK above), so the texel there is
        // round noise, not curtains (thresholded, curtains drew a row of flat-topped blocks); its band edges move +-250 m
        // with the texel. Cloud, not rain: its share leaves f.rain (the bows keep the rain's).
        if (frK > 0.0) {
            float hJ    = q.h + 500.0 * (sr.a - 0.5);   // (+-250 m: at +-120 the band's flat top and base showed)
            float band  = smoothstep(base - fDep, base - 0.55 * fDep, hJ) * (1.0 - smoothstep(base - 160.0, base - 60.0, hJ));
            float rag   = smoothstep(0.56, 0.8, sr.r * 0.5 + sr.g * 0.3 + sr.b * 0.2 + 0.15 * min(rate * 2.0, 1.0)) * band
                        * frK * min(rate * 4.0, 1.0) * smoothstep(80.0, 250.0, q.h - cv2Ground(q.dirE));   // (not a ground fog)
            float sFr   = 0.006 * cv2.vert.w * rag;   // (0.012: from under a raining edge, a dark veil over the whole view)
            f.rain      = f.sigma / max(f.sigma + sFr, 1e-9);
            f.msBright  = mix(0.6, 0.85, 1.0 - f.rain);
            f.sigma    += sFr;
        }
#endif
        if (below) return false;
        }
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
    // "Cumulus height variation" (cu.x, issue 20261012_235712): a field's clouds are not all one height. The top
    // varies by region (~1/16 of the cluster period) and by cell (each fine cell a turret of its own height), and
    // the floor falls from 0.25 to 0.12 of the span, so a weak cell is a small puff. With one T per neighbourhood
    // and a 0.25 floor, a region of low coverage was sheets of one thickness: "pancakes".
    // (By cell on shallow types only: deep convection's coarse cells drew the towers out into fins.)
#if !defined(SKY_LITE)
    // CUMULUS HEAD WIDTH ("Cumulus head width", ciHu2.w; 2026-11-05): the cell's own top follows its strength `cells`, a
    // dome (1 - d^2) over its radius, so a tall cell's outline was that dome stretched kilometres up over ~1 km: a bullet
    // with sheer sides (from below and the side, "slabs"). Toward a plateau (smoothstep(0.05, 0.45, cells)) the top is
    // near-constant over the cell and the vertical profile (widest at zm, a narrower stem) shapes the tower: a head wider
    // than its stem.
    float cellsH = mix(cells, 0.15 + 0.85 * smoothstep(0.05, 0.45, cells), cv2.ciHu2.w);
#else
    float cellsH = cells;
#endif
    float hVar   = 1.0 + cv2.cu.x * (0.5 * clamp((cl16.a - 0.5) * 9.0, -1.0, 1.0) + 0.9 * (cellsH - 0.55) * (1.0 - deep));
#if !defined(SKY_LITE)
    // CUMULUS TOWER SPREAD (envelope, 2026-11-05; clouds.md "Cumulus envelope"): the large-scale silhouette. Every cell's top
    // came from its cell-scale strength eB with the +-cu.x variation above, capped at flow.w of the span, so a mass of merged
    // cells was ONE dome with one top, and the bubbles, turrets and lobes only dressed its surface ("still gumdrops, still
    // 2D"). Here hVar takes k x (1.8 s - 1) (k = "Cumulus tower spread", shade.z; s = a km-scale field, the cell volume's
    // Perlin (its 1-8 km octaves) plus the cells), and the top's cap below is 1.7 x flow.w, then the type's span: inside one
    // mass, towers several km tall stand beside shoulders a few hundred metres high, and the dome becomes a skyline.
    // (Written as T x mix(1 - k, 1 + 0.8 k, s) after the clamp, the look was the same and the march went to 249 registers
    // on the merged tree; so did the cap as 1 + 0.9 k, and the Perlin at a fixed coarse mip read with cellsC (its tops also
    // varied too slowly: a whole mass tall again). This form is 128. The ranks of tall lit columns seen from 25 km are
    // towers, the same with the fixed mip: not a footprint artefact. The cap is a constant, so at k = 0 a cell whose hVar
    // already took it past 1 (cu.x) now rises up to 1.7 x flow.w as well.)
    hVar += cv2.shade.z * (1.8 * smoothstep(-1.0, 1.0, clamp((ce.a - 0.5) * 6.0, -1.0, 1.0) + 0.8 * (cells - 0.55)) - 1.0);
    const float kCv2TopCap = 1.7;
#else
    const float kCv2TopCap = 1.0;
#endif
    // A deck is a deck only where the sky is mostly covered: a stratiform type at low coverage is patches of
    // shallow cumulus (humilis, the broken edge of a stratocumulus sheet), shaped as cells with lobes like any
    // other. Kept flat, a region of low coverage was smooth lens-shaped sheets kilometres wide and a few
    // hundred metres thick ("pancakes", issue 20261012_235712 shot 1). Placement is unchanged.
    // (Not stratus: its thin, torn sheets are right as they are; as cells they came out as smooth blobs.)
    float lowCov = max(1.0 - smoothstep(0.25, 0.65, covT), cv2.cu4.z * smoothstep(0.02, 0.2, w.b)) * cv2.cu.y
                       * smoothstep(0.1, 0.5, ty.look.z);
    float flatK  = ty.alt.z * (1.0 - lowCov);
    float T      = min(max(cv2.flow.w, 0.05) * clamp(max(pow(eB, 0.83), 0.6 * pow(e, 0.83)) * max(hVar, 0.3),
                                                   mix(0.25, 0.12, cv2.cu.x), kCv2TopCap), 1.0);
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
    // The lobes LIFT the cloud's upper surface by a height in metres (kCv2LobeLiftM x their strength), whatever
    // the cell's size: to first order, the profile's slope there x the lift. As a margin alone they moved a
    // dome's top by a few tens of metres (the profile is steep near the top), and every cumulus seen from a
    // distance was a smooth mound ("gumdrops", no cauliflower: issue 20261012_235712).
    // (At most 0.7 of the cell's own thickness per unit: a small cloud has small lobes. Uncapped, a 300 m puff took
    // 300 m bumps and a field of clean cumulus broke up into scattered shreds.)
    float thickM = T * max(topMax - base, 1.0);
    float liftK  = 2.0 * (1.0 + 0.5 * cv2.cu.z - reqMin) * min(vC, 1.3) / (1.0 - zmC) * min(kCv2LobeLiftM / thickM, 0.7)
                                * (1.0 - flatK);
    float reqC   = reqMin + (req0 - reqMin) * uC * uC + (1.0 + 0.5 * cv2.cu.z - reqMin) * vC * vC;
    // ("Cumulus top headroom", cu.z: the strength needed AT the cell's top is past what a column has, so the top is
    // shaped by the field and the lobes all the way up. At 1.0 every strong column reached zeta = 1 and the tops of
    // a dense field were cut flat at one level: mesas, "blocky layers".)
    // (2026-10-13: the pass-14 deep CORES of this layer, the towers drawn here while "Cb columns" was 0 -- a full-span
    // column with a waist and a head read from the cells -- are gone, with the pass-11 "old cones" (Tower top 0). Both
    // were A/B relics, and their code kept the march on the slow side of its register cliff once this pass's shape
    // terms were added: 222 registers with them, 128 without. With the column layer off a storm is tall congestus.)
    // A deck's top, in its span: thin parts glow from below, thick parts go dark (from below an
    // overcast is seen by the light it transmits, so its thickness IS its texture).
    float zTop   = clamp(0.75 + 0.3 * deckVar, 0.4, 1.0);
    float turW   = cv2.cu3.x * ty.look.z * (1.0 - deep);   // (recomputed here: carried from the top test it cost registers)
    float gStrat = 1.2 * smoothstep(0.3, 1.0, z / zTop);
    // A deck's top is lifted by the lobes too (up to ~100 m: stratocumulus tops are lumpy, not a lens's skin).
    {
        float uD = clamp((z / zTop - 0.3) / 0.7, 0.0, 1.0);
        liftK += flatK * 7.2 * uD * (1.0 - uD) / (0.7 * zTop) * min(600.0 / max(topMax - base, 1.0), 0.6);
    }
    // In e units (the stratiform margin keeps the raw strength: a deck's thickness follows it).
    float m0     = mix(eS - reqC, e - gStrat, flatK);
    float fpFade = mix(1.0, 0.3, smoothstep(100.0, 800.0, fpM));   // sub-pixel lobes average away
    // Convective lobes are 2.5x stronger with the profile: the reference's shapes come from its 3D
    // noise thresholded by a soft coverage, and here the 2D field's steep edge dominated — the lobes
    // moved a cell's side by ~100 m, so every cloud was its 2D outline extruded (flat faces).
    // Review 22: on convective types the lobes TAPER up the cell, "Cumulus lobes (bottom)" to "(top)"
    // (form.x units): strong bulges low, a smoother head. Until then they grew 0.8 -> 1.25x upward.
    float convW  = mix(ty.look.z, 1.0, lowCov) * (1.0 - flatK);
    float lobeH  = mix(cv2.farLight.z, cv2.farLight.w, smoothstep(0.2, 0.9, zeta));
    // "Lobe variation" / "Erosion variation" (tune.z / .y): by region (the cluster Perlin at ~1/4 of its
    // period, +-2 sd -> +-1), some clouds are smooth and big-lobed, others small-lobed and finely broken (ONE value: a second,
    // carried down to the erosion, put the march at 222 registers): one lobe size and one
    // erosion everywhere made every cloud the same popcorn.
    float rV     = clamp((cl4.a - 0.5) * 9.0, -1.0, 1.0);
    // (Stratocumulus, a deck type that is half convective: x2.4 on the deck's own lobes. Its tops were smooth mounds.)
    float lobeK  = mix(cv2.form.x * (1.0 + 4.0 * ty.look.z * flatK), lobeH, convW) * mix(0.5, 1.0, max(ty.look.z, lowCov)) * fpFade
                 * mix(1.0, 2.5, convW) * max(1.0 + cv2.tune.z * rV, 0.1);
    // Sub-pixel cells: thresholding the mip-averaged field would erase them (the average sits
    // below the threshold), so where a cell is smaller than the footprint the presence becomes the
    // fraction of area covered — a haze of the right opacity instead of nothing, or aliasing dots.
    // Starts at a 2x footprint (it began at 3x: from orbit the barely resolved cells read as salt).
    float subPix = smoothstep(1.0, 3.0, cv2Lod(fpM, cv2.anchorCell.w)) * ty.look.z;
    // Everything the shape fetch does not change is folded into a few scalars BEFORE it: what stays alive across
    // the fetches decides the march's register allotment (128 or ~222, the second 20-60% slower; the first cut of
    // this pass, with the same maths written after the fetch, was at 223).
    // BIG lobes with the cumulus-scale ones on their crowns, for deep convection and (at 0.7) for any convective
    // cloud thicker than ~1.2-3.5 km: the storm volume read 2x finer. A tall cumulus read only the cumulus-scale
    // volume, whose strongest octave is ONE size of ball: a cone stuck all over with equal ~0.5 km balls ("popcorn";
    // shots 5, 8). Thin clouds keep the small lobes alone ("not so many lobes for smaller clouds").
    float bigL   = max(deep, 0.7 * smoothstep(1200.0, 3500.0, thickM) * convW);
    float aBase  = max(mix(0.3, 0.05, cv2.high2.w), cv2.cu.y * max(convW, 0.5 * flatK));
    float aRamp  = mix(clamp(1.2 * thickM, 150.0, 700.0), 250.0, flatK);
    float lobeKL = lobeK * mix(mix(0.4, 1.0, smoothstep(0.0, 0.25, e)), 1.0, flatK) * (1.0 + liftK);
    // A SHALLOW cloud is carved by the lobes in proportion to its own strength (cu.y): troughs cut it through to
    // the base, crests stand as puffs about as wide as they are tall. An additive lobe moves a sheet's top by a few
    // metres and its edge by a few tens: the sheet stayed a smooth lens. (A deck's thin margin, where its strength
    // runs out, frays the same way: it tapered to a knife edge.)
    float AshK   = cv2.cu.y * 1.5 * min(eS, e) * fpFade * max(convW, flatK * ty.look.z)
                                * mix(1.0 - smoothstep(300.0, 1200.0, thickM), 1.0 - smoothstep(0.2, 0.6, e), flatK);
    // "Cumulus break-up" (cu.y): in the cloud's lowest part the lobes are read at one height (lobeRef above the
    // base), i.e. extruded straight down to the base: they can cut a shallow cloud through to its base, into puffs
    // with flat bottoms, where lobes that vary with height would dimple the base. Until then the lobes were damped
    // to 5-30% at the base and reached full strength 700 m up, so a cloud thinner than that had none at all.
    float lobeRef = clamp(0.35 * thickM, 60.0, 400.0) * convW * cv2.cu.y;
    // The sub-pixel cells' haze, in the mesoscale presence's patches (their mean is still ~cov).
    // Review 19: the share of the pixel the unresolved cells cover — the field (its cells now at their mip:
    // ~their mean) against the threshold, spread by the variance the mip averaged away (~0.2 of field units
    // for fully sub-pixel cells). It replaced a Perlin/morphology presence of its own (a second placement).
    float sigU    = 0.03 + 0.2 * smoothstep(0.0, 3.0, lodCell);
    float presFar = clamp(0.5 + (xe + cv2.tune.x * (1.0 - smoothstep(0.0, 3.0, lodCell))) / (2.5 * sigU), 0.0, 1.0);
    float PhSub = presFar * 0.8 * clamp((mix(0.2 + 0.4 * covT, 1.0, flatK) - z) * cv2.form.w, 0.0, 1.0)
                * smoothstep(0.0, 150.0, hb);
    // Height within THIS column (for the lighting terms and billowing): the column's own top.
    // (Convective: the CELL's top T, so hf runs 0..1 up the cloud, not up each column's own cone.)
    float hf     = clamp(z / max(mix(T, zTop * 0.85, flatK), 0.05), 0.0, 1.0);
    f.hf       = hf;
    f.msBright = ty.look.x;
    f.ambient  = ty.look.y;
    f.deck     = strat * smoothstep(0.6, 1.0, e);
    // The deck's upper envelope (a lumpy top reaches ~zTop): the grazing-light shadow measures the
    // depth below it, and a mean top made every trough between the lumps black at sunrise.
    f.topH     = base + mix(T * 1.1, zTop, flatK) * (topMax - base);
    // (With turrets a deck's requirement keeps rising over its top, 1 e unit per 1.5 km x turW: only the turrets stand
    // there. Written into gStrat, with z and zTop, it put the march at 250 registers.)
    if (turW > 0.0) m0 -= flatK * max(q.h - f.topH, 0.0) / (1500.0 * turW);
    // Erosion: base = bite the lumps (torn, wispy); higher up = bite the cracks, keep the lumps (billows). Deep
    // convection keeps storm.y of it and bites far fewer cracks (dark veins over every storm otherwise).
    float billow = ty.shape.z * smoothstep(0.05, 0.4, hf) * (1.0 - 0.7 * deep);
    float erK    = ty.shape.y * cv2.look.z * mix(1.0, cv2.storm.y, deep) * max(1.0 - 0.6 * cv2.tune.y * rV, 0.1);
    float erBase = mix(1.0, 1.0 - 0.7 * cv2.high2.w, ty.look.z);
    // "Cumulus wisps" (cu2.y): where a cloud is evaporating its edge is ragged and soft, where it grows it is hard.
    // Small, weak clouds (fractus) are wispy all round; a formed cloud on the parts the shape volume's Perlin
    // channel picks (hundreds of metres to a km: one flank, a trailing shelf). There the erosion is 1.7x and the
    // edge sharpening is let go, so the detail noise tears the surface into shreds instead of dimpling it.
    float wispE  = cv2.cu2.y * (1.0 - smoothstep(0.05, 0.3, e)) * (1.0 - deep);
    // The surface's softness scales with the cloud: a small cell (low strength) reaches full density over its own
    // smaller margin. With one ramp for all (form.w), a small cumulus never left the ramp's foot: a translucent,
    // fuzzy blob whatever the edge sharpness, where a real one is as white and hard-edged as a large one.
    // The ramp steepens ABOUT the level the erosion cuts at (~half its strength), so the cloud's outline stays where
    // it was: steepened about zero, every small cloud grew by the margin the erosion used to remove, and a field of
    // separate cumulus closed up into a sheet.
    // "Cumulus edge" (cu4.y): the margin's density ramp x this on every low cloud with a convective share (a deck type's
    // humps ramped in over ~100-300 m: the soft, foggy flanks of issue 20261104_181014), less as the footprint grows.
    float phK     = cv2.form.w / mix(1.0, clamp(eS, 0.3, 1.0), convW)
                  * mix(1.0, cv2.cu4.y, smoothstep(0.1, 0.5, ty.look.z) * (1.0 - smoothstep(60.0, 600.0, fpM)));
    float phP     = 0.5 * erK * (1.0 - phK / cv2.form.w);
    // Denser upward (cloud water grows with height); precipitation loads the base.
    float sigK   = mix(0.6, 1.0, smoothstep(0.0, 0.4, hf)) * (1.0 + w.b * ty.look.w * (1.0 - hf)) * ty.shape.x * cv2.look.y;
    c.m0 = m0; c.hb = hb; c.baseAmp = baseAmp; c.lobeRef = lobeRef; c.bigL = bigL; c.deep = deep; c.lobeKL = lobeKL;
    c.aBase = aBase; c.aRamp = aRamp; c.AshK = AshK; c.phK = phK; c.phP = phP; c.subPix = subPix; c.PhSub = PhSub;
    c.sigK = sigK; c.billow = billow; c.erK = erK; c.erBase = erBase; c.wispE = wispE; c.rV = rV; c.fpFade = fpFade;
    // TURRETS (cu3, 2026-11-04, issue 20261104_181014): every convective cloud, not only the thick ones, takes the big
    // lobes' 3D field (the storm volume x2: 2.9 km / 1.4 km / 710 m / 360 m) as towers ON TOP of its own lobes, read
    // stretched upward and leaned (cv2FieldLow). Without them a cell was ONE dome (a 2D strength under a parabolic
    // profile) dimpled by lobes <= 475 m: "every cloud a gumdrop". Shallow puffs (< ~250 m) keep their small lobes alone.
    // Their amplitude is their own (not "Cumulus lobes (bottom/top)", the small billows' strength), lifted at the top as the
    // lobes are (liftK), and kept off the bottom by its own ramp in cv2FieldLow (a cumulus base stays flat).
    float tur = turW * (1.0 - deep) * fpFade
              * mix(0.5 * (1.0 + liftK) * smoothstep(250.0, 900.0, thickM), 1.0, flatK);
    c.tur = tur;
    // The bubbles' gate: the turrets' own gate without their strength (cu3.x), so "Cumulus turrets" 0 keeps the bubbles
    // (shape2, 2026-11-05; until then the bubbles rode on tur and went with it). The old min(tur / (0.5 cu3.x), 1), but from
    // ~60 m of thickness, not ~250 m: a small cloud is where the fine bubbles matter most (they were one smooth oval).
    c.bub = min(2.0 * ty.look.z * (1.0 - deep) * (1.0 - deep) * fpFade
                * mix(0.5 * (1.0 + liftK) * smoothstep(60.0, 400.0, thickM), 1.0, flatK), 1.0);
    c.thk = thickM;
    c.lobeB = lobeK * 0.75 * (1.0 + (1.5 + 0.4 * cv2.cu.w) * bigL) * (1.0 + liftK) + cv2.cu.y * 1.2
            + tur * 2.7 * (1.0 + 1.5 * cv2.cu3.w);
#ifndef SKY_LITE
    // (The bubbles, cv2FieldLow: m0 -> (m0 + x B) / (1 + x) with B <= 1.2 y, the lobes L added undivided: nothing where
    // m0 + x 1.2 y + (1 + x) L <= 0.)
    if (c.bub > 0.0) c.lobeB = c.lobeB * (1.0 + cv2.cu5.x) + cv2.cu5.x * 2.0 * cv2.cu5.y * (0.6 + 0.45 * cv2.cu5.z);
#endif
    return true;
}

// -- Anvil fibres (2026-11-05, storm3; "Anvil fibres", clouds_v2.anvil_fibres) -------------------------------------------
// An anvil is ice spreading from the tower's top and blown downwind: a smooth-ish sheet combed into fibres, thinning into
// streaks at its edge (refs cb_cluster_sunset, cb_lightning_layers). Carved by the cumulus' Worley erosion it was an
// eroded slab with holes ("swiss cheese", the user, 2026-11-05). The plate's erosion is now mostly gone (storm3b.x) and its
// extinction is combed by this: the shape volume's Perlin + Worley read STRETCHED along the (drifted) parallels, as the
// cirrus' fibres are (cv2HighSigma; the along-wind period divides the equator, storm3a.y: no seam), in absolute
// coordinates (an observer-relative stretch would slide as the eye moves). One fetch, only where the plate has density.
// Returns the density factor: gentle inside (0.55-1.1), fibres with clear gaps toward the skin (edge 0..1). Self-contained:
// it shares neither the cirrus' code nor its tunables.
// (The fetch alone: the anvil's veil reads its fray from it too, cv2AnvilSigmaP. w of the result: the lod.)
// (acrossK: x the across-wind frequency; the veil reads it 4x wider, fibres 2-3x longer than wide: at the plate's ~10:1
// the veil seen along the wind from 10 km was a sunburst of rays converging on the vanishing point.)
vec4 cv2AnvilFibreTex(vec3 wd, float h, float fpM, float acrossK, out float lod)
{
    float lon = atan(wd.y, wd.x), lat = asin(clamp(wd.z, -1.0, 1.0));
    float aK  = cv2.storm3a.z * acrossK;
    vec3  c   = vec3(lon * cv2.storm3a.y, lat * (R_EARTH * aK), h * (2.0 * aK));
    // The fibres meander (two waves along the wind, +-~3 spacings): straight, they read as ruled lines.
    c.y += 1.8 * sin(c.x * 0.79 + 1.3) + 1.1 * sin(c.x * 2.3 + c.y * 0.11);
    lod = cv2Lod(fpM, aK);
    return textureLod(cv2ShapeTex, c + vec3(0.37, 0.61, 0.19), lod);
}
float cv2AnvilFibre(vec3 wd, float h, float fpM, float edge)
{
    float lod;
    vec4  s   = cv2AnvilFibreTex(wd, h, fpM, 1.0, lod);
    float n   = clamp(0.5 + 2.5 * (s.a - 0.5) + 1.2 * (s.g - 0.45), 0.0, 1.0);
    float cut = mix(0.0, 0.42, edge);
    float k   = mix(0.55, 1.1, smoothstep(cut, cut + 0.45, n)) * smoothstep(cut - 0.05, cut + 0.2, n + 0.25 * (1.0 - edge));
    // Sub-pixel fibres average to their mean (the far field keeps the sheet's opacity).
    return mix(k, mix(0.85, 0.6, edge), smoothstep(1.5, 4.0, lod));
}

CV2Field cv2FieldLow(CV2Pos q, float detailAmt, float fpM, float stormProx, CV2Storm sg)
{
    CV2Field f;
    f.sigma = 0.0; f.hf = 0.0; f.msBright = 1.0; f.ambient = 1.0; f.deck = 0.0; f.topH = 0.0; f.thin = 0.0;
    f.rain = 0.0; f.soft = 0.0;
    CV2Carve c;
    bool cu = cv2LowA(q, detailAmt, fpM, stormProx, f, c);
    // A storm object's towers and shield (cv2StormGeom): their margin, in this field's units, joins the cumulus' as a
    // smooth max, and the carve's scalars blend toward the storm's where it dominates: the SAME lobes and erosion shape
    // both, from one set of fetches (2026-11-04, storm-rework.md). Past the lobes' reach the storm is not there at all.
    float mS = sg.m / cv2.stormU.y;
    if (mS > -2.0) {
        // (Weather v2: 1.5 km of billow x "Storm lobes"; it was "Cb lobes" x the old towers' radius, which a setting tuned
        // for those towers left at ~250 m: smooth walls and flat tops on every storm.)
        const float kCv2StormBillowM = 1500.0;
        float lobeS = cv2.stormU.w * kCv2StormBillowM / cv2.stormU.y
                    * mix(1.0, 0.3, smoothstep(300.0, 2000.0, fpM))     // (sub-pixel lobes average away)
                    * mix(1.0, 0.3, sg.deck);                           // (the anvil plate: gentle domes, not a cumulus deck)
        // (storm3, 2026-11-05) The anvil plate's share (x "Anvil fibres" up to 1): its lobes, erosion, skin and fibres.
        float plK  = sg.deck * min(cv2.storm3a.x, 1.0);
        float wS    = cu ? clamp(0.5 + (mS - c.m0) * 5.0, 0.0, 1.0) : 1.0;
        float hS    = cu ? max(0.2 - abs(mS - c.m0), 0.0) / 0.2 : 0.0;
        c.m0      = max(c.m0, mS) + hS * hS * 0.05;     // a smooth max (over 0.2 e units)
        // (The towers' base, cv2StormGeom's: recomputed, the ground is cached. Carried in CV2Storm it cost a register
        // across cv2LowA, and the march sits at the 128-register cliff.)
        c.hb      = mix(c.hb, q.h - (cv2.types[4].alt.x + cv2Ground(q.dirE) * 0.9), wS);
        c.baseAmp = mix(c.baseAmp, 200.0, wS);
        // (lobeRef < 0 carries the plate's share past the fetch: its lobes only BULGE (cv2FieldLow, after m), the lid's
        // gentle domes and the underside's pouches; cutting into a sheet a few hundred metres thick they holed it through,
        // the "swiss cheese". A negative lobeRef is inert where it is read, max(lobeRef - hb, 0).)
        c.lobeRef = mix(c.lobeRef, -plK, wS);
        c.bigL    = max(c.bigL, wS);
        // THE CUMULUS' MATERIAL (2026-11-06, the user: anvils read as another material next to the billows, meet the cumulus
        // at a seam, miss the bubbles and only scattered like cloud at density ~0.1): a storm is a thick cumulus of this
        // layer, not deep convection with its own surface. The storm gives the SHAPE (its margin: towers, lid, plate, the
        // overshoot's swell); the lobes, bubbles, billow erosion, density and lighting are the congestus type's, read
        // through the same path as every cumulus. (deep 0: the cumulus-scale shape and detail under the storm-scale lobes,
        // bigL, as on a thick cumulus; the core floor, ciHu2.y, keeps the bubbles from holing the plate.)
        c.deep    = max(c.deep, wS);
        // An anvil plate's lid is smooth-ish (spreading ice under the tropopause); its underside and edges billow.
        // Over a tower's head (sg.near) the lid is smoother still; toward the plate's outline (sg.cap) it keeps a
        // quarter fewer (an overshooting dome's smooth cap shared sg.cap until 2026-11-06, issue 20261105_201719).
        // (storm3, 2026-11-05: the plate's lid keeps storm3b.y of the lobes, its underside storm3b.z; plK blends from the
        // polished plate's 0.4 / 1.)
        float nr  = min(sg.near, 1.0);
        float lkP = mix(mix(1.0, cv2.storm3b.z, plK), mix(0.4 - 0.35 * nr, cv2.storm3b.y * (1.0 - 0.8 * nr), plK),
                        smoothstep(0.55, 0.95, sg.hf));
        c.lobeKL  = mix(c.lobeKL, lobeS * mix(1.0, lkP, sg.deck) * (1.0 - 0.75 * sg.cap), wS);
        c.aBase   = mix(c.aBase, 0.3, wS);
        c.aRamp   = mix(c.aRamp, 600.0, wS);
        c.AshK    = mix(c.AshK, 0.0, wS);
        // The surface ramps in over stormU.z metres whatever the storm's size (a tower is km wide, its skin is not).
        // (Never sharper than ~1.5 footprints: a cut-out edge a pixel cannot resolve stipples from afar.)
        c.phK     = mix(c.phK, cv2.stormU.y / max(cv2.stormU.z, 1.5 * fpM), wS);
        c.phP     = mix(c.phP, 0.0, wS);
        c.subPix  = mix(c.subPix, 0.0, wS);
        // (sg.sig: 1, or a hurricane CDO's top density factor.)
        c.sigK    = mix(c.sigK, mix(0.6, 1.0, smoothstep(0.0, 0.4, sg.hf)) * cv2.types[3].shape.x * cv2.look.y * sg.sig, wS);
        c.billow  = mix(c.billow, cv2.types[3].shape.z * smoothstep(0.05, 0.4, sg.hf) * 0.3, wS);
        c.erK     = mix(c.erK, cv2.types[3].shape.y * cv2.look.z * cv2.storm.y * mix(1.0, cv2.storm3b.x, plK), wS);
        c.erBase  = mix(c.erBase, 1.0 - 0.7 * cv2.high2.w, wS);
        c.wispE   = mix(c.wispE, 0.0, wS);
        c.rV      = mix(c.rV, 0.0, wS);
        float fpS = mix(1.0, 0.3, smoothstep(300.0, 2000.0, fpM));
        c.fpFade  = mix(c.fpFade, fpS, wS);
        // (The storm lobes' largest addition in practice: 2 x lobeS x ~0.7 (the billow channels' spread is ~0.12, the
        // storm-scale lobes x 2.5 plus the fine ones on their crowns), x (1 + x) with the cumulus bubbles, + their own reach
        // (cv2LowA's bound). The cumulus' lobe formula, 3.35 lobeS, kept every point within ~6 km of a storm's surface
        // fetching: coarse view reads x1.8 looking down on an anvil plate.)
        float bubS = 0.0;
#ifndef SKY_LITE
        bubS = fpS * (1.0 - sg.deck) * (1.0 - sg.deck);
#endif
        c.lobeB   = mix(c.lobeB, lobeS * 1.4 * (1.0 + cv2.cu5.x * bubS) + cv2.cu5.x * bubS * 2.0 * cv2.cu5.y * (0.6 + 0.45 * cv2.cu5.z), wS);
        c.tur    *= 1.0 - wS;
        c.bub     = mix(c.bub, bubS, wS);
        c.thk     = mix(c.thk, 4000.0, wS);
#ifndef SKY_LITE
        // A HURRICANE'S CDO (2026-11-05, the user: "a nice smooth but textured swirl, right now it is a bumpy mess"): the
        // plate of a cyclone's central dense overcast is a smooth sheet of outflow ice, not a storm's billowing anvil: its
        // lobes and billow erosion (dark erosion pits all over it from orbit) mostly go. "Hurricane top smoothing" (ciHu.z).
        if (gCv2CdoK > 0.0) {
            float smK = cv2.ciHu.z * wS * gCv2CdoK;
            c.lobeKL *= 1.0 - smK;
            c.erK    *= 1.0 - 0.94 * smK;
            c.billow *= 1.0 - min(smK * 1.18, 1.0);
            c.lobeB  *= 1.0 - smK;
        }
#endif
        f.hf       = mix(f.hf, sg.hf, wS);
        f.topH     = mix(f.topH, sg.top, wS);
        f.msBright = mix(cu ? f.msBright : 1.0, cv2.types[3].look.x, wS);
        f.ambient  = mix(cu ? f.ambient : 1.0, cv2.types[3].look.y, wS);
        f.deck     = mix(f.deck, sg.deck, wS);
    } else if (!cu) return f;
    float m0 = c.m0, hb = c.hb, baseAmp = c.baseAmp, lobeRef = c.lobeRef, bigL = c.bigL, deep = c.deep, lobeKL = c.lobeKL;
    float aBase = c.aBase, aRamp = c.aRamp, AshK = c.AshK, phK = c.phK, phP = c.phP, subPix = c.subPix, PhSub = c.PhSub;
    float sigK = c.sigK, billow = c.billow, erK = c.erK, erBase = c.erBase, wispE = c.wispE, rV = c.rV, fpFade = c.fpFade;
    float tur = c.tur;
    float mEnv = m0;   // the envelope, before the bubbles divide it (the core floor, below)
    {
        // Before the shape fetch, bounded as if the base bumps lifted this point all the way (the
        // gBase and the lobe damping only fall with height): the lobes add at most ~0.6 x 1.25 lobeK.
        float hbMax = hb + 2.2 * baseAmp;
        float gB    = 0.3 * (1.0 - smoothstep(0.0, 350.0, hbMax));
        if (m0 - gB * gB / 0.3 + c.lobeB <= 0.0 && subPix <= 0.0) return f;
    }
    vec3 wd = cv2Drift(q.dirE);
    vec3 rS = cv2Mat(q.rSeaE);

    // The shape volume tiles, and a sphere slices it: wherever the local horizontal plane holds a short lattice
    // direction of the tile (an axis, a face or body diagonal), the slice repeats along it every 1-1.7 periods and
    // the puffs lie in rows of the same clouds (user snapshot 2026-10-04, 9.6 N 134.6 W: the equator always holds
    // the z axis, and there the drifted frame's x axis too). It is read in a rotation of the tile chosen by the CPU
    // at the observer, clear of such alignments (a pure function of position and time; a change of frame reshapes
    // the lobes, never where the clouds are). Tried: a per-sample warp breaking the lattice (Perlin or analytic)
    // flipped this function to ~220 registers (+5-9 ms); a cross-fade between two frames cost 0.7 ms while idle.
    // A change of frame cross-fades (2026-11-06, issue_20261105_202810 "Sudden shape change"): for ~90 frames a falling
    // share of the RAYS (shapeB[0].w; IGN of the invocation, a new offset each frame: a ray -- one invocation of the view
    // march -- sees one frame's field whole) take the old frame, and the history blends the two images. Picked per point,
    // a ray crossed a random mix of two fields: a foggy mush for the fade's length; a per-ray global set by the march put
    // it at 253 registers (the invocation id is a special register). (Each use selects its product, not the matrix: a
    // selected mat3 + anchors held across the reads put the march at 250 registers; the UBO operands need none.)
    bool  useB = cv2.shapeB[0].w > 0.0
              && fract(52.9829189 * fract(dot(vec2(gl_GlobalInvocationID.xy), vec2(0.06711056, 0.00583715))) + 0.6180340 * cv2.frame.x)
                 < cv2.shapeB[0].w;
    vec3  vS  = rS + wd * max(lobeRef - hb, 0.0);
    vec3  rSA = useB ? mat3(cv2.shapeB[0].xyz, cv2.shapeB[1].xyz, cv2.shapeB[2].xyz) * vS
                     : mat3(cv2.shapeRotA[0].xyz, cv2.shapeRotA[1].xyz, cv2.shapeRotA[2].xyz) * vS;
    vec4  s   = textureLod(cv2ShapeTex, (useB ? cv2.shapeB[3].xyz : cv2.anchorShapeA.xyz) + rSA * cv2.anchorShape.w,
                           cv2Lod(fpM, cv2.anchorShape.w));
    float lobes = (s.g * 0.6 + s.b * 0.25 + s.a * 0.15) - 0.47;
    // Deep convection's lobes are storm.x times larger: at the cumulus scale a 12 km storm was a
    // pile of small bubbles, a "mountain" rather than towers.
    float turL = 0.0;   // the turrets' share of the lobes (added after the base bumps: a cumulus base stays flat)
    if (bigL > 0.02 || tur > 0.02 || c.bub > 0.0) {
        // (x2 of the storm anchor -- an integer, so the tile stays in register: lobes of 5, 2.6 and 1.3 km at 12x.)
        // (The storm anchor carries the lobes' FLOW: anchorStormA rises along the observer's up, CPU side, as the
        // towers' own did up their columns: SatelliteSimCloudsV2.cpp.)
        // Turrets: the field read slower upward (cu3.y: columns taller than wide) and leaned downwind with the cells
        // (cu3.z x form.y; the cells are read upwind with height, cv2LowA's mp). A shift along the sample's own up and
        // east by its own height: no observer dependence, the anchored tile stays in register.
        vec3 rT = rSA;
        if (tur > 0.0 || c.bub > 0.0) {
            float hP    = max(hb, 0.0);
            vec3  eastT = vec3(-wd.y, wd.x, 0.0) * inversesqrt(max(dot(wd.xy, wd.xy), 1e-6));
            vec3 dT = eastT * (cv2.cu3.z * cv2.form.y * hP) - wd * (cv2.cu3.y * hP);
            rT += useB ? mat3(cv2.shapeB[0].xyz, cv2.shapeB[1].xyz, cv2.shapeB[2].xyz) * dT
                       : mat3(cv2.shapeRotA[0].xyz, cv2.shapeRotA[1].xyz, cv2.shapeRotA[2].xyz) * dT;
            // BUBBLE WARP ("Cumulus bubble warp", ciHu2.z, m; 2026-11-05): the bubbles are a union of true spheres, and from
            // below their faces and creases read as circles and polka dots (harness: gone with the bubbles off, kept with
            // the fine ones off). The read is displaced by the cumulus lobes already fetched (s: Worley G, B and the Perlin,
            // each ~+-0.1-0.2 about its mean), so each sphere becomes a lumpy blob. No fetch; the tile stays in register.
            rT += (vec3(s.g, s.b, s.a) - vec3(0.45, 0.45, 0.5)) * (4.0 * cv2.ciHu2.z);
        }
        vec4 s2 = textureLod(cv2ShapeTex, ((useB ? cv2.shapeB[4].xyz : cv2.anchorStormA.xyz) + rT * cv2.anchorStorm.w) * 2.0,
                             cv2Lod(fpM, cv2.anchorStorm.w * 2.0));
        // "Storm cumulus fine lobes" (cu.w): the cumulus-scale lobes stay ON the storm-scale ones, strongest on
        // their crowns (big lobes carrying small ones). With the storm-scale noise alone (storm.x, 12x at the user's
        // settings: 10 km lobes) deep convection was smooth, lobeless mounds and pillars.
        // Read at 2x the storm coordinates (period / 8, 16, 32 of the storm scale: 5, 2.6 and 1.3 km at 12x), the
        // lobes a few km across that a congestus mass is made of. Amplitude follows size (x2.5 on them): at the
        // cumulus lobes' strength a 5 km lobe moved the surface by a few hundred metres, i.e. not at all.
        // (Tried: the storm scale itself on thick cumulus, smooth blobs; a third fetch 4x finer, 219 registers.)
        float lobS = (s2.g * 0.6 + s2.b * 0.25 + s2.a * 0.15) - 0.47;
        // (Skewed: the positive excursions grow with cu3.w, so a few turrets stand well over the rest.)
        if (tur > 0.0) lobS += cv2.cu3.w * max(lobS, 0.0) * 1.5;
        lobes = mix(lobes, 2.5 * lobS + cv2.cu.w * lobes * (0.35 + 1.3 * clamp(lobS + 0.12, 0.0, 0.5)), bigL);
        turL  = tur * 2.5 * lobS;
#ifndef SKY_LITE
        // CUMULUS BUBBLES (cu5, 2026-11-04, the structure round of issue 20261104_181014): a cumulus is a cluster of thermal
        // bubbles, not one dome. The billow shape bake's R is the signed distance to a heavy-tailed union of spheres (radii
        // 0.08-0.9 of an eighth of the period: at the user's 1.9 km x 12 / 2, ~110 m to 1.3 km; cloud_v2_noise.comp
        // bubbleSdf), read here with the turrets (the same fetch, the same upward stretch and lean). The dome's margin is
        // DIVIDED by 1 + x and the bubbles added x times: the dome keeps the cloud where it was (the envelope: where, how
        // big, how high) while the bubbles, the turrets and the small lobes, all un-divided, now shape it at x + 1 times their
        // old weight against it -- stacked sub-cells of different sizes and heights, heads wider than their stems, creases
        // and concavities between them, masses splitting into fragments. (Added to the dome undivided, the bubbles had to
        // out-steepen it, and their smooth faces swallowed the lobes: sausages, 2026-11-04.) Faded out by a footprint of
        // ~1 km (the far field keeps its dome). Rides on the turret fetch, gated on its own (c.bub): "Cumulus turrets" 0 keeps them.
        if (c.bub > 0.0) {
            float gB  = c.bub * cv2.cu5.x * (1.0 - smoothstep(150.0, 1000.0, fpM));
            // BUBBLE HIERARCHY (shape2, 2026-11-05; clouds.md "Bubble hierarchy"). (a) Base flatness (cu5.w, fract): the
            // bubbles' share fades toward the base, so a dense field stands on ONE flat-ish base (the dome's) with distinct
            // towers over it, not a sheet draped over bubble-shaped bumps.
            gB *= 1.0 - fract(cv2.cu5.w) * (1.0 - smoothstep(0.0, max(0.4 * c.thk, 150.0), hb));
            float sdB = 1.2 * (s2.r - 0.5);   // the big bubbles' signed distance (eighth-periods of this read)
            // (b) Lobes on lobes (cu5.z): the same bubble volume read 4x finer (bubbles ~30-330 m) shapes a shell of
            // +-shF around the big bubbles' surface, sd = clamp(sdF, sdB - shF, sdB + shF): deep inside / far outside the
            // big bubble nothing changes, at its surface the fine bubbles carve and add (cauliflower on the cauliflower).
            // The fine distance is x3 its geometric value (0.9 = 3 x 1.2 / 4): at its true scale it moved a surface by tens
            // of metres and the views did not change (harness A/B, 2026-11-05).
            // (c) Size follows the cloud (cu5.w, floor / 1000): on a small cloud the big bubbles are bigger than the cloud (it was ONE
            // smooth oval slice of one sphere); their distance is scaled toward 0 by the cloud's thickness, and a small
            // cloud is built of the fine bubbles alone, a big one of big bubbles carrying the fine ones.
            float shF = 0.45 * cv2.cu5.z * (1.0 - smoothstep(80.0, 400.0, fpM)) * (1.0 - deep);
#ifndef CV2_NO_FINE_BUBBLES
            if (shF > 0.0) {
                float sdF = 0.9 * cv2.cu5.z * (textureLod(cv2ShapeTex, ((useB ? cv2.shapeB[4].xyz : cv2.anchorStormA.xyz) + rT * cv2.anchorStorm.w) * 8.0
                                              + vec3(0.41, 0.17, 0.73), cv2Lod(fpM, cv2.anchorStorm.w * 8.0)).r - 0.5);
                sdB *= mix(1.0, smoothstep(250.0, 1800.0, c.thk), floor(cv2.cu5.w) * 0.001);
                sdB  = min(max(sdF, sdB - shF), sdB + shF);
            }
#endif
            m0 = (m0 + gB * sdB * (2.0 * cv2.cu5.y)) / (1.0 + gB);
        }
#endif
#ifndef SKY_LITE
        // STORM BUBBLES (storm3, 2026-11-05): a tower is the same thermal-bubble cumulus at storm scale (ref
        // cb_lightning_layers: cauliflower towers): the bubbles above, from the same fetch, on the storm margin
        // (wispE < 0: cv2FieldLow's storm block), and the turrets' skew on its lobes (a few crowns standing proud).
        // (They carve, and bulge at most 0.1 margin units (~400 m) past the storm's surface: added freely as on a cumulus,
        // a tower's margin, kilometres deep in metres / 4000, let them stand as detached blobs up to ~2 km out, a debris of
        // puffs round every tower and along the anvil's edge.)
        if (wispE < 0.0) {
            float gS = -wispE * cv2.cu5.x;
            m0 = min((m0 + gS * (s2.r - 0.5) * (2.4 * cv2.cu5.y)) / (1.0 + gS), m0 + 0.1);
            lobes += bigL * 3.75 * cv2.cu3.w * max(lobS, 0.0) * -wispE;
        }
#endif
        s = mix(s, s2, deep);
    }
    // Inverted Worley at 4/8/16 cells (G: 1.75 km, 875 m, 440 m lobes for the 7 km period) and
    // 8/16/32 (B), with a little Perlin (A) so the lobes are not all spheres. ~zero mean.
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
    float A     = lobeKL * mix(aBase, 1.0, smoothstep(0.0, aRamp, hbE));
    float m     = m0 + (A + AshK) * lobes * 2.0 + turL * mix(0.3, 1.0, smoothstep(0.0, aRamp, hbE)) - gBase * gBase / 0.3;
#ifndef SKY_LITE
    if (lobeRef < 0.0) m -= lobeRef * A * 2.0 * max(-lobes, 0.0);   // (storm3: an anvil plate's lobes only bulge)
#endif
#ifndef SKY_LITE
    // CUMULUS CORE ("Cumulus core", ciHu2.y; 2026-11-05, issue 20261105_084815's follow-up: "keep turrets contained a bit so
    // that they don't entirely tear up clouds into tiny pieces. We still need some big continuous clouds"). The bubbles divide
    // the envelope's margin by 1 + x and the turrets and lobes are signed: together, deep inside a big mass, they cut it through
    // into a field of fragments. Toward the envelope's interior the margin blends toward max(m, envelope): the carving gets
    // shallower with depth, so the surface keeps the bubbles' shape (the blend is 0 at the envelope's skin) but a cut cannot
    // reach through a big mass. (A hard floor, max(m, envelope - D), bared the envelope's smooth inner shell wherever the
    // bubbles cut past D: paper cut-outs and gumdrops, harness 2026-11-05.) The positive side (towers) is untouched; cumulus
    // only (deep convection keeps its own surface).
    if (cv2.ciHu2.y > 0.0) m = mix(m, max(m, mEnv), min(cv2.ciHu2.y, 1.0) * (1.0 - deep) * smoothstep(0.0, 0.25, mEnv));
#endif
    float Ph    = clamp(m * phK + phP, 0.0, 1.0) * smoothstep(0.0, 60.0, hbE);
    Ph = mix(Ph, PhSub, subPix);
    if (Ph <= 0.0) return f;

    // Inside, a little of the macro noise for density variation (the surface is the margin).
    float d = Ph * mix(0.75, 1.0, s.r);
#ifndef SKY_LITE
    // (storm3) An anvil plate's ice combed into fibres (cv2AnvilFibre), inside the cloud only (one fetch where the plate
    // has density, not over the band its surface may reach): toward its skin (Ph small) the fibres open into wisps.
    if (lobeRef < 0.0) {
        d *= mix(1.0, cv2AnvilFibre(wd, q.h, fpM, 1.0 - Ph), -lobeRef);
        if (d <= 0.0) return f;
    }
#endif

    if (detailAmt >= 0.0) {
        float df = 0.45;   // the detail's mean (inverted-Worley fBm), when it is not fetched
#ifdef CV2_DETAIL_BINDING
        if (detailAmt > 0.0) {
            vec3  dist = (vec3(s.a, s.g, s.b) - vec3(0.5, 0.45, 0.45)) * 0.35;
            // (Not at deep ~ 1, a storm's own surface: only the storm-scale detail is used there.)
            vec4  dn   = (deep < 0.99) ? textureLod(cv2DetailTex, cv2.anchorDetail.xyz + rS * cv2.anchorDetail.w + dist,
                                                    cv2Lod(fpM, cv2.anchorDetail.w)) : vec4(0.45);
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
            float fS = clamp(0.25 - 0.35 * cv2.tune.y * rV + 0.35 * (1.0 - smoothstep(8.0, 80.0, fpM)), 0.0, 0.7);   // the fine octave's share, by region; more of it up close
            df = mix(0.45, dn.r * (1.0 - fS) + dn.g * fS, detailAmt);
        }
#endif
        float wz     = max(wispE, cv2.cu2.y * smoothstep(0.45, 0.62, s.a) * (1.0 - 0.6 * billow));
        float erode  = min(mix(df, 1.0 - df, billow) * erK * (1.0 + 0.7 * wz), 0.92);
        // Erosion shapes the SURFACE. Deep inside (d high) only form.z of it applies: at full
        // strength it punched holes through the interior, and from inside a cloud the sky showed.
        erode *= mix(1.0, cv2.form.z, smoothstep(0.35, 0.85, d));
        // A cumulus base is the condensation level: flat. The base erosion ("bite the lumps") dimpled
        // every underside, and a field seen from below read as mammatus (user snap 1, pass 13; the
        // pass-11 shape too). "Base flatness" keeps 1 - 0.7 x it of the erosion in the lowest ~400 m.
        erode *= mix(erBase, 1.0, smoothstep(0.0, 400.0, hbE));
        d = clamp((d - erode) / max(1.0 - erode, 1e-3), 0.0, 1.0);
        if (d <= 0.0) return f;
    }

    // Edge sharpness (the reference's noiseEdgeHardness): real cumulus surfaces are abrupt, but a
    // remapped noise ramps in over hundreds of metres and reads as fog. Applied AFTER erosion, so
    // the eroded lumps are what gets sharpened, and clamped so the interior saturates.
    // Relaxed toward 1 as the footprint grows: a surface a pixel cannot resolve should not be a
    // hard cut-out edge (orbit views read as paper cut-outs otherwise).
    // ... and tightened as it shrinks below ~80 m (x3 by 8 m; x4 cost another 0.2 of tstab's walk score at shot 5): the margin's slope across a weak cell's edge is gentle, so
    // its density ramps in over ~100 m whatever the sharpness — sub-pixel from afar, but a cloud a kilometre or two
    // from the eye was an out-of-focus blob (issue 20261012_235712: "muddy", "no fine detail").
    d = min(d * mix(cv2.extra.x * (1.0 + 2.0 * (1.0 - smoothstep(8.0, 80.0, fpM))), 1.0,
                    max(smoothstep(150.0, 1500.0, fpM), 0.8 * wispE)), 1.0);

    // (+ the rain floor above, if any: its share of the extinction is rain.)
    float sigR = f.sigma;
    // "Cumulus body" (cu4.x): cloud water grows adiabatically with height over the base (LWC ~ hb, extinction ~ LWC^2/3):
    // a 1.2 km cumulus is tau ~100 (30-60 with entrainment), where the old profile gave its bases 13-19 toward the Sun
    // (cloud-bottoms, 2026-11-04). Optically thick, a lobe shadows the next and the creases go dark. Not on a storm's
    // own surface (deep). (Here, not in cv2LowA's sigK: there it put the march at 248 registers.)
    f.sigma    = d * sigK * (1.0 + cv2.cu4.x * 2.0 * (1.0 - deep) * pow(max(hb, 0.0) * 0.001, 0.6667)) + sigR;
    f.rain     = sigR / max(f.sigma, 1e-9);
    // "Lobe occlusion" (mid3.z): the sky light by where the sample sits in the lobe noise: the creases between
    // lobes (low) see a narrow cone of sky, the tips (high) most of it; mean ~1. The probe 250 m up and the depth
    // under the column's top cannot tell a crease from a tip.
    // The creases dim the DIFFUSE sunlight as well (the multiple-scattering term; never brightened): occluding the sky
    // light alone left the creases in the Sun's colour and the lobes in the sky's, a two-colour noise pattern on the
    // underside of an overcast at a low Sun (issue 20261012_235159).
    float occL = 1.0 - cv2.mid3.z * clamp(-4.0 * lobes, -0.6, 1.0) * fpFade;
    f.ambient  *= occL;
    // (Toward the base only: on a sunlit top the creases went dirty grey, and the direct light there is not diffuse.)
    f.msBright *= mix(min(occL, 1.0), 1.0, f.hf);
    return f;
}

#ifdef CV2_MORPH_BINDING
// ── Review 18: the FAR CLOUD LAYER's column (cloud_v2_far.comp) ──────────────────────────────────────
// The low layer as cv2FieldLow places it, as a 2D column (review 19: the same blend of the cells and the imagery,
// at this footprint; kept in step BY HAND): its strength e (0 at the edge .. 1 well inside), the share of the
// pixel the unresolved cells cover (frac, cv2FieldLow's presFar), the type's extinction, base and top.
struct CV2Far { float e; float eRaw; float frac; float sigma; float base; float top; float msBright; float strat; float deep; float covE; };
CV2Far cv2FarColumn(CV2Pos q, float fpM)
{
    CV2Far r;
    r.e = 0.0; r.eRaw = -1.0; r.frac = 0.0; r.sigma = 0.0; r.base = 0.0; r.top = 0.0; r.msBright = 1.0; r.strat = 0.0; r.deep = 0.0; r.covE = 0.0;
    { vec3 dD = cv2NoiseDispAt(q.dirE, kWfLowH); q.rSeaE -= dD; q.seaProjE -= dD; }   // (as cv2Field)
    vec3  wd    = cv2Drift(q.dirE);
    vec3  sP    = cv2Mat(q.seaProjE);
    vec3  flowD = cv2FlowDisp(wd, vec3(0.0));   // as cv2Field evaluates it for every layer (gCv2FlowD)
    vec3  wdF   = cv2FlowWeatherDir(wd, flowD);
    float covSpan = max(cv2.cover.y - cv2.cover.x, 1e-3);
    float wCoarse = textureLod(cv2WeatherTex, wdF, CV2_WX_LOD(2.0)).r * cv2.look.x;
    if (wCoarse < cv2.cover.x * 0.25) return r;
    vec3  mp  = sP;
    vec3  mpF = mp + flowD;
    vec4  cl  = textureLod(cv2MesoTex, cv2.anchorCluster.xyz + mpF * cv2.anchorCluster.w, cv2Lod(fpM, cv2.anchorCluster.w));
    vec4  ce  = textureLod(cv2MesoTex, cv2.anchorCell.xyz + mp * cv2.anchorCell.w, cv2Lod(fpM, cv2.anchorCell.w));
    vec3  warp = (vec3(ce.a, cl.a, 0.5 * (ce.a + cl.a)) - 0.5) * cv2.extra.y;
    vec4  w    = cv2WeatherSmooth(wdF + warp);
    float cov  = clamp((w.r * cv2.look.x - cv2.cover.x) / covSpan, 0.0, 1.0);
    float covF = clamp((wCoarse - cv2.cover.x) / covSpan, 0.0, 1.0);
    if (cov <= 0.0 && covF <= 0.0) return r;
    CV2Type ty = cv2TypeAt(w.g);
    float strat = 1.0 - ty.look.z;
    float covT  = max(cov, 0.5 * covF);
    float lodCell = cv2Lod(fpM, cv2.anchorCell.w);
    float rimAmt  = 0.25 * (1.0 - smoothstep(0.5, 2.5, lodCell));
    float field = mix(0.5 + rimAmt * (cl.b - 0.5) + 0.125 + 0.3 * (ce.a - 0.5) + 0.2 * ce.g,
                      0.15 + 0.85 * sqrt(ce.g) * mix(0.6, 1.0, cl.r), ty.look.z);
    float fpF   = fpM * 2.0 * cv2.morph.w;   // the march's placement filter (2^-"Far-field sharpness" x 2)
    vec4  cl4   = textureLod(cv2MesoTex, (cv2.anchorCluster.xyz + mpF * cv2.anchorCluster.w) * 4.0
                             + vec3(0.37, 0.61, 0.13), cv2Lod(fpF, cv2.anchorCluster.w * 4.0));
    vec4  cl16  = textureLod(cv2MesoTex, (cv2.anchorCluster.xyz + mpF * cv2.anchorCluster.w) * 16.0
                             + vec3(0.71, 0.23, 0.47), cv2Lod(fpF, cv2.anchorCluster.w * 16.0));
    float fz    = ((cl.a - 0.5) * 0.5 + (cl4.a - 0.5) * 0.3 + (cl16.a - 0.5) * 0.2) / 0.035;
    float seaM  = 1.0 - smoothstep(5.0, 120.0, w.a * 8000.0);
    float zMs   = mix(fz, cv2MorphZ(wd, mpF, fpF, strat, seaM, cl.r), cv2.morph.x);
    field += 0.3 * cv2.morph.y * clamp(zMs, -2.5, 2.5);
    float thr   = 1.0 - covT - cv2.farTune.y;   // review 24: "Far cloud layer coverage bias"
    float sigU = mix(0.03, 0.03 + 0.2 * smoothstep(0.0, 3.0, lodCell), ty.look.z) * max(cv2.farTune.w, 0.05);
    // WHERE THE MARCH DRAWS CLOUD (issue 20261021_173412). The march does not draw cloud wherever the field passes the
    // coverage threshold: a column is cloud where its density at the cloud's widest point survives the EROSION. That
    // density is the convective profile's margin (e capped by the cell, less the profile's narrowest requirement
    // reqMin), steepened about the erosion level (phK / phP), or for cells below the pixel the sub-pixel share
    // (presFar), x the macro noise's mean (0.875); the mean erosion takes "Detail" (look.z) x the type's strength. At the
    // shipped "Detail" 0.77 that moves the edge well inside the field's: the layer had ~2x the march's cloud from
    // 1000-2000 km (frames 72% against 45% at bias 0; at the review-24 bias +0.12, 90%, r 0.59), and with "Detail" 0
    // the march covers far MORE than the field (94%). So the edge here is found the way cv2FieldLow draws it: the
    // smallest margin xe = field - thr at which that eroded density is positive (bisection on the same terms). Kept in
    // step with cv2FieldLow BY HAND (small-cumulus lift, eS / capS, reqMin, flatK, convW, phK / phP, subPix / presFar,
    // erosion with billow at the cloud's middle and the edge wisps; the lobes average out).
    float xe0  = field - thr;
    float xeMin = 0.0;
    float eLift;
    {
        float thr0   = 1.0 - covT;
        float deepE  = smoothstep(0.45, 0.85, w.g);
        float cellsC = cv2MesoSmoothG(cv2.anchorCell.xyz + mp * cv2.anchorCell.w, max(lodCell, 2.0));
        float cellsB = deepE > 0.02
                     ? mix(cellsC, cv2MesoSmoothG(cv2.anchorCell.xyz + mp * cv2.anchorCell.w, max(lodCell, 4.0)), deepE)
                     : cellsC;
        float fieldB = 0.15 + 0.85 * sqrt(min(cellsB * 1.8, 1.0)) * mix(0.6, 1.0, cl.r) + 0.08 * clamp(fz, -2.5, 2.5);
        float eB     = clamp((fieldB - thr0) / 0.55, 0.0, 1.0);
        float reqMin = 0.2 * smoothstep(0.1, 0.6, eB);
        float capS   = 0.3 + 0.7 * mix(ce.g, min(cellsC * 1.8, 1.0), deepE);
        float lowCov = max(1.0 - smoothstep(0.25, 0.65, covT), cv2.cu4.z * smoothstep(0.02, 0.2, w.b)) * cv2.cu.y
                       * smoothstep(0.1, 0.5, ty.look.z);
        float flatK  = ty.alt.z * (1.0 - lowCov);
        float convW  = mix(ty.look.z, 1.0, lowCov) * (1.0 - flatK);
        float subPix = smoothstep(1.0, 3.0, lodCell) * ty.look.z;
        float tE     = cv2.tune.x;
        float tP     = cv2.tune.x * (1.0 - smoothstep(0.0, 3.0, lodCell));
        float sigM   = 0.03 + 0.2 * smoothstep(0.0, 3.0, lodCell);           // the march's own presFar spread
        float rVE    = clamp((cl4.a - 0.5) * 9.0, -1.0, 1.0);
        float billE  = ty.shape.z * (1.0 - 0.7 * deepE);
        float erK    = ty.shape.y * cv2.look.z * mix(1.0, cv2.storm.y, deepE) * max(1.0 - 0.6 * cv2.tune.y * rVE, 0.1);
        float lo = -0.6, hi = 0.6;
        [[dont_unroll]] for (int it = 0; it < 10; ++it) {
            float x   = 0.5 * (lo + hi);
            float eM  = clamp(max(x, min(x + tE, tE + 0.6667 * x)) / 0.55, 0.0, 1.0);
            float eS  = min(eM, capS);
            float m   = mix(eS - reqMin, eM, flatK);
            float phK = cv2.form.w / mix(1.0, clamp(eS, 0.3, 1.0), convW);
            float phP = 0.5 * erK * (1.0 - phK / cv2.form.w);
            float Ph  = clamp(m * phK + phP, 0.0, 1.0);
            float pS  = clamp(0.5 + (x + tP) / (2.5 * sigM), 0.0, 1.0) * 0.8;
            float d   = mix(Ph, pS, subPix) * 0.875;
            float wz  = max(cv2.cu2.y * (1.0 - smoothstep(0.05, 0.3, eM)) * (1.0 - deepE), cv2.cu2.y * 0.2 * (1.0 - 0.6 * billE));
            float er  = min(mix(0.45, 0.55, billE) * erK * (1.0 + 0.7 * wz), 0.92) * mix(1.0, cv2.form.z, smoothstep(0.35, 0.85, d));
            if (d > er && d > 0.0) hi = x; else lo = x;
        }
        xeMin = hi;
        eLift = max(xe0, min(xe0 + tE, tE + 0.6667 * xe0));
    }
    r.eRaw  = eLift / 0.55;
    r.e     = clamp(r.eRaw, 0.0, 1.0);
    // The share of the pixel: the march's eroded density grows from its edge over ~half the cells' spread (1.25 sigU);
    // while the cells resolve, its 3D lobes (zero mean, ~+-0.05 field units) move the edge both ways (lobeW).
    float lobeW = 0.1 * (1.0 - smoothstep(1.0, 3.0, lodCell));
    r.frac  = clamp((xe0 - xeMin + 0.5 * lobeW) / (1.25 * sigU + lobeW), 0.0, 1.0);
    r.covE  = covT;
    float tropo  = cv2Tropo(wd);
    float lift   = cv2Ground(q.dirE) * mix(0.6, 0.9, ty.look.z);
    float topMax = ty.alt.x + (ty.alt.y - ty.alt.x) * tropo + w.b * strat * 3500.0;
    vec2  bSh    = cv2BaseShift(ty, cl.a, w, cov, covF, cv2Ground(q.dirE), q.dirE);   // as cv2LowA (vertical, 2026-11-05)
    r.top   = topMax + lift + cv2BaseTopShift(bSh);
    r.base  = clamp(ty.alt.x + lift + bSh.x + bSh.y, lift + 150.0, max(r.top - 400.0, lift + 150.0));
    r.sigma = ty.shape.x * cv2.look.y;
    r.msBright = ty.look.x;
    r.strat = strat;
    r.deep  = smoothstep(0.45, 0.85, w.g);
    return r;
}
#endif

// ── The mid-level layer: altocumulus / altostratus (2.8-7 km) ────────────────────────────────────
// A second, independent field above the low system, so clouds overlap in altitude and cover: the
// low field alone put every cloud on the same flat base. Present only where the map has cloud (the
// map's brightness is the column's total) and a mid-level "moisture regime" (the cluster field's
// Perlin channel, 8-64 km) says so. Altocumulus — rounded cloudlets from the shape volume's 3D
// Worley octaves (220-875 m) in a thin lens-shaped sheet — where the low cloud is broken;
// altostratus — a smooth translucent sheet the sun shows through — where it is stratiform.
const float kCv2MidWanderM = 450.0;
float cv2MidSigma(CV2Pos q, float fpM, out float hfMid, out float topMid, out float deckMid)
{
    hfMid = 0.0; topMid = 0.0; deckMid = 0.0;
    // Mid-level wisps (vert2.x, 2026-11-05) reach down to 1.35 km over the ground.
    float hLoM = cv2.vert2.x > 0.0 ? 1350.0 : 2800.0;
    if (q.h < hLoM || q.h > 12000.0) return 0.0;
    float gl  = cv2Ground(q.dirE) * 0.8;                                  // follows the terrain
#ifdef CV2_ORO
    float liftO = clamp(gCv2Oro.x, -1200.0, 1600.0);                     // the lee wave's lift (orographic clouds)
#else
    const float liftO = 0.0;
#endif
    if (q.h < hLoM + gl || q.h > 5900.0 + gl + 1300.0 * max(cv2.mid2.y, 1.0) + 0.5 * max(liftO, 0.0)) return 0.0;
    vec3  wF  = cv2FlowWeatherDirAt(q);                                 // the low cloud's (flowed) map
    vec4  w   = textureLod(cv2WeatherTex, wF, CV2_WX_LOD(max(1.0, gCv2WxLod)));                     // mid decks are broad
    float cov = clamp((w.r * cv2.look.x - cv2.cover.x) / max(cv2.cover.y - cv2.cover.x, 1e-3), 0.0, 1.0);
    // (The spread below can place mid cloud where the local low coverage is 0, so no early out on cov
    // alone when it is on.)
    if (cov <= 0.0 && cv2.flow.y <= 0.0) return 0.0;
    vec4  cl  = textureLod(cv2MesoTex, cv2.anchorCluster.xyz + q.seaProjE * cv2.anchorCluster.w,
                           cv2Lod(fpM, cv2.anchorCluster.w));
    // Fades in with the coverage: the source map is a JPEG, and in clear areas its 8x8 blocks
    // (~40 km) sit just above or below the clear threshold. A full altostratus sheet wherever the
    // coverage was above 0 drew those blocks as straight-edged translucent rectangles.
    // Layer spread (cv2.flow.y): mid-level cloud keys on the weather over ~80 km (mip 4), not only the
    // low cloud under it, and thins over a low overcast. It used to appear only where (and wherever)
    // the low coverage was high: every layer stacked on the same spot.
    float covW   = clamp((textureLod(cv2WeatherTex, wF, CV2_WX_LOD(4.0)).r * cv2.look.x - cv2.cover.x)
                         / max(cv2.cover.y - cv2.cover.x, 1e-3), 0.0, 1.0);
    float covM   = mix(cov, max(cov, covW * 0.85), cv2.flow.y);
#ifndef SKY_LITE
    // ── Mid-level wisps (2026-11-05, vertical; "Mid-level wisps", vert2.x): the layer's second, LOW height mode. Thin
    // (260-460 m), sparse, wind-stretched shreds between the cumulus tops and the altocumulus (1.5-2.5 km + ground):
    // remnants of decayed tops and moist layers, where the low cover is broken-to-high but not overcast. The low field
    // cannot draw them (a margin bounded below by its base), so they are this layer's: one shape fetch on its period,
    // stretched 2.5x along the stand-in wind (east). Kept >= 260 m thick: the CDC's coarse reads are 160 m apart.
    if (q.h < 2800.0 + gl) {
        float crW  = clamp(0.5 + (cl.r - 0.5) * 4.4, 0.0, 1.0);
        float bWs  = 1500.0 + gl + 1000.0 * crW;
        float thW  = 260.0 + 200.0 * (1.0 - crW);
        if (q.h <= bWs - 150.0 || q.h >= bWs + thW + 150.0) return 0.0;
        float wReg = smoothstep(0.15, 0.45, covM) * (1.0 - smoothstep(0.85, 1.0, cov)) * cv2.vert2.x
                   * smoothstep(0.59 - 0.05 * cv2.vert2.x, 0.67 - 0.05 * cv2.vert2.x, cl.a);   // at 1 ~a fifth of the area, at 2 ~2/5
        if (wReg <= 0.0) return 0.0;
        // Stretched along the Earth's axis-normal plane: the drifted frame's z squeezed x0.4 (shreds ~2.5x longer east-west
        // than north-south at the equator, ~1.7x at 30 deg), on its own anchor (vert2.yzw, reduced on the CPU). A squeeze of
        // the eye-relative offset across the local wind was centred on the eye: the wisps slid and changed as the eye moved
        // (issue 20261105_084815).
        vec3  rW   = q.rSeaE * vec3(1.0, 1.0, 0.4);
        vec4  shW  = textureLod(cv2ShapeTex, cv2.vert2.yzw + vec3(0.29, 0.53, 0.11) + rW * cv2.anchorMid.w,
                                cv2Lod(fpM, cv2.anchorMid.w));
        float bW2  = bWs + 150.0 * clamp(2.5 * (shW.a - 0.5), -1.0, 1.0);
        float tW2  = bWs + thW + 150.0 * clamp(2.5 * (shW.g - 0.5), -1.0, 1.0);
        float zW   = (q.h - bW2) / max(tW2 - bW2, 150.0);
        if (zW <= 0.0 || zW >= 1.0) return 0.0;
        float lensW = smoothstep(0.0, 0.35, zW) * (1.0 - smoothstep(0.5, 1.0, zW));
        float str   = clamp((shW.b * 0.6 + shW.g * 0.4 - 0.64 + 0.12 * wReg) * 5.0, 0.0, 1.0);
        str         = mix(str, 0.08 * wReg, smoothstep(400.0, 3000.0, fpM));      // sub-pixel: their thin mean
        hfMid = zW; topMid = tW2; deckMid = 0.0;
        return lensW * str * min(wReg, 1.0) * 0.03 * cv2.atmo.z;
    }
#endif
    float clA    = cl.a, uMid = -1.0;
#ifdef CV2_MORPH_BINDING
    // Review 17: altocumulus organises in cells too — the closed-cell morphology (one read, the cube face the
    // point is on) blended into the regime field at the Perlin's spread (0.057 sd), weighted by "Morphology
    // (orbit)": from orbit the mid layer was the cluster Perlin's blotches.
    if (cv2.morph.x > 0.0) {
        vec3  wdM = cv2Drift(q.dirE), aw = abs(wdM);
        // At 1.7x the frequency, on its own anchor (farTune2.yzw): never scale an anchored coordinate by a
        // non-integer — frac(a) x 1.7 jumps 0.7 of a period as the observer crosses a period plane.
        vec3  aM  = cv2.farTune2.yzw + q.seaProjE * (cv2.anchorMorph.w * 1.7);
        vec2  uvM = (aw.x >= aw.y && aw.x >= aw.z) ? aM.yz : (aw.y >= aw.z ? aM.xz + vec2(0.37) : aM.xy + vec2(0.71));
        float uM  = textureLod(cv2MorphTex, uvM, cv2Lod(fpM, cv2.anchorMorph.w * 1.7) + 3.0).r;
        clA = mix(cl.a, 0.5 + 0.057 * 2.6 * (uM - 0.5) * 1.2, cv2.morph.x * 0.75);
        uMid = uM;
    }
#endif
    // "Mid layer field size" (mid3.x): the regime's own noise at that size on the flowed sphere (as the cirrus
    // regime's), in place of most of the cluster field's: sheets hundreds of km across, not a peppering of patches.
    if (cv2.mid3.x > 0.0) {
        vec3 rgm = wF * (R_EARTH * cv2.mid3.x);
        clA = 0.5 + (textureLod(cv2MesoTex, rgm + vec3(0.47, 0.19, 0.83), cv2Lod(fpM, cv2.mid3.x)).a - 0.5) * 0.85
            + (clA - 0.5) * 0.4;
    }
    float regime = smoothstep(0.5 - cv2.mid2.w, 0.68 - cv2.mid2.w, clA + 0.15 * (covM - 0.5)) * cv2.misc.w * smoothstep(0.0, 0.35, covM)
                 * mix(1.0, 0.55, cv2.flow.y * smoothstep(0.75, 1.0, cov));
    // The baked Perlin is 0.50 +- 0.057: read raw, the base spanned ~300 m, and the layer was one flat
    // sheet at ~4.5 km around the planet — seen edge-on from near its height, a dead-straight line
    // across every storm, with the towers below it seen dimmed through it (user snap 1, pass 17).
    // Stretched to +-2 sigma it ranges 3-5.6 km over the cluster scale.
    float cr    = clamp(0.5 + (cl.r - 0.5) * 4.4, 0.0, 1.0);
    float base  = 3000.0 + gl + 2600.0 * cr;
    float thick = mix(1200.0, 450.0, cr) * cv2.mid2.y;
    // ── Orographic clouds (2026-11-06, issue 20261105_174850; gCv2Oro, cv2OroField): the layer FOLLOWS the lee wave,
    // lifted by half the streamlines' lift over the ridges and their lee crests and sunk in the troughs (where its regime
    // thins to 15%). Where the lift passes the air's condensation lift (gCv2Oro.y x 1.3 .. 0.7 as the layer's cover covM
    // rises) the layer's cloud condenses into a LENS (lensC: a sharp rim over 120 m of lift): its vertical profile is a
    // saucer centred in the layer, half as thick as the lift's excess + 200 m (>= 160: the CDC's coarse reads are 160 m apart),
    // flatter below; its cloudlets merge (a dense texture of the same shape fetch, x2.2 contrast), x3.5 density (it was x1.5: thinner
    // since the lift is smoothed, a lens was a translucent grey film). Lenses form
    // in moist air even where the regime has no altocumulus (a lenticular in an otherwise clear sky), never with the mid
    // layer off ("Mid-level amount" 0) or no cover at all. Earth-fixed: they stay over the relief while the map drifts.
    // Finite lenses (2026-11-06, issue 20261105_201331: "too uniform"; continuous ribbons along every ridge): the air's
    // moisture at the layer is not even along a range. The cluster Perlin (the moist patches the wind carries, 8-64 km,
    // +-1.5 sd -> +-225 m of the condensation lift) and, below, the shape volume's base-wander channel (km scale, +-220 m)
    // break a crest's lens into separate almonds that form and fade as the patches drift through the standing wave.
    float lensC = 0.0, lensP = 0.0, lensV = 0.0, lensH = 1.0, tO = -1e4;
    if (liftO != 0.0) {
        base += 0.5 * liftO;
        tO = liftO - gCv2Oro.y * mix(1.3, 0.7, covM) + 150.0 * clamp((cl.a - 0.5) * 8.8, -1.5, 1.5);
        // (an upper bound for the early out: the terms below add up to 520 m)
        lensC = smoothstep(-520.0, 0.0, tO) * smoothstep(0.0, 0.15, covM) * smoothstep(0.0, 0.3, cv2.misc.w);
        regime *= 1.0 - 0.85 * smoothstep(100.0, 600.0, -liftO);
    }
    float regL = max(regime, 0.9 * lensC);
    if (regL <= 0.0) return 0.0;
    // The sheet's base and top wander by up to +-kCv2MidWanderM over a few km (the shape volume at the layer's own
    // period, already read for the cloudlets; 2026-10-27, issue 20261027_002051 shot 1): with only the cluster-scale
    // height (8-64 km) it was flat over the tens of km a view along it crosses, and seen edge-on from near its height
    // its top and base lined up into a straight translucent strip across a storm.
    if (q.h <= base - kCv2MidWanderM || q.h >= base + thick + kCv2MidWanderM) return 0.0;
    float strat = 1.0 - smoothstep(0.3, 0.55, w.g);                 // stratiform below -> As
    // Its own period (anchorMid): on the cumulus shape period, enlarging the cumulus lobes made the
    // altocumulus cloudlets huge too.
    vec4  sh    = textureLod(cv2ShapeTex, cv2.anchorMid.xyz + q.rSeaE * cv2.anchorMid.w,
                             cv2Lod(fpM, cv2.anchorMid.w));
    float bW    = base + kCv2MidWanderM * clamp(2.5 * (sh.a - 0.5), -1.0, 1.0);
    float tW    = base + thick + kCv2MidWanderM * clamp(2.5 * (sh.g - 0.5), -1.0, 1.0);
    float zm    = (q.h - bW) / max(tW - bW, 150.0);
    if (lensC > 0.0) {
        // The 4-32 km Perlin (the closed-cell volume's, on its own anchor; read only here, inside a lens): a crest's lens
        // in separate almonds 10-20 km long. Added to the lift alone it only frayed the rims of a crest lifted far past its
        // condensation level (still one ribbon down the range), so it also cuts.
        vec4  ceL = textureLod(cv2MesoTex, cv2.anchorCell.xyz + q.seaProjE * cv2.anchorCell.w,
                               cv2Lod(fpM, cv2.anchorCell.w));
        float brk = clamp((ceL.a - 0.5) * 8.8, -1.5, 1.5);
        tO   += 220.0 * clamp(2.5 * (sh.a - 0.5), -1.0, 1.0) + 200.0 * brk;
        lensC = smoothstep(0.0, 120.0, tO) * smoothstep(-0.4, 0.2, brk) * smoothstep(0.0, 0.15, covM) * smoothstep(0.0, 0.3, cv2.misc.w);
        // Its half-height grows with the lift's excess and swells and thins with the cloudlets' own lobes (x0.75 .. 1.25):
        // a lens of even thickness along its whole length was the flat ribbon.
        lensH = min((0.5 * max(tO, 0.0) + 200.0) * (0.75 + 0.5 * sh.g), 0.5 * thick + 0.5 * kCv2MidWanderM);
        lensV = (q.h - base - 0.5 * thick) / lensH;
        // A flatter base and a domed top (the saucer): rims over the outer 40%.
        lensP = 1.0 - smoothstep(0.6, 1.0, lensV < 0.0 ? -lensV / 0.7 : lensV);
        regL  = max(regime, 0.9 * lensC);
    }
    // A lens: thin at its edges and top and bottom, the cloudlets' own lobes shaping both faces. Over a lee-wave crest
    // the orographic lens' profile (lensP) instead: the cloudlets condense into one smooth-faced saucer, their texture
    // softened (x2.2 contrast, not 5) but kept, so it reads as the layer's cloud shaped by the relief.
    float lens  = (zm <= 0.0 || zm >= 1.0) ? 0.0
                : smoothstep(0.0, 0.3, zm + (sh.a - 0.5) * 0.4) * (1.0 - smoothstep(0.55, 1.0, zm + (sh.b - 0.45) * 0.5));
    lens        = mix(lens, lensP, lensC);
    if (lens <= 0.0) return 0.0;
    float ac    = clamp((sh.b * 0.7 + sh.g * 0.3 - (1.0 - regime * 0.8)) * 5.0, 0.0, 1.0);
    ac          = mix(ac, clamp(0.6 + (sh.b * 0.7 + sh.g * 0.3 - 0.45) * 2.2, 0.4, 1.0), lensC);   // (floor 0.4: from 0 it tore dark holes; x1.2 was flat paint, 201331)
    // Cloudlets far below a pixel become the haze of their coverage (as the low cells do).
    // Review 17: from orbit the cloudlets' haze is the closed cells at the regime's area fraction, not a flat sheet
    // (the mid layer over the whole low field was what flattened orbit views: low layer alone D 1.42 / slope 2.3,
    // with the mid layer 1.33 / 2.5, MODIS 1.5-1.7 / 1.5-2.3 — tools/cloud_stats.py).
    float haze  = regL * 0.45;
    if (uMid >= 0.0)
        haze = mix(haze, 0.55 * smoothstep(0.88 - regime, 1.12 - regime, uMid), cv2.morph.x);
    ac          = mix(ac, haze, smoothstep(400.0, 3000.0, fpM));
    float as_   = clamp(regime * 1.3, 0.0, 1.0) * mix(0.7, 1.0, sh.a);
    strat      *= 1.0 - lensC;
    hfMid       = clamp(mix(zm, 0.5 + 0.5 * lensV, lensC), 0.0, 1.0);
    topMid      = mix(tW, base + 0.5 * thick + lensH, lensC);
    // An orographic lens is flagged by a NEGATIVE deck share (2026-11-06, issue 20261105_201331): the march adds its own
    // body toward the Sun, which its light march cannot see (cloud_v2_march.comp, the key light).
    deckMid     = strat - lensC;
    return lens * mix(ac * 0.035, as_ * 0.004 * cv2.mid2.x, strat) * cv2.atmo.z * (1.0 + 2.5 * lensC);
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
// FIBROUS ICE (2026-11-05, the user: cirrus "can kind of look gooey ... more wispiness and general structure"; the
// reference, qa/waves/20261105/refs/cirrus_uncinus.png: fibrous streaks with hooked tails, thin bright filaments and
// clear sky between the bands). Self-contained so other ice (an anvil's fray, a cyclone's outflow) can reuse it.
//   p    the shape volume's coordinates, x ALONG the fibres (the caller stretches it), y across, z up; every factor
//        applied here to x is an integer, so a period the caller keeps seam-free stays so.
//   lodR the unclamped LOD of a read at p's own scale (log2 of footprint x cells per unit along y).
// Returns x: the filaments, 0..1 (ridged: thin bright lines where a stretched fBm crosses its median, dark between);
// y: the tufts, 0..1 (the elements: a less stretched fBm thresholded, so the streaks come in patches with clear sky
// round them, not as an even marbling); z: the tufts' noise in standard deviations (cirrocumulus and the heads use it);
// w: the tufts' billow cells (G, ~0.475 +- 0.12: cirrocumulus cloudlets).
// The strands are read in a frame DEFLECTED by the tufts (y += 0.12 max(sd + 0.3, 0)^2, one-sided: a two-sided
// deflection wound every strand like stirred paint): they bend round each element's head and
// fan out along its edges -- the hooks and feathers of uncinus -- instead of running as parallel contours of one field
// (the old look: Perlin contours, marbled). The baked Perlin (A) spreads 0.50 +- ~0.057.
vec4 cv2IceFibres(vec3 p, float lodR)
{
    // Tufts: 4x along, 1x across (elements ~3:1 where the fibres are ~40:1).
    vec4  t   = textureLod(cv2ShapeTex, vec3(p.x * 4.0, p.y, p.z) + vec3(0.31, 0.73, 0.17), max(lodR, 0.0));
    float tn  = (t.a - 0.5) * (1.0 / 0.057);
    // Strands: 4x across (filaments ~1/5 of a 700 m cell at the 11 km period), deflected by the tufts.
    float tp  = max(tn + 0.3, 0.0);
    // (Slow in height, 0.5x: a strand is a sheet hanging through the layer, sharp seen from below. At 2x its ridge moved
    // ~a cell across over the layer's depth and every filament seen from the ground was the blur of that.)
    vec3  pf  = vec3(p.x, p.y * 4.0 + 0.12 * tp * tp, p.z * 0.5);
    vec4  f1  = textureLod(cv2ShapeTex, pf, max(lodR + 2.0, 0.0));
    // Hair: 3x finer again, on the strands (branching, frayed ends), a little less deflected.
    vec4  f2  = textureLod(cv2ShapeTex, pf * vec3(3.0, 3.0, 3.0) + vec3(0.57, 0.11, 0.83) - vec3(0.0, 0.15 * tp * tp, 0.0),
                           max(lodR + 3.585, 0.0));
    float n1  = (f1.a - 0.5) * (1.0 / 0.057), n2 = (f2.a - 0.5) * (1.0 / 0.057);
    // Ridges: 1 on the median line, 0 by ~0.8 sd; sharpened. Sub-pixel ridges average to their area share (~0.35): faded
    // toward it with the reads' LOD, or a far streak shimmered.
    float r1  = 1.0 - smoothstep(0.0, 0.8, abs(n1));
    float r2  = 1.0 - smoothstep(0.0, 0.9, abs(n2));
    float fib = r1 * r1 * mix(0.45, 1.0, r2) + 0.3 * r2 * r2;
    fib = mix(fib, 0.32, smoothstep(1.5, 3.5, lodR + 2.0));
    float tuf = smoothstep(-0.3, 1.1, tn + 0.5 * (t.b - 0.479) * (1.0 / 0.119));
    return vec4(clamp(fib, 0.0, 1.0), tuf, tn, t.g);
}

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
    vec4  wc     = textureLod(cv2WeatherTex, wF, CV2_WX_LOD(max(2.0, gCv2WxLod)));
    // Cirrus lives with the weather systems: in the jet ahead of fronts and in the outflow of deep
    // convection; the subtropical highs are mostly free of it. The regime therefore follows the map's
    // coverage over the surrounding ~150 km (mip 4) as much as its own noise. On the noise alone the
    // high layer covered the globe uniformly, and from orbit it was an even field of splotches.
    vec4  wb     = textureLod(cv2WeatherTex, wF, CV2_WX_LOD(4.0));
    float span   = max(cv2.cover.y - cv2.cover.x, 1e-3);
    float covL   = clamp((wc.r * cv2.look.x - cv2.cover.x) / span, 0.0, 1.0);
    float covS   = clamp((wb.r * cv2.look.x - cv2.cover.x) / span, 0.0, 1.0);
    float sys    = max(covS, 0.7 * covL);
    vec4  cl     = textureLod(cv2MesoTex, cv2.anchorCluster.xyz + q.seaProjE * cv2.anchorCluster.w
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
    // Fall streaks: the ice lower in the layer lags the generating heads above it, more the further it has fallen (the
    // wind slackens below the jet core), so each tail CURVES away from its head -- the uncinus hook seen from the side --
    // and drifts a little across the wind as well (directional shear: a hook seen from below too). (Linear, 2 m a metre,
    // until 2026-11-05: straight slanted sheets.)
    float dep   = zb + thick - q.h;
    float shear = dep * (2.0 + dep * (cv2.ciHu.y / 600.0));   // ("Cirrus hooks" bends them)
    // "Cirrus direction warp" (tune.w, 2026-10-11): the across-wind coordinate displaced by three waves of whole
    // counts around the globe (no seam), so the streaks' heading swings by ~atan(0.7 x it) (rms) over 600-5700 km
    // and their spacing breathes; and the bundles break into patches (below). Every streak ran along the (flowed) parallels: from orbit, one set of stripes.
    float latW  = lat * R_EARTH + dep * dep * (cv2.ciHu.y / 2500.0);
    if (cv2.tune.w > 0.0)
        latW += cv2.tune.w * 0.45 * R_EARTH * (sin(7.0 * lon + 3.0 * lat + 1.3) / 7.0 + sin(13.0 * lon - 5.0 * lat + 4.1) / 13.0
                                               + sin(23.0 * lon + 8.0 * lat + 2.2) / 23.0 + sin(41.0 * lon - 13.0 * lat + 5.7) / 41.0
                                               + sin(67.0 * lon + 29.0 * lat + 0.4) / 67.0);
    vec3  cc  = vec3((lon * R_EARTH - shear) * cv2.high.y + cv2.high.z,
                     // meander: the cluster Perlin normalised by its spread (0.057), +-~0.35 period (+-0.7 until 2026-11-05:
                     // with the fibres' own deflection every streak wound like stirred paint)
                     latW * cv2.high.w + (cl.a - 0.5) * 6.0,
                     q.h * cv2.high.w);
    float lodR = log2(max(fpM * cv2.high.w * 128.0, 1e-6));
    float lod  = max(lodR, 0.0);
    // Bundles: the same volume read 4x coarser (swaths ~30 km across and a few hundred km long; the
    // CPU keeps the along-wind period's count around the equator a multiple of 4, so no seam).
    // Streaks come in bundles that converge and part, not as an even comb: at one scale, thresholded,
    // they read as regular sand ripples. From orbit, where the fibres are far below a pixel, the
    // bundles are what shows: long bands along the jet.
    vec4  sb     = textureLod(cv2ShapeTex, cc * 0.25 + vec3(0.13, 0.47, 0.29), max(lod - 2.0, 0.0));
    float bundle = smoothstep(0.43, 0.6, sb.a);
    // ... in patches (with the direction warp): unbroken, the bundles were evenly spaced stripes hundreds of km long.
    bundle *= mix(1.0, smoothstep(-1.2, 0.6, (cl.a - 0.5) / 0.057), min(cv2.tune.w * 2.0, 1.0));
    cc.y += (sb.g - 0.45) * 0.5;                          // streaks crowd together and fan apart (1.2 until 2026-11-05: marbling)
    float strat = 1.0 - smoothstep(0.3, 0.55, wc.g);
    float ccK   = smoothstep(0.5, 0.7, cl.g) * (1.0 - strat) * 0.8;

    // Fibrous ice (cv2IceFibres, 2026-11-05): tufts with clear sky round them, filaments inside, hooked by the
    // tufts' deflection. Until then: two octaves of the stretched Perlin, thresholded over 0.07 of a +-0.04 spread
    // (a soft ramp over the whole field: every streak a smooth ribbon, the gaps never clear), combed by a third --
    // marbled, "gooey" (the user, 2026-11-05).
    vec4  fb   = cv2IceFibres(cc, lodR);
    float lo   = mix(0.6, 0.12, regime);
    float el   = smoothstep(lo, lo + 0.2, fb.y) * mix(0.25, 1.0, bundle);
    float ci   = el * max(mix(0.9, mix(0.04, 2.2, fb.x), cv2.ciHu.x), 0.0);   // ("Cirrus fibres": 0 = even tufts)
    // Heads: the generating cells, where an element's tuft is strongest, are a denser, less fibrous core (the bright
    // comma of uncinus); lower in the layer the same core, lagged downwind by the fall-streak shear, is its tail.
    float head = smoothstep(0.75, 1.0, fb.y) * smoothstep(0.9, 1.8, fb.z);
    ci += el * head * mix(0.6, 2.4, smoothstep(0.3, 0.85, z)) * mix(0.6, 1.0, fb.x) * cv2.ciHu.y;   // ("Cirrus hooks")
    float cs   = regime * mix(0.6, 1.0, clamp(fb.z * 0.25 + 0.5, 0.0, 1.0)) * mix(0.6, 1.0, bundle)
               * mix(0.75, 1.1, fb.x);                    // (the veil fibrous too, faintly)
    float cu   = clamp((fb.w - mix(0.75, 0.55, regime)) * 5.0, 0.0, 1.0);
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
    // Seen from INSIDE the layer (2026-09-30): within a few km of the eye (a view footprint of 4-20 m) the
    // fibres and cloudlets, metres across and thresholded, drew puffy patches with hard gaps all around
    // the eye; an observer in cirrus sees a soft haze. The footprint is the VIEW's (the light march passes
    // it too), so this is near the eye only.
    float nearK = 1.0 - smoothstep(4.0, 20.0, fpM);
    ci = mix(ci, regime * 0.4 * mix(0.6, 1.0, bundle), nearK);
    cu = mix(cu, regime * 0.25, nearK);
    float prof = smoothstep(0.0, 0.25, z) * (1.0 - smoothstep(0.55, 1.0, z));
    float d    = prof * mix(mix(ci, cs, strat), cu, ccK);
    hfH  = z;
    topH = zb + thick;
    return d * mix(mix(0.0008, 0.0003, strat), 0.0016, ccK) * cv2.high2.x * mix(1.0, 0.6, far);
}

float cv2SqC(float x) { return x * x; }
uvec3 cv2Pcg3(uvec3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

// (Weather v2 step 5, 2026-11-06: the Cb tower lattice / storm-object towers (cv2ColumnSigma) and the anvil shields
// (cv2AnvilSigmaP, its veil) are gone; a storm's column and anvil are the cumulus field's margin, drawn from the
// convection cube: cv2StormGeom below.)
bool  gCv2AnvVeil = false;
float gCv2PlateA  = 0.0;   // the anvil plate's presence here (cv2StormGeom): the cirrus gives way to it
bool  gCv2ShSet = false;
vec4  gCv2Sh    = vec4(0.0);

// Debug view 7: when gCv2DbgWant is set, cv2Field records the density each layer added (low incl.
// rain, mid, anvil + Cb columns, high) in gCv2Dbg. The march used to call the layer functions a second
// time for it: every extra inlined copy of the field costs every sample registers (perf sprint).
bool gCv2DbgWant = false;
vec4 gCv2Dbg     = vec4(0.0);

// THE cloud field: the low system (stratus .. cumulonimbus, nimbostratus), the mid layer, the anvils
// and the high layer (cirrus family).
// ── Fog and dust (2026-09-29; replaced v1's fogMarchCS, a global 1.4 km noise haze) ──────────────
// Both follow the weather cube. They are NOT layers of cv2Field: every addition to the march's main loop
// (even an unused struct member) flipped cloud_v2_march.comp to ~224 registers, +19%; the march gives them
// a short march of their own after the clouds' (its state is dead by then), composited by distance.
// FOG: a layer over the SMOOTHED ground (cv2Ground, ~15 km): valleys below it fill deeper, ridges stand
// out of it. Radiation fog under a clear sky at night in valleys and low ground (it forms through the
// night and burns off as the Sun climbs past ~17 deg), advection / sea fog near water under a stratiform
// regime, and mist in rain; patchy on the cluster field. Lit like a cloud deck.
// DUST: over dry land (the map clear over ~80 km, not rain, not sea), in plumes on the coarse cluster
// field, falling off with height (scale height fog2.x). Returns the fog's extinction; dustS the dust's.
// A smooth value noise on the unit sphere's direction (float is fine at these ~200-1000 km scales).
float cv2DirNoise(vec3 p)
{
    vec3  i = floor(p), f = p - i;
    vec3  u = f * f * (3.0 - 2.0 * f);
    float a[8];
    for (int k = 0; k < 8; ++k) {
        uvec3 c = uvec3(ivec3(i) + ivec3(k & 1, (k >> 1) & 1, k >> 2));
        uint  h = c.x * 73856093u ^ c.y * 19349663u ^ c.z * 83492791u;
        h = (h ^ (h >> 16)) * 0x7feb352du;
        h = (h ^ (h >> 15)) * 0x846ca68bu;
        a[k] = float((h ^ (h >> 16)) & 0xFFFFu) / 65535.0;
    }
    return mix(mix(mix(a[0], a[1], u.x), mix(a[2], a[3], u.x), u.y),
               mix(mix(a[4], a[5], u.x), mix(a[6], a[7], u.x), u.y), u.z);
}

// ICE FOG / DIAMOND DUST (2026-09-29, user review): over the ice sheets (Antarctica, Greenland's high
// interior, the high Arctic's land) under a clear sky, a thin haze of ice crystals in the lowest few
// hundred metres (scale height kIceFogH), patchy on the cluster field. It carries the ice optics (halos,
// sundogs, arcs, pillars: the fog march in cloud_v2_march.comp). fog2.w = amount, sunE.w = its
// extinction at the ground (1/m). iceS = its extinction here; the caller integrates its height profile.
const float kIceFogH = 250.0;

float cv2FogDust(CV2Pos q, float fpM, out float dustS, out float fogTopH, out float fogHf, out float dustAgl,
                 out float iceS)
{
    dustS = 0.0; fogTopH = 0.0; fogHf = 0.0; dustAgl = 0.0; iceS = 0.0;
    if (cv2.fog.x <= 0.0 && cv2.fog.w <= 0.0 && cv2.fog2.w <= 0.0) return 0.0;
    float gF  = cv2Ground(q.dirE);
    float agl = q.h - gF;
    float topMax = max(max((cv2.fog.x > 0.0) ? 1.6 * cv2.fog.y : 0.0, (cv2.fog.w > 0.0) ? 4.0 * cv2.fog2.x : 0.0),
                       (cv2.fog2.w > 0.0) ? 6.0 * kIceFogH : 0.0);
    if (agl > topMax) return 0.0;
    vec3  wF   = cv2FlowWeatherDirAt(q);
    vec4  w1   = textureLod(cv2WeatherTex, wF, CV2_WX_LOD(max(1.0, gCv2WxLod)));
    vec4  w4   = textureLod(cv2WeatherTex, wF, CV2_WX_LOD(4.0));
    float span = max(cv2.cover.y - cv2.cover.x, 1e-3);
    float cov1 = clamp((w1.r * cv2.look.x - cv2.cover.x) / span, 0.0, 1.0);
    float cov4 = clamp((w4.r * cv2.look.x - cv2.cover.x) / span, 0.0, 1.0);
    float sea  = 1.0 - smoothstep(5.0, 60.0, gF);                  // the coast and the sea
    float sig  = 0.0;
    if (cv2.fog.x > 0.0 && agl < 1.6 * cv2.fog.y) {
        vec4  cl    = textureLod(cv2MesoTex, cv2.anchorCluster.xyz + cv2Mat(q.seaProjE) * cv2.anchorCluster.w,
                                 cv2Lod(fpM, cv2.anchorCluster.w));
        float sunS  = dot(q.dirE, cv2.sunE.xyz);                   // sin of the Sun's elevation here
        float night = 1.0 - smoothstep(0.0, 0.2, sunS);            // burns off by ~12 deg of Sun
        float gC    = textureLod(cv2WeatherTex, q.dirE, CV2_WX_LOD(3.5)).a * 8000.0;   // the ground over ~80 km
        float valley = smoothstep(20.0, 400.0, gC - gF);          // lower than its surroundings: cold air pools
        float strat = cov1 * (1.0 - smoothstep(0.35, 0.55, w1.g));  // an overcast, stratiform regime
        // Radiation fog: a clear or broken night sky cools the ground; it pools in valleys (and on low
        // ground by the sea). Sea / advection fog: the marine layer, on the coast and at sea under a
        // stratiform regime, day or night. Mist in rain. (The first cut also made fog under ANY stratiform
        // regime inland: the Sahara at noon under fog patches, the Po plain white at 15 deg of Sun.)
        float rad   = night * (1.0 - smoothstep(0.3, 0.8, cov1)) * max(valley, 0.5 * sea);
        float pot   = max(max(rad, sea * strat), 0.4 * w1.b) * cv2.fog.x;
        // Patches on the cluster field (the baked Perlin is 0.50 +- 0.057: x 8.8 spans +-1 over 2 sd).
        float n01   = clamp(0.5 + 0.5 * ((cl.a - 0.5) * 8.8 + 0.5 * (cl.r - 0.5) * 4.4), 0.0, 1.0);
        float pres  = smoothstep(0.3, 0.7, pot * (0.4 + 1.2 * n01));
        // Its top undulates with the low cloud's shape noise (~2 km): a flat sheet read as a painted plane.
        float sN    = textureLod(cv2ShapeTex, cv2.anchorShape.xyz + cv2Mat(q.rSeaE) * cv2.anchorShape.w,
                                 cv2Lod(fpM, cv2.anchorShape.w)).r;
        float top   = cv2.fog.y * (0.5 + pres) * (0.6 + 0.8 * sN);
        sig     = cv2.fog.z * pres * (1.0 - smoothstep(0.55 * top, top, agl));
        fogTopH = gF + top;
        fogHf   = clamp(agl / max(top, 1.0), 0.0, 1.0);
    }
    if (cv2.fog.w > 0.0) {
        vec4  clC   = textureLod(cv2MesoTex, cv2.anchorCluster.xyz + cv2Mat(q.seaProjE) * cv2.anchorCluster.w,
                                 cv2Lod(fpM, cv2.anchorCluster.w) + 2.0);
        float dry   = (1.0 - smoothstep(0.02, 0.25, cov4)) * (1.0 - sea) * (1.0 - clamp(w1.b * 3.0, 0.0, 1.0));
        float plume = smoothstep(0.42, 0.68, clC.a);
        // Regions (~500-1500 km): dry land is not all equally dusty — but a GENTLE variation. A uniform
        // level read as one flat veil (user snapshot 9), and hard-edged regions times hard-edged plumes
        // read as distinct blobs scattered over the land (user review 2, snapshot 2).
        float reg   = 0.7 * cv2DirNoise(q.dirE * 5.0) + 0.3 * cv2DirNoise(q.dirE * 15.0 + 7.3);
        float region = 0.3 + 0.7 * smoothstep(0.3, 0.8, reg);
        dustAgl = max(agl, 0.0);
        dustS = cv2.fog2.y * cv2.fog.w * dry * region * (0.35 + 0.65 * plume) * exp(-dustAgl / cv2.fog2.x);
    }
    if (cv2.fog2.w > 0.0) {
        float latD = degrees(asin(clamp(q.dirE.z, -1.0, 1.0)));
        // Antarctica: the ice shelves read ~0 m in the DEM, so "not the sea" cut the ice fog at every grounding
        // line (a hard arc from 180 km over the Ronne shelf, review 14). South of ~78 S everything is ice; north
        // of it the land test uses the regional (~80 km) ground, so a coast is a gradient, not an edge.
        float seaR = 1.0 - smoothstep(5.0, 120.0, textureLod(cv2WeatherTex, q.dirE, CV2_WX_LOD(4.0)).a * 8000.0);
        float antL = max(1.0 - seaR, 1.0 - smoothstep(-80.0, -72.0, latD));
        float ice  = max((1.0 - smoothstep(-72.0, -63.0, latD)) * antL,                          // Antarctica + shelves
                     max(smoothstep(62.0, 70.0, latD) * smoothstep(800.0, 1600.0, gF),          // Greenland
                         smoothstep(70.0, 78.0, latD) * (1.0 - sea)));                            // high Arctic land
        if (ice > 0.0) {
            float clear = 1.0 - smoothstep(0.25, 0.7, cov1);
            vec4  clI   = textureLod(cv2MesoTex, cv2.anchorCluster.xyz + cv2Mat(q.seaProjE) * cv2.anchorCluster.w,
                                     cv2Lod(fpM, cv2.anchorCluster.w) + 1.0);
            float patchI = smoothstep(0.40, 0.64, clI.a);
            // REGIONS of its own (~960 km and ~590 km; the dust's frequencies are not reused): one stretch of
            // the ice sheet hazes, the next one barely does. patchI alone (the cluster field at ~30 km) left the
            // ice fog one uniform white film over the whole continent and the ocean south of it, at every amount
            // (issue 20261030_204305: "encroaches too much ... uniformly"; "more variation than the noise scale
            // that's there"). 0.12..1: the thinnest regions keep a trace, none is erased.
            float regI    = 0.78 * cv2DirNoise(q.dirE * 6.64 - 8.47) + 0.22 * cv2DirNoise(q.dirE * 10.86 - 6.04);
            float regionI = 0.12 + 0.88 * smoothstep(0.32, 0.57, regI);
            dustAgl = max(agl, 0.0);
            iceS = cv2.sunE.w * cv2.fog2.w * ice * clear * (0.2 + 0.8 * patchI) * regionI * exp(-dustAgl / kIceFogH);
        }
    }
    return sig;
}

// A HABOOB's dust wall (2026-10-24, the user: "I really just want the cumulus clouds but beige"). It is cloud in the field,
// marched, light-marched and shadowed like any cumulus, and tinted in the lighting (its share of a sample is carried as a
// NEGATIVE CV2Field.soft: a member more flips the march's registers). The CPU rasterises the storms' gust fronts into a grid
// round the observer (cv2DustWallAt: the wall's strength e 0..1 across the front, and its top H); the shape is the low
// layer's 3D margin, m = e + lobes - g(z), with g rising from 0.15 at the ground to 1 at H: the wall's face leans up from
// the ground over the front's ~3 km ramp, its top a row of cauliflower heads, the pall behind it a low dense layer. The
// lobes are the cumulus shape volume at the dust's own period, read at an anchor that MOVES WITH THE WALL (and rises
// ~2 m/s: the billows churn up the face) -- the plume Gaussians it replaced near the eye were 4-20 km slabs with an
// Earth-fixed noise the wall slid through at the storm's speed ("huge and flat with a fast moving noise texture").
// Dense like a cumulus core (visibility ~100-200 m inside), ground-hugging, so the shell floor at 0 m and the march's
// soft ground contact hold it.
const float kCv2DustSigma = 0.016;   // extinction at the wall's core (1/m)
const vec3  kCv2DustTint  = vec3(0.95, 0.70, 0.45);   // the light a dust sample gives back, x a cloud's (cloud_v2_march.comp)
const float kCv2DustClearFadeM = 400.0;   // the clear air ahead of a front fades out over this above the walls' tops (+- 720 m of lumps)
float cv2DustWallSigma(CV2Pos q, vec3 w, float fpM, vec4 s, out float hfD, out float topD)
{
    hfD = 0.0; topD = 0.0;
#ifdef CV2_STORM_BINDING
    float agl = q.h - gCv2Ground;
    if (w.x <= 0.02 || w.y < 50.0 || agl > 1.25 * w.y + 300.0 || agl < -200.0) return 0.0;
    float z = max(agl, 0.0) / w.y;
    // The strength needed at height z: least at the NOSE (0.35 of the top: the gust front's head runs ahead of its foot,
    // held back by the ground), rising to the top and, less, to the ground.
    float zn = z - 0.35;
    float g = 0.12 + ((zn < 0.0) ? 0.7 * zn * zn : 2.1 * zn * zn);
    if (w.x + 0.6 <= g) return 0.0;                     // past what the lobes can reach
    float lobes = (s.g * 0.6 + s.b * 0.25 + s.a * 0.15) - 0.47;
    lobes *= mix(1.0, 0.3, smoothstep(150.0, 1200.0, fpM));   // sub-pixel lobes average away
    float m = w.x + 1.6 * lobes - g;
    if (m <= 0.0) return 0.0;
    hfD  = clamp(z, 0.0, 1.0);
    topD = gCv2Ground + w.y;
    return kCv2DustSigma * clamp(m * 6.0, 0.0, 1.0) * mix(0.55, 1.0, s.r) * sqrt(min(w.x, 1.0));
#else
    return 0.0;
#endif
}

// -- Storms through the cumulus field (2026-11-04, issue 20261104_111947 "Anvil performance", storm-rework.md) ---------
// The storm objects' towers (with their flared heads: the anvil's mushroom) and shields (the anvil plate spreading
// downwind) as a MARGIN in metres, with no noise of their own: cv2FieldLow joins it to the cumulus' margin and carves
// both with the same billow lobes and billow erosion, from one set of fetches. Until then each was its own density
// function with its own noise (cv2ColumnSigma: 3 shape + 1 detail fetch; cv2AnvilSigmaP: 3 more), summed with the
// cumulus' at every sample: a storm looked like another material, and a thin anvil haze cost a full field read per
// fine step (issue 111947: 36 anvil samples a ray at 11 km). The geometry is cv2ColumnSigma's (the same towers, radii,
// waist, flared head, drift, overshoot, lean, turrets) and cv2ShieldAt's capsules.
float cv2SMax(float a, float b, float k)
{
    float h = max(k - abs(a - b), 0.0) / k;
    return max(a, b) + h * h * k * 0.25;
}
// The convection cube read smooth at a level: four bilinear taps half a texel round wd, averaged (a tent over ~2 texels):
// one bilinear tap drew contours as the texels' polygons. The cores (x, y) at level 0 (5 km: they are ~10 km across), the
// anvil fields (z, w) at level 1.
#ifdef CV2_CONV_BINDING
vec4 cv2ConvSmooth(vec3 wd, float lod)
{
    vec3  e  = normalize(cross(vec3(0.0, 0.0, 1.0), wd) + vec3(1e-6, 0.0, 0.0));
    vec3  n  = cross(wd, e);
    float h  = 0.5 * 2.0 / (2048.0 * exp2(-lod));   // (half a texel: level l has 2048 / 2^l a face side, 2 / that rad each)
    return 0.25 * (textureLod(cv2ConvTex, wd + e * h, lod) + textureLod(cv2ConvTex, wd - e * h, lod)
                 + textureLod(cv2ConvTex, wd + n * h, lod) + textureLod(cv2ConvTex, wd - n * h, lod));
}
#endif
CV2Storm cv2StormGeom(CV2Pos q, float fpM)
{
    // Weather v2 (2026-11-06): the storm's SHAPE from the convection cube (cloud_v2_weather.comp wxConvection), not from
    // walking towers and shields: the column is the cores' intensity, domed (its top rises toward the strongest core),
    // the plate a lens from the anvil field under the lid. The margin goes to the cumulus field (cv2FieldLow), which carves
    // it with its own lobes and material.
    CV2Storm g = cv2StormNone();
    gCv2RainCore = 0.0;
    gCv2PlateA   = 0.0;
    gCv2CdoK     = 0.0;
    gCv2ShSet    = false;
#ifdef CV2_CONV_BINDING
    if (cv2.column.x <= 0.0 || q.h < (cv2.rain.x > 0.0 ? -100.0 : 400.0)) return g;
    vec3  wd    = q.dirE;
    float tropo = cv2Tropo(wd);
    float top   = cv2.types[4].alt.x + (cv2.types[4].alt.y - cv2.types[4].alt.x) * tropo;
    const float kCv2DomeU = 600.0;
    if (q.h > top + 50.0 + cv2.anvil2.w + kCv2DomeU) return g;
    vec4  cvB = textureLod(cv2ConvTex, wd, 2.0);   // ~20 km: is a storm near at all (and its surroundings, its cumulus)
    if (cvB.x < 0.01 && cvB.z < 0.01) return g;
    // (the cores read through the same tent at level 0: one bilinear tap, through the distance's sqrt(-ln C), drew the
    // 5-km texels' facets as flat faces on every tower)
    vec4  cv  = vec4(cv2ConvSmooth(wd, 0.0).xy, cv2ConvSmooth(wd, 1.0).zw);
    float C     = cv.x;
    float lift  = cv2Ground(q.dirE) * 0.9;
    float base  = cv2.types[4].alt.x + lift;
    float hb    = q.h - base;
    float lid   = top - 250.0;
    float osM   = cv2.anvil2.w + kCv2DomeU;
    float full  = smoothstep(0.8, 1.0, cv.y);
    float Rb    = cv2.column.z;
    g.storm = C;
    g.prox  = smoothstep(0.2, 0.55, cvB.x);
    if (cv2.rain.x > 0.0 && hb < 0.0) gCv2RainCore = smoothstep(0.45, 0.85, C) * smoothstep(0.4, 0.9, cv.y);
    // The COLUMN: where the cores' intensity passes 0.35, under a DOME: its top here rises from the base at that contour
    // to the column's top (cv.y of the way to the lid) at the core, so a cluster of cells stands as towers with rounded
    // heads, not a block with a flat roof. A little wider at mid-height (the updraft's buoyant swell); over the lid the
    // strongest core overshoots in a dome.
    const float c0 = 0.35;
    float best = -1e9, hfT = 0.0, topHere = base;
    if (hb >= -300.0 && C > c0 - 0.15) {
        // dH: the DISTANCE in from the c0 contour (m): for a core C = exp(-r^2 / sigma^2), r = sigma sqrt(-ln C), sigma the
        // cells' 0.35 Rb. ((C - c0) x 1.6 Rb ran ~2.4 m a metre across the wall, which flattened the cumulus lobes carved
        // into it to ~40% of their size: smooth walls with erosion pits, a snow mountain instead of cauliflower.)
        float sg0  = 0.35 * Rb;
        float rcol = sg0 * sqrt(-log(c0));
        float dH   = sg0 * (sqrt(-log(c0)) - sqrt(-log(max(C, 1e-4))));
        // The HEAD: a cap as round as the column is wide (radius rh), the top cv.y of the way to the lid; over it the
        // strongest core overshoots in a dome. (The top rising linearly with the intensity drew every young tower as a
        // spike.)
        float topC = base + (lid - 300.0 - base) * cv.y;
        float rh   = min(rcol, 0.5 * max(topC - base, 600.0));
        float dh   = clamp(rh - dH, 0.0, rh);
        topHere    = topC - rh + sqrt(max(rh * rh - dh * dh, 0.0))
                   + osM * full * smoothstep(0.3, 0.9, dH / max(rcol, 1.0));
        float zeta = clamp(hb / max(topHere - base, 300.0), 0.0, 1.0);
        // + a little swell at mid-height, and the head's FLARE where the column reaches the lid: it widens over its top
        // third into the anvil it feeds (the outflow turning outward), not a drum under a mushroom cap.
        float zl   = clamp(hb / max(lid - base, 300.0), 0.0, 1.0);
        float mH   = dH + Rb * 0.06 * sin(3.14159 * zeta) + Rb * 0.7 * full * cv2SqC(smoothstep(0.6, 1.0, zl));
        float mV   = topHere - q.h;
        best = -cv2SMax(-mH, -mV, 600.0);                                   // a smooth min(mH, mV)
        hfT  = zeta;
    }
    g.near = full * smoothstep(0.4, 0.8, C);
    // The PLATE: a LENS from the anvil field, its top the lid (rising toward an updraft that reaches it), as thick as
    // "Anvil thickness" where the field is dense and thinning to nothing at its outline, its underside hanging lower
    // ("Anvil hang") near the column it grows out of. Its outline is where the field falls to 0.3, ~15 km of margin a
    // unit of field: frayed by the cumulus lobes, not cut.
    float mSh = -1e9, topS = lid, hfS = 0.0, sigS = 0.0, edgeS = 0.0;
    if (cv2.storm.z > 0.0 && q.h > max(5000.0, 0.6 * top) && cv.z > 0.15) {
        float a     = cv.z;
        gCv2PlateA  = a;
        float aK    = smoothstep(0.3, 1.0, a);
        topS        = lid + 400.0 * smoothstep(0.4, 0.9, C) * full;
        float thick = 200.0 + cv2.anvil2.x * pow(aK, 0.7) + cv2.anvil2.y * g.near;
        float mH    = (a - 0.3) * 15000.0;
        float mV    = min(q.h - (topS - thick), topS - q.h);
        mSh   = -cv2SMax(-mH, -mV, 300.0);
        edgeS = 1.0 - smoothstep(0.0, 0.35, a - 0.3);
        hfS   = clamp((q.h - (topS - thick)) / thick, 0.0, 1.0);
        sigS  = mix(0.35, 1.0, smoothstep(0.3, 0.8, a));
    }
    float m = cv2SMax(best, mSh, 700.0);
    if (m < -1e8) return g;
#ifdef CV2_STORM_BINDING
    // A tropical cyclone's EYE (Weather v2 Phase 3): carved analytically, a stadium whose radius grows with height (0.8x at
    // the base to 1.7x at the lid); the cube's ~12-km texels fill a 10-20 km hole in the CDO's field.
    for (uint i = 0u; i < min(cv2CycHead.x, 4u); ++i) {
        CV2Cyclone cy = cv2Cyclones[i];
        float eR = max(cy.c.x, 0.5 * cy.d.x);
        if (eR <= 0.0 || cy.a.w < 0.3) continue;
        float rC = acos(clamp(dot(wd, cy.a.xyz), -1.0, 1.0)) * R_EARTH;
        float rE = eR * (0.8 + 0.9 * clamp(hb / max(lid - base, 1000.0), 0.0, 1.0));
        if (rC < rE + 30000.0) m = min(m, (rC - rE) * 1.5);
        gCv2CdoK = max(gCv2CdoK, 1.0 - smoothstep(0.8 * cy.c.y, 1.3 * cy.c.y, rC));
    }
#endif
    float wT = clamp(0.5 + (best - mSh) / 1200.0, 0.0, 1.0);
    g.m    = m;
    g.top  = mix(topS, max(topHere, q.h), wT);
    g.hf   = mix(hfS, hfT, wT);
    g.deck = 1.0 - wT;
    g.sig  = mix(sigS, 1.0, wT);
    g.cap  = 0.67 * edgeS * (1.0 - wT);
#endif
    return g;
}

// The haboob code in the field on / off (perf pass 4, 2026-10-25). Its mere presence, never run, cost the march 0.5-1 ms in
// every view (the field is inlined at two sites: code size), so the march is built twice, specialization constant 3, and the
// host binds the dust-free one while the dust grid holds no wall (SatelliteSim::dustGpuOn). Other shaders keep it on.
#ifdef CV2_DUST_SPEC
layout(constant_id = 3) const int kCv2DustOn = 1;
#else
const int kCv2DustOn = 1;
#endif
// Storms through the cumulus field (clouds_v2.storm_unified): the march's is specialization constant 4, so its pipeline
// holds only the path in use (both behind the uniform: +0.6 ms of code size in views with no storm); the other shaders
// read the uniform.
#if defined(CV2_CONV_BINDING)
// Weather v2: storms are the convection cube's, always drawn through the cumulus field (the old per-tower path is not
// compiled in; it kept ~0.6 ms of code size and the register pressure).
#define CV2_STORM_U true
#elif defined(CV2_DUST_SPEC)
layout(constant_id = 4) const int kCv2StormSpec = 1;
#define CV2_STORM_U (kCv2StormSpec != 0)
#else
#define CV2_STORM_U (cv2.stormU.x > 0.5)
#endif
// The LAYER MASK (2026-11-04, the occupancy clipmap's cells, cv2_cdc.glsl): which layer functions cv2Field calls. The march
// sets it per field read from the clipmap cell the point lies in (every bit where the cell is unknown); every other caller
// keeps all of them. A bit is clear only where the bake found that layer's function zero, and without effect on the
// others, at every one of the cell's points: a storm's tower walk, the low field's weather reads, the shields, the mid and
// high layers' reads ran in every field read of the light march through an altostratus sheet, nothing to add there.
// The bake (CV2_LAYER_HITS) collects the bits a read needed in gCv2LayHit.

const uint kCv2LayCol  = 1u;    // the storm objects' towers (cv2ColumnSigma): their sigma, or a proximity / strength /
                                // rain core another layer reads
const uint kCv2LayLow  = 2u;    // cv2FieldLow (the low layer, rain)
const uint kCv2LayMid  = 4u;    // cv2MidSigma
const uint kCv2LayHigh = 8u;    // cv2HighSigma
const uint kCv2LayAnv  = 16u;   // cv2AnvilSigmaP, its presence (the cirrus gives way) and the shield-less presence read
const uint kCv2LayDust = 32u;   // the haboob's dust (never skipped: its own specialization)
uint gCv2LayMask = 0xFFFFu;
#ifdef CV2_LAYER_HITS
uint gCv2LayHit = 0u;
#endif
CV2Field cv2Field(CV2Pos q, float detailAmt, float fpM)
{
    uint lm = gCv2LayMask;
    gCv2FlowSet = false;
    gCv2FlowD   = cv2FlowDisp(cv2Drift(q.dirE), vec3(0.0));
    gCv2FlowSet = true;
    {   // Weather v2: the texture rides the wind. The sample's noise positions are shifted IN PLACE (q is a copy): a
        // displacement held live through the field put the march over the 128-register cliff (251).
        vec3 dD = cv2NoiseDispAt(q.dirE, kWfLowH);
        q.rSeaE -= dD; q.seaProjE -= dD;
    }
    gCv2GroundSet = false;
    gCv2Ground    = cv2Ground(q.dirE);
    gCv2GroundSet = true;
#if defined(CV2_ORO) && !defined(CV2_ORO_EXTERNAL)
    gCv2Oro = cv2OroField(q);   // (the march sets it itself, at its main sample: CV2_ORO_EXTERNAL)
#endif
    gCv2RainCore = 0.0;
    // Storms are the cumulus field's margin (cv2StormGeom: the convection cube), carved by its lobes and erosion.
    CV2Storm sg = cv2StormNone();
    if ((lm & (kCv2LayCol | kCv2LayLow)) != 0u) sg = cv2StormGeom(q, fpM);
#ifdef CV2_LAYER_HITS
    float rcHit = gCv2RainCore;
#endif
    CV2Field f;
    f.sigma = 0.0; f.hf = 0.0; f.msBright = 1.0; f.ambient = 1.0; f.deck = 0.0; f.topH = 0.0; f.thin = 0.0;
    f.rain = 0.0; f.soft = 0.0;
    if ((lm & kCv2LayLow) != 0u) f = cv2FieldLow(q, detailAmt, fpM, sg.prox, sg);
#ifdef CV2_ORO
    // The low cloud PARTS round a massif (orographic clouds, 2026-11-06, issue 20261105_174850): its bases ride on the ~20 km
    // ground, so a deck crossed the prominent relief as a slab through the mountains. Where the ~5 km relief stands
    // prominent over that ground (gCv2Oro.w), the low cloud below the relief + 900 m gives way (soft over 1.1 km): the deck
    // lies in the valleys and flows over the lower ridges, and the peaks stand in clear gaps.
    f.sigma *= 1.0 - gCv2Oro.w * (1.0 - smoothstep(gCv2Oro.z - 300.0, gCv2Oro.z + 900.0, q.h));
#endif
    float dbgLow = f.sigma;
#ifdef CV2_LAYER_HITS
    // The towers' proximity and rain core change the low field (the storm cumulus' top moves toward its own, either way;
    // the rain core adds rain): needed where the low field reads differently without them, which is what a skip reads.
    bool colLow = false;
    if (sg.prox > 0.0 || rcHit > 0.0) {
        if (dbgLow > 0.0) colLow = true;
        else { gCv2RainCore = 0.0; colLow = cv2FieldLow(q, detailAmt, fpM, 0.0, cv2StormNone()).sigma > 0.0; gCv2RainCore = rcHit; }
    }
#endif
    float hfD, topD, sd = 0.0;
#ifdef CV2_STORM_BINDING
    vec3 dw = vec3(0.0);
    // The air ahead of a haboob's gust front sinks and is dry: the low cloud and rain there (the line's shelf band, drawn
    // 1-22 km ahead of its towers whatever the outflow's lead, the weather map's own) give way, so the wall stands under
    // open sky with the storm's anvil above (dw.z: from just behind the front's foot to ~12 km ahead of it).
    // Cleared OUTRIGHT where the clear air is (perf pass 4, 2026-10-25): x (1 - 0.95 z) left 5% of the shelf deck as a
    // thin milky veil over the whole view that every ray marched and lit for tens of km (haboob Watch, the user's
    // settings: cloud march 20.3 -> 13.0 ms), and hid the storm behind it.
    // Its EDGES are soft and lumpy (2026-10-25, issue 20261025_184550). The clear air was read only below the walls'
    // highest top (cv2DustFrame[1].w, ~3-5 km) and cleared everything under it: a flat horizontal cut through every cloud
    // straddling that height (the "odd horizon band" seen from 2 km; the tornado probe, which reads this field, hung the
    // funnel from the cut, 3.5 km up, in open sky), and its ~2.5-km ramp across the grid was, on a dense cloud (any
    // density factor acts as a threshold there), a flat vertical plane. Now the clearing fades out over
    // kCv2DustClearFadeM above that height (subsidence raising the bases, not a slice), and both edges are displaced by
    // the wall's own lobe volume (one fetch, shared with the wall): lumpy in 3D, like the cloud eroding. Read above the
    // walls' tops only where there is low cloud to clear.
    float aglD = q.h - gCv2Ground;
    float hT   = cv2DustFrame[1].w;
    if (kCv2DustOn != 0 && cv2DustHead.x != 0u && (aglD < hT || (f.sigma > 0.0 && aglD < hT + kCv2DustClearFadeM + 900.0)))
        dw = cv2DustWallAt(q.seaProjE + cv2NoiseDispAt(q.dirE, kWfLowH));   // (placed in the Earth frame)
    float clr = dw.z;
    float lobD = 0.0, lobG = 0.0;   // the lobe volume about its mean (all octaves, the coarse one), 0 where not read
    if (dw.x > 0.02 || (clr > 0.0 && (clr < 1.0 || aglD > hT - 900.0))) {
        vec4 sl = cv2ShapeSmooth(cv2DustFrame[2].xyz + q.rSeaE * cv2DustFrame[2].w, cv2Lod(fpM, cv2DustFrame[2].w));
        lobD = (sl.g * 0.6 + sl.b * 0.25 + sl.a * 0.15) - 0.47;
        lobG = clamp(sl.g - 0.47, -0.3, 0.3);
        sd = cv2DustWallSigma(q, dw, fpM, sl, hfD, topD);
    }
    // (the edge noise fades to nothing at both ends of the ramp: continuous where the grid reads 0 or 1 exactly)
    clr += 1.3 * smoothstep(0.0, 0.12, clr) * (1.0 - smoothstep(0.88, 1.0, clr)) * lobD;
    clr *= 1.0 - smoothstep(hT, hT + kCv2DustClearFadeM, aglD - 1200.0 * lobG);
    f.sigma *= 1.0 - smoothstep(0.0, 0.8, clr);
#endif
    if (sd > 0.0) {
        float s0 = f.sigma;
        cv2Add(f, sd, hfD, topD, 0.0);
        f.rain *= s0 / f.sigma;
    }
    float hfX = 0.0, topX = 0.0, deckX = 0.0;
    float sm = 0.0;
    if ((lm & kCv2LayMid) != 0u) sm = cv2MidSigma(q, fpM, hfX, topX, deckX);
#ifdef CV2_LAYER_HITS
    float smRaw = sm;
#endif
    // The mid layer (a thin Ac/As lens) gives way over a storm: drawn through it, it sliced every
    // tower with a flat plate at ~6 km, seen edge-on as a hard line across the storm (user snap 1,
    // pass 17). Only where the towers are drawn (the storm's strength is read for them).
    // Wherever towers can stand (a weak storm-edge tower cut by a mid-layer belt, pass 20).
    if (cv2.column.x > 0.0) sm *= 1.0 - smoothstep(0.0, 0.15, sg.storm);
    if (sm > 0.0) cv2Add(f, sm, hfX, topX, deckX);
    float hfH = 0.0, topHh = 0.0;
    float sh = 0.0;
    if ((lm & kCv2LayHigh) != 0u) sh = cv2HighSigma(q, fpM, hfH, topHh);
#ifdef CV2_LAYER_HITS
    float shPre = sh;
#endif
    // The anvil plate (the convection cube's, in the low field's margin) is the high cloud round a storm: the cirrus
    // gives way to it.
    float presA = smoothstep(0.3, 0.6, gCv2PlateA);
    // The anvil is the high cloud around a storm: the cirrus layer (0.69-0.93 of the tropopause, the
    // anvil ~0.7-0.98) gives way to it, and continues outward from its edge. Both drawn, cirrus sheets
    // cut straight through the anvils' sides (user snap 3, pass 15).
    sh *= 1.0 - presA;
    hfX = hfH; topX = topHh;
    if (sh > 0.0) cv2Add(f, sh, hfX, topX, 0.0);
    if (sh > 0.0) f.thin = sh / f.sigma;
    if (sm > 0.0) f.soft = sm / f.sigma;
    else if (sd > 0.0) f.soft = -sd / f.sigma;   // the dust's share, negative (cv2DustWallSigma)
    if (gCv2DbgWant)
        gCv2Dbg = vec4(dbgLow + max(sd, 0.0), max(sm, 0.0), 0.0, max(sh, 0.0));
#ifdef CV2_LAYER_HITS
    // The layers this read needed: a function whose result was zero AND changed nothing else. The storm geometry (Col):
    // where its proximity or rain core changed the low field, its strength cut a mid sheet, or its plate cut the cirrus.
    gCv2LayHit = ((colLow || (sg.storm > 0.0 && smRaw > 0.0) || (presA > 0.0 && shPre > 0.0)) ? kCv2LayCol : 0u)
               | (dbgLow > 0.0 ? kCv2LayLow : 0u) | (sm > 0.0 ? kCv2LayMid : 0u)
               | (sh > 0.0 ? kCv2LayHigh : 0u)
               | (sd > 0.0 ? kCv2LayDust : 0u);
#endif
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
#include "cv2_optics.glsl"   // CV2_N_ICE / CV2_N_WATER, cv2Sq, the ice and rain optics, cv2HG

// Crystal habits, per region (the cluster field, tens of km): which ice optics a patch of cirrus can
// show at all. A halo needs crystals of the right shape; sundogs and the arcs need plates falling
// flat, which calm air allows and turbulence scrambles. Real skies show a 22 degree halo in maybe a
// third of cirrostratus, sundogs less often, the circumzenithal arc and parhelic circle rarely and the
// 46 degree halo very rarely. The first cut drew every arc on every cirrus, which made the rare
// look commonplace. x = 22 deg halo, y = sundogs (flat plates), z = circumzenithal arc + parhelic
// circle (very well aligned plates), w = 46 deg halo. Evaluated once per RAY (the march caches it).
vec4 cv2IceHabit(CV2Pos q)
{
    vec3  m = cv2.anchorCluster.xyz + cv2Mat(q.seaProjE) * cv2.anchorCluster.w;
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
