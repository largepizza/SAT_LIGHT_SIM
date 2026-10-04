# Sun, Moon and planets

The ephemerides of the Sun, the Moon and the five naked-eye planets plus Uranus, the Moon's brightness,
how eclipses are computed and found, and the star catalogue. How the Moon's disk, the corona and the
eclipse sky are drawn is on [Moon and eclipses](../rendering/moon-and-eclipses.md). All of this runs on
the CPU in double precision once per frame (`updatePositions()`), from the sim time \( t \) in seconds
since J2000 (see [Time and reference frames](time-and-frames.md)).

Throughout, \( d = t / 86400 \) is days since J2000 and \( T = d / 36525 \) Julian centuries.

## The Sun

`sunDirEciAt()` uses the low-precision formula of the *Astronomical Almanac* (accuracy about 0.01 deg
over a few centuries around 2000):

\[
\begin{aligned}
L &= 280.46^\circ + 0.9856474^\circ\,d && \text{mean longitude} \\
g &= 357.528^\circ + 0.9856003^\circ\,d && \text{mean anomaly} \\
\lambda &= L + 1.915^\circ \sin g + 0.020^\circ \sin 2g && \text{ecliptic longitude} \\
\varepsilon &= 23.439^\circ - 0.0000004^\circ\,d && \text{obliquity}
\end{aligned}
\]

\[
\hat{\mathbf{s}} = (\cos\lambda,\ \sin\lambda\cos\varepsilon,\ \sin\lambda\sin\varepsilon).
\]

The distance, from the same series (`sunDistAuAt()`), is

\[
r_\odot = 1.00014 - 0.01671\cos g - 0.00014\cos 2g\ \ \text{AU},
\]

and the Sun's **true angular radius** \( \arcsin(R_\odot / r_\odot) \) with \( R_\odot = 6.96 \times
10^8 \) m runs from 0.2624 deg in July to 0.2711 deg in January. It is what decides whether a solar
eclipse is total or annular.

The direction is geocentric: the Sun's parallax from the Earth's surface (at most 8.8 arcsec) is ignored,
except in the eclipse limits below.

## The Moon

### Position: Meeus chapter 47

`moonGeoEciAt()` evaluates the main periodic terms of Meeus, *Astronomical Algorithms*, chapter 47 (the
ELP-2000/82 truncation): 14 terms in longitude, 7 in latitude and 10 in distance, with the Earth's
orbital eccentricity factor \( E = 1 - 0.002516\,T \) on the terms involving the Sun's anomaly. The
fundamental arguments are

\[
\begin{aligned}
L' &= 218.3164477^\circ + 481267.88123421^\circ\,T && \text{mean longitude} \\
D &= 297.8501921^\circ + 445267.1114034^\circ\,T && \text{mean elongation} \\
M &= 357.5291092^\circ + 35999.0502909^\circ\,T && \text{Sun's mean anomaly} \\
M' &= 134.9633964^\circ + 477198.8675055^\circ\,T && \text{Moon's mean anomaly} \\
F &= 93.2720950^\circ + 483202.0175233^\circ\,T && \text{argument of latitude}
\end{aligned}
\]

and the leading terms are, for example, \( \lambda = L' + 6.288774^\circ \sin M' + 1.274027^\circ
\sin(2D - M') + 0.658314^\circ \sin 2D + \dots \) and \( r = 385000.56 - 20905.355\cos M' - \dots \) km.
Ecliptic longitude and latitude are rotated to the equator by the obliquity of date. The truncated
series is good to about 0.05 deg in position, which is a few minutes in the timing of an eclipse. Terms
that would push it to Meeus's full accuracy (about 10 arcsec) are left out.

### Topocentric direction and size

The Moon is close enough that the observer's position matters. Its direction in the sky is
**topocentric**, from the observer's eye (up to about 1 deg of parallax from the ground, far more from
deep space):

\[
\hat{\mathbf{m}}_\text{topo} = \widehat{\mathbf{r}_\text{moon} - \mathbf{r}_\text{obs}},
\]

and its true angular radius is \( \arcsin(1737.4\ \text{km} / |\mathbf{r}_\text{moon} -
\mathbf{r}_\text{obs}|) \). The disk is drawn larger than true by "Moon size (x real)" (default 2.5,
because the real half-degree disk is a few pixels at a wide field of view), except within a few degrees of
the Sun: between 4 and 1.5 deg of separation the scale eases back to 1, so a solar eclipse's contacts,
totality and annularity happen at their real times.

### Orientation

The Moon is tidally locked: its texture's facing axis points at the **Earth's centre**, not at the
observer, so the face does not turn as the observer moves. Seen from the Earth's centre the same face
always points back exactly: there is no optical libration, apart from the observer's own parallax. The far side, seen from deep space, is the near-side
image flattened toward its mean.

### Brightness: the phase law

The Moon's light relative to full Moon follows Allen's phase law,

\[
\frac{F(a)}{F(0)} = 10^{-0.4\,(0.026\,a + 4 \times 10^{-9} a^4)}, \qquad a = \text{phase angle in degrees} \approx 180^\circ - \text{elongation},
\]

which is about 0.09 at quarter and 0.04 for a 32% crescent, far less than the lit fraction because of the
lunar surface's opposition surge. This value drives the Moon's light on clouds, the ground, the sky and
the star field. The lit **fraction** \( (1 - \hat{\mathbf{s}}\cdot\hat{\mathbf{m}})/2 \) is used for the
disk's own shading.

!!! note
    The satellites' moonlit-sky suppression in `sat_flare.comp` computes its own moon term from the lit
    fraction rather than the phase-law value (see
    [Seeing a satellite from the ground](visibility.md#sky-brightness)).

## Eclipses

### Solar eclipses

For the observer, the fraction of the Sun's disk hidden by the Moon is the overlap area of two disks of
angular radii \( \theta_\odot \) and \( \theta_m \) at separation \( s \) (both topocentric):

\[
f = \frac{\theta_\odot^2 \cos^{-1} c_1 + \theta_m^2 \cos^{-1} c_2 - \tfrac{1}{2}\sqrt{(-s + \theta_\odot + \theta_m)(s + \theta_\odot - \theta_m)(s - \theta_\odot + \theta_m)(s + \theta_\odot + \theta_m)}}{\pi\theta_\odot^2},
\]

with \( c_1 = (s^2 + \theta_\odot^2 - \theta_m^2)/(2s\theta_\odot) \) and
\( c_2 = (s^2 + \theta_m^2 - \theta_\odot^2)/(2s\theta_m) \) (`discOverlapFrac()`). The visible
\( 1 - f \) scales the Sun's glare, its bloom, and the observer's adaptation.

For every other point (ground, air, cloud) the shaders use a cheaper per-point form
(`eclipseSunVis()`, `shaders/include/eclipse.glsl`): the covered share is the equal-disk lens area at
the point's position between the umbra's edge (separation \( |\theta_m - \theta_\odot| \)) and first
contact (\( \theta_m + \theta_\odot \)), with a floor of \( 1 - \theta_m^2/\theta_\odot^2 \) for an
annular eclipse. It is exact for equal disks and within about 2% of the Sun's disk for the real radii.
A blurred version (averaged over about 180 km) gives the sky light in the shadow. All sunlit terms take
this factor, so the Moon's shadow falls consistently on everything; see
[Moon and eclipses](../rendering/moon-and-eclipses.md).

### Lunar eclipses

The Moon's surface is shadowed point by point by the Earth's disk seen from that point, against the Sun's
disk (including the Sun's 0.15 deg parallax between the Earth and the Moon), through the penumbra into
the umbra. The Earth's disk is enlarged by 1/85 to account for its atmosphere (the Danjon enlargement),
and the umbra carries the red light refracted through the Earth's atmosphere.

### Finding eclipses

`findEclipse()` searches forward from a time, for up to three years:

1. Step hourly through the angle between the Moon's geocentric direction and the Sun (solar) or the
   anti-Sun (lunar), and find local minima below 3 deg.
2. Refine each minimum by golden-section search (40 iterations).
3. Accept it if the minimum separation is below the eclipse limit:

\[
\text{solar: } s < \pi_m + \theta_\odot + \theta_m - \pi_\odot, \qquad
\text{lunar (umbral): } s < 1.0118\,(\pi_m + \pi_\odot - \theta_\odot) + \theta_m,
\]

with \( \pi = \arcsin(6378.137\ \text{km} / \text{distance}) \) the horizontal parallaxes and
\( \theta \) the angular radii. The solar limit accepts any eclipse visible somewhere on Earth, partial or
central; the lunar one accepts umbral eclipses only.

The time returned is the minimum of the **geocentric** separation, close to (but not by definition) the
instant of greatest eclipse.

### Verification

Running the search and comparing with published eclipse predictions:

| Eclipse | Search result (minimum geocentric separation) | Published greatest eclipse |
|---|---|---|
| total lunar | 13:59:48 UTC, 0.40 deg from the anti-Sun | about 14:01 UTC |
| total solar | 02:42:56 UTC, 0.68 deg | about 02:40 UTC, over central Australia |

The two are the total lunar eclipse of 31 January 2037 and the total solar eclipse of 13 July 2037. <!-- history-ok -->
The search times were reproduced independently from the formulas on this page. The harness script
`tools/harness/scripts/eclipses_2037.satcmd` views both from Australian sites at their real UTC
times, and the harness `eclipse solar|lunar` command finds the next one and places the observer under it.

## Planets

Mercury, Venus, Mars, Jupiter, Saturn and Uranus are computed (Neptune, at magnitude 7.8, is never
naked-eye and is left out).

### Positions

Each planet, and the Earth-Moon barycentre, follows a Keplerian ellipse whose elements change linearly
with time: JPL's *Keplerian Elements for Approximate Positions of the Major Planets* (Standish), the table
valid 1800 to 2050 (the sim's 2030s are inside it). For each body, at time \( T \):

1. Elements \( a, e, i, L, \varpi, \Omega \) from their J2000 values and centurial rates.
2. Mean anomaly \( M = L - \varpi \), wrapped to \( \pm 180^\circ \).
3. Kepler's equation \( M = E - e\sin E \) by Newton-Raphson (to \( 10^{-9} \) rad, at most 8
   iterations).
4. Position in the orbital plane \( (a(\cos E - e),\ a\sqrt{1 - e^2}\sin E) \), rotated by
   \( \omega = \varpi - \Omega \), \( i \), \( \Omega \) into the J2000 ecliptic.
5. Geocentric vector = planet minus Earth-Moon barycentre, rotated to the equator by the obliquity of
   date.

Not applied: light time (up to about 2.7 hours for Uranus, in which it moves under 0.01 deg),
aberration (20 arcsec), the Earth-Moon barycentre offset (up to 4700 km, negligible at planetary
distances), and the J2000-to-date precession of the ecliptic.

### Brightness

Apparent magnitudes follow Paul Schlyter's formulas, with \( r \) the planet's distance from the Sun,
\( \Delta \) from the Earth (AU) and \( \alpha \) the phase angle at the planet (degrees):

| Planet | \( V \) |
|---|---|
| Mercury | \( -0.36 + 5\log_{10}(r\Delta) + 0.027\alpha + 2.2 \times 10^{-13}\alpha^6 \) |
| Venus | \( -4.34 + 5\log_{10}(r\Delta) + 0.013\alpha + 4.2 \times 10^{-7}\alpha^3 \) |
| Mars | \( -1.51 + 5\log_{10}(r\Delta) + 0.016\alpha \) |
| Jupiter | \( -9.25 + 5\log_{10}(r\Delta) + 0.014\alpha \) |
| Saturn | \( -9.00 + 5\log_{10}(r\Delta) + 0.044\alpha \) |
| Uranus | \( -7.15 + 5\log_{10}(r\Delta) + 0.001\alpha \) |

Saturn's rings are omitted (they need the ring-plane geometry, not just the phase angle), so Saturn reads
up to about a magnitude too faint when the rings are wide open. The phase angle is
\( \cos\alpha = \mathbf{r}_\text{helio}\cdot\boldsymbol{\Delta}_\text{geo} / (r\Delta) \).

A planet's magnitude is converted to a relative flux \( 10^{-0.4V} \) and goes through the same sky,
extinction, light-pollution and point-spread chain as a star (see
[Seeing a satellite from the ground](visibility.md)), except that planets do not twinkle. Colours are
fixed tints (planets have no B−V index to derive them from); Mars is the only one that is distinctly
coloured.

## Stars

The star field is the **Yale Bright Star Catalogue**, 5th revised edition (Hoffleit and Warren 1991,
CDS V/50): all 8404 stars brighter than \( V = 6.5 \), the catalogue's own completeness limit and the
naked-eye limit under a dark sky. `tools/parse_bsc.py` downloads it from CDS and writes
`src/simulations/star_catalog.h` (right ascension and declination J2000, V magnitude, B−V), sorted
brightest first.

Each star's relative flux is \( 10^{-0.4V} \) (capped at 8, about \( V = -2.3 \)), and its colour comes
from B−V through a linear ramp:

\[
(R, G, B) = \big(\operatorname{clamp}(0.9 + 0.1\,\beta,\ 0.6, 1),\ \operatorname{clamp}(1 - 0.15\,\beta,\ 0.5, 1),\ \operatorname{clamp}(1 - 0.9\,\beta,\ 0.1, 1)\big), \qquad \beta = B - V.
\]

Each frame the J2000 unit vectors are rotated into the observer's ENU frame, and the brightness goes
through the visibility chain.

## Precession

The Sun and the Moon are computed in the mean equinox **of date**; stars, the Milky Way texture and the
planetary ecliptic are J2000. General precession is 50.3 arcsec per year, so in the 2030s the two frames
differ by about 0.5 deg, mostly along the ecliptic. Star patterns are unaffected (they rotate together),
but a star's position relative to the Sun, the Moon or the horizon is off by up to that amount. Nutation
(up to 17 arcsec), stellar proper motion and annual aberration are also not applied, and there is no
atmospheric refraction.

## Settings

| UI label | `settings.json` key | Default |
|---|---|---|
| Moon size (x real) (Atmosphere tab) | `clouds.moon_size` | 2.5 |
| Planets on/off, per planet (Constellations tab) | `planets.show_planets`, `planets.list` | on |

## Where in the code

| File | Function / symbol |
|---|---|
| `src/simulations/SatPhotometry.cpp` | `sunDirEciAt()`, `sunDistAuAt()`, `moonGeoEciAt()` |
| `src/simulations/SatelliteSim.cpp` | `updatePositions()` (Moon size, phase law, eclipse fractions, planets), `keplerEclipticPos()`, `planetApparentMagnitude()`, `kPlanetElements`, `kPlanetColor`, `discOverlapFrac()`, `findEclipse()`, `initStars()`, `updateStars()`, `updatePlanets()` |
| `shaders/include/eclipse.glsl` | `eclipseSunVis()`, `eclipseSkyLight()` |
| `src/simulations/star_catalog.h` | the catalogue (generated) |
| `tools/parse_bsc.py` | catalogue generator |
