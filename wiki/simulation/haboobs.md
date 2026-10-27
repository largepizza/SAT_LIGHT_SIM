# Haboobs

A haboob is a dust wall raised by a thunderstorm's gust front: the cold outflow spreads ahead of the storm, and
where it crosses bare, dry ground it lifts dust into a billowing wall one to two kilometres tall with a thinner
pall behind it. This page covers the model: which storms raise one, where the front is, how strong and how tall the
wall is, the clear air ahead of it, and how the front fades when the storm dies. How the wall is drawn (as cloud in
the volumetric clouds near the eye, as plume Gaussians from afar) is on
[Storms in the clouds](../rendering/clouds/storms.md). The storms themselves are on
[Storms and the weather catalog](weather.md).

## Which storms

Squall lines, MCSs, multicells and supercells raise haboobs; air-mass storms and tropical cyclones do not. A storm
is a candidate while it has a dominant tower (any tower, for a line system) with strength over 0.03, and also
**after its towers have died** while it is still alive in the catalog (`stormOrganisedNow`), since its outflow
outlives them (below).

The dust comes from the [biome map](wildfires.md#the-biome-map)'s **dust** field: deserts 0.55-1, shrubland up to
0.35, warm grassland up to 0.12, savanna 0.08, everything else 0. So haboobs rise over the Sahara and the Sahel,
Arabia, the Thar, the Gobi, the south-western United States and inland Australia.

## The gust front

`haboobFrontOf()` builds the front as a list of points, each with a position, the front's tangent, its outward
direction and a strength from 0 to 1.

**Lead.** The outflow leads the towers once the storm has rained for a while:

\[
d_G = \begin{cases}
8 + 22\,g \ \text{km} & \text{squall line or MCS}\\
4 + 11\,g \ \text{km} & \text{multicell or supercell}
\end{cases}
\qquad g = \mathrm{smoothstep}(0.5\,\text{h}, 3\,\text{h}, \text{age})
\]

A mature line's front runs about 30 km ahead of its convective line, out in the subsidence ahead of the line's
shelf band; a cell's about 15 km.

**Direction.** "Ahead" is the storm's motion **over the ground**: its motion in the map frame plus the cloud map's
drift. Where the two disagree (a storm moving against the drift more slowly than the drift carries it) the front
is on the side the storm is actually heading.

**Shape: arcs.**

- A line system, or a storm with two or more towers: points every 4 km along the towers sorted across the motion,
  \( d_G \) ahead, **scalloped**: furthest ahead in front of each tower and 0.55 of that midway between two. At each
  end the front curves back round the last tower over four more points, weakening.
- A single cell: a semicircle of eleven points round the strongest tower, widest and strongest ahead of its motion.

**Strength.** Each tower contributes its strength × smoothstep(0.03, 0.4, strength), so a decaying tower fades its
stretch of wall out instead of cutting it. Along the front only about half is dusty: a value noise of the distance
across the storm's motion (about 60 km and 22 km cells, a coordinate fixed to the storm, so the stretches stay put
as towers come and go) is thresholded by smoothstep(0.38, 0.62), leaving gaps.

**Lingering outflow.** The cold pool keeps spreading and the dust settles over the better part of an hour after a
cell collapses. The storm's towers of 15, 30 and 45 min ago, carried forward with the storm, are evaluated from the
catalog (a pure function of time) with their strength decaying to nothing by 45 min: they **sustain** the current
towers (a current tower within 15 km keeps the larger of the two strengths). When the storm has no towers left, the
latest lag that had towers stands in for them on its own. Mixing the lagged towers in as extra sources would draw
a zigzag front of old and new tower positions.

!!! warning "Invariant"
    The front is computed from the catalog at time *t* with the cloud map's drift phase **at t**, not the current
    one. The Weather window's forward search and the Watch build fronts hours ahead; with the current drift they
    would place the wall elsewhere.

## The wall

At each front point the dust raised is

\[
k = \text{dust} \times \text{strength} \times \text{"Haboobs (x)"}
\]

and a wall stands where \( k \ge 0.08 \) (`haboobPointK()`).

**Height.** Head by head the wall is 0.5-1.8 km tall: \( H_0 = (700 + 500\min(1, k)) \) m × 0.75-1.25 (a hash
per point), and along the front a smooth noise of the distance along it (3.5-km knots) sets each stretch's height
(× 0.65-1.2) and strength (× 0.55-1), so the wall is a row of rounded heads of different sizes, not a slab. Real
haboobs are about 1-2 km tall, rarely about 3 km, under a dry-desert cumulonimbus base 2.5-4 km up.

**Profile across the front** (distance *c* outward from the front, metres):

| Where | Strength |
|---|---|
| more than 1.5 km ahead | 0 (the clear air, below) |
| from 1.5 km ahead to 1 km behind | rising to full (the wall's face and nose) |
| 3-12 km behind | falling to half (the pall) |
| 16-26 km behind | falling to nothing |

The top falls to 0.4 of the head's height over the pall.

**Clear air ahead.** The air ahead of the outflow sinks and is dry, so the low cloud and rain there are removed: a
**base lift** of the low cloud to about 1.35 × the tallest head + 300 m (about 3 km above the ground), from 8 km
behind the front (over the pall, so the wall's top is not inside cloud) to 12 km ahead (fading over the last 3 km),
with lumpy edges. The storm's cumulus above that height stays whole. Within 3.5-6.5 km of a tornado on the ground
(or about to be) the clear air is held back, so the funnel keeps the cloud base it hangs from.

## The dust grid

Near the eye the walls are cloud in the volumetric clouds. `weatherFillDustGrid()` rasterises every candidate
storm's front into a 512 × 512 grid on the tangent plane at the observer's sea-level point: cells of 400 m near the
ground, + 0.6% of the eye's altitude, at most 6 km. Per cell: the wall's strength (16 bits), its top in 2-m units
(12 bits) and the clear air (4 bits). Each front point stamps a strip of ±1.2 of its spacing along the front; the
strongest stamp wins a cell. The grid is rebuilt (on the storm pool's threads, about 1-2 ms) when sim time moves
2 s or the eye a quarter of a cell. Beyond the grid (from orbit) plume Gaussians stand in, faded in over the grid's
outer 15%.

The wall's lobe noise moves with the wall: it is anchored to the nearest drawn haboob's storm motion over the
ground, integrated over sim time, plus a rise of 2 m/s, so the billows churn up the face. The rendering (the 3D
margin, the beige tint, the extinction of 0.016 /m in the core, the dust-free specialisation of the march) is on
[Storms in the clouds](../rendering/clouds/storms.md).

## Finding one

The Weather window's Haboob card searches forward (on the scout catalog, see
[Storms and the weather catalog](weather.md#performance)) for a line system at full strength by day whose leading
edge (nine points along it, \( d_G \) ahead) lies mostly over dust, inland, with its face front-lit and clear of the
anvil's shadow. The Watch stands 6 km ahead of the wall's dustiest, best front-lit stretch, stepping forward 2 min
at a time (up to an hour) when the regenerating towers are weak at the chosen moment. See
[The Weather window](../using/weather.md).

## Settings

| UI label | settings.json key | Default | Effect |
|---|---|---|---|
| Haboobs (x) | `clouds_v2.haboobs` | 1 | × the dust raised (0 = none) |
| Haboobs as cloud | `clouds_v2.haboob_field` | true | on: walls drawn by the volumetric clouds within the dust grid; off: plume Gaussians everywhere |

Both are in the Weather window's Controls tab. Changing either starts a new epoch of the cloud-density clipmap, as
does a wall entering or leaving the grid.

## Harness

`haboob [aloft=1]` runs the Weather window's Haboob card (its search and Watch; `aloft=1` stands 2 km up);
`fires` → `dust_grid` {on, walls, cells, cell_m, ms, nearest_wall_km, e_max}. See
[Automation harness](../development/harness.md).

## Where in the code

- `src/simulations/SatelliteSimHaboob.cpp`: `haboobFrontOf()`, `haboobPointK()`, `haboobSunOnFace()`,
  `weatherFillDustGrid()`, `weatherFillHaboobs()` (the far-field Gaussians).
- `src/simulations/SatelliteSimWeatherFeatured.cpp`: the Haboob card's search.
- `shaders/include/cv2_storms.glsl` (`cv2DustWallAt()`), `shaders/include/clouds_v2.glsl`
  (`cv2DustWallSigma()`): the wall in the cloud field.
- `src/simulations/Biome.cpp`: the dust field.
