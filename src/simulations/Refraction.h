#pragma once
// Atmospheric refraction, mirages and optical turbulence along a ray from the observer (2026-10-26, issue
// 20261026_203414 "Sun refraction mk 2"). App-independent: a refractivity profile (a standard atmosphere, the
// surface layer over a warmer / cooler sea or ground, an optional elevated inversion) and a ray tracer that maps
// each APPARENT elevation to the true direction its light came from, per colour channel, with the air columns
// along the bent path and the turbulence integrals (angle-of-arrival jitter, scintillation). One table per frame
// (rebuilt a few times a second on a worker) serves the Sun's disc, the stars' positions and their twinkle.
//
// Physics (Young's "Sunset science" papers; Lehn 1985 for the inferior mirage; Andrews & Phillips for turbulence):
//  - refractivity N = 77.6 P/T (hPa, K) x (1 + 0.00752 / lambda^2 um) / 1.0249 at 550 nm (Edlen-like dispersion);
//  - dN/dz = N (dlnP/dz - dlnT/dz); a ray's curvature is -dn/dz cos e. Rays bend down in standard air (-30..-40
//    N/km), are ducted past -157 N/km (the Earth's curvature: an elevated inversion), and bend UP where the air is
//    warmed from below (dT/dz < -34 mK/m over a warm sea: the inferior mirage — Etruscan vase, Omega Sun);
//  - turbulence: Cn^2(z) = a surface term ~ (dT)^2 z^(-4/3) + Hufnagel-Valley's free troposphere; the angle of arrival
//    variance 2.91 D^(-1/3) int Cn^2 ds (an aperture D), the Rytov scintillation 2.25 k^(7/6) int Cn^2 s^(5/6) ds.
#include <vector>

namespace refr
{
constexpr int kTableN = 512;          // apparent elevations in a table (GLSL REFR_N in sat_sky.frag)
constexpr float kHit = 10.0f;         // a refraction value this large = the ray ends on the surface (sea / ground)

// (n - 1) at the R / G / B channels' effective wavelengths (610 / 545 / 465 nm) over its value at 550 nm.
extern const double kDisp[3];

struct Profile
{
    double surfaceZ = 0.0;   // the surface the rays end on and the surface layer sits on (m above sea level)
    double T0 = 288.15;      // the air temperature zRef above the surface (K)
    double P0 = 1013.25;     // sea-level pressure (hPa)
    double dTs = 0.0;        // surface minus air temperature (K): > 0 a warm sea / hot ground (inferior mirage)
    double zRef = 10.0;      // the surface layer's reference height (m)
    double z0 = 2e-4;        // its roughness length for heat (m)
    double invZ = 0.0;       // an elevated inversion's middle above the surface (m); invDT 0 = none
    double invDz = 20.0;     // its thickness (m)
    double invDT = 0.0;      // the warming across it (K)
    double cnSurf = 1e-14;   // Cn^2 of the surface layer 1 m up (m^-2/3)
    double cnFree = 1.0;     // x Hufnagel-Valley's free troposphere
    double dispScale = 1.0;  // x the physical dispersion (1 = physical)
};

struct Entry
{
    float dR[3];    // refraction per channel (rad): apparent minus true elevation; kHit = ends on the surface
    float aoa;      // rms angle-of-arrival jitter (rad)
    float colR;     // Rayleigh column along the bent path (m at sea-level density, scale height 7994 m)
    float colM;     // Mie column (scale height 1200 m)
    float colO3;    // ozone: the path's length in the 15-35 km layer / 20 km
    float colH;     // the haze-near-clouds column (scale height 2500 m)
    float scint;    // scintillation index sigma_I of a point source along this ray (0..1)
};

struct Table
{
    bool valid = false;
    double h0 = 0.0;                     // the observer's height it was built for (m above sea level)
    double eC = 0.0, c = 8e-4, alpha = 1.0, u0 = 0.5;   // e(u) = eC + c sinh(alpha (u - u0)), u = i / (N - 1)
    double topScale = 1.0;               // above the table: refraction = topScale x Saemundsson(e)
    double zenAoa2 = 0.0, zenSr2 = 0.0;  // the zenith ray's jitter variance (rad^2) and Rytov variance: above the
                                         // table they grow as 1 / sin(e) and sin(e)^(-11/6)
    std::vector<Entry> e;
    double eAt(int i) const;
    double uOf(double eApp) const;       // inverse of eAt, in [0, 1] (unclamped)
};

// Builds the table for an observer h0 m above sea level. Spreads the rays over `threads` threads.
void build(const Profile &p, double h0, Table &out, int threads);

// Bennett/Saemundsson refraction (rad) for a ground observer at apparent elevation e (rad), e >= ~-1 deg.
double saemundsson(double eApp);

// Looks the table up at an apparent elevation: refraction (green) and whether the ray ends on the surface.
bool lookup(const Table &t, double eApp, double &dRg, Entry *outInterp = nullptr);
} // namespace refr
