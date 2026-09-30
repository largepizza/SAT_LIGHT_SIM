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
// assets/textures/earth_elevation.png is R8_UNORM, 14999x7500 (not 21600x10800 as this comment long
// said), LAND-ONLY. It is NOT ETOPO1 and
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
//
// ── Water (terrain v2 P2, tools/make_water_map.py) ─────────────────────────────────────────────
// The "specular" binding is the WATER MAP, R8G8: R = a smoothed signed distance to the shore
// (0.5 + d / (2 kShoreSdfMaxM), d > 0 in water — so `r > 0.5` is still "water" for every consumer of
// the old binary mask, and the filtered field draws a smooth shoreline where the filtered binary mask
// drew its 5 km staircase); G = the level of the nearest water body, in DEM units (15 = sea level).
// Each lake takes the DEM's value over it (the DEM stores its flat surface).
//
// A water texel's height is its body's LEVEL (0 for the sea): the flat surface the march hits. Land
// meets it exactly at the shoreline: within kShoreBankM of it, land is lifted to at least the level
// (a bank, no wall of water standing above a dip), and within kShoreRampM it is capped by a ramp
// rising kShoreSlope per metre from the shore (the DEM's 35-m land baseline was a step at every
// coast). The cap lets go between kShoreRampM and twice that: everywhere it applied, a mountain
// rising from a lake was flattened to 0.25 m/m for kilometres.
//
// G = 0 means the map was not baked (the plain mask, SatelliteSim.cpp's fallback): then the old rule,
// water only where the DEM itself is near sea level (kWaterMaskMaxM) — it marks INLAND lakes too,
// and forcing those to 0 m dug a sea-level pit into every lake above sea level.
const float kWaterMaskMaxM = 160.0;
const float kShoreSdfMaxM  = 4.0 * PI * R_EARTH / 4096.0;  // 4 mask texels (~19.5 km) = full scale
const float kShoreSlope    = 0.25;                          // m of land per m from the shore
const float kShoreBankM    = 3000.0;
const float kShoreRampM    = 1500.0;
const float kTdWaterMark   = -1.0e4;                        // hMip3 on water (terrain_detail.glsl)

// Height of the ground (or water surface) given the DEM height and the water map texel(s) wm.
// dOff (metres) moves the shoreline: terrain_detail.glsl's coves and headlands (tdShoreOffset).
float waterAdjustHeight(float hDem, vec2 wm, float dOff, out bool water) {
    if (wm.g < 14.5 / 255.0) {                     // not baked: the old mask rule
        water = hDem < kWaterMaskMaxM && wm.r > 0.5;
        return water ? 0.0 : hDem;
    }
    float level = max(0.0, wm.g * kElevRange - kElevOffset);
    float d     = (wm.r - 0.5) * 2.0 * kShoreSdfMaxM + dOff;   // metres, > 0 in water
    water = d > 0.0;
    if (water) return level;
    float land = mix(max(hDem, level), hDem, smoothstep(0.0, kShoreBankM, -d));
    return min(land, level - kShoreSlope * d + 1.0e4 * smoothstep(kShoreRampM, 2.0 * kShoreRampM, -d));
}

// Where city lights may stand (2026-09-30, the user: lights should follow the terrain's contours, with
// altitudes and slopes they never reach). hLoc = the ground (DEM + water, tdDemAt's h0), dens01 = the
// night map's density there. A city fills its valley floor and lower slopes: its lights end at a CONTOUR
// a few hundred metres above the ~40 km mean ground (higher where the map is brighter, wobbled +-90 m so
// the edge is not one level), instead of the night map's 5-km blur spilling up every mountainside; and
// none above ~4.5 km. sat_sky.frag (the surface) and city_sprites.comp (the far points) share it.
float cityTerrainLimit(sampler2D elevTex, vec2 uv, float hLoc, float dens01) {
    float hM  = max(0.0, textureLod(elevTex, uv, 4.5).r * kElevRange - kElevOffset);
    float wob = 90.0 * sin(uv.x * 4100.0 + 1.3) * sin(uv.y * 2900.0 + 0.7);
    float thr = 150.0 + 330.0 * dens01 + wob;
    return (1.0 - smoothstep(thr, thr + 120.0, hLoc - hM)) * (1.0 - smoothstep(4200.0, 4900.0, hLoc));
}

float terrainHeightAtUV(sampler2D elevTex, sampler2D specTex, vec2 uv, float lod) {
    float h = max(0.0, textureLod(elevTex, uv, lod).r * kElevRange - kElevOffset);
    bool water;
    return waterAdjustHeight(h, textureLod(specTex, uv, lod).rg, 0.0, water);
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
