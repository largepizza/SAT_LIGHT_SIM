# Seeing a satellite from the ground

What happens between a satellite's above-atmosphere brightness (see [Satellite photometry](photometry.md))
and the point of light on screen: the Earth's shadow, the horizon, atmospheric extinction, the brightness
of the sky it is seen against, light pollution, and the point-spread model that turns a magnitude into
pixels. Stars and planets go through the same chain, so equally bright objects look equal.

## The chain in one line

For a satellite, `sat_orbit.comp` produces the above-atmosphere flux in effectFlare units (0.008 = mag 6)
and `sat_flare.comp` applies, in order:

\[
F_\text{shown} = \operatorname{ceil}\!\Big(
\frac{F}{1 + (D\,s_d + M\,s_m)\,a}
\cdot 10^{-0.4\,X}
\cdot (1 - 0.85\,P)
\cdot (1 - 0.85\,P_b)\Big)
\]

where \( D \) and \( M \) are the day and moon sky brightness, \( a \) the share of the atmosphere still
above the observer, \( X \) the extinction in magnitudes, \( P \) the light-pollution dome and \( P_b \)
the dome from Reflect-Orbital beams in that direction. Each term is described below. Stars and planets
use the same terms, computed on the CPU (`updateStars()`, `updatePlanets()`), with the differences noted.

## Earth's shadow

A satellite is lit unless it is inside the Earth's shadow. The shadow is modelled as two cones from the
Sun's and the Earth's real sizes (Sun radius \( 6.96 \times 10^8 \) m at 1 AU, Earth radius 6371 km),
with half-angle tangents

\[
\tan\beta_u = \frac{R_\odot - R}{1\ \text{AU}}, \qquad \tan\beta_p = \frac{R_\odot + R}{1\ \text{AU}}.
\]

For a satellite at \( \mathbf{r} \), let \( x = -\mathbf{r}\cdot\hat{\mathbf{s}} \) be its distance
behind the Earth's centre along the shadow axis and \( \rho \) its distance from that axis. The umbra and
penumbra radii there are

\[
\rho_u = \max(0,\ R - x\tan\beta_u), \qquad \rho_p = R + x\tan\beta_p,
\]

and the lit factor is \( f_\text{lit} = \operatorname{smoothstep}(\rho_u, \rho_p, \rho) \) for
\( x > 0 \), else 1. In LEO the penumbra is about 60 km wide; at GEO about 400 km. The smoothstep is a
stand-in for the true visible fraction of the solar disk (a lens-area function of the disk overlap);
both run from 0 at the umbra's edge to 1 at the penumbra's.

Not modelled: refraction of sunlight into the umbra by the Earth's atmosphere, and the atmosphere's
extra shadow height (the "shadow" is a hard sphere of radius \( R \)). As a visual cue only, satellites in
the penumbra are tinted toward red by \( 4 f_\text{lit}(1 - f_\text{lit}) \), peaking mid-penumbra, for
the long grazing path their sunlight takes through the air. Brightness benchmarks exclude penumbra
passes, as the published campaigns do.

## The horizon

A satellite below the observer's Earth limb is not processed further (see
[Orbits](orbits.md#horizon-cull-and-the-compact-list)). Stars and planets below the limb are culled the
same way: at the ground the limb is the horizontal; from altitude it dips to
\( -\arccos(R/r_\text{obs}) \).

## Atmospheric extinction

Light from a point source loses \( X \) magnitudes on its way through the air. One function computes it
for every consumer (satellites, stars, planets, the Milky Way, zodiacal light, aurora):
`atmExtinctionMag()` in `shaders/include/atmosphere.glsl`, with a double-precision C++ mirror in
`src/simulations/SatPhotometry.cpp`.

### The model

Two exponential components share the **sea-level zenith extinction** \( k \) (the "Extinction" setting,
default 0.079 mag):

| Component | Scale height | Share of \( k \) |
|---|---|---|
| molecular (Rayleigh, ozone folded in) | \( H_m = 8 \) km | 60% |
| aerosol haze | \( H_a = 1.2 \) km | 40% |

\[
X = k\left[0.6\,N(H_m) + 0.4\,N(H_a)\right],
\]

where \( N(H) \) is the column of a component along the line of sight, in units of the sea-level
vertical column \( \int_0^\infty e^{-h/H}\,dh = H \). The model is absolute: looking straight up from sea
level loses exactly \( k \); from orbit, a ray that does not dip into the air loses nothing.

### The Chapman column

For a ray starting at radius \( r \) with zenith angle \( \chi \) and running to infinity, the column is
given by the Chapman function. With \( x = r/H \):

\[
N_\infty(r, \chi) = e^{-(r - R)/H}\,\sqrt{\frac{\pi x}{2}}\ \operatorname{erfcx}\!\left(\sqrt{\frac{x}{2}}\,|\cos\chi|\right), \qquad \cos\chi \ge 0,
\]

the asymptotic form for large \( x \) (here \( x \ge 800 \)). For a descending ray (\( \cos\chi < 0 \)) the
column is the whole line through its tangent point, radius \( r_t = r\sin\chi \) (never below \( R \)),
minus the part behind the start:

\[
N_\infty(r, \chi) = 2\,e^{-(r_t - R)/H}\sqrt{\frac{\pi r_t}{2H}} - N_\infty(r, \pi - \chi).
\]

A finite segment to a target at distance \( L \) (a satellite at its range) is the difference of two
infinite columns, evaluated forwards for a climbing or tangent-crossing ray and backwards for a ray that
only descends. So a satellite silhouetted against the Earth, seen from orbit, gets only the air between
it and the eye.

The scaled complementary error function uses a one-parameter fit,

\[
\operatorname{erfcx}(y) \approx \frac{1}{\sqrt{\pi}\,\big(0.6564\,y + 0.3436\sqrt{y^2 + 2.6961}\big)},
\]

exact at 0 and at infinity, with a maximum relative error of 0.33%. The whole column agrees with a
brute-force ray integral to 0.32% for observers from sea level to 400 km and elevations from +90 deg to
-19 deg (`SatModelTool --selftest`).

### Numbers

Extinction in magnitudes for a source at infinity:

| Eye height | \( k \) | Zenith | 30 deg | 10 deg | Horizon |
|---|---|---|---|---|---|
| sea level | 0.079 (default) | 0.08 | 0.16 | 0.44 | 4.6 |
| 4 km | 0.079 | 0.03 | 0.06 | 0.17 | 1.1 |
| 10 km | 0.079 | 0.01 | 0.03 | 0.08 | 0.48 |
| sea level | 0.25 | 0.25 | 0.50 | 1.41 | 14.4 |
| 4 km | 0.25 | 0.09 | 0.19 | 0.53 | 3.5 |
| 10 km | 0.25 | 0.04 | 0.09 | 0.24 | 1.5 |

The horizon value is larger than a classic airmass formula would give (Kasten and Young's airmass of
about 38 times \( k \)) because the low aerosol layer stacks up along grazing paths. Real clear-sky V-band
values of \( k \) are 0.2 to 0.3 at sea level; the default 0.079 is a tuned, very clear sky.

### Limits

Fixed scale heights and a fixed 60/40 split; no weather, no site-specific haze, no wavelength dependence
(stars and satellites are not reddened by extinction); no refraction, so apparent elevations are
geometric.

## Sky brightness

A satellite seen against a bright sky is harder to see. The model treats this as a division of its flux
by a sky term, uniform over the sky:

\[
F \leftarrow \frac{F}{1 + (D\,s_d + M\,s_m)\,a}.
\]

- **Daylight** \( D = t^2 \), \( t = \operatorname{clamp}\!\big((\sin e_\odot + 0.05)/0.39,\ 0, 1\big) \),
  with \( e_\odot \) the Sun's geocentric elevation at the observer. \( D \) rises from 0 at
  \( e_\odot \approx -2.9 \) deg to 1 at about 20 deg. \( s_d \) is "Day suppress" (default 570).
- **Moonlight** \( M = t_m^2\, \ell \), \( t_m = \operatorname{clamp}(\sin e_\text{moon} / 0.5,\ 0, 1) \)
  (full by 30 deg of Moon elevation), where \( \ell = (1 - \hat{\mathbf{s}}\cdot\hat{\mathbf{m}})/2 \) is
  the Moon's lit fraction. \( s_m \) is "Moon suppress" (default 6.6).
- **Altitude** \( a \) is 1 up to 40 km and falls linearly to 0 at 100 km: above the atmosphere the sky
  is black whatever the Sun does.

The sky is uniform in this term: there is no brighter twilight glow toward the Sun and no brightening
around the Moon's disk.

**Stars and planets** use a separate, older form of the same idea: they are multiplied by a night factor
\( \operatorname{clamp}(-5\sin e_\odot,\ 0, 1) \) (dark from about 11.5 deg of solar depression) and by
\( 1 - 0.9\,M_\star \), where \( M_\star \) uses the Moon's **phase-law brightness** (relative to full,
see [Sun, Moon and planets](sun-moon-planets.md#brightness-the-phase-law)) instead of the lit fraction.
From orbit (where \( a = 0 \)) their gate is instead the Sun-glare visibility, so a sunlit view hides
them.

## Light pollution

City light scattered in the air brightens the sky toward cities and, less, overhead. The model is a
16-sector **dome** around the observer, rebuilt every frame by `updateLightPollutionDome()`.

### Building the dome

For each of 16 bearings (22.5 deg sectors, clockwise from north), the night-lights map is sampled at
2, 8, 20, 45, 90 and 150 km along the bearing (a tangent-plane offset, bilinear on a blurred copy of the
map), and the sector takes the **weighted maximum**

\[
c_\text{raw} = \max_j \Big[ \max(0,\ L_j - 0.06)\; e^{-D_j / 35\,\text{km}} \Big],
\]

where \( L_j \) is the map's brightness. A maximum, not a mean, so one nearby bright city dominates its
direction. The value is compressed and scaled:

\[
c = \frac{c_\text{raw}}{c_\text{raw} + 0.08}\; e^{-h_\text{obs}/3\,\text{km}}\; g,
\]

with \( h_\text{obs} \) the observer's altitude (skyglow fades within a few kilometres of climb) and
\( g \) "Pollution gain" (default 25). Finally a circular 5-tap blur (0.1, 0.2, 0.4, 0.2, 0.1) across
neighbouring sectors removes single-bearing sampling noise. \( c \) is deliberately not clamped, so a
high gain still reaches high elevations.

### Reading the dome

A consumer looking along azimuth \( A \) and elevation \( e \) interpolates linearly between the two
nearest sector centres, then applies an elevation falloff with an isotropic floor:

\[
P = \operatorname{clamp}\!\Big(c(A)\,\big[\,0.4 + 0.6\,\tfrac{0.35}{\max(\sin e, 0) + 0.35}\,\big],\ 0,\ 1\Big).
\]

The bracket is 1 at the horizon and 0.56 at the zenith. Without the 0.4 isotropic share it would be 0.26
at the zenith, and no city could noticeably brighten the sky overhead; real urban skyglow brightens the
zenith by over an order of magnitude. The object's flux is then multiplied by \( 1 - 0.85P \) for
satellites and \( 1 - 0.99P \) for stars and planets, so the brightest objects still show in a city.

### Beams

Reflect-Orbital beams landing near the observer fill a second 16-sector dome (`beamGlowDome`, built in
`sat_orbit.comp` by `atomicMax`), with a 80 km distance falloff from the observer to the beam's site. It
dims objects the same way, without the isotropic floor (a beam is a horizon-hugging source). See
[Reflectors and beams](reflectors.md).

## The brightness ceiling

After the suppressions, a soft ceiling keeps any satellite from out-shining the Sun's own glare:

\[
F \leftarrow \frac{S\,F}{F + S},
\]

where \( S \) is the Sun's reference brightness (40 in flare units) times the cloud transmittance toward
the Sun. It is linear for faint objects and saturates at \( S \).

## From magnitude to pixels: the point-spread model

Satellites, stars and planets are drawn by one model, `pointPsf()` in
`shaders/include/point_style.glsl` (CPU mirror in `SatelliteSim.h`), from the magnitude as shown (after
every term above). The displayed flux is

\[
D = 10^{-0.4\gamma(m - m_\text{ref})} - 10^{-0.4\gamma(m_\text{lim} - m_\text{ref})},
\]

clamped at 0, so a point fades to exactly nothing at the limit magnitude \( m_\text{lim} \). It is drawn
as a Gaussian of standard deviation \( \sigma_0 \) pixels and peak \( D \) while \( D \le 1 \). A brighter
point keeps its peak at 1 and widens, \( \sigma = \sigma_0\sqrt{D} \) (the drawn energy keeps growing
as \( D \)), up to \( \sigma_\text{max} \), past which the core brightens instead. The sprite holds
\( 3\sigma \) plus a pixel of margin each side, at least about 5 px, which keeps a faint point from
flickering as its sub-pixel centre moves.

| Parameter | Setting | Default |
|---|---|---|
| \( m_\text{ref} \): magnitude whose peak reaches 1 | Point peak mag | 1.5 |
| \( \gamma \): display contrast (1 = linear) | Point contrast | 0.8 |
| \( m_\text{lim} \): faintest drawn | Point limit mag | 8.0 |
| \( \sigma_0 \) | Point size (px) | 0.45 |
| \( \sigma_\text{max} \) | Point max size (px) | 6.0 |

**Zoom gathers light.** Narrowing the field of view acts like a telescope whose aperture grows with the
magnification \( M = \tan 35^\circ / \tan(\text{fov}_y/2) \): the effective aperture is
\( \min(7\,\text{mm}\times M,\ A_\text{max}) \) and both \( m_\text{ref} \) and \( m_\text{lim} \) shift
fainter by \( 5\log_{10}(\text{aperture}/7\,\text{mm}) \). \( A_\text{max} \) is "Zoom aperture max (mm)"
(default 200, so up to 7.3 mag of gain). The global exposure shifts them the same way.

A satellite whose shown brightness falls below "Vis threshold" (default 0.0001 in flare units, about
mag 10.7) is dropped before drawing.

## Settings

| UI label (Photometry tab) | `settings.json` key | Default |
|---|---|---|
| Day suppress | `photometry.day_suppression` | 570 |
| Moon suppress | `photometry.moon_suppression` | 6.6 |
| Pollution gain | `photometry.light_pollution_gain` | 25 |
| Extinction | `photometry.extinction_coeff` | 0.079 |
| Vis threshold | `photometry.vis_thresh` | 0.0001 |
| Point peak mag | `photometry.point_ref_mag` | 1.5 |
| Point contrast | `photometry.point_gamma` | 0.8 |
| Point limit mag | `photometry.point_limit_mag` | 8.0 |
| Point size (px) | `photometry.point_sigma_px` | 0.45 |
| Point max size (px) | `photometry.point_sigma_max_px` | 6.0 |
| Zoom aperture max (mm) | `photometry.zoom_aperture_max_mm` | 200 |

## Where in the code

| File | Function / symbol |
|---|---|
| `shaders/sat_orbit.comp` | Earth shadow (`litFactor`), eclipse tint, horizon cull, beam glow dome |
| `shaders/sat_flare.comp` | sky suppression, extinction, light pollution, ceiling, point size |
| `shaders/include/atmosphere.glsl` | `atmColumnInf()`, `atmColumn()`, `atmExtinctionMag()` |
| `shaders/include/point_style.glsl` | `pointPsf()`, `satFlareToMag()`, `pointSpriteSizePx()` |
| `src/simulations/SatPhotometry.cpp` | `atmColumn()`, `atmExtinctionMag()` (CPU mirror) |
| `src/simulations/SatelliteSim.cpp` | `updateLightPollutionDome()`, `updateStars()`, `updatePlanets()` |
| `src/simulations/SatelliteSim.h` | `pointPsf()`, `opticsGainMag()` |
