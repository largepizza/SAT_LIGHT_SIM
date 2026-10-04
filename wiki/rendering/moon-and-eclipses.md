# Moon and eclipses

How the Moon is drawn as a body at its true position, and how both kinds of eclipse are rendered: the Moon
darkened by the Earth's shadow, and the Sun hidden by the Moon with the Moon's shadow falling on the air, the
ground and the clouds. The ephemeris (the Moon's position, the Sun's distance, eclipse search) belongs to
[Sun, Moon and planets](../simulation/sun-moon-planets.md); this page starts from the values the CPU hands to
the shaders.

## What the CPU provides

`updatePositions()` computes the Moon's geocentric position and, from it, everything the shaders need. These
ride in the `CloudParams` UBO and the push constants:

| Value | Where | Meaning |
|---|---|---|
| `moonDirENU.xyz` | `SatDrawPC`, `PointDrawPC` | the **topocentric** direction from the observer (up to about 1° of parallax from the ground, far more from deep space) |
| `moonDirENU.w` | `SatDrawPC` | the Moon's **light relative to full**, from the phase law \( V(a) = V_0 + 0.026\,a + 4\times10^{-9} a^4 \) (a = phase angle, degrees). Every moonlit term (clouds, ground, sky, halo, star and satellite dimming) reads it. A quarter Moon gives about 0.09 of full, a 32 % crescent about 0.04. |
| `moonDirENU.w` | `PointDrawPC` | the drawn angular radius (rad), for the star cull |
| `cloud.moonCenter.xyz` | UBO | the Moon's centre from the Earth's centre, in the observer's ENU axes, km |
| `cloud.moonCenter.w` | UBO | the drawn angular radius (rad) |
| `cloud.moonMisc.x` | UBO | the share of the Sun's disc the observer sees past the Moon |
| `cloud.moonMisc.y` | UBO | 1 when a solar eclipse is possible this frame (the Moon within 2° of the Sun, geocentric) |
| `cloud.moonMisc.z` | UBO | 1 when a lunar eclipse is possible (the Moon within 2.5° of the antisolar point) |
| `cloud.moonMisc.w` | UBO | the Moon's distance, km |
| `cloud.taaJitter.w` | UBO | the Sun's **true** angular radius (0.2624° in July to 0.2711° in January) |

The Sun's true radius is what decides whether an eclipse is total or annular, so it is computed from the Sun's
distance rather than taken as a constant. Its name in the shaders is `kSunAngR`.

The two "possible" flags exist so that every per-pixel eclipse test is a uniform branch: outside an eclipse
season the eclipse code costs nothing but the branch.

## The Moon's disc

`sat_sky.frag` intersects each pixel's ray with a sphere of angular radius `cloud.moonCenter.w` around the
Moon's direction. On a hit:

- **Size.** The drawn radius is the true radius × "Moon size (x real)" (default 2.5: the true 0.5° is only a
  few pixels at a wide field of view). Within 1.5°-4° of the Sun the scale eases back to 1, so a solar eclipse's
  contacts, totality and annularity happen at their real geometry.
- **Orientation: tidally locked.** The texture frame's z axis points at the **Earth's centre**
  (`-moonCenter.xyz`), and its "up" is the celestial north pole in the observer's frame, so the face rotates
  with the parallactic angle as the observer moves across the Earth, and from deep space or behind the Moon the
  face turns away as it should. The surface normal is projected orthographically onto the face plane for the
  texture UV, rotated 180° to align the image's north with the lunar north pole.
- **The far side.** Seen only from space. The texture is the near side; mirrored through the face plane it
  would show the familiar maria, which the far side lacks, so it is flattened 70 % toward its mean luminance.
- **Shading.** Lambert on the Sun direction × the lit fraction, limb darkening \(0.35 + 0.65\sqrt{\mu}\),
  and earthshine \(0.0008\,\mu\,(1 - f)\), strongest at new Moon when the Earth seen from the Moon is full.
- **Air.** The disc is attenuated by the camera-side Rayleigh and Mie column, like a surface.
- **Occlusion.** Any terrain, sea or opaque cloud on the pixel's ray hides it.

**Glow.** After the tonemap the Moon adds a tight corona at the disc's edge and a wide diffuse halo, both scaled
by the air the ray crosses, so neither appears above the atmosphere. See
[Atmosphere and sky](atmosphere-and-sky.md#moonlight-moon-glow).

### Depth

The Moon writes its own distance into the unified depth when no nearer surface does (`cloud.moonMisc.w`), so:

- stars and planets behind it are hidden by the hardware depth test (they draw at infinity);
- satellites, all nearer than the Moon, draw in front of it.

`star_point.vert` additionally culls stars inside the Moon's drawn radius (`PointDrawPC::moonDirENU.w`) before
rasterising them, and the Milky Way and zodiacal light are gated off on the disc.

## Lunar eclipses

When `cloud.moonMisc.z` is set, every point of the lit disc is shaded with the Earth's shadow **seen from that
point**:

1. The point's position on the Moon, Earth-centred, km.
2. The Earth's disc from there: angular radius \(\arcsin(6446\,\text{km} / d)\). 6446 km is the Earth's radius
   enlarged by 1/85 for the atmosphere's contribution to the shadow (Danjon's rule).
3. The Sun's disc from there, including the Sun's 0.15° parallax between the Earth and the Moon.
4. The visible share of the Sun is one minus the overlap of the two discs (`discOverlapFrac()`), which gives the
   penumbra's gradual darkening and the umbra's edge with no special cases.
5. Inside the umbra, red light refracted into the shadow by the Earth's atmosphere,
   \( (1, 0.33, 0.12) \times 0.025 \), weaker toward the umbra's centre.

The rest of the scene follows from the same data: the Moon's light on everything else is `moonDirENU.w`, which
the CPU computes from the phase alone.

!!! note
    The light the Moon casts on the ground and clouds during a lunar eclipse is not dimmed by the eclipse;
    only the disc's own shading is.

## Solar eclipses

A solar eclipse is rendered in two parts: what the observer sees of the Sun, and the Moon's shadow on
everything the Sun lights.

### The Sun seen past the Moon

Near the Moon (centre separation below about 2°, eased between 0.02 and 0.035 rad) the Sun's disc changes:

- it takes its **true** radius (it is normally drawn about 1.8 times larger for visibility);
- pixels on the Moon's disc hide it;
- the glare lobes and the wide corona scale with `cloud.moonMisc.x`, the share of the Sun the observer sees;
- at totality (weight \((1 - \text{share})^6\)) the **solar corona** is drawn around the Moon's black disc.

The lens flare and the Sun's bloom seed (`FlareSourcePC::sunRefIntensity`) scale with the same share. The
satellites' soft brightness ceiling (`SatFlarePC::sunRefIntensity`) deliberately does not: it caps every
satellite, and zeroing it would zero them all.

### The Moon's shadow per point

`shaders/include/eclipse.glsl` gives the shadow at any point in space:

- **`eclipseSunVis(p, sunD, blurKm)`**: the share of the Sun's disc seen from p past the Moon. The covered share
  is the equal-disc lens area evaluated where the Moon-Sun separation lies between the umbra's edge
  \(|a_m - a_s|\) and first contact \(a_m + a_s\); this is exact for equal discs and within about 2 % of the Sun
  for the real radii. An annular eclipse leaves the ring \(1 - a_m^2/a_s^2\). `blurKm` > 0 widens both edges by
  that radius, which turns the direct visibility into the visibility of the sunlit air around the point.
- **`eclipseSkyLight(p, sunD, h)`**: the **sky light** at a point in the shadow. The visibility blurred over
  180 km (`kEclipseSkyBlurKm`, the sunlit air that lights a point's sky), coloured by the path that light takes:
  80 km sideways through the air at the point's height (`kEclipseSideKm`), plus the ozone shell crossed at a
  grazing angle (secant about 20). Low in the shadow it is deep orange, the 360° sunset of totality; high up it
  is pale; outside the shadow it is white. The ozone's Chappuis band removes green and yellow, which makes the
  shadow's zenith deep blue rather than teal.

`cloud_v2_march.comp`, `cloud_v2_far.comp` and `sat_sky.frag` all call these with the same geometry, so the
shadow falls identically on air, ground and cloud. A CPU mirror, `eclipseSunVisCpu()`, gives the observer's own
sky light for exposure.

### The sky in the Moon's shadow

In the atmosphere loop, every sunlit sample's attenuation becomes

\[
a \leftarrow a \left[ v_D + (1 - v_D)\,k\,S(p) \right],
\qquad v_D = \text{eclipseSunVis}(p, 0),\quad S = \text{eclipseSkyLight},\quad k = 2
\]

where \(k\) (`kEclipseMultiScatter`) stands in for second-order scattering from the sunlit air around the
shadow. Single scattering alone would leave the umbra black. The orange horizon of totality comes out of this
without any colour grading: the lit air beyond the umbra seen through the low air, plus the reddened sideways
light. Because the test is per sample, from space the air over the shadow is dark too, and the shadow on the
ground stays sharp. The sea's sky-reflection march applies the same expression.

### Which terms take the eclipse factor

Every term lit by the Sun must take it, or that part of the scene stays at full daylight through totality.

| Term | Where | Factor |
|---|---|---|
| sky in-scatter, per sample | `sat_sky.frag` atmosphere loop | direct + 2 × sky light |
| sky reflected in the sea | `sat_sky.frag` reflection march | direct + 2 × sky light |
| land, direct sun | `sat_sky.frag` (`sunDiscVis`) | `moonSunVisible()` = direct |
| sea, direct sun | `sat_sky.frag` (`directSun`) | direct |
| terrain sky light | `sat_sky.frag` (`skyAmbientTerrain`) | sky light |
| cloud key light, per sample | `cloud_v2_march.comp` | direct |
| cloud sky light (zenith cache) | `cloud_v2_march.comp` | sky light |
| fog and dust, Sun and sky | `cloud_v2_march.comp` fog/dust march | direct, sky light |
| far cloud layer | `cloud_v2_far.comp` | direct, sky light |
| Sun disc, glare, corona | `sat_sky.frag` | the observer's share |
| lens flare | `sat_sky.frag` | the observer's share |
| the Sun's bloom seed | `FlareSourcePC::sunRefIntensity` | the observer's share |

None of these change the cloud march's register count; it stays at 128 (see
[The cloud march](clouds/march.md)).

### Exposure during totality

`SatelliteSim::skyExposure()` multiplies its dayness by the sky light left at the observer
(`moonEclipseSkyObs`, the 180-km blurred visibility), so it moves linearly toward the night exposure: at the
umbra's few percent of sky light it is about twilight's. `skyExposure()` sets the exposure of the environment
probes and the mesh bloom threshold. The sky pass's own exposure ramp in `sat_sky.frag` uses the Sun's elevation
only; the point sources follow the global exposure (`globalExposureEV()`), which an eclipse does not change.

## The solar corona

`solarCoronaAt()` in `sat_sky.frag` gives the corona's radiance relative to the drawn Sun disc. It is evaluated
only near totality and within 9 solar radii of the Sun (`kCoronaMaxR`), and scaled by `kCoronaGain` = 0.4.

- **Field lines.** The fibres follow a **dipole's** field lines (\(\sin^2\theta / r\) constant) out to a source
  surface at 2.5 R (`kCoronaRss`), and are radial beyond it. Each point is mapped to its field line's footpoint
  angle on the limb, and the fibre pattern is the cirrus noise volume (`cloudNoiseTex`) read on a circle of
  footpoints: fine across the lines, stretched about 20 times along them. Polar plumes fan out and low-latitude
  lines arch toward the solar equator.
- **Symmetry.** Footpoints are mirrored about the solar equator, where the two hemispheres' lines meet. Taking
  them per hemisphere independently would make the fibres jump there and draw a straight seam out of the limb.
- **Streamers.** A coarse read of the same volume gives a few broad bundles along the same lines, and a narrow
  **stalk** along the solar equator beyond about 1.5 R where the lines from both poles meet.
- **Brightness.** Baumbach's K-corona profile: \(1.425\,r^{-7} + 2.565\,r^{-17}\) for the inner corona and
  \(0.0532\,r^{-2.2}\) for the outer, carried mostly by the streamers, faded to zero before the cut-off radius.
- **Colour.** Pearl white, slightly warmer at the limb.
- **Chromosphere and prominences.** An H-alpha pink rim about 0.015 R thick, and prominences up to about 0.1 R
  where a noise read exceeds a threshold. They show where the Moon's edge is narrower than the Sun's
  chromosphere (near second and third contact) or a prominence stands taller.
- **Axis.** The Sun's rotation axis is taken as the ecliptic pole projected on the sky (the real axis is 7° off).

## Settings

| UI label (tab) | `settings.json` key | Default |
|---|---|---|
| Moon size (x real) (Atmosphere) | `clouds.moon_size` | 2.5 |
| Moon gain (Terrain): the moonlight's brightness on terrain and clouds | `clouds.moon_gain` | 0.0053 |

The eclipse model has no settings; its constants (`kEclipseSkyBlurKm` 180, `kEclipseSideKm` 80,
`kEclipseMultiScatter` 2, `kCoronaGain` 0.4) are in the shaders. The harness command `eclipse solar|lunar` jumps
to the next eclipse and places the observer in it (see [Automation harness](../development/harness.md)).

## Where in the code

- `shaders/sat_sky.frag`: the "Moon disc" block, `discOverlapFrac()`, `moonSunVisible()`, `solarCoronaAt()`,
  the eclipse terms in the atmosphere loop and the Sun disc block.
- `shaders/include/eclipse.glsl`: `eclipseSunVis()`, `eclipseSkyLight()`.
- `shaders/cloud_v2_march.comp`, `shaders/cloud_v2_far.comp`: the cloud and fog terms.
- `shaders/star_point.vert`: the star cull behind the Moon.
- `src/simulations/SatelliteSim.cpp`: the Moon block of `updatePositions()`, `skyExposure()`, the `CloudParams`
  fill.
