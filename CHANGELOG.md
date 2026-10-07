# Changelog

## 1.2.0 (release date TBD)

The biggest update yet: a new weather system, a living Earth from the ground to orbit, satellites you can fly up
to and inspect as real 3D models, and a sim clock that runs on real UTC. Accepted photometric error is listed in
`data/benchmarks/KNOWN_RESIDUALS.md`; the full reference is the project wiki (`wiki/`).

### New in the sky

- **Volumetric clouds, rebuilt (clouds v2).** One 3D cloud field from the ground to orbit, driven by a weather
  map: cumulus with lobed, cauliflower sides and flatter bases, stratus decks with textured undersides,
  altocumulus and altostratus, cirrus streaks, veils and cirrocumulus, all lit by the Sun or the Moon through
  the clouds above them, with sunset colours, silver linings and soft cloud shadows on the ground (long streaks
  at sunset).
- **Storms.** Cumulonimbus towers grouped into storm cells (a dominant tower and a flanking line), flared anvil
  heads under the tropopause, overshooting domes, rain shafts under the towers, cumulus that rises around storms.
- **The weather moves and evolves.** The cloud map drifts, is advected by a slow wind, and storms grow and decay
  over hours; over land, afternoon convection builds storms that die back after dark.
- **Rain, snow, sleet and diamond dust.** Precipitation at your eye is drawn as real particles: drops sized and
  falling like real rain, flakes that flutter, blown by the wind, gusts and a storm's outflow, lit by the sky
  around you. The precipitation type follows the air temperature.
- **Lightning and thunder.** Whole storms flash: cloud-to-ground strokes with branches and tendrils, in-cloud
  flashes, spider lightning under the cloud base and rare red sprites high above. Clouds glow around each flash,
  and thunder rolls in from the channel's own shape, delayed by distance (a crack when it's close).
- **Rainbows, halos and ice optics.** Rainbows at storms with the Sun behind you, 22-degree halos, sundogs, the
  circumzenithal arc, sun pillars and glints off ice crystals, for the Sun or the Moon.
- **Fog, dust and ice fog.** Radiation fog pooling in valleys at night and burning off in the morning, sea fog
  on the coast, dust plumes over dry land, diamond-dust ice fog over Antarctica and Greenland.
- **Clouds from orbit.** Real satellite-imagery morphology (closed and open convective cells, cloud streets,
  popcorn cumulus from MODIS scenes) shapes clouds seen from space, and from high orbit the low clouds become a
  full-resolution far layer: sharper, steadier and several times cheaper. The clouds stay the same clouds as
  you climb.
- **Aurora driven by space weather.** The auroral oval is a ring fixed to the Sun, widest near local midnight,
  dark inside. Its size and brightness follow a Kp index from a space-weather model of the solar cycle,
  recurrent solar-wind streams, CME storms and substorms; a big storm brings the aurora to mid-latitudes. The
  curtains are thin folded sheets with sharp lower borders, rays and red tops. An "AURORA NOW" readout shows
  the current activity; Kp can also be set by hand.
- **Real time, real eclipses.** The sim clock is real UTC (the Earth turns by Greenwich sidereal time), and the
  Moon follows the standard lunar series: its true position, size, phase and orientation (tidally locked).
  Solar and lunar eclipses happen when and where they really do (total lunar eclipse 31 Jan 2037, total solar
  eclipse 13 Jul 2037 over Australia). At totality the Moon's shadow darkens the air, ground, clouds and dust
  point by point, leaving an orange ring around the horizon, the stars come out, and a fibrous corona with
  pink prominences appears. Moonlight follows the phase law.

### New on the ground and sea

- **Terrain detail.** A true 3D surface with metre-scale relief and branching erosion gullies, soft terrain
  shadows, snow, rock and close-up ground textures (grass, forest floor, rock, snow, sand, dirt).
- **Lakes and coasts.** Lakes sit at their own level (Titicaca, Baikal, the Great Lakes), shorelines come from
  Natural Earth's 10 m coastlines, beaches have dry and wet sand.
- **Night lighting.** The night side is lit by the Moon and the night sky; only real city lights glow.
- **The sea.** Waves that stay seamless however far you travel, a sea state that follows the weather (calm
  seas to storm swell), whitecaps and foam, breaking surf and shoaling on the shore, a physically based Sun
  glitter path, sharp sky and cloud reflections close up and a rougher, darker sea toward the horizon.
- **Procedural cities.** Street grids, lamp posts in sodium and LED colours, signs and traffic lights, real
  major roads from Natural Earth, rooftops, yards and tree crowns by day, and nine regional styles (North
  American grids, European courtyard blocks, Soviet slab rows, East Asian plans and more). From altitude and
  the horizon, distant cities glitter as fields of crisp points instead of blurred blobs. Cities stop at the
  foot of the mountains.
- **Farmland.** Rectangular fields that follow regional styles: mile sections and centre pivots, strip fields,
  rice paddies that reflect the sky.
- **Solar parks.** Reflector target sites are drawn as solar PV parks sized by their capacity, with single-axis
  trackers or fixed-tilt tables; the glass glints the Sun and, at night, the Reflect Orbital beams. Rooftop
  solar appears where a park overlaps a city. Mirrors only beam while they are in sunlight.

### Satellites

- **Satellites as real 3D models.** Every satellite type has a geometry model (ISS, Tiangong, Hubble, Starlink
  v1.0 / VisorSat / v1.5 / v2 Mini / Direct-to-Cell / v3, OneWeb, Amazon LEO, Guowang, Starmind AI1, Reflect
  Orbital mirrors, the Starship depot, commercial stations, debris). Any satellite big enough on screen is drawn
  as a mesh in the scene, reflecting the full sky and Earth, with a seamless hand-off from its point of light.
- **Physical brightness.** Magnitudes come from the models: microfacet reflection per surface, earthshine from
  the whole lit Earth, backlit translucent arrays, optional shadowing between parts, and validated against
  published observation campaigns.
- **Info window and 3D view.** Select a satellite for its orbit, brightness, sky position and a live 3D view
  (free, from you or toward you; live or studio light), which pops out into its own window. A photometric
  check compares the render with the model.
- **Trace pass.** Plot a satellite's magnitude over its current or next pass, with peak, length and live values,
  and export it as CSV.
- **Go to, Follow and Track.** Fly to a satellite along your line of sight to it and ride with it; Track locks
  the camera onto a satellite as it crosses your sky while you stay on the ground.
- **SpaceX Starmind orbital data centers.** The roster follows SpaceX's May 2026 FCC filing: sun-synchronous
  shells whose planes cross at the equator (the "X-ring") and 30-degree shells, about 990,000 satellites flying
  in formation rings of eight.
- **Bulk export** of simulated observations for a satellite or a whole constellation (Settings > Photometry).
- **Satellite glints on the sea** have their own gain and floor (Ocean tab).

### Camera, capture and UI

- **Bookmarks.** Save a view (place, camera, time and the weather) with a thumbnail and jump back to it.
- **Nine default bookmarks** ship with the sim (a rainbow at dawn, the 2037 total solar and lunar eclipses, an
  aurora storm from orbit, Reflect Orbital over Anchorage, AI satellites at twilight, Los Angeles by night, a
  Starmind formation ring, sunset from orbit); "Restore defaults" brings back any you deleted.
- **Cinematics.** A camera-path editor: shots of keyframes on smooth splines, follow shots that ride with a
  satellite, a timeline, saved files, and frame-by-frame export (quick preview or supersampled HQ frames with
  motion blur), with a progress display.
- **HQ photo** (F8 or the camera-sparkle button): a settled, supersampled screenshot at up to 4x the window.
- **First-run tutorial** for looking, moving, climbing, boosting and selecting (keyboard, mouse or gamepad),
  then the HUD's buttons. Replay it from Settings > Display.
- **Loading screen** with progress instead of a white window; much faster startup on repeat launches.
- **Type instead of drag.** Slider values, the clock, latitude, longitude and altitude are text fields.
- **A cleaner UI.** Every window and settings tab shares one style; settings tabs are grouped (General, Sky,
  Rendering behind "Show advanced settings", About); a Performance tab holds the frame timings and knockouts;
  new filled pixel icons; the bottom-right panel ends in the Bookmarks, Cinematics and Settings buttons.
- **Gamepad layout 2.** D-pad left/right move between a selected satellite's buttons, A presses, B steps back;
  time on D-pad up/down, pause on X, reverse on Y. Saved pad bindings are reset to this layout once.
- **Movement scales with height**, so walking near the ground is walking pace and orbit is still fast.

### Sound

- **Ambient sound.** Location-aware ambience under the music: winds, surf and gulls, crickets and birds,
  jungle day and night, city traffic, rain, thunder, and drones and pads in orbit, all in the music's key.
- **Music.** A player with previous / pause / next and a gap between tracks; new tracks LEO Motif and BIOS.
  Each track has an "upwell" stem that rises over it when Reflect Orbital flares fill the screen. The music
  fades out toward geostationary altitude.

### Graphics and performance

- **Temporal anti-aliasing** of the terrain, sky and sea, which becomes a **temporal upscaler** below 100%
  render scale, keeping most of full-resolution detail.
- **Render scale now scales the clouds too**, so lower scales give real frame-rate gains ("Clouds follow
  render scale"), and an optional **automatic render scale** holds a target frame rate.
- **Presets reshuffled.** Potato and Planetarium for weak hardware (Planetarium has no volumetric clouds),
  Low (50% render scale, every effect on), Medium (67%), High and Ultra. Integrated GPUs start on Planetarium, with a
  tutorial card pointing at the presets.
- **Startup graphics chooser.** Before anything heavy loads, the loading screen asks for Full graphics,
  Planetarium (no clouds) or Potato (very old hardware), pre-selecting the last choice, the device's recommendation
  on a first run, or one tier lighter after a crash. If the first frames are very slow it steps down one tier once.
  "Ask for graphics mode on startup" (Display tab) turns the question off.
- **Planetarium and Potato are the light v1.1 skies again**: the 1.2 ground features (terrain detail, the
  procedural city, sea reflections, satellite meshes) are compiled out of them and nothing of the volumetric
  clouds runs, so they start fast and run on the hardware they were made for. After a crash the preset steps
  down one tier instead of always to Planetarium.
- Large performance work across the cloud march (adaptive sampling, tile culling, LODs), the sky pass, terrain
  depth pyramid and the satellite pipeline (about 1.37 million satellites in the default roster).

### Changed defaults and behaviour

- Defaults are the author's tuned settings (volumes, rain and lightning, brightness, automatic render scale on,
  HQ photos at 1x the window).
- Saved times now show a different local time of day (the clock is real UTC; about 5 h 15 min earlier for the
  same timestamp). The start time moved so the intro is unchanged.
- "Storm strength" for the aurora is replaced by Kp (automatic by default).
- Glare defaults are the distance-tuned values; glare grows when the camera is close to a satellite.
- The cinematics, bookmarks and settings buttons moved from the time bar to the bottom-right panel; the
  Camera settings tab is now the Controls tab's Mouse section.

### Fixed

- Far city lights no longer blink at low angles under temporal AA; the sea no longer shows a fine white static
  under broken cloud; waves no longer leave trails in cinematic playback.
- Render-only satellite parts (fittings, trusses) no longer seed bloom or glare.
- A mirror in the Earth's shadow no longer beams; stale bloom, lightning and depth no longer show after
  switching satellites, clouds or the depth pass off; the sky's exposure follows a total eclipse.
- The Attributions tab lists every bundled asset (terrain textures, Natural Earth data, ambience recordings).

### Technical and modding

- **Satellite types as data.** Rigid attitude groups (`attitude_groups`) replace the attitude enum (legacy
  modes are converted at load); geometry models (`satellite_models/<id>.json`) with primitives, materials,
  kinematic trees with hinges and pivots, render-only parts, open lattices, provenance blocks. See
  `docs/CONSTELLATION_MODDING.md`.
- **New roster features.** `"distribution": "Shells"` with a `groups` table (Walker or sun-synchronous groups),
  formation clusters (`cluster_size`, `cluster_shape` line or ring), and per-plane node spread
  (`raan_spread_deg`, `raan_spread`).
- **Reflector targets** carry a `"kind"` (`solar`, `agriculture`, `daylight`); solar sites become parks.
- **SatModelTool** (`tools/sat_model_tool/`): bake, validate, OBJ export, self-tests, benchmark runs
  (SatBench), trace replay, material overrides. Nine published photometry datasets in `data/benchmarks/`,
  and a photometric accuracy gate in CI (`cmake --build <dir> --target accuracy-gate`).
- **Automation harness** (`tools/harness/`, `docs/HARNESS.md`): scripted and live runs, captures with state
  sidecars, perf and knockout sweeps, UI layout dumps, camera paths, temporal-stability and climb benchmarks,
  offline audio rendering. Launches are spaced and capped machine-wide.
- **Project wiki** (`wiki/`, MkDocs) as the maintained reference; `docs/rendering/` describes the rendering
  architecture.
- Unified log-distance scene depth written by every surface; one magnitude-to-sprite model for satellites,
  stars and planets; line-of-sight Chapman extinction at any altitude; exact GPU orbit phase.
- Large textures ship pre-decoded (`.r8`, `tools/make_raw_textures.py`); the app keeps its own pipeline cache.
- Tools: `make_icons.py`, `make_ambience.py`, `make_water_map.py`, `make_city_roads.py`,
  `make_terrain_materials.py`, `make_cloud_morph.py`, `SoundTool`, `cloud_stats.py`.

### Build / packaging

- `SatModelTool` target, the `linux-accuracy-gate` preset and the accuracy gate in CI; `PackageRelease.cmake`
  ships `satellite_models/`. Runtime files re-sync next to the exe when they change, and user screenshots live
  outside the build tree.

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
