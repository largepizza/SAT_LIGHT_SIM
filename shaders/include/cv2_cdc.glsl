#ifndef SATLIGHTSIM_CV2_CDC_GLSL
#define SATLIGHTSIM_CV2_CDC_GLSL
// ── The cloud density clipmap (2026-10-21, .plans/CLOUD_REBUILD_PLAN.md S1 / S4) ───────────────────────────────────────
// EXPERIMENT. A baked, conservative occupancy of the COARSE field (cv2Field with detailAmt -1, what the view march's
// coarse steps read) about the eye, so a coarse step in known-empty air skips its field evaluation. S0 measured those
// steps at 70-94% of every field evaluation of a frame (harness_runs/s0).
//
// kCv2CdcL levels of kCv2CdcN x kCv2CdcN horizontal cells (cell = cdcA.z x 2^level, from 100 m) on the layer shadow
// map's gnomonic plane (cv2.layA / layE / layN: an anchor fixed in the DRIFTED cloud frame, so the drift moves nothing),
// stored toroidally about the eye, x kCv2CdcNZ cells of kCv2CdcDZ in height above sea level. Each cell holds the MAX
// coarse extinction over 9 points of a box TWICE its size (centre + 8 corners: a dilation by half a cell each way) and a
// 16-bit stamp: the window wrap of its slot (a slot that changed hands as the eye moved), the epoch (the anchor, the field
// settings, a time jump), and a staggered sim-time quantum of half a cell of wind slide at kCv2CdcWindMps. A cell whose
// stamp does not match reads UNKNOWN and the march evaluates the field as before: correctness never depends on the fill.
// Define CV2_CDC_BINDING (and CV2_CDC_WRITE for the bake) after clouds_v2.glsl.
const int   kCv2CdcN   = 128;
const int   kCv2CdcNZ  = 64;
const int   kCv2CdcL   = 8;
const float kCv2CdcDZ  = 320.0;      // m: 64 cells to 20.48 km above sea level
const float kCv2CdcR   = 6371000.0;  // == kCv2LayR (the plane's radius)
const int   kCv2CdcOff = 1 << 20;    // keeps absolute cell indices positive for the wrap bits
const int   kCv2CdcLevelCells = kCv2CdcN * kCv2CdcN * kCv2CdcNZ;
const int   kCv2CdcTotal   = kCv2CdcL * kCv2CdcLevelCells;   // == SatelliteSim::kCv2CdcCells
const int   kCv2CdcListCap = 65536;                          // == SatelliteSim::kCv2CdcListCap

layout(std430, set = 0, binding = CV2_CDC_BINDING)
#ifndef CV2_CDC_WRITE
readonly
#endif
buffer CV2CdcBuf {
    uint cv2CdcListCount;        // this frame's stale cells (zeroed by the host before the check pass)
    uint cv2CdcPad[3];
    uint cv2CdcList[kCv2CdcListCap];   // the cells the bake pass bakes this frame
    // Then three arrays of kCv2CdcTotal (runtime-sized, so a small placeholder can be bound while the clipmap is off):
    // [cell] low 16 bits half(max coarse sigma), high 16 the stamp; [kCv2CdcTotal + cell] / [2 kCv2CdcTotal + cell] S2's
    // fixed-point sums (x kCv2CdcMeanQ) of the light march's effective extinction and its square over the bake's points
    // (zeroed when the cell is listed; valid with the cell's stamp).
    uint cv2CdcData[];
};
const float kCv2CdcMeanQ = 1.0e6;

float cv2CdcCell(int lv) { return cv2.cdcA.z * float(1 << lv); }
// The staggered time quantum of an absolute cell on level lv (cdcP0 / cdcP1: sim time / the level's period, mod 4096).
uint cv2CdcQuantum(ivec3 c, int lv)
{
    uint  h  = uint(c.x) * 0x9E3779B1u ^ uint(c.y) * 0x85EBCA77u ^ uint(c.z) * 0xC2B2AE3Du;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    float ph = (lv < 4 ? cv2.cdcP0[lv] : cv2.cdcP1[lv - 4]) + float(h & 0xFFFFu) / 65536.0;
    return uint(floor(ph)) & 15u;
}
uint cv2CdcStamp(ivec3 c, int lv)
{
    return 0x8000u | (uint((c.x + kCv2CdcOff) >> 7) & 15u) | ((uint((c.y + kCv2CdcOff) >> 7) & 15u) << 4u)
         | ((uint(cv2.cdcB.x) & 7u) << 8u) | (cv2CdcQuantum(c, lv) << 11u);
}
int cv2CdcIndex(ivec3 c, int lv)
{
    return lv * kCv2CdcLevelCells + ((c.z * kCv2CdcN + (c.y & (kCv2CdcN - 1))) * kCv2CdcN) + (c.x & (kCv2CdcN - 1));
}

#ifndef CV2_CDC_WRITE
// The level a point would be read from (kCv2CdcL = outside), for the diagnostics.
int cv2CdcLevel(vec3 rel, float eyeR)
{
    vec3  d  = vec3(rel.xy, rel.z + eyeR);
    float dA = dot(d, cv2.layA.xyz);
    vec2  uv = vec2(dot(d, cv2.layE.xyz), dot(d, cv2.layN.xyz)) * (kCv2CdcR / max(dA, 1.0));
    vec2  du = abs(uv - cv2.cdcA.xy);
    return min(int(ceil(log2(max(max(du.x, du.y) / (cv2.cdcA.z * float(kCv2CdcN / 2 - 2)), 1.0)))), kCv2CdcL);
}
// 1 = the coarse field is zero at this point (known), 0 = it may not be (or the cell is unknown / outside the clipmap).
// rel: offset from the eye (observer ENU); eyeR: the eye's distance from the Earth's centre.
// The expected transmittance over a light step of length L at a point, as an effective extinction -ln(E[exp(-L e)]) / L,
// when the cell is valid (S2). The cell's mean and variance of the light extinction e give the gamma model
// E[exp(-L e)] = (1 + L var / mean)^(-mean^2 / var) (the layer shadow map's): exp(-L mean) of a cell holding cloud and
// gaps is darker than the mean transmittance the point samples converge to (Jensen; s2 runs: -1..-18 levels).
bool cv2CdcMeanAt(vec3 rel, float eyeR, float L, out float m)
{
    m = 0.0;
    vec3  d  = vec3(rel.xy, rel.z + eyeR);
    float dA = dot(d, cv2.layA.xyz);
    if (dA <= 0.0) return false;
    float h  = length(d) - kCv2CdcR;
    int   iz = int(floor(h / kCv2CdcDZ));
    if (h < 0.0 || iz >= kCv2CdcNZ) return false;
    vec2  uv = vec2(dot(d, cv2.layE.xyz), dot(d, cv2.layN.xyz)) * (kCv2CdcR / dA);
    vec2  du = abs(uv - cv2.cdcA.xy);
    int   lv = int(ceil(log2(max(max(du.x, du.y) / (cv2.cdcA.z * float(kCv2CdcN / 2 - 2)), 1.0))));
    if (lv >= min(kCv2CdcL, int(cv2.cdcB.w))) return false;
    if (lv >= int(cv2.cdcC.y)) return false;   // only the finest levels (cdc_light_levels): see the bake
    ivec3 c  = ivec3(ivec2(floor(uv / cv2CdcCell(lv))), iz);
    int   ci = cv2CdcIndex(c, lv);
    if ((cv2CdcData[ci] >> 16u) != cv2CdcStamp(c, lv)) return false;
    float n  = cv2.cdcA.w > 1.5 ? 36.0 : 9.0;
    float mu = float(cv2CdcData[kCv2CdcTotal + ci]) / (kCv2CdcMeanQ * n);
    float m2 = float(cv2CdcData[2 * kCv2CdcTotal + ci]) / (kCv2CdcMeanQ * n);
    float va = max(m2 - mu * mu, 0.0);
    if (mu <= 1e-9) return true;
    m = (va <= 1e-6 * mu * mu) ? mu : (mu * mu / va) * log(1.0 + L * va / mu) / L;
    return true;
}
bool cv2CdcEmpty(vec3 rel, float eyeR)
{
    vec3  d  = vec3(rel.xy, rel.z + eyeR);
    float dA = dot(d, cv2.layA.xyz);
    if (dA <= 0.0) return false;
    float h  = length(d) - kCv2CdcR;
    int   iz = int(floor(h / kCv2CdcDZ));
    if (h < 0.0 || iz >= kCv2CdcNZ) return false;
    vec2  uv = vec2(dot(d, cv2.layE.xyz), dot(d, cv2.layN.xyz)) * (kCv2CdcR / dA);   // m on the plane
    vec2  du = abs(uv - cv2.cdcA.xy);
    float reach = cv2.cdcA.z * float(kCv2CdcN / 2 - 2);
    int   lv = int(ceil(log2(max(max(du.x, du.y) / reach, 1.0))));
    if (lv >= min(kCv2CdcL, int(cv2.cdcB.w))) return false;
    ivec3 c  = ivec3(ivec2(floor(uv / cv2CdcCell(lv))), iz);
    uint  v  = cv2CdcData[cv2CdcIndex(c, lv)];
    return (v >> 16u) == cv2CdcStamp(c, lv) && (v & 0xFFFFu) == 0u;
}
#endif
#endif
