#ifndef SATLIGHTSIM_BEAM_VOL_WARP_GLSL
#define SATLIGHTSIM_BEAM_VOL_WARP_GLSL
// The beam light volume's horizontal cell spacing (2026-11-24, round 2; beam_vol.glsl, sat_sky.frag's ground lookup).
// A box's x and y are asinh-warped: u = 0.5 + 0.5 asinh(x / a) / asinh(H / a), so the 128 columns are ~0.58 km at the
// site, ~1.3 km 20 km out and ~10 km at the walls, +-200 km (H). The beams converge on the site, and a low one (5 deg)
// is 177 km out at 15.5 km: with uniform 625-m cells and +-40 km the box's walls cut the beams' light off, a visible
// rectangle in the air and on the cloud deck (issue_20261124_085028, _085151).
const float kBvHalf  = 200000.0;   // the box's half width (m)
const float kBvWarpA = 10000.0;    // the warp's linear core (m)
const float kBvWarpK = 3.6895038;  // asinh(kBvHalf / kBvWarpA)
const int   kBvNXY   = 128;        // == kBvNX == kBvNY

float bvWarpU(float x) { float q = x / kBvWarpA; return 0.5 + 0.5 * log(q + sqrt(q * q + 1.0)) / kBvWarpK; }
float bvUnwarp(float u) { float e = exp((2.0 * u - 1.0) * kBvWarpK); return kBvWarpA * 0.5 * (e - 1.0 / e); }
// The column index (continuous: cell i spans [i, i + 1)) of a box coordinate, and a column's centre.
vec2  bvColF(vec2 xy) { return vec2(bvWarpU(xy.x), bvWarpU(xy.y)) * float(kBvNXY); }
vec2  bvColCentre(vec2 ij) { return vec2(bvUnwarp((ij.x + 0.5) / float(kBvNXY)), bvUnwarp((ij.y + 0.5) / float(kBvNXY))); }
// The cell's width (m) at a box coordinate.
float bvCellW(float x) { return 2.0 * kBvWarpK * sqrt(kBvWarpA * kBvWarpA + x * x) / float(kBvNXY); }
#endif
