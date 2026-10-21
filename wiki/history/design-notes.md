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

- **2026-10-15** — The roster's random numbers moved from the global CRT `rand()` stream to one `SatRng` stream per constellation (an emulation of MSVC's LCG, its start state recorded on the first build); before, changing an earlier constellation reshuffled every later one, and Linux/macOS drew a different sky than Windows. The new stream reproduces the old Windows layout bit for bit.
- **2026-10-19** — Model files gained the `profile` (body of revolution) and `fin` primitives and the `hex_tiles` pattern for the launch-vehicle rework; until then rockets were cylinders and straight cones with flat-plane flaps, five flat black heat-shield panels and one cone standing for all the engines.
- **2026-10-19** — Components gained `stow` and models `deployment` (unfolding after release); before, train members were drawn in the open-book pose from the moment they left the dispenser, so a Starmind's 70 m wings overlapped the ship.
- **2026-10-20** — Station art pass (issue 20261019_225545): each future station had been 1-5 plain cylinders with two floating rigid wings on one cross-track axis, sized to the v1.1 legacy cross-sections. Generated layouts moved mean m1000: Haven-1 2.89 -> 2.98, Haven-2 1.41 -> 0.78, Axiom 1.46 -> 1.27, Orbital Reef 1.53 -> 0.34, ROS 2.25 -> 1.44, BAS 2.24 -> 1.52, Starlab 1.68 -> 2.39; ISS and Tiangong unchanged. A whole domed hull as one profile failed the occlusion self-test (Orbital Reef p95 0.30 vs the 0.25 gate), hence one profile per meridian segment.
- **2026-10-20** — Issue 20261020_214656: the ISS's TRRJ radiators were `edge_on` and flipped 178.7 deg between two 20-s samples; `edge_on_continuous` turns them 1.3 deg per 20 s (all stations <= 2.3 except two track joints at 7 deg where the Sun passes near their axis).
- **2026-10-20** — The default sea-level zenith extinction became 0.18 mag (it was a tuned 0.079), part of baking the author's tuned settings into the compiled defaults.
- **2026-10-20** — Components gained `fold` and profiles `sweep_deg` with the payload stacks (issue 20261019_223922: Starmind's 10 m bus did not fit Starship's 8 m bay; it carried 40, now 8 with the bus folded in half).
- **2026-10-20** — Station art pass (issue 20261019_225545): the seven future stations are generated by `tools/make_station_models.py`; until then each was 1-5 plain cylinders with two floating rigid wings on one cross-track axis, sized to the v1.1 legacy cross-sections. Mean m1000 moved: Haven-2 1.41 -> 0.78, Orbital Reef 1.53 -> 0.34, ROS 2.25 -> 1.44, BAS 2.24 -> 1.52, Starlab 1.68 -> 2.39; Axiom and Haven-1 little; ISS and Tiangong unchanged.
- **2026-10-20** — Joint mode `edge_on_continuous` (issue 20261020_214656): the ISS's TRRJ radiators were `edge_on` and flipped 178.7 deg between two 20-s samples; now about 1.3 deg per 20 s.
- **2026-10-20** — Station models carry `docking_ports` and no longer draw visiting crew vehicles as render-only greebles; the sim flies real crew (and from 2026-10-21 cargo) vehicles to them. Ports gained `use` (crew / cargo) on 2026-10-21.
- **2026-10-20** — GEO belt: the `List` distribution and `category` field (issue 20261020_190331). The comsat models bake to 112 exact lobes; at that count the ~200 GEO satellites above any horizon cost ~0.3 ms of orbit dispatch, so `geo_*` models bake at the 48-lobe budget (validator p95 < 0.01 mag).
- **2026-10-20** — Starship's ship model got an open aft skirt with recessed bells (issue 20261020_181510): the barrel had ended in a cap at the track point with the bells half above it, so glowing bell interiors read as crescents.
- **2026-10-21** — Components and docking ports gained `since` (stations built module by module); before, stations appeared whole on their first module's date.
- **2026-10-21** — OneWeb and Amazon Leo still have no `stack`: with the models' stowed sizes (about twice the real satellites) no column fits their fairings (OneWeb misses Falcon 9 by 0.08 m at best).

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
- **2026-10-04** — A mirror's beam ignored the Earth's shadow: mirrors past the terminator kept beaming at full
  strength (~400 beams on an Antarctic polar night in a user snapshot). The beam takes the satellite's shadow
  factor. [Reflectors](../simulation/reflectors.md)
- **2026-10-06** — The Starmind orbital data centers fly the shells of SpaceX's FCC letter (two roster entries,
  "Shells" distribution, rings of 8). They replaced one 1M-satellite dawn-dusk disk that predated the filing. The
  first cut flew lines of 10, 1 km apart; with tight nodes, shells 1.6-1.8 km apart slid through each other
  (~13,000 cross-cluster pairs within 1 km), so the X-ring's planes spread over +-10 deg (26 pairs).
  [Orbits](../simulation/orbits.md)
- **2026-10-06** — Starmind's ring formations fly Clohessy-Wiltshire relative orbits about their cluster
  centres, 350 m in radius, with a per-cluster scatter. The first cut drew each ring as a rigid 2 km polygon in the
  orbit plane, held together by giving every member its centre's mean motion at its own radius; at 2 km each
  cluster resolved into a small ring of points from the ground, and the user asked for the real dynamics,
  sub-750 m formations and irregularity so the constellation stops drawing perfect lattices.
  [Orbits](../simulation/orbits.md#formation-clusters)

- **2026-10-15** — The orbit epoch rebake moved from a CPU loop in double plus a full synchronous upload (~27 ms at 1.37M satellites, every other frame at 1 yr/s: a 28.6 ms frame against a 12.5 ms GPU) to `sat_rebake.comp` in exact fixed-point turns (16.7 ms, vsync). GPU vs CPU mirror within ~2.4 m of range at 1995, 2037 and 2061.
- **2026-10-15** — `buildOrbits()` drew from the global CRT `rand()` stream, which reshuffled every later constellation whenever an earlier one changed, and Linux/macOS drew a different sky from Windows. Per-constellation `SatRng` streams reproduce the Windows layout bit for bit. Runtime roster rebuild added (harness `roster`).
- **2026-10-19** — The disabled legacy-lighting Starlink Gen2 comparison shell (1584 satellites) was removed from the shipped roster (the user: "should be removed").
- **2026-10-20** — Issue 20261020_190331 asked for real GEO satellites: the List distribution and the 398-satellite GEO belt. The Show / Highlight masks were two 32-bit push-constant words: any entry past the 32nd (and Kessler / launch records, whose constellation index was masked to 5 bits) silently read another entry's bit. They moved to 256-bit arrays in the type-buffer header. The comsat models bake to 112 exact lobes, which cost ~0.3 ms of orbit dispatch for the ~200 GEO satellites above a horizon, so they bake at the 48-lobe budget (validator p95 < 0.01 mag).

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

## Kessler syndrome

- **2026-10-16** — Built overnight from the user's brief (branch kessler). The user picked physical rates, 10 cm fragments and J2 only inside the scenario, and wanted it to happen after the 2037 eclipse with a mystery bookmark. Dead satellites were pool records at first; the pool passed 2M records by day 120, so they became roster records with a death second.
- **2026-10-16** — The event log was too large to ship (about 60 MB by day 200), so the cascade is computed at runtime and cached locally in the user data folder.
- **2026-10-16** — Round 2, after the user's first test ("you'd only know because the sim gets laggy"): the 750k records had been 10 cm representative fragments, invisible and mostly hitting each other; rendering them cost ~0.2 ms, the slowness was the engine and the time hold. Records became the visible pieces (>= 1 m, drawn with the debris kit), the small fragments a density field, and dust clouds were added.
- **2026-10-16** — The density grid's first cut gave every hit the cell's mean fragment mass (~0.5 kg), so nearly every hit was catastrophic (1.1M by day 60); projectile sizes are now drawn from the power law.
- **2026-10-16** — The dust's first cut used the sky's radiance units directly and was invisible even at 100000x: the night exposure is not physical, so the clouds are calibrated through the point-source magnitude scale.
- **2026-10-16** — Round 3: slot recycling (a record's slot released 300 s after its death) replaced a growing pool; one stored record per small statistical orbit had reached 66M records and ~5 GB by day 150, so the small fragments leave the grid through 30-day death buckets. Engine events were all applied at the 6-h step's midpoint (bursts of dust and re-entries) until they were spread through the step. One simulated year went 670 s / 13.3 GB -> 507 s / ~4.3 GB.
- **2026-10-16** — Issues 20261016_154921 / _155342: re-entry fireballs seen from above were ~8 mag too faint (an airmass clamped at 1/0.03); late in the cascade the game stuttered (50-180 ms spikes from the settings tab's live counts walking 5M pieces four times a second, 15-25 ms a frame from the dust code scanning the whole future event log, and the worker running 100+ days ahead after a burst of fast warp). The worker now follows the current warp down, the counts are gathered over frames, the dust scan stops 6 h ahead.
- **2026-10-16** — Round 4: small-fragment re-entries as per-hour expanded streams; 9.8M streams / 350 MB before merging to at most 4 per breakup and packing to 24 bytes (6.4M / ~155 MB at day 100).
- **2026-10-18** — Issue 20261018_205438: fireball trails were symmetric Gaussians about the midpoint of the last 1.5 s of path, so they reached well ahead of the fireball; issue 20261018_211013: dust clouds popped as the view moved once the 384-Gaussian budget was full, and vanished at exactly 3 h. Material colours for re-entry fragments added the same day.
- **2026-10-19** — A player breakup used to restart the cascade from the CME (replaying every event) while time held at the frontier, and breakups were refused before the CME: 2.4 s (day 9) to 4.3 s (day 30) of hold, the debris vanishing meanwhile. The rollback journal and in-place step re-run made it 30-100 ms (day 9) / 229 ms (day 30) with no hold. Steps moved to a fixed grid with seeds from the step index (the history had depended on when the worker paused). A breakup before the CME now starts the scenario at its step (the user: "if the user clicks the explode button, we presume we want to start the Kessler situation").
- **2026-10-19** — The info window's ASAT strike button was removed (one click, Explode only); the ASAT kind stays in the harness.
- **2026-10-20** — Issue 20261019_231126 ("Space Station Explosion"): an exploded station stayed selectable from the Missions window and its crew flights kept flying; its dust cloud was up to 12 straight Gaussians ~2000 km long whose count was a step function of age (the width jumped ~50% at 1 -> 2) and whose lumps were hashed per piece index, so seen from orbit they were crossing "beams" that stretched and glitched. Now roster fates, no crews for a lost station, and the Clohessy-Wiltshire tube. The explosion scale factor S = (m / 2 t)^0.75 took the ISS from 239 / 7 pieces over 10 cm / 1 m to ~13,000 / ~330, and a big cloud's brightness goes as the square root of its dust past ~5 t.
- **2026-10-20** — Issue 20261020_221525 ("Dust doesn't seem to appear anymore", day 48): after the tube rework clouds were ranked by amp / (nearest range floored at 100 km)^2; in a late cascade dozens of identical breakups tied at the budget's cut and the cut's fade drew every one at ~0. Ranking by surface brightness, fewer pieces for clouds small on screen and protecting the first 60% from the fade restored it (x1000: 10% of pixels changed, p99 +70 levels).
- **2026-10-20** — The deployment era: satellites not launched yet are left out of the Kessler world (the storm cannot kill them, they collide with nothing); cache version 15, then 16 when the 2020 benchmark shells left the era by default.
- **2026-10-20** — Bookmark audit: with the 2026-10-11 cloud defaults the Venezuela rainbow and the Antarctic snow bookmarks showed a clear sky; their storms were re-framed (same moment, new place or cloud phase) rather than the defaults re-tuned. Weather bookmarks must be re-checked after any change of the cloud defaults.

## Launches and rockets

- **2026-10-18** — Launches built on the kessler branch (plan .plans/LAUNCHES_PLAN.md): every plane refreshed once per design life, trains climbing to their slots, retirees re-entering; roster size constant. Measured cost with a launch in view at first: +0.3 ms GPU, 0.3 ms CPU.
- **2026-10-19** — Pad deconfliction added after two Starships were scheduled off Starbase 7 minutes apart.
- **2026-10-19** — Before this date the whole train appeared at once at deployment, a string 120 m apart behind the stage, and members were drawn in the open-book pose from the moment they left the dispenser (a Starmind's 70-m wings overlapped the ship). Members now leave one at a time, stowed, and unfold.
- **2026-10-19** — Issue 20261019_205535 ("serious jitter at 10x during deployment"): train records were built from the bare circle's velocity while the member drifted in a straight line in the rotating frame with a fixed dispenser offset up to ~80 km (a constant 0.07 m/s^2 push no orbit has), had no SSO node rate, and used the double mean motion where the GPU multiplies its float: jumps up to ~2.4 px a frame at 1 km at 10x. Records are now the member's osculating orbit (~0.1 m CPU, ~1 m GPU at each rewrite).
- **2026-10-19** — Crew rotations added (ISS, Tiangong, commercial stations, ROS, Bharatiya). Before, the launch and Sun's Fury controls lived in Settings > Constellations and there were no crewed flights; launch "Go to" kept the eye's height (a jump from orbit stayed in orbit).
- **2026-10-19** — Exhaust trails became polylines through their parcels with a growing head at the nozzle; before, each parcel was drawn as the stretch of path it was emitted on and popped in as a needle ahead of the stage, the upper stage's young vacuum parcels (8.5 km apart, each with a ring of rim Gaussians) read as puffs, and following a climbing stage the frame pulsed about 9 levels at each new parcel (issue 20261019_175126).
- **2026-10-19** — The ozone term in the plume colour was applied twice with a secant up to 30, turning the plumes between the orange low trail and the white bell magenta; it is now counted once, capped at 12, and the ice's blue applies only where the sunlight keeps its blue.
- **2026-10-19** — The flame was one 950 m by 126 m Gaussian centred half its length behind the stage (it reached past the nose), carrying the point's display energy about 10 mag over physical; its point sat on the aft end as a white disc, and a coasting stage's point faded only to +10 mag was a -7 disc at 100 m. Replaced by three Gaussians out of the model's engines with a resolved surface brightness.
- **2026-10-19** — Rough stainless steel at grazing incidence passed the Sun-like radiance test and drew dozens of glares on a Starship in daylight; mesh glare now needs a compact Sun image (roughness below about 0.1). Measured with billows: GPU 21.3 / 16.0 / 18.5 ms (off: 21.1 / 15.9 / 18.3), RTX 3070 Ti, 1600x900.
- **2026-10-19** — Every upper stage now deorbits with a real burn (pitch-over to retrograde, burn onto an entry ellipse, coast to 100 km). Before, Starship's ship never turned or burned and jumped from its 350-km orbit to 100 km at the entry interface, and only expendable stages burned (40 s, sinking 30 km on their circle).
- **2026-10-19** — Stage attitudes are smoothed: the booster's pitch angle is averaged over 10 s and the upper stage's nose over 6 s. A first cut averaged booster nose vectors and spun the booster ~340 deg/s after an RTLS boostback (opposite vectors); averaging the pitch angle replaced it. Before that, axes switched instantly at staging and for the deorbit burn.
- **2026-10-19** — Rocket sound uses a scan back over 440 s for the latest sign change of the retarded-time equation. A fixed-point iteration did not converge for a fast distant stage: an upper stage 1200 km downrange was heard at full level 300 m from its pad minutes after lift-off.
- **2026-10-19** — Clicking a rocket's mesh selected the mesh slot (holding only last frame's state), so a selected or followed rocket froze in place (still "up there" months later) and, followed, was drawn twice. It now picks the rocket's transient; followed transients get their own slot evaluated at the exact frame time. A stack followed through staging used to go on with the booster; it now continues with the upper stage.
- **2026-10-19** — The ship comes home to a tower: the deorbit burn is timed (72-h search) for an entry ending 120-450 km short of a Starship pad. The entry's start was made continuous (it jumped ~0.5 km).
- **2026-10-20** — Issues 20261019_223922 / _225243: Starmind did not fit Starship (the 10 m bus needs a 10.4 m circle against an 8 m bay; it carried 40, now 8 with the bus folded), and Guowang appeared out of a fairing that never opened. Payload stacks, fairing separation and folding added. Launch CPU at the default Starship bookmark went 1.0 -> 2.1 ms (Starmind's launch rate x5, ~5300 live batches), then 1.24-1.28 ms after a CPU pass.
- **2026-10-20** — Deployment era added. Scenario numbers from 20 Oct 2026 to 22 Nov 2036: ~197k launches (Starmind 188.5k, 123.6k of them first fills at 8 per Starship), peak 107 a day (Starship 104) in late 2032. Same-pad pairs within 90 min per week at the 2032 peak went 398 -> 89 after rotating the deconfliction's alternative pads (they all started at the list's head: 20 a day on Louisiana A-1) and adding 27 Starship sites. The two launch bookmarks were re-shot (adding sites re-hashes every launch's pad). Launch CPU at 2036 unchanged (0.97 vs 1.04 ms era off).
- **2026-10-20** — Mission-ops suite (tools/harness/mission_ops.py) found: trains' climbs ended at Kepler's rate rather than the slot's J2 mean motion; the phasing residual was a constant drift that stopped dead at arrival (18 m/s for direct-to-cell); fairings separated at MECO + 30 s (~60-100 km) because the altitude search ran before the deorbit plan set tStageGone; trains released 2 s apart overlapped (~1 m) for minutes; formation members raised on independent schedules and spread up to 10,000 km before meeting at arrival (now the cluster shares its leader's schedule).
- **2026-10-20** — Issue 20261020_214759: only the rocket flew on crew launches, the capsule's rendezvous was not drawn and milestones jumped from separation to the stage's deorbit; crew spacecraft now fly as their own objects. The first crew attitudes slerped through 180-deg turns and flipped sides frame to frame (178 deg/s); the parachute phase started with an Earth-rotation velocity jump.
- **2026-10-20** — Issue 20261019_231126: an exploded station stayed selectable from the Missions window and its crew flights kept flying. Crew launches now stop when the station is lost.
- **2026-10-20** — Ground clouds at lift-off and landing, a shared Earth-fixed billow field below a few km and per-Gaussian self-shadowing were added; before, a launch had six fixed puffs at the pad, smooth Gaussian chains for a trail and nothing at a landing. Daytime plumes were about 13x too dim because their phases were per steradian while the sky's average 1 (4 pi).
- **2026-10-20** — Engine and floodlight light on clouds, haze, exhaust and ground added (lightning.frag mode 2, 32 lights in the dust buffer's tail). Measured +0.26 ms at a Starship night launch under broken cumulus. Shading the cloud by screen derivatives of the half-res distance was tried and drew a crosshatch.
- **2026-10-20** — QA sweep: following a Starship 310 m off at night the view was a saturated orange-white haze (the exhaust lit at the night lift, hundreds of times past white); the eye's adaptation to the engine lights was added. The first ~0.5 s after a jump beside a stage was a white-out until cuts snapped the adaptation.
- **2026-10-20** — A bright ball ~300 m across round a followed stage was the exhaust lit twice: once in cloud_march and again by the engine-light pass as if it were a cloud at its own distance; the same double lighting drew far horizon clouds as darker half-res blocks inside the glow. Resolved by flagging emissive/plume texels in the half-float alpha and self-shadowing each Gaussian's engine light. Cost +0.05 to +0.25 ms of cloud march.
- **2026-10-20** — From 200 m under a Super Heavy the flame point was an effectFlare of ~2e4 (a screen-wide glare) and the tail Gaussian lit the whole screen from inside (issue 20261019_222938); the point is now capped toward 14 as the flame resolves, Gaussians the eye is inside fade, and the nozzle bells glow. Night bells first saturated to a white ring and were capped to a displayable colour per exposure.
- **2026-10-20** — A launch's glow seen from 3.7 km over a cumulus far below it was cut out by that cloud (issue 20261019_222829); emissive light is now hidden only by clouds nearer than it.
- **2026-10-20** — The flare point sat on the core Gaussian ~33 m behind a Starship's exits in vacuum, ~300 px off the engines seen at 66 m (issue 20261020_181510); it now sits 0.08 exit diameters behind the exit plane.
- **2026-10-20** — The low billow field was read in the eye's ENU axes, which turn with the eye's position, so it slid with the camera (2 km/s at the reporter's speed, issue 20261020_214200); it is now anchored to the terrain detail's integer ECEF cell.
- **2026-10-20** — Floodlit pad apron given a flood-shaped falloff and per-slab shade (it was a uniform pale slab); pad floods dimmed from -4 to -3 at 1 km and their flare cap from 25 to 14 (they read as a field of large stars from a pad's edge). Open lattices (catch tower, grid fins) switched to a per-frame threshold under the TAA (they were a fixed speckle that swam).
- **2026-10-20** — Booster recovery is chosen per launch from payload mass (RTLS to the landing zone when light, else droneship / landing ship / ground). Droneship trim replaced a boostback aimed at 0.55 x the MECO speed that was never burned, under which every droneship booster came down ~410 km out. Falcon 9 / Crew Dragon / New Glenn MECO speeds were raised (~2.2 / 2.2 / 3.3 km/s) so their boosters reach the ships.
- **2026-10-20** — The landing burn became a constant deceleration zeroing both velocity components at touchdown; the earlier suicide-burn profile touched down at ~40 m/s down and ~360 m/s across for a droneship booster.
- **2026-10-20** — Issue 20261020_213739 "Rockets land tilted": drag was half the real value and there was no lift, so boosters reached 8 km crossing the ground at 650-1000 m/s and their landing burns leaned 35-50 deg. Fixed with beta = 2200 kg/m^2 and steered lift up to 0.6 x drag; touchdown now ends at 1.5 m/s straight down and boosters with legs deploy them 7 s before touchdown.
- **2026-10-20** — The mission-ops suite (tools/harness/mission_ops.py) found: the booster track lacked its MECO sample (everything after staging 0.5 s early, ~800 m jump at separation); the upper stage's ground-arc rate was its inertial speed (5-13% too fast, a 250-940 m/s drop at SECO); fairings separated at MECO + 30 s because the search ran before the deorbit plan; the booster milestones used the vehicle's droneship track and label for RTLS / ground landings. All corrected.
- **2026-10-20** — Mission milestones and skipping added (keys ] / [, Info window MISSION section, Go to bar, Missions window). A selected or followed upper stage now stays selected through its re-entry even when the fireball cannot be seen.
- **2026-10-20** — QA sweep: a pre-launch rocket read "coasting, T+-4 s, 0.41 km/s" (the Earth's rotation) and a ship being caught read "deorbit burn"; labels now read "on the pad", "belly-flop descent", "flip and landing burn", "caught by the tower", and speeds are over the ground below 80 km.
- **2026-10-21** — Free flight about one reference orbit: Falcon 9 trains had closed to 0.8-1.6 m (drift linearised about each member's own plane + random spread). Every member now gets the same in-plane push, a -2 n x0 trim, and deploy intervals raised for >= 30 m separation (Falcon 9 dispenser ~67 s, Gen1 ~100 s).
- **2026-10-21** — Before this date pads had one fixed kind (39A showed its Starship tower in 2019), stations appeared whole on their first module's date, ~1.5k Starlinks were counted twice in 2019-2024 (benchmark shells + Gen1 first fills), and the ambience heard the 2036 roster at every date. OneWeb / Amazon Leo stacks rechecked: with the models' stowed sizes (~2x the real satellites) no column fits their fairings (OneWeb misses Falcon 9 by 0.08 m); left unstacked.
- **2026-10-21** — Visiting vehicles: before, no cargo flights (Progress / Tianzhou were render-only greebles on station models), Soyuz / Shenzhou docked 8-40 h after lift-off (no launch-time phasing), phasing burns were impulsive (~84 m/s^2 in a 1-s difference), the approach arc rolled 180 deg in a second on some arcs, and a docked vehicle at a destroyed station stayed docked to nothing. Measured after (92 cached flights, three epochs): fast dockings 3.3-6.8 h, burns <= 13 m/s^2, turns <= 1.5 deg/s, port conflicts 0.
- **2026-10-21** — Meshes gained local lights (8 per frame) from engines and pad/ship floods, replacing emissive Gaussians that stood in for the floodlit deck and booster up close. The white halo round a night droneship landing was the sea-spray plume (6x the exhaust's mass) lit by the burn and floods; a deck landing now raises a thin exhaust skirt and landing trails are at half cross-section. The lit haze's core was floored at r0, a flat disc with a rim; it is now softened in quadrature (issue 20261020_213739).
- **2026-10-21** — High plume billows were read in the eye's axes scaled by the inverse sigma along the view, so they rescaled as the camera moved round an elongated trail; now read in the parcel's own frame. Landing ships ride a swell; grid fins draw solid at the grid's mean below 3-6 px a cell (they were a dithered speckle).
- **2026-10-21** — Fast boosters (> 2.4 km/s, New Glenn) start the entry burn up to 100 km and burn until peak drag stays near 1.5 g; New Glenn's booster had peaked at 92 m/s^2. The deorbit burn became a flat thrust with 1-s ramps and the ellipse's apoapsis at the burn's middle; the smoothstep blend it replaced peaked at ~4x dv/D (~90 m/s^2 for Qianfan). Booster feet had sat 1.7-2.4 m below the deck before `leg_m` lifted the touchdown.

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
- **2026-10-04** — Rain, snow and diamond dust at the eye became instanced particles drawn after the sky TAA. The
  old per-pixel lattice inside the half-res composite (a few hundred fat streaks, smeared by the TAA, no bow
  optics) is gone; the cloud march got ~1 ms cheaper. **2026-10-05**: drop motion is integrated over sim time on
  the CPU, so fall speed and wind can change smoothly. [Weather](../rendering/clouds/weather.md)
- **2026-10-04** — Rain-only samples stand for four coarse steps: a snowing storm at night went 40.6 to 27.2 ms of
  cloud march. The fog/dust march is skipped under opaque cloud seen from above (-2.8 ms in an orbit storm view; its
  "~0" cost had held only at ground-level benchmarks). Cirrus bounce light reuses the field's flow (~1-2 ms inside
  the high layer, which is ~11 of 32 ms there). [The march](../rendering/clouds/march.md)
- **2026-10-04** — Rows of identical puffs converged to the horizon at 9.6 N 134.6 W, where the shape tile's x axis
  lay in the horizontal plane. Per-sample warps hit the register cliff (+5-9 ms), so the CPU picks one of four
  tile rotations per frame. [The cloud field](../rendering/clouds/field.md)
- **2026-10-04** — Render scale shrank only the sky pass, so 50% bought ~10 FPS; the clouds and depth pass follow
  it now (orbit storm 32.4 / 20.6 / 11.9 ms at 100 / 75 / 50%). Without keeping the 100% noise footprint the
  storm cumulus texture vanished at 50%.
- **2026-10-05** — Lightning rebuilt: only dominant anvil-reaching towers had flashed (a map-typed storm without
  them had none), and the 14-vertex channel inside the half-res composite was pudgy and smeared. Flashes come from
  towers and a fixed storm-region lattice; channels are host-built geometry drawn at true width. The first region
  lattice grew with altitude (descending reshuffled every flash) and put flashes in clear air above capped cumulus.
  Thunder became the channel's own arrival envelope instead of a synth roll with random peals.
  [Weather](../rendering/clouds/weather.md)
- **2026-10-06** — The Reflect beams lit every precipitation shaft with the rain phase whatever the temperature (the
  Sun and Moon already switched to snow below freezing), so the user's "Satbow over Antarctica" bookmark at McMurdo
  (-23 C) showed a rainbow opposite the satellites. The beams now follow the shafts' liquid share; the bookmark
  became "Beam-lit Snow over Antarctica". The same day the rain/snow particles at the eye got the beams' light,
  rain defaults rose (Rain 3.0, fall speed 5) and snow got its own capped fall phase.
  [Weather](../rendering/clouds/weather.md)
- **2026-10-06** — Dust seen from above fades to 8 % by 60 km (was a quarter by 80 km): from orbit at night its
  city-lit glow drew tan blobs round desert cities in the release cinematic's pull-out.

- **2026-10-10** — Surface refinement added to the view march: before it a far fine step (~460 m at 40 km) against a lit skin of tens of metres made each ray sunlit or self-shadowed by chance (grain on distant anvils) and ~17% too dark versus a 25-m-step reference. Dropped variants: no distance gate (+3.4 ms over near cumulus), two bisections for short steps (a darker oval), shading at view optical depth 0.7 (stripes, 222 registers).
- **2026-10-10** — Cb tower lobes made to flow up the stalk and out of the head: they had read the storm volume at the global wind anchor, a slide along one ECEF direction through towers standing still in it, which at 25 N 134 W pointed 56% downward, so the lobes sank down the columns.
- **2026-10-10** — The single eye Sun ray applied within a 2-km Gaussian of the eye's Sun line (review 22b's profile) was replaced by a 32 x 32 Sun shadow grid read where each sample's light march ends: the profile darkened every cloud near the eye when the Sun slipped behind one cloud's tip and counted the blocking cloud's depth twice. The grid's first cut (7 x 7 single rays 500 m apart, length tied to the eye's height) drew straight-edged squares that flipped with ~100 m of climb.
- **2026-10-10** — Sun shadow cascade (128 x 32 rays, +-205 km) added: before it a tower 20 km away stood sunlit behind a storm that shaded it and the shadow popped in on approach. Both grids then became jittered running means; fixed sub-rays had drawn stair-stepped shadow edges at a grazing Sun.
- **2026-10-10** — God rays moved from the march (which scaled a pixel's cloud transmittance by the shadowed share of its airlight from a 6-km light volume, dimming only pixels that had some cloud: a stair-edged sheet over the cirrus) to the sky pass reading the cascade. The light volume was left unbaked and deleted on 2026-10-13; the Sun / sky table took its bindings.
- **2026-10-10** — Thunder became streamed sound shells: the old per-flash envelope computed for where the listener stood at the flash was wrong for a fast-moving player, its brown noise low-passed at 100-360 Hz was nearly inaudible, and the layer was always listed as playing.
- **2026-10-11** — Cloud defaults rebaked from the user's tuning; per-pixel age / accumulation added to the resolve (tstab at snapshot 27: pan 2.19 -> 1.90, pitch 1.72 -> 1.36, walk 3.81 -> 3.54). Light LOD defaulted to 0 after it was found to be the root of grain on lit / shadowed cloud with an 8-km light march (raw std 3.69 -> 0.93 levels) and brighter than the full march. Multi-scatter octaves added for blue-grey tops under a 10-12 deg Sun with a very low multi-scatter strength. A smoothstep small-cumulus lift and a second regional value each cost the march its 128-register allocation.
- **2026-10-12** — Still-view resolve box widened 3x3 -> 7x7: with all-or-nothing samples a zero-width box clamped the history to one value and drew the 64-texel blue-noise tile over half-shadowed surfaces (9.3 levels of grain, 16 levels too dark). Slack and 5x5 tried (4.8 / 1.8 levels); 7x7 gives 0.4.
- **2026-10-12** — Cloud samples began reading the shaft lattice, and its rays march their own far field: the cascade's coarse cells read lit where the fine grid read shadow, so a cloud shaded by a far one was shadowed only within 3-6 km of the eye. Ring 2 (1000 m cells) added on request; GPU frame unchanged.
- **2026-10-12** — skyZenithAt 6 -> 12 steps: at six, twilight cloud sky-light was magenta (R/G 1.7, B/G 1.9 at -1.7 deg); equal-mass strata were worse. The light from below became the ground's (partly shaded, mostly neutral): it had been 0.35 x the zenith colour, unshaded, so overcast bases glowed blue and the lobe occlusion drew sky-coloured blobs. "Terrain sky light" default 1 -> 8, range 0-4 -> 0-20.
- **2026-10-13** — Batched march (density loop, then lighting loop, 32 samples): before, rays in empty air waited out neighbours' light marches. Cloud bucket at 1920x1009: orbit storm 78 -> 38 ms, 58 km 38-48 -> 22, anvils 17 -> 9.2, cumulus 16.3 -> 9.2. The Sun / sky table replaced a per-ray cache (-16-18% from orbit); light steps merged in pairs under large pixels; ground-shadow gate under opaque cloud. Dropped: one sample per batch, early termination (1-4%), a view-step floor (contour rings), three tower-search rewrites (215-255 registers), DontInline (3-4x slower).
- **2026-10-13** — Cumulus shape pass (issues 20261012_235712 / _235159): pancake decks, lobeless storm cumulus, smooth towers and muddy near clouds addressed with height variation, low-coverage decks as cells, lobe lift, big lobes, break-up, headroom, wisps and near edge sharpening. The pass-14 deep cores ("Cb columns" 0) and pass-11 cones ("Tower top" 0) were deleted, which brought the march from 222 back to 128 registers. Contour rings on anvil heads traced to the fixed coarse lattice; per-step coarse strides fixed them. GPU 21.2-24.8 -> 19.8-22.5 ms at nine shots; tstab at shot 5 1.45 -> 1.88 (more relief to get wrong in motion).
- **2026-10-13** — Edge trails in fast low flight (issue 20261013_184710): clear pixels behind a cloud's trailing edge reprojected at infinity (parallax floored at 50 km) passed the still 7x7 box; they now take the nearest 7x7 cloud's parallax. "Light march footprint" default 0.08 -> 0 (it erased a cumulus's self-shadow). Silver lining rebuilt as forward-scattered transmission (the narrow lobe alone moved a backlit rim ~6 levels). Near shadow lattices stopped carrying the mid/high layers (a grey disc following the observer under altocumulus).
- **2026-10-13** — Grain on moving clouds: the 3x3 motion clamp collapsed on all-or-nothing light samples; the colour box was widened to the 5x5 samples at the pixel's distance (13.1% -> 0.4% of pixels past 16 levels 1 s after stopping). Shaft rings got stamps after far shadows broke for ~100 frames after a 6-km hop (15% -> 1%).
- **2026-10-14** — Light march to equal steps with per-step jitter: the 1.6 geometric series left clouds in another's shadow 30-60 levels too bright under a low Sun (Jensen bias); equal steps leave +8-10 at the same cost. The resolve takes history unclamped when truly still (flicker at 158 km 0.10% -> 0.0002%, 20.8 km 1.14% -> 0.00002%), gated off while the shadow lattices fill; the eye-parallax cap became max(50 km, 2 x eye height). Ring stamps extended to 8 wraps, running means per slot, leading-edge bakes while moving; the cascade is no longer cut at 60 km.
- **2026-10-14** — Layer shadow map added: until then the mid/high layers' shadows lived only in the light march (~8 km) and the cascade, and a sheet shaded nothing beyond the light march at a low Sun. With the map on, the cascade drops the mid layer too, which also removed fake sunbeam wedges along the horizon. Validated against a brute-force march (the map softens a sheet's edge over a few km).
- **2026-10-15** — HQ export running mean: at the still weight every exported frame was an independent draw keeping ~15% of per-sample noise (grain on wispy edges, worse than the preview). broll_earth shot 1, pixels reversing > 4 levels: 0.78% -> 0.25%.
- **2026-10-20** — Cloud defaults rebaked from the author's settings (sun gain 2.3, multi-scatter 0.27, history weight 0.046, accumulation 39, mid density 0.3, cloud sun Rayleigh 0.86, among others); adaptive rate, sparse-when-still and "Clouds follow render scale" default off, with the Low..Potato presets turning them on. Verified at four views: mean |diff| 0.01-0.32 levels against the author's settings.

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

- **2026-10-12** — "Terrain sky light" default 1 -> 8 (range 0-4 -> 0-20), so twilight terrain follows the sky glow as the naked eye sees it.
- **2026-10-19** — Launch pads drawn on the ground. The first cut graded every coastal pad into a round island and turned the Starbase lagoon into land (the DEM reads 0 m there); water and sea-level land are now left alone except the apron.
- **2026-10-20** — Coast patches around launch sites: the pads had sat on the 5 km water map and DEM, so Starbase's foreground and the Cape's pads read as sea and the pads as islands of apron. Floodlit apron: it had been a uniform pale slab (0.35 x albedo), now pooled by the floods at 0.2.

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
- **2026-10-06** — "White static" on the ocean was the cloud shadow's 2x2 ordered march grid read raw by the
  reflection; it reads the filtered shadow. Cinematic playback over the sea counted as frozen (the pause flag was
  held), so moving waves trailed under an unclipped history; still/frozen come from the sim time.

- **2026-10-13** — Storm sea trails apply only where the weather map rains and fade over 1-4 px of parallax (issue 20261012_235712 shot 7): on every stormy sea the kept history had dragged the clouds above it across the water in fast flight.
- **2026-10-20** — Engine lights are mirrored in the sea from their real position (issue 20261020_181339 "Rocket Ocean Flare"): from 4.9 km up, 3 km from a Starship at T+46 s, the reflected cloud composite drew a glitter path from the horizon down at the mirrored direction, while the true mirror point was 74 deg below the horizon, not 17. The same issue's shot 2 (a flame in front of horizon cloud sits in that cloud's unflagged texel) needed the second test against the engine-light list; 2026-10-21 it was limited to texels the flame dominates, so real clouds near a flame keep their reflection.
- **2026-10-20** — Defaults: far sea ripple 1.5 -> 1.7, ripple size 5 -> 1.5 px, wave sharpness 0.5 -> 0.1.

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
- **2026-10-06** — Far city lights still "z-fought" at low angles: sprites just behind a crest were hidden or drawn
  by turns with the TAA jitter (~240 / ~37 levels a frame). They are depth-tested against the unjittered half-res
  scene depth instead of the restored hardware depth (still-view flicker 0.57% to 0.31% of pixels).
- **2026-10-06** — The city lights under cloud are no longer blurred. The blur mixed them toward the night map at
  mip 8.1 (~1000 km per texel) by the cloud's opacity in front, so from orbit every city under thin cloud was a grey
  wash with the cloud's outline ("blobby" cities in the release cinematic). The "City light blur LOD" slider went
  with it. The air's city glow takes a height-following mip from 800-3000 km up; applied from 30 km it drew 10-20 km
  texels as squares round towns from 400 km. [Cities](../rendering/cities.md#under-clouds)

- **2026-10-10** — City points under cloud give way to their local mean. Once the under-cloud blur of the night map was removed (2026-10-06, the grey wash from orbit), glitter points and road dots stayed crisp through a deck well under 1% transmittance; a cap on the points' peak was tried first and still read as dots.
- **2026-10-19** — Launch pads exclude city lights within ~3 km (city lights had run across the launch sites).

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
- **2026-10-03** — The auroral oval became a Sun-fixed ring in magnetic local time driven by a Kp index from a
  space-weather model. It had been a band at 20 deg colatitude (+8 x "Storm strength") fading over twice its
  width, lit from ~5 deg off the pole (a cap from orbit) and fixed to the ground. Calibration over 2030-2041:
  936 / 501 / 174 / 103 / 9 three-hour intervals per cycle at G1-G5 against NOAA's 1700 / 600 / 200 / 100 / 4.
  [Aurora and airglow](../rendering/aurora-airglow.md)
- **2026-10-06** — The sky TAA's weight ramps on parallax (0.1-1 px) as well as total motion: climbing over a
  city at night (Glasgow, 60 m/s) the ground moved 0.1-0.5 px a frame, ten frames of fractional resampling stacked
  up at the still weight, and the lights smeared (tstab 2.08 to 1.67 at 75 % render scale).
  [Atmosphere and sky](../rendering/atmosphere-and-sky.md#sky-taa)

- **2026-10-10** — God rays: the sky pass shadows its own air by the clouds' Sun shadow cascade, so the clouds and the air between them share one shadow. The first god rays (an experimental option) scaled a pixel's cloud transmittance by the shadowed share of its airlight from a 6 km light volume; since the sky pass dims only the air behind a pixel's cloud distance, clear pixels were never dimmed and any pixel with a faint veil was dimmed in full (a darker, stair-edged sheet over the high layer's haze). The light volume is no longer baked. Cost of the replacement about 0.7-1.0 ms of the sky pass at 1920x1009.
- **2026-10-13** — The sky's Earth-shadow test became an exact per-pixel interval in the shadow cylinder. The midpoint test drew a hard band above the sunset horizon (about 4.3 deg at a -0.84 deg Sun) and veiled the clouds inside it through the air split (row luminance step 186.1 -> 177.7 before, smooth after).
- **2026-10-14** — The same exact sunlit share was applied to the zenith sky light, the terrain's sky light, the flat layers' twilight and the sea's sky reflection (offline replicas: largest step per 0.01 deg of Sun 52% -> 2.5%).
- **2026-10-14** — Aurora sky gate (issue 20261014_193231): a Kp 9 storm was invisible from an Iowa farm and from Arizona and cut off along a hard azimuth toward a town. The aurora read the Milky Way's dark-sky dome, a log ramp from 1e-6 toward an artistic 13 mag/arcsec^2: a raw glow of 0.01 read 83% "city" and populated farmland read 1.0 in every sector; its brightness slope was 0.85 mag/decade, so a whole display vanished at once. It now uses a physical, linear skyglow background and 2.5 mag/decade.
- **2026-10-14** — Aurora storm features. From orbit the Kp 9 oval had been ~50 even concentric rings; arc systems and a coverage threshold that no longer fills the oval replaced it. A first arc-system cut (55% of groups, short stretches) left a Kp 9 sky over Coldfoot nearly empty; a first storm red at 7x the final strength drew a solid red donut from orbit; the first giant rays were single ~10 km lines (magenta when unlit) that read as isolated straight lines from orbit; tall red pillars ended on a hard ceiling at the 700 km march top seen from 590 km, hence the fade below the top.
- **2026-10-16** — The sky glow bins sum the bin's bright flares instead of holding the brightest one (issue 20261016_162337): one tumbling piece of Kessler debris at -4 lit a 51-degree patch of sky like a whole Reflect site.
- **2026-10-20** — Defaults baked from the author's tuned settings: extinction 0.079 -> 0.18 mag; aurora sheets 1 -> 1.4, spacing 0.3 -> 0.34 deg, crisp 0.5 -> 0.77, folds 1 -> 1.1, coverage freq 0.43 -> 0.58, az freq 4.3 -> 3.1, shimmer 0.0018 -> 0, storm features 1 -> 1.2, blue rays 0.012 -> 0.004, arc systems 1 -> 2, storm red 1 -> 1.1. Automatic render scale and "Clouds follow render scale" default off; the presets now set the cloud march rate and that toggle.

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
- **2026-10-06** — A sunlit render-only aluminium fitting on a Starmind satellite seeded white bloom discs ~250 px wide
  while the model's own intensity toward the camera was ~0; render-only parts carry no photometric share.
- **2026-10-06** — Mesh glints are placed to a fraction of a texel. The list held quarter-res texel centres, so in
  the release cinematic's mirror shot (500 to 160 m dolly) the glare held still, jumped 4 px and held again; after,
  steps of 0.1-2 px within ~1 px of the Sun's image. The CPU's sprite placement clamped the Sun's image to a circle
  of half the side (25 px off at 500 m); a square along the body axes leaves 11 px.
  [Points, bloom and glare](../rendering/points-bloom-glare.md#mesh-glints)

- **2026-10-07** — The point, bloom and glare now all read one displayed effectFlare (exposure, satellite adaptation, resolved-object factor, formation merge); bloom and glare had ignored the exposure. Measured on the user's StarMind snapshots: a backlit cluster at 4-5 km lit only by earthshine had drawn as full starbursts (x7 by the glare near gain) and now reads as dark silhouettes; the lit Antarctic limb view gains 2.7 mag of adaptation; a ground night view is unchanged.
- **2026-10-07** — The formation merge first scaled the physical flux; the sky glow bins took a MAX per bin, so merged leaders at 8x brightened the whole night sky from the ground (+1.7 levels). The merge was moved to the drawn point, bloom and glare only.
- **2026-10-07** — A small mesh used to keep its sprite's glare for all of its light up to 36 px; only the Sun-like share (glintShare) keeps it now. The mesh glint's kept share was first packed as a half and underflowed for a sun-bright mirror (~1e-9); it is packed as log2.
- **2026-10-07** — Resolved satellites, satellite adaptation and formation merge were added after the user's StarMind snapshots: a backlit, earthlit cluster at 4-5 km drew as full starbursts, an earthlit cluster at 11 km as bright dots. The formation merge first scaled the physical flux, and since the sky glow bins took a per-bin maximum, the leaders brightened the whole night sky from the ground; it was moved to the drawn point.
- **2026-10-19** — Mesh glare limited to compact Sun images (roughness below ~0.1); rough stainless steel at grazing incidence had reached the Sun-like radiance test along every edge and panel seam of a Starship in daylight (viewer glints at the "toward" preset 6 -> 0).
- **2026-10-19** — The Info 3D view switches model on a hand-over; before it kept the old model placed above the observer after staging.
- **2026-10-20** — Shadow-ray acceleration (issue 20261019_225545 shot 3, 85 ms GPU frames next to the ISS): each shadow ray had looped over every primitive and recomposed its pose per pixel. Mesh pass before -> after (RTX 3070 Ti, 1920x1009): ISS report pose 68.9 -> 1.1 ms, 50 m 58.5 -> 0.9, 200 m 6.3 -> 0.4, 1 km 0.6 -> 0.3; Tiangong 40 m 5.9 -> 0.3; Orbital Reef 60 m 3.9 -> 0.3.
- **2026-10-20** — Open lattices use a per-frame IGN threshold under the sky TAA; a fixed per-pixel hash drew a catch tower from ~1 km and grid fins as a speckle that swam as the view moved.
- **2026-10-20** — Engine bells' night glow is capped at a display colour per channel; at the night exposure the physical glow saturated to a white ring.
- **2026-10-20** — Environment probes re-render whole after a sim-time jump; a probe made by day lit a pad's tanks pale blue-white after a jump to night. The Info 3D view of a rocket on its pad at night was a flat yellow-beige: SKY_ENV's ground took the 5-km night map at full strength.
- **2026-10-20** — Glare clusters (issue 20261019_222938, "spidery, sea-urchin-y" clusters): every glaring source is summed into a screen grid and keeps w^2 of its spikes. A lone source changes by <= 2 levels; cost within noise. GlarePC grew to 128 bytes for the grid.
- **2026-10-21** — Meshes take local lights (engines, pad and ship floods); before, the deck glows were emissive Gaussians standing in for the missing mesh lighting, and grid fins were a dithered speckle (now a solid mean below a few pixels a cell).

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
- **2026-10-04** — Presets reshuffled: Medium renders at 67% and Low at 50% with every effect on; Planetarium is the
  all-off tier, and integrated GPUs start there. Below 100% the sky TAA became a temporal upscaler, and an
  automatic render scale was added. [Graphics settings](../using/graphics-settings.md)
- **2026-10-04** — Distant city lights blinked under the sky TAA: sprites z-fought the jittered ground depth, and
  ground points were hit or missed by the jitter and clipped from history. Several fixes decided "still" per pixel;
  climbing over clouds they left trails (climb benchmark 1.60 to 3.87) and were reverted. The final version relaxes
  the history rules only when the whole camera is stationary and reads the lights at the unjittered pixel on steep
  views (still-view flicker 2.31 / 0.58 / 1.07 / 0.94% to 0.57 / 0.04 / 0.13 / 0.04%).
  [Cities](../rendering/cities.md)
- **2026-10-06** — SKY_LITE had grown from 142 KB (v1.1.0) to 567 KB of SPIR-V, because new features were gated by
  runtime knockouts that still compile every call; v1.2 ground features are compiled out of it (the 10-04 choice to
  draw the procedural city on Planetarium was reverted for that), and the build fails past a size budget. Crash
  recovery stepped straight to Planetarium; it steps one tier. During totality the sky pass used the day exposure
  while every C++ consumer of `skyExposure()` used the eclipse-adapted one. [Weak-hardware tiers](../rendering/hardware-tiers.md)
- **2026-10-06** — A startup graphics chooser replaced first-run device seeding plus the intro benchmark, which
  adjusted the preset only after everything (the full sky shader included, on every tier) had loaded.
  [Graphics settings](../using/graphics-settings.md)
- **2026-10-04 .. 10-06** — Fixed from the October documentation pass's suspected bugs: the rain particles' barrier
  after the lightning pass also orders it before the march (the Sun-path profile hazard); a mirror in the Earth's
  shadow no longer beams (`litFactor`); with no active satellite the bloom composite is skipped and the glow and
  ocean-glint lists emptied; the lightning is drawn from a list written this frame only (its old in-composite
  `lightningCS` is gone); the sky pass's eclipse exposure matches `skyExposure()`; knockout 1024 clears the depth
  images to "no surface" when switched on.

- **2026-10-13** — Cloud march restructured into density-then-light batches of 32 and a Sun-colour table (commits c622aa2, 73cf44b): orbit storm 78 -> 38 ms, 58 km 38-48 -> 22, anvils 17 -> 9.2, cumulus 16.3 -> 9.2 (1920x1009, RTX 3070 Ti). Tried and dropped: one sample per batch (slower on overcast), early termination of opaque rays (1-4%), a view-step floor (contour rings), three rewrites of the Cb tower search (215-255 registers, slower), `DontInline` in the SPIR-V (3-4x slower). The light volume and its two bindings were deleted.
- **2026-10-13** — Register cliff: with the cumulus shape terms the march sat at about 222 registers whatever was folded or reordered; deleting relic code (deep cores, old cones) brought 128 back.
- **2026-10-15** — The orbit rebake moved from a CPU double loop plus full synchronous upload (~27 ms at 1.37M satellites, every other frame at 1 yr/s: 28.6 ms frames) to `sat_rebake.comp` (16.7 ms frames, vsync).
- **2026-10-16** — Late in the cascade the Settings tab's live counts walked millions of records four times a second on the main thread (50-180 ms spikes), the dust code scanned the whole future event log (15-25 ms a frame) and the worker ran 100+ days ahead after a fast warp. Counts are now gathered over frames and the worker follows the current warp.
- **2026-10-20** — Hitch pass: right after load the first frames took 6 x 100-140 ms (launch batch builds; first frame 390-570 ms); 1 min/s - 1 h/s had 80-220 ms frames when a launch first mattered (plume parcels on the main thread); 1 mo/s and 1 yr/s had 80-300 ms frames (batch builds with a 72-h ship-landing scan, pad deconfliction over ~2000 planes per build, the Kessler sync, pool arrays reallocated a slot at a time). After: first frame ~270-390 ms, every other rate's max under ~60-90 ms. `src/FrameProfile.h` and `hitches` built for it.
- **2026-10-20** — Mesh shadow-ray acceleration: each ray had looped over every primitive and recomputed its pose per pixel (issue 20261019_225545 measured 85 ms GPU frames next to the ISS). Mesh pass before -> after: ISS report pose 68.9 -> 1.1 ms, 50 m 58.5 -> 0.9, Tiangong 40 m 5.9 -> 0.3, Orbital Reef 60 m 3.9 -> 0.3. Knockout 8388608 keeps the old loop for A/B.
- **2026-10-20** — Launch CPU at the Twilight Starship bookmark fell from ~2.1 ms to ~1.25 ms a frame with identical results (walk reuse, skipping batches that cannot change, cheaper plume Gaussians); `cpu_split_ms` added to `launch state`.

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

- **2026-10-10** — Thunder was a whole envelope computed at the flash for where the listener stood then, played at that distance / 343 m/s: a player moving at many times the speed of sound heard it at the wrong time from the wrong side. Its noise was brown low-passed at 360 Hz from 5 km and 100 Hz from 20 km, close to inaudible on small speakers, and the layer was always up below 12 km, so the Sound tab listed thunder as playing everywhere (at the user's Anchorage snapshot the nearest flash was 27 km away). Replaced by streamed sound shells.
- **2026-10-19** — Rocket sound: the retarded time was a fixed-point iteration that did not converge for a fast, distant stage; an upper stage 1200 km downrange was heard at full level 300 m from its pad minutes after lift-off. Replaced by a backward scan and bisection.
- **2026-10-21** — With the deployment era, the satellite-shell ambience counts only the deployed share; until then it heard the 2036 roster at every date.

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
- **2026-10-03** — UI review: every window and settings tab built from one UI kit (sections, label/value rows, stat
  tiles, pills); a Performance tab took the profiling tools out of Display; the Camera tab folded into Controls;
  bookmarks were added. The HUD altitude readout had added the terrain to an offset that is already above sea
  level. The typed time checked the day only against 31, so "2036-11-31" rolled over to December 1.
  [Features](../using/features.md)
- **2026-10-06** — Nine default bookmarks ship with the sim. The rainbow one was the user's snapshot at rain 3.0;
  at the shipped rain amount (0.71) the bow barely shows, and its storm changes within ~10 minutes, so it opens
  paused. [Features](../using/features.md)
- **2026-10-06** — Exposure, auto exposure, highlight roll-off, white balance and the Moon's size moved to the
  Photometry tab's EXPOSURE AND MOON section (keys unchanged). The startup chooser's first cut had its own soft
  blue theme; it uses the HUD palette. The default bookmarks became six: Reflect Orbital over Anchorage, AI
  Satellites at Twilight, Los Angeles by Night, Sunset from Orbit and Blood Moon over Alice Springs were dropped,
  Blood Moon from Anchorage and Beam-lit Snow over Antarctica added, two renamed.
  [Features](../using/features.md)

- **2026-10-11** — "Dev mode: unbounded values" added for tuning past a slider's range (`numberField(..., devUnbound)`, `sliderStep()`).
- **2026-10-11** — "Dev mode: unbounded values" added for tuning: the author often needed a value past a slider's range. Settings replay used to clamp a few keys on load, which changed values typed past their range in dev mode.
- **2026-10-12** — Issue reports replaced bare snapshot lines in profile_log.jsonl, whose meaning lived only in the chat they were handed over in; several shots of one problem had no link and no image of what the reporter saw.
- **2026-10-14** — UI themes: the palette had been compile-time constants and about 60 colours were literals at call sites (text fields, cards, sliders, toasts, the tutorial's keycaps); they moved into the `UI_PALETTE` table. Midnight became the default on 2026-10-15 and the old look was renamed Classic.
- **2026-10-14** — UI themes: the palette had been compile-time constants with about 60 colours as literals at their call sites; all moved into one table. Midnight became the default on 2026-10-15 (the user's call) and the old look was renamed Classic.
- **2026-10-19** — Explode became a single click (it had needed a confirm click, beside a separate ASAT strike button).
- **2026-10-19** — The info window opened on SATELLITE (model id, triangle count) and ORBIT, with BRIGHTNESS, SKY POSITION and the BREAKUP section (Explode / ASAT strike, each needing a confirm click) collapsed below the 3D view's settings; the user found Explode only by chance. It was reordered to STATUS / BRIGHTNESS / SKY POSITION / ORBIT with an always-visible ACTIONS row, and the ASAT strike removed from the UI (one button asked for); the harness keeps it. Screenshots and issue shots always dropped the UI until the "include the UI" settings and F10.
- **2026-10-19** — The selection panel's second line printed the constellation's type; rockets and fireballs have none, so a bare "?" showed under them. A rocket's selection froze where it was when its mesh slot was clicked (it was "still up there" seven months later) and, followed, was drawn twice.
- **2026-10-19** — The launch and Sun's Fury controls moved from Settings > Constellations (with "Watch the next launch / a twilight launch" buttons and a one-line "Next launch" readout) into the new Missions window. Launch Go to kept the eye's height, so a jump from orbit stayed in orbit; it now puts the observer on the ground.
- **2026-10-19** — Go to: Stay here added. Before, a breakup restarted the cascade from the CME and time held 2.4 s (day 9) to 4.3 s (day 30) with the debris invisible meanwhile, and breakups were refused before the CME.
- **2026-10-20** — Window opacity added; a window's floating children had used zIndex 11-12 and drew over any later window covering them.
- **2026-10-20** — The first-run preset on a discrete GPU with >= 5.5 GiB became High (the compiled defaults); it had been Medium's 67% scale and smaller budgets.
- **2026-10-20** — Following a low rocket could take the camera kilometres underground (3.9 km under Louisiana in a report). There was no way back to the followed object short of re-selecting it, and Go to from one followed object to the next passed the camera through the new one. Missions' rocket Select was disabled when the rocket was not drawn, and a selected rocket was dropped 2 s after it stopped being listed.
- **2026-10-20** — Mission milestones, the Info window's MISSION section, one row of four action buttons, the Sun's Fury card pinned on Upcoming (it was the list's last card and needed scrolling), and window opacity (a window over another let the other's chips and labels show through).
- **2026-10-20** — QA sweep: a pre-launch rocket read "coasting, T+-4 s, 0.41 km/s" (the Earth's rotation), a ship being caught read "deorbit burn", the Info window read "Dark" for a burning stage at night and "Operational" for an exploded satellite. The "Go to ... again" chip offered a booster long since caught after a day-long jump; it is dropped on a jump of more than 10 minutes.
- **2026-10-20** — Defaults became the author's tuned settings and a discrete GPU with >= 5.5 GiB starts on High; automatic render scale and "Clouds follow render scale" default off, and the presets now own the cloud rate and that toggle. Changing the render scale or that toggle by hand makes the preset Custom: before, a named preset re-derived them on every load and the change was lost (a render scale raised to 100% on Medium came back as 67%).

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

- **2026-10-07** — Harness gained `select at <x> <y>` and `state` fields for the CPU photometry by lobe, formation magnitude, mesh glint share, the mesh fade size and the point adaptation, to diagnose the StarMind flare snapshots. A/B in one launch needs the volumetric clouds knocked out first: a `set` resets the cloud history.
- **2026-10-10** — Thunder became streamed sound shells; the harness `lightning` command reports the sources and `audio record` advances them on its own clock. The old precomputed envelope played at the wrong time and from the wrong side for a fast-moving listener.
- **2026-10-12** — Issue reports introduced: snapshots had been bare lines in `profile_log.jsonl` whose meaning lived in the chat they were handed over in, several shots of one problem had no link, and no image recorded what the reporter saw. Settings replay had clamped a few keys on load, changing values typed past their range in dev mode; replays now go through `settingClamp()` and are unclamped.
- **2026-10-12** — Window (step 2) and export / `reports.py` triage loop (step 3) added the same and following day.
- **2026-10-13** — The first description box was one wrapped Clay text element with a '|' caret: each key press was a new string in Clay's text-measurement cache and a long description exhausted it ("Clay ran out of capacity while attempting to measure text elements"). Replaced by `textArea()` drawing its own lines through `UITextBlock`; pin areas and crops added.
- **2026-10-15** — `roster list|remove|add` added with runtime roster rebuilds; `buildOrbits()` had used the global CRT `rand()`, which reshuffled every later constellation when an earlier one changed and drew a different sky on Linux and macOS than on Windows. Per-constellation `SatRng` streams reproduce the Windows layout bit for bit.
- **2026-10-16 .. 2026-10-21** Harness families `kessler`, `launch`, `missions`, `deploy`, `joints`, `cluster`, `flares`, `follow stay|return|toward` grew alongside the Sun's Fury and launch work.
- **2026-10-19** — `ui key` gained the F-keys and the screenshot/issue-shot UI options; `capture ui=1` is unaffected by the player's screenshot settings.
- **2026-10-20** — `launch select kind=rocket` right after `launch next` found nothing (the stack on the pad is not drawn), and with `nearest=1` could take another launch's landed booster; it now takes the framed launch's stage first.
- **2026-10-20** — The mission-operations suite (`mission_ops.py`) found, among others: the booster track lacked its MECO sample (an ~800 m jump at separation); the upper stage's ground-arc rate was its inertial speed (5-13% too fast); fairings separated at MECO + 30 s; trains' climbs ended at Kepler's rate instead of the slot's J2 mean motion; the phasing residual stopped dead at arrival; booster milestones used the droneship track for RTLS landings; trains released 2 s apart overlapped for minutes. `launch_bench.py` added for launch cost.
- **2026-10-20** — The weather bookmarks (Venezuela rainbow, Antarctic snow) showed a clear sky after the 2026-10-11 cloud default tuning; the storms were re-framed rather than the defaults re-tuned, hence the audit rule for bookmarks after a cloud default change.

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
