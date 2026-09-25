# Changelog

## Unreleased

> DRAFT, summarised from `git log v1.1.1..HEAD` (43 commits, 2026-09-10 .. 2026-09-24). Trim, split or
> re-word before tagging — in particular decide whether the lighting overhaul makes this v1.2.0.
> Per-subsystem detail lives in `CLAUDE.md`; accepted photometric error lives in
> `data/benchmarks/KNOWN_RESIDUALS.md`.

### Added
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

### Changed
- Unified scene depth: one encoding (log2 of the true ray distance, 1 cm to 1e9 m) written by
  terrain, ocean, opaque cloud, meshes, points and stars, replacing the 150 km cap and its manual
  occlusion tests. Satellites in front of the distant Earth (or a mountain, or a cloud) now occult
  correctly, and bloom survives at every range.
- One magnitude-to-sprite model shared by satellites, stars and planets.
- Line-of-sight atmospheric extinction now computed for every observer altitude, not just the
  surface.
- The satellite roster is model-first: `data/constellations.json` ships the modelled types, and lobe
  budgets scale with roster size.
- **UI layout pass.** The selection panel now shows name, type and magnitude plus a row of
  **icon-only** buttons whose tooltip is just the button's name — **Info** (a serif "i", opens the
  info window), **Go to** (follow mode) and **Trace pass** (a new chart icon); the GPU-parity readout
  is gone (the check and its mismatch log stay). Its orbit rows and the range/phase line moved into the
  info window, whose settings column is narrower (170 px) and which now opens 640x460 pinned to the
  top-right corner. The trace window opens 520x380 in the bottom-left above the time controls, so the
  two coexist instead of covering the middle of the sky.

### Fixed
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
