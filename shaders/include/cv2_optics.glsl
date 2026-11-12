#ifndef SATLIGHTSIM_CV2_OPTICS_GLSL
#define SATLIGHTSIM_CV2_OPTICS_GLSL
// Atmospheric optics shared by the volumetric clouds (clouds_v2.glsl includes this) and the rain
// particles at the eye (rain_particles.vert): ONE copy of the bows, halos and phase lobes, so a drop
// drawn as a particle scatters exactly as the rain volume it stands in for. Pure functions; needs
// common.glsl (PI) and the CloudV2Params UBO (cv2.optics). Moved out of clouds_v2.glsl unchanged (2026-10-04).
//
// ICE OPTICS ARE PREDICTED, NOT PLACED (2026-11-10, issue_20261110_180734 "Optics physics"): cv2IceOptics reads a
// table ray-traced through hexagonal ice crystals (tools/make_ice_optics.py -> assets/textures/ice_optics.rgba8:
// Monte-Carlo geometric optics, refraction + Fresnel + total internal reflection, per colour channel) for five
// crystal POPULATIONS: randomly oriented columns (the 22 deg halo), compact random crystals (22 + 46 deg), plates
// falling flat (sundogs, parhelic circle, circumzenithal / circumhorizon arcs, subsun, subparhelia), singly oriented
// columns (tangent arcs -> circumscribed halo, supra / infralateral arcs) and Parry-oriented columns (Parry arcs).
// Every arc is where the crystals send the rays, for the light's elevation above the crystals' LOCAL horizontal
// (`up`: from orbit the first ice sample's own vertical, not the eye's), including the mirror pattern below the
// horizon that an eye ABOVE the layer sees (subsun, subparhelia, the halos' lower halves). The analytic lobes
// (cv2IceOpticsAnalytic) remain as the A/B and the fallback without the table (cv2.optics.x = 0).
const vec3 CV2_N_ICE   = vec3(1.3069, 1.3108, 1.3165);
const vec3 CV2_N_WATER = vec3(1.3314, 1.3350, 1.3403);

float cv2Sq(float x) { return x * x; }

// The table's layout (tools/make_ice_optics.py: change both). x theta from the light: 0..64 deg at 0.5 (128 texels)
// then 64..180 at 1 (116); y rows 0-1 random columns, 2-3 random compact, then 91 rows (phi 0..180 at 2 deg: the azimuth
// about the light from its zenith side) per oriented population: plates, columns, Parry; z the light's elevation:
// 0..20 at 1 deg (21 layers; 2.5 until 2026-11-11: the subsun / subparhelia, which move 2 deg per degree of elevation,
// blended from two layers 5 deg apart into one streak ~40% too long, issue_20261111_164656), 22.5..60 at 2.5 (16),
// 65..90 at 5 (6). Texel = (P / 512)^(1/3): P the phase in cv2HG's convention
// (isotropic = 1), the undeviated forward rays left out (the march's delta-scaled extinction carries them).
const float CV2_IO_NT = 244.0, CV2_IO_H = 277.0, CV2_IO_NE = 43.0, CV2_IO_NP = 91.0, CV2_IO_PMAX = 512.0;
// The table's 22 deg halo peaks at ~16 x isotropic (its physical single-scatter contrast); the look the user approved
// drew it at 4 on the ice lobe, so the table comes in at a quarter: the relative strengths (sundogs ~30 x the halo,
// the 46 deg halo ~1/8 of it) are the trace's, the overall level the previous one. "Optics (halos, rainbows)" scales it.
const float CV2_IO_GAIN = 0.25;

#ifdef CV2_ICE_OPTICS_BINDING
layout(set = 0, binding = CV2_ICE_OPTICS_BINDING) uniform sampler3D cv2IceOpticsTex;
#endif

float cv2IoU(float thDeg) { return (thDeg < 64.0 ? thDeg * 2.0 : 128.0 + (thDeg - 64.0)) / CV2_IO_NT; }
float cv2IoW(float eDeg)  { return ((eDeg <= 20.0 ? eDeg : eDeg <= 60.0 ? 20.0 + (eDeg - 20.0) * 0.4 : 36.0 + (eDeg - 60.0) * 0.2) + 0.5) / CV2_IO_NE; }
vec3  cv2IoDecode(vec3 v) { return CV2_IO_PMAX * v * v * v; }

// Ice crystals, the analytic stand-ins (until 2026-11-10 the only version): the 22 and 46 degree halos (randomly
// oriented hexagonal prisms: minimum deviation of the 60 and 90 degree prisms), and from plates falling flat the
// sundogs (at the light's elevation, through the effective index n' = sqrt(n^2 - sin^2 e) / cos e — 22 deg out at
// the horizon, 36 at 40 deg, none above ~61), the parhelic circle and the circumzenithal arc (only below 32 deg).
// v, s in a frame whose z is up. hab: cv2IceHabit's weights for this region.
vec3 cv2IceOpticsAnalytic(vec3 v, vec3 s, vec4 hab)
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

// Ice crystals, predicted: the traced table at (theta from the light, phi about it, the light's elevation over the
// crystals' local horizontal `up`), weighted by the region's habit. hab: x random columns (the 22 deg halo; with
// "Ice optics columns" cv2.optics.z of them singly oriented: tangent arcs), y plates (sundogs, parhelic circle,
// CZA / CHA, subsun), z well-aligned crystals (with "Ice optics Parry" cv2.optics.y: Parry arcs), w compact random
// crystals (the 46 deg halo). v the view ray, s toward the light, up the local vertical, all unit, one frame.
vec3 cv2IceOptics(vec3 v, vec3 s, vec3 up, vec4 hab)
{
#ifdef CV2_ICE_OPTICS_BINDING
    if (cv2.optics.x > 0.5) {
        float sz  = dot(s, up);
        float e   = degrees(asin(clamp(sz, -1.0, 1.0)));
        float th  = degrees(acos(clamp(dot(v, s), -1.0, 1.0)));
        vec3  u   = up - s * sz;                              // the zenith side of the light, on the sky
        float ul  = length(u);
        u = ul > 1e-4 ? u / ul : normalize(cross(s, abs(s.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0)));
        vec3  r   = cross(s, u);
        float phi = degrees(atan(abs(dot(v, r)), dot(v, u)));   // 0 above the light .. 180 below it
        if (e < 0.0) { e = -e; phi = 180.0 - phi; }           // lit from below the horizontal: the mirror pattern
        float x = cv2IoU(th), z = cv2IoW(e);
        float py = phi * (0.5 / CV2_IO_H);                     // 2 deg per row
        vec3 res = vec3(0.0);
        if (hab.x > 0.0) res += hab.x * cv2IoDecode(textureLod(cv2IceOpticsTex, vec3(x, 1.0 / CV2_IO_H, z), 0.0).rgb);
        if (hab.w > 0.0) res += hab.w * cv2IoDecode(textureLod(cv2IceOpticsTex, vec3(x, 3.0 / CV2_IO_H, z), 0.0).rgb);
        if (hab.y > 0.0) res += hab.y * cv2IoDecode(textureLod(cv2IceOpticsTex, vec3(x, 4.5 / CV2_IO_H + py, z), 0.0).rgb);
        float col = hab.x * cv2.optics.z, par = hab.z * cv2.optics.y;
        if (col > 0.0) res += col * cv2IoDecode(textureLod(cv2IceOpticsTex, vec3(x, 95.5 / CV2_IO_H + py, z), 0.0).rgb);
        if (par > 0.0) res += par * cv2IoDecode(textureLod(cv2IceOpticsTex, vec3(x, 186.5 / CV2_IO_H + py, z), 0.0).rgb);
        return CV2_IO_GAIN * res;
    }
#endif
    // The analytic lobes assume z up: express v and s over `up`.
    vec3 e1 = normalize(cross(up, abs(up.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0)));
    vec3 e2 = cross(up, e1);
    return cv2IceOpticsAnalytic(vec3(dot(v, e1), dot(v, e2), dot(v, up)), vec3(dot(s, e1), dot(s, e2), dot(s, up)), hab);
}

// The same for TWO habits at one geometry (2026-11-11, regdiet): the beams light thin ice with the region's habit and snow
// shafts with a fixed one (plates and columns), at the same view ray, light and vertical; as two calls inside the march's
// light loop each computed the frame and fetched its rows, the snow's two rows being rows of the ice's. One frame, each row
// fetched once and weighted twice; a row is fetched when either habit wants it (a zero weight adds exactly 0). Bit-identical
// to the two calls' results, in their summation order.
void cv2IceOptics2(vec3 v, vec3 s, vec3 up, vec4 habA, vec4 habB, out vec3 resA, out vec3 resB)
{
    resA = vec3(0.0); resB = vec3(0.0);
#ifdef CV2_ICE_OPTICS_BINDING
    if (cv2.optics.x > 0.5) {
        float sz  = dot(s, up);
        float e   = degrees(asin(clamp(sz, -1.0, 1.0)));
        float th  = degrees(acos(clamp(dot(v, s), -1.0, 1.0)));
        vec3  u   = up - s * sz;
        float ul  = length(u);
        u = ul > 1e-4 ? u / ul : normalize(cross(s, abs(s.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0)));
        vec3  r   = cross(s, u);
        float phi = degrees(atan(abs(dot(v, r)), dot(v, u)));
        if (e < 0.0) { e = -e; phi = 180.0 - phi; }
        float x = cv2IoU(th), z = cv2IoW(e);
        float py = phi * (0.5 / CV2_IO_H);
        if (habA.x > 0.0 || habB.x > 0.0) {
            vec3 d = cv2IoDecode(textureLod(cv2IceOpticsTex, vec3(x, 1.0 / CV2_IO_H, z), 0.0).rgb);
            resA += habA.x * d; resB += habB.x * d;
        }
        if (habA.w > 0.0 || habB.w > 0.0) {
            vec3 d = cv2IoDecode(textureLod(cv2IceOpticsTex, vec3(x, 3.0 / CV2_IO_H, z), 0.0).rgb);
            resA += habA.w * d; resB += habB.w * d;
        }
        if (habA.y > 0.0 || habB.y > 0.0) {
            vec3 d = cv2IoDecode(textureLod(cv2IceOpticsTex, vec3(x, 4.5 / CV2_IO_H + py, z), 0.0).rgb);
            resA += habA.y * d; resB += habB.y * d;
        }
        float colA = habA.x * cv2.optics.z, parA = habA.z * cv2.optics.y;
        float colB = habB.x * cv2.optics.z, parB = habB.z * cv2.optics.y;
        if (colA > 0.0 || colB > 0.0) {
            vec3 d = cv2IoDecode(textureLod(cv2IceOpticsTex, vec3(x, 95.5 / CV2_IO_H + py, z), 0.0).rgb);
            resA += colA * d; resB += colB * d;
        }
        if (parA > 0.0 || parB > 0.0) {
            vec3 d = cv2IoDecode(textureLod(cv2IceOpticsTex, vec3(x, 186.5 / CV2_IO_H + py, z), 0.0).rgb);
            resA += parA * d; resB += parB * d;
        }
        resA *= CV2_IO_GAIN; resB *= CV2_IO_GAIN;
        return;
    }
#endif
    vec3 e1 = normalize(cross(up, abs(up.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0)));
    vec3 e2 = cross(up, e1);
    vec3 vv = vec3(dot(v, e1), dot(v, e2), dot(v, up)), ss = vec3(dot(s, e1), dot(s, e2), dot(s, up));
    resA = cv2IceOpticsAnalytic(vv, ss, habA);
    resB = cv2IceOpticsAnalytic(vv, ss, habB);
}

// The sun pillar: plate crystals near the ground, drifting almost level, make a vertical streak above
// (and below) a low Sun, diamond dust's signature. v, s in the observer's ENU. (Wobbling plates, tilts of
// 5-15 deg: a population the table does not carry, the one feature still placed.)
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
