# Design notes

Why SAT LIGHT SIM is built the way it is, as a dated record: decisions, alternatives that were tried and
dropped, measured before-and-after numbers, and the bugs whose lessons became invariants. The main pages
state the current design and its rules; this page keeps the story behind them.

Entries are grouped by subsystem and dated, oldest first within each group. Each links the main page the
decision concerns. Dates come from `git log` and from the working notes in `CLAUDE.md`; "review N" refers to
the numbered rounds of user feedback that drove much of the 1.2 work (late September to early October 2026).
New entries arrive through wiki passes (see [History](index.md)).

!!! note "Not a commit log"
    Only decisions a future developer would want to know about before changing the code are kept. Small
    tuning changes, renames and one-off fixes stay in `git log`.

## Satellites and photometry

- **2026-03-18** — The first model was two surfaces (primary, optional secondary) each oriented by an
  `AttitudeMode` enum, a Phong lobe and a `mirrorFrac` spike boosted by `MIRROR_BOOST` = 300. It produced
  display units tuned by eye, not magnitudes. It survives as the "legacy" type path.
  [Photometry](../simulation/photometry.md)
- **2026-06-02** — `KnifeEdge` attitude added for Starlink's post-2020 roll-angle flare mitigation (Mallama
  2023). [Attitude](../simulation/attitude.md)
- **2026-09-22** — Attitude became data: rigid attitude groups, each a two-vector (TRIAD) law plus an optional
  one-axis joint, replaced the enum switch. Every legacy mode is converted at load by a table and reproduces
  the old normals exactly (checked over 20k random geometries per mode; the two joint modes to ~1e-15).
  `Perpendicular` has no fixed-body equivalent in general but is only ever used at weight 0.
  [Attitude](../simulation/attitude.md)
- **2026-09-22** — Geometry models: primitives with materials are tessellated and merged into at most 48 facet
  lobes per type, evaluated with GGX, Schlick and Smith. Folding the Sun's disc into every lobe as
  `SUN_ALPHA` = 0.0023 reproduces a flat mirror's physical peak (1.50e4 vs 1.47e4 m^2), so model types need no
  `mirrorBoost` or cross-section hacks. [Satellite models](../modding/satellite-models.md)
- **2026-09-22** — The CPU photometric evaluator (`evalSatPhotometry()`, benchmarking milestone M1) became the
  double-precision mirror of the GPU path, with a GPU parity readout (M2). Benchmark data format (M3),
  reference models with provenance (M4) and the SatBench campaign runner (M5) followed the same day.
  [SatBench](../accuracy/satbench.md)
- **2026-09-22** — Benchmark files are trusted only once `SatModelTool --benchmark` reproduces the paper's own
  printed statistics from its rows. `mallama2021_visorsat.json` reproduces n, the censored rows, the mean
  7.218, sigma and both phase fits. [Benchmark datasets](../accuracy/benchmarks.md)
- **2026-09-22/23** — Occlusion between parts (Phase 3b, M7) on the CPU, then the GPU. Two rules make it exact
  and cheap: a primitive never occludes its own surface (without it a faceted cylinder's samples sat inside
  the smooth cylinder and Hubble read 3 mag too dim), and a same-group occluder counts only if part of it is
  in front of the lobe. Coincident surfaces (touching end caps) disagreed between float and double at shadow
  edges, so models carry 0.3-0.4 m berthing gaps. [Satellite models](../modding/satellite-models.md)
- **2026-09-23** — Part occlusion measured 30.1 ms vs 2.8 ms of orbit compute at 955k visible satellites on the
  10M stress roster, for a subtle effect, so it ships opt-in and off; no preset may turn it on. SatBench keeps
  it on for validation. [Photometry](../simulation/photometry.md)
- **2026-09-23** — The phase-curve benchmark (M8) exposed an earthshine bug: `0.3 (R/d)^2 litFraction` treated
  every lit ground point as if the Sun were overhead, 17x too bright with the Sun on the horizon and 60 to
  15 000x at twilight zenith angles. It had been carrying the benchmark by lighting nadir faces. Replaced by
  the exact irradiance of the lit cap, tabulated for both CPU and GPU. Legacy types kept their own term.
  [Photometry](../simulation/photometry.md)
- **2026-09-23** — With earthshine fixed, GGX beat Beckmann for the polished base (Beckmann made 70-110 deg of
  phase 0.3-0.5 mag too faint). Two values were then fitted: `solar_cell` diffuse albedo 0.06 to 0.02 and the
  VisorSat visor's size at Cole's 23 deg cutoff, taking the curve RMS from 0.42 to 0.13. The V1.0 benchmark
  stayed held out and landed at 5.926 vs 5.93. [Results](../accuracy/results.md)
- **2026-09-23** — Magnitude traces with CSV export (M9), bulk export in SatBench's sample schema (M10) and the
  CI accuracy gate (M11). The gate was deliberately not tightened to chase VisorSat: it guards regressions
  and is not a model optimiser. [SatBench and the accuracy gate](../accuracy/satbench.md)
- **2026-09-24** — Earthshine as a broad source: a single tilted direction gave a face edge-on to it nothing,
  though the cap really gives it ~30% of a nadir plate's light at 550 km (the "black sides" the user saw). An
  order-4 spherical-harmonic fit of the cap's irradiance replaced it; order 2 was tried first and was 6-8% off
  in the worst case, order 4 is within 2.9%. Benchmarks moved by at most 0.05 mag.
  [Photometry](../simulation/photometry.md)
- **2026-09-24** — Diffuse transmission for Kapton-backed arrays. The first cut used the front-only occluder
  mask, which was the ISS's largest occlusion error: a translucent lobe is lit from behind, so occluders on
  both sides count (self-test p95 0.26 to 0.17). [Satellite models](../modding/satellite-models.md)
- **2026-09-24** — The ISS forced per-component sampling: 16 samples per lobe gave its 16 blankets (one lobe)
  about one sample each, so any partial shadow was all or nothing. Lobes now get 16 per component, up to 64,
  and 64 occluders per type. Per-component joint pivots let one joint turn parts about several parallel axes
  (the four beta gimbals). [Satellite models](../modding/satellite-models.md)
- **2026-09-24** — The v1.1 roster was ported to 23 geometry models (~1.38M satellites) with six new Mallama
  benchmarks; Guowang is marked `"gated": false` as a known residual rather than hidden.
  [Results](../accuracy/results.md)
- **2026-09-24** — Highlight mode skipped lighting and drew every satellite at a fixed ~mag 5.4 dot, so
  highlighting a sunlit ISS made it fainter. It now lights normally and raises satellites to at least the
  highlight level. [Seeing a satellite](../simulation/visibility.md)
- **2026-09-24** — The Starmind AI1 model was rebuilt twice the same day to the project owner's reading of its
  spec sheet (nadir-pointing door bus, 20 m radiators, 70 m two-wing arrays on single trusses).
  [Satellite models](../modding/satellite-models.md)

## Orbits and reflectors

- **2026-03-22, 2026-06-09** — Sun-synchronous disks precess at the SSO rate with inclination from the J2
  formula. The RAAN is anchored at sim start from the Sun direction, which avoids the ~3 deg obliquity phase
  error that extrapolating from J2000 accumulated. [Orbits](../simulation/orbits.md)
- **2026-06-03** — Orbital mechanics moved to the GPU (`sat_orbit.comp`); the CPU update became O(1).
  [Orbits](../simulation/orbits.md)
- **2026-07-31** — Reflector targets came from a curated JSON of real solar installations instead of random
  points, with the observer spawn as a pinned entry. [Reflector targets](../modding/reflector-targets.md)
- **2026-08-06** — Reflector targeting rebuilt to be reversible. The old lock with acquire/release hysteresis
  and a frame-integrated slew depended on the sequence of frames, so playing forward and reversing to the same
  instant showed different pairings. Target identity is now chosen per fixed sim-time window, and orientation
  is a closed-form rate-limited ease from the previous window's aim. [Reflectors](../simulation/reflectors.md)
- **2026-08-06** — The same rework found the target-preference hash (`hash11` on a large float index)
  collapsed to 0 for every candidate past ~15 000 satellites. Reflect Orbital sits at index ~57 000, so every
  score tied and only the first eligible site ever won. Replaced by an all-integer `pairScore`.
  [Reflectors](../simulation/reflectors.md)
- **2026-08-06** — A first ease covered only one transition case with a fixed fraction of the window and read
  as satellites snapping to target; a real angular-rate cap now covers every transition. A single global
  window phase moved every mirror at the same instant (a synchronized wave), so each satellite gets a hashed
  phase offset. [Reflectors](../simulation/reflectors.md)
- **2026-09-22** — Lighting overhaul Phase 1: per-satellite traffic, not math, was the cost at millions of
  satellites (~304 B per satellite per frame, at the RTX 3070 Ti's memory wall). Per-type fields left the
  orbit record (112 to 64 B), the 80-byte `GpuSatInput` hand-off was deleted, and Phase 1b appended only
  visible satellites to a compact list drawn indirectly. `sat_flare.comp` stays a separate dispatch because
  its photometry needs the beam glow dome complete. [Orbits](../simulation/orbits.md)
- **2026-09-22** — Per-satellite buffers had reserved ~2.2 GB for 10M satellites regardless of roster; they
  are now sized to the loaded roster (~131 MB for 1.38M). [Building](../development/building.md)
- **2026-09-23** — The orbit phase `u0 + meanMot dt` was ~700 rad by day 7 of a rebake and off by a median 60 m,
  up to ~770 m along-track, in float: invisible as a point, fatal for a mesh beside its sprite. It is now an
  exact two-float product (Dekker), marked `precise` and deliberately without `fma()`. Median error 1.1 m.
  [Orbits](../simulation/orbits.md)
- **2026-09-24** — A model type had left the legacy `crossSectionM2`/`mirrorFrac` the beam code reads at 10 m^2
  and 0, so no Reflect beam was drawn from the roster's move to models until `bakeModelType` wrote them from
  the lobes. [Reflectors](../simulation/reflectors.md)
- **2026-09-24** — Ground-site aim got its double-precision CPU mirror (`satGroundSiteIdeal()`); until then a
  mirror mesh was posed facing straight down. [Reflectors](../simulation/reflectors.md)
- **2026-09-29 (review 7)** — Beam positions relative to last frame's eye were rebased by rotation only, so
  they lagged the observer's displacement (~2 km a frame flying fast at altitude) and ground spots swung.
  They are rebased rigidly. The CPU and shaders also disagreed on the eye radius by up to ~3 km over the
  Sierra; both use one eye now. [Reflectors](../simulation/reflectors.md)
- **2026-10-02** — Ground spots were traced to the sea-level sphere, so a beam arriving at 11-20 deg on a site
  1.7 km up lit the ground ~5 km past it. Spots intersect the target's own radius. Target kinds were added and
  solar parks drawn at `solar` targets. [Reflectors](../simulation/reflectors.md)
- **2026-10-03** — Sim time became real UTC: the Earth rotation angle is GMST, not `kOmegaEarth t` alone, which
  was self-consistent but put every place's sky ~5.3 h off. The start time and cloud phase moved by the same
  shift so the intro is unchanged. [Time and frames](../simulation/time-and-frames.md)

## Beams and clouds (Reflect Orbital)

- **2026-07-28** — A volumetric beam "tube" was deleted for performance and look; the pointing ray that had
  been a debug line became the production beam, which is why its flag is still called `showBeamDebugRays`.
  [Reflectors](../simulation/reflectors.md)
- **2026-08-09** — Per-beam cloud occlusion (`beam_self_march.comp`) replaced a per-target vertical column.
  The standing "never per satellite" rule had been about per-pixel cost multiplied by satellites and about
  winner-take-all flicker; a bounded march per beam with no arbitration has neither problem.
  [Reflectors](../simulation/reflectors.md)
- **2026-08-09** — The march first aimed at the target, which was right for locked beams and wrong for slewing
  ones; it marches the beam's real ray to the ground. A first per-pixel cloud glow used the camera's distance
  to the beam line and drew large rings; it was reverted outright for a per-cloud-sample light.
- **2026-08-10** — Anchorage worst case: beam rendering was 14 ms, 52% of the Medium frame, mostly a pointing-ray
  loop over every beam on every half-res texel (277M iterations). Forward+ tile culling cut it from 7.54 ms to
  1.08 ms; the same cull for cloud lights and a CPU hoist of the ground-spot maths followed.
  [Profiling](../development/profiling.md)
- **2026-08-11/12** — Cloud lights clustered per frame had emergent identity, so one satellite dropping out
  repartitioned everything (popping, re-aiming). A fade on top was reverted (still flickery, 20 FPS). Identity
  is now declared by (target, direction bucket) and state is eased in Earth-fixed doubles. Lesson: fix
  identity before smoothing. [Reflectors](../simulation/reflectors.md)
- **2026-09-28 (pass 10)** — Beams became light: a beam is the Sun's disk seen in the mirror, not a Gaussian
  cut at 4 sigma, which lit ~16x the area in a hard circle and crowded the tile cull. Summing beam lights and
  marching occlusion once along their mean took Anchorage frames from 350 ms to 3-4 ms.
  [Weather and beams in clouds](../rendering/clouds/weather.md)
- **2026-09-28 (pass 10b)** — Shafts became a ray-cylinder intersection; the closest-point chord turned with
  the camera when standing inside a beam. [The cloud march](../rendering/clouds/march.md)
- **2026-09-28 (pass 12)** — Only beams landing near their site feed cloud light; a departing mirror dragged
  the light out and it snapped back.

## Clouds

The first volumetric renderer ("v1", 2026-06-30 to 2026-09-27) lived in `sat_sky.frag`, then in a half-res
compute pass. Clouds v2 replaced it on 2026-09-27; v1 was deleted the same day.

- **2026-06-30** — The first shell march needed ~150 steps from sea level to be glitch-free; noise seams came
  from non-power-of-two Worley octave frequencies in the bake. [The cloud field](../rendering/clouds/field.md)
- **2026-07-12** — `raySphere` computed `c = |ro|^2 - r^2`, which cancels catastrophically at Earth scale on
  grazing rays (every horizon); rewritten as `(|ro| - r)(|ro| + r)`. Every shell march classifies the eye as
  below, inside or above the shell; one that assumed "below" broke as soon as the eye flew up.
  [GPU conventions](../development/gpu-conventions.md)
- **2026-07-17** — The cloud march moved to a half-resolution compute pass returning an affine (A, B) pair so
  cirrus and cloud composite exactly. Its terrain-bleed gate first used an opacity-gated distance and drew
  thin cloud through mountains. [The cloud march](../rendering/clouds/march.md)
- **2026-07-21** — The domain warp was baked; interpolating it linearly drew tessellating triangles, fixed with
  a C1 manual blend. Later the warp was found to fold the noise: its shear must stay below 1.
- **2026-08-04** — Clouds read near-black or red at long range because the volumetric march had no airlight;
  a camera-path in-scatter integral was added. Shared hardcoded `optDepth` between shaders was tried and
  reverted: the shared function took the loop count as a parameter, the unroll was lost, and it measured
  slower. Lesson: sharing declarations is free, sharing bodies is not. [GPU conventions](../development/gpu-conventions.md)
- **2026-07 (v1)** — An RGBA16F alpha held `tEnterCombined`; every near-horizon entry overflowed 65 504 to
  +inf and suppressed the whole composite on rays that hit the sea sphere. This is why distances never go in
  half floats and the scene depth is R32F. [GPU conventions](../development/gpu-conventions.md)
- **2026-09-27** — Clouds v2: one field (`cv2Field`) for the view march, light march, ground shadow and beam
  occlusion, in metric coordinates anchored at the observer's sea-level point. v1 had a separate copy of its
  field per consumer. [The cloud field](../rendering/clouds/field.md)
- **2026-09-27** — The map's drift was rate x time since J2000 (~7500 rad), so any rate change threw it to an
  unrelated longitude, and the intro forced its own rate into `settings.json`. The phase is now measured from
  2036-06-21 plus a session offset, which the intro sets instead.
- **2026-09-27** — Coverage used the map's brightness as a fraction and never let anywhere close over; it is
  remapped (clear below 0.12, overcast above 0.6).
- **2026-09-27** — The shape became a 3D margin, not a heightfield. Extruding a 2D presence under a height made
  every cloud a flat-bottomed, vertical-walled "cola can"; noise must act in the units of the 2D strength to
  move a wall. [The cloud field](../rendering/clouds/field.md)
- **2026-09-28** — The source map is a JPEG: anything gated on `cov > 0` drew its 8x8 blocks as rectangles.
  Rule: scale new layers by coverage, never gate them on it.
- **2026-09-28** — The baked Perlin fBm is 0.50 +- 0.057; the first high-layer thresholds (0.5-0.7) drew nothing.
  Thresholds must be set against the measured spread.
- **2026-09-28** — Erosion faded toward none with distance and then switched to the mean, drawing a ring
  ~220 km out from orbit; it fades toward the mean at full strength.
- **2026-09-28** — Bases became terrain-relative (the weather cube's alpha holds the smoothed DEM); before, the
  Andes and Tibet stood inside the cloud.
- **2026-09-28** — Motion: raising the history weight on total pixel motion swapped the image for the quarter
  grid's noise during a pan; the weight follows parallax instead. Sparse jitter stepped per frame gave each
  pixel of a 2x2 block its own bias (squares); jitter is per visit. [Temporal resolve](../rendering/clouds/temporal.md)
- **2026-09-28** — Full rate above 30 km: from orbit the sparse march's history is lost to motion and the
  fallback was a 1/8-resolution upsample. [Temporal resolve](../rendering/clouds/temporal.md)
- **2026-09-28** — Target A's alpha became a signed distance in kilometres; in metres an RGBA16F alpha was +inf
  past 65.5 km and clouds dimmed every satellite in front of them from orbit.
- **2026-09-28 (pass 10b)** — Sky light through cloud uses two-stream diffuse transmission; `exp(-sigma 250 m)`
  left the lower half of every large cloud one flat dark grey.
- **2026-09-28 (pass 13)** — Never read a displacement from a hardware-filtered 8-bit texture: the flow read at
  ~375 km texels stepped every ~1.5 km and shifted clouds by hundreds of metres per step (straight seams).
  The flow is analytic. The same 8-bit sub-texel limit hit the DEM. [The cloud field](../rendering/clouds/field.md)
- **2026-09-28 (pass 14)** — The weather-lookup warp included the cell rim field, a near step, so lookups jumped
  across every rim and storms extruded the folds into walls. Weather and coarse cell reads are C1-smooth.
- **2026-09-28 (pass 15)** — Cumulonimbus became three layers: a margin field thresholded per height cannot
  narrow and flare again, so the old Cb was a mountain sloping into a floating anvil. Cloud airlight counted
  extinction twice (inherited from v1), turning clouds past the terminator into dark blue patches.
- **2026-09-28 (pass 16-17)** — The tower lattice went from 3D (cut big towers on cell faces) to 2D on an
  equal-angle cube map, and the cell grew to cover a whole tower's reach. Sinking heads, a max() join of towers
  and anvil, and a combined light-march cutoff were each tried and reverted as slower.
- **2026-09-29 (pass 18)** — The "waffle" on tower walls was the tower strength read from a coarse mip with
  8-bit filtering weights; read in float bilinear. A radius floored at 0.7 left a flat crescent wall where the
  strength crossed its cutoff; weak towers are shorter, not thinner (pass 19).
- **2026-09-29 (perf sprint)** — Code size and NVIDIA's register allocation govern the march: every call site
  inlines the whole field (~60 KB). Merging call sites took the shader from 459 KB to 220 KB, and storm views
  got 62-67% cheaper. The compiler gives either 128 registers or ~227, and 227 is 20-60% slower; trivial edits
  flip it. Variants are A/B'd in one live app after checking the register count.
  [The cloud march](../rendering/clouds/march.md)
- **2026-09-29** — Tried and dropped in that sprint: per-tile tower lists, a shared-memory lattice cache,
  caching coarse reads in globals, reusing the far sample every other step, one call site for view and light
  (234 registers, +60%), skipping empty shell above a column ceiling (0%), a footprint-based step cap (-0.4%).
- **2026-09-29** — Adaptive rate: skipping pixels inside a full-rate dispatch saves nothing (a warp costs its
  slowest lane), so moving views march sparse plus full-rate tiles from a separate classify pass. The vote was
  first inside the march and flipped it to ~224 registers (+19%). Storm views panning 22.5 to ~8-10 ms.
  [Temporal resolve](../rendering/clouds/temporal.md)
- **2026-09-29** — Fog and dust replaced v1's global noise haze; they are not part of `cv2Field`, because any
  addition to the main loop, even an unused struct member, flipped the register count.
  [Weather, rain, lightning, fog](../rendering/clouds/weather.md)
- **2026-09-29** — Rain drops moved into the world (a lattice fixed to the ground); streaks in direction space
  travelled with the eye. Wind drift is the integral of the gusting wind: wind(t) x t made the rain stop and
  rise near t = 600 s. [Weather](../rendering/clouds/weather.md)
- **2026-09-29** — Lightning flashes are drawn after the resolve because the history (weight 0.05, sparse) would
  swallow a flash lasting a few frames. A halo around bolts was dropped: rain curtains drew their banding over
  it. [Weather](../rendering/clouds/weather.md)
- **2026-09-29** — God rays (experimental, off): subtracting shadowed airlight overshot to black, and scaling
  transmittance on surfaces darkened the already-shadowed sea; the remaining effect is subtle.
- **2026-09-29 (review 4-5)** — Storms became clusters of towers (one dominant tower with a flanking line); an
  even forest of mushroom-capped columns read as smokestacks. The user's hand-tuned anvil became the defaults.
- **2026-09-29 (review 7-8)** — Air in front of clouds comes from the sky pass: the march's own 8-step airlight
  read far darker at a grazing Sun, and a distance split between the two disagreed at edges (dark specks).
  History weight in motion is capped at 0.5; at 1.0 motion showed raw rays.
- **2026-09-30 (review 12-13)** — The resolved depth got its own history (the 4-frame sparse cycle made the air
  split flicker from orbit, 2.6% to 0.13% of pixels), and opaque texels store the mean distance (a class flip
  between opaque and translucent moved the split tens of km: hard blocks on the horizon).
- **2026-09-30 (review 15)** — The depth history was gated on parallax only, so a pitch blended sky and cloud
  distances into horizon streaks; it is gated on the whole shift. The harness's recordings had rendered ~12
  frames per recorded frame, which is why no earlier motion test saw it. `tstab.py` measures it now.
  [Temporal resolve](../rendering/clouds/temporal.md)
- **2026-09-30 (review 16)** — The light march shared the view march's jitter, so sample depth and light-step
  positions moved together and never averaged out (rings on Cb lids). A variance-adaptive history weight was
  measured within noise and reverted.
- **2026-10-01 (review 17)** — Sunset "blocky sea reflections" were the cloud shadow: the dense shadow stretch
  ignored Earth's curvature. A low Sun's long shadow path also made the map's JPEG blocks binary.
- **2026-10-01 (review 17-18)** — Cloud morphology from real MODIS scenes (closed and open cells, streets,
  popcorn cumulus) and a fractal benchmark against them (`tools/cloud_stats.py`). The remaining gap is 1-5 km
  structure below the half-res pixel from orbit; a procedural set was made first.
- **2026-10-01 (review 18)** — The far cloud layer (`cloud_v2_far.comp`) replaced the march from high orbit:
  8000 km cloud bucket 9.3 to 1.4 ms, still-view flicker 1.0% to 0.04% of pixels. A plain attenuator for the
  mid/high layers drew grey camouflage; they use the adding method. [The far layer](../rendering/clouds/temporal.md)
- **2026-10-01 (review 19)** — One placement at every distance: near and far fields were two placements switched
  by pixel footprint, and climbing over one place replaced the clouds between ~35 and 450 km (correlation ~0).
  The imagery alone everywhere was tried first and turned cumulus masses into scattered puffs.
  [The cloud field](../rendering/clouds/field.md)
- **2026-10-01 (review 20)** — The resolve's mean-colour clamp divides by opacity, so beam shaft light at ~0
  opacity was amplified 50x into blue-white blocks; history is clamped in plain radiance too.
- **2026-10-02 (review 22)** — Concentric rings round the nadir came from the coarse lattice sampling only half
  its phases; the start jitter spans the whole coarse interval. A per-interval sample position or a second
  blue-noise read flipped the march to 222 registers. Foveated full rate made strafing worse and stays off.
- **2026-10-02 (review 22b)** — The Sun's glow through a storm was the clouds' own forward scattering; a
  Sun-path optical-depth profile from the lightning pass shades clouds near the eye's line to the Sun. The
  first cut overwrote the profile's end after a full march and darkened everything near the Sun line.
- **2026-10-02 (review 25)** — That darkening was applied only for samples in front of the eye, drawing a
  straight line across the sky 90 deg from the Sun; behind the eye the distance is to the eye.

## Terrain

- **2026-06-23** — The elevation PNG is not ETOPO1: ocean is 15/255, not 0. Forgetting the offset made every
  coastline a ~530 m cliff, and the bug was reintroduced across several sessions before it was written down
  as an invariant. [Terrain](../rendering/terrain.md)
- **2026-07-17** — The terrain march's step count formula was `mix(320, 320, ...)` (a no-op), then made
  path-length adaptive: grazing and steep rays had the same budget, a cause of horizon jitter. Profiling
  showed terrain steps and aurora, not `optDepth`, were the dominant costs. [Terrain](../rendering/terrain.md)
- **2026-09-25** — Procedural 3D detail as a real surface (`H = DEM + D`) shared by every pass. An earlier
  `erosion` branch displaced the hit point and perturbed normals; its own notes said a real surface was the
  goal, so it was studied and not merged. [Terrain](../rendering/terrain.md)
- **2026-09-25** — Never form an absolute ECEF coordinate in float: the lattice is anchored at the observer as an
  integer cell plus offset. The erosion branch had fought exactly that precision loss (swimming noise).
- **2026-09-25** — A full-res march from the eye, then an envelope depth, cost 10-25 ms on the ground; the
  quarter-to-half-to-full seed pyramid made depth 30-50% cheaper. Empty-space skipping with a DEM max-mip was
  slower at every altitude (3.3 to 4.5 ms) because the costly rays graze within the bound for tens of km.
- **2026-09-25** — Lighting of faces turned from the Sun: gating `dayFrac` by the shading normal sent faces past
  edge-on to the night branch (no skylight, city lights in daylight). DEM normals rarely did; detail normals
  constantly did. Found by flying the harness into a glacier.
- **2026-09-25** — The DEM is sampled exactly near the observer: the float UV and 8-bit bilinear weights built
  metre-high shelves on a steep wall seen from 10 m. Out of march budget is not a miss (holes through hills).
- **2026-09-25** — Erosion octaves joined the marched surface. A first cut inside the octave loop, with several
  call sites, slowed every terrain pixel ~50% with the erosion switched off (GLSL inlines everything; registers
  size for the largest path). One call site, `[[dont_unroll]]` loops and a linearised plane in refinement fixed
  it. A per-cell cull was slower (divergence). [Terrain](../rendering/terrain.md)
- **2026-09-25** — The sea is not terrain: a "hit" on the sea sphere was step-size luck and drew rings of
  flat land over open water. Measured at 15.3% of the frame at 60 m AGL, 0% after the gate.
- **2026-09-29** — Steps past the exit land on it: over low land a last step jumped past the sea sphere, a miss
  drawn as flat land in rings about the nadir (found with the harness `probe`).
- **2026-09-29 (terrain v2)** — The night map's blue base out-shone full-moon ground; the terrain takes only the
  lights and is lit by the Moon and the night sky. A knee on the filtered value drew texels as squares.
- **2026-09-29** — Lakes at their own level: water had been drawn only below 160 m and at sea level, so
  Superior, Victoria, Titicaca and Baikal were blue land. Fetching the water map on every height evaluation
  cost +1.7 ms in the Alps; it is fetched only near water. The source mask's last row made a 3600 m "lake"
  round the South Pole. [Terrain](../rendering/terrain.md)
- **2026-09-29** — Elevation PNGs are no longer decoded at runtime; raw `.r8` files are read instead (decoding
  the large PNGs was implicated in machine freezes, documented separately).
- **2026-09-29** — The biplanar material weights have a floor: near the cube diagonal both weights were ~0 and
  the normalisation returned black blobs (central Japan).

## The sea

- **2026-06-25** — First wave normals on sea-level hits, gated by the specular map, with a Blinn-Phong glint.
  [The sea](../rendering/sea.md)
- **2026-09-26** — The ocean glint of satellite flares got its own gain and floor (it shared the satellite's
  brightness). The same change found hover/drag arrays sized 33 while the table's index reached 34, so the
  near-glare rows wrote into the next array. [The sea](../rendering/sea.md)
- **2026-09-29 (review 9)** — Waves read the observer's float lat/lon times R, wrapped to the first octave only,
  so other octaves slid against the terrain; they ride the double world offset.
- **2026-09-30 (review 10a)** — The unbounded float offset broke the texture after long flights (arguments ~12x
  the offset lost their fraction). The wave field is now exactly periodic and the offset wrapped into one
  period. The sky-reflection march used uniform samples over a ~1000 km grazing path and tinted the reflected
  horizon tan. The satellite-glint loop cost 6 ms from orbit when its result was zero; it is skipped.
- **2026-09-30 (review 14)** — A rough sea under a low Sun aliased into crawling glitter; normals are filtered to
  the footprint along the view and the removed slope becomes reflection roughness.
- **2026-10-01** — A light-blue disc under the observer at dusk: the reflection march was skipped where Fresnel
  is ~0 while foam took the fallback as sky light. Open ocean decoded as a "lake" at 2e-5 m.
- **2026-10-02 (review 22-23)** — The Phong glint narrowed as waves were filtered and lost the glitter path; it
  is a Beckmann lobe with Cox & Munk slopes. Foam took the mirrored sky ("mercury"); it is a diffuser. The sea's
  body kept noon brightness to the terminator. A diagonal-only warp read as a square grid.
- **2026-10-02 (review 24-28)** — The far band past the wave octaves was a mirror; a far-sea ripple was added,
  then smoothed (blocks at 3 footprints), then made one pattern per level (rings in motion). A hard line at
  the wave range was the lost-slope roughness dropping to 0 in a pixel.

## Cities, farms and solar parks

- **2026-07-15** — A tiled city detail texture multiplied the night map. [Cities](../rendering/cities.md)
- **2026-09-29** — Procedural city lights, phase 1: street grids and Natural Earth major roads. Glowing street
  lines were replaced the same day by lamp posts at the user's request, and one grid per region of districts
  let streets cross district borders. [Cities](../rendering/cities.md)
- **2026-09-29** — Day side from the same layout. Equal lots, centred roofs, grey yards and no trees read as a
  board game; a density-only chance of big buildings made half of Los Angeles warehouses.
- **2026-09-29** — Night v2: averaging an unresolved light over its pixel is right in energy but wrong on
  screen, since a real light saturates its pixel and distant cities glitter. Glitter points replaced lines;
  v3 extended them to every distance on purpose, changing orbit views.
- **2026-09-29** — City light sprites for the far horizon. Up close they read as floating lanterns (review 2), so
  they are far only. A trail-pass flag keyed on > 0.5 hid every sprite at the Medium preset (render scale 0.85).
- **2026-09-29 (review 2)** — Farm lights removed entirely ("everybody is not blasting floodlights"). Coloured
  dots seen from orbit were residue of subtracting an uneven blue base, amplified by sparse glitter. A round
  glitter point of the stretched footprint smeared at grazing angles; points are anisotropic.
- **2026-09-29 (review 8)** — Farms became regional rectangles (grid, strips, paddies); a stretched Voronoi
  patchwork had been the first version. [Cities](../rendering/cities.md)
- **2026-09-30** — Regional city styles. A per-channel day ratio turned neutral asphalt magenta on the greyed
  base; the ratio is taken against luminance. Lights end at a terrain contour.
- **2026-09-30 (review 14)** — Blurring the map the pattern reads (for cloud in front) spread street grids into
  the desert; the blur applies to the finished light.
- **2026-10-02/03** — Solar PV parks at `solar` reflector targets, and rooftop PV where a park overlaps a city.

## Sky, atmosphere and aurora

- **2026-07-12** — Airglow: green and sodium accumulate in the main atmosphere loop; red at 275 km needed its own
  march, since extending the main loop would coarsen near-surface sampling. [Aurora and airglow](../rendering/aurora-airglow.md)
- **2026-07-13** — Light pollution became a 16-sector dome. Hard binning showed blocks over bright regions; the
  sectors are interpolated and blurred. Clamping before the elevation falloff capped the effective maximum.
  [Atmosphere and sky](../rendering/atmosphere-and-sky.md)
- **2026-07-16** — Aurora closed after 22 follow-up rounds of tuning. **2026-07-17**: its curtain noise was baked
  to a texture and the march moved to half resolution; it had cost a ~40 FPS swing.
- **2026-07-30** — The isotropic floor in the dome: `elevFalloff` alone floored at 0.26 at zenith, so no gain
  could dim an overhead target more than ~26%, which is why the Milky Way survived near cities.
- **2026-08-09** — Extinction went to zero near the horizon once the observer left the atmosphere; a tangent
  altitude gate was added, then replaced on 2026-09-23 by line-of-sight Chapman extinction (the old fit kept 95%
  of sea-level extinction on a 4 km mountain, really ~35%). [Seeing a satellite](../simulation/visibility.md)
- **2026-08-09** — Aurora seen from space cut off at ~40 km near the horizon: the far leg past the inner-shell dip
  was discarded; a second segment marches it.
- **2026-09-29** — The atmosphere loop starts at the atmosphere entry: from orbit nearly every step sampled vacuum
  while the air was undersampled. Sky pass 2000 km 16.5 to 4.9 ms. The same measurement showed a far cloud
  impostor was not worth building at that time.
- **2026-09-29 (review 7)** — Air in Earth's shadow had no light (single scattering), so clouds under a just-set
  Sun were a near-black band; shadowed air gets half the zenith sky's radiance.
- **2026-09-30** — Sky TAA. Disocclusion tested against one history texel rejected every silhouette pixel every
  frame (no anti-aliasing); it tests the range of the 2x2. The validation layer found the main pass's three
  variants had different dependencies, making load and boot frames invalid. [Atmosphere and sky](../rendering/atmosphere-and-sky.md)
- **2026-09-30** — Aurora sheets, then (review 11) integrated per step: crediting a whole Gaussian to a crossing
  step drew stacked zigzags where rays grazed a fold. The aurora's light on the ground was removed (user: green
  and bad).
- **2026-09-30 (review 10e)** — Measured against Artemis II photographs, sunlit cloud is ~10x the sea; the sim
  showed ~2.5x. The cause was exposure and tonemap, not scattering gains (which changed nothing at 70 000 km).
  Auto exposure spot-meters the lit Earth, and an orbit grade applies on the sunlit side only (review 11).

## Moon and eclipses

- **2026-04-08** — Moon on a circular orbit with a hand-calibrated phase. [Moon and eclipses](../rendering/moon-and-eclipses.md)
- **2026-09-30** — The Moon as a body: true position, size, orientation, tidally locked (the face had pointed at
  the observer). The disc had been a fixed 3x at infinity.
- **2026-09-30 (review 16)** — Moonlight was the lit fraction, linear, so a crescent lit clouds at a third of full;
  it follows the phase law. At 2.5x size the Moon swallowed the Sun long before contact, so near the Sun it eases
  to its true size. The eye is capped at 100 000 km (boost reached the Moon).
- **2026-10-03** — Meeus's lunar series replaced a two-body fit that put eclipses hours late. At the real
  2037-07-13 totality the sky stayed bright: the land's direct sun, sky light, clouds, fog and bloom did not all
  take the eclipse factor. Every sunlit term does now. Eclipse search had required 0.55 deg separation and
  skipped that eclipse. [Sun, Moon and planets](../simulation/sun-moon-planets.md)

## Points, bloom, glare and meshes

- **2026-06-22** — Satellite sprites got a minimum size against sub-pixel aliasing flicker.
  [Points, bloom and glare](../rendering/points-bloom-glare.md)
- **2026-07-29** — The per-pixel loop over a capped list of flare entries (8, then 64, 128, 256, still flickering)
  was deleted for a render-to-texture bloom. Satellites lost lens-flare ghosts; the Sun kept them.
- **2026-07-30** — Planets added on the star pipeline, not the satellite pipeline; a phase-angle sign error was
  caught only by checking against a known real configuration. [Sun, Moon and planets](../simulation/sun-moon-planets.md)
- **2026-09-23** — Unified scene depth (Phase 4a): a 150 km cap had kept the distant Earth from swallowing
  satellites from orbit, but meant mountains or clouds beyond 150 km never hid anything and the flare source
  killed the bloom of satellites in front of the Earth. [Points, bloom and glare](../rendering/points-bloom-glare.md)
- **2026-09-23** — One magnitude-to-sprite model for satellites, stars and planets: a mag-3 satellite had drawn
  about as bright as Jupiter and a mag-5 satellite ~13x a mag-5 star.
- **2026-09-23** — The model viewer came up all black: Clay draws an element's own background over its custom
  content. [Satellite meshes](../rendering/satellite-meshes.md)
- **2026-09-23** — Mesh/sprite hand-off decided on the CPU in double, the same frame: letting the GPU fade a sprite
  when it nominated a mesh drawn a frame later left a "flare, nothing, flare" gap.
- **2026-09-24** — The bloom's six-spoke then 16-direction streak drew a dotted starfish; it became round, with
  the sharp part moved to glare sprites. A hard per-channel bloom ceiling flattened overlaps into plateaus.
- **2026-09-24** — Mesh bloom seeded from over-white pixels exploded edge-on to the Sun; it seeds from
  photometric light. Overflow in the flare target made a mirror's glint position NaN.
- **2026-09-24** — Every edge of a close-up satellite glared and filled the glint slots with ~400 px sprites; only
  Sun-like surface brightness may glare. A diffuse-limit gate missed Sun-facing rough glass.
- **2026-09-24** — A depth pre-pass for meshes: the open-lattice discard had disabled early-Z, so every overlapping
  layer of a station ran full shading.
- **2026-09-25** — Only 64 meshes, picked in random GPU append order, made neighbours in the AI ring flicker; 256,
  largest first, with an adaptive fade-in size. Environment probes keyed by position were outrun every frame at
  5 min/s; probes belong to a satellite now.
- **2026-09-26** — Glare scales with proximity; the glare defaults became the distance-tuned release values.
- **2026-10-02 (review 28)** — A meshed satellite's flare sprite sits on the Sun's image in its dominant lobe, so it
  no longer jumps when the mesh's own glints take over.

## Performance and hardware

- **2026-07-13, 2026-07-17** — View step limiting, then render scale and GPU timestamp profiling replaced guesses
  with measurement. [Profiling](../development/profiling.md)
- **2026-07-28** — `GpuCloudParams` drifted from its GLSL mirror by a permutation, not a size change:
  `flatSunGainScale` read a pad (black clouds) while coverage read 4.0, and the size `static_assert` passed.
  `tools/check_cloud_params.py` was written to catch it. [GPU conventions](../development/gpu-conventions.md)
- **2026-08-10** — Knockout bits were added for blocks that had none and no preset reach; an automated sweep reads
  raw timings (the HUD's smoothed values take ~40 frames to settle) with time paused.
- **2026-08-10** — CPU frame buckets: once Medium reached 15.5 ms GPU, a ~2.8 ms remainder was identical at
  Planetarium. Fixed cost that does not scale with rendering is what the CPU buckets expose.
- **2026-09-07** — A 2015 MacBook Pro (R9 M370X, MoltenVK) spent ~490 ms a frame in `sat_sky.frag`: compiled size
  and register pressure, untouchable by sliders or knockouts. Potato (a separate small shader) and SKY_LITE (the
  same shader with cuts) followed. [Weak-hardware tiers](../rendering/hardware-tiers.md)
- **2026-09-07** — Push constants trimmed to the 128-byte guaranteed minimum; the app could not create its pipeline
  layouts on that hardware. Per-frame fields moved to the CloudParams UBO. The device gate had read 144 and rejected
  exactly the hardware the trim was for. [GPU conventions](../development/gpu-conventions.md)
- **2026-10-02** — The driver's capped Vulkan pipeline cache had filled and stopped storing; every launch recompiled
  changed shaders (30-40 s to start). The app keeps its own cache: 4.6 s on a warm cache.
  [Code architecture](../development/architecture.md)

## Sound

- **2026-09-25** — Ambient sound: layers are ramps, never switches. The first fades eased exponentially and never
  finished (a jump to orbit dragged crickets up with it); fades are linear slews. [Ambience](../sound/ambience.md)
- **2026-09-25** — The first wind was a sub-bass wall and its gusts measured flat; every synth is high-passed and
  gusts swing in dB. FSK beeps and disk clicks read as glitches and were replaced by drones in one key.
- **2026-09-26** — The ground wind bed follows the cloud map; it had been the loudest layer everywhere. A jungle
  recording carried people talking and was re-sourced.
- **2026-09-27** — The beam sound's first cut (a saw stack on beams near the line of sight) read as an FPV drone
  and sounded the same everywhere; it became a screen-space glare chorus plus a site pedal.
- **2026-09-27** — Tuning a synth to the soundtrack in real time never fit its rhythm and harmony, so the composer
  wrote an upwell stem per track, played sample-synced. [Music and tonality](../sound/music.md)
- **2026-09-27** — A `wind_rush` layer on airspeed swamped everything while moving and was removed at the user's
  request.
- **2026-10-02** — The music analysis cache was keyed on write time, which changed with every build's copy; it is
  keyed on a content hash.

## UI and controls

- **2026-03-27** — UIRenderer batching: one batch broke once the UI grew. [UI (Clay)](../development/ui.md)
- **2026-08-01** — The intro: `camera.azDeg` was never set during playback (only `obsFacing`), so the view never
  panned and snapped at the end. `timeScaleIdx` defaulted to 10x. Per-segment smoothstep stopped the camera at
  every beat; the path is a Catmull-Rom spline. "Any key" skipped the intro before anyone saw it.
  [Controls](../using/controls.md)
- **2026-08-02** — The gamepad virtual cursor captured the pointer under itself and blackholed every button; any
  floating element at the pointer must pass pointer events through. [UI (Clay)](../development/ui.md)
- **2026-09-24** — The selection panel's click rects were size estimates; a click outside the estimate fell
  through to picking and deselected before the button ran. They use laid-out bounds.
- **2026-09-29 (review 6)** — WASD moved ~510 km/s at any height and crossed a cloud in a frame; speed scales with
  height above ground.
- **2026-10-03** — HQ export "froze and crashed": it was a ~20-minute export with no feedback; a progress frame
  and Esc-to-stop were added. Follow-shot keys kept only the offset, so framing was lost on playback.
- **2026-10-03** — The intro's controls beat and the post-intro hint were replaced by a first-run tutorial. Clip
  regions now nest; a nested clip's end had reset to the full screen.

## Tooling and harness

- **2026-09-25** — The automation harness replaced the rule "never launch the app": scripted, muted runs in their
  own user-data folder return captures and timings. [Automation harness](../development/harness.md)
- **2026-09-25** — `observer agl=` used the CPU's 18 km/px DEM, so a golden view at 30 m stood 758 m over ground
  really at 342 m; it reads the GPU's ground back.
- **2026-09-25** — Launches are spaced 30 s apart after back-to-back relaunches were suspected in machine freezes.
- **2026-09-28 (pass 13)** — Snapshots carry the view and settings: lat/lon and time alone framed other clouds,
  since the drift phase is session state.
- **2026-09-30 (review 15)** — `path play record=` waited on the PNG encode and rendered ~12 frames per recorded
  frame; it renders one. [Automation harness](../development/harness.md)

## Build and release

- **2026-04-18** — v1.0. **2026-08-15** — v1.1.0. [Building](../development/building.md)
- **2026-09-08** — A hardcoded Visual Studio generator broke the v1.1.1 tag build when GitHub's runner image moved
  toward VS2026; CI uses Ninja with whatever MSVC the runner ships. MSVC accepted a narrowing conversion that
  GCC and Clang rejected, so C4838 is an error locally and CI runs on PRs to main.
- **2026-09-08** — Machine paths had reached the public history through `.vscode/settings.json`; committed config
  must stay path-free. They had worked around CMake 4 rejecting old `cmake_minimum_required` in dependencies,
  fixed properly with `CMAKE_POLICY_VERSION_MINIMUM`.
- **2026-09-08** — macOS ships universal with a fixed deployment target: arm64-only builds failed with "bad CPU
  type" on Intel Macs, and the build machine's OS version refused Monterey.
- **2026-09-25** — Runtime files re-sync whenever they change: POST_BUILD copies ran only on relink, so edited
  JSON or icons silently "did not take".
- **2026-09-27** — Screenshots live behind a link to the source tree; `release.bat` deleted the build tree and the
  user's screenshots with it.
