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
    // Written by the same pass for the drops at the eye (cloud_march.comp rainDrops, review 4): the Sun's
    // transmittance through the clouds from the eye (float bits), and the ice fog's extinction at the eye
    // (1/m, float bits: diamond dust sparkles when it is > 0 and the Sun is out). cv2FlashPad2 (review 22): the
    // Sun DISC's transmittance from the eye (not gated by the horizon), read back by the host.
    uint     cv2EyeSunT, cv2EyeIceS, cv2FlashPad2;
    CV2Flash cv2Flashes[kCv2FlashMax];
    // Review 22: the rain rate (0..1, as at the eye) on a kCv2RainMapN^2 grid of kCv2RainCellM cells about the eye,
    // at its height (ENU east x north, cell (i, j) centred at ((i, j) - N/2 + 0.5) x the cell), filled by the
    // lightning pass's extra workgroups; and each of those workgroups' maximum (0 = no rain near the eye).
    float    cv2RainWgMax[4];
    // Review 22b: the eye's march toward the Sun as a PROFILE: [0] its length L (0 = none), [1 + k] the optical depth
    // from the eye to L x (k / 16)^2, k = 0..16 (the march's own quadratic steps); [18..19] pad.
    float    cv2SunProf[20];
    float    cv2RainMap[1024];
};
const int   kCv2RainMapN  = 32;
const float kCv2RainCellM = 40.0;

// The cloud's glow around a flash, per unit intensity, at distance r (m) from it. Inside a storm the
// light diffuses (multiple scattering over a mean free path of tens of metres): a flash in a tower
// lights the whole tower and the anvil over it — from orbit a storm top blinks across 10-20 km —
// fading with the inverse square past a few km.
float cv2FlashGlow(float r)
{
    return exp(-r / 5000.0) / (1.0 + r * r * (1.0 / 6.25e6));
}

// A cloud-to-ground channel's main stroke has kCv2BoltSeg segments (cloud_march.comp lightningCS builds
// the whole branching tree: main channel, branches, twigs).
const int kCv2BoltSeg = 14;

#endif
