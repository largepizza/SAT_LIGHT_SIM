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
  four hours old disperses into a dust band around the parent's orbit (a chain of clumps covering part of the
  orbit, a whole ring once it has wrapped) instead of staying a round cloud; re-entry fireballs with a
  breakup flash, fragment trains and material colours (aluminium white, copper green, sodium yellow, lithium
  crimson); and once the cascade runs, many small meteor-like streaks.
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

- **Volumetric clouds, rebuilt (clouds v2).** One 3D cloud field from the ground to orbit, driven by a weather
  map: cumulus with lobed, cauliflower sides and flatter bases, stratus decks with textured undersides,
  altocumulus and altostratus, cirrus streaks, veils and cirrocumulus, all lit by the Sun or the Moon through
  the clouds above them, with sunset colours, silver linings and soft cloud shadows on the ground (long streaks
  at sunset), far cloud shadows and god rays, and mid / high layer shadows at any distance.
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
- **Clouds from orbit.** Satellite-imagery morphology (closed and open cells, cloud streets, popcorn cumulus from
  MODIS scenes) shapes clouds seen from space; from high orbit a full-resolution far layer takes over. The clouds
  stay the same clouds as you climb.

### Storms, hurricanes and wildfires

- **Storms are objects with a life.** Air-mass cells, multicells, supercells, squall lines and mesoscale complexes
  form at a thunderstorm climatology, grow, mature and decay over hours, each with its towers, anvil shield
  (lingering as cirrus after the storm), rain core, cloud field and lightning. A pure function of sim time.
- **Tropical cyclones.** About 80 a year across six basins, named from the WMO lists, following climatological
  tracks with landfalls and recurvature: an eyewall, a clear eye, spiral rain bands, outflow cirrus, a whipped-up
  sea, wind that drives the rain and the ambience, and a storm surge that floods low coasts.
- **The deck inside a hurricane keeps its structure.** Flying through a cyclone's central dense overcast at 1-3 km,
  the cloud under it was one smooth, slowly varying slab - a smooth-walled "carved cavity" with no cell structure
  and the rain hanging below the deck's mass. The deck's forced overcast now keeps the weather map's own brightness
  instead of erasing it: at a reported view the area you can see more than 4 km through drops 70% and the deck's
  local contrast rises 46%, with the eyewall's brightness unchanged (issue_20261027_213254).
- **Tornadoes and waterspouts** under supercells and strong storms at sea, hanging from the storm's real base,
  spinning, with a wall cloud, debris cloud and roar.
- **Haboobs.** A storm's gust front over dry land raises a towering wall of dust, drawn as beige billowing cloud.
- **Wildfires.** Fires start by biome, season and dryness and spread with a coherent wind: a ragged flaming front
  with embers, smoke that follows real trajectories into a regional pall, burn scars, and their glow at night.
- **A burn scar looks like burnt ground, not a black oval.** From 26 km a large fire's scar was one smooth ellipse
  with a clean 0.2-km rim - the reporter read it as an oversized smoke particle. Its perimeter now wanders with the
  fire's own unburnt fingers, its fringe is a mosaic, and its interior carries km-scale unburnt patches: at the
  reported view the rim's luminance step falls 50% on average (the share of the rim over 10 levels: 17% -> 2.6%),
  the scar's structure at the 2-km scale doubles, and the smoke itself is untouched (issue_20261028_192133).
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
- The sky glow around satellites sums bright flares, so one tumbling piece of debris no longer lights half the sky.
- Very bright glares (a sun glint, a burn, a re-entry) share a cluster's spikes like every other light: the
  cluster grid weighed them up to 8x lighter than the sprite did, so the brightest flares kept every spike in
  a crowd (issue_20261019_222938).
- Ice fog is no longer one uniform white film over the ice sheets and the Southern Ocean: it now comes in regions
  of ~600-1000 km (one stretch hazes, the next is nearly clear), and it no longer whitens the open ocean south of
  the Antarctic coast at high amounts.
- Flat, hard-edged teal planes no longer lie across mountainsides on coasts finer than the water map's texels
  (the Chilean fjords, the worst case): the map's shore ramp could take a kilometre-wide, 300-900 m deep bite
  out of a slope, and now only smooths the land it was meant to smooth.
- Many fixes from QA of launches and missions: no plume pulsing, no train jitter in time warp, smooth handovers
  in the Info window, no "?" under a selection, the camera never goes underground following a rocket.
- The Attributions tab lists every bundled asset.
- QA fixes: the Sun's lens-flare ghosts no longer wash out a backlit satellite's earthlit side; the selection
  panel's magnitude follows the Info window's rule; the Info window's 3D view shows a burning stage's flame and
  destroyed objects; long durations read in days / hours / minutes and visiting spacecraft by phase ("Docked
  since"); the Ongoing tab lists build-out last; long window titles are ellipsized instead of clipped;
  Planetarium shows the green airglow only at the limb; engine light reaches the hull above it; the Beam-lit Snow
  bookmark frames its snow shower again.

### Technical and modding

- **Satellite types as data.** Rigid attitude groups (`attitude_groups`) replace the attitude enum; geometry
  models (`satellite_models/<id>.json`) with primitives (including profiles and fins), materials, kinematic trees,
  stowed and folding parts, docking ports, render-only parts, open lattices and provenance blocks. See
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
