# Reflectors and beams

Mirror satellites that reflect sunlight onto chosen places on the night side (the Reflect Orbital
concept): how ground targets are loaded, how each satellite picks a target and aims at it without any
stored state, the beam each one sends down, how clouds block it, where it lands, and how solar parks
under the beams respond. How the beams are drawn (shafts, cloud light, ground spots, glints) is on
[Weather, rain, lightning, fog](../rendering/clouds/weather.md) and
[Cities, farms and solar parks](../rendering/cities.md); the target file format is on
[Reflector targets](../modding/reflector-targets.md).

## Ground targets

Targets are read once at start from `reflector_targets.json` next to the executable (source:
`data/reflector_targets.json`). Each has a latitude, longitude, a name, an optional `capacity_mw` and a
`kind`:

| Kind | Meaning | Shipped |
|---|---|---|
| `solar` | a photovoltaic park; drawn on the terrain, its trackers follow the beams at night | 58 |
| `agriculture` | a farm-belt point | 2 |
| `daylight` | a high-latitude town given extra light | 7 |
| `none` | no purpose (the observer-spawn pin) | 1 |

A file without `kind` has it inferred from the name and capacity. Up to 201 targets are held
(`kNumReflectorTargets`, a buffer capacity). If the file is missing, unreadable or empty, random targets
are generated instead, with one fixed at the observer's default spawn point.

For each target the CPU stores its unit ECEF direction and a **ground radius**: the Earth's radius plus
the terrain height (the maximum over a 3×3 neighbourhood of the coarse CPU elevation model) plus a
75 m margin, which biases the aim point slightly above rather than below the ground. Both are uploaded
once (`reflectorTargetsECEFBuf`).

## Choosing a target: deterministic lock windows

A mirror satellite must pick one target among those it can serve, keep it long enough to be useful, and
move to another when it cannot serve it any more. The choice must also be a pure function of sim time, so
that time reversal and jumps give the same picture (see
[Reversibility](index.md#reversibility-state-is-a-function-of-sim-time)). The design: **fixed sim-time
windows** of length \( W \) (the "Target lock window", default 90 s). Within a window a satellite's target
is constant, and it is decided from the geometry at the window's **start**.

### Per-satellite phase

If every satellite's windows began at the same instants, the whole fleet would retarget at once, a
synchronised wave of motion. Each satellite \( i \) therefore offsets its windows by a hash of its index:

\[
f_i = \operatorname{fract}\!\left(\frac{t}{W} + \frac{h(i)}{2^{32}}\right),
\]

which is exactly the window fraction of the shifted time \( t + W h(i)/2^{32} \). The CPU computes
\( \operatorname{fract}(t/W) \) in double and the shader adds the offset, so no per-satellite state is
needed. \( h \) is a 32-bit integer mixing hash (`hashU()`).

### The window's start instant, exactly

The window began \( \delta = -f_i W \) seconds ago. The shader evaluates the satellite's position and the
Earth's rotation there by shifting its inputs:

\[
\Delta t_\text{start} = \Delta t + \delta, \qquad \theta_\text{start} = \theta_\text{now} + \omega_\oplus\,\delta.
\]

This is exact rather than approximate: in this model both the orbital phase and GMST are linear in time,
so evaluating at a shifted time is the same as having computed them for that instant. The only
approximation is that the Sun's direction is held constant over a window; it moves about
0.001 deg in 90 s. (Numerically, the shader evaluates these window-start positions with a plain float
phase product, good to a few hundred metres along-track, rather than the two-float product of
[the main orbit](orbits.md#the-orbital-phase-in-two-float-arithmetic); that matters only for a target
sitting exactly at the elevation threshold.)

### Eligibility and the winner

At an evaluation instant (`findWinner()`), every loaded target \( k \) is tested:

1. **Night side:** \( \hat{\mathbf{g}}_k\cdot\hat{\mathbf{s}} < 0 \), with \( \hat{\mathbf{g}}_k \) the
   target's ECI direction at that instant.
2. **Elevation:** the satellite stands at least \( e_\text{min} \) above the target's horizon,
   \( \hat{\mathbf{g}}_k\cdot\widehat{(\mathbf{r}_\text{sat} - \mathbf{g}_k)} \ge \sin e_\text{min} \)
   ("Min beam elevation", default 10 deg).

Among the eligible targets the satellite takes the one with the highest **pair score**

\[
\text{score}(i, k) = \frac{h\big(\,i \cdot 2654435769 \oplus h(k)\,\big)}{2^{32}},
\]

an all-integer hash of the (satellite, target) pair (\( \oplus \) is bitwise XOR, arithmetic mod
\( 2^{32} \)). The score does not depend on time or on which other targets are eligible, so a satellite's
preferences form a fixed ranking: it keeps its favourite target across windows for as long as that target
stays eligible, and changes only when it drops out (sunrise at the site, or the satellite setting below
\( e_\text{min} \)). Because the ranking is a hash, load spreads evenly over the targets a satellite can
see. Integer arithmetic keeps the score exact for any index; the CPU mirror reproduces the GPU's float
conversion so ties break identically.

When no target is eligible, the mirror pre-aims at the **nearest** night-side target with no elevation
test, or, with no night-side target at all, reflects the Sun straight down (`sun_reflect_nadir`). No beam
is emitted in either case.

## Aiming: a rate-limited ease in closed form

The mirror normal that sends sunlight from the satellite at \( \mathbf{r} \) to a ground point
\( \mathbf{g} \) bisects the Sun and the target directions:

\[
\hat{\mathbf{n}}(\mathbf{g}) = \widehat{\hat{\mathbf{s}} + \widehat{\mathbf{g} - \mathbf{r}}}.
\]

Within each window the mirror eases from where it was aimed when the window began toward the live ideal,
at no more than a maximum angular rate \( \dot\theta_\text{max} \) ("Mirror max slew rate", default
0.11 deg/s):

- **Start aim** \( \hat{\mathbf{n}}_0 \): the ideal toward the **previous** window's winner, evaluated at
  this window's start. One window of look-back, itself a pure function of time.
- **Destination at start** \( \hat{\mathbf{n}}_1 \): the ideal toward this window's winner at the same
  instant. The angle \( \Delta = \angle(\hat{\mathbf{n}}_0, \hat{\mathbf{n}}_1) \) is fixed for the window.
- **Duration** \( \tau = \operatorname{clamp}(\Delta / \dot\theta_\text{max},\ 0.001\,\text{s},\ W) \).
- **Aim now**: \( \hat{\mathbf{n}} = \widehat{\operatorname{mix}\big(\hat{\mathbf{n}}_0,\ \hat{\mathbf{n}}_\text{live},\ \operatorname{smoothstep}(0, \tau, f_i W)\big)} \),
  where \( \hat{\mathbf{n}}_\text{live} \) is the ideal toward this window's winner at its **current**
  position.

When the winner does not change (the common case), \( \Delta = 0 \) and the mirror simply tracks its
target for the whole window. A swing is capped at one window, so it can never run into the next
window's independently computed ease. The angle left between the eased and the live aim is reported per
beam as `aimErrorRad`; while it is non-zero the beam is still slewing and does not land on its site.

The attitude law `sun_reflect_ground_site` (see [Attitude](attitude.md#targets)) consumes this aim, so a
geometry model can mount its mirror on any group; legacy `TargetedReflector` types use it as their
primary surface. `satGroundSiteIdeal()` (`src/simulations/SatPhotometry.cpp`) is the double-precision CPU
mirror used by the selection readout, traces and the mesh renderer.

## The beam

Every mirror satellite with an eligible target and a lit mirror appends a beam to `reflectBeamsBuf`
(capacity 2048, `BEAM_MAX_ACTIVE`), recording the satellite and target positions relative to the
observer, the mirror's current reflected direction \( \hat{\mathbf{d}} =
\operatorname{reflect}(-\hat{\mathbf{s}}, \hat{\mathbf{n}}) \), the target index and the quantities below.

### Power

The power the mirror sends down, in the beam's own units:

\[
P = S\, A\, F_0\, (\hat{\mathbf{n}}\cdot\hat{\mathbf{s}})_+\, g,
\]

with \( S = 1361 \) W m⁻² the solar constant, \( A \) the mirror area, \( F_0 \) its reflectance and
\( g \) the "Beam gain" setting. For a geometry model, \( A \) and \( F_0 \) are the total area and
area-weighted Fresnel reflectance of the lobes facing along the site-aimed axis; a model with no such face
emits no beams.

!!! note
    The beam's power is not multiplied by the satellite's Earth-shadow factor: a mirror inside the
    Earth's shadow but still facing the Sun's direction keeps emitting. For terminator-aligned
    (dawn-dusk) mirror orbits the satellites are almost always sunlit, so this rarely matters.

### Footprint: an image of the Sun

The dominant spreading of a reflected beam is not diffraction (about \( 10^{-8} \) rad for a 50 m
mirror) but the size of the Sun: even a perfectly flat mirror reflects a cone of the Sun's angular
radius. The spot on the ground is therefore an image of the solar disk, of radius

\[
R_\text{spot} = r_m + L\,\tan\theta_\odot, \qquad r_m = \sqrt{A/\pi}, \quad \theta_\odot = 0.0046505\ \text{rad},
\]

with \( L \) the slant range from mirror to target and \( \theta_\odot \) the Sun's mean angular radius.
Its **mean irradiance** is the reflected power spread over the disk:

\[
\bar{E} = \frac{A\,F_0\,(\hat{\mathbf{n}}\cdot\hat{\mathbf{s}})_+}{\pi R_\text{spot}^2}\ \ \text{(in Suns)}.
\]

A 55 m square mirror at 600 km slant range makes a spot about 2.8 km in radius at about \( 10^{-4} \) of
sunlight per mirror (some 60 full Moons); a site served by dozens of mirrors at once receives the sum.
The irradiance does not fall with range for a fixed spot area, but the spot grows as \( L^2 \).

!!! note
    The renderer draws each spot and shaft with a soft-edged profile of the same total energy rather than
    the physical disk with a limb-darkened, 2%-wide edge: a real mirror's figure and the air blur the edge.
    See [Weather, rain, lightning, fog](../rendering/clouds/weather.md).

## Cloud occlusion of a beam

`beam_self_march.comp` runs right after the orbit pass, one thread per beam. It traces the beam's
**actual** reflected direction from the satellite to the sea-level sphere and marches the first stretch
of that segment inside the cloud shell through the same cloud field the renderer draws (`cv2Field`, the
mean-erosion version), with steps of about 200 m (at most 96):

\[
T = \prod_j e^{-\sigma_j \Delta s}.
\]

It writes two numbers per beam: the **opacity** \( 1 - T \), and the **block altitude**, the height
where \( T \) first falls below 0.5 (or, for a column that never gets that thick, the absorption-weighted
mean height). Everything that draws the beam's effect below the cloud multiplies by \( T \); everything
above the block altitude is unaffected. Marching the real direction rather than the direction to the
target matters while a mirror is slewing, when the two differ.

## Where the beam lands

The CPU reads the beam list back each frame and builds the ground spots. A beam lands where its reflected
ray meets the sphere of its **target's own ground radius** (minus the 75 m margin, never below sea
level), not the sea-level sphere: a beam arriving at 20 deg on a site 1.7 km up would otherwise land about
5 km beyond it. A spot's weight is

\[
w = P \cdot f_\text{range} \cdot f_\text{elev} \cdot T,
\]

where \( f_\text{range} \) fades the spot out between 900 and 1100 km from the observer to the site
("Beam max range", default 1100 km, faded over the last 200 km) and \( f_\text{elev} \) fades it below
5 deg of beam elevation. The 256 strongest spots (ranked by \( P \), a quantity that changes smoothly)
are kept.

The list is one frame old when read, so the CPU rebases it rigidly: positions move with both the
observer frame's rotation and the eye's own displacement over the frame; directions with the rotation
only.

## Solar parks under the beams

For the 8 nearest `solar` targets within 2500 km of the observer, the CPU lays out a PV park:

| Property | Value |
|---|---|
| area | `area_km2`, else 2.2 ha per MW of `capacity_mw` (at least 1 km²) |
| mount | single-axis north-south trackers in the Americas, India and Australia; fixed tilt elsewhere (`"mount"` overrides) |
| tracker rows | 5.8 m pitch, 2.4 m panels |
| fixed rows | 8 m pitch, 4 m tables, tilted toward the equator by 0.8 × latitude (10 to 35 deg) |

**Trackers turn toward the light.** About their north-south axis, a tracker faces the Sun while it is
more than about 1.7 deg up at the site; otherwise the strongest beam landing within the park; otherwise it
stows flat. The angle is clamped to \( \pm 55 \) deg and eased with an 8 s time constant, so a beam swap
turns the rows rather than snapping them. (This easing is over wall-clock time and is not reversible.)
Each beam's glint off the panels, seen from other directions, is drawn by the terrain shader; see
[Cities, farms and solar parks](../rendering/cities.md).

## Settings

| UI label (Beams tab) | `settings.json` key | Default |
|---|---|---|
| Target lock window (s) | `clouds.reflector_lock_window_s` | 90 |
| Mirror max slew rate (deg/s) | `clouds.mirror_max_rate_deg_per_sec` | 0.11 |
| Min beam elevation (deg) | `clouds.reflector_min_elev_deg` | 10 |
| Beam gain | `clouds.beam_gain` | 0.0088 |
| Beam max range (m) | `clouds.beam_max_range_m` | 1 100 000 |
| (no UI) solar parks drawn | `clouds.solar_arrays` | on |

## Where in the code

| File | Function / symbol |
|---|---|
| `src/simulations/SatelliteSim.cpp` | `loadReflectorTargets()`, `computeReflectorTargetElevationRadius()`, `fillSolarSites()`, the beam readback in `recordCompute()` |
| `shaders/sat_orbit.comp` | `hashU()`, `pairScore()`, `findWinner()`, `nearFallbackIdeal()`, `idealTowards()`, the ground-site block of `processSatellite()` |
| `src/simulations/SatPhotometry.cpp` | `satGroundSiteIdeal()` (CPU mirror) |
| `shaders/beam_self_march.comp` | per-beam cloud transmittance |
| `shaders/include/reflect_beam.glsl` | `ReflectBeam`, `BEAM_MAX_ACTIVE` |
| `data/reflector_targets.json` | the shipped target list |
