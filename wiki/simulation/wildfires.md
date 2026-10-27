# Wildfires

Wildfires burn where the land-cover map has fuel and the season is dry, and like storms they are a pure function
of sim time. This page covers the model: the biome map and its built-up mask, how often and where fires start,
a fire's lifecycle and its burnt ellipse, the active arc of its front, and its smoke: how much it makes, where it
is injected, and the trajectories that carry it on the wind. How the front's points, the flames, the burn scars,
the glow and the smoke Gaussians are drawn is on [Storms in the clouds](../rendering/clouds/storms.md), and how
smoke dims and reddens the Sun and the sky is on [Atmosphere and sky](../rendering/atmosphere-and-sky.md). The
Weather window's Wildfire card is on [The Weather window](../using/weather.md).

## The biome map

`wx::BiomeMap` is a coarse land-cover map built at load (about 17 ms on 12 threads) from what the app already has:
the day map's colour (the ground as drawn), latitude, the CPU DEM and the ocean mask. It is equirectangular,
2048 × 1024 (about 20 km a texel) and Earth-fixed.

Each texel's class comes from the day map's colour averaged over 3 × 3 texels (about 60 km: the day map is a
dry-season mosaic and its texels are mottled):

| Class | Test (roughly) | Fuel | Dust |
|---|---|---|---|
| Water | the ocean mask | 0 | 0 |
| Ice | bright and not reddish, or poleward of 64° and bright, or south of 62° S | 0 | 0 |
| Desert | bright, tan, not green | 0 | 0.55-1 by brightness |
| Tundra | poleward of 58° or above 3800 m, not forest | 0.15 | 0 |
| Boreal forest | forest poleward of 50° or above 2200 m | 0.95 | 0 |
| Temperate forest | forest between 20° and 50° | 0.7 | 0 |
| Tropical forest | forest within 20° | 0.35 | 0 |
| Grassland | green, poleward of 25° (steppe, prairie, pasture, cropland) | 0.5 | up to 0.12 below 45° |
| Savanna | green within 25° | 0.85 | 0.08 |
| Shrubland | the rest between 20° and 40° (chaparral, scrub, semi-desert) | 0.75 | up to 0.35 |

The **dust** value is what a gust front can lift (see [Haboobs](haboobs.md)). Some places read oddly from the
colour alone: Kansas reads shrubland (the day map's prairie is tan).

### The built-up mask

A third byte per texel is **urban**: the night map's lights, the brightest of nine reads over the texel and its
edges of an about 18-km box mean of sRGB luminance, through smoothstep(20, 34) (the land's blue night-map base is
about 14, farmland and roads 15-25, a town's texel about 30, a city's 60-255). What a fire can burn is

\[
\text{burnable} = \text{fuel} \times (1 - \text{urban})
\]

(`burnableAt()`): no fire starts or runs in a town or city.

### The fire season

`seasonalDryness(lat, day of year)` is 0 to 1:

- **Extratropics**: the summer (peaking about day 210 north, day 25 south), a season that **narrows poleward**:
  smoothstep(\( c_0 \), 1, \( \cos(2\pi(d - d_{\text{peak}})/365.25) \)) with \( c_0 \) rising from −0.45 at 30°
  to 0.35 by 62°. May to October at mid-latitudes, June to August in the boreal belt, nothing in the winter half.
- **Tropics** (blended in within 18-30°): the dry season half a year from the summer monsoon (peaking about day 15
  north of the equator, day 220 south).
- The equatorial band (within 8°) stays wetter, down to 0.4 of the rest.

## Ignition

Fires are hashed per **1° cell and 1-day bin** (`fireInCell()`). A cell's chance of a fire that day is

\[
\lambda = 0.045 \cdot f \cdot p_{\text{biome}} \cdot \text{burnable} \cdot \text{dryness}^2 \cdot \cos\varphi
\]

at a random point inside the cell, with \( f \) = "Wildfires (x)" and an ignition propensity by biome:

| Biome | Propensity |
|---|---|
| Savanna | 1 |
| Shrubland | 0.5 |
| Grassland | 0.45 |
| Tropical forest | 0.4 |
| Temperate forest | 0.2 |
| Boreal forest | 0.18 |
| Tundra | 0.03 |

Savanna carries most of the world's burnt area; boreal and humid forests burn rarely, in larger fires. A point
with burnable under 0.1 never ignites.

**A fire needs a land mass.** An ignition is kept only if at least 6 of 8 points about 30 km round it are
burnable land (burnable ≥ 0.08): no fires on small islands such as the Aleutians or on the last texel of a coast.

Burning worldwide at the default rate (harness `fires world=1`): about 43 on 15 January, 63 on 15 April, 27 on
15 July, 137 on 12 August and 66 on 15 October, mostly savanna.

### A fire's life, fixed at birth

| Property | Model |
|---|---|
| Ignition time | 12:00-19:00 local solar time |
| Size | \( u^2 \), 0 to 1: most fires small |
| Life | log-normal, median 2.5 days × (0.4 + 1.6 × size), 0.4-30 days |
| Spread rate | \( (0.02 + 0.12\,\text{size}^{1.5}) \) × 0.6-1.4 m/s, day-averaged |
| Spread direction | the [wind field](#wind) 10 m above the ground a third of the way through its life |
| PyroCb | 35% of fires larger than 0.7 in boreal, temperate or shrubland fuel |

## The fire at an instant

`fireStateAt(f, t)`:

- **Activity** = growth over the first 4 h × decay over the last 30% of the life × a diurnal cycle
  \( 0.3 + 0.7\,e^{-(\Delta h/4.5)^2} \) peaking at 15:30 local × (0.6 + 0.4 × fuel). Quiet nights, an
  afternoon run.
- **The burnt ellipse** along the spread direction: the head \( 40 + 0.7\,v_{\text{spread}}\,t_{\text{age}} \) m
  downwind of the ignition, the back 0.12 of that upwind, half width 0.28 × head + 30 m. Its axis is fixed: a burnt
  area does not turn with the wind (the smoke does).
- **Plume top**: \( (800 + 2200\,\text{size})(0.4 + 0.6\,\text{activity}) \) m, × 2.6 for a pyroCb, at most
  8 km.

### Fingers

The front is the ellipse scaled radially from its centre by a fingered factor
\( 1 + (0.10 + 0.14\,h)\,n_1 + 0.06\,n_2 \) (`fireFrontRadiusK()`), with \( h \) the share toward the head and
\( n_1, n_2 \) periodic noises round the perimeter (9 and 31 cells, redrawn over 20 h and 7 h).

### The active arc

A fire burns all the way round only while it is young (`fireFrontHeat()`):

- **The arc.** Its half angle (in the ellipse's parameter) narrows from about 165° to 50-80° (per fire) as the
  fire matures over 1-14 h: the head and the upper flanks of an ellipse about three times as long as it is wide.
  It is turned off the axis by a slowly varying skew of up to about ±30° (one flank runs harder), redrawn over 9 h.
- **Runs and gaps.** A coarse pattern of 18-32 runs round the perimeter (by size), redrawn over 1.5 h, is
  thresholded so that about 40% of the head and more of the flanks are out, with nothing burning in a gap; a
  finer variation (120 cells, 0.6 h) within each run.
- **Shape.** 0.25 at the flanks to 1 at the head.
- **Flare-ups.** A few isolated flare-ups off the arc (a stump, a pocket of fuel), fewer after the first two days.
- **Fuel.** × smoothstep(0.08, 0.3, burnable) at the front's own point: the front stops at a lake, a town or bare
  ground.

Behind the arc the ground is a burn scar: a band just behind each burning stretch burns out red over a few hours,
and smouldering spots are scattered through the burnt area. Flame height is
\( (1.5 + 26\,\text{size}^{1.5}(0.4 + 0.6\,\text{fuel}))(0.35 + 0.65\,\text{activity}) \) m (a grass fire's
1.5 m to a crown fire's 20-40 m) and the flaming zone is 6-40 m deep.

**Burning share.** `fireBurningShare()` is the share of the front's heat (48 points round it, each weighted by its
stretch of perimeter) on burnable ground. The smoke and the flames are × it, and a fire whose share is under 0.4
is not offered as an event: a fire whose front has run into a town, the sea or rock is out though its state still
burns.

## Wind

Everything a fire's smoke does follows one coherent wind field, `wx::windAt(dir, height, t)`, shared by all fires
so that neighbouring plumes agree. It is described on [Storms and the weather catalog](weather.md#the-free-atmosphere-wind-field):
a zonal climatology (trades, westerlies, polar easterlies) plus the curl of travelling waves at synoptic,
mesoscale and 30-km scales, slower and backed near the ground, turning with height. A plume therefore bends over,
wiggles, curves as the wind changes over hours and fans out where a lower layer goes another way.

## Smoke

### Production and injection

Smoke comes from the whole active front. Its heat (the 48 front points) is cut into *S* **sections** of equal heat,
starting at the arc's coldest point; each section is a **source** at its heat-weighted centre, as wide as the
section, carrying its share of the smoke. *S* is 5 for a fire within 150 km of the eye, 3 within 500 km, 2 within
1200 km and 1 beyond. Sources are labelled by their bearing round the burnt ellipse's centre from its spread axis,
so a source index names the same stretch of front from one emission to the next.

- **Rate**: the head's optical depth \( \tau_0 = (2.5 + 5.5\,\text{size})(0.6 + 0.4\,\text{fuel}) \) over a source
  width of max(150 m, 0.3 × half width) + 300 m × size, × the activity when it was emitted, × the burning share,
  × "Wildfire smoke".
- **Injection height**: the plume top × clamp(\( \sqrt[3]{6/(|u|+2)} \), 0.65, 1.25), \( u \) the wind at half the
  plume top: lower in a strong wind (the column bends over). A section injects at
  \( H \cdot \mathrm{clamp}(0.7 + 0.3\sqrt{wS}, 0.55, 1.1) \) for its heat share \( w \): weaker sections lower.
  Within 800 km and for plumes over 1200 m a low layer at 0.4 \( H \) takes 20% of the strongest source's smoke.
- **Rise**: from a start 120 m + 0.4 × the flaming depth downwind of its section, a parcel rises toward its layer
  \( H_b \) as \( H_b(1 - e) + (30 + 0.08 H_b)\,e \), \( e = \exp(-a\,w_0/H_b) \), with the updraft
  \( w_0 = 5 + 9\,\text{size} + 5\,\text{pyro} \) m/s. The column is drawn only from 0.12 of the rise time on: the hot
  gas just above the flames is clear.

So the smoke leaves the arc as a curtain along it and merges downwind into one plume.

### Trajectories

Each source's streak is a chain of **parcels** with fixed **emission times** on a nested grid: spacing
\( d \cdot 2^l \) for the ages in band *l* (*d* = 60 s for a near fire's main source, 120 s for its other sources and
for fires within 500 km, 240 s within 1200 km, 480 s beyond), so the knots are fine near the fire and geometric
down the trail. A parcel whose emission time is not on the next band's grid merges into its neighbours over the
last 35% of its band and drops out where it coincides with them: nothing pops.

A parcel leaves the fire **as the fire was when it was emitted** (its sections, injection height, activity and
smoke production then: `SmokeEmit`, cached) and is advected through `windAt` at **its own** position, height and
time, step by step on a fixed schedule of ages (`fireSmokeStepS()`: steps of 0.15 × the age, 20-900 s; Euler,
`fireSmokeAdvect()`). Trajectories are cached and only ever extended:

- A trajectory is a pure function of its emission time. Time warp and reverse playback show the same trail; a jump
  integrates each trajectory from its emission.
- A change of wind at the fire bends only the smoke emitted after it.

!!! warning "Invariant"
    A smoke parcel's position must come from integrating the wind along its own path since its emission. Walking
    the trail from the fire with the current wind makes the whole trail swing as one body whenever the wind at the
    fire turns.

The trail's length is fixed per fire, \( (3 + 9\,\text{size}) \times 0.8 \) h (2-14 h), its last segments fading
over the final 20%. Only the head segment (the column's last minute or so) rides on the current wind.

### The pall

Smoke older than the trail is one flat, **mass-conserving** pall per fire: what the fire made over its life past the
trail, less a removal time of about 4 days. It sits at the mass-weighted mean of parcels emitted on a 4-h grid over
the 36 h past the trail (weights \( \sin^2 \) of their place in that window), each carried 2-50 h by the winds it
met, and its width (20-350 km) comes from their spread. One big fire's pall is about 0.01-0.05 in optical depth seen
from above; a fire season's palls overlap into the regional haze. `wx::fireSmokeAt()` integrates the same
trajectory for one age (the harness's views).

### Colour and light

Smoke is grey: fresh smoke (with black carbon) a little darker, single-scattering albedo 0.45, aged smoke and the
pall paler and slightly cooler, 0.6. It is lit by the Sun's colour at each piece (its path through the air), shadowed
by the Earth per pixel, and by a blue sky. Its scattering is as spectral as its extinction (Angstrom exponent about
1.6 × "Smoke spectral"), so thin smoke is blue-grey from the side, a thick column grey, and the Sun and the land seen
through it turn yellow to red. Brown-orange appears only where the light crossed smoke or air toward a low Sun.

### The drawn geometry

The fires within 600 km of the eye (up to 3000 km from orbit, faded over the last 20% of the reach) are drawn, the
nearest first. A fire's sources and pall become Gaussians in the dust list: each trail segment between two knots is
cut into up to 12 round pieces along a Catmull-Rom curve through the neighbouring knots, at most about 2.2 × the
smoke's width apart and never closer than 6 px on screen, sharing its mass; every smoke Gaussian is at least
about 1.5 full-resolution pixels thick as seen (the pall 3), its column kept. The budget is 640 smoke Gaussians a
frame, with at least 96 left for the flames. `SATLIGHTSIM_SMOKE_PIECES=0` draws single segments (an A/B). See
[Storms in the clouds](../rendering/clouds/storms.md).

## The front, flames and glow (summary)

What the renderer is given, from the model above:

- **Points**: the active arc as a field of flickering point sprites on a nested hierarchy round the perimeter,
  about 3.5 px apart on screen near (2 px from afar), each carrying its stretch of front's light: flame luminance
  about 2 × 10⁴ cd/m² × the flame's visible area. Embers glow in the burning-out band, smouldering spots inside the
  scar, spot fires ahead of the head. A big fire's head reads about magnitude 2 from orbit. A GPU pass
  (`wildfire_ground.comp`) puts every point and flame on the detailed terrain, which the CPU does not know.
- **Flames**: within 40 km, emissive Gaussian tongues along the arc, 0.66 × the flame height tall and
  0.35 × it + 0.05 × min(spacing, 6 × height) wide, at most half the height + 1 m deep across the front.
- **Burn scars**: the nearest 8 fires' burnt ellipses (with their fingers) turn the day map charcoal with ash and
  unburnt islands; the scar ends with the fire.
- **Glow**: at night the nearest 6 fires within 120 km light their smoke's base and the ground round them, a few
  km, from up to three points along the arc.

## Settings

All under `clouds_v2` in settings.json; the Weather window's Controls tab, WILDFIRES.

| UI label | settings.json key | Default | Effect |
|---|---|---|---|
| Wildfires (x) | `wildfires` | 1 | × the calibrated fire rate (0 = none) |
| Wildfire smoke | `wildfire_smoke` | 1 | × the smoke's optical depth (columns and pall) |
| Smoke brightness | `wildfire_smoke_brightness` | 1 | × the light the smoke scatters |
| Smoke tint (-1 blue .. +1 brown) | `wildfire_smoke_tint` | 0 | the smoke's own colour |
| Smoke spread (x) | `wildfire_smoke_spread` | 1 | × the plumes' and pall's width, same mass |
| Smoke spectral (x) | `wildfire_smoke_spectral` | 0.6 | × the Angstrom exponent (1 ≈ 1.6, fresh smoke; 0 = grey) |
| Wildfire light | `wildfire_light` | 1 | × the front's light (points and glow) |

## Harness

`biome [<lat> <lon>] [export=<png>]` (the class there, or the whole map as a PNG), `fires [world=1]` (the fires
drawn, the nearest's smoke knots, `front_burnable_share`, `smoke_traj_steps_frame`, cache sizes; `world=1` the
worldwide count and the fronts on lit land), `fires go [k] [view=side|under|orbit] [...]` (skips fires whose
burning share is under 0.4), `fires look [k] [what=head|plume]`. See [Automation harness](../development/harness.md).

## Where in the code

- `src/simulations/Biome.h`, `Biome.cpp`: `wx::BiomeMap` (`build()`, `burnableAt()`, `seasonalDryness()`).
- `src/simulations/Wildfire.h`, `Wildfire.cpp`: `fireInCell()`, `fireStateAt()`, `fireFrontAt()`,
  `fireFrontHeat()`, `fireBurningShare()`, `windAt()`, `fireInjectionHeightM()`, `fireSmokeRiseM()`,
  `fireSmokeAdvect()`, `fireSmokeAt()`, `firesBorn()`, `firesWorld()`.
- `src/simulations/SatelliteSimWildfire.cpp`: the fires near the eye (cached per day bin), the smoke sources,
  trajectories and pall, the points, flames and glow, the burn scars' UBO.
- `shaders/wildfire_ground.comp`: points and flames onto the drawn ground.
- `shaders/include/smoke.glsl`, `shaders/cloud_march.comp`: how smoke is composited and lit.
