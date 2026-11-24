#ifndef SATLIGHTSIM_BEAM_VOL_GLSL
#define SATLIGHTSIM_BEAM_VOL_GLSL
// ── The beam light volume (2026-11-24, cloud_v2_beamvol.comp) ────────────────────────────────────────────────────────
// The Reflect-Orbital beams' light around each busy site, baked once a frame so every consumer reads one texel instead of
// looping over the beams: per box cell, the irradiance of every beam whose disk covers it, each beam attenuated by the
// cloud along ITS OWN path toward its satellite (a ray map per beam, marched through the box's density bake), and the
// irradiance-weighted direction toward the satellites. A box per site (up to kBvSites), fixed in the site's frame
// (east / north / up at the target, origin at its ground point), stacked along z in one 3D texture.
// Needs beam_cloud_lights.glsl (the sites and beams).
const int   kBvNX = 128, kBvNY = 128, kBvNZ = 64;     // == SatelliteSim.h kBvNX / kBvNY / kBvNZ
const float kBvHalf = 40000.0;                        // the box's half width (m): +-40 km about the site
const float kBvZ0 = -2500.0, kBvZ1 = 15500.0;         // its height range above the site's ground (m)
const float kBvCellXY = 2.0 * kBvHalf / float(kBvNX); // 625 m
const float kBvCellZ = (kBvZ1 - kBvZ0) / float(kBvNZ); // 281.25 m
const float kBvScale = 1024.0;                        // the stored irradiance's scale (Suns x kBvScale: half-float range)
const int   kBvRays = 20;                             // a beam's ray map: kBvRays x kBvRays across its disk (1.15 R)
const float kBvDiskCut = 1.15;                        // == kBeamDiskCut

// The shafts' volume (kMode 3): kBvShX x kBvShY screen tiles x kBvShZ log-spaced depths from bvShT0() to bvShT1(), each
// texel the beams' light scattered toward the eye by the air between the eye and the texel's depth (integrated, the air's
// extinction included), rgb x kBvScale, before the shafts' gains. The depth range is the boxes' (the CPU: bvSites[0].md.w
// / [1].md.w): 50 m .. ~200 km past the farthest box near the sites, only their range from orbit (it was a fixed 50 m -
// 3000 km: 80-km slices at a site 460 km away, the beams' streak smeared into the deck behind it).
const int   kBvShX = 160, kBvShY = 90, kBvShZ = 64;   // == SatelliteSim.h
float bvShT0() { return max(bvSites[0].md.w, 50.0); }
float bvShT1() { return max(bvSites[1].md.w, 4.0 * bvShT0()); }
float bvShC()  { return log(bvShT1() / bvShT0()) / float(kBvShZ); }
float bvShaftU(float t) { return clamp(log(max(t, bvShT0()) / bvShT0()) / bvShC(), 0.0, float(kBvShZ)) / float(kBvShZ); }

// A box-frame point -> the site's texel coordinates in the stacked texture (normalised), z clamped inside its slab.
vec3 bvUVW(uint s, vec3 L)
{
    float zc = clamp((L.z - kBvZ0) / kBvCellZ, 0.5, float(kBvNZ) - 0.5) + float(s) * float(kBvNZ);
    return vec3(L.xy / (2.0 * kBvHalf) + 0.5, zc / float(kBvNZ * int(kBvSites)));
}

// The site whose box holds this observer-relative point (pR = world - eye), and the point in its frame; -1 = none.
int bvSiteAt(vec3 pR, out vec3 L)
{
    L = vec3(0.0);
    for (uint s = 0u; s < min(bvSiteCount, kBvSites); ++s) {
        vec3 v = pR - bvSites[s].o.xyz;
        vec3 q = vec3(dot(v, bvSites[s].ax.xyz), dot(v, bvSites[s].ay.xyz), dot(v, bvSites[s].az.xyz));
        if (abs(q.x) < kBvHalf - kBvCellXY && abs(q.y) < kBvHalf - kBvCellXY && q.z > kBvZ0 && q.z < kBvZ1) {
            L = q;
            return int(s);
        }
    }
    return -1;
}

// The beam disk's profile (== cloud_v2_march.comp beamDisk): a soft dome of mean 1 over the disk of radius R.
float bvDisk(float d2, float R)
{
    return (1.0 - smoothstep(0.25 * R, 1.15 * R, sqrt(d2))) * 1.885;
}

// The light volume's texel: x the irradiance with each beam attenuated only by cloud more than kBvNearM up ITS path (the
// clouds' read: the march's three reads up the beams take the near cloud), y the same with all the cloud (the air's: the
// shafts), zw the irradiance-weighted direction toward the satellites, box frame, upper-hemisphere octahedral (z > 0).
const float kBvNearM = 1950.0;   // == lightAmbOD's three beam reads (150 + 450 + 1350 m)
vec2 bvOctEnc(vec3 d) { d.z = max(d.z, 0.0); return d.xy / max(abs(d.x) + abs(d.y) + d.z, 1e-6); }
vec3 bvOctDec(vec2 e) { return normalize(vec3(e, max(1.0 - abs(e.x) - abs(e.y), 0.0) + 1e-4)); }

// A beam's ray-map basis: two unit vectors across the beam (box frame).
void bvBasis(vec3 d, out vec3 e1, out vec3 e2)
{
    e1 = normalize(abs(d.z) < 0.999 ? cross(d, vec3(0.0, 0.0, 1.0)) : cross(d, vec3(1.0, 0.0, 0.0)));
    e2 = cross(d, e1);
}
#endif
