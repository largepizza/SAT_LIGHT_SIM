# Aurora and airglow

The emissive upper atmosphere: the faint airglow that every night sky carries, and the aurora around the
geomagnetic poles. Both are drawn as emission added to the sky; neither lights the ground. The scattering
atmosphere they sit in is on [Atmosphere and sky](atmosphere-and-sky.md); the half-resolution compute pass that
draws the aurora is shared with the clouds ([The cloud march](clouds/march.md)).

## Where each part is computed

| Part | Pass | Resolution |
|---|---|---|
| Green and sodium airglow | `sat_sky.frag`, inside the atmosphere loop | full |
| Red airglow | `cloud_march.comp`, `airglowRedMarchCS()` | half |
| Aurora volume and sheets | `cloud_march.comp`, `auroraMarchCS()` | half |
| Aurora in environment probes and mirrors | `sat_sky.frag`, `envAurora()` (`SKY_ENV` only) | probe |
| Aurora and airglow reflected in the sea | `sat_sky.frag`, ocean branch | full |

The half-resolution results are added to the cloud composite's in-scattered light (`B_total`, stored in the
cloud target A), which `sat_sky.frag` composites as `color x cloudB + cloudA`. The aurora therefore respects the
shared scene depth: terrain in front hides it ([Terrain](terrain.md#the-seed-pyramid)).

## Airglow

Three emission bands, each a Gaussian shell in altitude:

| Band | Line | Peak | Half-width | Colour (linear) | Gain |
|---|---|---|---|---|---|
| Green | O I 557.7 nm | 96 km | 9 km | (0.35, 1.0, 0.25) | "Airglow green" |
| Sodium | Na D 589.3 nm | 90 km | 6.5 km | (1.0, 0.65, 0.15) | "Airglow sodium" |
| Red | O I 630 nm | 275 km | 75 km | (1.0, 0.12, 0.05) | "Airglow red" |

The density at a sample is \( \exp(-((h - h_{\text{peak}})/w)^2) \) times the step length, all times
"Airglow gain" and `kAirglowScale` (5e-7).

**Day and night per sample.** Each sample is gated by its own geographic day/night, not the observer's:
`night = 1 - clamp((up . sun + 0.15) / 0.3, 0, 1)`. Looking across the terminator, the night side glows and the
day side does not. There is no Earth-shadow test beyond that.

**Patchiness.** Each band is multiplied by `0.6 + 0.4` of a slowly drifting warped Perlin noise over the sky
direction, and by a coverage mask (`smoothstep(-0.2, 0.5, noise)`, a separate seed per band) mixed in by
"Airglow coverage". Real airglow is structured into patches and gravity-wave bands, not a uniform shell.

**Polar boost** (red only): \( 1 + \text{"Airglow polar boost"} \times \text{ss}(45^\circ, 12^\circ, \theta) \)
times a coverage mask, where \( \theta \) is the geomagnetic colatitude: the red line brightens toward the
auroral zones.

**Where they are marched.**

- Green and sodium peak inside the main atmosphere loop's range (its ceiling is about 100 km), so they accumulate
  in that loop at no extra march cost.
- Red peaks at 275 km, well past that ceiling. Extending the main loop for one band would coarsen the
  near-surface scattering everything else depends on, so red has its own 16-step midpoint march from 100 km to
  500 km, at half resolution in `cloud_march.comp`. The march is cut at the scene depth. Its legs follow the eye:
  from below the shell, inside it, or above it (the ray direction and the eye's position select the near or far
  root, so the march works with the observer flying through the band).

**Variants.** Planetarium and Potato knock out the red march (bit 16); environment probes keep green and sodium
but have no red airglow. `SKY_LITE` (Planetarium) compiles out the noise-based green and sodium terms and draws the
green band in closed form instead, with no patchiness: the band's column along the ray, without a march.

- A ray that grazes the shell at tangent height \( h_T \) holds \( 1.813\sqrt{2 r_0 w}\,
  \exp(-((h_T - 96\,\text{km})/w)^2) \) of it, \( r_0 \) the shell's radius and \( w \) its half-width (1.813 is
  the integral of \( e^{-u^4} \)).
- A ray that crosses the shell at local zenith angle \( Z \) holds \( w\sqrt{\pi}/\cos Z \) per crossing: two
  crossings if it passes under the shell and out again, one if it starts inside the shell.
- A ray from above the shell that ends on the Earth keeps its crossing only near the limb (faded out as
  \( \cos Z \) rises from 0.18 to 0.32), so the night disc seen from orbit is not tinted green.
- The column is capped at the grazing peak and gated by the night at the ray's point nearest the Earth.

From orbit at night this draws the faint green line that marks the Earth's limb; from the ground, a glow low in
the sky. It costs about 3 KB of SPIR-V in the Planetarium variant.

## Aurora

### The geomagnetic frame

The oval is centred on the geomagnetic pole, `kGeomagPoleECEF` = (0.0481, -0.1543, 0.9868), a dipole whose
antipode serves the southern hemisphere: each sample picks the pole on its own side. `auroraFrame()` gives a
sample's geomagnetic **colatitude** and **azimuth** around the pole. The azimuth is fixed to the ground and
drives the noise (folds, columns, coverage); the oval's shape is fixed to the Sun and uses magnetic local time.

**Magnetic local time (MLT).** The CPU projects the anti-Sun direction onto the dipole's equatorial plane each
frame and passes it as `cloud.auroraMidnight.xyz` (ECEF): that direction is magnetic midnight, 0 MLT. A sample's
MLT angle is its angle around the pole from that direction, positive toward dawn. The Earth turns under the oval
once a day, which is how a site at a fixed latitude passes under the midnight bulge in the evening.

### The oval

The oval is a **ring** around the pole, not a cap: inside its poleward edge is the dark polar cap, outside its
equatorward edge the sky is dark too. `auroraOvalGeom()` (`shaders/include/aurora_oval.glsl`) gives the two edges
at a sample's MLT, and `auroraOvalBand()` the brightness between them.

**Shape in MLT.** The ring is widest and furthest equatorward about an hour before magnetic midnight (near
23 MLT) and thinnest, closest to the pole, near noon. Each edge is interpolated between its midnight and noon
colatitude with the weight \( w = \tfrac12 + \tfrac12\cos(\varphi + 0.26) \), \( \varphi \) the MLT angle (the
0.26 rad offset puts the widest point on the dusk side of midnight).

**Edges from Kp.** The edges follow the activity index Kp (see [Space weather](#space-weather)), in magnetic
latitude (MLAT = 90 deg minus colatitude), a Feldstein-Starkov-like fit (`auroraOvalBounds()`):

| Edge | Midnight (MLAT, deg) | Noon (MLAT, deg) |
|---|---|---|
| Equatorward | 66.5 - 2.1 Kp | 76.5 - Kp |
| Poleward | 72.5 - 0.8 Kp | 79 - 0.6 Kp |

A quiet oval (Kp 0) spans 66.5-72.5 deg at midnight; at Kp 9 its equatorward edge is near 47 deg, overhead at
mid-latitudes. Kp is clamped to 0-11.5 here.

**Edge profiles.** The two edges differ:

- the **poleward edge** is sharp, a ramp over 2 deg (from 0.8 deg into the polar cap to 1.2 deg into the oval): the discrete
  arcs end there;
- the **equatorward edge** is soft, fading over 5.5 deg (2 deg inside to 3.5 deg outside): the diffuse aurora.

Both edges are rippled by a slowly drifting noise around the ring (3 cycles per turn, +-1.5 deg). The dayside is
dimmer: the band is scaled from 0.3 near noon to 1 toward midnight.

**Brightness from activity.** The whole band is multiplied by \( 0.4 + 0.2\,\text{Kp} \), clamped to
0.25-2.6: 1 at Kp 3, dim on a quiet night, more than twice as bright in a great storm.

**Discrete and diffuse light.** `auroraDiscreteShare()` splits the light between the [sheets](#sheets) and the
volume: the poleward part of the oval (the first 30% of its width) gives the sheets their full share, and over
the equatorward part (past 75% of the width) their share falls to a quarter. The diffuse equatorward aurora is
therefore mostly the soft volume.

**Coverage** (`auroraCoverage()`): a warped Perlin over azimuth and colatitude, thresholded from 0.2 (quiet)
toward a storm value, so a quiet night shows patches and a storm fills in. The storm value is -0.6 with arc
systems off (the oval fills completely) and rises to -0.1 as "Arc systems" goes to 1: with [arc systems](#arc-systems) carrying the
storm's structure, coverage alone must not fill the oval into a uniform ring. Its frequencies and drift are the
"Coverage" sliders. The storm level it reads is derived from Kp (below), plus 0.6 x a substorm's local
intensity.

**Early out.** The CPU also passes the largest colatitude the oval can light this frame (the midnight
equatorward edge plus a substorm's equatorward shift, the larger of the soft edge with the ripple and the
[storm red glow](#storm-aurora)'s reach plus 2 deg, and 1 deg of margin, `cloud.auroraOval2.y`). Samples beyond it
return at once, and so do rays whose path stays more than 10 deg beyond it (see [The march](#the-march)).

!!! warning "Invariant"
    `auroraOvalWeightCpu()` in `src/simulations/SpaceWeather.cpp` is a hand-kept CPU mirror of
    `auroraOvalGeom()` + `auroraOvalBand()` (without the ripple); the ambience's `aurora` driver reads it. Change
    the shader's shape and the mirror together, or the sound and the picture disagree about where the oval is.

### Space weather

The activity index is a Kp equivalent (0 to about 11 for a Carrington-class event), computed on the CPU each
frame by `spaceWeatherAt()` (`src/simulations/SpaceWeather.cpp`). It is a **pure function of absolute sim time**
(seconds since J2000), with every random draw a hash of a time bin, like the orbits and the Reflect lock windows.
Time reversal, time warp and a bookmark therefore see the same storm at the same instant.

Kp combines three drivers through a smooth maximum (the strongest dominates, a second adds a little):
\( \text{Kp} = a + 0.2\ln\sum_i e^{k_i - a} \), \( a = \max_i k_i \), clamped to 0-11.5.

**The solar cycle.** An 11-year envelope from the cycle 25 minimum (December 2019), \( 0.15 + 0.85\sin^2(\pi p^{0.75}) \)
with \( p \) the phase in the cycle: it peaks about 40% of the way through and declines more slowly. It scales the
background, the coronal holes' odds and the CME rate.

**The background**: \( 0.5 + 1.6\,\text{cycle} \times \text{noise} \), wandering over half a day, plus +-0.3 of
three-hourly jitter: Kp 0.5-2.5.

**Recurrent coronal-hole streams.** Two hole slots, each at a fixed phase of the 27.27-day solar rotation. In
each 6-rotation epoch a slot holds a hole with probability \( \min(0.9, (0.4 + 0.4\,\text{cycle})\sqrt{r}) \)
(\( r \) = "Storm frequency"). A live hole's high-speed stream arrives once per rotation, rises over 0.6 day to a
peak of Kp about 2.8-7.2 (scaled by the square root of the equinox factor) and decays with a 2.2-day e-folding
time: moderate storms that recur every 27 days.

**CME storms.** Each day has one chance of a CME arrival, with probability \( 0.15 \times \text{cycle} \times
\text{equinox} \times r \). The **equinox factor** \( 1 + 0.3\cos(4\pi(d - 80)/365.24) \), \( d \) the day of the
year, is the Russell-McPherron effect: the solar wind's southward field couples best near the equinoxes. A storm's
peak Kp is drawn from a survival curve through these knots (log-linear between them):

| Peak Kp at least | 4.3 | 6 | 7 | 8 | 9 | 10 | 11.3 |
|---|---|---|---|---|---|---|---|
| Share of CMEs | 1 | 0.40 | 0.17 | 0.075 | 0.004 | 0.0006 | 0.00006 |

Calibrated with the cycle and rate above against NOAA's per-cycle counts of three-hour intervals at each storm
level (G1 1700, G2 600, G3 200, G4 100, G5 4), the model gives about 936 / 501 / 174 / 103 / 9 per cycle.

A storm's profile: a **sudden commencement** (a jump to half the peak within about 40 minutes of arrival), a
**main phase** of 3-9 h, a hold of 2.5-13 h near the peak, then an exponential **recovery** with an e-folding time
of 0.5-1.3 day, longer for bigger storms. A +-0.35 flicker on a 1.5-h scale gives storm-time intermittency.

**Substorms.** Each 2.5-h bin of sim time holds a substorm with probability \( 0.35 + 0.6\,\text{ss}(1, 4,
\text{Kp}) \) (Kp of that bin; ss = smoothstep), so they come every few hours and nearly every bin in a storm.
The strongest one in progress drives the oval:

- **onset** between 22.1 and 23.5 MLT, at a random time in its bin;
- **expansion**: the light brightens within about 5 minutes; a **bulge** pushes the poleward edge
  \( (2 + 4.5\,s) \) deg toward the pole (\( s \) its strength, 0.35-1 times \( 0.5 + 0.12\,\text{Kp} \)), growing
  over about half an hour as its MLT half-width widens from 0.8 h to 3.2 h and its centre drifts 0.7 h west (the
  westward travelling surge);
- **recovery** from 30 minutes on, an e-folding time of 55-85 minutes, so a substorm is over in about two hours.

Inside the bulge (a Gaussian in MLT angle) the band brightens by \( 1 + 1.5\,b \), its equatorward edge moves
\( 0.8\,b \) deg equatorward and its coverage fills in, \( b \) the local intensity.

In manual mode the index is the fixed "Aurora Kp (manual)" value; substorms still come and go, at that activity.

**The scripted storm.** On top of the simulated history, one Carrington-class CME is scripted
(`spaceWeatherSetScriptedStorm()`): the CME of [The Sun's Fury](../simulation/kessler.md), with a peak of about
Kp 11. Its shape follows the procedural storms (a sudden commencement to 60 % within half an hour, a main phase of
about 3 h, about 10 h near the peak, a 20 h recovery) and it enters as a CME driver, so the aurora follows with no
code of its own.

**Storm level.** The fold chaos and coverage fill read a storm level derived from Kp,
\( \text{storm} = \text{clamp}((\text{Kp} - 1)/9, 0, 0.85) \) (`cloud.stormStrength`). It stops at 0.85
because at 1 the coverage gate fills the whole oval and a storm reads as a featureless ring from orbit.

The NOAA geomagnetic storm scale shown in the readout is G1 at Kp 5 up to G5 at Kp 9 (`geomagStormScale()`).

!!! note "AURORA NOW"
    The Atmosphere tab shows the current activity above the aurora sliders: **Activity** (Kp, its G scale or
    "active"/"quiet", and "(manual)" in manual mode), **Driver** (a CME storm and the hours since it arrived, a
    high-speed solar wind stream, the background with the solar cycle's level, or "fixed Kp"), **Substorm**
    (expansion or recovery, minutes since onset, and its MLT, or "none") and **Oval at midnight** (the magnetic
    latitude range of the midnight oval).

### The curtain volume

The volume lives between 95 km (`kAuroraShellInnerM`) and 300 km (`kAuroraShellOuterM`), with soft sigmoid
edges (7.5 km below, 15 km above). Per sample (`auroraCurtainSample()`):

- **Night gate** at the sample, as for airglow.
- **Column window**: each azimuth column has its own height range inside the shell, read from the noise bake,
  with 12 km soft edges, so curtains have varied bottoms and tops.
- **Folds** (`auroraCurtainNoise()`): curtain noise stretched to be fine across the azimuth (frequency 40 per
  radian, about 55 km cells, finer in a storm), coarser in colatitude (about 94 km), and long in altitude
  (about 167 km): many separate folds, each a long vertical streak. A live warped noise ("Fold shimmer rate")
  shifts the folds' phase over time.
- **Colour** by altitude: green at the base, toward red-magenta (0.65, 0.15, 0.45) at the top; it blends into the
  airglow colours at both shell edges, so the aurora meets the airglow layers without a seam.

### The noise bake

Sampling warped noise per step is what made the aurora costly, so the fold and column noise is baked once at
init by `shaders/aurora_noise.comp`: a 1024 x 16 x 256 RGBA8 volume (U = geomagnetic azimuth, wrapping;
V = altitude over 40-410 km; W = colatitude over 0-75 deg), periodic gradient noise wrapping in U.

| Channel | Holds |
|---|---|
| R | the curtain folds (256 cells per turn of azimuth) |
| G, B | the column window's two height bounds (64 cells per turn) |
| A | unused |

Sampler: linear, repeat in U, clamp in V and W. It is binding 16 of the sky set and binding 8 of the cloud march
set. The only live noise call left per sample is the shimmer phase.

### The march

`auroraMarchCS()` marches the padded shell from 40 km to 410 km. In a storm (Kp past 5, with any storm feature
switched on) the top rises with the storm level up to 700 km, so tall curtains, the red glow and giant rays fit;
a quiet night marches exactly the quiet shell. Every sheet's emission fades out over the top 35% of the march's
reach above 100 km (`gAurTopM`), so a tall curtain seen from orbit never ends on a hard ceiling where the lines of
sight leave the shell.

- **Legs.** The ray's path through the shell depends on where the eye is. From inside or above it, a ray that
  dips below the inner sphere has a **near leg** and a **far leg** (beyond the dip); both are marched
  (`auroraMarchSegment()`). Dropping the far leg halves the path near the horizon and the aurora looks cut off at
  a fixed altitude.
- **Terrain.** Both legs end at the scene depth, and a ray whose entry into the real shell is behind terrain
  skips the march.
- **Bounding test.** Five points along the path; if all lie more than 10 deg beyond the largest colatitude the
  oval can light this frame, the ray is skipped. Most of the sky away from the poles costs nothing.
- **Steps.** Adaptive: \( N = \text{clamp}(\lfloor L / 15\,\text{km} \rfloor, 4, 64) \) per leg, evenly spaced.
- **Jitter.** The start is jittered per pixel by interleaved gradient noise and per frame by the TAA's Halton
  phase (`gAurJitter`), so the step pattern averages out in the sky's temporal AA instead of banding.

### Sheets

Over the diffuse volume, thin emissive curtains: the crisp, folded sheets of a real display.

- **Index field** (`auroraSheetIndex()`): \( S = (\theta - \theta_c) / \text{spacing} + W \), where
  \( \theta_c \) is the middle of the oval at the sample's MLT (halfway between its two edges, so the sheets run
  along the oval's shape) and \( W \) is a sum of travelling waves in azimuth (3, 11, 29 and 71 cycles per turn).
  The sheets are the level sets \( S = n \) for integer \( n \), spaced "Sheet spacing (deg)" apart. Where \( W \)'s gradient across
  the oval passes 1, a sheet folds back on itself into curls; "Sheet folds" scales the 11 and 29 cycle waves.
- **Exact integration per step.** With \( S \) taken as linear in \( t \) over a march step, each sheet's Gaussian
  profile (width \( \sigma \) in index units) integrates in closed form:

  \[ L = \Delta t\,\sigma\sqrt{\pi/2}\;\frac{|\operatorname{erf}(b) - \operatorname{erf}(a)|}{|\Delta S|}, \qquad
     a, b = \frac{S_{0,1} - n}{\sigma\sqrt 2} \]

  (Winitzki's erf, error below 1.3e-4). Edge-on curtains are therefore crisp ribbons at any resolution with no
  sampling noise. Candidate sheets are those within 1.2 sigma of the step's range of \( S \), at most four per
  step, faded at the edge of that window; on an actual crossing, two regula-falsi steps refine the exact index.
  The index is carried across every sample, including those where the oval mask is zero, so sheets are not cut
  into whole-step chunks.
- **Crisp or fuzzy** per sheet and per region: \( \sigma \) from 0.28 (fuzzy) to 0.03 (crisp) index units,
  with "Crisp sheets (share)" setting the mix.
- **Light along a sheet** (`auroraSheetLight()`):
    - a sharp **lower border** near 100 km (+-8 km per sheet, rippled), below which the light dies within
      about 2.5 km, coloured pink (N2), turning green within about 3 km above it;
    - above the border, an exponential fall-off with a 35-70 km scale height, turning red 80-150 km above the
      border;
    - **rays**: field-aligned striations, a sharpened sinusoid in azimuth (170-260 cycles per turn) wavering
      slowly with time. From under the oval they converge toward the magnetic zenith;
    - a per-sheet brightness from 0.25 to 1.75.
- Each sheet's light is weighted by the oval mask, the night gate and the discrete share at its point (full in
  the poleward part of the oval, a quarter in the diffuse equatorward part).
- The sheets' total is weighted by "Curtain sheets" and added to the volume's.

### Arc systems

A real display is not every sheet alike: a few bright arc systems carry most of the light. `auroraArcWeight()`
groups the sheets five at a time (`kAurArcGroup`). About 70% of groups are arc systems, each bright over one or two
long stretches of the oval (a sinusoid in azimuth with 1-2 cycles per turn, thousands of km), with a per-group
strength; inside a stretch a sheet is about six times the mean. Adjacent groups blend smoothly across their
boundary. The diffuse volume follows the arc systems at 0.6 of their contrast, since the diffuse aurora is the glow
around them. "Arc systems" mixes from all sheets alike (0) to the full grouping (1); a saved value outside 0-1 is
clamped on load. With arc systems on, a storm's coverage stops short of filling the oval ([Coverage](#the-oval)).

### Storm aurora

Above Kp 5 the display changes character, ramped by \( \text{ss}(5, 8.5, \text{Kp}) \) (`auroraStormHi()`) and
scaled by "Storm features":

- **Tall sheets.** A share of the sheets, growing with the storm, have a 190 km scale height above their lower
  border (35-70 km otherwise), redden from about 35 km above it (80 km otherwise) and are more strongly rayed: the
  red pillars of a great storm.
- **Magenta borders.** Bright sheets get a deep magenta lower border about 9 km lower than usual (harder
  electrons reach about 90 km: N2 first positive and N2+ first negative, red over violet), and magenta between their
  green and their red top.
- **Sunlit tops.** Above 150-300 km, a sheet out of the Earth's shadow adds N2+ resonant scattering of sunlight,
  blue-violet. The shadow test is per crossing point, softened over 60-140 km of altitude.
- **Storm red glow** (`auroraStormRed()`, "Storm red glow"). The 630 nm glow of soft electrons at 290-380 km
  (higher toward its equatorward end), reaching up to 7 deg past the green aurora's equatorward edge, brightest
  toward midnight and patchy along the oval (from the baked column noise). It is dim per unit length and shows
  because a ray toward the horizon crosses hundreds of km of it: at Kp 9 a red glow across the northern sky from
  about 41 deg magnetic latitude, a faint red skirt from orbit. It is accumulated apart from the arc-system
  weighting.
- **Giant blue rays** ("Blue rays (chance)", ramped in over Kp 4-7). In about 220 km cells along each sheet, a cell
  with that chance becomes a stretch of the curtain about 100 km long whose own rays stand to 350-650 km, soft at
  both ends and fading at the top: blue where sunlit, the sheet's dim red in the Earth's shadow. They are rare, like
  red sprites.

### Visibility

- **Brightness**: the march's sum x `kAuroraScale` (1e-6) x "Aurora gain".
- **Light pollution**: the aurora is treated as an extended source of a magnitude per square arcsecond and dimmed
  against the sky background's (`darkSkySurfMag()`, `darkSkyVis()`). The background is **physical**
  (`darkSkySkyMagLinear()`): skyglow is light added to the natural sky, so
  \[ m_\text{sky} = 22 - 2.5\log_{10}\!\left(1 + \frac{d \cdot e(z)}{0.0241}\right) \]
  combined with the twilight term, where \(d\) is the [light-pollution dome](atmosphere-and-sky.md)'s linear city
  brightness in that direction (its \( d/(d + 0.08) \) compression and "Pollution gain" undone) and \(e(z)\) the
  dome's elevation term. A downtown core reads about 18 mag/arcsec² (Bortle 8-9), rural Arizona or Iowa farmland
  about 21.3, Fairbanks about 20. The aurora's own surface brightness runs at the physical 2.5 mag per decade, so
  a storm's bright arcs survive a suburban sky while its faint diffuse parts fade, and the gate is exactly 1 under
  a pristine sky. The Milky Way and zodiacal light keep their own, artistic gate (`darkSkySkyMag()`, a log ramp of
  the dome toward 13 mag/arcsec²), which is right for a fade but not as a brightness: on it a faint glow reads
  most of the way to a city.
- **Moonlight**: dimmed by up to 85% under a high, full Moon.
- **From orbit**: those two gates are for a sky seen through the air; they fade out between 40 and 100 km of eye
  altitude, so from space the aurora shows against the night side.
- **Extinction**: the line-of-sight extinction (`atmExtinctionMag()`, [Atmosphere and sky](atmosphere-and-sky.md)).
  From below 95 km it is taken to infinity; from above, only to the aurora's inner sphere, since looking down the
  path to infinity runs into the ground.
- **Clouds**: from below 95 km, the aurora is hidden by the cloud transmittance in front of it (cubed).

The aurora puts no light on the ground or the sea surface: a real aurora lights the ground no more than a full
Moon does, and colourlessly to the eye; the sky's own light reaches the ground through the sky ambient.

### Other views

- **Environment probes and mirrors** (`envAurora()`): a 16-step midpoint march of the volume through the shell,
  with extinction, no sheets and no visibility gates.
- **In the sea**: a 6-step march of the volume along the reflected ray, with extinction, hidden behind reflected
  clouds and faded with the wave range ([The sea](sea.md#reflected-clouds)).

## Cost

The volume, the sheets and the red airglow run at half resolution. The noise bake and the bounding test keep the
aurora's cost small where there is none in view; the sheets add about 0.5 ms of cloud march in a storm view
(RTX 3070 Ti, harness); the storm features cost nothing measurable (a Kp 9 view over Coldfoot measures the same
with them off). The red airglow march (16 fixed steps) would cost about 1 ms at full resolution, which is why it runs at half.

## Settings

Atmosphere tab ("Airglow & zodiacal light" and "Aurora" sections); all under `clouds` in `settings.json`.

| UI label | Key | Default |
|---|---|---|
| Airglow gain | `airglow_gain` | 0.066 |
| Airglow green | `airglow_green_gain` | 0.053 |
| Airglow red | `airglow_red_gain` | 0.013 |
| Airglow sodium | `airglow_sodium_gain` | 0.08 |
| Airglow coverage | `airglow_coverage_gain` | 0.32 |
| Airglow polar boost (red) | `airglow_polar_gain` | 2.4 |
| Space weather (0 manual / 1 auto) | `aurora_activity_auto` | 1 |
| Aurora Kp (manual) | `aurora_kp_manual` | 3 (range 0-11) |
| Storm frequency (x) | `aurora_storm_rate` | 1 (range 0-5) |
| Aurora gain | `aurora_gain` | 0.1 |
| Coverage freq | `aurora_coverage_freq` | 0.58 |
| Coverage az freq | `aurora_coverage_az_freq` | 3.1 |
| Coverage drift | `aurora_coverage_drift_rate` | 0.0012 |
| Fold shimmer rate | `aurora_shimmer_rate` | 0 |
| Curtain sheets | `aurora_sheets` | 1.4 |
| Sheet spacing (deg) | `aurora_sheet_spacing_deg` | 0.34 |
| Crisp sheets (share) | `aurora_sheet_crisp` | 0.77 |
| Sheet folds | `aurora_sheet_fold` | 1.1 |
| Arc systems | `aurora_arc_systems` | 1 (range 0-1; 0 = all sheets alike) |
| Storm features (red / magenta) | `aurora_storm_features` | 1.2 |
| Storm red glow | `aurora_storm_red` | 1.1 |
| Blue rays (chance) | `aurora_blue_rays` | 0.004 |

"Space weather" switches between the simulated history (1) and a fixed index (0, "Aurora Kp (manual)").
"Storm frequency" multiplies the CME arrival rate and the coronal holes' odds; 0 leaves only the background.

Knockout bits: 32 the aurora march, 16 the red airglow march. The Low preset turns the aurora off; Planetarium and
Potato turn off both.

!!! tip "Testing"
    Test from a dark site under the oval in winter (for example Coldfoot, 67.25 N 150.18 W, in December). A
    city's skyglow gates the aurora out, so a site such as Fairbanks shows little. The harness command
    `aurora [next <kp>|substorm|kp <v>|auto]` reports the activity, jumps sim time to the peak of the next storm or
    just after the next substorm onset, or fixes Kp; `state` carries an `aurora` block (Kp, G scale, drivers, the
    oval's magnetic latitudes, the observer's MLAT and MLT) and a `light_dome` block (the 16 sectors, the gained
    values and the dark-sky dome). See `docs/HARNESS.md`.

## Where in the code

| File | What |
|---|---|
| `shaders/cloud_march.comp` | `auroraMarchCS()`, `auroraMarchSegment()`, `auroraCurtainSample()`, `auroraSheetIndex()`, `auroraSheetLight()`, `auroraArcWeight()`, `auroraStormRed()`, `airglowRedMarchCS()` |
| `shaders/include/darksky.glsl` | `darkSkySkyMagLinear()` (the aurora's physical sky background), `darkSkySurfMag()`, `darkSkyVis()` |
| `shaders/include/aurora_oval.glsl` | the oval's geometry: `auroraOvalGeom()`, `auroraOvalBand()`, `auroraDiscreteShare()` |
| `src/simulations/SpaceWeather.h/.cpp` | `spaceWeatherAt()`, `spaceWeatherKp()`, `auroraOvalBounds()`, `auroraGpuParams()` (the UBO block), `auroraOvalWeightCpu()` (the CPU mirror), `geomagStormScale()` |
| `shaders/aurora_noise.comp` | the noise bake |
| `shaders/sat_sky.frag` | green and sodium airglow, `auroraSampleAt()`, `envAurora()`, the sea reflection |
| `shaders/include/cloud_params.glsl` | the aurora and airglow UBO fields |
| `src/simulations/SatelliteSim.cpp` | `createAuroraNoisePipeline()`; the per-frame space weather, the derived storm level and the aurora UBO fields (`auroraMidnight`, `auroraOval`, `auroraSub`, `auroraOval2`) |
| `src/simulations/SatelliteSimUI.cpp` | the Atmosphere tab's aurora sliders and the AURORA NOW readout |
| `src/simulations/SatelliteSimHarness.cpp` | the harness `aurora` command |
