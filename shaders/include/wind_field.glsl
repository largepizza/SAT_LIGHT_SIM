// ── The one wind (Weather v2, 2026-11-06; .plans/WEATHER_V2_PLAN.md) ──────────────────────────────────────────────
// Every cloud motion reads this: the weather bake's moisture advection, the cloud noise's flow map (clouds_v2.glsl),
// the resolve's reprojection. Its CPU twin is wx::windAt (src/simulations/WindField.cpp): keep the two line for line,
// the storms' tracks and the eye's surface wind come from it (harness `wind probe` compares them).
//
// W(d, h) = the zonal-mean circulation (trades, mid-latitude westerlies, the subtropical jet, polar easterlies, the
// upper tropical easterlies, a Hadley overturning), following the Sun's declination and stronger in the winter
// hemisphere, + mid-latitude Rossby waves (a stream function, so divergence-free; drifting east, each growing and
// decaying over days) + mesoscale eddies (~1200 km, the curl of six waves). Every frequency in time is a whole multiple
// of 1 / kWfPeriod, so the clock can wrap there seamlessly and stays exact in float.
//
// ws: x the wind clock (s: sim time mod kWfPeriod, reduced in double on the CPU), y the Sun's declination (rad),
//     z the Rossby waves' gain, w the eddies' gain.
#ifndef WIND_FIELD_GLSL
#define WIND_FIELD_GLSL

const float kWfPeriod = 2592000.0;   // 30 days
const float kWfR      = 6371000.0;

float wfG(float x, float c, float s) { float u = (x - c) / s; return exp(-u * u); }

// d: unit ECEF direction (z = north pole), h: metres above sea level. The wind as an ECEF vector (m/s), tangential.
vec3 wfWind(vec3 d, float h, vec4 ws)
{
    const float D2R = 0.017453293;
    float lat   = asin(clamp(d.z, -1.0, 1.0));
    float lon   = atan(d.y, d.x);
    float cl    = max(length(d.xy), 0.02);
    vec3  east  = vec3(-d.y, d.x, 0.0) / cl;
    vec3  north = cross(d, east);
    float hemi  = lat >= 0.0 ? 1.0 : -1.0;
    float hk    = h * 1e-3;
    float tw    = ws.x * (6.2831853 / kWfPeriod);   // the clock as a phase (one turn per kWfPeriod)

    // ── zonal mean: degrees from the THERMAL equator (it follows the Sun at ~0.35 of its declination) ──
    float latS   = lat - 0.35 * ws.y;
    float a      = abs(latS) / D2R;
    float winter = clamp(-hemi * ws.y / 0.41, -1.0, 1.0);    // +1 midwinter in this hemisphere, -1 midsummer
    float lowK   = 1.0 - smoothstep(0.5, 7.0, hk);           // the surface regimes thin out with height
    float jetH   = smoothstep(0.0, 11.0, hk) * (1.0 - 0.45 * smoothstep(12.0, 20.0, hk));
    float u = -7.0 * wfG(a, 14.0, 10.0) * (0.25 + 0.75 * lowK)                          // trades
            +  8.0 * wfG(a, 47.0, 11.0)                                                   // westerlies
            -  3.5 * wfG(a, 72.0, 8.0) * lowK                                             // polar easterlies
            + (27.0 + 9.0 * winter) * wfG(a, 33.0 - 3.0 * winter, 10.0) * jetH            // subtropical jet
            -  7.0 * wfG(a, 0.0, 14.0) * smoothstep(8.0, 14.0, hk);                       // upper tropical easterlies
    // Hadley overturning: toward the thermal equator near the ground, away from it aloft.
    float v = -2.5 * (latS >= 0.0 ? 1.0 : -1.0) * wfG(a, 12.0, 9.0) * (1.0 - 2.0 * smoothstep(1.5, 11.0, hk));

    // ── Rossby waves: psi = A E(lat) sum a_k(t) cos(m_k lon + l_k lat - n_k tw + p_k); u = -dpsi/(R dlat),
    // v = dpsi/(R cos(lat) dlon). E: a band about 47 deg (44 in winter), ~0 at the equator and the poles. ──
    float c0  = 47.0 - 3.0 * winter;
    float la  = abs(lat) / D2R;
    float e0  = wfG(la, c0, 13.0);
    float dE  = e0 * (-2.0 * (la - c0) / (13.0 * 13.0)) * hemi / D2R;   // dE / dlat (per radian)
    float A   = ws.z * 6.0e6 * (0.45 + 0.75 * clamp(hk / 10.0, 0.0, 1.0)) * (1.0 + 0.3 * winter);
    const float wm[5] = float[5](4.0, 5.0, 6.0, 7.0, 9.0);
    const float wl[5] = float[5](3.0, -4.0, 6.0, -2.0, 5.0);
    const float wn[5] = float[5](3.0, 4.0, 4.0, 5.0, 7.0);    // eastward drift: turns per kWfPeriod
    const float wq[5] = float[5](5.0, 7.0, 3.0, 8.0, 6.0);    // growth and decay: cycles per kWfPeriod
    const float wp[5] = float[5](0.4, 2.3, 4.1, 1.2, 5.5);
    const float wa[5] = float[5](1.0, 0.8, 0.65, 0.5, 0.4);
    float S = 0.0, Sl = 0.0, Sm = 0.0;
    float hp = hemi > 0.0 ? 0.0 : 1.9;                          // the hemispheres' waves are unrelated
    for (int k = 0; k < 5; ++k) {
        float amp = wa[k] * (0.6 + 0.4 * cos(wq[k] * tw + wp[k] * 1.7 + hp));
        float th  = wm[k] * lon + wl[k] * lat - wn[k] * tw + wp[k] + hp * float(k + 1);
        float cs = cos(th), sn = sin(th);
        S  += amp * cs;
        Sl -= amp * wl[k] * sn;
        Sm -= amp * wm[k] * sn;
    }
    u += -(A / kWfR) * (dE * S + e0 * Sl);
    v +=  (A / (kWfR * cl)) * e0 * Sm;

    // ── mesoscale eddies: the curl of six waves (~1200 km) on the sphere, ~unit rms, each turning over ~1-2 days ──
    const vec3  kd[6] = vec3[6](vec3( 0.62,  0.48,  0.62), vec3(-0.71,  0.30,  0.64), vec3( 0.18, -0.93,  0.32),
                                vec3( 0.85,  0.21, -0.48), vec3(-0.34, -0.55, -0.76), vec3( 0.05,  0.77, -0.64));
    const float ks[6] = float[6](1.0, 1.13, 0.87, 1.31, 0.74, 1.21);
    const float kn[6] = float[6](17.0, 23.0, 19.0, 29.0, 13.0, 21.0);
    float k0 = 6.2831853 * kWfR / 1.2e6;
    vec3  g  = vec3(0.0);
    for (int i = 0; i < 6; ++i) {
        vec3 kv = kd[i] * (k0 * ks[i] / length(kd[i]));
        g += kv * (cos(dot(kv, d) + float(i) * 1.37 + kn[i] * tw) / ks[i]);
    }
    vec3 eddy = cross(d, g) * (ws.w * 3.5 / (k0 * 1.73));

    return east * u + north * v + eddy;
}

// The steering height of each cloud layer (m): the cloud TEXTURE of a layer moves with the wind there, whatever the
// sample's own height (a passive tracer in the shear would be smeared into sheets within minutes of a flow-map window).
const float kWfLowH  = 2000.0;
const float kWfMidH  = 5000.0;
const float kWfHighH = 10000.0;

#endif
