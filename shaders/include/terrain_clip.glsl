#ifndef SATLIGHTSIM_TERRAIN_CLIP_GLSL
#define SATLIGHTSIM_TERRAIN_CLIP_GLSL
// ── The terrain height clipmap (2026-11-10) ──────────────────────────────────────────────────────────────────────────
// The procedural detail of terrain_detail.glsl (eight value octaves + the erosion octaves) BAKED into nested textures
// round the observer (terrain_clip_bake.comp), so the terrain marches read it instead of evaluating it: at ground level
// the detail was 7-10 ms of a 21-30 ms frame (three marches a frame: quarter, half and full resolution), the march
// over the DEM alone ~1 ms.
//
// Level j holds the value octaves at the geometry LOD lod_j = kTcLod0 x 2^j (1 m .. 1024 m): the octaves whose cell is
// at least 2 lod_j, at full weight. An octave's weight is smoothstep(lod, 2 lod, cell) and its slope damping depends
// only on the octaves before it, so for lod_j <= lod < 2 lod_j the height is EXACTLY
//     mix(level j + 1, level j, smoothstep(lod, 2 lod, 2 lod_j))
// (one octave is in transition, the coarser level is the same sum without it). Past the last level nothing is left
// (0). The finest octave's cell is 16 m, so levels 0-3 hold the same sum: they are there for their texels (0.33 m at
// level 0), because bilinear texels of 2.7 m put the ground within ~80 m of a 2-m eye a few centimetres off, pixels
// of shift at that grazing angle (harness_runs/cloudsky/tc: the Alps from the ground, 1.4 -> 0.5 levels, the floor).
//
// The plane: orthographic onto the tangent plane at an ANCHOR point (its east / north axes, ECEF), texel = lod_j / 3
// (six texels across the level's finest cell), kTcN texels a side: a level reaches 170 lod_j from the observer, and
// the LOD is at least 1.2% of the distance, so the level a point needs always holds it. Toroidal: a texel's place is
// its absolute plane cell mod kTcN; the CPU (double) hands each level the observer's cell and the fraction inside it,
// so no coordinate larger than a level's window is formed here.
//
// A: r32f, the VALUE octaves' height (m): the detail without the erosion's own height (the erosion's gullies crease
// at the scale of a pixel and are evaluated by the readers, as a plane round one sample per march; its slope's damping
// of the fine octaves is in A). B: rgba16f, x = the first three octaves' height (the terrain shadow's surface, hC3),
// yz = the coarse octaves' gradient along the anchor's east / north (what the erosion follows; its third component
// follows from the gradient being tangent to the sphere).
// B has no layers for levels 0 .. kTcBFirst - 1 (the same content as level kTcBFirst, and smooth: cells of 256 m up):
// those read level kTcBFirst's.
const int   kTcLevels = 11;
const float kTcLod0   = 1.0;
const float kTcN      = 1024.0;
const int   kTcBFirst = 3;

#ifndef TD_CLIP_SET
#define TD_CLIP_SET 1
#endif
#ifndef TD_CLIP_UBO_BINDING
#define TD_CLIP_UBO_BINDING 0
#endif
layout(std140, set = TD_CLIP_SET, binding = TD_CLIP_UBO_BINDING) uniform TdClip {
    vec4 tcAxE;                   // the anchor's east (ECEF)
    vec4 tcAxN;                   // the anchor's north (ECEF)
    vec4 tcAxU;                   // the anchor's up (ECEF); w = the finest level baked (levels below it are not read)
    vec4 tcObs;                   // the observer's sea-level point in the anchor's frame (m): x east, y north, z up
    vec4 tcLevel[kTcLevels];      // xy = the observer's cell mod kTcN, zw = the fraction inside it
    ivec4 tcCell[kTcLevels];      // xy = the observer's absolute cell (the bake)
};

float tcTexel(int j) { return kTcLod0 * exp2(float(j)) * (1.0 / 3.0); }

#ifndef TD_CLIP_BAKE
layout(set = TD_CLIP_SET, binding = 1) uniform sampler2DArray tdClipA;
layout(set = TD_CLIP_SET, binding = 2) uniform sampler2DArray tdClipB;

// The plane offset (m) of q's ground point from the observer's. offE: tdSphereOffset(q) in ECEF axes.
vec2 tcPlane(vec3 offE) { return vec2(dot(offE, tcAxE.xyz), dot(offE, tcAxN.xyz)); }

// The level a point at plane offset d reads at the LOD lodM, and the weight of that level against the next coarser.
int tcLevelAt(vec2 d, float lodM, out float fade)
{
    int   j  = int(log2(max(lodM, kTcLod0) * (1.0 / kTcLod0)));
    // (Inside the level's window, whatever the LOD said, and never below the finest level baked.)
    float m  = max(abs(d.x), abs(d.y));
    int   jw = int(max(ceil(log2(max(m, 1.0) / (tcTexel(0) * (0.5 * kTcN - 3.0)))), 0.0));
    j = max(j, max(jw, int(tcAxU.w)));
    fade = smoothstep(lodM, 2.0 * lodM, 2.0 * kTcLod0 * exp2(float(j)));
    return j;
}

// Level j's detail height at plane offset d: bilinear over its four texels with exact weights (one gather; the
// sampler's 8-bit weights would step a near slope by centimetres).
float tcHeightAt(int j, vec2 d)
{
    vec2 p  = d / tcTexel(j) + tcLevel[j].zw - 0.5;
    vec2 b  = floor(p);
    vec2 f  = p - b;
    vec4 g  = textureGather(tdClipA, vec3((tcLevel[j].xy + b + 1.0) * (1.0 / kTcN), float(j)));
    return mix(mix(g.w, g.z, f.x), mix(g.x, g.y, f.x), f.y);
}
vec4 tcSideAt(int j, vec2 d)
{
    int  jb = max(j, kTcBFirst);
    vec2 p  = d / tcTexel(jb) + tcLevel[jb].zw;
    return textureLod(tdClipB, vec3((tcLevel[jb].xy + p) * (1.0 / kTcN), float(jb - kTcBFirst)), 0.0);
}

// The detail's height (m) at a point (offE as above) at the geometry LOD lodM.
float tcDetail(vec3 offE, float lodM)
{
    vec2  d = tcPlane(offE);
    float fade;
    int   j = tcLevelAt(d, lodM, fade);
    if (j >= kTcLevels) return 0.0;
    float h = tcHeightAt(j, d);
    if (fade < 1.0) h = mix((j + 1 < kTcLevels) ? tcHeightAt(j + 1, d) : 0.0, h, fade);
    return h;
}
// B at the point: x = hC3, y = the erosion's height, zw = its gradient in the anchor's plane.
vec4 tcSide(vec3 offE, float lodM)
{
    vec2  d = tcPlane(offE);
    float fade;
    int   j = tcLevelAt(d, lodM, fade);
    if (j >= kTcLevels) return vec4(0.0);
    vec4 s = tcSideAt(j, d);
    if (fade < 1.0) s = mix((j + 1 < kTcLevels) ? tcSideAt(j + 1, d) : vec4(0.0), s, fade);
    return s;
}
#endif // !TD_CLIP_BAKE

#endif // SATLIGHTSIM_TERRAIN_CLIP_GLSL
