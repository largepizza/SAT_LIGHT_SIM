# Storms and the weather catalog

Thunderstorms in SAT LIGHT SIM are **objects**, not regions of the cloud map. A storm catalog spawns them over
the whole globe from a lightning climatology, gives each one its whole life at birth, and answers "which storms,
towers, anvils and tornadoes exist at this instant" as a pure function of sim time. This page covers that model:
how storms are spawned, the five kinds, their towers and lifecycles, the anvil shields, squall lines and
mesoscale convective systems, tornadoes and waterspouts, and the surface wind field that storms drive.

It does not cover how storms are drawn (the towers, anvils and stamped cloud in the volumetric clouds are on
[Storms in the clouds](../rendering/clouds/storms.md)), tropical cyclones (their own page,
[Tropical cyclones](tropical-cyclones.md)), wildfires ([Wildfires](wildfires.md)) or dust walls
([Haboobs](haboobs.md)). Lightning flashes come from the towers; they are on
[Weather, rain, lightning, fog](../rendering/clouds/weather.md#lightning). How a player finds and watches
storms is on [The Weather window](../using/weather.md).

## A pure function of sim time

Nothing about a storm is integrated frame to frame. The catalog (`wx::Catalog`) divides time into **spawn bins**
of 30 min (`kBinS`) and the globe into equal-area **cells**; every (cell, bin) pair hashes its own storms. A storm
draws its whole life from a seeded generator the moment it is spawned: its kind, motion, towers, their offsets,
start times, lifetimes, peak strengths and growth times, and its anvil's linger. Asking for time *t* means
"collect every storm born in the last 8 h (`kMaxLifeS`) whose life covers *t*, and evaluate each at *t*".

So time warp, reverse playback, bookmarks and issue-report snapshots all see the same storms, and two runs of the
app agree storm for storm. The same idea drives the [Kessler](kessler.md) small re-entries and the aurora's
substorms.

!!! warning "Invariant"
    A storm's state at *t* must depend only on *t*, its seed and the catalog's inputs (the settings, the
    climatology, the cloud map's coverage and drift). Any state carried from the previous frame breaks reverse
    playback and makes a bookmark show different weather from the moment it was taken.

### The cloud map's frame

Storms live in the cloud **map frame**: the drifted sphere the weather cube is baked in, where a map direction
\( \mathbf{d} = R_z(\phi)\,\mathbf{e} \) for an Earth-fixed direction \( \mathbf{e} \) and the drift phase
\( \phi \) (see [Drift: the map moves with the wind](../rendering/clouds/field.md#drift-the-map-moves-with-the-wind)).
A storm therefore rides with the moisture it was born in. `wx::Env::mapToEarth` turns a map point back into an
Earth-fixed one for the things that are Earth-fixed: the climatology, the land mask, the local solar time and
the terrain.

The drift phase is session state (a bookmark stores it), and the catalog keys every cached bin on it
(`stormEnvHash()`): changing the drift, the coverage remap or a storm setting drops the cached bins. The storms
of a moment are the same for the same drift; a different drift moves them all.

Tropical cyclones are the exception: their tracks are Earth-fixed (a hurricane moves west in the trades while the
map drifts east), and they are converted to the map frame each frame.

## Spawning

### The climatology

Where and when storms are born follows a thunderstorm climatology (`wx::StormClimatology`): lightning flash
density in flashes per km² per year, as a function of latitude, longitude, day of year and local solar hour. A
baked table from the NASA LIS/OTD gridded lightning climatology is read from
`assets/textures/storm_climatology.bin` when present (written by `tools/make_storm_climatology.py`); without it an
analytic model answers: an ITCZ that follows the Sun with a lag, the land/sea contrast, the afternoon maximum over
land, the nocturnal maxima of the Great Plains, La Plata and the Sahel, and the maritime early-morning maximum.
The shipped build uses the analytic model (the log says which).

### Cells and the spawn rate

Spawn cells are bands 2° tall cut into \( \mathrm{round}(180 \cos\varphi) \) cells each, about 220 km across
and of equal area. In each cell and bin the number of candidates is a Poisson draw with mean

\[
\lambda = K \cdot f \cdot \rho_{\text{flash}}(\text{place}, \text{season}, \text{hour}) \cdot
\bigl(1 + (s - 1)(1 - \text{land})\bigr) \cdot A_{\text{cell}} \cdot \Delta t_{\text{bin}}
\]

- \( f \) is "Storm frequency (x)".
- \( s \) = 5 (`seaFactor`): a maritime storm makes about an order of magnitude fewer flashes than a continental
  one of the same size, so the flash density alone under-counts ocean storms.
- \( K \) is a calibration constant, computed when the settings change (`calibrate()`): an integral over the globe
  and twelve mid-month days of flash density × area × the local mean storm life, scaled so that about
  **3000 storms** (`targetActive`) would be alive worldwide if the cloud map were cloudy everywhere.

Each candidate takes a random point and moment inside its cell and bin, then is kept with probability
\( \mathrm{smoothstep}(0.15, 0.6, m) \), where \( m \) is the cloud map's coverage averaged over about 100 km
round it (the CPU's 1024 × 512 copy of the map, 3 × 3 texels, remapped like the shaders' coverage). No storm is born
in clear air; full rate from a cover of 0.6. A storm keeps its id (a hash of the cell, the bin and its index in
the cell) for life.

### Kinds

The kind is drawn at birth from the birth's latitude, land share and day of year (`kindMix()`):

| Kind | Share | Towers | Life |
|---|---|---|---|
| **Air-mass** | the rest (about 64%) | one tower | 40-90 min; about half never reach the anvil |
| **Multicell** | 35% (+5% over tropical seas) | 2-4 towers, a new one every 12-30 min | each tower 40-70 min |
| **Supercell** | 1% + up to 14% in warm-season mid-latitudes over land | one dominant tower + 0-2 flanking | the dominant tower 2-4 h |
| **Squall line** | own spawn pass (below) | 3-16 regenerating towers in a line | 6-11 h |
| **MCS** | own spawn pass (below) | an arc of regenerating towers | 8-15 h |

The supercell share peaks between 20° and 56° latitude over land in the hemisphere's late spring and summer (the
cosine of the season peaks near day 165 north, half a year later south).

### Motion and shear

Every storm has a **shear** direction (the way its anvil blows) and a motion. Poleward of about 20° the shear is
toward the east (the westerlies), equatorward of about 12° toward the west (the easterlies), turning through the
poleward direction between; each storm adds ±40°. A single-cell storm moves at 1-6 m/s within ±60° of its shear.
`Storm::centreAt(t)` moves the birth point along a great circle at that velocity.

## Towers

A storm is a set of convective **towers** (`TowerSpec`), each with an offset from the storm centre, a start time,
a lifetime, a peak strength (0 to 1), a growth time and a decay time. Its strength at time \( \tau \) after its
start is

\[
S(\tau) = S_{\text{peak}} \cdot \mathrm{smoothstep}(0, t_{\text{grow}}, \tau) \cdot
\mathrm{smoothstep}(0, t_{\text{decay}}, t_{\text{life}} - \tau)
\]

and 0 outside its life (`towerStrength()`). Strength is what the clouds draw: a weaker tower is **shorter**, not
thinner, and a dominant tower past about 0.42 reaches the anvil and flares its head. Each tower also has a role
(1 = dominant, the only one that can reach the anvil; 0.7 and 0.5 = flanking), a size (which scales its radius
between 0.7 and 1.3 × "Cb tower radius (km)"), an overshoot hash and a seed.

- **Multicell**: new towers form on the flank opposite the shear, 0.8-1.4 tower radii from the last.
- **Supercell**: the dominant tower grows over 22-28 min to a peak of 0.85-1 and lives 2-4 h; its flanking towers
  (peak 0.5-0.8) start from 15 min on and step down beside it.
- **Line systems**: each tower slot **regenerates**: a new tower every 45-75 min (`TowerSpec::period`), each cycle
  with its own seed, peak and a small shift along the line, while the system's envelope allows
  (`towerCycleAt()`).

`Catalog::towersAt(t)` lists every tower alive at *t* with its map-frame axis, its strength, its shear and its
age; that list is what the GPU's storm buffer is filled from each frame.

## Anvil shields

Every storm with an anvil-reaching tower carries an anvil **shield** (`wx::Shield`): a capsule on the sphere, a
spine from point *a* to point *b* with radius \( r_A \) at *a* growing to \( r_B \) at *b*, a strength, a
thickness and an age. The volumetric clouds draw the anvil from the shields, not from the weather map's
cumulonimbus typing, so an anvil only ever stands over a storm.

For the single-cell kinds the shield is fed by the dominant towers while they are above 0.42 (`anvOn` to
`anvOff`, precomputed at birth):

- It rises over 15 min after a tower first reaches the anvil, centred on that tower's head blown downwind.
- Its length grows with the storm's upper wind (6-18 m/s, drawn at birth) to at most 45 km (air-mass),
  90 km (multicell) or 160 km (supercell).
- \( r_A \) is the flared head's radius (1.15 × the tower radius × flare) times 0.55 rising to 1 as the anvil matures; \( r_B = 1.15\,r_A + 0.25\,L \).
- After the last tower falls below the anvil the shield **lingers**: 35-70 min for an air-mass storm, 45-90 min
  for a multicell, 60-120 min for a supercell (× "Storm lifetime"). It drifts downwind at 0.8 × the upper wind,
  thins (thickness down to 0.4 of full) and widens, and its age (`age01`, 0 to 1) tells the shader to fray it.
  Orphan anvils turning into cirrus.

The linger is drawn after everything the towers took from the storm's generator, so it does not change the
towers. `Storm::life` includes it; the spawn rate is calibrated on the towers' life alone, so the number of
storms does not depend on it.

## Squall lines and MCSs

Organised systems come from their own spawn pass (`buildLineBin()`): 3-h bins (`kLineBinS`) looking back 22 h
(`kLineMaxLifeS`), on coarser 6° cells. The rate is the flash climatology's monthly mean × an
**organised-convection factor** (`lineSystemFactor()`: warm-season mid-latitude continents such as the Plains and
La Plata, the Sahel and the Indian monsoon band, and the tropical oceans' squall lines) × an evening peak over
land (\( 1 + 0.8\,\text{land}\cos(2\pi(h - 19)/24) \)), calibrated so that about 160 (`targetLineActive`) would
be alive worldwide at full moisture, × "Storm complexes (x)". The moisture gate is a little stricter than for
single storms. An MCS is chosen with probability 0.25 + 0.4 × land (they are continental); the rest are squall
lines.

| | Squall line | MCS |
|---|---|---|
| Length | 180-520 km | 100-220 km, an arc |
| Speed | 9-17 m/s | 6-13 m/s |
| Bow (forward bulge at the middle) | 0-35% of the half length | 25-55% |
| Trailing stratiform region | 45-100 km behind | 110-200 km |
| Convective life | 6-11 h | 8-15 h |
| Shield linger | 1.5-3 h | 1.5-3 h |

A line moves with the westerlies poleward of 20° and with the easterlies equatorward of 12° (±35°); its towers
stand across the motion, spaced 1.8-2.5 tower radii, with the bow pushing the middle ones forward and an MCS's
cells straggling behind its arc. The system ramps up over 1.2-2 h: the middle forms first and the ends die
first.

Its shield is one capsule along the line, behind it: the spine sits 45% of the trail behind the towers, radius
0.55 × the trail + one tower radius, with a **rain** share (the trailing stratiform rain), the share of its
spine that is the convective line (`lineK`) and the line's bow (`bowM`). The weather bake uses these to stamp the
trailing rain, the shelf-cloud band ahead of the line and the clear air ahead of that; see
[Storms in the clouds](../rendering/clouds/storms.md).

## What a storm brings into the weather cube

A storm also brings its own cloud field: the weather bake (`cloud_v2_weather.comp`) stamps a broken cumulus field
thickening toward each tower inside the storm's envelope, a raining stratiform deck under the anvil's forward
flank, a clear moat at the envelope's edge and, for line systems, the trailing stratiform region and shelf band.
Inside a storm's envelope only the storm's own rain falls (the map's rain is suppressed), with a rain-free base
on the rear flank. The detail, and why the bake reads a separate envelope index, is rendering; it is on
[Storms in the clouds](../rendering/clouds/storms.md).

## Tornadoes and waterspouts

A storm's tornado is fixed by its id like everything else about it (`tornadoOf()`):

- **Supercells**: 35% put one down, for 5-25 min, in their mature phase: somewhere between 20 min after the
  dominant tower reaches the anvil and 10 min before it falls back.
- **Waterspouts**: 12% of air-mass and multicell storms whose first tower peaks at 0.6 or more, for 4-14 min
  between the tower's growth and three quarters of its life, and only where the funnel stands over the sea.
- **Terrain gate**: a supercell's tornado needs low, open ground. At its touchdown time the storm's centre and four
  points about 20 km round it (±0.18°) must all be below 1000 m with under 400 m between the highest and lowest.
  The test is made once per storm at its own touchdown time, not per frame. Waterspouts have no gate.
- **Place**: on the rear flank, just past the dominant tower's rain core: \( (-0.65\,\hat{s} + 0.7\,\hat{p})
  \times 1.1\,R_c \) from the tower's axis, with \( \hat{s} \) the shear, \( \hat{p} \) across it and \( R_c \)
  the tower's own radius at maturity (a waterspout 0.3 of that). The rain core reaches 0.9 of the radius, so the
  funnel stands under the rain-free base.
- **Width**: a tornado's radius is 60-220 m (most near the small end), a waterspout's 25-65 m.

Tornadoes are drawn for storms whose dominant tower stands within about 200 km of the eye. The funnel hangs from
the cloud base actually drawn over it: up to four funnel columns a frame are probed by the lightning pass for the
first height with cloud and the optical depth above it, and the funnel runs into that base and is lit as the
cloud march lights it. The geometry, the spin, the lighting and the opacity are on
[Storms in the clouds](../rendering/clouds/storms.md). The roar is the ambience layer `tornado_roar` (see
[Ambience](../sound/ambience.md)).

## The surface wind at the eye

The rain, the snow, the sea's surf and several ambience layers follow one **surface wind** at the observer
(`surfaceWindAt()` in `SatelliteSimRain.cpp`), a pure function of place and sim time:

- **A mean wind** from a slow value noise over place (about 500 km) and hours, at the eye's height: median about
  1.4 m/s, up to about 6 m/s, any direction. × "Surface wind (x)".
- **Storm outflow**: away from each tower within about 60 km, strongest a few km out, by its strength (a flanking
  tower half). "Storm outflow (m/s)" is the speed at full strength. The rain map's shower outflow (away from its
  rain-weighted centre) adds 0.25 of that × the rain rate, 0.3 of it for snow.
- **Gust fronts**: events hashed per 15-s bin of sim time and 2° cell. Each is a 1-2 s rise to 50-100% more wind
  (plus up to about 5 m/s near storms and in heavy rain, at most +10 m/s), turned 15-40°, decaying over 5-20 s.
  A bin's chance is about 0.1 in calm air and up to about 0.45 in a downpour beside a storm; it fades in with the
  conditions (a smoothstep on its hash), so a change of conditions never pops a gust in mid-decay. × "Gust fronts".
- **Turbulence**: small continuous wobble, × "Wind turbulence".
- **Tropical cyclones**: a Holland-like profile (see [Tropical cyclones](tropical-cyclones.md#wind-field)).

The result is eased over about 0.25 s. Its gust envelope drives the ambience's `gust` driver and its speed the
`surface_wind` driver. How the precipitation moves in it is on
[Weather, rain, lightning, fog](../rendering/clouds/weather.md#motion).

### The free-atmosphere wind field

Wildfire smoke uses a second, coherent wind field, `wx::windAt(dir, height, t)` (m/s east and north), shared by
every fire so neighbouring plumes agree:

- A **zonal climatology**: trades of 6.5 m/s toward the west (and 2.5 m/s toward the equator) centred on 13°
  latitude, weakening with height; westerlies of 8.5 m/s centred on 46°, strengthening with height
  (× (1 + h / 4 km)); polar easterlies of 3 m/s centred on 75°.
- **Eddies**: the curl of three sets of six travelling waves on the sphere (divergence free): synoptic (2500-4500 km,
  periods of 3-7 days), mesoscale (250-600 km, 6-18 h, changing with height over 3-6 km) and small (15-45 km,
  25-70 min, changing over 0.8-2 km of height; this set gives plumes their wiggle).
- **Friction**: near the ground the large-scale part is slower (0.55 of it at the surface, full by 1 km) and
  backed by about 24° (turned toward low pressure).
- **Thermal wind**: above the surface the wind turns with height by a slowly varying angle, up to about ±35° by
  3 km, so a plume's layers go different ways.

It is independent of the surface wind above: the two are separate fields. See [Wildfires](wildfires.md#wind).

## Performance

The storm update (`updateStorms()`) runs on the main thread each frame and costs about 1 ms: one scan of the
cached bins (each bin's storms' birth and end times are kept packed, so the scan does not touch the 1.3-KB storm
records), the tower and shield lists, and the GPU indices. Bins are built on a persistent pool of threads (three
quarters of the hardware threads, at most 12; `SATLIGHTSIM_STORM_THREADS` overrides the count for A/B
measurements): after a jump in sim time the missing spawn bins are cut into ranges of 512 cells, built in
parallel with the missing line bins and then the missing cyclone seasons, and joined in cell order, so the result
is identical to a serial build. The calibration is kept across a change of the drift (a bookmark, a jump), which
alters the bins but not the settings.

`stormsAt(t)` evicts every bin outside about [t − 9 h, t + 2 h]. A forward search for future events therefore never
runs on the main catalog (it would evict the bins the renderer needs this frame): the Weather window's search runs
on a second **scout catalog** with the same environment and settings, on a worker thread (see
[The Weather window](../using/weather.md)).

When sim time moves more than 2 h a frame for two frames in a row (one week per second and faster) the storms are
hidden: each frame would be a new, unrelated set of storms. A single jump still computes them.

Harness `state` → `weather.gpu_hash` / `env_hash` is a checksum of everything the storm buffer gives the GPU, for
checking that a change leaves it unchanged.

## Settings

All under `clouds_v2` in settings.json; Settings → Weather → Storm objects and the Weather window's Controls tab.

| UI label | settings.json key | Default | Effect |
|---|---|---|---|
| Storm objects | `storm_objects` | true | off: no storm towers at all (the cloud map only) |
| Storm frequency (x) | `storm_frequency` | 1 | × the calibrated spawn rate |
| Storm lifetime (x) | `storm_life` | 1 | × every lifetime (towers, systems, anvil linger) |
| Storm complexes (x) | `storm_complexes` | 1 | × the squall lines' and MCSs' spawn rate |
| Tropical cyclones (x) | `storm_cyclones` | 1 | × each basin's cyclones a season |
| Storm cloud field | `storm_field` | 1 | each storm's own cloud stamped into the weather cube |
| Storm clear moat | `storm_moat` | 1 | the clear moat round a storm |
| Cb tower radius (km) | `cb_radius_km` | 15 | a tower's base radius (km); flanking offsets scale with it |
| Surface wind (x) | `surface_wind_gain` | 1 | × the mean surface wind at the eye |
| Storm outflow (m/s) | `storm_outflow_mps` | 8 | a tower's outflow at full strength |
| Gust fronts | `gust_fronts` | 1 | × the gust fronts |

## Harness

`storms` (counts and the nearest towers), `storms list [kind=] [n=]`, `storms go storm=<id> | kind=squall|mcs|super
[day=1]`, `storms tornado [spout=1] [dist_km=] [alt=]` (the next daylit tornado among the storms alive over the
next 12 h, time set into it), `storms tornado draw=0|1`, and `state` → `weather` (counts, `cpu_ms_split`,
`tornado_list`, `line_systems`, `shields`). See [Automation harness](../development/harness.md).
`tools/weather_tool/` builds and prints the catalog offline.

## Where in the code

- `src/simulations/Weather.h`, `Weather.cpp`: `wx::Catalog` (spawn bins, `makeStorm()`, `makeLineSystem()`,
  `buildLineBin()`, `calibrate()`, `stormsAt()`, `towersAt()`, `shieldsAt()`, `towerCycleAt()`), the cyclone
  seasons.
- `src/simulations/StormClimatology.h`, `.cpp`: the flash climatology (table or analytic).
- `src/simulations/SatelliteSimWeather.cpp`: `updateStorms()` (the `wx::Env` the catalog reads, the storm
  buffer), `tornadoOf()`, `weatherFillTornadoes()`, the storm indices.
- `src/simulations/SatelliteSimRain.cpp`: `surfaceWindAt()`.
- `src/simulations/Wildfire.cpp`: `wx::windAt()`.
- `tools/weather_tool/main.cpp`: the offline catalog tool.
