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
// The god rays' froxel volume (2026-10-22, cloud_v2_froxel.comp): screen tiles x log-spaced depths (cv2.frox).
const int   kCv2FroxX = 320;
const int   kCv2FroxY = 180;
const int   kCv2FroxZ = 64;
// The Sun transmittance volume (S3, 2026-10-22, .plans/CLOUD_REBUILD_PLAN.md): kCv2StvL nested levels of kCv2StvN^2
// parallel rays toward the Sun on the shaft lattice's axes, each level's cells 4x the last (cv2.stvL0..3.z), riding
// with the clouds and toroidal like the rings; each ray holds the Sun's transmittance BEYOND its point at kCv2StvK
// HEIGHTS (cv2.stvH.xy, evenly) on its sunward side, so a level covers every point within its reach across the Sun at
// any distance along it. [((level * N + J mod N) * N + I mod N) * K + k], packed as the rings' values (wrap stamp only).
const int   kCv2StvL = 4;
const int   kCv2StvN = 128;
const int   kCv2StvK = 48;
// The terrain Sun-horizon map (2026-10-22, cloud_v2_terrsun.comp): 4 nested levels of kCv2TshN^2 parallel lines toward
// the Sun on the volume's axes (cells cv2.tsh0..3.z, Earth-fixed: the lattice is snapped to whole cells of an ECEF
// coordinate, cv2.tsh*.xy the eye's fraction), each holding s_e = the distance along the Sun (from the plane through the
// eye) where its line, coming from the Sun, first enters the terrain (DEM); -1e30 = never. A point on the line further
// from the Sun than s_e is in the terrain's shadow. Rebaked whole every frame. [(level * N + j) * N + i].
const int   kCv2TshN = 128;
const int   kCv2SkyPsiN = 32;
const int   kCv2GroundSkyW = 64, kCv2GroundSkyH = 16;   // cv2GroundSky: every 4th column of the Sun table, rows 0..15
layout(std430, set = 0, binding = CV2_FLASH_BINDING)
#if !defined(CV2_FLASH_WRITE) && !defined(CV2_EVAL_COUNT)
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
    // two axes across the Sun's direction (observer ENU); [8..19] the tornado probes' columns (Earth-fixed directions,
    // written by the HOST: SatelliteSim::weatherFillTornadoes); the rest pad.
    float    cv2SunProf[20];
    float    cv2RainMap[1024];
    // Rain particles (2026-10-04, rain_particles.vert): the light on a drop at the eye, written by the lightning
    // pass (thread 1) with the same terms the cloud march gives a rain sample there (its key light and sky ambient),
    // so the drawn drops and the rain volume beyond them agree. Pre-exposure radiance, / the white balance.
    vec4     cv2RainKey;      // xyz the key light (Sun, or the Moon at night) x its cloud and Earth shadow, w 1 = the Moon
    vec4     cv2RainKeyDir;   // xyz the direction TO the key light (ENU at the eye), w the liquid share (1 rain .. 0 snow)
    vec4     cv2RainAmb;      // xyz the ambient (the sky through the cloud above + the ground), w the column's optical depth
    uvec4    cv2RainMisc;     // the tornado probes' results (cloud_v2_lightning.comp threads 2-5: base | optical depth)
    // Field-evaluation counters (2026-10-21, .plans/CLOUD_REBUILD_PLAN.md S0): zeroed by the host each frame, added to
    // only by shaders built with -DCV2_EVAL_COUNT (CV2_CNT below); harness `state` -> clouds_v2.eval_count.
    uint     cv2EvalCount[32];   // [16] cdc cells baked / [17] refused by the budget (cloud_v2_cdc.comp, always)
    // The clouds' Sun transmittance along the view on the froxel grid ([(z * Y + y) * X + x]; cloud_v2_froxel.comp).
    float    cv2GodFrox[kCv2FroxX * kCv2FroxY * kCv2FroxZ];
    float    cv2Stv[kCv2StvL * kCv2StvN * kCv2StvN * kCv2StvK];
    float    cv2TerrSun[4 * kCv2TshN * kCv2TshN];
    // The atmosphere's multiple-scattering function Psi (2026-10-26; cloud_v2_sunlut.comp pass 1 writes it with the
    // table's layer 4): [y * N + x], x the Sun's zenith cosine -1..1, y sqrt(height / the atmosphere's top). Here for
    // the sky pass, which has no sampled image left (cv2SkyPsiAt).
    vec4     cv2SkyPsi[kCv2SkyPsiN * kCv2SkyPsiN];
    // The sky's light on a horizontal face (2026-10-26, cloud_v2_sunlut.comp pass 2): the COSINE-weighted mean radiance
    // of the upper hemisphere (single + multiple scattering, the sky's units), i.e. the irradiance / pi. [y * W + x]:
    // x the Sun table's column 4x (its cube-root cosine), y its row (800 m levels, 0 .. 12 km).
    vec4     cv2GroundSky[kCv2GroundSkyW * kCv2GroundSkyH];
};
// Psi at a point (bilinear): the radiance of the light the air scattered twice or more reaching it, as if isotropic,
// in the sky's units; the in-scatter it gives is (sigma_R + sigma_M) x Psi (the phase's mean, 1).
vec3 cv2SkyPsiAt(float mu, float h)
{
    float x = clamp((mu * 0.5 + 0.5) * float(kCv2SkyPsiN) - 0.5, 0.0, float(kCv2SkyPsiN - 1));
    float y = clamp(sqrt(clamp(h / (R_ATMOS - R_EARTH), 0.0, 1.0)) * float(kCv2SkyPsiN) - 0.5, 0.0, float(kCv2SkyPsiN - 1));
    ivec2 i0 = ivec2(floor(vec2(x, y)));
    ivec2 i1 = min(i0 + 1, ivec2(kCv2SkyPsiN - 1));
    vec2  f  = vec2(x, y) - vec2(i0);
    vec3 a = mix(cv2SkyPsi[i0.y * kCv2SkyPsiN + i0.x].rgb, cv2SkyPsi[i0.y * kCv2SkyPsiN + i1.x].rgb, f.x);
    vec3 b = mix(cv2SkyPsi[i1.y * kCv2SkyPsiN + i0.x].rgb, cv2SkyPsi[i1.y * kCv2SkyPsiN + i1.x].rgb, f.x);
    return mix(a, b, f.y);
}
// The sky's light on a horizontal face at height h with the Sun's zenith cosine mu (bilinear): the cosine-weighted mean
// radiance of the sky above it (irradiance / pi), in the sky's units. Needs cv2_sunlut.glsl.
#ifdef CV2_SUNLUT_GLSL
vec3 cv2GroundSkyAt(float mu, float h)
{
    float x = clamp((cv2SunLutUV(mu, 0.0).x * kCv2SunLutW - 0.5) * 0.25, 0.0, float(kCv2GroundSkyW - 1));
    float y = clamp(h / kCv2SunLutStepM, 0.0, float(kCv2GroundSkyH - 1));
    ivec2 i0 = ivec2(floor(vec2(x, y)));
    ivec2 i1 = min(i0 + 1, ivec2(kCv2GroundSkyW - 1, kCv2GroundSkyH - 1));
    vec2  f  = vec2(x, y) - vec2(i0);
    vec3 a = mix(cv2GroundSky[i0.y * kCv2GroundSkyW + i0.x].rgb, cv2GroundSky[i0.y * kCv2GroundSkyW + i1.x].rgb, f.x);
    vec3 b = mix(cv2GroundSky[i1.y * kCv2GroundSkyW + i0.x].rgb, cv2GroundSky[i1.y * kCv2GroundSkyW + i1.x].rgb, f.x);
    return mix(a, b, f.y);
}
#endif
#ifdef CV2_EVAL_COUNT
#define CV2_CNT(i) atomicAdd(cv2EvalCount[i], 1u)
#else
#define CV2_CNT(i)
#endif
// The Sun transmittance volume's axes across the Sun's direction s (a horizontal, b the other; thread 1 of the lightning
// pass writes them to cv2SunProf[2..7] every frame, the lookup reads them there).
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
// The volume's values carry a STAMP (from the shaft rings it replaced, 2026-10-13/14): the lattice is toroidal (128 x 128
// rays), so as the eye moves a slot at the window's trailing edge becomes a cell at its leading edge. The CPU integrates the
// eye's lattice coordinate modulo 1024 cells (8 wraps of the window) and a value records WHICH wrap of its slot it was baked
// for (3 bits per axis; the third field, from the rings' knots, is 0): a slot that changed hands reads as invalid until it is
// baked again. A value is a float in [1, 2): 10 bits of transmittance, 9 of stamp and 2 of the slot's bake count (its running
// mean: 1, 1/2, 1/3, then 1/4 a bake). 0 (cleared) reads as invalid.
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
// ── The Sun transmittance volume (S3) ──
vec4 cv2StvLat(int lv) { return (lv == 0) ? cv2.stvL0 : (lv == 1) ? cv2.stvL1 : (lv == 2) ? cv2.stvL2 : cv2.stvL3; }
// A level's value at a point rl (from the eye, observer ENU) at height h: x the transmittance, y the level's weight there
// (0 outside its reach, while it fills, or where its slots changed hands since their bake).
vec2 cv2StvLevelT(vec3 rl, float h, int lv)
{
    vec4  la = cv2StvLat(lv);
    if (la.w <= 0.0) return vec2(1.0, 0.0);
    vec3  ga = vec3(cv2SunProf[2], cv2SunProf[3], cv2SunProf[4]);
    vec3  gb = vec3(cv2SunProf[5], cv2SunProf[6], cv2SunProf[7]);
    vec2  ce = vec2(dot(rl, ga), dot(rl, gb)) / la.z;
    // The window's centre: the eye, or from above mid-shell the point under it at mid-shell (cv2.stvH.w / stvV.w, m), as
    // the cascade's: centred on the eye in orbit, the clouds below sat hundreds of km off across a low Sun.
    vec2  cc = ce - vec2(cv2.stvH.w, cv2.stvV.w) / la.z;
    float w  = (1.0 - smoothstep(48.0, 56.0, max(abs(cc.x), abs(cc.y)))) * la.w;
    if (w <= 0.0) return vec2(1.0, 0.0);
    vec2  gg = ce + la.xy + 4096.0;
    ivec2 g0 = ivec2(gg);
    vec2  gf = gg - vec2(g0);
    float kk = clamp((h - cv2.stvH.x) / max(cv2.stvH.y - cv2.stvH.x, 1.0) * float(kCv2StvK - 1), 0.0, float(kCv2StvK - 1) - 1e-3);
    int   k0 = int(kk);
    float kf = kk - float(k0);
    float Tb = 0.0, wv = 0.0;
    int   lb = lv * kCv2StvN * kCv2StvN;
    [[dont_unroll]] for (int c = 0; c < 4; ++c) {
        ivec2 sc = ivec2(g0.x + (c & 1), g0.y + (c >> 1));
        int ri = ((lb + (sc.y & (kCv2StvN - 1)) * kCv2StvN + (sc.x & (kCv2StvN - 1))) * kCv2StvK) + k0;
        uint b0 = floatBitsToUint(cv2Stv[ri]), b1 = floatBitsToUint(cv2Stv[ri + 1]);
        uint st = cv2ShaftStampOf(sc.x, sc.y, 0);
        float bw = ((c & 1) != 0 ? gf.x : 1.0 - gf.x) * ((c >> 1) != 0 ? gf.y : 1.0 - gf.y);
        bw *= (cv2ShaftStampOk(b0, st) && cv2ShaftStampOk(b1, st)) ? 1.0 : 0.0;
        Tb += mix(cv2ShaftT(b0), cv2ShaftT(b1), kf) * bw;
        wv += bw;
    }
    return vec2(Tb / max(wv, 1e-4), w * wv);
}
// The volume's transmittance at rl, height h: the finest level that reaches the point, blended into the next one out
// over its edge; 1 (lit) beyond the coarsest. ONE call site of the level lookup (a loop).
// (From the finest level out since 2026-10-24, the perf pass: the same sum, sum_l T_l w_l prod_{finer} (1 - w), stopping
// once the finer levels cover the point (rem < 1e-5: the rest can move T by less than that). Coarsest first it read all
// four levels (32 reads of the flash buffer) for every lit sample, the thin ice's too: -2 ms of cloud march under an
// anvil at 12 km, issue 20261023_230322.)
float cv2StvT(vec3 rl, float h)
{
    float acc = 0.0, rem = 1.0;
    [[dont_unroll]] for (int lv = 0; lv < kCv2StvL; ++lv) {
        vec2 s = cv2StvLevelT(rl, h, lv);
        acc += rem * s.x * s.y;
        rem *= 1.0 - s.y;
        if (rem < 1e-5) break;
    }
    return acc + rem;
}
// The same value, from the finest level out, stopping once the finer levels cover the point (within 1e-3): a point
// inside level 0's window reads one level instead of four. For the god rays (the froxels and the sky pass's direct
// path); the cloud march takes cv2StvT (the same order, to 1e-5: the key light's shadow is not god rays).
float cv2StvTFine(vec3 rl, float h)
{
    float acc = 0.0, rem = 1.0;
    [[dont_unroll]] for (int lv = 0; lv < kCv2StvL; ++lv) {
        vec2 s = cv2StvLevelT(rl, h, lv);
        acc += rem * s.x * s.y;
        rem *= 1.0 - s.y;
        if (rem < 1e-3) break;
    }
    return acc + rem;
}
// ── The terrain Sun-horizon map: the terrain's shadow at rl (from the eye, observer ENU). 1 = lit. Not for the ground
// itself (sat_sky.frag's terrainSunShadow draws that sharply): a point within about a cell of a sunlit slope may read a
// neighbouring line's entry, so the shadow starts a cell + 30 m past the entry and softens over another cell.
vec4 cv2TshLat(int lv) { return (lv == 0) ? cv2.tsh0 : (lv == 1) ? cv2.tsh1 : (lv == 2) ? cv2.tsh2 : cv2.tsh3; }
float cv2TerrSunT(vec3 rl)
{
    if (cv2.tsh0.w < 0.5) return 1.0;
    vec3  ga = vec3(cv2SunProf[2], cv2SunProf[3], cv2SunProf[4]);
    vec3  gb = vec3(cv2SunProf[5], cv2SunProf[6], cv2SunProf[7]);
    float sP = dot(rl, cross(ga, gb));                       // along the Sun (ga x gb = S)
    vec2  ab = vec2(dot(rl, ga), dot(rl, gb));
    float acc = 0.0, keep = 1.0;
    [[dont_unroll]] for (int lv = 3; lv >= 0; --lv) {
        vec4  la = cv2TshLat(lv);
        vec2  gg = ab / la.z + la.xy + float(kCv2TshN / 2);   // line (i, j) sits at (i, j) - N/2 - frac cells from the eye
        float w  = 1.0 - smoothstep(float(kCv2TshN / 2) - 14.0, float(kCv2TshN / 2) - 3.0,
                                    max(abs(gg.x - float(kCv2TshN / 2)), abs(gg.y - float(kCv2TshN / 2))));
        if (w <= 0.0) continue;
        ivec2 g0 = ivec2(floor(gg));
        vec2  gf = gg - vec2(g0);
        float m  = la.z + 30.0;
        float T  = 0.0;
        [[dont_unroll]] for (int c = 0; c < 4; ++c) {
            ivec2 sc = clamp(ivec2(g0.x + (c & 1), g0.y + (c >> 1)), ivec2(0), ivec2(kCv2TshN - 1));
            float se = cv2TerrSun[(lv * kCv2TshN + sc.y) * kCv2TshN + sc.x];
            float bw = ((c & 1) != 0 ? gf.x : 1.0 - gf.x) * ((c >> 1) != 0 ? gf.y : 1.0 - gf.y);
            float x  = clamp((sP - se + m + la.z) / la.z, 0.0, 1.0);   // (smoothstep with se = -1e30 has equal edges)
            T += bw * x * x * (3.0 - 2.0 * x);
        }
        acc  = acc * (1.0 - w) + T * w;
        keep *= 1.0 - w;
    }
    return acc + keep;
}
// cv2TerrSunT from the finest level out (as cv2StvTFine), for the god rays.
float cv2TerrSunTFine(vec3 rl)
{
    if (cv2.tsh0.w < 0.5) return 1.0;
    vec3  ga = vec3(cv2SunProf[2], cv2SunProf[3], cv2SunProf[4]);
    vec3  gb = vec3(cv2SunProf[5], cv2SunProf[6], cv2SunProf[7]);
    float sP = dot(rl, cross(ga, gb));                       // along the Sun (ga x gb = S)
    vec2  ab = vec2(dot(rl, ga), dot(rl, gb));
    float acc = 0.0, rem = 1.0;
    [[dont_unroll]] for (int lv = 0; lv < 4; ++lv) {
        vec4  la = cv2TshLat(lv);
        vec2  gg = ab / la.z + la.xy + float(kCv2TshN / 2);   // line (i, j) sits at (i, j) - N/2 - frac cells from the eye
        float w  = 1.0 - smoothstep(float(kCv2TshN / 2) - 14.0, float(kCv2TshN / 2) - 3.0,
                                    max(abs(gg.x - float(kCv2TshN / 2)), abs(gg.y - float(kCv2TshN / 2))));
        if (w <= 0.0) continue;
        ivec2 g0 = ivec2(floor(gg));
        vec2  gf = gg - vec2(g0);
        float m  = la.z + 30.0;
        float T  = 0.0;
        [[dont_unroll]] for (int c = 0; c < 4; ++c) {
            ivec2 sc = clamp(ivec2(g0.x + (c & 1), g0.y + (c >> 1)), ivec2(0), ivec2(kCv2TshN - 1));
            float se = cv2TerrSun[(lv * kCv2TshN + sc.y) * kCv2TshN + sc.x];
            float bw = ((c & 1) != 0 ? gf.x : 1.0 - gf.x) * ((c >> 1) != 0 ? gf.y : 1.0 - gf.y);
            float x  = clamp((sP - se + m + la.z) / la.z, 0.0, 1.0);   // (smoothstep with se = -1e30 has equal edges)
            T += bw * x * x * (3.0 - 2.0 * x);
        }
        acc += rem * T * w;
        rem *= 1.0 - w;
        if (rem < 1e-3) break;
    }
    return acc + rem;
}
// The god rays' froxel volume: slice s (0..kCv2FroxZ) <-> distance along the view ray, log-spaced from cv2.frox.y (m) by
// cv2.frox.z (the log ratio per slice).
float cv2FroxDepth(float s) { return cv2.frox.y * exp(s * cv2.frox.z); }
// The froxels' sample offset this frame, in froxels (cv2.frox.w = the frame's sequence index, < 0 = none: the tile and
// slice centres). The grid moves by a 3D R-sequence each frame and the sky TAA averages the frames' reconstructions:
// the volume's samples cover the tile instead of one point (thin shafts no longer alias), and the bilinear kinks at the
// tile edges (the god rays' "pixelation") average into a smooth tent. Without the sky TAA nothing would average it.
vec3 cv2FroxJitter()
{
    return (cv2.frox.w < 0.0) ? vec3(0.5) : fract(vec3(0.5) + cv2.frox.w * vec3(0.8191725, 0.6710436, 0.5497005));
}
// The Sun transmittance at screen fraction uv (0..1, top-left origin as gl_FragCoord / screen) and distance t: trilinear
// over the froxels' samples (at their cells' corners + cv2FroxJitter()).
float cv2GodFroxT(vec2 uv, float t, vec3 jit)
{
    vec3 f = vec3(uv * vec2(kCv2FroxX, kCv2FroxY), log(max(t, cv2.frox.y) / cv2.frox.y) / cv2.frox.z) - jit;
    f = clamp(f, vec3(0.0), vec3(kCv2FroxX - 1, kCv2FroxY - 1, kCv2FroxZ - 1) - 1e-3);
    ivec3 i = ivec3(f);
    vec3  w = f - vec3(i);
    int   b = (i.z * kCv2FroxY + i.y) * kCv2FroxX + i.x;
    const int sy = kCv2FroxX, sz = kCv2FroxX * kCv2FroxY;
    float c00 = mix(cv2GodFrox[b],           cv2GodFrox[b + 1],           w.x);
    float c10 = mix(cv2GodFrox[b + sy],      cv2GodFrox[b + sy + 1],      w.x);
    float c01 = mix(cv2GodFrox[b + sz],      cv2GodFrox[b + sz + 1],      w.x);
    float c11 = mix(cv2GodFrox[b + sz + sy], cv2GodFrox[b + sz + sy + 1], w.x);
    return mix(mix(c00, c10, w.y), mix(c01, c11, w.y), w.z);
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
