// ── Atmospheric extinction along a line of sight (2026-09-23) ──────────────────────────────────
// The air a ray actually crosses, for an observer anywhere — sea level, a mountain, an aircraft,
// orbit, or a limb-grazing sightline — in one formula. Two exponential components: molecular
// (Rayleigh, ozone folded in; 8 km scale height) and aerosol haze (1.2 km), sharing the sea-level
// zenith extinction k in the fixed ratio ATM_F_RAY. Replaces the sea-level Kasten & Young airmass
// times exp(-tangentAlt / 80 km) — a 10x too slow thinning that left a 4 km mountain with 95% of
// sea-level extinction (here ~35%) and an aircraft at 10 km with 88% (here ~15%).
//
// Columns come from the Chapman function Ch(x, chi) ~ sqrt(pi x / 2) * erfcx(sqrt(x / 2) cos chi),
// x = r / H (asymptotic in x; x >= 800 here), with the tangent-point form for rays that dip before
// climbing out. CPU mirror: atmColumn() / atmExtinctionMag() in SatPhotometry.cpp — keep in step.
// Positions are Earth-centred metres, directions unit vectors, in any one frame.
#ifndef ATMOSPHERE_GLSL
#define ATMOSPHERE_GLSL

const float ATM_R     = 6371000.0;
const float ATM_H_RAY = 8000.0;   // molecular scale height, m
const float ATM_H_AER = 1200.0;   // aerosol scale height, m
const float ATM_F_RAY = 0.6;      // molecular share of the sea-level zenith extinction

// exp(y^2) erfc(y) for y >= 0: 1 / (sqrt(pi) ((1-a) y + a sqrt(y^2 + b))), a = 0.3436,
// b = 1/(pi a^2) (exact at 0 and infinity) — max relative error 0.33%.
float atmErfcx(float y)
{
    return 1.0 / (1.7724539 * (0.6564 * y + 0.3436 * sqrt(y * y + 2.6961)));
}

// Column from radius r to infinity along a ray at zenith angle chi, in sea-level vertical columns.
float atmColumnInf(float r, float cosChi, float H)
{
    float x = r / H;
    float up = exp(-(r - ATM_R) / H) * sqrt(1.5707963 * x) * atmErfcx(sqrt(0.5 * x) * abs(cosChi));
    if (cosChi >= 0.0)
        return up;
    // Descending: the whole line through the tangent point minus the part behind the start.
    float rt = max(r * sqrt(max(0.0, 1.0 - cosChi * cosChi)), ATM_R); // tangent radius, >= ground
    return 2.0 * exp(-(rt - ATM_R) / H) * sqrt(1.5707963 * rt / H) - up;
}

// Column along the segment p -> p + L d (L <= 0: to infinity).
float atmColumn(vec3 p, vec3 d, float L, float H)
{
    float r = length(p);
    float cosP = dot(p, d) / r;
    if (L <= 0.0)
        return atmColumnInf(r, cosP, H);
    vec3 q = p + d * L;
    float rq = length(q);
    float cosQ = dot(q, d) / rq;
    float c = (cosP >= 0.0 || cosQ > 0.0)
        ? atmColumnInf(r, cosP, H) - atmColumnInf(rq, cosQ, H)     // climbing, or through the tangent
        : atmColumnInf(rq, -cosQ, H) - atmColumnInf(r, -cosP, H);  // descending: evaluate it backwards
    return max(c, 0.0);
}

// Extinction in magnitudes; k = sea-level zenith extinction (the "Extinction" slider).
float atmExtinctionMag(vec3 p, vec3 d, float L, float k)
{
    return k * (ATM_F_RAY * atmColumn(p, d, L, ATM_H_RAY) + (1.0 - ATM_F_RAY) * atmColumn(p, d, L, ATM_H_AER));
}

#endif
