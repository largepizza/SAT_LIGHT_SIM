# Attitude

How each satellite is oriented: rigid attitude groups with closed-form pointing laws, one-axis joints,
kinematic trees for articulated models, tumbling debris, and how the older enumerated attitude modes map
onto groups. How groups are written in JSON is on [Satellite models](../modding/satellite-models.md) and
[Constellations](../modding/constellations.md).

## Rigid attitude groups

A satellite is one to four **rigid groups** (`AttitudeGroup`, `kMaxAttitudeGroups` = 4). Every group
has a body frame; the satellite's surfaces are mounted on groups. The first group, and any group without
a parent, is a **root**; its orientation comes from a pointing law. A **child** is hinged to its parent
and turns about the hinge by a one-degree-of-freedom joint.

Everything is evaluated in closed form from the satellite's orbit state (position, along-track velocity,
nadir) and the Sun's direction at the current sim time. No angular velocity is integrated, so attitude
is reversible like the rest of the [simulation](index.md#reversibility-state-is-a-function-of-sim-time).

### Targets

A pointing law or a joint aims a body vector at one of these directions (`AttTarget`):

| Target | Direction in ECI |
|---|---|
| `nadir` | \( -\hat{\mathbf{r}} \) |
| `zenith` | \( \hat{\mathbf{r}} \) |
| `sun` / `anti_sun` | \( \pm\hat{\mathbf{s}} \) |
| `velocity` / `anti_velocity` | \( \pm\hat{\mathbf{v}} \) (along-track) |
| `orbit_normal` / `anti_orbit_normal` | \( \pm\,\widehat{\mathbf{r}\times\mathbf{v}} \) |
| `sun_reflect_nadir` | \( \widehat{\hat{\mathbf{s}} - \hat{\mathbf{r}}} \): the mirror normal that reflects sunlight straight down |
| `sun_reflect_ground_site` | the mirror normal that reflects sunlight onto the satellite's chosen ground site (see [Reflectors and beams](reflectors.md)) |

The Sun direction is geocentric. Seen from a satellite it differs by at most the orbit radius over 1 AU
(about \( 5 \times 10^{-5} \) rad in LEO), which is negligible.

### The two-vector law (TRIAD)

A root's orientation is set by two body axes and two targets: the **primary** body axis
\( \mathbf{b}_1 \) points exactly at the primary target \( \mathbf{t}_1 \), and the **secondary** body
axis \( \mathbf{b}_2 \) points as close as it can to the secondary target \( \mathbf{t}_2 \). This is the
align/constrain scheme of STK and GMAT, solved with the TRIAD construction. Build an orthonormal triad in
each frame:

\[
\begin{aligned}
\text{body:}\quad & \mathbf{e}_1 = \hat{\mathbf{b}}_1, &
\mathbf{e}_2 &= \widehat{\mathbf{b}_1 \times \mathbf{b}_2}, &
\mathbf{e}_3 &= \mathbf{e}_1 \times \mathbf{e}_2, \\
\text{world:}\quad & \mathbf{w}_1 = \hat{\mathbf{t}}_1, &
\mathbf{w}_2 &= \widehat{\mathbf{t}_1 \times \mathbf{t}_2}, &
\mathbf{w}_3 &= \mathbf{w}_1 \times \mathbf{w}_2,
\end{aligned}
\]

and the rotation from body to world is

\[
R = W B^\top, \qquad B = [\mathbf{e}_1\ \mathbf{e}_2\ \mathbf{e}_3],\quad W = [\mathbf{w}_1\ \mathbf{w}_2\ \mathbf{w}_3].
\]

When the two targets are parallel (for example the Sun exactly along nadir) the roll about
\( \mathbf{t}_1 \) is undefined for that instant; the code picks any perpendicular (crossing with +Z, or
+X near the poles). That instant is a discontinuity in roll and lasts one frame at most.

The default root is primary \( +Z \to \) nadir, secondary \( +X \to \) velocity: a nadir-pointing,
velocity-aligned bus.

### The tumble law

Uncontrolled objects use the `tumble` law instead. Each satellite carries its own spin axis
\( \hat{\mathbf{a}} \), rate \( \omega \) and phase \( \phi_0 \) (drawn at random by the
[RandomShell](orbits.md#randomshell) distribution). With \( \hat{\mathbf{p}}, \hat{\mathbf{q}} \) a fixed
orthonormal pair perpendicular to the axis, body +X sweeps

\[
\mathbf{w}_1 = \cos\phi\,\hat{\mathbf{p}} + \sin\phi\,\hat{\mathbf{q}}, \qquad \phi = \phi_0 + \omega t,
\]

and the frame is \( [\mathbf{w}_1,\ \hat{\mathbf{a}} \times \mathbf{w}_1,\ \hat{\mathbf{a}}] \). The axis
is fixed in inertial space (torque-free spin about a principal axis); there is no nutation.

## One-axis joints

Any group may carry one joint: a rotation by \( \theta \) about a body axis \( \hat{\mathbf{k}} \) (for a
child, its hinge axis). Four modes:

| Mode | Angle |
|---|---|
| `track` | turns a body vector \( \mathbf{j} \) (perpendicular to \( \hat{\mathbf{k}} \)) as close as possible to a target: a solar-array gimbal |
| `edge_on` | turns \( \mathbf{j} \) perpendicular to the target, choosing the smaller of the two solutions: a knife-edge roll |
| `fixed` | a constant angle (`angle_deg`) |
| `flare_mitigation_tilt` | the global "Flare mitigate tilt" setting |

For `track` and `edge_on`, write the rotated vector against the target \( \hat{\mathbf{t}} \):

\[
\mathbf{j}(\theta)\cdot\hat{\mathbf{t}} = a\cos\theta + b\sin\theta, \qquad
a = \mathbf{j}\cdot\hat{\mathbf{t}},\quad b = (\hat{\mathbf{k}}\times\mathbf{j})\cdot\hat{\mathbf{t}}.
\]

`track` maximises it: \( \theta = \operatorname{atan2}(b, a) \). `edge_on` zeroes it:
\( \theta_1 = \operatorname{atan2}(-a, b) \) and \( \theta_2 = \theta_1 \pm \pi \), the one of smaller
magnitude. Both are then clamped to \( \pm \) the joint limit (`jointLimitDeg`, default 180). When an
authored \( \mathbf{j} \) is not exactly perpendicular to \( \hat{\mathbf{k}} \), its component along
the axis is removed first.

The rotation is applied with Rodrigues' formula to the whole frame, about the axis through the hinge
point:

\[
R' = R_{\hat{\mathbf{k}}}(\theta)\,R, \qquad \mathbf{t}' = R_{\hat{\mathbf{k}}}(\theta)\,(\mathbf{t} - \mathbf{p}) + \mathbf{p},
\]

where \( \mathbf{p} \) is the hinge point in world coordinates.

### Flare-mitigation tilt

`flare_mitigation_tilt` reads one global angle, the Photometry tab's "Flare mitigate tilt (deg)". On a
Sun-tracking group with the joint axis \( -Y \) (secondary target nadir), it pitches the Sun-facing normal
toward zenith, so the panel's normal makes exactly that angle with the Sun and its specular lobe moves
away from the ground. The array's power cost is \( 1 - \cos(\text{tilt}) \); the satellite info window
shows it.

## Kinematic trees and pivots

Geometry models use the group list as a tree. A child names its `parent` (which must come earlier in the
list) and a `hinge` (position and axis in the parent's body frame). At joint angle 0 the child's body frame
**is** its parent's; its joint then turns it about the hinge. A typical array: a root bus
(nadir/velocity), a child wing hinged on the bus's side with a `track` joint toward the Sun about the
wing's span.

Evaluation runs parents first. Each group's pose is a rigid transform from the model's rest frame (every
joint at zero) to the world:

\[
\mathbf{x}_\text{world} = R_g\,\mathbf{x}_\text{rest} + \mathbf{t}_g.
\]

### Per-component pivots

A component of a child group may set a `pivot` (relative to the group's hinge). The group's joint then
turns that component about the parallel axis through hinge + pivot, at the same angle. One joint thus
drives several parallel axes: the ISS model's four beta gimbals are one group with four pivots.
Orientations are unchanged, so every lobe normal and magnitude is unaffected; a posed **position** gains

\[
\Delta\mathbf{x} = (R_\text{parent} - R_\text{group})\,\mathbf{p}_\text{pivot},
\]

which every position consumer adds: occlusion samples and occluders, the reference ray-casts, OBJ export
and the mesh renderer (`satPivotOffset()` on the CPU, `pivotOffset()` in `sat_orbit.comp`).

## GPU form

On the GPU a group is `GpuAttGroup`, 64 bytes. To keep the shader's work to the minimum, **every body
vector of a tree is stored in the root's TRIAD coordinates**: a body vector \( \mathbf{v} \) becomes
\( (\mathbf{e}_1\cdot\mathbf{v},\ \mathbf{e}_2\cdot\mathbf{v},\ \mathbf{e}_3\cdot\mathbf{v}) \), computed
once per type on the CPU in double (`attTriadCoords()`). The shader then only builds the world triad
\( W \) from the two targets, and a body vector maps to world as \( W\,\mathbf{v}_T \). Lobe normals,
joint axes, joint vectors, occluder axes and sample points all use this convention.

`typeFrames()` builds up to four world frames, parents first, into four fixed registers and selects among
them with explicit comparisons (`pickFrame()`); a dynamically indexed local array would spill to memory.
Groups are read straight from the type buffer by index for the same reason. A child's translation is
rebuilt from the frames and each group's rest hinge point (`GpuSatType::originT`), mirroring the CPU's
pose translation.

`evalGroupPoses()` (`src/simulations/SatModel.cpp`) is the double-precision CPU mirror of
`groupFrame()` / `typeFrames()`, used by the photometric evaluator, traces, the mesh renderer and the
model viewer.

!!! warning "Invariant"
    `evalGroupPoses()` and `sat_orbit.comp`'s `groupFrame()` must stay in step, including the
    degenerate-roll fallback and the joint clamp. A divergence shows up as a GPU parity mismatch on any
    articulated model.

## Legacy attitude modes

Older satellite types declare each surface's attitude as an enumerated `AttitudeMode`. At load,
`resolveAttitude()` converts every legacy surface to a group (`legacyAttitudeGroup()`), deduplicating
identical groups, so the GPU only ever sees groups. Defaults below are primary \( +Z \to \) nadir,
secondary \( +X \to \) velocity, surface normal \( +Z \).

| Legacy mode | Group | Surface normal (body) |
|---|---|---|
| `NadirPointing` | default | \( +Z \) (toward nadir) |
| `AntiNadir` | default | \( -Z \) (toward zenith) |
| `SunTracking` | primary \( +Z \to \) Sun, secondary \( +X \to \) nadir | \( +Z \) |
| `SunPerp` | as `SunTracking` | \( +Y \): edge-on to the Sun, perpendicular to the Sun-nadir plane |
| `SunTrackingTilted` | as `SunTracking`, joint `flare_mitigation_tilt` about \( -Y \) | \( +Z \) |
| `FlatMirror45` | primary \( +Z \to \) `sun_reflect_nadir` | \( +Z \) |
| `TargetedReflector` | primary \( +Z \to \) `sun_reflect_ground_site` | \( +Z \) |
| `KnifeEdge` | default, joint `edge_on` about \( +X \) keeping \( +Z \perp \) Sun, limit 10 deg | \( +Z \) |
| `Tumbling` | `tumble` law | \( +X \) |
| `Perpendicular` (secondary only) | the primary's group | \( +Y \) |

`Perpendicular` meant \( \widehat{\mathbf{n}_0 \times \text{nadir}} \), which has a fixed body equivalent
only when the primary is Sun-tracking; for other primaries it is approximated as \( +Y \) of the
primary's group (and logged when its weight is non-zero). Every shipped roster uses it only with weight
0. For the other modes the conversion reproduces the old per-mode normals to float rounding.

A type whose groups fail validation (a bad parent reference, too many groups) falls back to a single
nadir-pointing group, with the reason in `satlight_log.txt`. Every launch writes the resolved types to
`satellite_types_resolved.json` in the user data folder, in explicit group form that can be pasted back.

## Settings

| UI label | `settings.json` key | Default |
|---|---|---|
| Flare mitigate tilt (deg) (Photometry tab) | `photometry.flare_mitigation_tilt_deg` | 0 |

## Where in the code

| File | Function / symbol |
|---|---|
| `src/simulations/SatModel.h/.cpp` | `AttitudeGroup`, `AttTarget`, `JointMode`, `GpuAttGroup`, `attTriadCoords()`, `evalGroupPoses()`, `satPivotOffset()` |
| `src/simulations/SatelliteSim.cpp` | `legacyAttitudeGroup()`, `resolveAttitude()`, `writeResolvedSatTypes()` |
| `shaders/sat_orbit.comp` | `attTargetDir()`, `groupFrame()`, `typeFrames()`, `pickFrame()`, `childOffset()`, `pivotOffset()` |
