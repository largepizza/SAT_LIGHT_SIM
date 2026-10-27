# The Sun as an object: refraction, mirages and turbulence

How the air bends the light of the Sun, stars and planets near the horizon, and how the Sun's disc, its
sunspots, glare and corona are drawn through that bending. One ray tracer, run on the CPU against the air's
profile at the eye, produces a table that the sky pass, the star field and the planets all read: a low Sun is
flattened, a warm sea draws an inferior mirage under it, an inversion slices it, the dispersion leaves a green rim,
and turbulence makes its limb boil and the stars twinkle. The sky around the Sun is on
[Atmosphere and sky](atmosphere-and-sky.md); the Sun's position and size are on
[Sun, Moon and planets](../simulation/sun-moon-planets.md); the Sun near the Moon is on
[Moon and eclipses](moon-and-eclipses.md).

## The air's profile at the eye

`SatelliteSim::refrProfileAt()` describes the air above the observer as a `refr::Profile`. It is a pure function of
place and sim time, so time warp, reverse playback and bookmarks see the same mirages.

| Part | Model |
|---|---|
| Background | an ICAO-like standard atmosphere through the air temperature `T0` 10 m (`zRef`) above the surface: -6.5 K/km to 11 km, isothermal to 20 km, +1 K/km to 32 km, +2.8 K/km to 47 km; pressure from the barometric formula layer by layer, 1013.25 hPa at sea level |
| Surface temperature | the eye's sea-level climate \( -18 + 45\cos^{1.5}\varphi \) °C plus a seasonal swing of up to 15 °C, less 6.5 K/km to the surface, clamped to 200-330 K |
| Surface layer | a logarithmic profile (roughness length `z0` = 0.2 mm) carrying the step `dTs` between the surface and the air |
| Over the sea | the water warmer than the air by 0.2-1.6 K (smooth value noise over 3° cells and 12-h steps); on about 1 day in 5 colder by 0.3-1.8 K (warm air over a cold current: looming) |
| Over land | the ground's heating by day, \( 6\sqrt{\sin e_\odot} \) K, and -2 K after dusk |
| Breathing | `dTs` × (1 + 0.25 sin(2πt/83 s) + 0.12 sin(2πt/29 s)): convective cells over the water come and go over tens of seconds |
| Elevated inversion | on about 45 % of days at sea (30 % over land): a tanh step of 0.5-4 K, 5-35 m thick, 3-400 m above the surface (median about 35 m), its height riding gravity waves of 170 s and 53 s |
| Turbulence | \( C_n^2 = (2\times10^{-15} + 6\times10^{-15}\,dT_s^2)\, z^{-4/3}\, e^{-z/1\,\text{km}} \) in the surface layer, plus Hufnagel-Valley's free troposphere |

The surface is the sea (0 m) when the eye stands over water, else the ground under it. "Mirage" scales both
temperature steps; "Turbulence" scales \( C_n^2 \).

## The ray tracer

Refractivity at 550 nm is \( N = 77.6 \cdot 1.02486 \, P / T \) (hPa, K), with Edlen-like dispersion for the three
colour channels at 610, 545 and 465 nm (`refr::kDisp`: 0.9955, 1.0005 and 1.0097 of the 550 nm value), scaled by
"Dispersion" (1 = physical). Its vertical gradient follows from the pressure and temperature gradients,
\( dN/dz = N\,(-g/(R_d T) - (dT/dz)/T) \). Standard air bends rays down (about -30 to -40 N/km); a gradient below
-157 N/km (the Earth's curvature, an elevated inversion) ducts them; air warmed from below (a temperature gradient
steeper than about -34 mK/m over a warm sea) bends them up, which is the inferior mirage.

`refr::build()` traces rays **back** from the eye, in the eye's vertical plane:

- **Elevations.** 512 apparent elevations (`refr::kTableN`) from 0.34° below the eye's geometric horizon to 4° above
  it, on \( e(u) = e_C + c\,\sinh(\alpha(u - u_0)) \): about 3 arcsec apart at the horizon, about 30 arcsec at 0.5°.
- **Stepping.** Midpoint steps of \( d\hat{\mathbf{d}}/ds = \nabla \ln n - (\nabla \ln n \cdot \hat{\mathbf{d}})\,\hat{\mathbf{d}} \),
  sized by the local scale the profile varies on (1.5 km in the troposphere, down to the height above the surface in
  the surface layer and a fifth of the inversion's thickness inside it) and by the ray's curvature.
- **Ends.** At the top of the air (100 km), on the sea or ground (`refr::kHit`), or trapped in a duct after 4000 km.
- **Per entry** (`refr::Entry`): the refraction per colour channel (apparent minus true elevation), the Rayleigh,
  Mie, ozone and haze-near-clouds columns along the **bent** path, the rms angle-of-arrival jitter
  \( \sqrt{2.91\, D^{-1/3} \int C_n^2\, ds} \) with an aperture \( D \) = 1 cm, and the scintillation index
  \( \sigma_I = \sqrt{1 - e^{-\sigma_R^2}} \), with Rytov's \( \sigma_R^2 = 2.25\,k^{7/6} \int C_n^2 s^{5/6}\,ds \)
  (saturating for long horizontal paths).
- **Above the table** the refraction continues as Saemundsson's formula scaled to match the top entry (0 from
  orbit), the jitter variance grows as \( 1/\sin e \) and the Rytov variance as \( \sin(e)^{-11/6} \) from the zenith
  ray's values.

!!! note "Why one tracer"
    A formula above the horizontal and a tangent-height formula below it disagree at the horizontal (by about
    0.1° with a standard atmosphere), and a cap on the bending below it halves the refraction there: the low Sun's
    disc is then cut flat at the horizontal with a step. Mirages, ducting, the green rim and the twinkle are all
    consequences of the same profile, so they come from the same rays.

### When the table is rebuilt

`updateRefraction()` (`SatelliteSimRefraction.cpp`) builds the table on a worker (`std::async`, half the hardware
threads, about 7-11 ms a build): ten times a second while the Sun is within about 4.5° of the horizon, once a second
otherwise, and at once when the eye's height changes by more than 2 % (+0.3 m). The first table after a start or a
load is built on the frame itself, so no frame is drawn without one. `refrDerive()` then derives, on the CPU:

- the **primary image's** inverse map (true to apparent elevation) for the stars and planets;
- the Sun's centre's refraction and the **visible share** of its disc over every image (an inferior mirage adds the
  inverted one; ducting removes stripes);
- the air columns the Sun's **visible** disc is seen through: the mean over its visible rows, weighted by chord ×
  transmittance and scaled so its transmittance matches the rows' mean. While the centre's ray grazes the sea it
  crosses hundreds of km of air, and the moment it ends on the sea it gives the short column of a ray that never
  left; the mean over the visible rows changes smoothly as rows set. Above the table the shader takes the exact Sun
  path instead.

The table, the derived values and the sunspots reach the GPU in the refraction block at the tail of the
ground-beams buffer (`GpuRefraction`, binding 21 of the sky pass), which already holds per-frame CPU data.

## The Sun's disc

Drawn by `sat_sky.frag` in the full sky variants (`SKY_SUN_SHADOW`). Each pixel's ray is treated as an
**apparent** direction:

1. **Turbulence.** The apparent elevation is jittered by a value-noise field (cells about 40 arcsec across and 12
   arcsec high) that drifts with the wind and boils over time, scaled by the table's rms jitter at that elevation.
   The limb shimmers; the stars twinkle from the same table.
2. **Bending.** Per colour channel the table gives the true direction the light came from. A channel whose ray ends
   on the sea or ground draws nothing. The disc is drawn 1.5 times its true radius (0.007 rad), or at its true radius
   near the Moon.
3. **Mirages show below the drawn sea line.** Over the far sea (no terrain, mesh or opaque cloud on the ray) the
   table, not the drawn horizon, decides what is visible: the inverted image of an inferior mirage appears below the
   sea line, as the Etruscan vase merges into the Omega Sun.
4. **Light.** Each disc pixel's transmittance comes from the table's columns along its bent path (Rayleigh + Mie ×
   1.1 + ozone + the haze near clouds, the latter from the weather about 80 km toward the Sun), times the cloud,
   smoke and dust composite in colour (`cloudB.rgb`). The disc's radiance is physical: \( 4\pi / \Omega_\odot /
   (1 - u/3) \approx 2.3\times10^{5} \) in the sky's units (π × radiance per unit solar irradiance, with phase
   functions of mean 1), times "Sun disc gain", the exposure and limb darkening \( 1 - 0.6\,(1 - \mu) \), and it is
   shown per channel as \( 1 - e^{-L/250} \). A clean sea-horizon Sun is brilliant yellow-orange; a dim red disc
   needs a long, dirty column or smoke.
5. **Dispersion** leaves a green rim on the upper limb, and a green flash when a mirage magnifies it.

What reddens a low Sun is only the columns along its path (Rayleigh, the Mie aerosol and the haze near clouds) and
whatever the cloud composite holds toward it (cloud, smoke, dust). The fog and dust layers of the cloud march are part
of that composite only where they lie on the ray.

### Sunspots

`fillSunspots()` places bipolar active-region **groups** on the solar sphere as a pure function of sim time:

- 48 slots, each a sequence of groups with a cycle of 24-64 days; a group exists with a chance following the 11-year
  solar cycle's activity (`spaceWeather_.cycle`), lives 4-60 days (bigger groups longer), grows over its first ~12 %
  and decays over its second half;
- sizes are heavy-tailed (most groups are a few pores, a few are large, complex regions);
- latitude 6-32° in either hemisphere, tilted by Joy's law (the leader nearer the equator), carried east to west by
  the Sun's differential rotation, \( 14.71 - 2.40\sin^2\varphi - 1.79\sin^4\varphi \) °/day less the Earth's
  0.986 °/day.

The 16 largest groups on the visible side are sent. `sunspotFactor()` draws each as a leader spot, a follower or a
cluster of followers, and 8-22 pores (pinprick dots without a penumbra). Only spots larger than about 2500 km get an
umbra inside a ragged, filamented penumbra; complex (delta) groups have several umbrae sharing one. Faculae brighten
groups toward the limb. Spots show wherever the disc is not saturated: a low or hazy Sun, or a zoomed view through
haze.

The Sun's Fury's flare region is a large delta group at 14° N that crosses the central meridian at the scenario's
X-class flare (17.5 h before the CME's arrival, see [Kessler syndrome](../simulation/kessler.md)) and stays on the
disc for the weeks around it.

### Glare and corona

Three stacked glare lobes \( \cos^{1800}, \cos^{320}, \cos^{55} \) (σ ≈ 0.024, 0.056, 0.135 rad) bridge the disc to a
dim, wide corona whose width grows as the Sun nears the horizon; without the bridge the disc steps straight down to
the corona and reads as a dark ring after the tonemap. Both are centred on the Sun **as seen** (its refracted
centre), scaled by the visible share of its disc, and by \( 1 - e^{-60\,\ell} \), where \( \ell \) is the luminance
of the transmittance of the visible disc's mean column times the cloud composite: the veil follows the light that
actually arrives, continuously as a mirage or the sea swallows the disc. Their colour leans 80 % toward that
transmittance's hue. Over the far sea the veil is drawn too. The whole term is also multiplied by
`min(cloud transmittance, cloud.sunCloudT)`, the eye's transmittance toward the Sun through the clouds.

Near the Moon the disc and glare follow the Sun the observer sees past it, and at totality the solar corona is drawn;
see [Moon and eclipses](moon-and-eclipses.md#solar-eclipses). The lens flare is on
[Atmosphere and sky](atmosphere-and-sky.md#lens-flare).

!!! note "Light tiers and probes"
    `SKY_LITE` and Potato draw a hand-tinted disc reddened by its height above the limb, with the same glare lobes,
    and do not read the refraction table. The `SKY_ENV` probes leave the Sun's disc out.

## Stars and planets

`updateStars()` and `updatePlanets()` call `refractEnu()` for every object within about 1.7° of the limb (stars only
at night):

- the object is placed at the **primary image's** apparent elevation, so a star a little below the true horizon
  still shows, and one below the apparent horizon does not;
- its record's `meshPx` carries \( 1 + \lfloor \text{aoa}/\mu\text{rad} \rfloor + 0.99\,\sigma_I \).
  `star_point.vert` jitters the image by the angle of arrival (sines with a per-star phase) and twinkles it by
  \( \sigma_I / 0.55 \).

Planets are placed and jittered the same way but do not twinkle: their 10-40 arcsec discs average the
scintillation out. Satellites and the Moon are not refracted.

## The sea glint's sparkle

The same physics, the Sun's disc mirrored by millions of wave facets, makes the glitter on the sea: the glint lobe
is the pixel's mean, and where the facets that mirror the Sun cover a small share of a pixel it arrives as sparkles.
See [The sea](sea.md#the-sun-glint).

## Settings

No UI rows; the keys live under `refraction` in `settings.json`.

| Key | Default | Effect |
|---|---|---|
| `refraction.enabled` | true | the Sun's disc and the stars bent by the table (off: the disc unbent; the stars keep their turbulence) |
| `refraction.mirage` | 1 | × the surface layer's and the inversion's temperature steps (0-5) |
| `refraction.turbulence` | 1 | × \( C_n^2 \): the limb's shimmer, the stars' twinkle and jitter (0-20) |
| `refraction.dispersion` | 1 | × the physical dispersion: the green rim (0-10) |
| `refraction.sun_disc_gain` | 1 | × the Sun disc's physical radiance (0-10) |
| `refraction.force_sea_air_dt` | -100 | the surface-minus-air step in K; below -50 = from place and time |
| `refraction.force_inversion_m` | -1 | below 0 = from place and time, 0 = none, above 0 = the inversion's height in m |
| `refraction.force_inversion_dt` / `_dz` | 2 / 15 | the forced inversion's warming (K) and thickness (m) |

## Debugging

Harness `refraction [rebuild]` prints the profile, the apparent horizon, the Sun's refraction and visible share,
samples of the apparent-to-true map, the sunspot groups (`sunspot_groups`, `solar_cycle`) and the build time; `state`
carries the same under `refraction`. The `force_*` keys set up a mirage or an inversion on demand.

## Where in the code

| File | What |
|---|---|
| `src/simulations/Refraction.h`, `Refraction.cpp` | `refr::Profile`, `refr::build()` (the tracer), `refr::lookup()`, `refr::saemundsson()` |
| `src/simulations/SatelliteSimRefraction.cpp` | `refrProfileAt()`, `updateRefraction()`, `refrDerive()`, `fillSunspots()`, `refractEnu()`, `refractionJson()` |
| `shaders/sat_sky.frag` | `refrLookup()`, `refrTau()`, `sunspotFactor()`, the Sun disc block |
| `shaders/star_point.vert` | the stars' jitter and twinkle from `meshPx` |
| `src/simulations/SatelliteSim.cpp` | `updateStars()`, `updatePlanets()` |
