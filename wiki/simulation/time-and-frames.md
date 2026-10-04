# Time and reference frames

How the simulator represents time, which coordinate frames it uses, where the observer's eye is, and how
it keeps float32 shaders accurate. Ephemerides of the Sun, Moon and planets are on
[Sun, Moon and planets](sun-moon-planets.md).

## Sim time

Sim time is **seconds since J2000.0**, treated as UTC. J2000.0 is taken as 2000-01-01 12:00:00 UTC, <!-- history-ok -->
which is Unix time 946 728 000.

It is stored in two parts on the CPU:

| Member | Type | Range |
|---|---|---|
| `simDayJ2000` | `int64_t` | whole days since J2000 |
| `simSecInDay` | `double` | seconds within the day, kept in \([0, 86400)\) |

Each frame adds \( \Delta t_\text{wall} \times s \times d \) to the seconds, where \( s \) is the time
scale (1x, 10x, 1 min/s, 5 min/s, 1 h/s, 1 d/s, 1 week/s, 1 month/s, 1 yr/s) and \( d = \pm 1 \) the
direction, then carries whole days in or out with a loop (so reversed time is handled the same way). The
absolute time \( t = 86400\,\text{simDayJ2000} + \text{simSecInDay} \) is only formed in double, where
its resolution is about \( 2 \times 10^{-7} \) s at the sim's epoch.

The sim starts at 2036-11-22 02:06:22 UTC, a California twilight chosen for the intro. <!-- history-ok -->

### Approximations

- **UTC = UT1 = TT.** The difference between UTC and UT1 (under 1 s) and between UT1 and TT (about
  70 s) is ignored. The almanac series for the Sun and the Moon are formally in TT; at their own
  accuracy (0.01 deg and 0.05 deg) the 70 s is negligible for the Sun and about 0.01 deg for the Moon.
- **No leap seconds.** Clock displays convert by adding 946 728 000 to get a Unix time.

## The Earth's rotation: GMST

The rotation of the Earth is the **Greenwich mean sidereal time**, the linear part of the IAU 1982
expression:

\[
\theta(t) = 280.46061837^\circ + 360.98564736629^\circ \times \frac{t}{86400\ \text{s}} \pmod{360^\circ}
\]

`earthRotationAngle()` in `src/simulations/SatPhotometry.cpp` evaluates it in double, splitting \( t \)
into whole days and seconds so the large product keeps its precision: whole days contribute
\( 0.98564736629^\circ \) each (the excess over a full turn), the seconds contribute
\( \omega_\oplus s \) with \( \omega_\oplus = 7.29211585531 \times 10^{-5} \) rad/s.

Because sim time is real UTC and the rotation is GMST, the Sun rises at real local times and real
eclipses fall on the right places. Nutation (the difference between mean and apparent sidereal time,
under 1.2 s of time) is ignored.

!!! warning "Invariant"
    Every ECI to ECEF rotation goes through `earthRotationAngle()`: the observer, the reflector target
    search (as the push constant `gmstNow`), the weather bake, the harness. Writing
    \( \omega_\oplus t \) anywhere instead silently moves that consumer's sky by about 18.7 h of hour
    angle, the GMST of J2000.

The GPU extrapolates GMST over short intervals as \( \theta + \omega_\oplus \delta \) (in
`sat_orbit.comp`'s lock-window search, \( |\delta| \) under two windows). That is exact, since GMST is
linear in time.

## Reference frames

| Frame | Origin | Axes | Used for |
|---|---|---|---|
| **ECI** | Earth's centre | X toward the mean equinox, Z toward the north pole | orbits, Sun, Moon, planets, satellite photometry |
| **ECEF** | Earth's centre | X toward 0 deg longitude on the equator, Z toward the north pole | the observer, ground targets, terrain, clouds |
| **ENU** | the observer | East, North, Up | everything drawn in the sky |
| **Satellite body** | per satellite | defined by its attitude groups | reflectance, see [Attitude](attitude.md) |

### ECI

The inertial frame is equatorial, with the equinox and obliquity **of date** for the Sun and the Moon
(both series use the mean longitude of date and the obliquity \( \varepsilon = 23.439^\circ -
4 \times 10^{-7\,\circ} \times \text{days} \)). Stars and the Milky Way use J2000 coordinates, and
planets use the J2000 ecliptic rotated by the obliquity of date. Precession between J2000 and the
2030s (about 0.5 deg) is therefore not applied to the star field; see
[Sun, Moon and planets](sun-moon-planets.md#precession).

### ECI to ECEF

A rotation about Z by \( \theta \):

\[
\mathbf{r}_\text{ECI} = R_z(\theta)\,\mathbf{r}_\text{ECEF}, \qquad
R_z(\theta) = \begin{pmatrix} \cos\theta & -\sin\theta & 0 \\ \sin\theta & \cos\theta & 0 \\ 0 & 0 & 1 \end{pmatrix}
\]

Polar motion is ignored.

### The Earth is a sphere

Geometry uses a sphere of radius \( R = 6371 \) km (the mean radius). Latitude is geocentric: the
observer's state is a unit ECEF direction `obsDir`, and latitude and longitude are read back as
\( \varphi = \arcsin z \), \( \lambda = \operatorname{atan2}(y, x) \). Geodetic and geocentric latitude
differ by up to 0.19 deg; a place entered by its geodetic coordinates is placed up to about 21 km from
its true position on the ellipsoid. Oblateness enters only through J2 in the sun-synchronous precession
(see [Orbits](orbits.md#sun-synchronous-orbits-and-terminator-alignment)).

### ENU

At the observer, with \( \alpha = \theta + \lambda \) the local sidereal angle:

\[
\hat{e} = (-\sin\alpha,\ \cos\alpha,\ 0), \quad
\hat{n} = (-\sin\varphi\cos\alpha,\ -\sin\varphi\sin\alpha,\ \cos\varphi), \quad
\hat{u} = (\cos\varphi\cos\alpha,\ \cos\varphi\sin\alpha,\ \sin\varphi)
\]

in ECI. `updatePositions()` rebuilds this basis every frame and passes it to the satellite shaders as
`enuX/Y/Z`; a satellite's sky direction is its observer-relative ECI vector projected onto it. Shaders
that work in ECEF build the same basis from the ECEF direction (`enuBasis()` in
`shaders/include/terrain.glsl`: east = \( \hat{z} \times \hat{u} \) normalised).

## The observer

The observer is a point above the sphere, described by its ECEF direction `obsDir` and a height.

### Eye radius

\[
r_\text{eye} = R + \max(h_\text{ground},\ h_\text{offset}) + 2\ \text{m}
\]

where \( h_\text{ground} \) is the terrain height under the observer and \( h_\text{offset} \) the
altitude the player chose (the height above sea level, raised with Q/E or the altitude control). The
2 m is the eye height above the ground. The CPU computes this in `obsEyeRadiusM()` and uses it for the
observer's ECI position and every CPU evaluator (the selection readout, traces, exports). The shaders
compute the same quantity with `observerEffHeight()` and `observerPos()` (`terrain.glsl`), so beams,
satellites and terrain are seen from the same eye.

!!! note
    The two ground heights come from different copies of the elevation model. The CPU reads a
    2160-pixel-wide copy of the DEM (about 18.5 km per pixel, nearest pixel); the GPU reads the full
    DEM, and the depth pass adds procedural terrain detail. They agree on flat ground and can differ by
    hundreds of metres in mountains. Above the terrain (\( h_\text{offset} > h_\text{ground} \)) both
    use the offset and agree exactly. See [Terrain](../rendering/terrain.md).

The height is capped at 100 000 km above sea level (`kMaxObsHeightM`).

### Observer position in ECI

\[
\mathbf{r}_\text{obs} = r_\text{eye}\, R_z(\theta)\,\widehat{\text{obsDir}}
\]

`observerEciAt()` computes it in double; the GPU receives it as a float vector.

### Follow mode

When the camera rides with a satellite, its position is held in double ECEF (`followObsEcef`), the
satellite's position plus an offset in the satellite's along-track / cross-track / radial frame. The
observer radius for that frame is the camera's distance from the Earth's centre. The photometric
readouts keep using the parked ground observer, not the camera.

## Precision

Float32 resolves about 0.4 m at Earth's radius, \( 2.4 \times 10^{-7} \) rad of angle, and 0.06 s at a
week. The simulation keeps every float quantity small or relative:

| Quantity | Strategy | Residual |
|---|---|---|
| Absolute time | `int64` days + `double` seconds on the CPU | none |
| Elapsed time on the GPU | orbits rebaked every 7 sim days; \( \Delta t \) sent as two floats | exact to the double |
| Orbital phase | two-float product in `orbitPhase()` | about 1 m median, 9 m max along-track ([details](orbits.md#position-error-budget)) |
| GMST, window phases | reduced modulo \( 2\pi \) or the window length in double, then narrowed | \( < 10^{-6} \) rad |
| Observer and satellite positions | ECI floats, observer-relative differences | about 0.5 m |
| Meshes, follow camera | posed in double on the CPU, sent camera-relative | sub-millimetre |
| Terrain detail, city and sea patterns | integer-anchored lattices relative to the observer's sea-level point | see [Terrain](../rendering/terrain.md) |

Two rules follow, and the rendering code relies on them:

- **Never form an absolute ECEF coordinate in float** for anything that needs metre accuracy. Use a
  difference from the eye, or an integer lattice cell plus an offset computed in double.
- **Never multiply a large time by a rate in float.** Reduce it modulo its period in double first (the
  cloud drift phase, the sea's wave period, the lock-window fraction all do this).

## Settings

| UI label | `settings.json` key | Default |
|---|---|---|
| Time scale (time bar) | `time.scale_idx` | 0 (1x) |

Observer latitude, longitude and height are persisted under `observer`.

## Where in the code

| File | Function / symbol |
|---|---|
| `src/simulations/SatPhotometry.cpp` | `earthRotationAngle()`, `observerEciAt()`, `sunDirEciAt()`, `moonGeoEciAt()` |
| `src/simulations/SatelliteSim.cpp` | time advance in `recordCompute()`, `updatePositions()` (ENU basis, Sun, Moon, planets) |
| `src/simulations/SatelliteSim.h` | `obsEyeRadiusM()`, `kMaxObsHeightM`, `kTimeScales` |
| `shaders/include/terrain.glsl` | `enuBasis()`, `observerEffHeight()`, `observerPos()` |
