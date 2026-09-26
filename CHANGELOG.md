# Changelog

## Unreleased

> DRAFT, summarised from `git log v1.1.1..HEAD` (43 commits, 2026-09-10 .. 2026-09-24). Trim, split or
> re-word before tagging — in particular decide whether the lighting overhaul makes this v1.2.0.
> Per-subsystem detail lives in `CLAUDE.md`; accepted photometric error lives in
> `data/benchmarks/KNOWN_RESIDUALS.md`.

### Added
- **Ambient sound.** A location- and context-aware ambience bus under the music (Settings → Sound →
  "Ambience"): wind for plains, deserts, mountains and ice sheets; surf and gulls at the coast, the
  open sea offshore; crickets at night (silent inside a Reflect Orbital beam), forest birds and a
  dawn chorus, jungle day and night; city traffic day and night; the jet stream, the thin
  stratosphere; a cabin hum in orbit, a warm hum over the aurora at night, a low phased drone
  among the broadband shells, a fridge-like hum inside the AI datacenter disk, real VLF whistlers
  in medium Earth orbit and the "Firmament" pad rising toward high orbit (both original, made in FL
  Studio). Procedural voices (`src/AmbientSynth.cpp`) plus CC0 field recordings
  (`tools/make_ambience.py`), mixed by a moddable layer table
  (`assets/sound/ambience/ambience.json`). The automation harness renders and checks it
  (`audio state/record/expect/force/music`, `imgtools.py audio`). In orbit, low phased drones and soft status tones in one shared key (Settings → Sound →
  "Tonal root"). Advanced Sound settings: fade speeds, the tonal root, per-group gains (wind, water,
  nature, city, space, machines).
- **Music player.** Settings → Sound shows the current track and its position, with previous /
  pause / next. A 30 s gap between tracks (adjustable) lets the ambience breathe. Gravity Wave always
  plays first; any mp3/flac/wav added to `assets/sound/music/` joins the playlist. New track: LEO
  Motif, a short piece written to sit with the ambience. The music fades out as you climb: half
  volume in medium Earth orbit, silent from geostationary altitude up, leaving high orbit to the
  ambience.
- **Satellite lighting overhaul (Phases 1-3).** Data-driven rigid attitude groups (`attitude_groups`
  in `constellations.json`) replace the `AttitudeMode` enum — legacy modes are converted at load and
  verified against the old surface normals. Geometry models (`"model": "<id>"` →
  `satellite_models/<id>.json`) describe a type as primitives, materials and a kinematic tree with
  hinges and per-component pivots. Photometry is now physical: GGX/Beckmann · Schlick · Smith lobes,
  earthshine from the whole lit Earth cap, diffuse transmission for backlit arrays, and occlusion
  between a satellite's own parts on both CPU and GPU.
- **SatModelTool** (`tools/sat_model_tool/`, `cmake --build build --target SatModelTool`) — load,
  bake, validate and OBJ-export models; `--selftest`, `--benchmark`, `--run-benchmark`,
  `--sensitivity`, `--replay-trace`, `--set` material overrides, bulk export.
- **Benchmarking.** 9 published photometry datasets with provenance in `data/benchmarks/`, the
  `SatBench` campaign runner, `sat-light-sim-trace/1` CSV export/replay, a GPU parity check for the
  selected satellite (mismatches are logged), and the photometric accuracy gate (`cmake/AccuracyGate.cmake`, M11) which runs
  those selftests and benchmarks in CI.
- **In-app satellite renderer + model viewer (Phases 4a-4f).** Any satellite that is big enough on
  screen is drawn as a 3D mesh, composited as a surface of the scene (clouds in front occlude it,
  the atmosphere scatters over it, points and stars behind it are hidden); energy-matched bloom and
  mesh-glint glare across the sprite → mesh hand-off; environment probes give meshes reflections of
  the Earth and an ambient term; procedural surface detail (solar cells, MLI, panel seams);
  model-viewer window with Studio/Live lighting, sunlit/rest pose, a photometric check and camera
  presets; follow mode; telescope zoom whose aperture grows with magnification.
- **Real-satellite geometry roster** — ISS, Tiangong, Starlab, Axiom, Haven-1/2, Hubble, Ross,
  Orbital Reef, the Starship HLS depot, Starlink v1.0 / v1.5 / v2 Mini / v3 / VisorSat, OneWeb,
  Guowang, Amazon LEO, the SpaceX AI satellite, Reflect Orbital mirrors, Starmind AI1 and debris
  fragments — plus render-only parts (free greebles) and open lattice trusses.
- **UI**: trace window with axis ticks, live magnitude and reasons; model viewer; UI scaling.
- **Sharp mirror reflections** (Photometry, on by default): mirror-smooth surfaces of the four largest
  such satellites in view reflect the full sky renderer per pixel instead of a reflection map.
- **Stars in reflections and in the model viewer's sky**, drawn from the catalogue by the same point
  model as the main view.

### Changed
- **Glare scales with proximity (2026-09-26).** A satellite's glare sprite is up to 3× wider where the
  camera is right on top of it and untouched at 400 km and beyond, so a resolved model — in the 3D
  viewer, or drawn as a mesh in the main view — spreads a far wider flare than the same satellite 400
  km off, while a point sprite's glare from the ground is exactly the shape it was tuned to. The range
  is one the frame already had, so the cost is a couple of ALU ops. Settings → Photometry gained
  "Glare near gain" and "Glare near range (km)" (`photometry.glare_near_gain` = 3.0,
  `photometry.glare_near_range_km` = 400; gain 1 = off). The glare's own defaults are now the
  distance-tuned values a release build shipped in its `settings.json` (gain 0.684, size 29.79 px,
  threshold 1.765, falloff 4.678) instead of the earlier untuned 1.0 / 48 / 0.3 / 2.5.
- Unified scene depth: one encoding (log2 of the true ray distance, 1 cm to 1e9 m) written by
  terrain, ocean, opaque cloud, meshes, points and stars, replacing the 150 km cap and its manual
  occlusion tests. Satellites in front of the distant Earth (or a mountain, or a cloud) now occult
  correctly, and bloom survives at every range.
- One magnitude-to-sprite model shared by satellites, stars and planets.
- Line-of-sight atmospheric extinction now computed for every observer altitude, not just the
  surface.
- The satellite roster is model-first: `data/constellations.json` ships the modelled types, and lobe
  budgets scale with roster size.
- **UI layout pass, round 2 (2026-09-24).** The satellite UI is now ONE window with an on-demand
  pop-out: the info window carries a **fixed 4:3 render** of the subject with the **view-preset chips**
  floating over it (Select, Go to, Spin, Observer, Studio, Maximize — icon-only, tooltip = the button's
  name, accent fill for the active preset), and a **scrollable list of collapsible sections** below
  (Satellite / Orbit / Photometry open by default; Observer / Camera / Render / Check collapsed) — the
  Clouds settings tab's form. **Maximize** pops the 3D view out into its own 900x640 resizable window;
  one offscreen render serves both views, each sampling a centred sub-rect of its own aspect, so the 4:3
  band and the wide pop-out both stay unstretched. **Select / Go to** are title-bar icons again (they act
  on the subject, not the view — the chips are Spin / Observer / Studio / Maximize, and Spin is a plain
  toggle: free camera = nothing lit). The observer readouts, the "you" marker and the
  Observer preset now use the parked ground telescope rather than the camera (in follow mode the camera
  IS the observer, which is what made the marker line flip and flicker in space).
- **UI icons are generated, not hand-drawn** — `python tools/make_icons.py` rebuilds every new icon in
  `assets/icons/ui/` from geometry declared in that file and prints 48 px + 16 px previews (the size
  they are actually drawn at), replacing the ad-hoc shell rasterising those four glyphs started as.
- **Runtime files now re-sync next to the exe whenever they change** (`sat_sync_runtime_sources` in
  `CMakeLists.txt`): the POST_BUILD copies only ran when the target relinked, so a regenerated icon (or
  an edited `constellations.json`) could sit stale beside the exe forever with nothing reporting it.
  The satellite window's section list also draws a scroll thumb (`ui.scrollbar`), so it reads as
  scrollable instead of just ending at the window edge.

### Fixed
- **Meshes and flares no longer fight in dense constellations (2026-09-25).** Only 64 satellites could be
  meshes, picked in random GPU order, so in the AI ring neighbours flickered between model and flare
  every frame. Up to 256 are now drawn, largest first, and the size at which a satellite turns into a
  model rises while more than that are in range.
- A distant satellite's glare no longer jumps between texels of its small model: it stays the flare's,
  at its centre, until the model is resolved.
- Environment probes follow their satellite: at high time rates they no longer drop to the fallback
  lighting every frame (the ambient light of every model flickered). The model viewer no longer
  re-renders its whole probe every frame at those rates either.
- Model viewer: exposure follows the sky's rule at the satellite (it was ~5x too dark over twilight);
  marker lines are drawn over the model with their own depth and clipped to the view (they flickered
  when orbiting close); the dots are always on top and labelled "You" / "Target".
- Reflections dim the Milky Way and zodiacal light near the Sun and under sun glare as the direct
  view does, at the viewer's exposure.
- Scene meshes could render as a red bloom-only ghost; the model viewer's first cut came up all-black
  (Clay draws an element's own background over its custom content); a satellite that went dark while
  selected could drop out of view; and a click on the trace window fell through to the camera.
- Bloom: seeding it from a mesh's own over-white pixels exploded when looking edge-on to the Sun —
  it now seeds from photometric light only. Per-texel overflow in the flare source also made a
  mirror's glint position NaN, so the Sun in a Reflect Orbital mirror never glared.

### Build / packaging
- `SatModelTool` target, the `linux-accuracy-gate` preset and the M11 CI gate; `PackageRelease.cmake`
  now also ships `satellite_models/`.

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
