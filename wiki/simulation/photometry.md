# Satellite photometry

How bright a satellite is above the atmosphere: geometry models baked into facet lobes, the microfacet
reflectance of each lobe, sunlight and earthshine, shadowing between a satellite's own parts, the older
two-surface model, and the conversion to apparent magnitude. What happens to that light on its way down
through the air is on [Seeing a satellite from the ground](visibility.md); how the model compares with
published measurements is on [Results and known residuals](../accuracy/results.md).

## The quantity computed

For a satellite at range \( r \) from the observer, the model computes its **radiant intensity per unit
solar irradiance** \( I \) (units of m² sr⁻¹): the power it sends per steradian toward the observer,
divided by the solar irradiance at the satellite. The flux at the observer relative to the Sun's is
\( I / r^2 \), so the above-atmosphere apparent V magnitude is

\[
m = m_\odot - 2.5 \log_{10}\frac{I}{r^2}, \qquad m_\odot = -26.74.
\]

\( I \) has two parts, sunlight and earthshine:

\[
I = f_\text{lit}\, I_\odot(\hat{\mathbf{s}}, \hat{\mathbf{o}}) + I_\oplus(\hat{\mathbf{o}}),
\]

with \( \hat{\mathbf{s}} \) the direction to the Sun, \( \hat{\mathbf{o}} \) the direction to the
observer, both from the satellite, and \( f_\text{lit} \in [0, 1] \) the Earth-shadow factor (see
[Earth's shadow](visibility.md#earths-shadow)). Earthshine is not shadowed by the Earth: a satellite in
the umbra above a sunlit horizon still sees lit ground.

## From a geometry model to facet lobes

A **geometry model** (`data/satellite_models/<id>.json`, format on
[Satellite models](../modding/satellite-models.md)) is a set of primitives (plane, box, cylinder, cone,
sphere) with physical materials, mounted on [attitude groups](attitude.md). At load it is tessellated
once (`tessellateSatModel()`) and **baked** into a short list of **facet lobes** (`bakeSatLobes()`). A
lobe is a set of facets on one group that share (approximately) one normal; the GPU evaluates the lobes
of every visible satellite each frame.

### Curvature spread

A curved primitive is tessellated into flat facets, and each facet carries an intrinsic normal spread
\( \sigma^2 \) (in GGX \( \alpha^2 \) units) that stands for the smooth surface it replaces:

| Facet | \( \sigma^2 \) |
|---|---|
| plane, box face, end cap | 0 |
| side of a cylinder or cone spanning \( \Delta\phi \) of azimuth | \( \Delta\phi^2 / 12 \) |
| sphere facet of solid angle \( \Omega \) | \( \Omega / 2\pi \) |

Without it a faceted cylinder would glint as many small flat mirrors rather than as one continuous band.
The spread is isotropic, so it slightly over-blurs a cylinder along its axis.

### Merging

1. **Exact merge.** Triangles of the same group and material whose normals agree to within about
   0.5 deg (\( \hat{\mathbf{n}}_a\cdot\hat{\mathbf{n}}_b > 0.99996 \)) become one lobe. Every flat face
   is exact.
2. **Budget merge.** While there are more lobes than the budget, the best-scoring pair within the same
   group is fused. The score is the cosine between the lobes' mean normals, minus 2.5 when their
   materials differ, so same-material neighbours merge first. Lobes of different groups never merge
   (they move independently).

A merged lobe stores area-weighted means of its materials' albedo, \( F_0 \), \( \alpha^2 \) and
transmission. Its normal is the direction of \( \sum_k A_k \hat{\mathbf{n}}_k \), and the spread of the
merged normals is kept through the **mean resultant length**

\[
\bar{R} = \frac{\left|\sum_k A_k \hat{\mathbf{n}}_k\right|}{\sum_k A_k} \in (0, 1],
\]

which sets two things: the diffuse area \( A_d = \bar{R} \sum A_k \) (a spread set of faces scatters
less in its mean direction than a flat one of the same area) and an extra roughness
\( \alpha^2 \mathrel{+}= 2(1 - \bar{R}) \). A lobe is GGX or Beckmann by the majority of its area.

The **lobe budget** is 48 for types flown by more than 10 000 satellites and 256 for smaller rosters
(stations, telescopes, depots), because GPU cost is (visible satellites of the type) × (lobes). Every
model is checked at load against a brute-force per-triangle evaluation over random Sun and observer
directions (`validateSatLobes()`); the log reports p95 and maximum \( |\Delta m| \) over configurations
within 5 mag of the model's median brightness. The shipped models are exact within their budget except
the ISS (514 exact lobes, p95 0.08 mag at 256).

## The reflectance of one lobe

For a lobe of area \( A \), diffuse area \( A_d \), Lambertian albedo \( \rho_d \), Fresnel reflectance
at normal incidence \( F_0 \), roughness \( \alpha \) and transmission \( T \), with
\( \mu_s = \hat{\mathbf{n}}\cdot\hat{\mathbf{s}} \), \( \mu_o = \hat{\mathbf{n}}\cdot\hat{\mathbf{o}} \) and
the half vector \( \hat{\mathbf{h}} = \widehat{\hat{\mathbf{s}}+\hat{\mathbf{o}}} \):

\[
I_\text{lobe} =
\begin{cases}
0 & \mu_o \le 0 \\[2pt]
\dfrac{T}{\pi} A_d\, (-\mu_s)\, \mu_o & \mu_s \le 0 \quad\text{(light through the face)} \\[8pt]
\dfrac{\rho_d}{\pi} A_d\, \mu_s \mu_o \;+\; A\, \dfrac{D(\hat{\mathbf{h}})\, F(\hat{\mathbf{s}}\cdot\hat{\mathbf{h}})\, G_1(\mu_s)\, G_1(\mu_o)}{4} & \mu_s > 0
\end{cases}
\]

This is the radiant intensity of a surface with the standard microfacet BRDF
\( f = \rho_d/\pi + DFG/(4\mu_s\mu_o) \), integrated over its area. Its pieces:

- **Fresnel** (Schlick): \( F(c) = F_0 + (1 - F_0)(1 - c)^5 \).
- **GGX** distribution and Smith masking, the default:

    \[
    D = \frac{\alpha^2}{\pi\left(\cos^2\theta_h\,\alpha^2 + \sin^2\theta_h\right)^2}, \qquad
    G_1(x) = \frac{2x}{x + \sqrt{\alpha^2 + (1 - \alpha^2)x^2}}.
    \]

    \( \sin^2\theta_h \) is computed as \( |\hat{\mathbf{n}}\times\hat{\mathbf{h}}|^2 \), not
    \( 1 - \cos^2\theta_h \), so sub-degree mirror lobes keep their precision in float.

- **Beckmann** distribution, chosen per material (`"distribution": "beckmann"`), with Walter et al.'s
  rational fit to its Smith \( G_1 \):

    \[
    D = \frac{\exp\!\left(-\tan^2\theta_h / \alpha^2\right)}{\pi \alpha^2 \cos^4\theta_h}, \qquad
    G_1 = \begin{cases} \dfrac{3.535c + 2.181c^2}{1 + 2.276c + 2.577c^2} & c < 1.6 \\ 1 & c \ge 1.6 \end{cases},
    \quad c = \frac{x}{\alpha\sqrt{1 - x^2}}.
    \]

    Beckmann's tails are Gaussian where GGX's are a power law; at the same \( \alpha \), GGX is about
    10 times brighter 30 deg off the peak. Beckmann is the right choice when \( \alpha \) comes from a
    Phong \( \cos^n \) fit (\( \alpha = \sqrt{2/(n+2)} \)), and for mirror films whose grazing-angle
    forward scatter GGX overstates.

- **Transmission** (`"transmission"`, e.g. arrays on a translucent Kapton blanket): light falling on the
  back of a face leaves the front diffusely, \( T/\pi\, A_d\, (-\mu_s)\,\mu_o \). The renderer tints it
  and lets it through only between solar cells; photometrically it is the area mean.
- **Open lattices** (`"coverage"` \( c < 1 \), trusses): each facet counts \( c \times \) its area, and a
  closed primitive adds its inner faces seen through the gaps at \( c(1 - c) \). A lattice never shadows
  anything.

### The Sun's disk: `SUN_ALPHA`

The Sun is not a point: its angular radius is about 0.27 deg. Rather than convolving every lobe with a
disk, the source size is folded into the roughness. For the Sun,

\[
\alpha^2_\text{eff} = \alpha^2_\text{lobe} + \alpha_\odot^2, \qquad \alpha_\odot = 0.0023 \approx \tan\!\left(\tfrac{\theta_\odot}{2}\right).
\]

This gives a perfectly flat mirror its physical peak. A GGX lobe at \( \alpha = \alpha_\odot \) peaks at
\( I = A F / (4\pi\alpha_\odot^2) = 1.50 \times 10^4\, AF \) m² sr⁻¹ at normal incidence; the exact
value for a flat mirror reflecting a uniform solar disk, \( AF/\Omega_\odot \), is
\( 1.47 \times 10^4\, AF \). The 2% excess is the price of the closed form. There is therefore no
separate "mirror boost" or cross-section fudge for geometry-model types.

## Earthshine

The Earth reflects sunlight onto satellites, and near the terminator, where twilight observations are
made, this light can dominate the faces turned away from the Sun. It is modelled as the sunlit part of
the visible cap of a **Lambertian sphere of albedo 0.3**, where every ground point carries its **own**
Sun cosine.

### The exact cap integral

Work in units of the Earth's radius, with the satellite at distance \( r \) on the +z axis and the Sun at
zenith angle \( Z \) as seen from the sub-satellite point (in the x-z plane). A ground point at angular
distance \( \lambda \) from the sub-satellite point and azimuth \( \phi \) has Sun cosine

\[
\mu_0 = a + b\cos\phi, \qquad a = \cos\lambda\cos Z, \quad b = \sin\lambda\sin Z,
\]

and is lit where \( \mu_0 > 0 \), i.e. for \( |\phi| < \phi_0 = \arccos(-a/b) \). Its distance \( d \)
and emission cosine \( \mu \) toward the satellite depend on \( \lambda \) only:

\[
d^2 = 1 + r^2 - 2r\cos\lambda, \qquad \mu = \frac{r\cos\lambda - 1}{d}.
\]

The integrals around each ring are closed-form:

\[
\int_{-\phi_0}^{\phi_0} \mu_0\, d\phi = 2(a\phi_0 + b\sin\phi_0), \qquad
\int_{-\phi_0}^{\phi_0} \mu_0 \cos\phi\, d\phi = 2a\sin\phi_0 + b(\phi_0 + \sin\phi_0\cos\phi_0),
\]

and the integral over the cap radius \( \lambda \in [0, \arccos(1/r)] \) is numerical (512 steps). The
result is the **vector irradiance** \( \mathbf{E}_\oplus \) at the satellite, as a fraction of the solar
irradiance: its magnitude \( E_\oplus \) and its direction, nadir tilted toward the Sun's side by
\( \psi \) (the lit crescent sits off to that side). `earthshineExact()` in
`src/simulations/SatPhotometry.cpp` computes it.

### The lookup table

`earthshineLut()` tabulates \( (\ln E_\oplus, \psi) \) on 64 cap half-angles \( \lambda_0 =
\arccos(R/r) \) from 10 deg (about 100 km altitude) to 85 deg (about 67 000 km) and 128 values of
\( \cos Z \) from \(-1\) to 1. Both `sat_orbit.comp` (`earthshineAt()`) and the CPU evaluator
(`earthshineLookup()`) read this one table with the same bilinear interpolation, so they agree. The
table against the exact integral: p95 0.004 mag, maximum 0.014 mag.

### A broad source: the spherical-harmonic fit

A single tilted direction is exact only for a face that sees the whole cap. A face edge-on to
\( \mathbf{E}_\oplus \) would get nothing, though in LEO the cap still gives it about 30% of a nadir
plate's irradiance. Diffuse and transmitted light therefore use the irradiance of the whole lit cap on
the face's plane, as an **order-4 spherical-harmonic fit** of the irradiance function \( E(\hat{\mathbf{n}}) \),
computed from the same ring integrals extended to \( \int \mu_0 \cos k\phi\, d\phi \), \( k \le 4 \). The
fit has 11 non-zero terms (it is symmetric about the Sun-nadir plane, and the cosine kernel has no
\( l = 3 \) part), in a local frame \( \hat{\mathbf{z}} \) = nadir, \( \hat{\mathbf{x}} \) = the Sun's
side:

\[
E(\hat{\mathbf{n}}) \approx \max\!\Big(\textstyle\sum_k c_k P_k(\hat{\mathbf{n}}),\ \ E_\oplus\,(\hat{\mathbf{n}}\cdot\hat{\mathbf{e}}_\oplus)_+\Big),
\]

\[
P_k \in \{1,\ z,\ x,\ 3z^2-1,\ xz,\ x^2-y^2,\ 35z^4-30z^2+3,\ xz(7z^2-3),\ (x^2-y^2)(7z^2-1),\ xz(x^2-3y^2),\ x^4-6x^2y^2+y^4\}.
\]

The floor at the vector value is exact where the plane sees the whole cap. The fit's error is at most
2.9% of \( E_\oplus \) in LEO (4% at GEO); an order-2 fit would be 6 to 8%, because a twilight crescent
on the limb is a sharp source. The coefficients are tabulated alongside the vector table, as ratios to
\( E_\oplus \) so they interpolate across its many decades.

The **specular** part keeps the single tilted source, widened by the Earth's angular size: with
\( \sin\rho = R/r \),

\[
\alpha_\oplus = \tan(\rho/2), \qquad \alpha^2_\text{eff} = \alpha^2_\text{lobe} + \alpha^2_\oplus.
\]

So a lobe's earthshine intensity is

\[
I_{\oplus,\text{lobe}} = \frac{\rho_d E(\hat{\mathbf{n}}) + T\,E(-\hat{\mathbf{n}})}{\pi} A_d\,\mu_o
\;+\; E_\oplus\, I_\text{spec}(\hat{\mathbf{e}}_\oplus, \hat{\mathbf{o}};\ \alpha^2_\text{lobe} + \alpha^2_\oplus).
\]

### Limits of the earthshine model

The Earth is uniform: no clouds, no ocean glint, no seasonal or land/sea albedo. Real local albedo ranges
from below 0.1 (open ocean) to near 0.8 (fresh snow, thick cloud tops), so a satellite over a bright
cloud deck receives more earthshine than modelled, and one over open ocean less. The clouds drawn by
the renderer do not feed back into it.

## Shadowing between parts

Parts that move relative to each other shade one another: a wing over the bus, a visor under antennas.
This is evaluated at run time against the model's own primitives, posed at the live joint angles
(`buildSatOcclusion()`, `satLobeVisibility()`).

- **Occluders.** One per component (up to 64 per model): planes, boxes and spheres exactly, a cylinder
  as a capped smooth cylinder, a cone as the cylinder of its larger radius (conservative). Open-lattice
  and render-only parts never occlude.
- **Samples.** Each lobe keeps area-weighted sample points on its own surface: 16 per component of the
  lobe (up to 64), chosen by weighted k-means over evenly spread sub-triangle centroids (more on triangles
  over 32 m²).
- **Rule.** A sample counts only if its rays toward the light **and** toward the observer both leave the
  satellite. Earthshine is too broad a source for one shadow ray, so it tests the observer ray only. The
  lobe's intensity is scaled by the weighted fraction of samples that pass.
- **Two exactness rules.** A primitive never occludes its own surface: every primitive is convex or flat,
  so this is exact, and it stops a faceted cylinder's samples (which sit just inside the smooth occluder)
  from shadowing themselves. A same-group occluder counts only if part of it lies in front of the lobe
  (either side, for a translucent lobe lit from behind); this per-lobe mask is fixed at bake time.

Against a ray-cast reference (25 points per triangle, about one per 0.5 m² on large ones) the 16-sample
version has p95 errors of 0.14 mag (VisorSat), 0.17 (ISS), 0.07 (V2 Mini) and under 0.02 for the other
models.

### On the GPU

The occluders and samples are packed (`packSatOcclusionGpu()`) into `GpuSatOccluder` (80 bytes) and
`GpuSatLobeSample` (32 bytes) records in root-triad coordinates, the same convention as lobe normals
(see [Attitude](attitude.md#gpu-form)). `modelFlux()` in `sat_orbit.comp` computes the unoccluded total
first; only if occlusion is on, the type has occluders, and that total is brighter than mag 10 does it
subtract each lobe's blocked share. Lobes carrying less than \( 10^{-4} \) of the total are left
unoccluded (at most 0.005 mag). Coincident surfaces put samples exactly on shadow edges, where float and
double can disagree, so models leave small gaps between touching parts.

Occlusion is **off by default** in the app ("Satellite part occlusion", Photometry tab): it makes the
orbit pass roughly ten times more expensive at millions of satellites, for a subtle effect. The
benchmarks run with it on.

## The legacy two-surface model

Types without a geometry model use an older empirical model, kept so legacy and modded rosters render as
they always have. It is **not** a physical magnitude: it is in tuned display units anchored at "a
Starlink is about mag 6". Per type: a primary and an optional secondary surface, each oriented by an
attitude group and given a Phong exponent, a mirror fraction with a very narrow extra spike, a diffuse
floor and a cross-section \( c = \sqrt{A / 10\,\text{m}^2} \):

\[
\begin{aligned}
S_k &= |\hat{\mathbf{s}}\cdot\mathbf{n}_k|\,\big(\hat{\mathbf{r}}_k\cdot\hat{\mathbf{o}}\big)_+^{\,p_k}, \qquad \hat{\mathbf{r}}_k = \text{reflect}(-\hat{\mathbf{s}}, \pm\mathbf{n}_k), \\
S_0 &\mathrel{+}= |\hat{\mathbf{s}}\cdot\mathbf{n}_0|\,(\hat{\mathbf{r}}_0\cdot\hat{\mathbf{o}})_+^{\max(300 p_0,\,8000)}\, B\, f_m, \\
F &= \left(S_0 + w_1 S_1 + d\right) f_\text{lit}\, k_r\, c + F_\oplus,
\qquad k_r = \left(\frac{500\ \text{km}}{\max(r, 500\ \text{km})}\right)^2,
\end{aligned}
\]

with \( B \) the "Mirror boost" setting and \( f_m \) the mirror fraction; the earthshine term
\( F_\oplus \) uses the albedo times the Earth's share of the upward hemisphere times the cap's lit
fraction. (A Phong exponent of 0 means a Lambertian phase \( (\hat{\mathbf{s}}\cdot\hat{\mathbf{o}})_+ \).)
Legacy types cannot be traced, exported or benchmarked.

## From intensity to the display's flare units

The GPU carries brightness as **effectFlare**, a linear unit anchored at 0.008 = mag 6:

\[
\text{effectFlare} = K_\text{flux}\,\frac{I}{r^2}\,b, \qquad
K_\text{flux} = 0.008 \times 10^{0.4(6 - m_\odot)} = 9.979 \times 10^{10},
\]

where \( b \) is the global "Brightness" setting (default 0.93, so displayed magnitudes are about
0.08 mag fainter than physical). Conversely
\( m = 6 - 2.5\log_{10}(\text{effectFlare}/0.008) \). Everything downstream (sky suppression,
extinction, the point-spread function) works in these units; see
[Seeing a satellite from the ground](visibility.md).

### Magnitude conventions

| Quantity | Definition |
|---|---|
| apparent magnitude | \( m = -26.74 - 2.5\log_{10}(I/r^2) \), above the atmosphere |
| 1000 km magnitude | \( m_{1000} = m - 5\log_{10}(r / 1000\ \text{km}) \), the standard for comparing satellites |
| phase angle | angle at the satellite between Sun and observer (0 = fully lit face) |
| off-specular angle | angle between the observer and the Sun's mirror image in a nadir-facing plate |
| as seen | apparent magnitude plus extinction, see [visibility](visibility.md#atmospheric-extinction) |

## The CPU evaluator and the GPU parity check

`evalSatPhotometry()` (`src/simulations/SatPhotometry.cpp`) is the double-precision mirror of the GPU
chain for one satellite at one instant: orbit (`satOrbitStateAt()`), attitude (`evalGroupPoses()`),
posed lobes, Earth shadow, earthshine, optional occlusion, then magnitude, 1000 km magnitude, phase and
off-specular angles and the dominant lobe. Satellites aimed at a ground site take their mirror aim from
`satGroundSiteIdeal()`, the CPU mirror of the GPU's lock-window search (see
[Reflectors and beams](reflectors.md)). It is the single source for everything measured rather than
drawn: the selection's magnitude readout, pass traces, bulk exports and
[SatBench](../accuracy/satbench.md).

The **parity check** runs every frame for the selected satellite. The inputs of that frame's GPU dispatch
(sim time, Sun, observer, flare tilt, brightness and mirror-boost settings, occlusion state) are recorded;
next frame the CPU re-evaluates the satellite at exactly those inputs and compares with the
pre-photometry flux the GPU wrote for it. A difference above 0.02 mag is logged to `satlight_log.txt`
(at most every 120 frames). The expected gap is float rounding: about a metre of orbit position (see
[Orbits](orbits.md#position-error-budget)) and float lobe arithmetic, which exceed 0.02 mag only with the
observer very close to the satellite. Legacy types are compared in their own units through a mirror of
the legacy formula.

## Settings

| UI label (Photometry tab) | `settings.json` key | Default |
|---|---|---|
| Brightness | `photometry.brightness_scale` | 0.93 |
| Mirror boost (legacy model only) | `photometry.mirror_boost` | 1000 |
| Satellite part occlusion | `photometry.sat_part_occlusion` | off |
| Flare mitigate tilt (deg) | `photometry.flare_mitigation_tilt_deg` | 0 |

## Where in the code

| File | Function / symbol |
|---|---|
| `src/simulations/SatModel.h/.cpp` | `tessellateSatModel()`, `bakeSatLobes()`, `lobeIntensity()`, `validateSatLobes()`, `buildSatOcclusion()`, `satLobeVisibility()`, `packSatOcclusionGpu()`, `SatEarthLight`, `kSunAlpha` |
| `src/simulations/SatPhotometry.cpp` | `earthshineExact()`, `earthshineLut()`, `earthshineLight()`, `evalSatPhotometry()`, `satMagnitudeFromIntensity()` |
| `src/simulations/SatelliteSim.cpp` | `bakeModelType()` (budget, validation), `updateSelectedPhotometry()` (parity) |
| `shaders/sat_orbit.comp` | `lobeIntensity()`, `earthshineAt()`, `modelFlux()`, `occludedIntensity()`, `legacyFlux()`, `K_FLUX`, `SUN_ALPHA` |
| `tools/sat_model_tool/` | `SatModelTool`: bake, validate and self-test models without the app |
