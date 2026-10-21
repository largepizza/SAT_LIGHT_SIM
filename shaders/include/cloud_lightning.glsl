#ifndef SATLIGHTSIM_CLOUD_LIGHTNING_GLSL
#define SATLIGHTSIM_CLOUD_LIGHTNING_GLSL
// ── Lightning: the flash list (2026-09-29, .plans/CLOUDS_V2_PLAN.md §9) ──────────────────────────
// cloud_v2_lightning.comp writes it once per frame (one workgroup, before the march): the flashes in
// progress at the Cb towers around the observer, each on a schedule hashed from its tower's lattice
// cell and a 2 s time slot of SIM time — deterministic, so a harness run, a replay and reversed time
// all see the same flashes. The host reads the list back (SatelliteSimLightning.cpp: updateLightningBolts) and
// draws each flash as geometry after the sky TAA (lightning.vert/.frag: the channel tree and the cloud lit
// around it), and builds the thunder from the channel.
//
// Define CV2_FLASH_BINDING (and CV2_FLASH_WRITE for the writer) before including.
const uint kCv2FlashMax = 128u;
struct CV2Flash {
    vec4 a;   // xyz the flash's origin (observer ENU about the Earth's centre, like obsPos), w intensity now
    vec4 b;   // xyz a cloud-to-ground stroke's ground end (= a.xyz otherwise), w the KIND: 0 in the cloud,
              // 1 cloud-to-ground, 2 a red sprite (a.xyz its centre ~75 km up), 3 spider lightning under the base
    vec4 c;   // x id (uint bits), y the shape seed (uint bits), z age (s, from the leader's start), w distance (m)
    vec4 d;   // x the cloud's base, y its top (m above sea level), z the storm's strength, w the flash's duration (s)
};
const float kCv2FlashSlot = 2.0;     // the schedule's time slot (s of sim time)
const float kCv2LeaderS   = 0.05;    // the stepped leader before the first return stroke (s)

// A flash's brightness at `age` (s since its leader started): nothing during the leader (its faint channel is
// the host's to draw), then 1-4 return strokes (sharp pulses, ~35 ms) over a fading continuing current; a flash
// in the cloud flickers through 2-6 softer pulses over a longer glow. fh: the flash's hash.
float cv2FlashIntensity(float age, float dur, uvec3 fh, bool cg)
{
    float a = age - kCv2LeaderS;
    if (a < 0.0) return 0.0;
    int   nS = cg ? 1 + int(3.99 * float(fh.y >> 16u) / 65535.0) : 2 + int(4.99 * float(fh.y >> 16u) / 65535.0);
    float tau = cg ? 0.035 : 0.06;
    float I = 0.0;
    for (int k = 0; k < 6; ++k) {
        if (k >= nS) break;
        float tk = (k == 0) ? 0.0 : dur * (float(k) + 0.3 * float((fh.z >> uint(5 * k)) & 0x1Fu) / 31.0) / float(nS);
        if (a >= tk) I += (cg ? 1.0 : 0.6) * exp(-(a - tk) / tau);
    }
    return I + 0.3 * smoothstep(0.0, 0.02, a) * exp(-a / (0.4 * dur + 0.05));
}
layout(std430, set = 0, binding = CV2_FLASH_BINDING)
#ifndef CV2_FLASH_WRITE
readonly
#endif
buffer CV2FlashBuf {
    uint     cv2FlashCount;
    // Written by the same pass for the rain at the eye (review 4; read by the rain particles): the Sun's
    // transmittance through the clouds from the eye (float bits), and the ice fog's extinction at the eye
    // (1/m, float bits: diamond dust sparkles when it is > 0 and the Sun is out). cv2FlashPad2 (review 22): the
    // Sun DISC's transmittance from the eye (not gated by the horizon), read back by the host.
    uint     cv2EyeSunT, cv2EyeIceS, cv2FlashPad2;
    CV2Flash cv2Flashes[kCv2FlashMax];
    // Review 22: the rain rate (0..1, as at the eye) on a kCv2RainMapN^2 grid of kCv2RainCellM cells about the eye,
    // at its height (ENU east x north, cell (i, j) centred at ((i, j) - N/2 + 0.5) x the cell), filled by the
    // lightning pass's extra workgroups; and each of those workgroups' maximum (0 = no rain near the eye).
    float    cv2RainWgMax[4];
    // The Sun's shadow about the eye's line to it (review 22b; a grid of rays since 2026-10-10): [0] the rays' length
    // L (0 = none), [2..4] / [5..7] the grid's
    // two axes across the Sun's direction (observer ENU); the rest pad.
    float    cv2SunProf[20];
    float    cv2RainMap[1024];
    // Rain particles (2026-10-04, rain_particles.vert): the light on a drop at the eye, written by the lightning
    // pass (thread 1) with the same terms the cloud march gives a rain sample there (its key light and sky ambient),
    // so the drawn drops and the rain volume beyond them agree. Pre-exposure radiance, / the white balance.
    vec4     cv2RainKey;      // xyz the key light (Sun, or the Moon at night) x its cloud and Earth shadow, w 1 = the Moon
    vec4     cv2RainKeyDir;   // xyz the direction TO the key light (ENU at the eye), w the liquid share (1 rain .. 0 snow)
    vec4     cv2RainAmb;      // xyz the ambient (the sky through the cloud above + the ground), w the column's optical depth
    vec4     cv2RainMisc;     // spare
    // kCv2SunGridN^2 parallel rays from about the eye toward the Sun, kCv2SunGridM apart along the two axes (ray
    // r = iy * N + ix at ((ix, iy) - (N - 1) / 2) x the spacing): [r * 17 + k] = the Sun's transmittance through the
    // cloud along ray r BEYOND its point at L x (k / 16)^2, k = 0..16 (the mean over the ray's cell). cv2SunWgMax:
    // the largest optical depth each of the four workgroups' rays found (all 0 = nothing to shadow).
    float    cv2SunWgMax[4];
    float    cv2SunGrid[17408];
    // The Sun shadow CASCADE (2026-10-10): the same thing over the whole view. kCv2CascNA x kCv2CascNB parallel rays
    // on a lattice across the Sun's direction (the fine grid's axes), a = horizontal, b = the other; the lattice
    // RIDES WITH THE CLOUDS (the CPU integrates the eye's motion relative to them into its lattice coordinate,
    // cv2.sunCasc0.xy) and is stored toroidally: ray (I, J) at [(J mod NB) * NA + (I mod NA)], so a ray keeps its
    // slot while the eye moves and only the window's edge changes hands. [ray * (K + 1) + k] = the Sun's
    // transmittance through the cloud BEYOND knot k of that ray (cv2CascKnotS).
    float    cv2SunCasc[200704];
    // The SHAFT lattice (2026-10-10): the Sun's shadow near the eye at the scale of single clouds, for the god rays
    // (the sky pass samples it densely over its first kCv2ShaftRangeM). kCv2ShaftN^2 parallel rays kCv2ShaftCellM
    // apart across the Sun (the same axes), kCv2ShaftK knots cv2.sunCasc5.y apart ALONG it; it rides with the clouds
    // in all three coordinates (cv2.sunCasc4.xyz = the eye on it) and is toroidal in all three: ray (I, J), knot n at
    // [((J mod N) * N + (I mod N)) * K + (n mod K)] = the Sun's transmittance BEYOND that knot (the cascade's value
    // at the window's sunward end x the cloud between). 512 rays are baked a frame (all in 32).
    // Two RINGS of it (2026-10-12), ring r at [r * kCv2ShaftN^2 * kCv2ShaftK + ...]: ring 0 with cells of
    // cv2.sunCasc7.z (250 m), ring 1 of cv2.sunCasc7.w (1000 m; lattice cv2.sunCasc6, knots cv2.sunCasc7.y apart).
    float    cv2SunShaft[1048576];
};
const int   kCv2ShaftN      = 128;
const int   kCv2ShaftK      = 32;
const float kCv2ShaftRangeCells = 56.0;  // a ring's reach from the eye across the Sun, in its cells (inside its +-64)
const int   kCv2CascNA = 128;
const int   kCv2CascNB = 32;
const int   kCv2CascK  = 48;     // knot intervals: kCv2CascKU uniform ones through the window, the rest stretched sunward
const int   kCv2CascKU = 40;

// Knot k's distance along the Sun from the window's anti-Sun end (m). x = the uniform part's half depth, y = the stretch.
float cv2CascKnotS(float k, vec2 zs)
{
    float q = max(k - float(kCv2CascKU), 0.0) / float(kCv2CascK - kCv2CascKU);
    return min(k, float(kCv2CascKU)) * (2.0 * zs.x / float(kCv2CascKU)) + zs.y * q * q;
}
// ...and back: the (fractional) knot at that distance.
float cv2CascKnotAt(float s, vec2 zs)
{
    float u = s * (float(kCv2CascKU) / (2.0 * zs.x));
    if (u > float(kCv2CascKU))
        u = float(kCv2CascKU) + float(kCv2CascK - kCv2CascKU) * sqrt(clamp((s - 2.0 * zs.x) / zs.y, 0.0, 1.0));
    return clamp(u, 0.0, float(kCv2CascK));
}
const int   kCv2SunGridN = 32;
const float kCv2SunGridM = 250.0;

// The grid's axes across the Sun's direction s, and its rays' length (the shell's depth along the Sun's direction, at most 400 km).
void cv2SunGridAxes(vec3 s, out vec3 a, out vec3 b)
{
    a = normalize(cross(s, abs(s.z) > 0.99 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 0.0, 1.0)));
    b = cross(s, a);
}
// NOT a function of the eye's height: the rays' sample depths scale with it, and with a length that shrank as the
// eye rose every sample slid along its ray, so the far shadows (10-km steps) changed with 100 m of climb.
float cv2SunGridLen(vec3 s, float shellTop)
{
    return min(shellTop / max(s.z, 0.03), 400000.0);
}
// The shaft rings' values carry a STAMP (2026-10-13, issue 20261013_220718; exact since round 5): the lattice is
// toroidal (128 x 128 rays, 32 knots), so when the eye moves a slot at the window's trailing edge becomes a cell at its
// leading edge and keeps the other side's shadow until it is baked again. The CPU integrates the eye's lattice
// coordinate modulo 1024 cells (256 knots), i.e. 8 wraps of the window, and a value records WHICH wrap of its slot it
// was baked for (3 bits per axis): a slot that changed hands reads as invalid until it is baked, at any speed up to
// 8 windows between two bakes of a slot. A value is a float in [1, 2): 10 bits of transmittance, 9 of wraps and 2 of
// the slot's bake count (its running mean: 1, 1/2, 1/3, then 1/4 a bake). 0 (cleared) reads as invalid.
uint cv2ShaftStampOf(int ia, int ib, int ik)      // absolute cell (mod 1024) / knot (mod 256)
{
    return uint((ia >> 7) & 7) | (uint((ib >> 7) & 7) << 3u) | (uint((ik >> 5) & 7) << 6u);
}
bool cv2ShaftStampOk(uint bits, uint want)
{
    return (bits & 0xFFE00000u) == 0x3F800000u && ((bits >> 10u) & 511u) == want;
}
float cv2ShaftT(uint bits) { return float(bits & 1023u) * (1.0 / 1023.0); }
uint  cv2ShaftCount(uint bits) { return (bits >> 19u) & 3u; }
float cv2ShaftPack(float T, uint stamp, uint cnt)
{
    return uintBitsToFloat(0x3F800000u | (min(cnt, 3u) << 19u) | (stamp << 10u) | uint(clamp(T, 0.0, 1.0) * 1023.0 + 0.5));
}
#ifdef CV2_SUN_CASC_FN
// A ring's lattice: xy the eye's coordinate across the Sun (cells, mod 1024), z along it (knots, mod 256), w its fill
// weight; and the eye's motion this frame across the Sun in ring 0's cells (the bake follows it to the leading edge).
vec4 cv2ShaftLat(int ring) { return (ring == 0) ? cv2.sunCasc4 : cv2.sunCasc6; }
vec2 cv2ShaftVel() { return cv2.sunCasc5.zw; }
// The Sun shadow CASCADE's transmittance (cloud_lightning.glsl cv2SunCasc) at a point rl (from the eye, observer
// ENU) and a distance sAlong along the Sun from the eye (the caller adds its bias): four rays bilinear on the toroidal
// lattice, two knots. 1 outside the window and while the cascade fills. Read by the cloud samples' key light
// (cloud_v2_march.comp) and by the sky's air samples (sat_sky.frag). Define CV2_SUN_CASC_FN (it needs the cv2 UBO).
float cv2SunCascT(vec3 rl, float sAlong)
{
    vec3  ga = vec3(cv2SunProf[2], cv2SunProf[3], cv2SunProf[4]);
    vec3  gb = vec3(cv2SunProf[5], cv2SunProf[6], cv2SunProf[7]);
    vec2  ce = vec2(dot(rl, ga) / cv2.sunCasc1.x, dot(rl, gb) / cv2.sunCasc1.y);     // cells from the eye
    vec2  cc = abs(ce - vec2(0.0, cv2.sunCasc0.z));                                   // ... from the centre
    // (The a edge fades over the window's last fifth: over 3.5 cells the far shadows ended on a visible line.)
    float wC = (1.0 - smoothstep(0.4 * float(kCv2CascNA), 0.5 * float(kCv2CascNA) - 1.5, cc.x))
             * (1.0 - smoothstep(0.5 * float(kCv2CascNB) - 3.5, 0.5 * float(kCv2CascNB) - 1.5, cc.y))
             * cv2.sunCasc2.x;
    if (wC <= 0.0) return 1.0;
    // Each ray half (rows 0-15 / 16-31) is read at the knots it was baked for (cv2.sunCasc8.xy, 2026-10-21): the knots
    // stand along the Sun relative to the EYE, so while the eye moves a bake a few frames old is shifted along its rays
    // — from orbit (~330 km/s at the height-scaled speed) by several knots, and the window read the cloud's own shadow
    // (a dark, ray-streaked rectangle under the eye while flying).
    float sK = sAlong - cv2.sunCasc0.w + cv2.sunCasc1.z;
    vec2  uh = vec2(cv2CascKnotAt(sK + cv2.sunCasc8.x, cv2.sunCasc1.zw), cv2CascKnotAt(sK + cv2.sunCasc8.y, cv2.sunCasc1.zw));
    vec2  gg = ce + cv2.sunCasc0.xy + vec2(float(kCv2CascNA), float(kCv2CascNB)) * 8.0;
    ivec2 g0 = ivec2(gg);
    vec2  gf = gg - vec2(g0);
    float Tb = 0.0;
    [[dont_unroll]] for (int c = 0; c < 4; ++c) {
        int   row = (g0.y + (c >> 1)) & (kCv2CascNB - 1);
        float uu  = (row < kCv2CascNB / 2) ? uh.x : uh.y;
        int   k0  = min(int(uu), kCv2CascK - 1);
        int ri = ((row * kCv2CascNA + ((g0.x + (c & 1)) & (kCv2CascNA - 1))) * (kCv2CascK + 1)) + k0;
        Tb += mix(cv2SunCasc[ri], cv2SunCasc[ri + 1], uu - float(k0))
            * ((c & 1) != 0 ? gf.x : 1.0 - gf.x) * ((c >> 1) != 0 ? gf.y : 1.0 - gf.y);
    }
    return mix(1.0, Tb, wC);
}
// The SHAFT lattice's ring `ring` at a point rl (from the eye, observer ENU), sAlong = dot(rl, the Sun's direction)
// (+ the caller's offset): x the Sun's transmittance, y the ring's weight there (0 outside its window or while it
// fills: use the next ring out, then the cascade).
vec2 cv2SunShaftT(vec3 rl, float sAlong, int ring)
{
    vec4  la   = cv2ShaftLat(ring);
    if (la.w <= 0.0) return vec2(1.0, 0.0);
    float cell = (ring == 0) ? cv2.sunCasc7.z : cv2.sunCasc7.w;
    vec3  ga = vec3(cv2SunProf[2], cv2SunProf[3], cv2SunProf[4]);
    vec3  gb = vec3(cv2SunProf[5], cv2SunProf[6], cv2SunProf[7]);
    vec2  ce = vec2(dot(rl, ga), dot(rl, gb)) / cell;
    float ks = sAlong / ((ring == 0) ? cv2.sunCasc5.y : cv2.sunCasc7.y);
    float w  = (1.0 - smoothstep(kCv2ShaftRangeCells, 62.0, max(abs(ce.x), abs(ce.y)))) * (1.0 - smoothstep(12.0, 14.5, abs(ks)))
             * la.w;
    if (w <= 0.0) return vec2(1.0, 0.0);
    // Absolute cells (mod 1024) and knots (mod 256): the offsets keep them positive.
    vec2  gg = ce + la.xy + 4096.0;
    ivec2 g0 = ivec2(gg);
    vec2  gf = gg - vec2(g0);
    float kk = ks + la.z + 1024.0;
    int   k0 = int(kk);
    float kf = kk - float(k0);
    float Tb = 0.0, wv = 0.0;
    int   rb = ring * kCv2ShaftN * kCv2ShaftN * kCv2ShaftK;
    [[dont_unroll]] for (int c = 0; c < 4; ++c) {
        ivec2 sc = ivec2(g0.x + (c & 1), g0.y + (c >> 1));
        int ri = rb + ((sc.y & (kCv2ShaftN - 1)) * kCv2ShaftN + (sc.x & (kCv2ShaftN - 1))) * kCv2ShaftK;
        uint b0 = floatBitsToUint(cv2SunShaft[ri + (k0 & (kCv2ShaftK - 1))]);
        uint b1 = floatBitsToUint(cv2SunShaft[ri + ((k0 + 1) & (kCv2ShaftK - 1))]);
        float bw = ((c & 1) != 0 ? gf.x : 1.0 - gf.x) * ((c >> 1) != 0 ? gf.y : 1.0 - gf.y);
        bw *= (cv2ShaftStampOk(b0, cv2ShaftStampOf(sc.x, sc.y, k0)) && cv2ShaftStampOk(b1, cv2ShaftStampOf(sc.x, sc.y, k0 + 1)))
            ? 1.0 : 0.0;
        Tb += mix(cv2ShaftT(b0), cv2ShaftT(b1), kf) * bw;
        wv += bw;
    }
    // Slots not baked since they changed hands (the toroidal window's leading edge after a move) are left out: the
    // next ring out / the cascade stands in for them until they are baked.
    return vec2(Tb / max(wv, 1e-4), w * wv);
}
// Ring 0 alone (the sky's god rays, within kCv2ShaftRangeCells x its cell of the eye).
vec2 cv2SunShaftT(vec3 rl, float sAlong) { return cv2SunShaftT(rl, sAlong, 0); }
// Both rings, the inner one where it reaches: x the transmittance, y their combined weight over the cascade.
// (One call site of the lookup: the cloud march inlines it per sample.)
vec2 cv2SunShaftRings(vec3 rl, float sAlong)
{
    float acc = 0.0, keep = 1.0;
    [[dont_unroll]] for (int ring = 1; ring >= 0; --ring) {
        vec2 s = cv2SunShaftT(rl, sAlong, ring);
        acc  = acc * (1.0 - s.y) + s.x * s.y;
        keep *= 1.0 - s.y;
    }
    return vec2(acc / max(1.0 - keep, 1e-5), 1.0 - keep);
}
#endif
const int   kCv2RainMapN  = 32;
const float kCv2RainCellM = 40.0;

// The rain's own variability: showers come in bursts (a value noise over ~6 s of sim time, on top of the rain
// field's rate). 0.35..1. (Moved here from cloud_march.comp with the drops, 2026-10-04.)
float cv2RainHash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
float cv2RainBurst(float t)
{
    float i = floor(t / 6.0), f = fract(t / 6.0);
    return mix(0.35, 1.0, mix(cv2RainHash(vec2(i, 7.1)), cv2RainHash(vec2(i + 1.0, 7.1)), f * f * (3.0 - 2.0 * f)));
}

// The rain map at en (metres east / north of the eye), bilinear, 0 off it.
float cv2RainMapAt(vec2 en)
{
    vec2  g  = en / kCv2RainCellM + 0.5 * float(kCv2RainMapN) - 0.5;
    vec2  g0 = floor(g), f = g - g0;
    float s  = 0.0;
    for (int k = 0; k < 4; ++k) {
        ivec2 c = ivec2(g0) + ivec2(k & 1, k >> 1);
        if (c.x < 0 || c.y < 0 || c.x >= kCv2RainMapN || c.y >= kCv2RainMapN) continue;
        s += cv2RainMap[c.y * kCv2RainMapN + c.x] * ((k & 1) != 0 ? f.x : 1.0 - f.x) * ((k >> 1) != 0 ? f.y : 1.0 - f.y);
    }
    return s;
}

#endif
