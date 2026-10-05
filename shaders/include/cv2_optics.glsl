#ifndef SATLIGHTSIM_CV2_OPTICS_GLSL
#define SATLIGHTSIM_CV2_OPTICS_GLSL
// Atmospheric optics shared by the volumetric clouds (clouds_v2.glsl includes this) and the rain
// particles at the eye (rain_particles.vert): ONE copy of the bows, halos and phase lobes, so a drop
// drawn as a particle scatters exactly as the rain volume it stands in for. Pure functions; needs
// common.glsl (PI). Moved out of clouds_v2.glsl unchanged (2026-10-04).
const vec3 CV2_N_ICE   = vec3(1.3069, 1.3108, 1.3165);
const vec3 CV2_N_WATER = vec3(1.3314, 1.3350, 1.3403);

float cv2Sq(float x) { return x * x; }

// Ice crystals: the 22 and 46 degree halos (randomly oriented hexagonal prisms: minimum deviation of
// the 60 and 90 degree prisms), and from plates falling flat the sundogs (at the light's elevation,
// through the effective index n' = sqrt(n^2 - sin^2 e) / cos e — 22 deg out at the horizon, 36 at
// 40 deg, none above ~61), the parhelic circle and the circumzenithal arc (only below 32 deg).
// hab: cv2IceHabit's weights for this region.
vec3 cv2IceOptics(vec3 v, vec3 s, vec4 hab)
{
    float a = acos(clamp(dot(v, s), -1.0, 1.0));
    vec3  D22 = 2.0 * asin(CV2_N_ICE * 0.5) - radians(60.0);
    vec3  D46 = 2.0 * asin(CV2_N_ICE * 0.7071068) - radians(90.0);
    vec3  av  = vec3(a);
    vec3  h22 = smoothstep(D22 - radians(0.25), D22 + radians(0.15), av) * exp(-max(av - D22, 0.0) / radians(1.8));
    vec3  h46 = smoothstep(D46 - radians(0.4), D46 + radians(0.3), av) * exp(-max(av - D46, 0.0) / radians(3.0));

    float e   = asin(clamp(s.z, -1.0, 1.0));
    float ev  = asin(clamp(v.z, -1.0, 1.0));
    float daz = abs(atan(v.y, v.x) - atan(s.y, s.x));
    daz = min(daz, 2.0 * PI - daz);
    float ce = cos(e), se = sin(e);
    vec3  np  = sqrt(CV2_N_ICE * CV2_N_ICE - se * se) / max(ce, 1e-3);
    vec3  arg = np * 0.5;
    vec3  Dp  = 2.0 * asin(min(arg, vec3(1.0))) - radians(60.0);
    vec3  dz  = vec3(daz);
    vec3  dogs = step(arg, vec3(0.999)) * smoothstep(Dp - radians(0.25), Dp + radians(0.15), dz)
               * exp(-max(dz - Dp, 0.0) / radians(3.0)) * exp(-cv2Sq((ev - e) / radians(1.3)));
    float circle = exp(-cv2Sq((ev - e) / radians(0.35))) * smoothstep(radians(8.0), radians(25.0), daz) * 0.6;
    vec3  cz2 = CV2_N_ICE * CV2_N_ICE - ce * ce;
    vec3  hZ  = radians(90.0) - acos(sqrt(clamp(cz2, 0.0, 1.0)));
    vec3  cza = step(cz2, vec3(1.0)) * exp(-(vec3(ev) - hZ) * (vec3(ev) - hZ) / cv2Sq(radians(0.6)))
              * (1.0 - smoothstep(radians(40.0), radians(80.0), daz)) * smoothstep(-0.02, 0.1, se);
    float up = smoothstep(-0.03, 0.02, se);          // plates need the light above the horizon
    return 4.0 * hab.x * h22 + 0.7 * hab.w * h46 + up * (12.0 * hab.y * dogs + hab.z * (vec3(circle) + 3.0 * cza));
}

// The sun pillar: plate crystals near the ground, drifting almost level, make a vertical streak above
// (and below) a low Sun, diamond dust's signature. v, s in the observer's ENU.
float cv2PillarOptics(vec3 v, vec3 s)
{
    float e   = asin(clamp(s.z, -1.0, 1.0));
    float ev  = asin(clamp(v.z, -1.0, 1.0));
    float daz = abs(atan(v.y, v.x) - atan(s.y, s.x));
    daz = min(daz, 2.0 * PI - daz) * cos(ev);
    float wide = exp(-cv2Sq(daz / radians(0.7)));
    float vert = (ev > e) ? exp(-(ev - e) / radians(6.0)) : exp(-(e - ev) / radians(3.0));
    return 6.0 * wide * vert * (1.0 - smoothstep(0.08, 0.4, s.z)) * smoothstep(-0.06, 0.0, s.z);
}

// Rain drops: the primary bow (one internal reflection, 42.3 deg red .. 41.0 blue from the antisolar
// point, red outside), the secondary (two reflections, 50.5 .. 52.8, colours reversed), the bright
// sky inside the primary and Alexander's dark band between them.
vec3 cv2RainOptics(vec3 v, vec3 s)
{
    float a  = acos(clamp(dot(v, -s), -1.0, 1.0));
    vec3  av = vec3(a);
    vec3  i1 = acos(sqrt((CV2_N_WATER * CV2_N_WATER - 1.0) / 3.0));
    vec3  R1 = 4.0 * asin(sin(i1) / CV2_N_WATER) - 2.0 * i1;
    vec3  i2 = acos(sqrt((CV2_N_WATER * CV2_N_WATER - 1.0) / 8.0));
    vec3  R2 = 2.0 * i2 - 6.0 * asin(sin(i2) / CV2_N_WATER) + PI;
    vec3  d1 = (av - R1) / radians(0.7);
    vec3  prim = exp(-d1 * d1)
               + 0.22 * (1.0 - smoothstep(R1 - radians(0.5), R1 + radians(0.3), av)) * exp(min(av - R1, 0.0) / radians(12.0));
    vec3  d2 = (av - R2) / radians(0.9);
    vec3  sec  = 0.4 * exp(-d2 * d2)
               + 0.08 * smoothstep(R2 - radians(0.3), R2 + radians(0.5), av) * exp(-max(av - R2, 0.0) / radians(15.0));
    return prim + sec;
}

// Henyey-Greenstein normalised so the isotropic value is 1 (this codebase's phase convention —
// phaseR/phaseM/phaseCloud in common.glsl average to 1, so radiance = SUN_INTENSITY x phase).
float cv2HG(float cosA, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cosA, 1e-4), 1.5);
}

#endif // SATLIGHTSIM_CV2_OPTICS_GLSL
