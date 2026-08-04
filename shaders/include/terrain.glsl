#ifndef SATLIGHTSIM_TERRAIN_GLSL
#define SATLIGHTSIM_TERRAIN_GLSL

// Terrain / DEM sampling and observer-frame helpers, shared by sat_sky.frag (full-res terrain
// march + shading), scene_depth.comp (half-res depth-only march), and cloud_march.comp.
//
// Requires common.glsl (PI, R_EARTH) — include that first.
//
// Textures are passed as PARAMETERS rather than referenced by name, because each consuming
// shader binds them at a different index (sat_sky.frag and cloud_march.comp do not agree). That
// keeps this header binding-agnostic; the caller owns its own descriptor layout.

// ── Elevation texture encoding — READ THIS BEFORE CHANGING ANY TERRAIN CODE ────────────────
//
// assets/textures/earth_elevation.png is R8_UNORM, 21600x10800, LAND-ONLY. It is NOT ETOPO1 and
// has NO bathymetry. Do not assume pixel=0 means sea level:
//
//   0-14/255    compression noise in ocean regions — treat as sea level
//   15/255      ocean / sea-level baseline
//   16-255/255  land elevation, linearly scaled above sea level
//   255/255     ~8848 m (Everest)
//
// Failing to subtract kElevOffset makes every coastline on Earth read as a ~530 m vertical cliff,
// because sea-level land decodes to 529 m above the ocean sphere. This bug has been introduced
// and re-introduced across multiple sessions — having exactly one decode function is much of the
// point of this header. The ocean sphere sits at exactly R_EARTH, so the height formula must
// produce 0 m for ocean-baseline pixels.
const float kElevRange  = 9000.0;
const float kMaxTerrain = 9000.0;                        // terrain shell height (m), just above Everest
const float kElevOffset = 15.0 / 255.0 * kElevRange;     // DEM ocean baseline (~529 m)

// Equirectangular UV for a unit ECEF direction. Longitude wraps at ±PI; latitude maps top-down
// so v=0 is the north pole.
vec2 dirToUV(vec3 dirECEF) {
    float lat = asin(clamp(dirECEF.z, -1.0, 1.0));
    float lon = atan(dirECEF.y, dirECEF.x);
    return vec2((lon + PI) / (2.0 * PI), (0.5 * PI - lat) / PI);
}

// Same, for a non-unit ECEF position.
vec2 posToUV(vec3 pECEF) {
    float len = length(pECEF);
    float lat = asin(clamp(pECEF.z / len, -1.0, 1.0));
    float lon = atan(pECEF.y, pECEF.x);
    return vec2((lon + PI) / (2.0 * PI), (0.5 * PI - lat) / PI);
}

// Terrain height in metres above sea level at an equirectangular UV.
//
// Every elevation read is gated by the ocean mask: where specMask > 0.5 the texel is water and
// the height is forced to exactly 0 regardless of what the elevation pixel says. That gate is
// load-bearing — JPEG compression puts 1-4/255 of DCT ringing (34-140 m) into ocean texels, which
// without the mask produces false terrain hits out at sea.
//
// LOD is a parameter and callers should pass 0.0. It exists because a previous session found the
// beam occlusion test sampling at mip 2.0 while the terrain actually being drawn sampled mip 0.0
// — an undocumented mismatch that let beams clip through hills the test could not see. If you
// pass anything other than 0.0 here, be explicit about why.
float terrainHeightAtUV(sampler2D elevTex, sampler2D specTex, vec2 uv, float lod) {
    if (textureLod(specTex, uv, lod).r > 0.5) return 0.0;   // ocean/water mask — sea level
    return max(0.0, textureLod(elevTex, uv, lod).r * kElevRange - kElevOffset);
}

float terrainHeightAtDir(sampler2D elevTex, sampler2D specTex, vec3 dirECEF) {
    return terrainHeightAtUV(elevTex, specTex, dirToUV(dirECEF), 0.0);
}

// ── Observer frame ────────────────────────────────────────────────────────────
// ENU basis in ECEF, built from the observer's ECEF up-direction. Identical construction in
// sat_sky.frag, cloud_march.comp and cloud_shadow.comp — the East axis is derived from the
// world Z axis, which is degenerate exactly at the poles and fine everywhere else.
void enuBasis(vec3 obsECEFDir, out vec3 enuX, out vec3 enuY, out vec3 enuZ) {
    enuZ = normalize(obsECEFDir);                         // Up
    enuX = normalize(cross(vec3(0.0, 0.0, 1.0), enuZ));   // East
    enuY = cross(enuZ, enuX);                             // North
}

// Observer height above sea level, resolved on the GPU.
//
// A single fetch at the observer's own lat/lon, maxed against the CPU's value (obsECEFDir.w =
// obsTerrainH + user altitude offset). Using the GPU value means the observer can never sink
// into terrain regardless of what the CPU computed, because this uses the exact same decode as
// the terrain march itself; taking the max preserves user-controlled altitude offsets.
//
// This MUST be the single definition. cloud_march.comp previously took a CPU-computed obsEffH
// via push constant while sat_sky.frag did this lookup, so the two disagreed on where the
// observer was — harmless while they only compared against themselves, but not once they share
// a depth buffer whose distances one produces and the other consumes.
float observerEffHeight(sampler2D elevTex, sampler2D specTex, vec4 obsECEFDir) {
    float groundH = terrainHeightAtDir(elevTex, specTex, normalize(obsECEFDir.xyz));
    return max(groundH, max(0.0, obsECEFDir.w));
}

// Observer position in the local ENU frame (+2 m eye height above ground).
vec3 observerPos(float obsEffH) {
    return vec3(0.0, 0.0, R_EARTH + obsEffH + 2.0);
}

#endif // SATLIGHTSIM_TERRAIN_GLSL
