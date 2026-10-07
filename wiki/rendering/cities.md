# Cities, farms and solar parks

The ground textures are 5 km per texel: from the ground or an aircraft a city is a smooth glow and farmland a
flat colour. This page covers the procedural patterns that resolve them: city lights at night, city layout by
day, the far city lights drawn as point sprites, farm fields, beaches and solar PV parks. The terrain they sit
on is described in [Terrain](terrain.md).

## Two shared techniques

Every pattern here rests on two rules.

**A world-fixed 2D frame** (`cityFrame()` in `shaders/sat_sky.frag`). The ground point is projected onto the
ECEF plane perpendicular to the dominant axis of up (the erosion octaves' cube-face projection), on the
anchored lattice described in [Terrain](terrain.md#precision-the-anchored-lattice). A district cell is two
anchor cells, 4096 m. Any lattice whose cell divides 4096 m is exactly world-fixed. A rotated lattice is
measured from a fixed **area** origin (a block of 32 districts), never from a moving one, or it would jump as
the observer moves.

**A ratio to the expected mean.** A pattern multiplies the map by `pattern / E[pattern]`. As the pixel
footprint grows past the pattern's scale it is filtered to its mean, so it converges to exactly the map.
Orbit views therefore show the map unchanged, and the hand-off from pattern to map has no seam. Stripes
(streets, rows, roads) use exact box-filtered coverage (`cityBoxCover()`, `solarPulseCover()`), so they keep
their mean at any footprint without aliasing.

The **footprint** that drives each fade is the ground footprint stretched along the view,
`pixAngle x t / max(|dir . n|, 0.2)`.

## Where cities are: the night map

The night map is a Black Marble composite. Its lights are taken with the blue base removed:

\[ \text{cityLights} = \max(\text{night} - (0.006,\, 0.006,\, 0.0132),\, 0) \]

(see [City light emission](terrain.md#city-light-emission)). Its luminance `lum` drives everything:

- **density** `dens = smoothstep(0.004, 0.12, lum)` (street drop-out, lamp mix, lot sizes);
- **presence** `smoothstep(0.002, 0.03, lum)` (how much of the day albedo the city replaces);
- **glitter density** `lum / 0.25`.

The procedural lights take only the map's brightness; their colour comes from the lamps. The base is not
uniform, so its subtraction leaves blue or red residue at a city's faint fringe, which sparse glitter would
amplify into coloured dots.

### Terrain-following limits

`cityTerrainLimit()` (`shaders/include/terrain.glsl`) keeps lights on the valley floor and lower slopes, instead
of letting the map's 5 km blur climb every mountainside:

- lights end at a contour \( 150 + 330\,\text{dens} \pm 90 \) m above the regional mean ground (DEM mip 4.5,
  about 40 km), over a further 120 m;
- none above 4.2-4.9 km;
- steep ground keeps only 4% (`smoothstep(0.86, 0.96, n.up)`).

The surface and the far city sprites share the same limit.

## City layout

`cityLayout()` builds one `CityLayout` per pixel, used by both the night pattern and the day albedo.

- **Districts**: jittered Voronoi cells of 4096 m (the nearest two are kept).
- **Regions**: one street grid per region of 4 x 4 districts (16 km). Streets run straight through district
  borders; districts differ only in which streets exist. Some districts lay out their own grid (T-junctions,
  organic warps), with a probability set by the style.
- **Grid** (`cityGridOf()`): an angle per region (or north-aligned, with a style's probability), a spacing
  from the style, an arterial every 6-8 lines, an organic warp. Every random choice is per region or district
  at a fixed lattice point, so nothing changes inside a region.
- **Which streets exist** (`cityLineKind()`): arterials always; other lines by a per-region hash against keep
  probabilities that fall with density (more drop-outs in suburbs). The decision depends only on the line, so
  it cannot jump at a district border.
- **Where two different grids meet** (another region, or an independent district), the day side draws a 14 m
  road along the border, within 80 m (`kCityBlendM`).
- **Voids** (parks, dark areas): a 1024 m value noise.

### Regional styles

Nine styles, by soft continental boxes with wobbled borders (`cityStyleWeights()`). **Geometry** is one style
per region, drawn at the region's centre, so a grid never changes inside a region. **Colour and lighting**
blend per pixel.

| # | Style | Street spacing (m) | North-aligned grids | Perimeter blocks | Slab rows | Roofs | LED share bias / signs |
|---|---|---|---|---|---|---|---|
| 0 | North America, Oceania | 115-160 | 85% | - | - | mixed | +0.05 / 1.0 |
| 1 | Latin America | 95-115 | 50% | 10% | - | terracotta | -0.25 / 0.8 |
| 2 | Mediterranean Europe | 70-100 | 5% | 55% | 5% | terracotta | 0 / 0.7 |
| 3 | Northern Europe | 85-120 | 5% | 45% | 15% | slate | +0.1 / 0.6 |
| 4 | Middle East, N Africa, Central Asia | 80-115 | 20% | 30% | 5% | light flat | 0 / 1.2 |
| 5 | Sub-Saharan Africa | 60-85 | 20% | - | - | mixed, red earth | -0.2 / 0.6 |
| 6 | South and SE Asia | 55-75 | 10% | 5% | 5% | light flat | 0 / 1.3 |
| 7 | East Asia | 75-100 | 55% | 10% | 35% | light, blue-green metal | +0.2 / 2.5 |
| 8 | Russia, post-Soviet | 190-260 | 30% | 10% | 70% | mixed | -0.2 / 0.5 |

??? info "All style parameters (`kCs*` tables)"
    | Table | Fields |
    |---|---|
    | `kCsGeo` | base street spacing, random range, block aspect (long blocks drop the odd cross streets), P(north-aligned) |
    | `kCsGeo2` | organic warp, P(independent district grid), lots per side, P(perimeter block) |
    | `kCsGeo3` | P(slab rows), large-commercial multiplier |
    | `kCsRoof` | terracotta, light flat, blue-green metal, slate shares |
    | `kCsGrnd` | bare-earth redness, P(bare yard), paved bias, tree multiplier |
    | `kCsLamp` | LED share bias, cool share of LEDs, sign multiplier |

    Perimeter (courtyard) and slab blocks are chosen per **district**, not per block: scattered single rings
    read as picture frames. One block in six is an exception and keeps ordinary lots.

## The night side

### Lamp posts and streets

Close up, `cityLightPattern()` draws lamp posts along the streets: one every 35 m (30 m on arterials, 3.5 x
brighter), each a **head** (1.2 m, emission) and a **pool** on the road (6 m). Per post: brightness 0.5-1.5 x,
one in ten dark, and its own lamp type:

| Lamp | Colour (linear) |
|---|---|
| sodium | (1.28, 0.90, 0.42) |
| warm LED | (1.04, 0.99, 0.90) |
| cool LED | (0.90, 0.98, 1.14) |
| metal halide | (1.02, 1.00, 0.96) |

The LED share rises from 0.25 in suburbs to 0.85 in cores, +0.25 on arterials, plus the style's bias and a
4 km regional noise. Arterials carry rare neon signs (red, blue, green) and traffic lights at their crossings,
cycling with time.

The **pools light the ground**: they multiply the final day albedo's brightness (relative to a mid grey, so
streets, roofs and textures show in each pool), times the face's up-facing (lamps are overhead, so relief
shades), times the terrain AO. Brightness only: a green lawn map does not make suburbs glow green.

As posts become unresolved they merge into lines, then into a uniform glow normalised to the expected lamp
density. Arterial streets and commercial strips along them are brighter (`strip`), voids are dark, and a 2 km
value noise varies the brightness. The mean ramps from 0.12 of the map close up to 1 at the layout's end, so a
city core at the night exposure is not a lit sheet.

### Glitter

A real light saturates its own pixel, so a distant city glitters rather than glowing evenly. `cityGlitter()`
draws a world-fixed lattice of points at every distance past the lamp posts:

- cells of \( 8 \cdot 2^k \) m, \( k \) = 0-9 (8 m to 4 km), chosen so a cell spans about four pixel footprints;
  two adjacent levels cross-fade;
- the 2 x 2 nearest cells are tested, so a point is never cut at a cell edge;
- a cell holds a point with probability \( 0.4 \times \) the glitter density: a suburb is fewer points at the
  same total brightness, and city edges stay crisp;
- each point has a lognormal weight (mean 1) and the result is divided by the occupancy, so the mean is 1;
- each is a 0.3 m source drawn with a star-like spread (a core plus a halo whose share grows with brightness),
  windowed to 0.55 cells;
- the spread is **anisotropic**: along the view's ground direction it uses the stretched footprint, across it
  the plain pixel footprint (`gCityViewE`, `gCityFootX`). A round point of the stretched size would draw far
  towns as wide horizontal smears; this keeps them dots on screen;
- under the temporal upscale (render scale below 100%) the footprint is that of the rendered (input) pixel,
  so every point is at least an input sample wide and every jitter phase sees it;
- a slow scintillation past 15-80 m per pixel, at "City light twinkle rate";
- below 4-16 m per pixel up to 14% of points (x the style's sign multiplier) are coloured signage.

Past "Street layout to", `cityLightFar()` draws the glitter alone on the world frame, without the street layout.

### Major roads

`tools/make_city_roads.py` bakes Natural Earth 10 m roads into `assets/textures/city_roads.bin`: a header
("SLRD", grid size, segment count), a cell list on a 0.125 deg equirectangular grid (2880 x 1440), and segments
as ECEF endpoints on the sea-level sphere with a class (0 road, 1 secondary highway, 2 major highway) and a
width (12, 18, 30 m). Ferries and tracks are dropped. The app uploads the file as a storage buffer (sky binding
28); without it, the header says "no roads".

`cityRoadLight()` reads up to 64 segments of the pixel's cell. Endpoints are taken relative to the anchor cell's
origin, a multiple of 2048 m, so the subtraction of float ECEF values is exact. Each road is a line of light by
class, dotted into posts every 40 m close up, and fades out by "Major roads to".

### Layers by footprint

The night layers end at ground footprints set on the Night lights tab, each fading in from about 0.4 x its value:

| Layer | Ends at (m per pixel) | Notes |
|---|---|---|
| Lamp posts | "Lamp posts to" (27) | close-up mix of streets vs glitter: "Street light share (near)" |
| Street grid | "Street grid to" (380) | past it only arterials; share "Street light share (far)" |
| Major roads | "Major roads to" (1300), capped by the layout | |
| Street layout | "Street layout to" (2000) | past it `cityLightFar()` |

### Under clouds

Seen through cloud, the **finished** light is blurred toward the map's mean at mip "City light blur LOD", by the
cloud's opacity in front, and the pools fade. Blurring the map the pattern reads instead would spread a city's
light onto the desert beside it and draw street grids there.

## City light sprites

Far city lights are drawn as satellite point sprites, so the horizon seen from the ground and from aircraft is
a packed field of crisp points where the surface can only draw the map's blobs.
`shaders/city_sprites.comp` runs after `sat_flare.comp` and **appends finished records to the satellite point
list** (see [Points, bloom and glare](points-bloom-glare.md)); the point, bloom and glare draws show them with
no pipeline of their own.

- **Levels**: seven dispatches of a world-fixed lattice whose cell grows with distance: 64 m x \( 2^k \), 125
  cells either side (250 for the last), reaching 8, 16, 32, 64, 128, 256 and 1024 km. Each level fades in over
  the last quarter of the one before, roughly one density on screen. Near levels are dispatched first, so a
  full list drops the far lights.
- **Occupancy**: a cell holds a light with probability \( 0.6 \times \) the glitter density, not on water, and
  passing `cityTerrainLimit()`. The light stands 6-26 m above the DEM; it must be above the horizon.
- **Far only**: a light is drawn only where a pixel spans more than "City sprites from (m/px)" of ground (the
  same stretched footprint the ground uses). Up close, sprites read as floating lanterns. Levels whose reach
  is inside the nearest range that can qualify are not dispatched.
- **Brightness**: `effectFlare = 8 x (cell / 64 m)^2 x w x twinkle x 10^6 / r^2 x exp(-airmass / 40 km) x gain`,
  with the air along the slant path from an 8 km scale height (40 km sea-level visibility), capped at 8 so no
  sprite reaches glare. Lamp colours only, warmer than the ground's mix.
- **Fades**: by night only (civil twilight), and with eye altitude over 100-150 km.
- **Records**: slot index `0xFFFFFFFF` (picking skips it) and `meshPx = -1` (the trail pass skips it), recorded
  at 99.5% of the range. Up to 131072 extra slots in the visible list.
- **Depth**: a city light skips the hardware depth test (`sat_point.vert` gives it depth 0) and is tested in
  the fragment shader (`sat_point.frag`) against the unjittered half-resolution scene depth, at 0.95 of its
  recorded range. Under the sky TAA the hardware depth is restored from this frame's jittered sample, and at a
  crest seen at a low angle a pixel's sample hits the ridge in some jitter phases and the ground beyond in
  others: a light just behind the crest would be hidden and drawn by turns. The 5% margin keeps the half-res,
  filtered ground around a light (which stands only a few metres above it) from hiding it; only real relief in
  front does.
- **Where they sit**: on the DEM plus the water map (the terrain detail is left out). With the terrain
  knocked out (knockout bit 1, set by Planetarium) the ground is drawn as the sea-level sphere, and the
  lights sit on it. They are not drawn under Potato: their eye height comes from the terrain frame the depth
  pass writes, which Potato skips.

Where sprites carry the lights, the ground keeps "Ground glitter under sprites" of its glitter: a point's display
saturates, so sprites alone cannot carry a dense city's light per area.

!!! note "Fog hides the sprites"
    Point sources are occluded by the cloud composite, and radiation fog over a city counts as opaque to
    points while the ground glitter still shows through it. Test sprites with `clouds_v2.fog_amount 0`.

## Presets and temporal anti-aliasing

The procedural city lights and the city day layout run in the full sky shader and in its `SKY_LITE` variant,
so every preset except Potato draws them, independent of the procedural terrain detail switch. Potato's sky
shader (`sat_sky_minimal.frag`) draws the night map and the legacy city detail textures, and the environment
probes and mirror reflections (`SKY_ENV`) take the night map alone. Farms and beaches belong to the terrain
detail (`tdEnabled()`), so they need "Terrain detail" above 0 (Terrain tab) and the full sky shader. See
[Weak-hardware tiers](hardware-tiers.md).

Under the [sky TAA](atmosphere-and-sky.md#sky-taa) a light smaller than a pixel would be caught by only some
of the sub-pixel jitter positions. Three rules keep distant city lights steady:

- **Unjittered inputs.** Where the view meets the ground at more than about 12 deg (\( |\cos| > 0.2 \) between
  the unjittered ray and the local up), the city lights read the night map, the terrain contour of
  `cityTerrainLimit()` and their own pattern (layout, lamp lines, glitter) at the pixel's **unjittered**
  point: the jittered hit moved back along the hit's tangent plane (`uvTaa0`, `taaDn`). Those inputs switch a
  light on or off (the map minus its blue base crosses zero at a city's fringe, the contour cuts over 120 m
  of height, and the glitter concentrates a sparse fringe's light into a few bright points), so a jittered
  read would light a point in only some phases. Toward a grazing horizon the tangent-plane point slides
  kilometres for half a pixel and is wrong over hills, so the jittered point is kept there.
- **Still history.** When nothing moved in a frame (the eye moved less than 1 cm, the view did not turn, and
  sim time advanced by at most 1.5 times the frame step since the last resolve), `sky_taa.comp` keeps its
  history unclipped and its flash rule uses the neighbourhood's mean without its brightest sample. A light
  caught by one phase in 16 then averages over all the phases instead of blinking, and a lone glint does not
  flash.
- **Sprite depth** tested against the unjittered half-res scene depth at 0.95 of the range (see
  [City light sprites](#city-light-sprites)).

## The day side

### City albedo

`cityDayAlbedo()` and `cityDayGrid()` return a ratio to the pattern's mean, applied where the presence is high
and the footprint is below "Street layout to". The base map is greyed first (the city is the map's brightness,
not its hue), and the ratio is taken against the mean's luminance on that grey base.

- **Streets**: asphalt, 12 m (arterials 24 m), as exact box coverage.
- **Lots**: each block split into lots per side (the style's count, plus random), roofs with their own margins
  and offsets from the style's palette; big commercial lots (1-2 per side) along the arterials, where the
  night's bright strips are.
- **Perimeter blocks** (a courtyard ring about 14 m deep) and **slab rows** (2-3 long slabs), by district.
- **Yards**: lawn, paved or bare earth by style and density.
- **Trees**: individual crowns, one jittered tree per 8 m cell, 2.5-6.5 m, clustered by a 64 m field, more in
  suburbs; they become their expectation as the footprint passes 3-8 m.
- **Fields** inside low-density blocks (up to 80%), so cities thin into farmland; **parks** in the voids.

### Far structure

From aircraft or low orbit (80 m to 4 km per pixel), `cityDayFar()` adds km-scale structure: parks on the
layout's void field, bright industrial zones, and a mottle at 4096 to 512 m plus one at the footprint's own
scale. Where the procedural streets stop resolving (30-120 m), the legacy tiled city day texture comes back.

## Farms

`farmDayAlbedo()` draws fields where the day map is cultivated: not forest, desert or snow, flat, outside cities,
below 2.5-3.5 km, more than 300-800 m from the shore, and **settled** (the night map at mip 5, about 160 km,
between 0.0015 and 0.008). The settled test keeps fields off the outback, the Amazon and the Congo, which the
colour test alone would farm.

Fields are rectangles from a recursive split of blocks (`farmRects()`). The style is chosen per area of about
130 km, by region:

| Style | Where | Layout |
|---|---|---|
| **GRID** | the Americas, Australia, some steppe | mile sections (1609.34 m), split into quarters and fields; 20 m section roads; centre pivots where dry, with fallow corners; 7 m crop rows |
| **STRIPS** | Europe, Africa, the Middle East, dry Asia | 300-700 m blocks split into long strips, orientation per 2 km cell, a track along its edge, hedges where green; 5 m rows |
| **PADDIES** | monsoon Asia where green | small bunded plots, a regional share flooded |

Flooded paddies (`farmWet`) reflect the sky (Schlick, F0 0.02) and glint the Sun in the terrain lighting.
Farms fade over 0.55-0.9 of "Ground pattern range" (800 m per pixel) and give way to solar parks.

## Beaches

`beachAt()` lays sand 40-140 m wide (a 600 m noise) on low, gentle shores: within 3-9 m of the water level
and nearly flat (`smoothstep(0.93, 0.985, n.up)`), measured from the height function's own waterline
(`shoreSignedDist()`, including the coves). The sand darkens and wets toward the water, and the waterline is
anti-aliased with the sea's ([The sea](sea.md#the-shore)). Beaches also select the close-up sand texture. They
need "Terrain materials" and fade out by a 200-350 m footprint.

## Solar PV parks

Reflector targets of kind `solar` ([Reflector targets](../modding/reflector-targets.md)) are drawn as PV parks.
Their area is `area_km2`, or 2.2 ha per MW of `capacity_mw` (at least 1 km^2). The mount is single-axis
north-south trackers in the Americas, India and Australia and fixed tilt elsewhere, unless `"mount"` says
otherwise.

### CPU side

`fillSolarSites()` (`src/simulations/SatelliteSim.cpp`) picks the 8 nearest solar sites within 2500 km, in
double, as `GpuSolarSite` (80 bytes) appended to the ground-beam buffer. Each centre is relative to the
observer's sea-level point, the frame of `q`, so the shader's subtraction is float-exact and the rows are
world-fixed.

- **Trackers** turn toward the Sun while it is up at the site; at night toward the strongest Reflect beam
  landing on the park; else they stow flat. The angle is eased over about 8 s and clamped to +-55 deg.
- **Fixed tilt** faces the equator at `clamp(0.8 |lat|, 10, 35)` deg.
- Row pitch and panel width: 5.8 / 2.4 m for trackers, 8 / 4 m for fixed.

### Drawing (`solarSiteAt()`)

- **Layout**: plots of 4 x 4 blocks in or out of a wobbly outline; blocks of whole rows with 6 m roads; rows and
  segments as exactly box-filtered pulse trains, so there is no aliasing at any footprint and the park is a dark
  patch from orbit.
- **Coverage as seen**: a panel of width \( W \) tilted to \( n \) covers \( W\,|n_z + n_x\,d_x/d_z| \) of ground
  along the view. Seen across the rows, the panels hide the ground. The same projection along the Sun casts
  their shadow on the visible ground.
- **Glass** (`solarGlint()`): a Beckmann lobe (F0 0.03) with Smith masking, like the sea's glint; it reflects
  the sky with Fresnel at the panel normal and glints the Sun. Per-row tilt jitter of about +-1.5 deg widens the
  lobe once unresolved. Panel backs are a grey backsheet with no glass reflection.
- **Beam glint**: in the ground-spot loop each Reflect beam's light (`GroundBeam::dirOct`) glints off the panels.
  Seen from the mirror direction, each beam's glint lands on other rows, so a park under the beam ring shows a
  glitter band. A beam spot's diffuse light also follows the surface's brightness.

### Rooftop PV

Where a park overlaps a city, ground blocks drop out block by block with the city's presence, and the park's
smooth radial density passes to the city pattern (`gCityPvK`). `cityDayGrid()` then puts panels on its own
roofs, per lot by hash: commercial, perimeter and slab roofs get rows at a 2.2 m pitch over most of the roof,
houses one array on one slope. Unresolved, the coverage is its expectation (12% of the ground at full rooftop
density), so it holds to orbit. Rooftop panels tilt about 15 deg toward the equator with +-8 deg per roof, and
join the park's glass lighting.

Toggle: `clouds.solar_arrays` (boolean, default on; settings.json only). Harness: `debugview solar` (R panel
coverage, G park ground, B panel normal east).

## Settings

Night lights tab unless noted; all under `clouds` in `settings.json`.

| UI label | Key | Default | Effect |
|---|---|---|---|
| City street lights | `city_lights_strength` | 1 | the procedural lights and city layout; 0 = the legacy detail textures |
| Major road lights | `city_roads_strength` | 2 | real major roads |
| City light sprites | `city_sprite_gain` | 4 | far city points |
| City sprites from (m/px) | `city_sprite_start_footprint_m` | 18 | sprites only where a pixel spans more ground than this |
| Ground glitter under sprites | `city_sprite_ground` | 1 | glitter kept where sprites carry the light |
| City light twinkle rate | `city_twinkle_rate` | 4 | scintillation speed |
| Lamp posts to (m/px) | `city_posts_footprint_m` | 27 | see [Layers by footprint](#layers-by-footprint) |
| Street grid to (m/px) | `city_grid_footprint_m` | 380 | |
| Major roads to (m/px) | `city_roads_footprint_m` | 1300 | |
| Street layout to (m/px) | `city_layout_footprint_m` | 2000 | |
| Street light share (near) | `city_street_share_near` | 0 | streets vs glitter close up |
| Street light share (far) | `city_street_share_far` | 0.13 | streets vs glitter as the grid fades |
| City light on clouds | `ambient_gain` | 1 | the clouds' city glow ([Clouds](clouds/march.md)) |
| City light blur LOD | `city_light_blur_lod` | 8.1 | blur of the lights seen through cloud |
| Night sky light | `terrain_night_sky_light` | 0.2 | see [Terrain](terrain.md#lighting) |
| Ground pattern range (m/px), Terrain tab | `ground_pattern_range_m` | 800 | where farms end |
| (none) | `solar_arrays` | true | solar PV parks |

## Cost

Measured on an RTX 3070 Ti with the harness, the city pattern costs about +1.1 ms over Los Angeles from 10 km by
night (pattern 0.9 ms, roads 0.2 ms) and +0.8 ms by day; farmland about +0.35-0.5 ms; solar parks within noise;
city sprites about 0. On the lighter presets the pattern costs about +0.7 ms on Low and +2.4 ms on
Planetarium over Los Angeles from 1.5 km.

## Where in the code

| File | What |
|---|---|
| `shaders/sat_sky.frag` | `cityFrame()`, `cityLayout()`, `cityGridOf()`, `cityStyleWeights()`, `cityLightPattern()`, `cityNightGrid()`, `cityLightFar()`, `cityGlitter()`, `cityLampColor()`, `cityRoadLight()`, `cityDayAlbedo()`, `cityDayGrid()`, `cityDayFar()`, `farmDayAlbedo()`, `farmRects()`, `beachAt()`, `solarSiteAt()`, `solarGlint()` |
| `shaders/include/terrain.glsl` | `cityTerrainLimit()` |
| `shaders/city_sprites.comp` | the far city sprites |
| `src/simulations/SatelliteSim.cpp` | `fillSolarSites()`, the city sprite dispatch, `createCityRoads()` |
| `tools/make_city_roads.py` | the roads bake |
