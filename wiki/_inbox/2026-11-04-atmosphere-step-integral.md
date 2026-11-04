---
pages: [rendering/atmosphere-and-sky.md]
code: [shaders/include/sky_atmos.glsl, shaders/sat_sky.frag, shaders/sky_ap.comp]
---
## Now
The single-scattering atmosphere loop (the per-pixel loop in the horizon band, with "Aerial perspective volume" 0, and
the aerial-perspective volume's own steps) integrates each step's sunlight-to-eye transmittance over the step instead of
taking it at one point (`atmStepAttn` in `shaders/include/sky_atmos.glsl`). The Sun's Rayleigh / Mie columns are still
taken once per step (at the middle of its lit part) and carried to any point of the step by height,
column x exp(-(h - hRef) / H); the eye's column to a point is the step start's plus the exact mean of exp(-h / H).
Rayleigh is averaged at two Gauss-Legendre nodes weighted by its density, Mie at the median of its own column along the
step. A step keeps the one-point form where the Sun's transmittance hardly changes over it (exponent spread < 0.5: day,
high air) or where the eye sees nothing of it, and always with the eye above the atmosphere. The stretch of a step in
front of the clouds (the "air in front", which the clouds are composited behind) is always integrated, and its multiple
scattering takes Psi at its own height. SKY_LITE (Planetarium) keeps the one-point form (SPIR-V budget).

## History
Until 2026-11-04 the loop used the step's middle for the whole step. With the default ~15-km steps (View samples min
6.5) the air sits at a step's low end, which toward a low Sun is the most extinguished, so the step was over-lit, most in
the Mie and the blue; the air in front of the clouds took the WHOLE first step's middle. At the issue_20261104_020521
sunset storm (822 m), loop everywhere: cloud mean RGB 154,100,84 against a 512-step reference 146,80,57 (a blue-grey
veil, 37% of pixels off by > 8 levels); now 146,80,59 (0.4%). The default frame (volume + band) is unchanged there
(mean 0.26 levels). A Python model of the loop put the one-point error at +10..+87% for a near piece, +14% for a whole
climbing step; the integrated form is within ~1-4%.
