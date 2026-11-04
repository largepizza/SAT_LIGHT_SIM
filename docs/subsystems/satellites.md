# Satellite types and geometry models

Attitude groups, SatModel geometry / materials / lobes, earthshine, occlusion, the mesh renderer + env probes, benchmarks, SatModelTool, station models.

Part of the subsystem notes (`docs/subsystems/`, map in `AGENTS.md` §4). Moved verbatim from
`CLAUDE.md` on 2026-11-03. Do not read this file whole: `python tools/outline.py docs/subsystems/satellites.md`
lists its sections and topics with line numbers. New notes go next to the section they belong to.

## Subsystem: Satellite Types

Each `SatelliteType` composes two surfaces + a diffuse floor:
- `primary` (`SurfaceSpec`) — always active
- `secondary` (`SurfaceSpec`) — optional; `weight=0` disables
- `diffuse` — constant Lambertian floor (always visible)
- `mirrorFrac` — fraction of primary that is near-perfect mirror; adds ultra-narrow spike on top of Phong lobe (MIRROR_BOOST=300×)

`SurfaceSpec`: `{AttitudeMode, specExp, weight, group, normal}` — oriented EITHER by a legacy
`AttitudeMode` (`group < 0`) OR mounted on a rigid attitude group (`group` + body-frame `normal`).

### Rigid attitude groups (lighting overhaul Phase 2, 2026-09-23)

Orientation is **data**, not an enum switch. A type has 1-2 `AttitudeGroup`s
(`kMaxAttitudeGroups`); each is a **two-vector law** — body `primaryAxis` points exactly at
`primaryTarget`, body `secondaryAxis` as close as possible at `secondaryTarget` (TRIAD, the STK/GMAT
align/constrain scheme) — plus an optional **1-DOF joint** about a body axis: `Track` (gimbal),
`EdgeOn` (knife-edge roll), `FixedAngle`, `FlareMitigationTilt` (the global slider). Targets
(`AttTarget`): nadir, zenith, sun, anti-sun, velocity, anti-velocity, orbit normal and its anti,
`SunReflectNadir`, `SunReflectGroundSite` (the TargetedReflector lock-window machinery, unchanged —
it now just produces `siteIdeal` for whichever group targets it). The `Tumble` law uses the
per-satellite tumble axis/rate/phase. All closed-form in sim time, so reversibility is intact.

- **Legacy modes are converted, not special-cased.** `resolveAttitude()` (`SatelliteSim.cpp`, top of
  file) runs for every type in `initConstellation()` and maps each legacy mode through
  `legacyAttitudeGroup()`'s table; the GPU only ever sees groups. The mapping reproduces every old
  `computeNormal()` result — verified numerically over 20k random geometries per mode: exact for
  all but the two joint modes (~1e-15). `Perpendicular` has no fixed-body equivalent in general
  (it depended on the primary's normal), becomes +Y of the primary's group, and is exact where it
  matters: every shipped/custom roster uses it only with weight 0, where it contributes nothing.
- **GPU form** (`GpuAttGroup`, `sat_orbit.comp` `groupNormal()`): every body vector is stored in the
  group's TRIAD coordinates (computed once per type on the CPU, in double —
  `attTriadCoords()`), so the shader builds only the world triad from the two target directions.
  Groups are read straight from the SSBO by index — never through a local copy of the array,
  which a dynamic index would spill.
- **Authoring**: `constellations.json` types may carry `"attitude_groups"` and surfaces
  `{"group": name|index, "normal": [x,y,z]}` (schema updated). Every launch writes
  `satellite_types_resolved.json` to the user data folder — each loaded type in explicit form,
  pasteable back and rendering identically. `data/custom/constellations_attitude_example.json`
  has a legacy Gen2, its explicit twin, and a gimbaled-wing V2 Mini the old enum could not express.
- Only the two-surface photometric model reads these normals until Phase 3 (primitives/materials).

### Geometry models (lighting overhaul Phase 3a, 2026-09-23 — `.plans/SAT_LIGHTING_PHASE3.md`)

A type may be `{"name", "model": "<id>"}` in `constellations.json`, loading
`<exe>/satellite_models/<id>.json` (source: `data/satellite_models/`, copied by the build and
packaged by `PackageRelease.cmake`). All model code is `src/simulations/SatModel.h/.cpp`, which
also now owns the attitude types (`AttTarget`/`AttLaw`/`JointMode`/`AttitudeGroup`/`GpuAttGroup`).

- **Kinematic tree.** Groups are a tree: a ROOT has the two-vector law; a CHILD has
  `"parent"` + `"hinge": {position, axis}` (in the parent's body frame) and is always the parent's
  frame rotated about the hinge by its joint. Parents precede children; `kMaxAttitudeGroups` = 4.
  **Per-component pivots (Phase 4f):** a component of a CHILD group may set `"pivot"` (relative
  to the group hinge, like `position`); the joint then turns it about the parallel axis through
  hinge + pivot. That is one joint at one angle on several axes, like the ISS's four beta gimbals
  in a single group. Normals, and so every lobe and magnitude, are unchanged. A posed *position*
  gains `satPivotOffset()` = (R_parent − R_group)·pivot, and every position consumer adds it:
  occlusion samples/occluders on the CPU and in `sat_orbit.comp` (`pivotOffset`; the pivot rides in
  `GpuSatOccluder`'s pad slots), the reference ray-casts, OBJ export, and the mesh renderer
  (`MeshComponents` binding 7, `instPivotOffset`). Any child group, including one with children of its
  own: the offset involves only the component's group and its parent (the ISS's Zvezda arrays pivot
  on the SARJ group, which carries the beta group).
  All vectors in a tree use the ROOT's triad coordinates; on the GPU `typeFrames()` builds up to 4
  world frames parents-first into fixed registers (`pickFrame` selects — no dynamically indexed
  local array). `evalGroupPoses()` is the CPU mirror (hand-kept in step with `groupFrame()`).
- **Geometry → lobes.** Components (`plane`/`box`/`cylinder`/`cone`/`sphere`, `position` relative
  to the group's hinge, `rotation_quat` or `rotation_deg` intrinsic XYZ) with materials (presets in
  `satMaterialPresets()` — estimates until 3c — or model-local, optionally extending a preset) are
  tessellated once, then merged into facet lobes: identical (group, material, normal) faces merge
  exactly; beyond `kSatLobeBudget` (48) same-group lobes merge agglomeratively with the lost normal
  spread folded into α². Curved facets carry an intrinsic spread ((Δφ)²/12, Ω/2π) so a faceted
  cylinder glints as a band, not N mirrors.
- **Profile and fin primitives (2026-10-19, rocket models).** `"type": "profile"` is a body of revolution about
  local Z from a meridian polyline `"profile": [[z, r], ...]` (ogive noses, domes, fairings, bells; listed bottom to
  top along the OUTSIDE so the normals point out); recentred at load so it spans ±height/2 about `position`
  (`radius` / `radiusTop` = the end radii, which engineGeomOf reads as an engine's exit). Options: `"caps"` (flat end
  discs, default off), `"arc_material"` + `"arc_deg": [start, end]` (a second material over an azimuth range from
  local +X toward +Y — Starship's heat shield; the boundaries are inserted into the tessellation), `"inner_material"`
  (the inside drawn too, render mesh only: an open bell). Photometry: 32 azimuths × the polyline's bands; the render
  mesh ~0.3-m facets (12 to 2× the base count), smooth normals except at creases > 35°. Its occluder is the EXACT
  cone frustum through its end radii (occluder kind 5 = `kOccluderFrustum`, in SatModel.cpp, sat_orbit.comp,
  sat_mesh.frag and SatModelTool's GPU-form mirror) — inscribed in an ogive, so a flap on a nose is not shadowed by a
  bounding cylinder. Keep neighbouring meridian segments > 0.5° apart in slope (or equal): the exact lobe merge
  joins normals within 0.5° and the selftest's exact-bake gate (1e-3 mag) fails. `"type": "fin"`: a tapered,
  swept, thick fin (`root_chord`, `tip_chord`, `span` along local +X, `sweep` = the tip leading edge's offset aft of
  the root's, `thickness` / `tip_thickness` along local Y, `bevel` = the share of the chord over which each edge
  thins to a knife edge); root chord on the local Z axis centred at the origin, leading edge toward +Z;
  `back_material` paints the faces toward local -Y (a flap's tiled windward side). Occluder: its bounding box.
- **Reflectance** (`modelFlux()` in `sat_orbit.comp`): per lobe Lambert + GGX/Schlick/Smith (or
  Beckmann, per material: `"distribution": "beckmann"`, `GpuSatLobe::distribution`); the
  Sun's disc is a SOURCE TERM folded into every lobe (2026-10-21, `sunDiscA2` / GLSL `sunA2`, constants in
  SatModel.h): a fixed disc maps onto half vectors as an ellipse 1/(s·h) wider across the plane of incidence, so the
  lobe takes an anisotropic α² pair in the frame (p, q = s × o, h), α_p² = α_m² + S·w, α_q² = α_m² + (S/c²)·w
  (S = `kSunAlpha`² = 0.00208², the limb-darkened disc centre; w blends to the disc's variance for broad lobes).
  Against a brute-force limb-darkened-disc convolution: peak within 0.006 (GGX) / 0.036 (Beckmann) mag to 75 deg of
  incidence, a round reflected beam. Until then one isotropic `SUN_ALPHA` = 0.0023: 1/cos θ too bright at oblique
  incidence (-0.95 mag at 70 deg) and cos θ too narrow across the plane of incidence (the Iridium flare validation).
  Callers: `satLobeIntensitySun()` (a lobe's own α²) for sunlight, `satLobeIntensity()` (α² with an isotropic source
  folded in) for the Earth; never add kSunAlpha² by hand. No `mirrorBoost`/`crossSection`/`specExp` hacks.
  **A material's haze** (2026-10-21, JSON `"haze": {"fraction", "roughness"}`, `SatMaterial::hazeFraction`): a share
  of the specular reflectance in a second, broader lobe — a silvered film's core and wings (Iridium). bakeSatLobes
  emits a hazy facet group as TWO lobes with the same normal / area / samples: the core (specular area × (1 − f),
  all the diffuse) and the haze (specular area × f, nothing else) — splitting the AREA keeps Schlick exact; a hazy
  lobe counts twice against the budget. The brute-force references use `satMaterialIntensitySun()` /
  `satMaterialEarthIntensity()`; the mesh renderer packs the haze into `GpuSatMeshMaterial`'s beckmann / pattern
  high halves (`specCosMat`).
  **A material's Minnaert diffuse** (2026-10-22, JSON `"minnaert": k` in [0.2, 1], `SatMaterial::minnaertK`, 1 = Lambert):
  the diffuse intensity ρ(k+1)/(2π)·A·(cos i·cos e)^k (`minnaertDiffuse`, SatModel.cpp; `lobeDiffuse` in sat_orbit.comp;
  `minnaertFac` in sat_mesh.frag on the Sun, the Moon, the local lights and the earthshine / probe irradiance). Under the
  broad earthshine source the face's irradiance En is one source at cos i = En / E. k travels as a 7-bit code (1 − k)·127
  (`satMinnaertCode`, snapped at load so the lobes and the brute-force references agree): GpuSatLobe::distribution bits
  1-7 (bit 0 alone is Beckmann now: `satLobeBeckmann` and the shader test `& 1u`), GpuSatMeshMaterial::beckmann bits 1-7.
  Every material without the key is bit-identical to before. The 2020 Starlink models use k 0.65 on the nadir face.
  **Earthshine** is the vector irradiance of the visible cap of a Lambertian Earth (albedo 0.3)
  whose every ground point carries its OWN Sun cosine (`earthshineExact()`, `SatPhotometry.cpp`:
  the ring integral is closed-form, one numerical integral over the cap radius), applied as a source
  of that irradiance tilted from nadir toward the Sun (the lit crescent is off to that side), with
  Earth's angular size folded into α. It is tabulated once — `earthshineLut()`, 64 cap half-angles ×
  128 Sun-zenith cosines of (ln E, tilt) — and both `sat_orbit.comp` (`earthshineAt()`) and the CPU
  evaluator (`earthshineLookup()`) read that same table, so parity holds (table vs exact: p95 0.004
  mag). **Until 2026-09-23 it was `0.3·(R/d)²·litFraction`**, which treats every lit point as if the
  Sun were overhead: 17× too bright with the Sun on the sub-satellite horizon and 60-15,000× at the
  95-110° zenith angles every twilight observation is made at. That bug was carrying the benchmark:
  nadir faces (a black visor underside, white antennas) were lit mostly by it. Legacy types keep
  their own `(1−R/d)/2 · illumFrac` term, deliberately, so they don't change. Output is a real apparent
  magnitude mapped into effectFlare through the existing anchor: `flare = K_FLUX·I/r²·brightnessScale`,
  `K_FLUX` = 9.979e10 (0.008 ↔ mag 6).
  **Earthshine is a broad source (2026-09-24, `SatEarthLight` in `SatModel.h`).** The single tilted
  direction above is exact only for a face that sees the whole cap; a face edge-on to it got nothing,
  though the cap really gives it ~30% of a nadir plate's irradiance at 550 km (the black, Earth-lit
  sides the user reported). Diffuse and transmitted light now use the irradiance of the whole cap on
  the face's plane: an order-4 SH fit of the cap's irradiance function (11 terms — symmetric about
  the Sun-nadir plane, and the cosine kernel has no l = 3), from the same closed-form ring integrals
  (`earthshineExact(..., sh)`), floored at the exact vector value: `E(n) = max(SH(n), E·(n·dir)₊)`.
  Specular keeps the tilted source. Tabulated with the vector table as ratios to E
  (`earthshineShLut()`, `earthShA/B/C` in `sat_orbit.comp`, `GpuEarthshineLut`); CPU
  `satLobeEarthIntensity()` / `evalSatLobesEarthPosed()`, GPU `lobeEarthIntensity()`; the mesh
  renderer reads the same fit per instance (`GpuMeshInstance::earthX/Z/ShA-C`, `instEarthDiffuse()`).
  Order 2 was tried first: 6-8% of E worst case (a twilight crescent on the limb is a sharp source);
  order 4 is ≤ 2.9% (`--selftest` gate 5%). `SatPhotResult::intensityEarth` is now ABSOLUTE (already
  × the irradiance). Benchmarks moved ≤ 0.05 mag.
- **Validation + shape check.** At load every model is compared against a brute-force per-triangle
  evaluation (logged p95/max |Δmag| over configurations within 5 mag of its median brightness), and
  its rest/sunlit poses are written to `<user data>/satellite_models_debug/<id>_{rest,sunlit}.obj`
  (+ `.mtl`, OBJ Y-up, zenith up). **`SatModelTool`** (`tools/sat_model_tool/`, EXCLUDE_FROM_ALL:
  `cmake --build build --target SatModelTool`) runs the same pipeline from the command line and
  prints the lobe table — the authoring loop, no app launch needed.
- **CPU photometric evaluator** (`SatPhotometry.h/.cpp`, benchmarking milestone M1 — plan and
  results log live on the "Satellite Brightness Benchmarking" design page). `evalSatPhotometry()`
  is the double-precision mirror of `processSatellite()` → `modelFlux()` for one satellite at one
  instant: orbit (`satOrbitStateAt`, same closed form as `satEciAt`), attitude (`evalGroupPoses`),
  posed lobes (`evalSatLobesPosed`), shadow cones, earthshine → physical magnitude, 1000-km
  magnitude, phase and off-specular angles, dominant lobe. It is what SatBench, the photometry
  readout and exports measure with — **keep it in step with `sat_orbit.comp`** like
  `evalGroupPoses`. `sunDirEciAt()`/`observerEciAt()` are the ONE copy of those formulas —
  `updatePositions()` calls them — and `SatelliteSim.cpp` static_asserts its orbital constants
  against `satphot::`. Ground-site (TargetedReflector) types take their `siteIdeal` from
  **`satGroundSiteIdeal()`** (SatPhotometry, 2026-09-24): the double-precision mirror of
  `sat_orbit.comp`'s ground-site block - lock window with the per-satellite hash offset, `pairScore`
  in the shader's integer hash and float, night-side + elevation eligibility, the nearest-site
  fallback, the rate-limited ease. It needs the targets and settings (`SatGroundSiteAim`, built by
  `SatelliteSim::groundSiteAim()` from the same floats as `reflectorTargetsECEFBuf`) and the
  satellite's roster index, which seeds its site preference. Traces carry all of it in their header
  (`ground_*` keys), so a replay needs no target file; the tool's self-tests use 60 fixed synthetic
  sites and check the settled beam lands on its site (< 0.01 deg). Every CPU consumer uses it: parity
  readout, traces, bulk export, the scene meshes and the tracked viewer - until then a mirror mesh
  was posed facing straight down. Legacy two-surface types aren't modelled (tuned display units, not
  a magnitude).
  `SatModelTool --selftest N` is its gate: known-value checks (Sun declination at the 2020
  solstice/equinox, orbit radius/velocity/inclination, zero off-specular/phase in constructed
  geometry, magnitude conventions) + posed lobes vs posed per-triangle brute force (must agree to
  < 1e-3 mag; configurations fainter than mag 20 are ignored as float noise).
  **The Earth rotation angle is GMST** (`earthRotationAngle()` = 280.46061837° + 360.98564736629°/day
  since J2000, the IAU 1982 linear part; 2026-10-03), so sim time is real UTC to ~1 s. Until then it
  was `kOmegaEarth·t` alone: self-consistent, but every place saw its sky ~5.3 h off real UTC. **Every
  ECI <-> ECEF rotation goes through that one function** (pc.gmst, gmstNow, the observer, the weather
  bake's Earth-fixed Sun, the harness) — never write `kOmegaEarth * t` again. `SatelliteSim::kGmstShiftS`
  (18901 s) added to an old time gives the old view; the start time and the cloud map's default phase
  moved by it, so the intro is unchanged.
- **Benchmark data** (`data/benchmarks/*.json`, format `sat-light-sim-benchmark/1`, loader
  `SatBenchmark.h/.cpp`, milestone M3): one published photometry dataset per file — citation,
  the paper's printed statistics each with a section/table `locator`, the `sampling` spec (shell,
  period, observer sites, stated and unstated constraints, censoring rule), individual
  `observations` when published (rows are arrays named by `columns`), and a `transcription` note.
  Kinds: `distribution`, `differential` (`references.test`/`baseline`). **A file is only trusted
  once `SatModelTool --benchmark <file>` reproduces its published values from its own rows** —
  `mallama2021_visorsat.json` (430 passes, App. A) reproduces n, the 7 censored rows, mean 7.218,
  median, σ and both phase fits (curves within 0.005 mag). `mallama2020a_original.json` is
  summary-only (that paper publishes no per-pass data); `visorsat_vs_original.json` is the 1.29 mag
  occlusion test. Summary-only files added 2026-09-24 for the parity models (Mallama et al.):
  `mallama2023_v2mini` (7.87, mitigated), `mallama2025_dtc` (6.47, held out - passes untuned),
  `mallama2020b_oneweb` (7.18), `mallama2026_amazon_leo` (6.81, operational mode at 630 km),
  `mallama2025_starlink_v15` and `mallama2025_guowang` (Table 1 of arXiv:2507.00107, whose text export
  shifts the count column one row - realigned by hand). Where a paper omits observers, split or
  period the file says so and assumes them. **`"gated": false`** (Guowang: orbit-raising satellites of
  unpublished hardware, model 1.6 mag faint) reports a miss without failing the run or the gate - a
  known residual, logged in `KNOWN_RESIDUALS.md`, not a way to hide a regression. **`"ungated_metrics":
  ["phase_curve_rms"]`** (2026-10-22, VisorSat) is the narrow form: that metric's miss prints `known` and the rest of the
  file stays gated.
  **Sampling windows** (`sampling.constraints` `sun_altitude_deg` [min, max] / `min_elevation_deg`, and the same two keys
  on a site, which override the file's for that site's draws) are MEASURED from the observers' own data and say how in
  the file's note: the 2020 Starlink files' visual sites Sun −24..−10°, elevation ≥ 30° (≥ 15° censored 40% of the
  visual samples where the table has 2.4% not seen); MMT Sun −24..−11° (−33..−11° for V1.0), ≥ 45° (the sampler's
  median elevation then matches MMT's 56°). Until 2026-10-22 every file but BlueWalker 3 sampled the runner's assumed
  −18..−6° / ≥ 20°, and the M8 Starlink calibration was fitted to that: ~1 mag too bright at MMT's real geometry.
  Not copied next to the exe yet — only the tools read them.
- **SatBench runner** (`SatBench.h/.cpp` + `tools/sat_model_tool/bench_run.cpp`, milestone M5):
  `SatModelTool --run-benchmark <file> [--samples N] [--seed S] [--sensitivity] [--report-dir D]`
  simulates a paper's campaign with the benchmark's model (`satellite.model`): site weighted by its
  share of the observations (drawn ONCE per sample and kept through the rejected time / satellite draws, since
  2026-10-22, so each site's share of the samples is its weight), a time in the period with the Sun in the twilight window, then a
  random shell satellite above the elevation limit and fully sunlit, evaluated by
  `evalSatPhotometry()`, censored by the paper's rule at visual sites. Unstated assumptions (Sun
  window -18..-6 deg, min elevation 20 deg, period-start fallback) are `BenchRunConfig` fields
  echoed into the report; `--sensitivity` reruns under alternatives. Deterministic: mt19937_64 +
  bit-cast uniforms (never the implementation-defined std distributions) — same seed, identical
  report bar `run_utc`, on any platform. Reports (`sat-light-sim-benchrun/1`, gitignored
  `benchmark_runs/`) carry the git commit + dirty flag, FNV-1a hashes of the model and benchmark
  files, every sample, and the comparison rows (mean/median pass within 0.3 mag; sd, phase slope,
  and the **phase-matched mean** — samples reweighted to the observed phase histogram, since the
  observers' geometry selection is unpublished — are informational). Plots:
  `tools/benchmarks/plot_benchrun.py <report.json>`. Differential files run both references.
- **Provenance** (`sources` block in a model file, milestone M4): entries `{subject | subjects[],
  status: sourced|derived|estimate|calibrated, value, source}` — `SatModel::sources`, checked by
  `unexplainedModelParts()`. Every group, component and model-local material must be covered;
  `SatModelTool` prints the counts, every non-sourced entry, and any UNEXPLAINED part. A benchmark
  reference model is only accepted with zero unexplained parts. `calibrated` (M8) = fitted to a
  benchmark; its `source` names the benchmark, the metric and which checks stay held out.
- **Phase-curve benchmark + calibration (milestone M8, 2026-09-23).** Distribution reports now carry
  `phase_curve` (model vs observed mean m1000 per 10° phase bin) and the gated `phase_curve_rms`
  (bins with ≥ 10 observations, weighted by count, tolerance 0.3 mag) — the per-pass data's shape,
  not just its mean. `SatModelTool --set [<model>/]<material>.<field>=<value>` overrides a material
  after loading (echoed into the report), and `tools/benchmarks/scan_overrides.py` runs the
  VisorSat, V1.0 and differential benchmarks per override set and tabulates them. Findings, in
  order: (1) the earthshine bug above; (2) with it fixed, GGX beats Beckmann for the polished base
  (Beckmann made 70-110° 0.3-0.5 mag too faint, even though the base's α came from the
  Beckmann↔Phong equivalence); (3) two fitted values — `solar_cell` diffuse albedo 0.06 → 0.02
  (low-phase bins, where the array face dominates) and the VisorSat visor's size at Cole's fixed 23°
  cutoff (inset 0.55 m, gap 0.233 m; curve RMS 0.42 → 0.13). Results (5000 samples, seed 1):
  VisorSat phase-matched mean 7.220 vs 7.218, curve RMS 0.13, mean 6.97 vs 7.22; V1.0 (HELD OUT —
  its antennas are an untouched estimate) 5.926 vs 5.93; differential 1.04 vs 1.29. Remaining: the
  110-120° bin is 0.37 too bright (25 obs); model scatter 0.72 vs 0.85 observed (fixed attitude).
  **Superseded 2026-10-22 (MMT replay refit, scratchpad `starlink_refit`; KNOWN_RESIDUALS.md):** the M8 fit was made under
  SatBench's assumed Sun −18..−6° window; at MMT's real geometry both models read ~1 mag too bright against 94 MMT tracks
  (`SatModelTool --pass`). Refit jointly on 42 tracks (39 held out): `bus_metal` Beckmann α 0.08 (F0 0.9, diffuse 0.11),
  `antenna_white` 0.31, `visor_black` 0.019, visor inset 0.39 m, Minnaert k 0.65 on those diffuse terms (Mallama 2020a's
  own regression: 0.578 ± 0.084). Held-out MMT medians −0.02 (V1.0) / +0.21 (VisorSat), Sun-altitude trend −0.29 →
  −0.07 (V1.0) / −0.36 → −0.22 (VisorSat) mag per 10°. At the measured windows VisorSat 7.428 (+0.21), V1.0 5.904,
  differential 1.52; VisorSat's curve RMS 0.35 is `ungated_metrics` (the table's bin means carry bright-attitude passes).
  The flare-facet population (115 polished facets) was tested and not shipped.
- **Validation report (2026-10-29, wiki `accuracy/validation.md`, `tools/benchmarks/accuracy_report/`).** The evidence
  in figures: `mmt_replay.py` re-runs the 94 MMT tracks with the CURRENT models (inputs from the refit study's scratch:
  `rows.pkl`, `fitset.pkl`, the pass specs; reproduces the logged medians exactly) and finds flares (1-s medians > 0.4
  mag over a sigma-clipped quadratic baseline): 24 in 22 tracks, 0.4-2.0 mag, the model's excess there 0.00-0.02 mag —
  the models make no Starlink flares (they need ~1e-5..1e-4 m² x F of near-perfect mirror; no published geometry).
  `figures.py` draws the page's figures from it, the gate's reports, an Iridium `--dump-json`, the BlueWalker 3
  replay and a harness parity run, printing every number the page quotes. Re-run both after a model change that moves
  a benchmark.
- **Selection panel (buildSelectedSatPanel) + the icon atlas (2026-09-24 UI pass).** The floating
  panel that tracks the selected satellite carries its name, its type and the magnitude readout, then
  one row of **icon-only** action buttons, all drawn by the single helper `buildSelActionButton`
  (which is also what the out-of-view chip uses): **Info** (pixel--info.png — a serif italic "i")
  opens the info window, **Go to** (pixel--eye.png) starts follow mode, **Trace pass**
  (pixel--trace.png) plots the pass, and **Track** (pixel--track.png, a sight reticle) locks the
  CAMERA onto it and re-aims every frame as it crosses the sky. Track is aim-only and moves nothing
  else: `updateTrack()` aims through `aimCameraAzEl` (so `obsFacing` stays the authority, and every
  look input — mouse deltas, the cinematic drift, the gamepad stick — is zeroed for the frame rather
  than left to fight it), while the observer stays where it is (WASD still walks) and the wheel's
  `fovYDeg` zoom is untouched, so the player can pull back and watch. The reticule answers the lock by
  tightening its corner brackets (radius 7 instead of 10) and adding four cardinal bars
  (`SelReticuleTick`: length 7, width 2, gap 3). Released by `select none`, `select planet`, any
  explicit aim (`camera az=`/`el=`, the harness's `camera look`/`camera track`, a scripted camera key)
  and by `startFollow`; `observer lat=`/`lon=` never releases it (the aim re-solves from the new spot).
  Unlike Info / Go to, Track needs no geometry model — any selected satellite can be tracked.
  **Each button's tooltip is its NAME, never a sentence** — the
  same rule the info window's title-bar Select / Go to icons follow — and the `on` state is the accent
  colour (the eye lights while following, the "i" while the info window is showing that satellite,
  the reticle while tracking).
  `kSelIconBtnMin` = 24 px targets, the info window's title-bar icon size; a planet selection gets no
  button row, and the Info / Go to buttons need the type to have a geometry model. Only the satellite
  ACTIONS are icon-only — the info window's CAMERA / RENDER / OBSERVER / CHECK rows are still text
  buttons. The orbit
  rows the panel used to list — altitude, inclination, RAAN, period, the flare-mitigation power readout —
  moved to the info window, as `viewerOrbitLine` under the image; the range/phase line moved to that
  window's OBSERVER box. **The GPU-parity Δmag line is gone from the UI entirely** (it was a
  development instrument in a panel the player reads): `updateSelectedPhotometry` still computes the
  check at the dispatch's own inputs and still logs a mismatch (throttled to 120 frames), it just has
  no readout line — so `selPhotLine` is one line now (`kSelPhotLines` = 1). Icons are packed into one
  atlas by `UIRenderer::loadIcons` in the order listed in `buildUI`'s lazy-load block
  (`assets/icons/ui/*.png`, index constants at the top of `SatelliteSimUI.cpp`); add a path there and
  bump the count when adding an icon.
- **Magnitude trace + CSV (milestone M9, 2026-09-23).** "Trace pass" in the selected-satellite panel
  (or its out-of-view corner chip — both are the icon-only `buildTraceButton`, pixel--trace.png, whose
  tooltip is just "Trace pass", since
  2026-09-24; see *Selection panel*
  above) runs the
  CPU evaluator over the selection's current pass — or its next one, within two orbits — at
  `kTraceSamples` = 400 points (`computeSelectedTrace()`), and
  plots it in its own window: apparent magnitude after extinction, above-atmosphere magnitude,
  phase angle on a second axis, whole-magnitude / phase / sim-clock tick labels (floating Clay text
  placed from the plot's last laid-out size), a moving "now" marker, and two rows of stat tiles (2026-10-03,
  `uiStatTile`): PASS — peak magnitude, its time, the pass length, max elevation (`tracePeakMag` etc., set by
  `computeSelectedTrace`) — and NOW — the clock, magnitude, elevation and phase evaluated per frame with the
  trace's own inputs (so the magnitude sits on the curve). The legend and both axis titles share one line; the
  toolbar is Retrace / Live / Export CSV (kit buttons; Retrace's tooltip carries one retrace's cost) and the
  export result. A status line (amber) says why there is no pass, or that the observer moved with Live off.
  **The window opens default 520x460 (min 340 tall) in the bottom-left, just above the time controls, so it
  coexists with the 3D view window in the top-right corner**; the title names the satellite as the selection
  panel does ("<constellation> #<n>"). **Live** (default on) retraces at up to `kTraceLiveHz` = 10 while the
  window is open, only when `traceStale()` says the result would change (observer moved, selection
  changed, pass over, extinction/tilt/occlusion changed). Tick
  labels are thinned to what fits the plot's pixel size (grid lines stay at every whole magnitude).
  When a satellite can't be traced (legacy type, ground-site aim, no pass
  within two orbits) the window says why instead of plotting. The plot is `UIPlot`, a Clay custom
  element that `UIRenderer::pushPlot()` draws as thickness-wide axis-aligned quads (no line
  pipeline). "Export CSV" writes `<user data>/traces/trace_<model>_<sat>_<sim time>.csv`, format
  `sat-light-sim-trace/1` (`SatTrace.h/.cpp`, shared with the tool): a header carrying every input
  (model id + file hash, lobe budget — `SatelliteType::lobeBudget`, roster-size dependent — occlusion,
  the orbit elements as `orbitElemsOf()` bakes them, observer, flare tilt, extinction k) and rows in
  the design page's export convention. `SatModelTool --replay-trace <csv> [--models-dir D]`
  re-evaluates every row and compares the written fields as strings (the M9 gate); `--selftest`
  runs a write/read/replay round trip per model. Occlusion in a trace is exact (no flux floor).
  **The selection panel's and corner chip's mouse-capture rects come from their real laid-out bounds**
  (`captureLaidOut()`, `Clay_GetElementData`), not size estimates: a click on the Trace button outside
  the estimate fell through to satellite picking, which deselected before the button ran — which is
  why low, faint satellites (shown as the wider out-of-view chip) could not be traced at first.
  **`APP_GIT_COMMIT` is stamped at CMake configure time** — reconfigure before a hand-off build, or
  traces and settings titles carry an old commit.
- **Bulk export (milestone M10, 2026-09-23).** Settings → Photometry → "BULK EXPORT": source (the
  selected satellite or a whole constellation), window (next 24 h / 7 days from the sim time),
  cadence (10/30/60 s). A worker thread (`startBulkExport()`, over snapshots of the type's lobes /
  occlusion and every source satellite's `orbitElemsOf()`) runs `runBulkExport()` (SatBench.cpp):
  every instant at the cadence with the Sun in SatBench's twilight window and the satellite ≥ 20°
  and fully sunlit — time-uniform sampling of visible satellites, which is what SatBench's random
  draws sample — evaluated by `benchEvalSample()`, **the same function SatBench's own draws now use**.
  Written to `<user data>/exports/samples_<source>_<sim time>.csv` by `writeBenchSamplesCsv()`, format
  `sat-light-sim-samples/1` — the one schema: every `--run-benchmark` distribution run also writes its
  samples there (`<report>.samples.csv`, header carrying the run's statistics).
  `SatModelTool --summarize-samples <csv>` reads either and, for a SatBench file, checks it
  reproduces the run's n/mean/median/sd (the M10 gate). `pass` numbers consecutive runs of visible
  instants per satellite; `satellite` is the roster index (-1 for SatBench's random draws).
  `photometryUnsupportedReason()` is the shared "why not" (legacy type; ground-site aim with no
  targets loaded).
- **Accuracy gate (milestone M11, 2026-09-23).** `cmake --build <dir> [--config Release] --target
  accuracy-gate` builds SatModelTool and runs `cmake/AccuracyGate.cmake` — **the only list** of
  checks: every `data/satellite_models/*.json` through `--selftest 400`, every `data/benchmarks/*.json`
  through `--benchmark` (transcription) and `--run-benchmark --samples 5000 --seed 1` (the logged
  configuration). Any non-zero exit fails the target. CI's Linux job runs it (`linux-accuracy-gate`
  build preset) between Build and Package, so a PR to main that moves a benchmark out of tolerance
  fails. ~40 s (23 models, 9 benchmarks, 2026-09-24). Verified identical on MSVC (Windows) and GCC 13 (Linux): the sampling is bit-for-bit
  deterministic and every reported metric matches to the printed precision. Adding a benchmark file
  or a model adds it to the gate automatically. **Accepted inaccuracies** (narrow margins, residual
  phase bins, fitted/estimated values, approximation errors) are logged in
  `data/benchmarks/KNOWN_RESIDUALS.md` — update it when a change moves one. The gate is deliberately
  NOT tightened to chase VisorSat (this is not a model optimizer); M11 runs in CI only on PRs/pushes to
  main, which are reserved for major releases.
- **Mesh renderer + model viewer (Phase 4b, 2026-09-23 — `.plans/SAT_RENDERER_PHASE4.md`).**
  `SatMeshRenderer` (`SatMeshRenderer.h/.cpp`) owns every model's render mesh in one vertex/index
  buffer, plus their materials and occluders. `buildSatRenderMesh()` (`SatMesh.h/.cpp`) is a
  separate tessellation from the photometric one: finer, smooth normals on revolved/sphere
  primitives, UVs in metres, and the same rest pose and body frame, so `evalGroupPoses()` poses it
  and `buildSatOcclusion()`'s occluders line up. `SatelliteType::model` keeps the loaded `SatModel`
  for it. Shaders: `sat_mesh.vert/.frag`, `sat_mesh_bg.*`, `include/sat_mesh_common.glsl`
  (descriptor mirror of `GpuMeshFrame`/`GpuMeshInstance`/`GpuSatMeshMaterial`/`GpuSatMeshOccluder`),
  and `include/earth_env.glsl`.
  - **Units: "π × radiance per unit solar irradiance"**, terrain's convention (a sunlit white
    Lambertian face = albedo × cos). The BRDF is the photometry's GGX/Beckmann · Schlick · Smith
    with the Sun's disc term (`sunA2`) and a material's haze (`specCosMat`), so a render's pixel sum reproduces the
    lobe model.
  - **Lighting:** the sun × litFactor, shadowed per pixel by the model's own primitives (the
    photometry's rule: a primitive never shadows its own surface); earthshine diffuse from the whole
    lit cap (`SatEarthLight`, per instance on the CPU) — or, with an environment probe, the probe's
    SH irradiance; moonlight at `moonGain` (terrain's scale).
  - **Environment probes = the full renderer's reflections and ambient (2026-09-24,
    `SatEnvProbes.h/.cpp`).** `sat_sky.frag` compiled with `-DSKY_ENV` (`sat_sky_env.frag.spv`)
    renders the sky from a SATELLITE's position (`pc.obsECEFDir` = its direction, w = its altitude):
    everything screen-space is cut (half-res cloud / depth / mesh targets, the sky-glow bins, ocean
    glints, beam ground spots), the flat cloud layers draw at full weight, the aurora gets a 16-step
    march of its own (`envAurora`), the sun disc and lens flare are left out, the Milky Way and
    zodiacal bases are turned from the main observer's ENU frame (`CloudParams::envMainObsDir`), and
    the output is pre-exposure HDR. **The post-tonemap terms (Milky Way, zodiacal light, stars) follow
    the VIEWER's rules (2026-09-25):** they are divided back by the exposure of the view that shows the
    result and gated by that view's sun glare — the main view's (`skyExposure()`, `skyGlareEased`) for
    scene probes, the model viewer's (`viewerExposureNow`, `viewerGlareEased`) for slot 0 and its
    background — carried in `pc.sunDirENU.w` = floor(exposure·100) + glare (the env fragment rebuilds
    sin(elevation) from `.z`; `envSkyPC(..., viewExposure, glareVis)`), and the Milky Way's
    sun-proximity dimming now applies in them too. They used the probe position's exposure and no
    glare, so a mirror showed the full Milky Way beside a Sun that had dimmed it everywhere else.
    **Stars** (2026-09-25): SKY_ENV draws the catalogue itself (`envStars`) — the main view's stars
    are point sprites no probe or mirror ever saw. `createStarEnvBuffer` bins it on a 6×32² ECI cube
    grid (skyDescSet binding 24; each star listed in every cell its widest PSF reaches, so a lookup
    reads one cell); each star is the point model's display flux (`point_style.glsl`, the header is
    rewritten by `uploadPointStyle`) as a Gaussian of the main view's sigma in THIS render's pixels,
    carrying its on-screen energy — sharp in the viewer and in mirrors, averaged away in a probe texel.
    A probe is
    six faces of that around one position (`faceCamToWorld`, checked against Vulkan's cube-face
    rule at init), a box mip chain, and an order-2 SH irradiance (`env_probe_sh.comp`, 1024
    Fibonacci directions, read from the 16² level). Face size per slot: the viewer's slot 0 is 512²
    (0.18°/texel, re-rendered ONE FACE PER FRAME, all six only for a new satellite — two a frame while
    it is > 20 km from where the cube was made; at 5 min/s "all six past 20 km" meant every frame),
    scene slots 256²; 128² read as pixelated in mirrors. `sat_mesh.frag` takes the texel size and
    last useful mip from the bound cube (`textureSize`/`textureQueryLevels`), reflects the probe (a
    mip whose texel spans ~2α) and
    takes its SH as diffuse light; the photometric earthshine still feeds the bloom normalisation.
    Each probe is its own cube image and descriptor set (pipeline set 1, bound per draw — no
    cube-array feature). Slot 0 is the model viewer's; scene instances share slots 1-7. **A probe
    belongs to the satellite it was made for and follows it** (`EnvProbeSlot::owner`, 2026-09-25): the
    owner keeps it however far it drifts; another instance shares the nearest probe within max(20 km, 2%
    of altitude) (and adopts one nobody claimed this frame — instances come largest first), else takes
    the least recently used slot. `kEnvRendersPerFrame` (1) re-renders per frame, new probes first, then
    the most drifted (> max(2 km, 0.2% alt), two faces at a time), then any older than 3 s. Until its own
    probe exists an instance BORROWS the nearest rendered one within 1000 km
    (`nearestRenderedProbe`); only with none does it fall back to `earth_env.glsl` + the photometric
    earthshine. Probes used to be keyed by position alone: at 5 min/s a satellite outran the reuse
    radius every frame, took a fresh slot, and every instance but the one rendered that frame dropped
    to the differently lit fallback — the ambient light of every mesh flickered. Off under Potato, the
    mesh knockout, or Settings → Photometry "Full-renderer reflections" (`envReflections`,
    `photometry.full_renderer_reflections`).
    **From near the ground (2026-10-20, the Info 3D view of a rocket on its pad / at 1 km at night was a flat
    yellow-beige):** SKY_ENV draws no procedural city, so its ground took the 5-km night map's light at full strength
    (the main view's pattern averages 0.12 of it close up); it now takes the pattern's close-up MEAN by its own pixel
    footprint (`envPixAngle` = |dFdy(dir)|: `skyLodScreenH` is the main view's). From orbit the footprint is past the
    ramp: unchanged. And the data in the MAIN observer's frame is off in SKY_ENV (`tdExactNear`: the exact DEM texel,
    the coast patches, the launch pads' grading): it described another place from another viewpoint.
  - **Sharp mirror reflections (2026-09-25, `sharpReflections`, Photometry "Sharp mirror reflections",
    `photometry.sharp_mirror_reflections`).** A probe texel is 0.35° (0.18° in the viewer): a Reflect
    mirror filling the screen magnified each over dozens of pixels. For the mirror-smooth pixels
    (sharp share = 1 − smoothstep(0.004, 0.02, α): the `mirror` preset fully, an OSR radiator ~3/4) of
    up to `kMaxSharpReflInstances` (4) instances — those whose type `hasSharp`, largest rectangle on
    screen first — `sat_mesh.frag`'s scene variant writes a reflection G-buffer instead of a probe
    lookup (a 3rd scene attachment, RGBA32UI: octahedral direction, weight = Fresnel × tint × share ×
    fade, slot + 1; flag `GpuMeshInstance::earthShC.y`). `SatMeshRenderer::recordReflections` then
    draws `sat_sky.frag -DSKY_ENV -DSKY_REFL` (`sat_sky_refl.frag.spv`) once per instance inside its
    screen rectangle, from the instance's position (its slot + 1 in `pc.aspect`, which an env fragment
    never reads; skyDescSet binding 25 = the G-buffer), into an RGBA16F target, and
    `mesh_refl_add.comp` adds that into the mesh radiance, rescaling the alpha's photometric share (a
    reflection is not in the model the bloom is normalised by). Exact for a flat mirror at any zoom;
    costs a sky render of the mirror's area. The viewer still reflects its 512² probe (MSAA pass).
  - **Fallback reflections:** the reflected ray goes into `earthEnv()`, the Potato sky's analytic
    atmosphere, textured ground and flat cloud deck, rewritten in ECEF from any origin and scaled by
    `kEnvToScene` into pre-exposure units. The sun disc is not in it; the GGX sun lobe is the glint.
  - **The satellite window + the popped-out 3D view** (split + sections, 2026-09-24). Always a REAL
    satellite where it is now: the selection panel's **Info** chip (it was "View model"; the window
    leads with the orbit, brightness and observer readouts, so it reads as an info pane) opens
    `infoChrome`; "VIEW" on a constellation row picks that constellation's satellite highest in the
    observer's sky (`pickViewerSatellite`), SELECTS it (`selectSatellite`) and opens BOTH windows
    (`openModelViewer(..., popOut = true)`), so the viewer never shows an unreferenced satellite and a
    station is one click to find.
    - **Info window** (`buildInfoWindow`, default 470 wide and tall down to the bottom-right HUD panel (up to
      1000), min 380x560, at the right edge): the title is
      the satellite's name as the selection panel gives it ("<constellation> #<n>"), amber while it is the
      selection, plus the **Select / Go to title-bar icons**
      (`buildViewTitleIcons`; both satellite windows carry them, each with its own hover pair, and
      `buildResizableWindow` skips the title drag when the click is on one — `viewTitleIconsHovered`).
      A **fixed 4:3 render band** sits at the top with the
      **view chips** floating over it (`buildViewChips`, anchored to the image element with
      `attachTo = ELEMENT_WITH_ID` so a resize cannot slide them off), a row of four **stat tiles**
      (MAGNITUDE as seen, ELEVATION, RANGE, PHASE; a phrase shortens to Dark / Below), then **ACTIONS**, never
      collapsed or scrolled (2026-10-19): Trace pass, Go to (Leave while following), Track (each selects the
      viewed satellite first), and **Explode** — ONE row of four equal (grow) buttons since 2026-10-20 (the user: "line
      up the explode button"; it sat alone on a second row; `uiDangerButton` gained `grow` and uiButton's padding)
      (`uiDangerButton`, `Pal::btnDanger*`; ONE click since 2026-10-19 — the
      user: no confirm click; the ASAT strike button is gone, see "Player breakups"). It calls `infoExplodeSelected(kind)` — THE
      call site of a player breakup from this window (today `kesslerBreakupFromUi`; its `kUiMsg` goes to the
      toast). Then the scrollable collapsible **sections** — the Clouds tab's form (`infoSection`, `+`/`-`
      headers, because the font atlas is ASCII-only), every body a label / value grid (`uiKV`), indexed by
      `InfoSect`: STATUS (`infoStatusValue`, formatted each frame: object, constellation, satellite type, status
      operational / dead / launch / debris; for a transient — a rocket, the ship's entry, a re-entry — its
      `transientLabel()`, altitude, speed, the time row from `transientTimeRow`: long durations as d/h/m, a visiting spacecraft by phase - "Since separation", "Docked / Berthed since <UTC> (n days)", "Since undocking / unberthing", "Since entry interface"; it read "Since separation 10086696 s" docked, 2026-10-21), BRIGHTNESS (`viewerPhotValue[6]` with
      `kViewerPhotLabels`), SKY POSITION (`viewerObsValue[4]`), ORBIT (`viewerOrbitValue[5]`; hidden for a
      transient) **open by default**; then MODEL (TECHNICAL) (`viewerSatValue[4]`: type, model id, size, mesh
      triangles and parts), 3D VIEW (camera / light / pose, spin, shadows, reflections, detail, glare, markers,
      Reset view) and PHOTOMETRY CHECK (`viewerCheckValue[3]` + Run check), collapsed. The first cut (2026-09-24 ..
      10-19) opened on SATELLITE (model id, triangles) and hid BRIGHTNESS, SKY POSITION and a BREAKUP section
      below the 3D view's settings: the user found Explode by chance ("how much is hiding in here?").
      `infoSectionOpen[]` is session state, like the Clouds sections'. **MISSION** (`kInfoMission`, the enum's last value
      but drawn right after STATUS, open; 2026-10-20) appears for anything with a launch timeline (see "Mission
      milestones" under *Launches*). The band does not scroll with the
      sections: it is re-drawn each frame from the same crop.
    - **The chips** (`buildViewChip`: the icon alone, tooltip = its name, on a translucent scrim so a
      white icon survives bright clouds) are the 3D VIEW's own controls, four of them: **Spin**
      (the free camera's idle rotation — a plain toggle now, so clicking it while lit stops it and
      "nothing lit" simply means the free camera; taking Observer clears the free camera so Spin reads
      off while it is active), **Observer** (the view from the ground observer's direction, which is
      what makes a ground flare's specular show on the model), **Studio** (LIGHTING, deliberately its
      own group: spin + live light is a legitimate combination) and **Maximize/Restore**. Select and Go
      to are NOT chips: they act on the subject rather than the view, so they live in the title bars.
    - **Pop-out** (`buildViewPopoutWindow`, winId 3, default 900x640 left of the info window, resizable):
      the same render, one chip row, nothing else. **One offscreen target serves both** — sized to
      whichever view is larger (`updateViewerView`) — and each view samples a centred sub-rect of its own
      aspect via `UIImage::u0..v1`, so a 4:3 band and a wide pop-out both show the frame unstretched
      (`viewerUvMini` / `viewerUvPop`). The small view keeps running while popped out.
    - **`updateViewerView` is the shared prologue** (called from buildUI before either window): it sizes
      the target and the background probe, registers the image, handles drag-to-orbit / scroll-to-zoom
      for EITHER view (hit-tested on the two image elements' last-frame boxes and gated on the chips'
      hover flags, or a chip click would also grab the camera), reads the photometric check back and
      refreshes the readouts at 10 Hz — all from the CPU evaluator on the VIEWED satellite.
      `recordModelViewer` runs when either window is open (it used to need the viewer open), so the
      photometric check still works with only the info window up. Both windows register mouse-capture
      rects, and the auto-grow-to-fit-column hack is gone — the sections scroll instead.
    - **The observer is wherever the player is: the camera while following** (2026-10-28, issue 20261028_223234;
      `readoutObsEcef()` / `readoutObsEci(t)`). Go to MOVES the observer (followObsEcef, which updatePositions already
      took as the eye); only the last ground position is remembered (`followSavedObsDir` / `followSavedHeight`: Exit's
      and Return's flight home, settings.json, the tutorial's way back). The selection panel's "Mag", the Info
      window's SKY POSITION / BRIGHTNESS, the "you" marker and Observer preset, `pickViewerSatellite` and the
      tutorial's pick all use it (the panel takes the dispatch's own eye, `parityPending.obsECI`, so the evaluation
      and the GPU see one observer). Until then they used the parked ground position (a "ghost" observer): a
      satellite 34 m from the camera read "Mag: below your horizon". The trace already used the camera's Earth-fixed
      place (obsDir / obsEyeRadiusM). `showYouMarker` hides the marker when the camera is within four model radii
      (the line's clip degenerates with the camera as its endpoint). A ground-site mirror still shows a solid orange
      line to its site.
    - **Markers are drawn OVER the mesh (2026-09-25, `sat_mesh_marker.frag`, `viewerMarkerPipe`):** a
      fullscreen pass after the model, depth-tested, no depth write. Each line pixel takes the line's own
      depth there (z/w is affine in screen space), so the model hides the part behind it and the part in
      front shows; the dots take depth 0 (always visible, unless the Earth hides them). Segments are
      clipped to w ≥ 1e-3 and |x|,|y| ≤ 1.5w in clip space before the divide — the first cut clipped
      only at w = 1e-4, and a line passing near or behind the camera projected to millions of pixels
      and flickered as the camera orbited close. "You" / "Target" labels: `recordModelViewer` projects
      the dots (`viewerMarkerLabels`), `buildViewerMarkerLabels` floats UI text beside them through the
      view's crop.
    - **Glare on the viewer's glints (2026-09-26, `viewerGlare`, RENDER "Glare", default on).** The main
      view's glare, drawn on the glints that make the flare the ground observer sees
      (`SatMeshRenderer::recordViewerGlare`, right after `recordViewer`): a half-res sun-only render from
      the viewer camera (`sat_mesh.frag` **mode 3** = the check's shading, writing the SPECULAR sunlight
      as L·d²), `viewer_glare_find.comp` (per texel effectFlare = L·d²·Ω/π × `viewerFlarePerI`, then
      glare_find.comp's 5×5 maxima), and `glare_mesh.vert/.frag` additive onto the resolved image.
      `viewerFlarePerI` (`updateViewerObserverInfo`) = K_FLUX·brightnessScale/r²·10^(−0.4·ext), the
      observer's, so at the Observer preset the glints carry their share of the ground flare, and the
      threshold is the main view's (glare from about mag 0.3). Live light + a tracked satellite only.
      Only reflections glare: the first cut gated on L > 2 (the most a diffuse face reaches), and a
      Sun-facing rough-glass array (L ≈ 1) never glared. Its glints carry the viewer camera's own range
      (`ViewerGlare::rangeM` = `viewerDist`), so they take the full proximity glare size (2026-09-26,
      above) — the viewer resolves a model from metres away. Harness: `viewer aim=observer|sun glare=`,
      `glints_last_frame`; `tools/harness/scripts/viewer_glare.satcmd`.
    - **Exposure = the sky's rule at the satellite** (`skyExposure()`'s curve on the Sun's elevation in
      its local sky, 2026-09-25): it was the day value whenever the satellite was lit, but a lit
      satellite over twilight is under the NIGHT exposure in the main view — the viewer read ~5x darker,
      as if it had no earthshine.
    Position, velocity and attitude come from
    `satOrbitStateAt` + `evalGroupPoses` in ECEF. With Live light its background is the SKY_ENV
    renderer at the viewer's own camera (`SatEnvProbes::recordViewerBg`, an HDR target
    `sat_mesh_bg.frag` samples and tonemaps) and its reflections come from probe slot 0; Studio light
    keeps `earth_env`. Markers: the observer (cyan dot + a dashed line from the satellite) and, for a
    ground-site mirror, its current site (orange). Camera presets: Free / From you (on the line to
    the observer: the side it shows you) / Toward you (behind it, 12° above the line, your marker
    beneath it). It follows the selection only when the selection CHANGES (`viewerLastSelected`).
  - **Diffuse transmission (Phase 4f)**, `SatMaterial::transmission` / `transmission_color`
    (presets `solar_cell_flex`, `solar_array_flex_back`: ISS-style arrays on a Kapton blanket).
    Light on the far side of a face leaves this side diffusely: `T/π·diffArea·(−n·s)₊(n·o)₊`. That
    term is in every lobe evaluator (CPU `lobeIntensity`, GPU `lobeIntensity`,
    `GpuSatLobe::transmission`, the brute-force references) and in `sat_mesh.frag`, for the sun and
    earthshine. The renderer tints it amber and, with the cell pattern, sends it only through the
    gaps between cells (`Surface::transM`, mean 1). A translucent lobe's occluder mask takes
    same-group occluders on BOTH sides (it is lit from behind). The first cut used the front-only
    mask, which was the ISS's largest occlusion error: its iROSAs shade the light the legacy
    blankets pass through (self-test p95 0.26 → 0.17).
  - **Render-only parts and open lattices (2026-09-24).** A component with `"render_only": true` is
    drawn and nothing else: no facets, lobes, occluder or `sources` entry — greebles cost only render
    triangles. `loadSatModel` moves them after every photometric component, so occluder i is still
    component i. **Their light seeds no bloom and no glare (2026-10-06):** the component buffer's w carries
    +100 for a render-only part (`instCompRenderOnly`, sat_mesh_common.glsl) and `sat_mesh.frag` writes it
    a photometric share of 0. The bloom/glare seed is normalised by the MODEL's intensity, which has no
    greebles in it, so a sunlit aluminium fitting on a Starmind satellite (model intensity toward the
    camera ~0, its arrays seen from behind) seeded white bloom discs ~250 px wide (user snapshot,
    profile_log 2026-10-06 index 2). A material's `"coverage"` (0..1, the `truss` preset: 0.3, pitch `"truss_pitch"`)
    makes an open lattice: facets count coverage × area, a closed primitive adds its inner faces at
    coverage·(1 − coverage), and the part never shadows (`satComponentOccludes`; its occluder is
    kept with `SatOccluder::blocks = false`, kind 0xFF in the renderer). `sat_mesh.frag` cuts the
    members out (`latticeCover`: bay grid + alternating diagonals, member width solved on the CPU for
    the same area fraction), kept per pixel against a threshold, so sub-pixel members at a distance
    become the right fraction of pixels with the far side's inner faces behind them. In the main view with the sky
    TAA on the threshold is interleaved gradient noise stepped by the golden ratio each frame (`frame.marker0` =
    phase, TAA on; the scene frame's marker0 is otherwise unused), so the TAA averages a sub-pixel truss to its mean
    coverage (2026-10-20: with a fixed per-pixel hash a catch tower from ~1 km and a booster's grid fins were a fixed
    speckle that swam as the view moved); the viewer (MSAA) and TAA off keep the fixed hash. Quad UVs now
    start at a face corner, so bays and cell modules start at an edge.
  - **Procedural surface detail** (`SatMaterial::pattern`, JSON `"pattern"`: none / solar_cells / mli
    / panel_seams / truss / hex_tiles; -1 = from the preset — `solar_cell` → cells, `mli_foil` → crinkle).
    `hex_tiles` (2026-10-19, Starship's heat shield): 0.31-m hexagons, 6-mm off-white felt gaps (x6 albedo), each tile
    ±25% in shade with 1.5% paler grey replacement tiles (1.6x, grey 0.5: darker than the felt so their gap
    outline shows; at 2.5x in the felt's grey they read as missing tiles, issue 20261027_184725), ~0.6° of normal jitter; fades to the plain material below ~3 px a
    tile. Visual only and
    **photometrically neutral**: each pattern scales albedo by a factor with area mean 1, leaves the
    specular alone, and is an exact box filter (`lineCover`/`gridCover`) so it keeps
    its mean at any distance. Each pattern has structure you can see with the whole model in frame:
    0.4 m array modules with gaps and a 0.25° tilt per module; MLI quilting seams and 12 cm crinkle.
    The first cut drew only cm-scale detail that faded out below a pixel, so it was invisible at
    normal framing. The viewer's "Detail" button toggles it.
  - **Photometric check** ("Check" in the viewer): a sun-only, single-sampled R32F render from the
    current direction at 60 model radii (`sat_mesh.frag` check mode writes L·d²), read back next frame
    and integrated as Σ L·d²·Ω/π, which is radiant intensity per unit irradiance. It is compared with
    `evalSatLobesPosed()` for the same pose, sun and direction, both as magnitude at 1000 km. With
    shadows on, the two differ by the occlusion sampling error (KNOWN_RESIDUALS); off, the difference
    is render tessellation plus smooth normals versus baked lobes. **Never give a `UIImage` element its own `backgroundColor`:** Clay emits CUSTOM
    before the element's own RECTANGLE, so the background covers the image. That was the first
    cut's all-black viewer. Put the backing colour on a parent.
    `recordModelViewer()` (end of `recordCompute`) places the model at its constellation's altitude
    above the observer and renders it offscreen: MSAA 4x, resolved into a swapchain-format image.
    That image is shown through **`UIImage`**, a Clay custom element (`UIRenderer::registerImage` /
    `updateImage`; each image gets its own descriptor set, drawn as a mode-2 quad). Lighting is
    Studio (sun at 35°) or Live (the sim's sun, including Earth's shadow); pose is Sunlit or Rest.
- **Meshes in the main view (Phase 4c/4d) + follow mode (4e).** Any model satellite whose mesh
  spans 1.5-3 px or more on screen is drawn by `SatMeshRenderer`'s **scene pass**, with no selection
  needed. It is recorded in `recordMeshScene()` just before `scene_depth.comp`.
  - **Choice (4d):** `sat_orbit.comp` writes each model satellite's on-screen diameter into the
    pre-photometry record (`GpuSatVisible::meshPx`, from `GpuSatType::meshRadius` and
    `GpuSatTypeHeader::meshPixelAngle`; the latter is 0 when meshes are off). `sat_flare.comp`
    lists every one past the CPU's nomination size (`GpuMeshKeepList::nominatePx`) into
    `GpuSatListHeader::meshCand[kMaxMeshCandidates = 1024]` (satellite, meshPx, final effectFlare,
    sprite size). The CPU sorts last frame's list by size and draws the largest
    `kMaxMeshInstances` (256): one frame of lag in *which* satellites, none in *where*. **The fade-in
    size adapts (`meshFadePx`, 2026-09-25):** eased toward the size of the 80%-of-the-cap'th largest (up
    in ~0.15 s, down in ~1 s) but never below the cap'th, so no more than the cap ever fade in and none
    is cut off part-way through its fade; the nomination size is 0.8× it, and it rises by 30% while the
    atomic counter says the list overflowed. Until then the first 64 appended were drawn — in
    atomic-append (random) order: in the AI ring ~250 satellites are mesh-sized at once, so which got a
    mesh changed every frame and neighbours flickered 50/50 between mesh and sprite. The instances are
    evaluated in parallel (`parallelFor`, SatelliteSim.cpp: `evalSatPhotometry` + `evalGroupPoses` per
    instance, a few µs each).
  - **Fades are decided on the CPU, this frame, in double** (mesh fades in over meshFadePx-2× it, 1.5-3
    px by default; the sprite fades out over meshFadePx-6.7× it, so the magnitude flare stays with the
    satellite while it resolves).
    The CPU hands the sprite weights to `sat_flare.comp` through `MeshKeepBuf` (descSet binding 12,
    `GpuMeshKeepList`). The first cut let the GPU fade a sprite the frame it nominated it, while the
    mesh came a frame later: a visible "flare, nothing, flare" gap. A kept sprite is drawn at 98% of
    its range so its own mesh doesn't depth-hide it.
  - **Picking:** meshed satellites are hit on their model's bounding circle (`meshDrawn`), ahead of
    sprites, since they have no sprite left to click.
  - **Followed satellite:** always drawn by the CPU, even in Earth's shadow where the GPU never lists
    it.
  - **Reverse-Z:** the scene projection is infinite reverse-Z (depth = 2 cm / distance, GREATER,
    clear 0), so meshes from 1 cm to 1000 km keep precision.
  - **Energy-matched bloom:** each instance's `bloomScale` is the bloom seed its sprite would have
    put down (flare_source: b(effectFlare) × 0.3926·s² over its point disc) per unit of rendered
    flux (Σ L·Ω·r²/π = I), times the share the sprite has given up (`spriteGone`), so sprite
    bloom plus mesh bloom stays constant. `mesh_bloom.frag` sums L·bloomScale per flare texel, reading the instance
    from the colour target's alpha (slot + 1). A flare keeps its full glow across the hand-off, and
    on a large model the glow sits on the glint. The first cut (1 texel per block over 2× white) was
    far too weak.
  - **Targets:** RGBA32F pre-exposure radiance plus R32F *true distance* (0 = none), full swap
    extent, GENERAL layout, always cleared.
  - **Depth pre-pass (2026-09-24):** each mesh is drawn twice — `sat_mesh_depth.frag.spv` (only the
    open-lattice discard) then `sat_mesh_scene.frag.spv` with depth EQUAL, depth writes off and
    `early_fragment_tests` (both `sat_mesh.frag` with `-DMESH_DEPTH_PASS` / `-DMESH_SCENE_PASS`;
    `sat_mesh.vert`'s `gl_Position` is `invariant`). The lattice discard had turned early-Z off, so every
    overlapping layer of a close-up station ran the full shader and its per-pixel occluder-loop shadow
    rays. Scene probes refresh TWO faces per frame (all six only when new, or after a jump in sim time of more than
    max(30 min, 10 wall seconds of the current warp): `EnvProbeSlot::renderedSimT`, 2026-10-20 — a probe made by day
    lit a launch pad's tanks pale blue-white for seconds after a jump to night); a satellite at 7.6 km/s
    crossed the 2 km drift threshold every few frames and re-rendered a whole cube each time.
  - **Shadow-ray acceleration (2026-10-20, `SatMeshRenderer::writeShadowAccel`, sat_mesh.frag `shadowBlocked`):** the
    per-pixel shadow rays (sun, transmission, reflection) looped over EVERY occluder of the model, composing each
    one's group rotation, pivot and stow state from the instance on the way (`rayBlocked`): the ISS filling a
    1920x1009 view cost 59-69 ms of mesh pass (user issue 20261019_225545 shot 3: 85 ms GPU at 12 m). Now, once per
    instance per frame, the CPU poses the blocking occluders in world space (centre + bounding sphere, axes, half
    extents; lattices and stowed parts left out) and bins their shadows on a 16x16 grid on the plane across the
    Sun: a sun ray tests only its cell's occluders (64-bit masks: `kAccelMaxOcc` = `kMaxOccluders`), and every ray
    rejects by sphere before the exact test. Same tests, same image (A/B max 3 levels = frame noise). Slots: 0
    viewer, 1 check, 2.. the 64 scene instances largest on screen (radius / distance); the rest, and knockout bit
    8388608 `mesh_shadow_accel_disabled`, keep the old loop. Mesh set binding 10 (`MeshShadowAccel`, uvec4s;
    layout at `writeShadowAccel`), `GpuMeshInstance::earthShC.w` = slot + 1. The reflection ray's facing test runs
    before its shadow test (half the old cost on its own). Measured (RTX 3070 Ti, 1920x1009, mesh bucket = the
    `scene_depth` timestamp): report pose 68.9 -> 1.1 ms, ISS 50 m 58.5 -> 0.9, 200 m 6.3 -> 0.4, 1 km 0.6 -> 0.3,
    Sun-side 60 m 63.9 -> 1.6, Tiangong 40 m 5.9 -> 0.3, Orbital Reef 60 m 3.9 -> 0.3; the info window's viewer on the
    ISS 27 -> 1.4. Render-only parts (greebles) never had occluders, so the cost no longer grows with the part
    count beyond the vertex work. `harness_runs/hp/` (`before`, `ab*`, `moving` .satcmd).
  - **Consumers**, all through `imageLoad`, because `sat_sky.frag` is one below the 16
    sampled-image floor:
    - `scene_depth.comp` (binding 3) mins the mesh distance into the shared depth, so clouds and
      beams stop at it.
    - `sat_sky.frag` (bindings 22/23): a mesh nearer than terrain *is* the pixel's surface
      (`meshHit` → `tSurface = tMesh`). The atmosphere march stops there and attenuates it, every
      `tSurface` gate (sun/moon disc, Milky Way) hides behind it, and `gl_FragDepth` carries it.
    - `mesh_bloom.frag`, drawn inside the flare-source pass: over-white radiance × the sky's
      exposure seeds the bloom.
  - **Hand-off:** `SatFlarePC::meshSatIdx/meshSpriteKeep` (the old pad3/pad4) fade that satellite
    out of every sprite-side effect (point, bloom, sky-glow bins, ocean glint) as the mesh fades in.
  - **Positions** are CPU double (`evalSatPhotometry` for litFactor/earthshine, `evalGroupPoses`),
    camera-relative. The camera rotation is ENU(obsDir) × `SkyCamera`, the same basis the sky uses.
    Not drawn under Potato (`sat_sky_minimal.frag` has no composite) or knockout bit 2097152.
  - **Follow mode:** "Follow" beside "Trace pass". `followObsEcef` (double) =
    satellite + `followOffset` in its along/cross/radial frame. `obsDir`/`obsHeightOffset` are
    derived from it (the sky shaders read `max(ground, obsHeightOffset)` as altitude, so it is set
    to the altitude above sea level), and `updatePositions()` uses `followRadiusM`. WASD/Q-E move
    the offset (speed ∝ distance); RMB look unlocks the aim lock. Settings persist the ground
    observer, not the orbit. The panel's **Track** is the aim-only sibling: it leaves `followObsEcef`
    alone (the observer keeps walking), and follow mode releases the lock instead of fighting it.
    The frame's along-track axis is the inertial VELOCITY (`velEci`), not `SatOrbitState::velocity`, which for a
    rocket is its body axis and flips through the vertical at a flip or a turn to retrograde.
  - **Stay here** (2026-10-19, `stopFollowHere`; the follow HUD's "Stay here", key G = `KB_FOLLOW_STAY` "Leave Go To
    Here", harness `follow stay`): leaves Go to WITHOUT flying home: the observer becomes a free observer at the
    camera's place (direction, altitude above sea level, facing, elevation, FOV kept; Earth-fixed from then on, so
    with time paused the satellite stays put beside you, and it flies off when time runs). The parked ground
    observer is dropped; WASD / Q-E then work as for any observer at altitude. Settings save it like any observer:
    lat / lon only (the observer's height has never been persisted: a relaunch lands on the ground below). "Exit"
    still flies home. Measured: ISS followed and left paused, the ISS stays within 0.4 px of the screen centre (the
    float `obsDir`), the altitude within 2 m.
  - **Floor, Return, Go to -> Go to (2026-10-20, issues 20261019_223321 / _223437 / _223726):**
    - **The camera never goes underground** (`followFloorM`, end of `updateFollow`): floor = max(the CPU DEM at the
      camera, the GPU ground under last frame's eye `terrainFrameMapped[1]` eased 0.2 s as `followGpuFloorM`, only
      within 20 km of it, 0 = the sea) + 3 m. A camera under it is pushed out radially and, while riding, the offset is
      rewritten from the result (it slides on the ground). The report: a Starship stack at T+73 s with the offset
      -20 km along / -18 km radial, the camera 3.9 km under Louisiana.
    - **A flight in the object's frame** (`followFlightRel`, `followFlightRelA/B`, TNR): the direction slerps
      (opposite sides go over the radial axis) and the distance changes geometrically. Used by (a) `startFollow` from
      another follow (`fromFollow`): the start is the camera NOW (`followObsEcef - followLastP` + the old object at this
      frame's time, `followTargetEcef`; `followObsEcef` alone is an update behind: ~120 m at 1x, km in warp) and B is
      A's direction at the follow distance; it was the observer's line-of-sight flight, whose start distance along that
      line clamped at 1 m whenever the camera was on the far side, so the camera dove through the new satellite
      (train members 120 m apart). Falls back to the arc / line when the Earth is in the way (> 50 km). (b) **Return**.
    - **Return** (`followReturn`, HUD "Return", `KB_FOLLOW_RETURN` H "Go To: Return", harness `follow return`):
      following, fly to the default offset (-4r, 0, 1.5r); on the way home, turn back. Not following: follow the LAST
      object again (`followRemember` in startFollow / stopFollow / stopFollowHere: index + launch owner/member + pool
      generation, or the transient continued to now by `transientContinue`); a "Go to <name> again" chip
      (`FollowAgainBtn`) shows 30 s after leaving. `state` -> `observer.follow_last`, `observer.follow.camera_asl_m /
      dist_to_target_m / flight_rel`.
  - **Selecting by name, not by brightness (2026-10-20, issue 20261019_224758).** Picking in the sky is the GPU's
    visible list (above the horizon, effectFlare past the point style's limit: `sat_flare.comp` writes zero records
    for the rest, which no click can find) — that stays. Everything NAMED goes by index or key: `selectSatellite(idx)`
    never needed the list (roster, train members, retirees, pool pieces; the panel shows the off-screen chip), but
    rockets are transients listed only by the code that draws them (within 4000 km, or already selected), so Missions'
    rocket Select was greyed "Not drawn from here". Now `launchRocketTransient(key, t)` builds the stack / upper stage /
    ship from the launch model, `launchSelectRocketOf` uses it when the list lacks the rocket, and `selTransientUpdate`
    keeps a selected rocket the list misses (transientContinue places it) instead of dropping it after 2 s. Missions
    Ongoing rows: Select always, plus Go to on rockets, trains (nearest member) and retirees (`missionsOngoingAct`,
    harness `missions ongoing <row> [go=1]`).
  - **Follow ends gracefully** (`followCheckTarget` before `updateFollow`, `followTransientUpdate` after the frame's
    transient list): a followed object that ends hands over to what continues it, the camera easing across the jump
    (`followHandD`, ECEF, decays with a 0.6-s time constant) — a raising train member to its roster slot at
    arrival, a retiree / a roster satellite / a debris piece that re-enters to its fireball (kinds 3 / 5, while the
    follow rides a coasting copy for a few seconds until the list has it); a destroyed one, a reused debris slot
    (`followPoolGen`) or a fireball that burns out ends in place (`stopFollowHere` with a toast). `isFollowing(i)`
    is the one "is this followed" test (the selected transient and the followed one are one object in two slots).
  - **The camera holds its offset and aim through a mission (2026-10-21, user: "camera resets when mission objectives
    occur"; harness `follow log on|off`, docs/HARNESS.md).** (1) A transient's frame heading (rocket, ship, capsule,
    fireball) is LAST frame's carried to this point (parallel transport in the local horizontal), turned toward the
    inertial velocity's heading at <= max(1.5 deg per sim s, 20 deg per real s); a velocity that reverses (a
    booster's boostback, a stage slowing to its catch) keeps the side (`Tv = -Tv`); a jump in time (dt_sim > 3x the
    warp's step + 2 s) takes the velocity's heading again (`followBasisSimT`). Roster satellites and launch records
    keep the plain velocity frame. (2) `transientPosAt` (a handover's "from") carries the predecessor on from its last
    state at that state's own time when its model refuses the time (rocketPos past tStageGone, shipEntry past 480 s):
    returning its frame-old `pos` made the eased jump one frame of motion (1.28 km at 10x at the entry interface,
    190 m at the descent). (3) At a handover the view stays put in the object's frame and the camera glides across
    `followHandD`: the view used to be turned at once onto the new object (`followHandViewA/B`, now unused) and back
    as the jump eased out — 6.2 deg at Crew Dragon's separation from 68 m, 21.7 deg at the ship's entry.
    (4) **The default offset goes BEHIND the motion** (2026-10-21, `followDefaultSideOffset` / `followDefaultSideRelB`):
    the carried heading keeps its side through a reversal, so after a boostback "along" points against the ground
    motion and Return's / Go to's (-4r, 0, 1.5r) landed IN FRONT of the booster. The next updateFollow after the default
    is set flips its along sign so it sits behind the motion over the ground (> 30 m/s), else behind a rocket's engine
    end (its nose's horizontal part). An explicit offset (harness `follow offset=` / `toward=`, a cinematic key) clears
    the flag. Measured (Starship booster, T+171 -> T+231 at 10x): held offset +150 m along the motion (in front),
    Return -150 m, a fresh Go to -150 m. `state` -> `observer.follow.cam_along_motion_m` (< 0 = behind),
    `default_side_dot`.
  - **Reflections in the scene** come from the environment probes above (4c part 2, 2026-09-24).
- A model that fails to load logs why and falls back to the type's legacy fields. Examples:
  `starlink_v2_mini.json`, `hubble.json`, `iss.json` (the first parity model: 50 components; station
  root, TRRJ radiators edge-on, SARJ alpha, one beta group with four mast pivots; 8 legacy Kapton
  wings of two blankets each plus the six iROSAs installed by 2023; 514 exact lobes; the integrated
  truss is the `truss` lattice at coverage 0.5, 2.3 m bays, so it no longer shadows as a solid box), `tiangong.json`
  (T of three modules; the labs' two-axis wings as alpha about the labs' axis + beta with two
  pivots), `starship_depot.json` (9 x 60 m body of revolution in the `stainless_steel` preset with a
  body-mounted solar band), `spacex_ai_sat.json` (Starmind AI1, rebuilt twice 2026-09-24 to the project
  owner's reading of the AI1 spec sheet: nadir-pointing, a flat 10 x 3 x 0.6 m "door" bus that stacks
  PEZ-style with its 3 x 0.6 m end toward the Sun, two 10 m-long white radiators standing up and hanging
  down from its flat faces (20 m top to bottom, edge-on to the Sun), and two 70 m-span wings of three
  strips, each on ONE truss to a gimbal at its root, tracking the Sun about the span axis (then the
  flare-mitigation tilt, toward zenith, about +Y); 10 lobes, flown by a million; trusses, gimbals,
  spars and terminals are render-only), `reflect_orbital.json` (Reflect Orbital's published quad sail since 2026-11-04 — see "Reflect Orbital quad sail" below;
  until then a 55 m square membrane mirror; on `sun_reflect_ground_site`; its beams take
  the mirror's area and F0 from the lobes along the site-aimed axis — `bakeModelType` writes them into
  the legacy `crossSectionM2`/`mirrorFrac` the beam code reads, which a model type had left at 10 m²
  and 0, so no Reflect beam was drawn from the roster's move to the model until 2026-09-24),
  the Starlinks `starlink_v1_5` (gen-1 dielectric mirror film, a translucent 'lampshade' backsheet
  whose transmission is fitted to the Post-VisorSat phase function), `starlink_v2_mini` (rebuilt
  2026-09-24 to SpaceX's published mitigations: gen-2 film on the nadir face, black paint, opaque
  backsheet, arrays tracking the Sun but held within 90 - rho of zenith so their plane never dips
  below the Earth's limb), `starlink_v2_mini_dtc` (+ a 25 m2 nadir phased array) and `starlink_v3`,
  then `oneweb`, `amazon_leo`, `guowang`, seven commercial stations (`haven1`, `haven2`,
  `axiom_station`, `orbital_reef`, `ross`, `bharatiya_station`, `starlab` - low confidence, render-level
  sizes; since the 2026-10-20 art pass GENERATED by `tools/make_station_models.py`, see "Station models" below)
  and `debris_fragment` (tumbling) - the parity models; sources and estimates in each file,
  magnitudes in `KNOWN_RESIDUALS.md`. Presets added for them: `stainless_steel`,
  `dielectric_mirror_gen1/_gen2` (Beckmann: GGX's tail made the film forward-scatter grazing sunlight),
  `array_backsheet_dark` - and the M4 benchmark references `starlink_v1_0.json`
  (Mallama 2020a period: shark-fin, array edge-on to the Sun) and `starlink_visorsat.json`
  (Mallama 2021 period: array fixed 24 deg from vertical away from the Sun, radio-transparent
  visor sheet under the antennas) — both sourced from Cole 2021 (arXiv:2107.06026) and
  McDowell's size table; roster: the primary `data/constellations.json` (the working set). The visor's
  real shape is unpublished (derived from Cole's 23 deg full-shade constraint), and the visor only
  dims anything through occlusion — so the VisorSat differential needs Phase 3b.
- **Station models (art pass 2026-10-20, issue 20261019_225545, `tools/make_station_models.py`).** The seven
  future stations are written by the generator (edit it, never the JSON); it also appends render-only `gb_*`
  detail to the hand-made `iss.json` / `tiangong.json` (re-running replaces the old `gb_*` parts and their
  material source entry, so it is idempotent). Layouts from the operators' concept art and published figures
  (cited per model): Haven-1 (cone-nosed 10.1 m hull, aft wings on yokes, a docked Dragon), Haven-2 (a 7 m core
  standing nadir-zenith, four 16 m modules docked radially in a horizontal cross, a wing pair per module, four
  radiator fins on the core), Axiom (11 m modules with a curved body-mounted array over the zenith half: a
  profile `arc_material`, PPTM with ISS-style Kapton wings, edge-on radiators, the glass Earth Observatory), Orbital
  Reef (7 x 28 m core, two LIFE inflatables off its sides, an aft truss of four 10 x 13 m wings, forward
  radiators), ROS (the Dec 2025 plan: Nauka + UUM + NEM + ShM + base module + two target modules), BAS (BAS-01/02/05
  in line, 03/04 on BAS-05's side ports, all 3.8 m) and Starlab (8 m domed metal habitat, four wings in a 45-deg
  cross on the service module, each turning about its own long axis: two diagonal-axis groups). Rules it keeps:
  (1) **every wing turns about its OWN long axis** (`arrays_y` / `arrays_x` / diagonal groups, per-component
  pivots on the axis), and a render-only yoke / mast joins it to its module (the old models' wings floated);
  (2) **photometric hulls are split into one profile per meridian segment** (`Station.hull`): a whole domed
  profile's occluder is the frustum through its two END radii (a cone for a LIFE habitat) and the occlusion
  self-test failed (Orbital Reef p95 0.30 vs the 0.25 gate); (3) detail (ports, rings, dishes, windows, RCS
  pods, bands, arms, visiting Dragon / Soyuz / Shenzhou / Tianzhou) is render-only and kept to a few dozen parts.
  Render-mesh triangles before -> after: ISS 5304 -> 8296 (+48 parts: SAW masts and blanket boxes, PMA-2 + Dragon,
  Soyuz, Progress, Canadarm2, ELCs, AMS, Ku dish, cupola glass, bands), Tiangong 2920 -> 4864 (wing masts, Shenzhou,
  Tianzhou, arm, radiators), Haven-1 200 -> 1996, Haven-2 584 -> 4880, Axiom 968 -> 4080, Orbital Reef 584 ->
  5804, ROS 584 -> 5292, BAS 584 -> 2392, Starlab 392 -> 4216. m1000 in KNOWN_RESIDUALS.md.
  **Module by module (2026-10-21):** a component's `"since": "YYYY-MM-DD"` (and a docking port's) is the date its
  module arrives; with the deployment era on a part is not there before it — not drawn (`sat_mesh.vert` collapses it to
  one point outside the clip volume, `instCompAbsent`: the component buffer's [5c + 4].w = its build-date code
  `satSinceCode`, the instance's `trans[0].w` = today's, `SatelliteSim::meshBuildCode`; 0 = every part), casting no
  shadow (`rayBlocked`, the shadow accel) and carrying no light in the photometry: lobes never merge across dates
  (`bakeSatLobes`: the date is in the exact-merge and budget-merge keys) and a lobe's code rides in
  `GpuSatLobe::distribution` bits 8-31 (bits 0-7 the GGX / Beckmann flag: `satLobeBeckmann`), its share
  `satLobePresence` on the CPU (`evalSatLobesPosed(..., buildDays)`, `SatOrbitElems::buildByDate` set by
  `rosterElemsOf` with the era on) and `lobeBuilt` in sat_orbit.comp against `GpuSatTypeHeader::buildDays` (the
  header is 128 B now; 1e9 = every part). Crew flights dock only at ports there at docking (`buildCrew`). Dates
  (`Station.since()` in the generator; `patch(..., dates)` for the hand-made Tiangong): Tiangong Tianhe 2021-04-29,
  Wentian 2022-07-24, Mengtian 2022-10-31 (real); Axiom PPTM 2027-06, Hab-1 2028-03, Airlock 2029-01, Hab-2 2029-09,
  RMF 2030-06; Haven-2 the forward module 2028-06, the core 2029-06, a spoke a year to 2032-03; ROS the core (Nauka +
  UUM + NEM) 2028-09, ShM 2029-06, the base module 2031-06, the target modules 2033-03 / 09; BAS-01 2028-12, -02
  2031-06, -05 2032-06, -03 2033-12, -04 2035-06; Orbital Reef the core 2030-06, LIFE 2031-03, the science module
  2031-09, LIFE 2032-06 (later dates the scenario's). The ISS, Haven-1 and Starlab (one launch) have none. Exact lobes
  grow where modules no longer merge (Tiangong 396 -> 438); selftests pass. NOT done: the dated parts' OCCLUDERS still
  block in the photometry's opt-in part occlusion (CPU and GPU alike, so parity holds); the mesh glint-share loop
  (`glintShare`) counts absent lobes; module launches are not flown (another agent was turning crew spacecraft into
  visiting vehicles: a module launch with a "module docked" milestone is the open item).
  **(2026-10-20, crew vehicles)** The visiting CREW vehicles are no longer greebles (the sim flies and docks real ones:
  Launches > "Crew spacecraft"); the generator writes each station's `docking_ports` instead (`dock()`, `port(...,
  dock=True)`; the ISS gained PMA-3 on Harmony's zenith port). Since 2026-10-21 the cargo ships are flown too: the
  ISS's Progress, ROS's Progress and Tiangong's Tianzhou greebles are replaced by `"use": "cargo"` ports (Zvezda aft,
  ROS's aft port, Tianhe aft; `dock(..., use=)`, `port(..., use=)`). Photometric parts unchanged.
- **Radiators turn continuously (2026-10-20, issue 20261020_214656: "advance time here, the radiators totally
  flip").** The ISS's TRRJ radiators (and Axiom's, Orbital Reef's) were `edge_on` joints: "the smaller of the two
  solutions pi apart", which swaps solutions wherever the two are equal — twice an orbit the whole radiator group
  jumped ~179 degrees (harness `joints`: 178.7 deg between 20-s samples, 20 s after the report's shot). Joint mode
  `edge_on_continuous` (`JointMode::EdgeOnContinuous`, sat_orbit.comp `JOINT_EDGE_ON_CONT` = 5, SatModel.cpp
  `evalGroupPoses`) always takes theta = atan2(-a, b): (axis x vector) turned toward the target, +-pi being the same
  pose, so the joint turns once per orbit (1.3 deg per 20 s). `edge_on` keeps its old meaning (the legacy KnifeEdge
  Starlinks rely on its clamp). Every station joint over three orbits: `scripts/station_joints.satcmd` (all <= 2.3 deg
  per 20 s except Haven-2's / BAS's `arrays_x` track joints, 7 deg, where the Sun passes near that axis — a fast
  turn, not a flip). The photometric lobes are unchanged (a joint mode changes only the pose).
- **Reflect Orbital quad sail (2026-11-04, the published deployed configuration; `tools/make_reflector_model.py`
  writes `reflect_orbital.json`).** An 18 x 18 m square of four triangular aluminised-film quadrants (`"type":
  "polygon"`, below) on two crossed telescoping booms along the diagonals (body X and Y, 26 m tip to tip, three
  sections of 60 / 48 / 36 mm radius each side, 0.12 m behind the film), a 0.4 m deployer hub and a 2.44 m bus behind
  it. Each quadrant has a scalloped outer edge (0.9 m sag) and boom edges (0.25 m), 0.15 m clear of the booms and cut
  back to 1.3 m from the centre: 256.4 m2 of film (the 55 m plate it replaced: 3025 m2, so the satellite and its beams
  are 2.68 mag fainter). **The quadrants are coplanar by default** (`--cant-deg 0`): the scallops are the edge-tension
  design that keeps each quadrant flat, and the analysis below shows any cant past ~0.13 deg darkens the aim point.
  Coplanar quadrants share one exact lobe (identical group / material / normal), so the photometry is a single
  256.4 m2 mirror lobe; the beam code's `crossSectionM2` is that lobe's area. Generator knobs: `--side`,
  `--cant-deg` (each quadrant turned about its hub edge, outer corners toward +Z: a shallow concave dish whose four
  normals lean toward the centre axis), `--outer-sag`, `--boom-sag`, `--gap`.
  **How the quad sail shapes the beam** (`tools/reflector_quad_analysis.py`: each film patch reflects the
  limb-darkened Sun, u 0.6, blurred by the film's GGX alpha 0.0005; 500 km, normal incidence):

  | case | film m2 | peak mW/m2 (lx, x full Moon) | mag at peak | mag at the aim point | half-peak diameter |
  |---|---|---|---|---|---|
  | flat 18 m square | 324 | 28.4 (2.65 lx, 10.6x) | -15.04 | -15.04 | 4.2 km |
  | 4 coplanar quadrants (the model) | 256.4 | 22.5 (2.10 lx, 8.4x) | -14.79 | -14.79 | 4.2 km |
  | cant 0.10 deg | 256.4 | 16.1 | -14.42 | -14.42 | 4.3 km |
  | cant 0.25 deg | 256.4 | 5.6 | -13.28 | -9.16 | 4 spots |
  | cant 0.50 deg | 256.4 | 5.6 | -13.28 | -5.69 | 4 spots |
  | cant 1.00 deg | 256.4 | 5.6 | -13.28 | -2.57 | 4 spots |
  | billow 2 cm (each quadrant a concave paraboloid) | 256.4 | 5.0 | -13.15 | -13.15 | 8.1 km |
  | billow 5 cm | 256.4 | 0.85 | -11.23 | -11.23 | 21.4 km |

  The Sun's disc sets the spot: 9.3 mrad, 4.65 km across at 500 km; diffraction (lambda / D = 3e-8 rad, 1.5 cm) and
  the outline (18 m) are invisible in it, so the scalloped edges matter only through the area they remove (-0.25 mag
  against a full square). Coplanar quadrants are therefore exactly one mirror of their summed area: one lobe, one
  spot, the same pass curve 0.25 mag fainter than a full 18 m square. A cant of theta turns each quadrant's beam by
  2 theta, so the spot splits into four sub-spots 2 theta R from the aim point along the diagonals (quadrant
  bisectors), each with a quarter of the light: their limb-darkened discs still overlap the aim point below
  2 theta = 4.65 mrad (theta < 0.13 deg; at 0.10 deg the aim point loses 0.37 mag and the spot widens to 4.3 km),
  separate from each other past theta = 0.19 deg, and leave the aim point dark beyond (0.25 deg: -9.2 at the aim
  point, four -13.3 spots 4.4 km out; 0.5 deg: 8.7 km; 1 deg: 17.5 km). The sub-spot offset and the spot size both
  scale with range, so the split (and the aim point's loss) is the same at every elevation of a pass; only
  A cos(i) / R^2 changes: a pass over the aim point (Sun 15 deg down) runs -14.5 overhead to -12.5 at 15 deg
  elevation for the coplanar sail. Billow is worse than cant for the same film slope: a 2 cm sag over a quadrant
  spreads slopes over ~7 mrad and the light over an 8 km spot (-1.64 mag at the centre). **The app cannot show a
  cant today**: the exact bake makes four quadrant lobes, but they are the closest normals in the model, so the 48-lobe
  budget merges them first into ONE lobe with the spread folded into alpha (0.5 deg: alpha 0.0087 — one broadened
  lobe, ~3 mag fainter at the centre, where the physics gives a dark centre and four -13.3 spots), and the beam's
  ground spot (`crossSectionM2`, one spot at the aim) has no sub-spots either. A canted default would need both to
  learn sub-lobes first; the coplanar default needs neither.
  **The polygon primitive** (`PrimitiveKind::Polygon`, JSON `"vertices": [[x, y], ...]` in local XY, front +Z,
  counter-clockwise from +Z and star-shaped about the vertex mean — checked at load; `back_material`,
  `single_sided`): fanned from the vertex mean in both the photometry (`tessellateSatModel`) and the render mesh
  (`buildSatRenderMesh`, uv = local XY). A membrane never occludes: its occluder entry is a non-blocking plane over its
  bounding box (occluder i stays component i) and `satComponentOccludes()` is false, so the brute-force reference and
  the GPU agree. **Deployment**: the booms stow toward the hub along their axes (plane stow, scale 0.06) and the film
  with the new RADIAL stow (`"stow": {"mode": "radial", "axis": [0, 0, 1], "scale": 0.08}`, launches.md "Unfolding
  after release"), both over the whole window, so the quadrant corners ride the boom tips out; 10 min after release,
  over 15 min (estimates).
- **Occlusion between parts (Phase 3b / benchmarking M7).**
  `buildSatOcclusion()` makes one occluder per component (plane, box, capped cylinder — a cone uses
  its larger radius, conservative — sphere; `kMaxOccluders` 64, a 64-bit mask per lobe) and
  `kDefaultLobeSamples` = 16 area-weighted sample points per COMPONENT of each lobe, up to
  `kMaxLobeSamples` = 64 per lobe (weighted k-means within each component over 16 sub-triangle
  centroids per source triangle, more on triangles over 32 m²; `bakeSatLobes(..., &lobeTris)`
  supplies the triangles). Until the ISS a lobe got 16 in all: its 16 array blankets share one lobe
  and got ~1 sample each, so any partial shadow on a blanket was all or nothing. A lobe's intensity is scaled by
  the weighted fraction of samples whose rays toward the light AND the observer both leave the
  satellite (earthshine: observer ray only), tested against every occluder posed at the live joint
  angles. Two rules keep it exact and cheap: **a primitive never occludes its own surface** (all
  are convex or flat — and without this a faceted cylinder's samples sit just inside the smooth
  cylinder and shadow themselves: Hubble read 3 mag too dim), and a same-group occluder counts only
  if part of it lies in front of the lobe (either side, for a translucent one; `occluderMask`, fixed
  at bake time). `--selftest` checks it against a ray-cast reference (25 points per triangle, one per
  ~0.5 m² on large ones, each component culled by its bounding sphere): p95 at 16 samples 0.14 on
  VisorSat (the visor's 8.5 cm gap), 0.17 on the ISS, 0.07 on V2 Mini, ≤ 0.02 elsewhere.
  **Coincident surfaces break the float GPU form:** two modules whose end caps touch, or a panel
  flush against a box, put samples exactly on a shadow edge, and float and double disagree there
  (the ISS read 0.54% of lobe evaluations different until its parts got 0.3-0.4 m berthing gaps).
  **GPU cost scales with lobes × samples on ONE thread per satellite**, so with occlusion on a
  station is far more expensive than a Starlink (the ISS: ~2300 samples at 48 lobes, more at the
  app's 256). Not measured yet. SatBench uses
  it by default (`--no-occlusion` = the M6 baseline).
  **GPU (2026-09-23):** `packSatOcclusionGpu()` converts it to `GpuSatOccluder` (80 B) +
  `GpuSatLobeSample` (32 B, point pre-nudged 1e-4 m) in ROOT TRIAD coordinates, like lobe normals,
  and writes each lobe's `sampleFirst`/`sampleCount`/`occluderMask`; `satOccluderBuf` /
  `satLobeSampleBuf` are bindings 9/10 of the `sat_orbit` set. The shader's group frames were
  rotation-only, so `typeOffsets()` rebuilds each child's translation from the frames and
  `GpuSatType::originT` (the rest hinge point) — the mirror of `evalGroupPoses()`' `pose.t`.
  `modelFlux()` computes the unoccluded total first, and only then — if `occlusionOn`, the type has
  occluders, and that total clears `occlusionFluxFloor` (mag `kOcclusionMagFloor` = 10; occlusion
  only dims) — subtracts each lobe's blocked share. Lobes facing away cost nothing; a lobe under
  1e-4 of the total is left unoccluded (≤ 0.005 mag, the one deliberate CPU/GPU difference). A
  sample whose observer ray is blocked stops its occluder loop. The parity readout applies the same
  on/floor gate. `--selftest` re-evaluates the packed GPU form in float the way the shader does
  (`selfTestOcclusionGpuForm`) against `satLobeVisibility`: 0 of 23,200 lobe evaluations differ
  across the five models, offsets within 3e-7 m. **In the app it is OPT-IN and off by default**
  (`satPartOcclusion`, Photometry tab "Satellite part occlusion", settings key
  `photometry.sat_part_occlusion`): measured 30.1 ms vs 2.8 ms orbit compute at 955k visible on the
  10M stress roster, for a subtle effect — no preset or first run may turn it on. Knockout bit
  1048576 can still force it off while it is on; both reach the shader as
  `GpuSatTypeHeader::occlusionOn` (`satOcclusionActive()`). SatBench keeps it on by default (physics
  validation, not an app setting). `data/custom/constellations_models_stress_10m.json`
  is the 10M all-model roster to profile it with. Still to do: calibration + reference models (3c),
  an inertial-pointing law (Hubble's attitude is an anti-sun stand-in).

### AttitudeMode values (legacy — converted to groups at load)
| Mode | surfN | Use case |
|------|-------|----------|
| `NadirPointing` | satNadir | Antenna/array face toward Earth (Starlink) |
| `SunTracking` | sunDirECI | Solar panels track sun (LEO Broadband, ISS) |
| `Tumbling` | spinning around random body axis | Debris, uncontrolled objects |
| `Perpendicular` | cross(surfN0, satNadir) | Secondary only — derived from primary normal |
| `AntiNadir` | -satNadir | Radiators facing deep space; brighter near horizon |
| `FlatMirror45` | normalize(sunDir + satNadir) | Flat mirror reflecting sunlight straight down |
| `TargetedReflector` | normalize(sunDir + toTarget) | Mirror aimed at nearest valid night-side ground target |
| `KnifeEdge` | roll around velHat; clamped ±80° | Starlink post-2020 roll-angle policy (Mallama 2023) |
| `SunPerp` | normalize(cross(sunDirECI, satNadir)) | Thermal radiator edge-on to sun; irr=0 always (correct thermal design — never receives direct sunlight). Visual contribution via diffuse. Used for AI1 datacenter radiators. |

`velHat` is computed in `sat_orbit.comp` from the orbital trig already in scope: `{-sinU·cosR - cosU·cosI·sinR, -sinU·sinR + cosU·cosI·cosR, cosU·sinI}` — already unit length for circular orbits.

### Satellite type catalogue (typeIdx)
| Idx | Name | Area (m²) | Primary attitude | Secondary | mirrorFrac |
|-----|------|-----------|-----------------|-----------|------------|
| 0 | Starlink | 10 | NadirPointing, spec=18 | — | 0.05 |
| 1 | LEO Broadband | 5 | SunTracking, spec=18 | — | 0.02 |
| 2 | GEO Comsat | 50 | SunTracking, spec=3 | AntiNadir, w=0.10 | 0.10 |
| 3 | ISS | 250 | SunTracking, spec=12 | AntiNadir, w=0.35 | 0.05 |
| 4 | SpaceX AI Sats | 600 | SunTracking, spec=25 | SunPerp, w=0.18 (radiators) | 0.01 |
| 5 | Reflect Mirror | 2376 | TargetedReflector, spec=200 | — | 0.97 |
| 6 | Debris | 1 | Tumbling, spec=6 | — | 0.03 |
| 7 | Starlink KE | 10 | KnifeEdge, spec=18 | — | 0.05 |

`crossSection = sqrt(crossSectionM2 / 10.0)` — so 10 m² → 1.0, 2376 m² → ~15.4.

### Satellite type data source
**`data/constellations.json` is the v1.1 roster ported to geometry models (2026-09-24)**: every v1.1
constellation with its v1.1 orbit and count (ISS and Tiangong at their current orbits) flying a model
type, ~1.38M satellites, plus Hubble and the 2020 V1.0 / VisorSat benchmark shells. **Constellations ship
enabled** - the user wants to see them (even the 1M AI disk). The disabled legacy-lighting Gen2 comparison
shell and its two-surface type were removed 2026-10-19 (the user: "should be removed"); a legacy Gen2 still
lives in `data/custom/constellations_attitude_example.json`. The v1.1 legacy roster is preserved as
`data/custom/constellations_v1_1_default.json`. The build copies `data/constellations.json` over the exe-dir copy whenever the
exe relinks — which is why hand-pasted rosters there kept "reverting": edit the file in `data/`.

**Settings > Constellations (2026-10-20, `buildSettingsConstellationsTab`)** lists the roster GROUPED by category,
folded at first (`constGroupOpen_`; persisted since 2026-10-21 with the open member list and its search, settings.json
`constellations_ui` {groups_open, list_open, list_filter}): a group header has ON / HLT for every entry in it (MIX when
some are on) and the group's satellite count; an entry row has ON / HLT and its action — Select / Go to for a
one-satellite entry (a station), LIST for a List entry (its members: a search field over name / operator /
purpose, pages of 25 sorted west to east, Select / Go to each; destroyed members greyed), else VIEW (3D view of
its satellite highest in the sky). Formatted strings live in `constUiText_` (a per-build deque: Clay keeps the
pointers). Ids: `ConstGrpName:<g>`, `ConstGrpOn:<g>`, `ConstGrpHlt:<g>`, `ConstBtn:<ci>`, `ConstListBtn:<ci>`,
`ConstListFind:<ci>`, `ConstMemSel:<sat>`, `ConstMemGo:<sat>`, `ConstSel:<ci>`, `ConstGo:<ci>` (harness `ui click`).

Types and constellations are loaded from `constellations.json` next to the exe. If the file is missing or malformed, `loadHardcoded()` provides the catalogue above as a fallback. The JSON schema is in `constellations.schema.json`.

### Adding a new satellite type
1. Add to `satTypes` in `constellations.json` (or `loadHardcoded()` as fallback) — either a legacy
   `attitude` per surface, or `attitude_groups` + surface `group`/`normal` for anything the legacy
   modes can't express (see "Rigid attitude groups" above)
2. No GPU struct changes needed; all fields map to existing `GpuSatType` members
3. Reference the new typeIdx in a constellation entry

---
