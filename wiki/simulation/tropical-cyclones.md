# Tropical cyclones

Hurricanes, typhoons and tropical cyclones are part of the [storm catalog](weather.md): seasons of named storms in
six basins, each with an hourly track and intensity fixed at genesis. This page covers the model: the basins and
their seasons, how a track is integrated, intensity and naming, what a cyclone's state at an instant carries
(its eyewall, central dense overcast and spiral bands as data), its wind field, and the storm surge. How a cyclone
is drawn in the volumetric clouds is on [Storms in the clouds](../rendering/clouds/storms.md); the sea it raises
is on [The sea](../rendering/sea.md#sea-state); the Weather window's Cyclones tab is on
[The Weather window](../using/weather.md).

## A season is a pure function of its year

Like every storm, a cyclone is fixed at birth (`wx::Cyclone`), and a whole season is a pure function of (basin,
year, settings) (`Catalog::Impl::buildSeason()`): the number of storms, their genesis dates and points, their
tracks and intensities, and their names. Seasons are built on first need and cached by year; `cyclonesAt(t)`
looks at the previous, current and next year's seasons and keeps those within two years of *t*. A new year after
a jump builds up to 18 seasons at once on the catalog's thread pool.

Unlike ordinary storms, cyclones live in the **Earth-fixed** frame: a cyclone moves west in the trades and
recurves poleward and east, while the cloud map drifts east everywhere. Each frame the state is converted to the
map frame by the inverse of the drift rotation, so the weather bake and the cloud field see it where it is.

The year index counts years of 365.2422 days from J2000 noon, so a season's boundary falls on a different hour
of 31 December each calendar year.

## Basins

| Basin | Genesis box (centre ± half-width) | Season peak (day) | Spread (days) | Mean count a year |
|---|---|---|---|---|
| North Atlantic | 15° ± 7° N, 52° ± 26° W | 250 | 33 | 14 |
| Eastern Pacific | 13° ± 4° N, 106° ± 12° W | 222 | 38 | 16 |
| Western Pacific | 14° ± 6° N, 140° ± 18° E | 240 | 55 | 25 |
| North Indian Ocean | 13° ± 4° N, 87° ± 9° E | 140, and 315 (60% of storms) | 22 / 25 | 5 |
| South-West Indian Ocean | 13° ± 5° S, 68° ± 20° E | 45 | 42 | 10 |
| Australian region / South Pacific | 14° ± 4° S, 152° ± 30° E | 50 | 45 | 11 |

About 81 a year in all, × "Tropical cyclones (x)". The count is a Poisson draw; each storm's genesis day is a
Gaussian about its basin's peak (the North Indian Ocean has two seasons, before and after the monsoon), and its
genesis point is uniform in the box, redrawn (from a generator of its own, so no other draw changes) until it is
not over land.

## Tracks

A track is integrated **hourly** from genesis (`Cyclone::kStepS`) for at most 18 days.

### Routes

Each basin has 3-5 climatological **route families** (`kRoutes*` in `Weather.cpp`), polylines after the
best-track climatology:

| Basin | Routes |
|---|---|
| North Atlantic | Cape Verde storms recurving; Caribbean into Mexico; Gulf coast landfalls; Florida and the East Coast; early recurvers |
| Eastern Pacific | out to sea; up the Baja coast; recurving into Sinaloa; toward Hawaii |
| Western Pacific | through the Philippines into South China; Taiwan into East China; recurving to Japan; early recurvers |
| North Indian Ocean | Odisha / Andhra Pradesh; Bangladesh / Myanmar; Tamil Nadu |
| South-West Indian Ocean | Madagascar and Mozambique; the Mascarenes, recurving; mid-ocean |
| Australian region / South Pacific | the Pilbara; onto Queensland; the Coral Sea southward; Fiji and Tonga south-east |

A storm picks a route weighted by the family's share × \( e^{-(d/1200\,\text{km})^2} \), where \( d \) is the
genesis point's distance from the route, so storms of one family run roughly parallel and the families differ.

### Steering

Each hour the storm aims at the point 500 km ahead along its route (pure pursuit), offset sideways by 35-75% of
the lateral offset it started with. On that heading it adds:

- two slow heading waves (±4-9° over 1.5-3.5 days and ±3-8° over 3-6.5 days),
- a 4° trochoidal wobble over 1.2-2.4 days,
- a speed of 4.5-7 m/s in the trades, rising to 8-14 m/s past its own recurvature latitude (drawn per storm from
  the basin's range, 18-40° by basin), ±18% over 2-5 days,
- for 10% of storms a **stall**: a day or two at about a fifth of its speed, 1.5-6 days after genesis.

### Intensity

The maximum sustained wind \( v \) starts at 12 m/s and relaxes each hour toward a target:

- Over warm water the target is the storm's own **potential** \( v_{\text{pot}} = 22 + 60\,u^{2.2} \) m/s
  (\( u \) uniform: median about 35 m/s, about a quarter of named storms major hurricanes), × a cold-water fade
  that falls from 4° equatorward to 8° poleward of the basin's cold latitude (22-30°). In the Eastern Pacific the
  water also cools westward from 122° W, so storms running out to sea die before Hawaii.
- Over land the target is 8 m/s.
- The time constant is 30 h while strengthening, 20 h while weakening at sea, 9 h over land.

The track ends when the storm has peaked (reached 90% of its potential, or three days old) and fallen below
11 m/s, when it passes 50° latitude, or after 48 h over land (a remnant two days inland). Hour by hour the track
records the position, \( v \), whether it is over land, and the **turn**: the trapezoid integral of
\( v / r_{\text{mw}} \), the angle the circulation has turned since genesis. Using that integral rather than
rate × age keeps the rotation from jumping back and forth as the storm strengthens and weakens.

### Categories and pressure

| Category | 1-min wind | Term |
|---|---|---|
| depression | under 17.5 m/s | Tropical depression |
| tropical storm | 17.5-33 m/s | Tropical storm (Cyclonic storm in the North Indian Ocean) |
| 1 | 33-43 m/s | hurricane / typhoon / cyclone by basin |
| 2 | 43-50 m/s | |
| 3 | 50-58 m/s | major hurricane, severe cyclone |
| 4 | 58-70 m/s | super typhoon in the Western Pacific |
| 5 | 70 m/s and up | |

The central pressure follows Atkinson and Holliday: \( p = 1010 - (v_{\text{kt}} / 6.7)^{1/0.644} \) hPa
(`cycloneCategoryName()` gives the basin's term).

### Names

Storms are named from the basin's WMO list in the order they reach tropical-storm strength (17.5 m/s); a depression
that never does stays unnamed. The North Atlantic and Eastern Pacific lists restart each year (the 2036 lists are <!-- history-ok -->
the 2024 six-year rotation with the 2025 retirements replaced); the other basins' lists run on from year to year, <!-- history-ok -->
offset by about the basin's mean count a year.

## The state at an instant

`cyclonesAt(t)` returns each live cyclone's `wx::CycloneState`, strongest first:

| Field | Model |
|---|---|
| centre | the track interpolated between hours, Earth-fixed and in the map frame |
| motion | the track's change over the next 30 min |
| radius of maximum wind | \( r_{\text{mw}} = r_0 (1 - 0.5\,\mathrm{smoothstep}(30, 70, v)) \), \( r_0 \) = 35-70 km drawn at genesis: it contracts as the storm intensifies |
| eye radius | \( 0.45\,r_{\text{mw}} \), opening between 33 and 45 m/s |
| central dense overcast (CDO) radius | 120-300 km by the storm's size, growing in between 14 and 40 m/s |
| outer bands | 350-800 km by size, 0.4 of that for a weak storm |
| spiral arms | \( 2 + 3\,\mathrm{smoothstep}(28, 65, v) \pm 0.35 \): 2 for a tropical storm, up to about 5 at Category 5 |
| arm pitch | \( 30^\circ - 8^\circ \) × intensity (intensity = smoothstep(17.5, 75, v)) |
| band rotation | 0.04 × the turn |

### The eyewall, CDO and bands as data

What the catalog hands the renderer:

- **Towers** (`cycloneTowersAt()`, storms over 20 m/s): an **eyewall** of 18 regenerating towers on a ring at
  0.85-1.15 \( r_{\text{mw}} \), turning cyclonically at a quarter of the circulation's turn, each pulsing over
  40-65 min with a strength of 0.55-1 by intensity; and 6 **inner rainband** towers on the crests of the spiral
  arms, 2 to 5 \( r_{\text{mw}} \) out. Their heads blow outward from the centre (the outflow aloft). They do
  not flare: the CDO is the anvil.
- **Shields** (`cycloneShieldsAt()`): the **CDO** as a round shield with an eye (strength rising from 15 to
  35 m/s); a thin, aged **outflow cirrus** sheet to max(1.6 × CDO, 0.6 × outer bands) at a quarter strength; and
  **arm cirrus**, a chain of 4 thin capsules along each arm's crest from inside the CDO's edge to the outer bands,
  a quarter of the arms' spacing wide, weaker outward (× "Hurricane arm cirrus").
- **The weather bake** draws the spiral bands themselves (logarithmic spirals trailing the rotation, crest at
  \( \theta = h\,(2\pi m/n - \ln(r/r_{\text{mw}}) / \tan\psi - \theta_{\text{rot}}) \) for arm *m* of *n*), the
  eyewall ring, an overcast raining deck under the CDO (heaviest near \( r_{\text{mw}} \), light stratiform rain
  at its edge) and a differential rotation of the map's own cloud about the centre, so the surrounding cloud
  trails round it in spirals. The details are on [Storms in the clouds](../rendering/clouds/storms.md).

## Wind field

A cyclone's wind adds to the [surface wind at the eye](weather.md#the-surface-wind-at-the-eye) as a Holland-like
profile: rising linearly from the centre to \( r_{\text{mw}} \), falling as \( (r_{\text{mw}}/r)^{0.6} \) beyond,
gone past 1.4 × the outer bands; cyclonic (counter-clockwise in the north), turned 20° inward, × 0.7 at the eye's
height (the catalog's \( v \) is the 10-m wind), calm in the eye. It moves the rain and snow, sets the gusts' storm
share, and drives the open-sea surf ambience. The two cyclones nearest the observer also set the **sea state**: the
waves take max(their own state, \( (U / 9\,\text{m/s})^{1.3} \)), at most 5 times the base sea, on the same
profile, with whitecaps and surf on a coast the storm makes landfall on (see [The sea](../rendering/sea.md#sea-state)).

## Storm surge

A cyclone raises the sea on the coast to the right of its track in the northern hemisphere, to the left in the
southern (`surgeParamsOf()`, GPU `tdSurgeAt()`).

**Peak rise.** The sea needs hours to pile up and drain, so the rise follows the wind of the last 6 h:
\( v_e = \max(v(t), 0.85\,v(t-3\,\text{h}), 0.7\,v(t-6\,\text{h})) \), and

\[
H = 1.2 \left(\frac{v_e - 17}{16}\right)^{1.4} \left(\frac{r_{\text{mw}}}{40\,\text{km}}\right)^{0.3}\ \text{m}
\]

(the radius term clamped to 0.5-2 of 40 km), at most 9 m, × "Storm surge (x)". That is about 1.2 m at Category 1
(33 m/s), 3.3 m at Category 3 (50 m/s) and 6.4 m at 70 m/s; a wide storm pushes more water.

**Field.** In the storm's frame, in units of \( r_{\text{mw}} \), with *a* along the motion and *x* toward the
surge side:

\[
S = H \exp\!\left(-\tfrac12\left[\frac{(x-1)^2}{\sigma_x^2} + \frac{a^2}{\sigma_a^2}\right]\right),\quad
\sigma_x = \begin{cases}1.8 & x > 1\\ 0.9 & x \le 1\end{cases},\quad
\sigma_a = \begin{cases}4 & a > 0\\ 2.5 & a \le 0\end{cases}
\]

out to 10 \( r_{\text{mw}} \). So a coast sees the sea rise over the half day before landfall and drain over the
following eight hours: Nadine (2036, at 29.4° N 90.3° W) gives 0.3 m 12 h before the peak, 1.6 m at 8 h, 4.3 m at <!-- history-ok -->
landfall and 0.9 m 8 h after. A stalled storm takes its motion over the last 6 h.

**Shelf.** The model has no bathymetry; the land's flatness stands for a shallow shelf. The rise is × 1 where the
DEM's mean height over about 85 km (its mip 5) is under 40 m, falling to 0.45 by 500 m.

**What floods.** Only the sea rises (the sea and water at sea level), and only near the coast: the rise tapers to 0
between 0.45 and 0.95 of the water map's reach offshore, so the open ocean is unchanged. The shoreline moves inland
by the rise over the coastal plain's slope (the mean height / 40 km, 0.25-20 m/km), at most 0.7 of the water map's
reach (about 14 km), and only through the DEM's lowest land class (under 30-60 m): the 8-bit DEM (about 37 m
steps) cannot tell 2 m from 30 m. The height function, both depth passes and the beaches all see the moved shore.
Over flooded land the waves are depth-limited and the water muddy; no beach is drawn on the flood line. See
[The sea](../rendering/sea.md) and [Terrain](../rendering/terrain.md).

The surge costs nothing away from a cyclone and about 0.2 ms of scene depth over a large flood at Medium (RTX 3070
Ti, 1600x900). It is not drawn in the SKY_LITE or SKY_ENV sky variants.

## Settings

| UI label | settings.json key | Default | Effect |
|---|---|---|---|
| Tropical cyclones (x) | `clouds_v2.storm_cyclones` | 1 | × each basin's count a season |
| Hurricane arm cirrus | `clouds_v2.storm_arm_cirrus` | 1 | × the cirrus along the arms (0 = none) |
| Storm surge (x) | `clouds_v2.storm_surge` | 1 | × the surge (0-2); in the Weather window's Controls |

"Storm objects" off removes cyclones too.

## Harness

`storms cyclones`, `storms season basin=<0-5> year= [track=1]` (6-hourly lat, lon, vmax, land),
`storms go cyclone=<name|id|strongest> [dist_km=] [alt=]`; `state` → `weather.surge_m` (the rise at the eye) and
`weather.surge` {field_m, shelf, peak_m, flood_reach_m, shore_dist_m, dem_m, flooded}. See
[Automation harness](../development/harness.md).

## Where in the code

- `src/simulations/Weather.cpp`: `kBasins`, `kRoutes*`, the name lists, `buildSeason()`, `cyclonesAt()`,
  `cycloneTowersAt()`, `cycloneShieldsAt()`, `cycloneCategoryName()`.
- `src/simulations/SatelliteSimWeather.cpp`: `surgeParamsOf()`, `surgeAtDir()` (the CPU mirror),
  `weatherFillSeaParams()` (the two nearest cyclones into the UBO).
- `src/simulations/SatelliteSimRain.cpp`: the cyclone term of `surfaceWindAt()`.
- `shaders/include/terrain_detail.glsl`: `tdSurgeAt()`; `shaders/include/terrain.glsl`: `waterAdjustHeightSurge()`.
- `shaders/cloud_v2_weather.comp`: the bands, CDO deck and swirl in the weather bake.
- `src/simulations/SatelliteSimWeatherCyclones.cpp`: the Weather window's Cyclones tab and milestones.
