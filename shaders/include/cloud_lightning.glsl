#ifndef SATLIGHTSIM_CLOUD_LIGHTNING_GLSL
#define SATLIGHTSIM_CLOUD_LIGHTNING_GLSL
// ── Lightning: the flash list (2026-09-29, .plans/CLOUDS_V2_PLAN.md §9) ──────────────────────────
// cloud_v2_lightning.comp writes it once per frame (one workgroup, before the march): the flashes in
// progress at the Cb towers around the observer, each on a schedule hashed from its tower's lattice
// cell and a 2 s time slot of SIM time — deterministic, so a harness run, a replay and reversed time
// all see the same flashes. cloud_march.comp draws them after the temporal resolve (a flash lasts a
// few frames and the history would swallow it): the cloud lit from inside around each flash, and the
// channel of a cloud-to-ground stroke. The host reads the list back for thunder (SatelliteSimAmbience).
//
// Define CV2_FLASH_BINDING (and CV2_FLASH_WRITE for the writer) before including.
const uint kCv2FlashMax = 32u;
struct CV2Flash {
    vec4 a;   // xyz the light's centre (observer ENU about the Earth's centre, like obsPos), w intensity now
    vec4 b;   // xyz a cloud-to-ground stroke's ground end (= a.xyz in cloud), w 1 = cloud-to-ground,
              // 2 = a red sprite (a.xyz its centre ~75 km up)
    vec4 c;   // x id (uint bits), y the stroke's shape seed (uint bits), z age (s), w distance from the eye (m)
};
layout(std430, set = 0, binding = CV2_FLASH_BINDING)
#ifndef CV2_FLASH_WRITE
readonly
#endif
buffer CV2FlashBuf {
    uint     cv2FlashCount;
    uint     cv2FlashPad0, cv2FlashPad1, cv2FlashPad2;
    CV2Flash cv2Flashes[kCv2FlashMax];
};

// The cloud's glow around a flash, per unit intensity, at distance r (m) from it. Inside a storm the
// light diffuses (multiple scattering over a mean free path of tens of metres): a flash in a tower
// lights the whole tower and the anvil over it — from orbit a storm top blinks across 10-20 km —
// fading with the inverse square past a few km.
float cv2FlashGlow(float r)
{
    return exp(-r / 5000.0) / (1.0 + r * r * (1.0 / 6.25e6));
}

// A cloud-to-ground channel: kCv2BoltSeg segments from the cloud base (a) down to the ground (b),
// each vertex kicked sideways by a hash of the seed — the jagged path of a stepped leader (the kicks
// shrink toward both ends, which stay on their points). Point k of 0..kCv2BoltSeg.
const int kCv2BoltSeg = 14;
vec3 cv2BoltPoint(vec3 top, vec3 ground, uint seed, int k)
{
    vec3  ax  = ground - top;
    float len = length(ax);
    vec3  ad  = ax / max(len, 1.0);
    vec3  p1  = normalize(cross(ad, abs(ad.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
    vec3  p2  = cross(ad, p1);
    float f   = float(k) / float(kCv2BoltSeg);
    uint  h   = (seed ^ uint(k) * 0x9E3779B9u) * 0x85EBCA6Bu;
    h ^= h >> 13u; h *= 0xC2B2AE35u; h ^= h >> 16u;
    vec2  j   = vec2(float(h & 0xFFFFu), float(h >> 16u)) / 65535.0 - 0.5;
    // A random walk, not independent kicks: each vertex adds to the ones above it, so the channel
    // wanders off its straight line in long zig-zags (pinned back to the ground point at the end).
    vec2  w   = vec2(0.0);
    for (int i = 1; i <= k; ++i) {
        uint hi = (seed ^ uint(i) * 0x9E3779B9u) * 0x85EBCA6Bu;
        hi ^= hi >> 13u; hi *= 0xC2B2AE35u; hi ^= hi >> 16u;
        w += vec2(float(hi & 0xFFFFu), float(hi >> 16u)) / 65535.0 - 0.5;
    }
    float amp = len * 0.11;
    vec2  off = (w + j * 0.6) * amp * sin(3.14159265 * f);
    return mix(top, ground, f) + p1 * off.x + p2 * off.y;
}

// A side branch: from vertex kb of the main channel, kCv2BoltSeg / 3 segments angling down and away.
vec3 cv2BoltBranchPoint(vec3 top, vec3 ground, uint seed, int kb, int k)
{
    vec3  s0  = cv2BoltPoint(top, ground, seed, kb);
    vec3  ax  = ground - top;
    float len = length(ax);
    uint  h   = (seed * 0x27D4EB2Du) ^ 0x165667B1u;
    h ^= h >> 15u; h *= 0x85EBCA6Bu; h ^= h >> 13u;
    vec3  side = normalize(cross(ax, vec3(float(h & 0xFFu) - 127.5, float((h >> 8u) & 0xFFu) - 127.5, 0.1)));
    float f = float(k) / float(kCv2BoltSeg / 3);
    vec3  dirB = normalize(ax / max(len, 1.0) + side * 0.8);
    vec3  pk = s0 + dirB * (len * 0.3 * f);
    uint  hk = (seed ^ uint(k + 101) * 0x9E3779B9u) * 0x85EBCA6Bu; hk ^= hk >> 13u;
    return pk + side * (float(hk & 0xFFFFu) / 65535.0 - 0.5) * len * 0.05 * f;
}

#endif
