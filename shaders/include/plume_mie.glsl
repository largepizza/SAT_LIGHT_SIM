// The rocket exhaust's ice: Mie optics (2026-11-15). The includer declares `uniform sampler2D plumeMieTex` (the table
// tools/make_mie_lut.py --ice writes to assets/textures/plume_mie.rgba16f: 2048 angles x 24 rows) and includes
// common.glsl first.
//   rows 0-11: per effective radius r = 0.05 x 60^(i/11) um (0.05-3 um, a gamma distribution, effective variance 0.1),
//              rgb = 4 pi dC_sca/dOmega over the luminance-weighted mean C_sca, / 8: the phase AND the ice's spectral
//              scattering (sub-micron ice scatters blue: ~2.3x red's 0.55x at 0.05 um, neutral by ~0.5 um), its
//              forward lobe (g 0.1 at 0.05 um, ~0.85 from 0.3 um) and the colours by angle near the Sun (corona,
//              iridescence) for a size that varies across the cloud;
//   rows 12-23: each radius's sphere-mean rgb (every texel): the colour its multiply scattered light takes.
// Both plume renderers read it (plume_brick.comp, cloud_march.comp's dustCloudsCS), so a brick hands its exhaust back
// to the Gaussians with the same light.
#ifndef PLUME_MIE_GLSL
#define PLUME_MIE_GLSL

// The exhaust ice's effective radius (um) at height h (m): log-linear between
//   0-15 km 3 um     contrail ice in the troposphere (rocket contrails and their ice: a few um),
//   35 km   0.8 um   the dry stratosphere: the exhaust's water freezes into small particles that cannot grow,
//   60 km   0.25 um
//   100 km  0.1 um   the mesosphere: fast expansion nucleates many small particles (mesospheric ice from exhaust and
//                    noctilucent cloud particles are ~0.05-0.1 um),
//   150 km+ 0.06 um  vacuum: free expansion, the smallest.
float plumeReffUm(float h)
{
    float k = h * 0.001;
    float l = k < 15.0  ? log(3.0)
            : k < 35.0  ? mix(log(3.0), log(0.8), (k - 15.0) / 20.0)
            : k < 60.0  ? mix(log(0.8), log(0.25), (k - 35.0) / 25.0)
            : k < 100.0 ? mix(log(0.25), log(0.1), (k - 60.0) / 40.0)
            : k < 150.0 ? mix(log(0.1), log(0.06), (k - 100.0) / 50.0)
            :             log(0.06);
    return exp(l);
}

// the table's row (0..11, fractional) for radius r (um)
float plumeMieRow(float rUm) { return clamp(11.0 * log(rUm * 20.0) / log(60.0), 0.0, 11.0); }

// The phase per steradian (rgb: x the spectral scattering relative to the luminance's) at cos(scattering angle) cosT
vec3 plumeMiePhase(float cosT, float rUm)
{
    float th = acos(clamp(cosT, -1.0, 1.0)) * (1.0 / PI);
    float v  = (plumeMieRow(rUm) + 0.5) * (1.0 / 24.0);
    return textureLod(plumeMieTex, vec2(th, v), 0.0).rgb * (8.0 / (4.0 * PI));
}

// The sphere-mean spectral scattering (luminance ~1): the colour of its multiply scattered light
vec3 plumeMieMean(float rUm)
{
    return textureLod(plumeMieTex, vec2(0.5, (12.0 + plumeMieRow(rUm) + 0.5) * (1.0 / 24.0)), 0.0).rgb;
}

#endif
