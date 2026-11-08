# Changelog

## 1.2.0 (release date TBD)

The biggest update yet. The constellations now have a life cycle: rockets launch them, trains of new satellites
climb to their slots, crews and cargo fly to the stations, old satellites re-enter, and from the late 2010s to
2036 the sky fills up launch by launch. A scripted solar superstorm in July 2037 can set off a Kessler cascade.
Around them: a new weather system with storms, hurricanes, tornadoes, haboobs and wildfires; a night sky with the
real stars, resolved planets, comets, meteor showers and a supernova; a living Earth from the ground to orbit;
satellites you can fly up to and inspect as real 3D models; and a sim clock that runs on real UTC. Accepted photometric error is listed in
`data/benchmarks/KNOWN_RESIDUALS.md`; the full reference is the project wiki (`wiki/`).

### Launches and rockets

- **Launches keep the constellations flying.** Each orbital plane is refreshed once per design life: a rocket
  lifts off from a pad the plane passes over (real launch sites, plus a few fictional offshore
  platforms for the busiest years), deploys a train of satellites low, the train climbs over
  weeks to its slots, and the satellites it replaces deorbit and re-enter as fireballs. Everything is a function
  of sim time, so time warp, reverse playback and bookmarks show the same launches.
- **Real vehicles and pads.** Falcon 9, Starship (including Starbase Louisiana's ten pads), New Glenn, Vulcan,
  Ariane 6, Long March 2F / 5B / 6A / 7 / 8A / 12, Zhuque-3, Tianlong-3, Pallas-1, Soyuz-2, LVM3 / HLVM3 and H3, each with its own 3D model
  (ogive noses, fins and flaps, Starship's tiled heat shield, every engine bell), from the launch sites they fly
  from. Pads are drawn on the ground: graded concrete, flame trenches, catch towers and service structures, tank
  farms, landing zones, floodlights and beacons at night, with no city grid around them.
- **Full flights.** Staging, max-Q, fairing halves that tumble away, payload stacks released one by one (Starship
  releases its satellites through its payload door), satellites that unfold their arrays and radiators after
  release, and raising trains flying "open book" (bright, as real fresh trains are). Every upper stage deorbits.
- **Recovery.** Boosters fly back to their pad or landing zone, land on droneships (A Shortfall of Gravitas, Just
  Read the Instructions, Of Course I Still Love You, Jacklyn) that then steam home, or are caught by the tower.
  Starship's ship re-enters as an eight-minute plasma glow, belly-flops, flips and is caught by a tower.
- **Exhaust and light.** Trails that billow, twist in the winds aloft and spread at twilight into huge sunlit
  bells high above a dark ground; steam and exhaust clouds rolling off the pad at lift-off; dust or spray at
  landing; engine-sized flames; fuel-dump spirals. At night the engines light the clouds, haze, exhaust, sea and
  ground around them. The rockets' roar arrives at the speed of sound.
- **Selectable rockets and fireballs.** Rocket stages, a ship coming home and every re-entry fireball can be
  selected, tracked and followed (Go to rides a rocket through staging, deployment and re-entry).
- **Mission milestones.** Every launch has a timeline (lift-off, max-Q, staging, fairing, SECO, deployment,
  deorbit, landing, docking ...): the Info window's MISSION section, the follow HUD and the `]` / `[` keys skip
  to the next or previous one.
- New programs: **Qianfan (Spacesail)** in the roster (11,664 satellites), Chinese commercial launchers for
  Guowang and Qianfan.

### Crew and cargo flights

- **Every station is visited.** Crews rotate twice a year: Crew Dragon (ISS and the commercial stations),
  Shenzhou (Tiangong), Soyuz MS (the Russian station), Gaganyaan (Bharatiya Antariksh Station).
- **Cargo ships.** Progress, Tianzhou and Cargo Dragon resupply the stations; Cygnus, HTV-X and Dream Chaser are
  captured by the ISS's Canadarm2 and berthed, then released. The arm also shows in the Info window's 3D view.
- **Dream Chaser** flies once a year on Vulcan Centaur from SLC-41. Coming home it jettisons its cargo module (which
  burns up behind it), glides to Kennedy's Shuttle Landing Facility, touches down on the centreline and rolls to a
  stop on the runway.
- **The flights are flown.** Spacecraft are their own objects: phasing burns, approach and docking at real ports
  (fast-track rendezvous of a few hours for Soyuz, Progress, Shenzhou and Tianzhou), time docked, undocking, entry
  glow, parachutes and landing or splashdown; Progress and Tianzhou burn up.
- A destroyed station gets no more flights; a flight on its way calls off its rendezvous.

### The deployment era (2019-2036)

- **The constellations are built up over time** (on by default). Each fills by launches from its program's start
  (Starlink from 2019) along an S-curve through its 2026 count to its 2036 size. Go back in time and only the
  satellites launched by then are in the sky.
- Stations appear on their launch dates and are built module by module; launch sites come online on their own
  dates and the pads show the towers they had at the time; early Starlink flies on Falcon 9 before Starship.
- The Missions window's BUILD-OUT rows show each constellation's progress and launches a day (the build-up peaks
  around 2032 at roughly a hundred launches a day). Off: the full 2036 roster at every date.

### The Sun's Fury (Kessler syndrome)

- **A scripted Carrington-class solar storm** arrives on 15 July 2037 at 03:00 UTC, two days after the Great
  Australian Eclipse. Its aurora reaches the mid-latitudes, it kills a share of the satellites, and the dead
  tumble, drift out of their formations and collide. Each collision is a cloud of debris feeding the next: a
  Kessler cascade computed at physical rates with the NASA Standard Breakup Model, up to 50 years ahead, in the
  background and cached. Before the storm nothing differs from the plain sky.
- **What you see:** dead satellites tumbling and glinting; debris pieces drawn close up as torn solar arrays,
  radiator sections and pieces of bus, appearing at the impact's own instant (not a fraction of a second early,
  converging on the impact point); a glowing dust cloud along each breakup's orbit, which from half an hour to
  half a day old disperses into a dust band around the parent's orbit (a chain of clumps covering part of the
  orbit, a whole ring once it has wrapped) instead of staying a round cloud, and then stays a dust RING around
  the Earth for days to weeks, fading and widening across its orbit as the fragments' planes drift apart (in a
  late cascade the rings of a shell overlap into a faint band of dust across the sky); re-entry fireballs with a
  breakup flash, fragment trains and material colours (aluminium white, copper green, sodium yellow, lithium
  crimson); and once the cascade runs, many small meteor-like streaks.
- **Collisions up close:** the projectile flies in on its real approach line and can be selected; the satellites a
  collision kills stay whole in their own attitude until the instant of impact; the impact is a flash at its peak in
  the frame that holds it at any time warp (white-hot and brief: it no longer lingers as a brown glow over a nearby
  camera), with a sound (energy, mass and distance shape it: a fast crack and a short
  reverb up close, a low-passed boom from the ground); and the debris, its dust and its small fragments carry the
  pair's momentum (a head-on hit sends the cloud along the impactor's path instead of leaving it with the
  formation).
- **Explode** (Info window) breaks any satellite or debris piece up with one click, at any date; the debris is
  there the same frame. A breakup before the storm starts the cascade then (computed in the background while the
  clock holds briefly); going back in time undoes it. A second click while that runs is queued, not waited for.
- Settings: collision rate, collision avoidance, horizon, dust and small re-entry brightness, fireball colours;
  "Watch the next re-entry". Launches stand down after the storm (or resume three weeks later).
- **The default cascade ships prebaked** (three years after the storm), so jumping into it is quick instead of
  minutes of "Computing the debris cascade". A player's own breakups are computed live.
- **The Kessler tab** of the Missions window: live counts, a globe of the debris shells with the collision risk,
  collisions per day and the latest breakups, each with Watch.

### GEO belt

- **398 real geostationary satellites** by name at their slots (mid-2020s positions), with operator, mission
  and slot in the selection panel, on four new GEO models. The Constellations tab's LIST finds them by name.
- The Constellations tab is grouped by kind (broadband, stations, data centers, reflectors, GEO, science,
  benchmark shells, debris), folding, with group-wide ON / HLT.

### Clouds and weather

- **Anvils that last.** Thunderstorm anvils now come mostly from organised storm systems, spaced apart, and live for
  hours: they form once, hold while their storm feeds them, and thin away slowly. Ordinary storms no longer leave their
  own anvils behind (Weather > Storm objects > "Single-storm anvils"; 2 restores them), so the grey speckled discs seen
  from orbit are gone. An anvil's top is a continuous smooth sheet instead of a holed, mottled one.
- **No more cloud rows from orbit.** Near the equator the cumulus field no longer lies in straight north-south streaks
  and a grid when seen from space.
- **Cumulus turrets.** Cumulus and stratocumulus fields are no longer one smooth dome per cell: a 3D field of towers
  of varied height, taller than wide and leaning with the wind, rises out of every cloud with a convective share,
  so a shower field from an aircraft shows turrets standing over a lower mass instead of a sheet of gumdrops.
  Settings > Clouds: "Cumulus turrets" (1.0; 0 = the old shape), "Turret stretch", "Turret lean", "Turret skew".
- **Clouds stay white longer as the Sun sets.** The white balance now adapts to the Sun's colour 2 km up wherever
  the camera is, so it no longer had to be faded out from 11.5 degrees of Sun: sunlit clouds stay white-gold down to
  about 6 degrees (they went beige-khaki from about 10), turn orange only in the last few degrees, and a sea of cloud
  seen from an aircraft or from 45 km is adapted like the view from the ground. Settings > Photometry: "White balance
  holds to" (6 deg; 11.5 = before), "White balance from the eye" (0; 1 = the old reference).
- **Sunlit clouds read brilliant, shadows deeper.** The tonemap gains "AgX range" (1.4): more contrast between lit and
  shaded cloud without darkening the sunlit faces or crushing dark ground, and the day's auto exposure aims a little
  brighter ("Auto exposure target", 0.40; was 0.32), so it no longer stops sky-and-cloud frames down until lit cloud is
  grey. AgX saturation is now 1.35. A cumulus at a 5-degree Sun: lit faces about 25% brighter, lit/shaded 4:1 -> 5-6:1.
  The white balance now uses the clouds' own sunlight colour ("Cloud sunlight Rayleigh" and ozone), so high cloud
  seen from above no longer reads lavender. Settings > Photometry: "AgX range" (1 = before), "Auto exposure target",
  "White balance from cloud sunlight" (0 = before).
- **Deeper shaded sides on towering clouds.** The light that has scattered through a lumpy cloud now escapes through
  its valleys before it reaches the far side (fitted to a path tracer), so the shaded faces and crevices of storm
  towers are darker and bluer while the sunlit tops stay as they were; and the sky's light keeps its colour inside a
  cloud. Settings > Clouds: "Multi-scatter side escape" (0.11; 0.05 = before), "Sky light keeps its colour" (1; 0 = before).
- **Showers fall from the big clouds.** In a region of scattered showers the rain now falls as shafts under the strong
  cells only, instead of a thin curtain under every cloud base that wrapped the whole field in grainy haze; the
  cumulus there now have crisp outlines. Settings > Clouds: "Showers from cores" (1.0; 0 = rain under every cloud).
- **Cumulus are built from bubbles.** A cumulus is now a cluster of rising thermal bubbles of very different sizes
  instead of one dome per cell: sub-cells stacked at different heights, heads wider than their stems, creases and
  hollows between them, clouds that merge into irregular masses or break into fragments, from the ground, at cloud
  level and from above. Settings > Clouds: "Cumulus bubbles" (3; 0 = the dome as before), "Bubble relief" (0.2);
  "Turret stretch" is now 0.2 (was 0.5).
- **Cumulus bubbles at two scales, sized by the cloud.** The big bubbles now carry small ones on their faces (cauliflower
  on the cauliflower), small clouds are built of small bubbles instead of being one smooth oval, the bubbles meet in
  rounded fillets instead of hard creases, and they fade toward the base so a dense field stands on a flatter base
  with separate towers above. Whole clouds come in a wider, long-tailed range of sizes (rare large masses, more small
  ones). Settings > Clouds: "Bubble hierarchy" (1), "Bubbles follow cloud size" (1), "Bubble base flatness" (0.6);
  "Cumulus turrets" 0 no longer turns the bubbles off.
- **Cumulus clouds of many heights.** Within one cloud mass, towers kilometres tall now rise beside low shoulders, so
  a big cumulus reads as a skyline of separate heads instead of one dome with a single top. Settings > Clouds:
  "Cumulus tower spread" (0.75; 0 = one top per cloud, as before). "Cumulus core" (0.5) keeps the bubbles and turrets
  from cutting a big cloud through into fragments: they shape its surface, and the mass stays whole. "Cumulus bubble
  warp" (150 m) turns the bubbles from perfect spheres into lumpy ones (no rings or polka dots from below), and "Cumulus
  head width" (0.5) gives towers heads wider than their stems.
- **Volumetric clouds, rebuilt (clouds v2).** One 3D cloud field from the ground to orbit, driven by a weather
  map: cumulus with lobed, cauliflower sides and flatter bases, stratus decks with textured undersides,
  altocumulus and altostratus, cirrus streaks, veils and cirrocumulus, all lit by the Sun or the Moon through
  the clouds above them, with sunset colours, silver linings and soft cloud shadows on the ground (long streaks
  at sunset), far cloud shadows and god rays, and mid / high layer shadows at any distance.
- **Fibrous cirrus.** Cirrus is drawn as tufts of thin bright filaments with clear sky between them, with denser
  heads and fall streaks that hook away below them, instead of a smooth marbled veil. Settings > Clouds: "Cirrus
  fibres", "Cirrus hooks".
- **Cloud bases vary, and mountains wear lenticular clouds.** Low cloud bases now sit at different heights from
  region to region and cloud to cloud: lower over the sea and in rain, high over dry land on a sunny afternoon. Ragged
  fractus hangs under raining bases, thin wisps drift between the cumulus tops and the altocumulus, and smooth
  lenticular clouds form over and downwind of mountain ridges, where the mid-level cloud rides the air's standing
  waves over the relief (Fuji, the Alps, the Himalaya), fixed to the ground while the weather drifts past, and low cloud
  parts around high massifs instead of cutting through them. Lenticulars come and go: they need a moist, stable
  "wave-cloud day" that passes over a region every few days, they form as separate almonds along a crest rather than
  one ribbon down a whole range, and they are lit like the clouds around them.
  Settings > Clouds: "Cloud base condensation level", "Cloud base cell spread (m)", "Cloud base regional spread",
  "Fractus (ragged scud)", "Mid-level wisps", "Orographic clouds", "Ridge wind (m/s, 0 = auto)", "Ridge wind from (deg, -1 = auto)".
- **Billowier cumulus.** The cauliflower lobes on cumulus sides and tops come from a new "billow" noise matched
  against a production cumulus model (the Disney WDAS cloud): round lobes of many sizes meeting in sharp creases,
  instead of pointed bumps over broad valleys, down to the fine erosion detail. Settings > Weather, "Lobe noise (0 cones, 1 caps, 2 billows)";
  billows are the default. Same cost per frame.
- **Storms.** Cumulonimbus towers grouped into storm cells (a dominant tower and a flanking line), flared anvil
  heads under the tropopause, overshooting domes, rain shafts under the towers, cumulus that rises around storms.
- **The weather moves and evolves.** The cloud map drifts, is advected by a slow wind, and storms grow and decay
  over hours; over land, afternoon convection builds storms that die back after dark.
- **Rain, snow, sleet and diamond dust** drawn as real particles at your eye: drops sized and falling like real
  rain, flakes that flutter, blown by the wind, gusts and a storm's outflow, lit by the sky, by lightning and by
  the Reflect Orbital beams. The precipitation type follows the air temperature. Each particle takes its light
  from the scene it is in front of: it can no longer be darker than the sky or cloud behind it (snow under a
  thick deck used to read as near-black dashes over sunlit cloud), and it keeps its own brighter-than-the-scene
  look where it has one - rain against dark cloud or sea renders exactly as before.
- **Lightning and thunder.** Whole storms flash: cloud-to-ground strokes with branches and tendrils, in-cloud
  flashes, spider lightning and rare red sprites. Clouds glow around each flash; thunder rolls in from the
  channel's shape, delayed by distance.
- **Rainbows, halos and ice optics** for the Sun or the Moon: rainbows at storms, 22-degree halos, sundogs, the
  circumzenithal arc, sun pillars.
- **Fog, dust and ice fog.** Radiation fog in valleys at night, sea fog on coasts, dust plumes over dry land,
  diamond-dust ice fog over Antarctica and Greenland, patchy by region the way the dust is.
- **Physical light.** Multiple scattering in the clouds (dark wisps, bright cores), the sky's light on clouds and
  ground from the whole dome, multiple scattering and ozone in the sky (blue-violet twilight), the ground under an
  overcast lit by what the clouds let through, and an AgX tone map that keeps the hue of bright highlights.
- **Droplet phase.** Clouds scatter light by their droplets' size: small droplets at a cumulus base, larger ones high in
  a storm tower, ice in anvils and Cb heads. Thin wisps keep the droplets' peaky forward phase (dark outlines from the
  side, a bright silver lining toward the Sun) while light that scatters deeper turns isotropic, so thick cores are
  bright. Settings: "Droplet phase", "Droplet size", "Droplet multi-scatter gain" (Clouds tab).
- **Clouds from orbit.** Satellite-imagery morphology (closed and open cells, cloud streets, popcorn cumulus from
  MODIS scenes) shapes clouds seen from space; from high orbit a full-resolution far layer takes over. The clouds
  stay the same clouds as you climb.

### Storms, hurricanes and wildfires

- **Storms are objects with a life.** Air-mass cells, multicells, supercells, squall lines and mesoscale complexes
  form at a thunderstorm climatology, grow, mature and decay over hours, each with its towers, anvil shield
  (lingering as cirrus after the storm), rain core, cloud field and lightning. A pure function of sim time.
- **Anvils spread and fade slowly.** A thunderstorm's anvil now spreads over about an hour after its tower reaches
  the top of the troposphere (the tower stands flat-topped first) and lingers one to four hours after it dies, instead
  of popping in and out within a few minutes in time warp. Settings > Weather: "Anvil formation time (min)" (60) and
  "Anvil decay time (min)" (70). Thunderstorm towers climb to the anvil over a "Tower growth time (min)" (30) instead
  of standing up in a couple of minutes.
- **Hurricane shape.** A hurricane now shows two main spiral arms by default with minor swirls between them; its arms
  narrow and break along their length instead of being cut off square; the central dense overcast winds out into the
  arms instead of sitting on top as a round glob; and the cirrus over the arms and the outflow cirrus follow the same
  spirals (the outflow in thin streaks curving the other way). Settings > Weather > Storm objects: "Hurricane main arms",
  "arm tightness", "arm speed", "minor swirls", "deck spiral", "outflow cirrus", "outflow streaks".
- **Hurricane tops are a smooth swirl.** The central dense overcast is a smooth, bright sheet with fine streaks
  running along the swirl and an edge that frays into streaks, instead of a grey sheet of cumulus lumps and pits.
  Settings > Weather > Storm objects: "Hurricane top smoothing", "Hurricane top streaks (m)", "Hurricane top density (x)".
- **Tropical cyclones.** About 80 a year across six basins, named from the WMO lists, following climatological
  tracks with landfalls and recurvature: an eyewall, a clear eye, spiral rain bands, outflow cirrus, a whipped-up
  sea, wind that drives the rain and the ambience, and a storm surge that floods low coasts.
- **Tornadoes and waterspouts** under supercells and strong storms at sea, hanging from the storm's real base,
  spinning, with a wall cloud, debris cloud and roar.
- **Haboobs.** A storm's gust front over dry land raises a towering wall of dust, drawn as beige billowing cloud.
- **Wildfires.** Fires start by biome, season and dryness and spread with a coherent wind: a ragged flaming front
  with embers, smoke that follows real trajectories into a regional pall, burn scars, and their glow at night.
- **The Weather window** (the cloud button, or N): featured events (the next tornado, the strongest hurricane, a
  haboob, a supercell, a squall line, a waterspout, the biggest wildfire), storms near you, a local precipitation
  radar with a loop, and the named storms with track maps and milestones. One **Watch** button per event lands on
  it as it happens.

### The night sky

- **Sky events window** (the crescent button, or K): solar and lunar eclipses, planetary conjunctions, lunar
  occultations, Moon-base glints, meteor showers and comets for the next two years, each with one Watch that puts
  you where and when to see it.
- **Real stars.** The HYG catalogue to magnitude 8.5 (about 62,000 stars, 417 with names): faint stars appear only
  under dark skies, stars are selectable with name, type, distance and colour, and twinkle with the air's real
  scintillation.
- **Betelgeuse goes supernova** on 25 December 2037: near magnitude -11 at its peak, visible by day, lighting the
  night landscape like a second Moon, and fading over two years.
- **Resolved planets.** Zoomed in, Mars, Jupiter and Saturn show their surfaces, Saturn its rings, Venus its
  phase and forward-scattering rim, Jupiter its four Galilean moons (with transits and shadows). Planets keep their
  true colours.
- **Comets.** The Great Comet of 2037 (a sungrazer in February, 560,000 km from the Earth in March) and a few
  minor comets a year, with dust and ion tails drawn as 3D particles.
- **Meteor showers.** The annual showers and the sporadic background at their real rates, with fireballs,
  fragments and glowing trains; a time-lapse view under time warp.
- **The Moon's real surface.** LROC colour and LOLA relief with true libration and mountain shadows along the
  terminator; lunar south-pole bases whose solar arrays mirror the Sun to Earth near full Moon.
- **The Milky Way** stays crisp zoomed in (star clouds, dust lanes, Andromeda), with a ground look and a space look.
- **Light pollution, physically.** One skyglow model, calibrated against measured sites, now dims the Milky Way,
  zodiacal light, aurora, comets and the faint stars alike.
- **Refraction, mirages and seeing.** A ray-traced atmosphere flattens and lifts the setting Sun and Moon, draws
  inferior mirages and green rims, and makes the image boil near the horizon. The Sun has sunspots and, at totality
  or behind the new occulter, a dynamic corona with streamers and CMEs.
- **Seeing that follows the air.** Zoomed in, the turbulence moves the picture by its physical amount at the
  right scale: fine shimmer and a slight softening (a sea horizon at sunset moves ~3 arcsec rms) instead of big
  chunky wobbles. It is tied to the sim clock (pausing freezes it, time warp speeds it up) and to the air in front of
  you (it stays on the scene as you turn, and changes when you move). Stars twinkle in brightness, more toward the
  horizon. Three sliders under Sky > Seeing: Turbulence strength, cell size and speed (they replace "Star twinkle
  rate" and "Turbulence warp").
- **Inertial camera lock:** keep the view fixed on the stars while the Earth turns beneath, with a Sun occulter.

### Sky, sea, terrain and cities

- **Aurora driven by space weather.** The auroral oval is a ring fixed to the Sun; its size and brightness follow
  a Kp index from a model of the solar cycle, recurrent streams, CME storms and substorms. Curtains are thin
  folded sheets with sharp lower borders, rays and red tops; big storms add arc systems, red glow, magenta borders
  and rare blue rays. The aurora now dims under real skyglow (it vanished near towns before). "AURORA NOW" shows
  the activity; Kp can be set by hand.
- **Real time, real eclipses.** The sim clock is real UTC and the Moon follows the standard lunar series (true
  position, size, phase, tidally locked). Eclipses happen when and where they really do (total lunar eclipse
  31 Jan 2037, total solar eclipse 13 Jul 2037 over Australia); at totality the Moon's shadow darkens air, ground
  and clouds point by point, the horizon turns orange and a fibrous corona with prominences appears.
- **Terrain detail.** A true 3D surface with metre-scale relief and branching erosion gullies, soft terrain
  shadows, snow and rock, close-up ground textures; lakes at their own level and 10 m coastlines, beaches; the
  night side lit by the Moon and the night sky.
- **The sea.** Seamless waves however far you travel, a sea state that follows the weather, whitecaps, breaking
  surf, a physical Sun glitter path, sky and cloud reflections, a rougher, darker sea toward the horizon.
- **Procedural cities.** Street grids, lamp posts in sodium and LED colours, signs and traffic lights, real major
  roads, rooftops, yards and tree crowns by day, nine regional styles; distant cities glitter as fields of crisp
  points; cities stop at the foot of the mountains. **Farmland** in regional styles and **solar parks** at the
  Reflect Orbital target sites (glinting the Sun and the beams).

### Satellites and photometry

- **Satellites as real 3D models.** Every type has a geometry model (ISS, Tiangong, Hubble, Starlink v1.0 /
  VisorSat / v1.5 / v2 Mini / Direct-to-Cell / v3, OneWeb, Amazon LEO, Guowang, Qianfan, Starmind AI1, Reflect
  Orbital mirrors, the Starship depot, the GEO satellites, debris). The commercial and national stations (Haven-1/2,
  Axiom, Orbital Reef, ROS, Bharatiya Antariksh Station, Starlab) were redrawn from their operators' concept art,
  and the ISS and Tiangong gained visiting vehicles, arms and detail. Any satellite big enough on screen is drawn
  as a mesh reflecting the sky and Earth, with a seamless hand-off from its point of light.
- **Physical brightness.** Magnitudes come from the models (microfacet reflection, earthshine from the whole lit
  Earth, backlit translucent arrays, optional shadowing between parts) and are validated against published
  observation campaigns. Resolved satellites fade from a point into their surface brightness, formations merge
  into one point, and glares clustered together share a soft glow instead of a sea-urchin of spikes.
- **Info window and 3D view** (live or studio light, pops out into its own window), **Trace pass** with CSV
  export, **Go to** (with Stay here, Return and follow again), **Track**, and **bulk export** of simulated
  observations.
- **Reflect Orbital's quad-sail mirror** in the layout of its published deployed configuration, at the 2036-era
  55 m size: four triangular film quadrants (2,819 m^2) with scalloped edges on two crossed telescoping booms and a
  central bus, replacing the single plate. After release the booms telescope out and the sails unfurl over 15
  minutes. Reflect Orbital launches on Starship only, 25 mirrors a ship (a stowed mirror is a 5.4 m cross of
  telescoped booms 0.8 m thick: 25 fill the payload bay), released through the payload door 2.5 minutes apart so
  the 78 m sails never unfold near each other, about 40 launches a year once the constellation is up.
- **SpaceX Starmind orbital data centers** per SpaceX's May 2026 FCC filing: about 990,000 satellites in
  sun-synchronous and 30-degree shells, flying in passive formation rings of eight that turn once per orbit.

### UI

- **Missions window** (the rocket button, or M): the next launches, pinned twilight and crewed launches, the Sun's
  Fury countdown; what is flying now (crew transfers, stations, launches, trains, retirees, build-out); and the
  Kessler tab. Every row has one **Watch** button that lands on the event as it happens.
- **Settings search** across every tab, a new **Sky** tab, and every control that changes the look has a row in
  Settings.
- **Video recording** (F6, or the record button) to MP4, with or without the UI; cinematics export video too.
- **Mission milestones.** Skip to the next or previous milestone of a launch, train, retiree or the satellite in a
  slot (`]` / `[`, the follow HUD, the Info window's MISSION section with a progress bar by milestone).
- **Issue reports.** F9 adds a shot (frame, place, camera, time, weather, settings, frame times, the last 12 s
  of camera motion) to an issue, F7 starts a new one; the Issue reports window adds notes, pins and areas, and
  exports a zip to send.
- **Bookmarks** with thumbnails; **ten default bookmarks** (the Great Australian Eclipse, the Anchorage blood
  moon, a solar storm from orbit, beam-lit snow at McMurdo, a rainbow over Venezuela, Starmind formation rings,
  The Sun's Fury, a twilight Starship launch, a Starmind deployment, a super typhoon over Luzon).
- **Cinematics.** A camera-path editor with shots, follow shots, a timeline, saved files and frame export
  (preview, or supersampled HQ frames with motion blur).
- **HQ photo** (F8), screenshots with or without the UI (F10 always with), star trails.
- **UI themes** (Classic, Navy, Night red, Midnight by default) and window opacity; one shared style for every
  window and settings tab; grouped settings tabs with advanced ones behind "Show advanced settings".
- **First-run tutorial** (keyboard, mouse or gamepad), **loading screen** with progress, and a **startup graphics
  chooser** (Full graphics, Planetarium or Potato) before anything heavy loads, stepping down a tier on its own if
  the first frames are very slow.
- **Type instead of drag** for slider values, the clock, latitude, longitude and altitude; **gamepad layout 2**;
  movement that scales with height.

### Sound

- **Ambient sound.** Location-aware ambience under the music: winds, surf and gulls, crickets and birds, jungle
  day and night, city traffic, rain, thunder, rocket roar, and drones and pads in orbit, all in the music's key.
- **Music.** A player with previous / pause / next and a gap between tracks; new tracks LEO Motif and BIOS, each
  with an "upwell" stem that rises when Reflect Orbital flares fill the screen. The music fades out toward
  geostationary altitude.

### Performance

- **Two frames in flight:** the CPU prepares the next frame while the GPU draws this one (frames about 5-10% faster).
- An **aerial-perspective volume** computes the sky's atmosphere once per screen tile (about a quarter off the sky pass).
- **Temporal anti-aliasing** of the terrain, sky and sea, which is also a **temporal upscaler** below 100%
  render scale; render scale can scale the clouds too; optional automatic render scale.
- **Presets:** Potato and Planetarium (the light v1.1 skies, with the 1.2 ground features compiled out and no
  volumetric clouds), Low, Medium, High and Ultra. First runs start on High for discrete GPUs with 6 GB or more,
  Planetarium on integrated GPUs, Potato on old Macs.
- Large performance work across the cloud march, the sky pass, the terrain depth pyramid, the satellite pipeline
  (about 1.4 million satellites in the default roster), the launch system (plume builds and batch builds off the
  main thread, culled exhaust), close-up station shadows (from 30-60 ms to about 1 ms) and frame hitches after
  loading and at high time rates.
- **Hitches.** The first frame no longer stalls (boot about 9-10 s on a warm cache, models baked in parallel);
  ambience voices load asynchronously; the debris pool grows, uploads deaths and frees old buffers without stalls
  even at 1 year/s; launch batches build within a per-frame budget, and after a time jump (a bookmark, a typed time,
  a Missions Go to) or at fast warps the bulk builds on a background thread, so a jump to a busy year fills in about
  a second. Pad-deconfliction lists are built in parallel. `hitch_check.py` gates frame hitches.
- **Flying through storms.** Below 6 km the clouds' empty-space map now stays on while you fly (it used to switch
  off above ~2.4 km/s, so every step through a storm's layers paid for the full cloud), and a storm's smooth
  altostratus sheet is shaded at half the samples. Between a storm's decks at 4.8 km: about 22 -> 17 ms of cloud
  standing still and about 50 -> 28 ms flying at 5 km/s (RTX 3070 Ti, 1080p, High). Since then at any height inside
  the cloud layers too, and at up to 5 km a frame: flying over storm tops at 12 km about 54 -> 45 ms of cloud, walking
  an anvil top at 11 km about 75 -> 67 ms (RTX 3070 Ti, 1600x900, High), and the moving frames are closer to the
  settled ones than before.
- **Storms are made of the same cloud as the cumulus.** Thunderstorm towers, their anvil heads and the anvil
  plate (and a hurricane's eyewall and dense overcast) are now shaped by the cumulus layer's own billowing lobes and
  erosion instead of a separate noise of their own, and an anvil's thin outer veil is drawn as ice at half the
  samples. Inside an anvil at 11 km the cloud time drops by about 40% (49 -> 29 ms standing still, 58 -> 37 ms
  flying; RTX 3070 Ti, 1080p, High); storms seen from above cost about the same as before. Setting (no UI):
  `clouds_v2.storm_unified` (1; 0 = the previous storm drawing).
- **Anvils, polished.** Neighbouring storms' anvils merge into one plate (a dark channel no longer cuts across an
  anvil where a smaller storm's shield overlaps a bigger one), the plate's edge thins out instead of standing as a
  cliff of billows, overshooting tops stand out of the anvil again as domes (the dark dots seen from orbit), and the
  billows of storm towers rise up the column again.
- **No more mushrooms on anvils.** A thin stem with a round cap no longer grows out of the top of a thunderstorm's
  anvil. An overshooting top is now a broad, low swell of the tower's head pushing up through the anvil's lid,
  rising and settling back over 10-20 minutes, instead of a smooth dome standing on the anvil.
- **Anvils are made of the same cloud as the cumulus.** A thunderstorm's towers and anvil now take the cumulus'
  density, lighting, billows and bubbles; only their shape is the storm's. The anvil plate is no longer a thin ice
  sheet of its own density, and it meets the cumulus around it without a dark seam.
- **Anvils are fibrous ice, not eroded slabs.** An anvil's plate no longer has holes punched through it: its billows
  only bulge (gentle domes on top, pouches underneath), its erosion is mostly gone, and its ice is combed into
  fibres along the wind that open into wisps toward its edges; the thin veil around it frays into the same fibres.
  Thunderstorm towers take the cumulus bubbles and turrets, so they read as the same cauliflower cloud as the
  cumulus at storm scale. Inside an anvil at 11 km the cloud time drops by a further ~25% (22 -> 16 ms, RTX 3070 Ti, High); an anvil seen from
  high above costs a little more (~+2 ms). Settings:
  "Anvil fibres" (`clouds_v2.anvil_fibres`, 1), "Anvil erosion" (`anvil_erosion`, 0.2), "Storm bubbles"
  (`storm_bubbles`, 1); no UI: `anvil_fibre_along_km` (15), `anvil_fibre_across_m` (1500), `anvil_lid_lobes` (0.25),
  `anvil_under_lobes` (0.7).
- **Storms seen from above lose their craters.** From orbit and 30 km at sunset an anvil was a capsule with a bright
  halo, a dark ring round it and a dark inside: the thin ice veil round the plate was lit through its whole depth
  under a grazing Sun (now it shades itself along the slant path to its top, as the plate does), and it ended
  before the plate's edge had any density, leaving a dark gap. Anvil outlines are less capsule-like: a sharper,
  flatter upwind edge and a wandering outline that keeps its shape as the storm drifts. A hurricane's central dense
  overcast is opaque ice ("Hurricane top density" `clouds_v2.storm_cyc_top_density` 3 -> 12, range now 1-20): at 3
  the low cloud under it showed through as a whiter, raised plateau beside the eye. Overshooting tops in the tropics
  are no longer cut flat by the top of the cloud volume (black blots from overhead).
- **Cloud ray budget.** The full-resolution clouds march one pixel in four each frame and rebuild the rest from
  history; a settings key (`clouds_v2.sparse_block`: 2, the default, or 3 / 4 for one pixel in 9 / 16) trades
  sharpness in motion for speed in dense storms and anvils (an anvil top at 11 km, flying: about 121 -> 68 ms of
  cloud at 3, 47 at 4; standing still 49 -> 36 / 29).
- **Cheaper clouds while flying.** Smooth stretches of cloud and clear sky are now marched at reduced density while
  the camera moves too, not only once it has stood still for a second: in motion the clouds traced 1.6-2.3x the
  rays than standing still. About 8-20% less cloud time while flying over anvils and storm tops (anvil at 11 km,
  10 fps: 77 -> 72 ms; storm trail at 12 km, 60 fps: 36 -> 32.5), the image within the run-to-run noise. Setting (no
  UI): `clouds_v2.smooth_moving` (0.5; 0 = the previous behaviour, 1 = more savings, slightly blockier edges at low
  frame rates).
- **Clouds skip absent layers.** The empty-space map also records which cloud layers (storm towers, low, mid,
  high, anvils) each cell can hold, and the cloud march computes only those there: about 10% less cloud time in
  storms and over cumulus (storm at 4.8 km 14.2 -> 12.6 ms, flying through it 21.4 -> 19.1, cumulus 9.3 -> 8.4;
  RTX 3070 Ti, 1600x900, High), the image unchanged.

### Tools

- **Automation harness** (`tools/harness/`, `docs/HARNESS.md`): scripted and live runs, captures with state
  sidecars, perf and knockout sweeps, frame-hitch reports, UI layout dumps, camera paths, temporal-stability and
  climb benchmarks, offline audio rendering, issue-report triage (`reports.py`).
- **Mission-operations suite** (`mission_ops.py`): every launch vehicle and program traced phase by phase
  (accelerations, staging, insertion, release, arrival, docking, landing) with PASS / FAIL thresholds, and a
  launch-system cost benchmark (`launch_bench.py`).
- **SatModelTool** (bake, validate, OBJ export, self-tests, SatBench benchmark runs, trace replay) with nine
  published photometry datasets and a photometric accuracy gate in CI.
- Generators for rocket, crew vehicle, ship, pad, station, GEO and debris models, the launch-site coasts, the
  GEO belt, icons, ambience, water map, roads, terrain materials and cloud morphology.
- **Project wiki** (`wiki/`, MkDocs) as the maintained reference.

### Changed defaults and behaviour

- Defaults are the author's tuned look (clouds, sea, aurora, extinction 0.18); automatic render scale is off by
  default; HQ photos at 1x the window; rain amount 3, rain fall speed 5x. The Milky Way, the launch exhaust and the
  meteors carry the author's tuned settings too: a dimmer band whose star clouds are in at nearly every field of view,
  plume opacity 0.3 with a long, wide jellyfish, fireballs at a tenth the old rate and trains at 7% of their decay time.
- Saved times now show a different local time of day (the clock is real UTC; about 5 h 15 min earlier for the
  same timestamp). The start time moved so the intro is unchanged.
- "Storm strength" for the aurora is replaced by Kp (automatic by default).
- Glare defaults are the distance-tuned values; glare grows when the camera is close to a satellite.
- The menu buttons (Issue reports, Missions, Weather, Sky events, Bookmarks, Cinematics, Settings) are on the
  bottom-right panel; the Camera settings tab is now the Controls tab's Mouse section; launch and Sun's Fury
  settings are in Settings > Launches.
- Go to moves you: magnitudes and sky positions are read from where the camera is. With a satellite selected,
  I opens its Info window, G is Go to and T is Track.
- "Moon size" defaults to 1 (the true size).
- Exposure, auto exposure, highlight roll-off, white balance and the Moon's size are in a new **Exposure and
  Moon** section of the Photometry tab (settings keys unchanged). The "City light blur LOD" slider is gone.
- The disabled legacy-lighting Starlink Gen2 comparison shell was removed from the roster.

### Fixed

- Auto exposure no longer brightens the whole picture while a window is open: it meters the frame without the UI.
  With the Settings window open the clouds came out ~1.4x brighter (+0.5 EV) than with it closed, so cloud bases
  looked too bright while you were tuning them (issue_20261105_181849).
- Shaded cloud no longer shows a pattern of rings and cells repeating every 64 pixels, in motion or standing still
  (issue_20261105_181702).
- The shaded grey-blue lobes and crevices of cumulus now show on every cloud: past ~20 km, under time warp and right
  after it, not only on nearby clouds in a still view (issue_20261105_202810, issue_20261105_203119).
- Flying across certain places no longer changes the shape of every cloud in view in one frame: the cumulus lobes'
  noise frame now cross-fades over about a second and a half (issue_20261105_202810 shot 3).
- Clouds no longer lose whole masses for a moment after you fly across certain places: the clouds' empty-space map
  now starts over when the cumulus lobes' noise frame changes under you (it changes at fixed lines on the globe).
- Raining cloud fields seen from above no longer show a thin dark line, like a contour, running just inside the
  edge of every clear gap: the clouds' thin margins over rain are drawn whole instead of half see-through
  (issue_20261105_180442).
- Rain falls only from clouds: rain curtains and the drops at the eye no longer hang under the clear gaps between a
  storm's clouds (issue_20261104_020642).
- Meteors are no longer drawn past 1 hour/s: at 1 day/s and faster the Earth's nightside no longer sparkles with
  one-frame spiked flashes. 1 hour/s and slower are unchanged (issue_20261030_175604).
- Far city lights no longer blink at low angles under temporal AA; the sea no longer shows a fine white static
  under broken cloud; waves no longer leave trails in cinematic playback; climbing over a city at night no longer
  smears the lights.
- Night-side cities seen from orbit are no longer grey, cloud-shaped blobs.
- The aurora is no longer invisible from rural sites near towns, nor cut off along a hard line toward a town.
- The glare on a mirror's glint follows the Sun's image smoothly; render-only satellite parts no longer seed
  bloom or glare; rough metal edges no longer glare.
- Beam-lit snow shafts no longer show a rainbow; snowflakes no longer fall at the rain's speed; a mirror in the
  Earth's shadow no longer beams.
- The weather bookmarks (Venezuela rainbow, Antarctic snow) frame their storms again with the new cloud defaults.
- HQ photos (F8) no longer veil the clouds in a light blue-grey haze at sunset and under storms: the photo now
  lights the air in front of the clouds the way the window does (issue_20261104_020521).
- The sky's per-pixel atmosphere (the band along the horizon, and "Aerial perspective volume" off) no longer lays a
  light blue-grey haze over clouds at sunset: it over-lit the air in front of them toward a low Sun. It now matches a
  512-step reference within ~2 levels at the reported storm (it was up to ~27 too blue).
- The sky glow around satellites sums bright flares, so one tumbling piece of debris no longer lights half the sky.
- The rain no longer keeps pointing at the sky's freed anti-aliasing images after they are rebuilt (switching from
  Planetarium or Potato to a full graphics tier; the harness's `shaders reload sky=`, where it killed the app in a
  storm).
- Ice fog is no longer one uniform white film over the ice sheets and the Southern Ocean: it now comes in regions
  of ~600-1000 km (one stretch hazes, the next is nearly clear), and it no longer whitens the open ocean south of
  the Antarctic coast at high amounts.
- Many fixes from QA of launches and missions: no plume pulsing, no train jitter in time warp, smooth handovers
  in the Info window, no "?" under a selection, the camera never goes underground following a rocket.
- The Attributions tab lists every bundled asset.
- Narrow fjords and sea channels (southern Chile, Norway's Sognefjord, the Zeeland estuaries) are water again
  instead of flat blue sea-level land, sea-level ground the satellite map shows as sea is water, and steep shores
  (fjord walls, cliffs) keep their height instead of being shaved into flat shelves.
- QA fixes: the Sun's lens-flare ghosts no longer wash out a backlit satellite's earthlit side; the selection
  panel's magnitude follows the Info window's rule; the Info window's 3D view shows a burning stage's flame and
  destroyed objects; long durations read in days / hours / minutes and visiting spacecraft by phase ("Docked
  since"); the Ongoing tab lists build-out last; long window titles are ellipsized instead of clipped;
  Planetarium shows the green airglow only at the limb; engine light reaches the hull above it; the Beam-lit Snow
  bookmark frames its snow shower again.
- A head-on collision's slow debris no longer bursts out of one point under the breakup as two mirrored
  fireball clusters: pieces knocked onto falling arcs now fly on and re-enter where their own arcs reach the
  atmosphere, minutes later and hundreds to thousands of km downrange (issue_20261104_015233).

### Technical and modding

- **Satellite types as data.** Rigid attitude groups (`attitude_groups`) replace the attitude enum; geometry
  models (`satellite_models/<id>.json`) with primitives (including profiles, fins and flat polygons), materials,
  kinematic trees, stowed (along an axis or radially) and folding parts, docking ports, render-only parts, open lattices and provenance blocks. See
  `docs/CONSTELLATION_MODDING.md`.
- **New roster features.** `"distribution": "Shells"` with a `groups` table, `List` constellations of named
  satellites, formation clusters (line or ring, `cluster_relative_orbit`), per-cluster scatter and per-plane node
  spread.
- **`data/launches.json`**: sites, vehicles, programs, stacks, crew and cargo blocks, station arms, deployment
  history. **`data/geo_satellites.json`**, **`data/launch_coasts.json`**.
- **Reflector targets** carry a `"kind"` (`solar`, `agriculture`, `daylight`); solar sites become parks.
- Unified log-distance scene depth written by every surface; one magnitude-to-sprite model for satellites, stars
  and planets; line-of-sight Chapman extinction at any altitude; exact GPU orbit phase; the orbit rebake runs on
  the GPU; the roster can be rebuilt at runtime.
- Large textures ship pre-decoded (`.r8`, `tools/make_raw_textures.py`); the app keeps its own pipeline cache.

### Build / packaging

- `SatModelTool` target, the `linux-accuracy-gate` preset and the accuracy gate in CI; `PackageRelease.cmake`
  ships `satellite_models/`, `launches.json`, `geo_satellites.json`, `launch_coasts.json`, `lunar_bases.json`, the
  default bookmarks and the prebaked Sun's Fury (`kessler_prebake/`). Runtime files re-sync next to the exe when they change; user screenshots and user data (settings,
  reports, bookmarks) live outside the build tree in a development checkout.

## v1.1.1 — 2026-09-08

### Added
- **Zodiacal light** — sunlight scattered by interplanetary dust rendered as a faint warm
  cone along the ecliptic, with a dim gegenschein patch opposite the sun. New Settings ›
  Clouds sliders: *Zodiacal gain*, *Zodiacal width*, *Zodiacal outer fade*.
- **Dark-sky exposure model** (`darksky.glsl`) — per-direction, per-sample sky-brightness
  gate shared by the Milky Way, zodiacal light and their ocean reflections, so skyglow and
  twilight thin faint features instead of just dimming them uniformly. New Settings ›
  Photometry sliders: *City sky mag*, *Twilight sky mag*, *Twilight end*, *Twilight aniso*.
- **Milky Way ocean reflection** with its own *Ocean MW refl* gain slider.
- **Minimal constellation preset** (`data/custom/constellations_minimal.json`, ~124k
  satellites) alongside the full default set.
- **Invert look axes** — Settings › Controls toggles for Mouse X/Y and Controller X/Y,
  persisted in `settings.json`.

### Changed
- Default constellation set expanded to ~1.38M satellites (larger Starlink Gen2, Guowang,
  SpaceX AI and debris populations).
- Vertical (raise/lower elevation) speed no longer approaches the terrain exponentially — it
  now keeps a brisk fixed floor speed near the surface, so pushing into the ground and
  climbing back out take a normal amount of time instead of feeling stuck.
- Holding sprint (Move Fast) now cancels fine/slow movement mode instead of being overridden
  by it.
- New Milky Way skybox: ESO's "The Milky Way panorama" (eso0932a, credit ESO/S. Brunier,
  CC BY 4.0), replacing the previous Solar System Scope star texture.
- Fine-movement key (stick-click / Ctrl) is now a latching toggle instead of a held modifier.
- Milky Way sun-glare suppression only applies while the sun is above the horizon, so the
  band no longer dims toward an Earth-occluded sun.
- Controls reference window is now opened from a button in Settings › Controls (replacing
  the "show on startup" toggle); intro captions restyled.
- App now anchors its working directory to the executable location, fixing asset loading
  when launched from Finder or another directory.
- Cloud *Ambient* gain default raised to 1.0.
- *City sky mag* (Photometry) default lowered to 13.0 and its slider now goes down to 1.0,
  below the real-world range, for stronger artistic washout of dark-sky features under city glow.

### Build / packaging
- New `SatLightSimFresh` build target (exe `SatLightSim_FRESH`) — same binary compiled with
  `SAT_FRESH_SETTINGS` so it never reads or writes `settings.json`, for testing the out-of-box
  first-run experience. Ships with its own VS Code build task and debug launch configs.
- Cross-platform CMake presets (`CMakePresets.json`) and a single `package-release` target
  (`cmake/PackageRelease.cmake`) driving CI and `release.bat`.
- macOS release is now a universal (arm64 + x86_64) binary with a fixed deployment target.
- Machine-specific absolute paths scrubbed from committed config; `CMAKE_POLICY_VERSION_MINIMUM`
  set so the project configures under CMake 4.
- Device limits and required features are logged and validated at startup.
- CI Windows job builds with Ninja + MSVC (`windows-ci` preset) instead of a hardcoded
  Visual Studio generator version, so a GitHub runner-image toolchain bump no longer breaks
  the release build. Release workflow also runs on pushes/PRs to `main`, not only on tags.
- Fixed a `float`→`int` narrowing conversion in a settings-slider table that MSVC accepted
  but GCC/Clang reject, which broke the Linux and macOS release builds.

### Fixed
- Satellite/star terrain occlusion at render scale < 100% (low-res sky prepass writes no
  depth) now matches the full-res path, including correct behaviour in orbital views.
