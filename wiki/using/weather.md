# Weather

The **Weather** window shows the storms of the simulated sky and takes you to them: a local rain radar, the
events worth seeing now (tornadoes, hurricanes, haboobs, supercells, squall lines, waterspouts, wildfires), the
storms near you, the named tropical cyclones with their tracks, and the main storm settings. This page explains how to
use it. How the storms are made and drawn is on [Weather](../simulation/weather.md),
[Tropical cyclones](../simulation/tropical-cyclones.md), [Wildfires](../simulation/wildfires.md),
[Haboobs](../simulation/haboobs.md) and [Storms](../rendering/clouds/storms.md).

Every storm is fixed when it forms: its whole life follows from the sim time. Time warp, reversing time and bookmarks
therefore see the same storms, and a storm found ahead in time is really there when the clock gets to it.

## Opening the window

The weather-cloud button on the bottom-right panel (between Missions and Bookmarks), or **N** ("Weather" in the key
bindings). The window has four tabs, **Radar**, **Storms**, **Cyclones** and **Controls**, and shows how many storms
are alive worldwide. A line under the tabs (amber) reports the result of the last button you pressed, for example
what a Watch has put in view; it disappears after 12 s. Nothing in the window is computed while it is closed.

## Watch: one button for every event

Every card and row in the window has a single button, **Watch**. It always lands on the event *already happening*:

- it moves the clock to a moment when the event is under way (forward to it when it is still to come, never to
  before it starts), sets time to 1x and unpauses it;
- it puts you somewhere the event is in frame, and ends Go to and Track;
- the line under the tabs says what you are looking at.

| Watch on | Where it puts you |
|---|---|
| A tornado | The funnel on the ground: now if it is down, else a third of the way into its life. You stand on the ground about 3.5 km away (3 km for a waterspout), looking at it with a 50 degree view. A moment later the view moves to the driest spot a few km round the funnel, so the rain does not hide it |
| A storm (a supercell, a squall line, a row in NEAR YOU, the storm selected on the radar) | Its tornado, if one is on the ground now. Otherwise the storm at its height (the clock moves forward to its mature stage while it is still growing), seen from 300 m above the ground about 35 km upwind (30 km for a line), looking 10 degrees up at it; then moved out of the rain the same way |
| A hurricane or typhoon | The whole storm now, from orbit over its eye (700 to 2500 km up depending on its size). If it is night over the storm, the clock moves on to its next daylight. A storm that is not alive now is shown at its peak |
| A haboob | On the ground 6 km in front of the dust wall, facing it as it rolls toward you. When the storm's cells are weak at the event's moment, the clock steps on a few minutes until its front raises dust |
| A wildfire | 150 m up, about 5 km beside the smoke column and 1.5 km upwind of it, looking a few degrees up; by day on the side with the Sun behind you |
| A cyclone milestone | That moment, the storm from orbit. A landfall is shown at the landfall, from just off the coast, across the surf; dissipation half an hour before the storm is gone |

## Radar

A local precipitation radar centred on you, north up. The rain it shows is the rain the clouds draw: the cloud map's
precipitation and the storms' rain cores, about 120 m above the ground. It needs the volumetric clouds, so it is not
available on the **Potato** preset.

- **Colours** follow the usual weather-radar scale in dBZ, from 10 to 70: rain green through yellow and red to
  magenta, snow in blues, mixed rain and snow in pinks. You are the coloured dot in the middle, with range rings
  around it.
- **Range**: 50, 150 or 400 km.
- **Clouds** draws the low cloud's cover faintly under the echoes. **Labels** names the storms; the number on a
  storm is its place in the Storms list.
- **Storm markers** sit on each storm's strongest tower. Hover one for its kind, stage, age and lightning; click it
  to select it, which shows its details and a **Watch** under the radar.
- **Tropical cyclones** in range are drawn with their tracks and names.
- **Loop** plays back the frames taken while the radar was open, one every 5 minutes of sim time, the last 10. Speed
  time up to fill it. Moving, or changing the range, starts it again.

The radar refreshes at most four times a second, and only when the time, your position or the range has changed.

## Storms

### Events

The tab opens with seven cards, always in the same order and always the same size, so nothing jumps about as storms
come and go. Each card has an icon, three lines and its **Watch** (greyed out when there is nothing to see).

| Card | What it holds |
|---|---|
| TORNADO | The next tornado touching down anywhere within 24 hours, preferring one in daylight, over land, in a drier region and outside the deep tropics. It says when it touches down (or that it is on the ground now and for how long), where, how long it lasts and how wide it is |
| HURRICANE | The strongest tropical cyclone now: its category in its basin's own term (hurricane, typhoon, cyclone), name, wind in m/s and knots, whether it is over land, and where. A second line names the next storm to become a major one (Category 3 or more) in any basin, and when |
| HABOOB | The next haboob within 48 hours: a squall line or mesoscale convective system whose gust front, by day, runs over desert and raises a wall of dust. It says when, where and over which kind of ground |
| SUPERCELL | The strongest supercell now (a mature one in daylight first): stage, place, strength, lightning, and when its tornado touches down if it has one |
| SQUALL LINE | The strongest squall line or mesoscale convective system now, the same way |
| WATERSPOUT | The next waterspout at sea within 24 hours, like the tornado card |
| WILDFIRE | The biggest fire burning now, among those whose front is actually burning: how hard it burns, its biome, the length of its front, how high the smoke rises and how long the fire has left |

Tornadoes, waterspouts and haboobs are found by looking ahead in sim time. The search runs in the background while
the window is open, and the card reads *searching ahead...* with how far it has looked until it has an answer. It
starts again when time jumps back before it, has moved 12 hours on, or the event it found is over.

### Near you

Under the events, **NEAR YOU** shows four fixed slots for the organised storms within 1200 km (multicells,
supercells, squall lines and mesoscale convective systems; plain single cells only when nothing organised is near). A
storm keeps its slot as long as it stays near. Each row has:

- its stage (*forming*, *growing*, *mature*, *decaying*) and kind, with how many of its towers are alive;
- its distance and bearing from you, and its position;
- its age, the time it has left, its strength and an estimate of its lightning in flashes a minute;
- a **milestone** when it has one: its tornado (*touches down in ...*, *on the ground now*, *touched down ... ago*),
  or, for a squall line or MCS heading your way, when its line reaches you where you stand;
- **Watch**.

An empty slot reads *No organised storm near you*.

## Cyclones

The named storms: every tropical cyclone, one per basin and season, with its whole track fixed when it forms.

- **The track map** shows the land and sea with every active storm's track, coloured by the Saffir-Simpson category
  at each point (bright where the storm has been, faint where it is going: the whole future track is known). The
  current centres are rings; click one to select that storm. **Zoom** fits the map to the selected storm's track;
  **World** shows the whole Earth.
- **ACTIVE NOW** lists every cyclone alive: category, name, basin, wind (m/s and knots), central pressure, motion,
  when it formed, whether it is over land and how far it is from you. Click a row to select it.
- **The selected storm's milestones**: genesis as a tropical depression, being named, reaching hurricane (or typhoon
  or cyclone) strength, the start of rapid intensification, Category 3, its peak, each landfall and its dissipation,
  each with its date and how long ago or how soon. The next one is highlighted. Each has a **Watch**.
- **SEASON** lists one basin's storms within about 200 days of now, past, active and to come (pick the basin: North
  Atlantic, East Pacific, West Pacific, North Indian, South-West Indian, Australia / South Pacific). Click a row to see
  its track and milestones; its **Watch** shows it now if it is alive, else at its peak.
- **NEXT TO FORM** names the next storm to form in each basin, when, and how strong it will get.

## Controls

The storm settings most worth changing. The same values, and many more (rain, lightning, the towers' shapes), are in
Settings → **Weather**, which **All weather settings** opens. Every value can be typed or stepped with **-** and
**+**; changing one sets the graphics preset to Custom.

| UI label | `settings.json` key | Default | Effect |
|---|---|---|---|
| Storm objects | `clouds_v2.storm_objects` | on | Off: no storms at all, only the cloud map |
| Storm frequency (x) | `clouds_v2.storm_frequency` | 1 | How many storms form. Changing it rebuilds every storm |
| Storm lifetime (x) | `clouds_v2.storm_life` | 1 | How long every storm lives. Changing it rebuilds every storm |
| Storm cloud field | `clouds_v2.storm_field` | 1 | The cloud each storm brings round itself |
| Storm clear moat | `clouds_v2.storm_moat` | 1 | The clear ring round a storm |
| Hurricane arm cirrus | `clouds_v2.storm_arm_cirrus` | 1 | The ice veils along a tropical cyclone's spiral arms (0 = none) |
| Storm surge (x) | `clouds_v2.storm_surge` | 1 | How far a tropical cyclone raises the sea on the coast (0 to 2) |
| Wildfires (x) | `clouds_v2.wildfires` | 1 | How many fires start (0 = none) |
| Wildfire smoke | `clouds_v2.wildfire_smoke` | 1 | How thick the smoke is |
| Smoke brightness | `clouds_v2.wildfire_smoke_brightness` | 1 | How much light the smoke scatters |
| Smoke tint (-1 blue .. +1 brown) | `clouds_v2.wildfire_smoke_tint` | 0 | The smoke's own colour; 0 is neutral grey |
| Smoke spread (x) | `clouds_v2.wildfire_smoke_spread` | 1 | The width of the plumes; the same smoke, thinner when wider |
| Smoke spectral (x) | `clouds_v2.wildfire_smoke_spectral` | 0.6 | How strongly the smoke reddens what is seen through it; 0 is grey at every wavelength |
| Wildfire light | `clouds_v2.wildfire_light` | 1 | The burning front's points of light and their glow at night |
| Haboobs (x) | `clouds_v2.haboobs` | 1 | The dust walls storms raise over deserts (0 = none) |
| Haboobs as cloud | `clouds_v2.haboob_field` | on | The dust walls drawn as billowing, cumulus-like cloud near you. Off: a simpler, smoother look everywhere |
| Lightning rate: Cb towers (/min each) | `clouds_v2.lightning_rate` | 1.1 | Flashes a minute from each full-grown storm tower |
| Lightning rate: storms (/min per 20 km) | `clouds_v2.lightning_storm_rate_per_area` | 0.53 | Flashes over the cloud map's stormy regions |

## Tips

- A tornado in very heavy rain can stay hidden behind the rain shafts even after the view has moved out of the
  rain. Lowering **Rain** in Settings → Weather shows it.
- The Weather window and the [Missions](missions.md) window can be open side by side; both use the same Watch rule.
- Storms move with the cloud map. To find the same storm again later, save a [bookmark](features.md#bookmarks): it
  keeps the cloud map's position as well as the place and time.

## Where in the code

- `src/simulations/SatelliteSimWeatherWindow.cpp`: the window (`buildWeatherWindow()`), the radar
  (`buildWeatherRadarTab()`, `weatherRadarRaster()`), the Storms and Controls tabs (`buildWeatherStormsTab()`,
  `buildWeatherControlsTab()`), the storm and tornado Watches (`weatherWatchStormNow()`, `weatherWatchTornadoOf()`)
  and the move out of the rain (`weatherTornadoFixTick()`).
- `src/simulations/SatelliteSimWeatherFeatured.cpp`: the EVENTS cards (`buildWeatherFeatured()`), the forward search
  and the cards' Watches (`weatherFeaturedAct()`).
- `src/simulations/SatelliteSimWeatherCyclones.cpp`: the Cyclones tab (`buildWeatherCyclonesTab()`), the milestones
  (`weatherCycloneMilestones()`, `weatherCycloneMilestone()`) and the hurricane Watch (`weatherWatchCyclone()`).
- `shaders/weather_radar.comp`: the radar's rain, read from the cloud field.
