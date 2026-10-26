// sky_ap.glsl — the aerial-perspective volume (2026-10-25, branch perf-frames): the sky pass's single-scattering
// atmosphere loop, run ONCE PER SCREEN TILE by sky_ap.comp instead of once per pixel, its running sums stored at
// kSkyApZ depths along each tile's ray. sat_sky.frag reads them at the pixel's surface distance (and at the air
// split in front of the clouds) instead of marching, except in a band round the horizon / the Earth's limb, where the
// integral changes too fast across a tile and the exact loop runs (cv2.skyAp.z).
//
// Grid: the god rays' froxel tiles (kCv2FroxX x kCv2FroxY, cloud_lightning.glsl) and their per-frame sample offset
// (cv2FroxJitter: the sky TAA averages the reconstruction); kSkyApZ slices along each ray at a NORMALISED depth
// (skyApSlice): tStart the ray's entry into the atmosphere (0 inside it), L its length to the atmosphere's exit or the
// sea-level sphere, d0 = cv2.skyAp.y. A pixel maps its own distance through its OWN ray's tStart and L, so a sky pixel
// reads the last slice of every neighbouring tile exactly.
// Per cell 12 half floats (two arrays: A = accumR.rgb, accumM, the sky-lit shadow light.rgb, the city glow; B = the
// camera's Rayleigh and Mie columns, the green and sodium airglow columns), per unit of air column (skyApNorm) and
// x kSkyApScale (a raw Rayleigh column along the horizon is ~6e5 m: past a half float's 65504).
// Needs, before it: cloud_lightning.glsl (kCv2FroxX/Y, cv2FroxJitter) and the clouds v2 UBO (cv2.skyAp).

const int   kSkyApX = 320, kSkyApY = 180, kSkyApZ = 48;   // == SatelliteSim.h kSkyApX/Y/Z (X, Y == the froxels')
const int   kSkyApCells = kSkyApX * kSkyApY * kSkyApZ;
const float kSkyApScale = 1e-4;
// The sky-lit / multiply scattered light per unit of air column is ~1e-7 .. 1e-5 (radiance over tens of km of air):
// below a half float's normal range (6e-5), quantised to visible contours. Stored x kSkyApShScale.
const float kSkyApShScale = 1e4;

#ifndef SKY_AP_SET
#define SKY_AP_SET 0
#endif
layout(std430, set = SKY_AP_SET, binding = SKY_AP_BINDING)
#ifdef SKY_AP_WRITE
writeonly
#else
readonly
#endif
buffer SkyApBuf {
    // Cell c: array A at [2c], [2c + 1] (four uints), array B at [2 kSkyApCells + c]. A runtime array, so a small
    // placeholder can be bound until the volume first runs (SatelliteSim::ensureSkyAp).
    uvec2 skyAp2[];
};
uvec4 skyApLoadA(int c) { return uvec4(skyAp2[2 * c], skyAp2[2 * c + 1]); }
uvec2 skyApLoadB(int c) { return skyAp2[2 * kSkyApCells + c]; }

int skyApCell(int x, int y, int z) { return (z * kSkyApY + y) * kSkyApX + x; }

// Slice coordinate (0..Z-1) of distance t on a ray entering the atmosphere at tStart with length L: log-spaced from BOTH
// ends (d0 from the start and from the end): fine near the eye for near surfaces, fine near the end where a ray
// descending into the dense air (from orbit, from an aircraft) gathers most of its light — a log from the start alone
// put the last ~17% of such a ray in one slice and its linear interpolation brightened every raised surface.
// u = (t - tStart) / L, a = d0 / L: s ~ log((a + u) / (a + 1 - u)), and its inverse u(s) for the volume's slices.
float skyApSliceU(float u, float a)
{
    float fm = log(1.0 + 1.0 / a);
    return (log((a + u) / (a + 1.0 - u)) + fm) / (2.0 * fm) * float(kSkyApZ - 1);
}
float skyApSlice(float t, float tStart, float L)
{
    L = max(L, 1.0);
    return skyApSliceU(clamp((t - tStart) / L, 0.0, 1.0), cv2.skyAp.y / L);
}
float skyApSliceInvU(float s, float a)
{
    float e = exp((2.0 * s / float(kSkyApZ - 1) - 1.0) * log(1.0 + 1.0 / a));
    return (e * (a + 1.0) - a) / (1.0 + e);
}
// The sums are stored per unit of the ray's own Rayleigh (accumR, the city glow, the shadow light, odR) and Mie (accumM,
// odM) air column from the eye (atmosphere.glsl atmColumn, Chapman, in sea-level vertical columns), and multiplied back
// by the pixel's own: per unit of air they change slowly across the screen and along the ray, where the raw sums grow
// like the path (1 / cos of the incidence near the limb), and a tile's bilinear blend of that convex growth read brighter
// than the pixel. The airglow columns are stored raw (a thin shell 90 km up). Needs atmosphere.glsl.
vec2 skyApNorm(vec3 o, vec3 d, float t)
{
    float tt = max(t, 1.0);
    return max(vec2(atmColumn(o, d, tt, H_R), atmColumn(o, d, tt, H_M)), vec2(1e-7));
}

struct SkyApSum {
    vec3  accR;     // as the loop's accumR / accumM (camera- and sun-attenuated, shadowed, terminator-gated)
    float accM;
    vec3  shadow;   // the sky-lit light of the air in the Earth's / the clouds' shadow (radiance, the loop's skyShadow)
    float city;     // accumCity
    float odR;      // odR_cam / odM_cam
    float odM;
    float airG;     // the green / sodium airglow columns (x their colour and gain in the pixel)
    float airS;
};

// The bilinear corner and weights at screen fraction uv (the tiles' samples at their corners + the frame's offset).
void skyApXY(vec2 uv, out ivec2 i0, out vec2 w)
{
    vec2 f = uv * vec2(kSkyApX, kSkyApY) - cv2FroxJitter().xy;
    f  = clamp(f, vec2(0.0), vec2(kSkyApX - 1, kSkyApY - 1) - 1e-3);
    i0 = ivec2(f);
    w  = f - vec2(i0);
}

void skyApAddA(inout SkyApSum r, uvec4 a, float k)
{
    vec2 a0 = unpackHalf2x16(a.x), a1 = unpackHalf2x16(a.y), a2 = unpackHalf2x16(a.z), a3 = unpackHalf2x16(a.w);
    r.accR   += k * vec3(a0, a1.x);
    r.accM   += k * a1.y;
    r.shadow += k * vec3(a2, a3.x);
    r.city   += k * a3.y;
}

// The full sums at slice s (trilinear: 4 tiles x 2 slices).
SkyApSum skyApRead(vec2 uv, float s)
{
    ivec2 i0; vec2 w;
    skyApXY(uv, i0, w);
    float sz = clamp(s, 0.0, float(kSkyApZ - 1) - 1e-3);
    int   z0 = int(sz);
    float wz = sz - float(z0);
    SkyApSum r = SkyApSum(vec3(0.0), 0.0, vec3(0.0), 0.0, 0.0, 0.0, 0.0, 0.0);
    vec4 b = vec4(0.0);
    for (int c = 0; c < 8; ++c) {
        int   dx = c & 1, dy = (c >> 1) & 1, dz = c >> 2;
        float k  = (dx != 0 ? w.x : 1.0 - w.x) * (dy != 0 ? w.y : 1.0 - w.y) * (dz != 0 ? wz : 1.0 - wz);
        int   ci = skyApCell(i0.x + dx, i0.y + dy, z0 + dz);
        skyApAddA(r, skyApLoadA(ci), k);
        uvec2 bb = skyApLoadB(ci);
        b += k * vec4(unpackHalf2x16(bb.x), unpackHalf2x16(bb.y));
    }
    r.accR *= 1.0 / kSkyApScale; r.accM *= 1.0 / kSkyApScale; r.city *= 1.0 / kSkyApScale; r.shadow *= 1.0 / kSkyApShScale;
    r.odR = b.x / kSkyApScale; r.odM = b.y / kSkyApScale; r.airG = b.z / kSkyApScale; r.airS = b.w / kSkyApScale;
    return r;
}

// Only array A (the air in front of the clouds: accumR / accumM, the shadow light, the city glow).
SkyApSum skyApReadA(vec2 uv, float s)
{
    ivec2 i0; vec2 w;
    skyApXY(uv, i0, w);
    float sz = clamp(s, 0.0, float(kSkyApZ - 1) - 1e-3);
    int   z0 = int(sz);
    float wz = sz - float(z0);
    SkyApSum r = SkyApSum(vec3(0.0), 0.0, vec3(0.0), 0.0, 0.0, 0.0, 0.0, 0.0);
    for (int c = 0; c < 8; ++c) {
        int   dx = c & 1, dy = (c >> 1) & 1, dz = c >> 2;
        float k  = (dx != 0 ? w.x : 1.0 - w.x) * (dy != 0 ? w.y : 1.0 - w.y) * (dz != 0 ? wz : 1.0 - wz);
        skyApAddA(r, skyApLoadA(skyApCell(i0.x + dx, i0.y + dy, z0 + dz)), k);
    }
    r.accR *= 1.0 / kSkyApScale; r.accM *= 1.0 / kSkyApScale; r.city *= 1.0 / kSkyApScale; r.shadow *= 1.0 / kSkyApShScale;
    return r;
}
