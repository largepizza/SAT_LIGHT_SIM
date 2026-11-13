# Earth / terrain and the sea

Terrain history, procedural terrain detail, ELEVATION ENCODING (read before touching terrain), water map, city lights, farms, the sea.

Part of the subsystem notes (`docs/subsystems/`, map in `AGENTS.md` §4). Moved verbatim from
`CLAUDE.md` on 2026-11-03. Do not read this file whole: `python tools/outline.py docs/subsystems/terrain.md`
lists its sections and topics with line numbers. New notes go next to the section they belong to.

## Active Development: Earth / Terrain Rendering

See `TERRAIN_PLAN.md` in the project root for the full step checklist and session log.
Read it at the start of any terrain-related session before making changes.

**Current state (as of 2026-07-17, session 29):**
- Steps 1, 2, 3, 4, 5, 5b, 6, 8 complete; C1–C8, C13, C15, C16 complete
- Phase E in progress (C13–C16: Cirrus rework, Anvil, Airglow, Aurora), sequenced ahead of
  C9/C11/C12. Full spec in `TERRAIN_PLAN.md`.
- **Cirrus (C13):** own standalone `cirrusMarch()` in `sat_sky.frag`, NOT a second `cloudMarch`
  call — `cloudMarch` already merges `layers[0]`/`[1]` (2-11km) into one low/mid shell, so there
  was no separate volumetric band to extend. Thin shell (700m) at `layers[1].shellAltM`,
  anisotropic streaks via a fixed global wind-axis compression (`cloud.cirrusWindAngle`/
  `cirrusStretch`, repurposed from the UBO's former `pad1`/`pad2`) — NOT a per-sample tangent
  decomposition (that's a no-op: the noise argument is purely radial from its own tangent frame).
  Sun-only lighting matching `evalCloudLayer`'s formula so it colour-matches the flat paste it
  crossfades against. See `TERRAIN_PLAN.md` session 21 log for the full writeup.
- **Airglow (C15):** three emissive bands (green 96km, sodium 90km, red 275km) gated by per-sample
  geographic day/night, not observer's. Green+sodium accumulate inside the existing `N_VIEW`
  atmosphere loop for free (their peaks fall inside its ~100km ceiling) and stay in `sat_sky.frag`.
  Red originally needed its own small 16-step supplemental march out to `R_EARTH+500km` since its
  peak/half-width sit well past that ceiling — extending the primary loop's far bound for one band
  would have coarsened the near-surface Rayleigh/Mie sampling everything else depends on; that red
  march itself moved to `cloud_march.comp` in session 29 (half-res, alongside aurora — see below),
  though the underlying peak/width/color constants and the day/night gating logic are unchanged.
  `CloudParams` UBO grew 176→192 bytes (all 3 pad slots were already consumed by C13) for 4 new gain
  fields (`airglowGain` master + per-band); peak altitude/width/color are hardcoded physical
  constants, not UBO fields. Reuses the analytic `warpPerlin3` noise (same one `cloudWarpOffset`
  uses) for horizontal patchiness — no new texture/binding. See `TERRAIN_PLAN.md` session 22 log for
  the original writeup, session 29 log for the move.
- **Airglow over time (2026-11-12, `sky_atmos.glsl` airglowTimeOfs / airglowWindDir / airglowNightK; red copies in
  cloud_march.comp).** The noise's time offset is a closed loop through noise space once a UTC day (radius 0.6:
  ~70 m/s of morphing at the 1600-km features), and the sample direction is turned about the pole by
  2 x `cloud.cloudPhase` (~47 m/s zonal; a whole multiple, so its 2-pi wrap is seamless). It was
  `pc.waveTime x 0.015` along x: ~24 km/s, and a jump at 00:00 UTC (`waveTime` = sim seconds in the day).
  Per-sample brightness (mean ~1 over a night, so the gains keep their meaning; hours since the sample's own
  sunset from the Sun's hour angle and the sunset hour angle): green 0.85 + 0.4 e^(-0.4 t) + a post-midnight
  bump at 2 h; sodium (0.9 + 0.2 e^(-t/3)) x a winter maximum (1 -/+ 0.45 at 45 deg, sin lat x sin dec);
  red 0.66 + 2.06 e^(-t/2) x (1 + 1.5 x the equatorial anomaly: |geomagnetic lat| ~15 deg +- 6, gone by 3-7 h
  after sunset). Above the air (40 -> 100 km) the bands are x the Sun's glare gate (`skyGlareVisibility`, the
  Milky Way's): with the Sun on screen from orbit the limb line stayed full beside its disc (39 levels -> 2).
  Not in SKY_LITE (its SPIR-V was 72 bytes under budget; the helpers are `#ifndef SKY_LITE`). Sky pass
  A/B at a night horizon (Atacama, 4 reps interleaved): 8.68 vs 8.84 ms without, GPU 16.64 vs 16.54, noise.
  Defaults since 2026-11-12 are the user's: gain 0.77 (was 0.066: from the ground the bands were < 1 level,
  invisible), red 0.005, sodium 0.095, coverage 0.47. Physical line ratios at a mid-latitude zenith are green
  ~250 R : sodium 30-150 R : red ~50-100 R: column-weighted, sodium 0.095 is ~1.3x green, which makes the limb
  line olive; ~0.02 reads green as in the ISS photographs (`harness_runs/airglow2`, o_na020).
- **Raymarch-from-inside-a-volume fix (session 22):** `raySphere` reformulates `c = dot(ro,ro)-r*r`
  as `c = (|ro|-r)*(|ro|+r)` — the naive form catastrophically cancels at R_EARTH scale (~1e13
  float32 magnitude) exactly at grazing/near-tangent rays, i.e. every horizon, across all 29 call
  sites. Also: any shell march must classify the observer as below/inside/above the shell (keyed on
  `obsEffH`) rather than assuming a fixed forward root — `cloudMarch`/`cirrusMarch` already did this;
  the new airglow red-band march didn't, and broke (bright zenith band + horizon seams) once the
  observer flew (via the uncapped "Raise Elevation"/Q control) into or above the shell and looked
  outward, where the "always below" forward root goes negative. Fixed to match the established
  pattern. Any future shell march (Aurora/C16) needs this from the start — see
  `TERRAIN_PLAN.md` session 22 log.
- **Cloud march perf (session 22 follow-ups):** (1) `cloudMarch`'s C8 altitude-stratified stepping
  uncapped the real 3D step length for oblique rays (up to 50× the vertical step) — this is what
  made "clouds viewed from the side" undersample and band; now capped at a fixed `kCloudMaxStepM =
  250` (meters, not a multiple of the vertical step — see comment on why that matters at high
  `marchSteps`). (2) `cloud.lightSteps` (the "Light steps" slider) was declared but never read
  anywhere — the sun self-shadow cone hardcoded `N_CONE = 6` regardless; now wired up, and it's the
  dominant per-inCloud-sample cost. (3) That shadow cone is now also distance-gated
  (`cloud.shadowMaxDistM`, camera-relative) so far/orbital clouds skip it almost entirely, which
  paid for raising the render-distance cap (`cloud.maxRenderDistM`, replaces a hardcoded 80km) to
  reduce horizon pop-in. `CloudParams` grew again, 192→208 bytes. See `TERRAIN_PLAN.md` session 22
  log (multiple entries) for the full history.
- **Half-resolution cloud compute pass (session 23) — `cloudMarch()`/`cirrusMarch()` no longer live
  in `sat_sky.frag`.** They moved to `shaders/cloud_march.comp`, a new compute shader dispatched
  once per frame in `recordCompute()` at half `ctx.swapExtent` (1/4 the pixels). `sat_sky.frag`
  samples two `RGBA16_SFLOAT` targets (bindings 10/11) instead of marching per full-res pixel.
  Restructured (not just relocated): the compute-shader copies return an `(A, B)` affine-composite
  pair instead of mutating `color` in place, so cirrus-then-cloud combine into one exact
  `(A_total, B_total)` algebraically. No terrain data in the compute shader — `sat_sky.frag` does a
  post-hoc terrain-occlusion suppression using its own accurate `tSurface` against the sampled
  occlusion distance (exact for full occlusion, not for mid-shell partial truncation — accepted
  approximation). New `CloudMarchPC` push constant, new `cloudMarchDescSet` (7 bindings), 2 new
  `skyDescSet` bindings. See `TERRAIN_PLAN.md` session 23 log for the full design (why two targets,
  the barrier sequencing, the `init()` ordering constraints — several real mistakes were caught and
  fixed during design review before any code was written, don't repeat them).
- **Terrain-bleed bug fix + `cloudShadowFactor()` removed (session 23 follow-ups):** the terrain-
  suppression gate above initially used the opacity-gated `tCloudOcclude` (≥90% opaque only, meant
  for satellite depth), so most non-solid cloud rendered through terrain regardless of depth — a
  real bug, not the documented approximation. Fixed with a second, always-valid entry distance
  (`tEnterOut` from both march functions, combined via `min()`) stored in Target B's alpha;
  `cloudBlock` (displaced from that slot) is now derived from Target B's RGB instead. Separately:
  Release-build FPS testing showed the half-res compute move hadn't changed SURFACE performance at
  all (unchanged across the whole session, through every cloud-march fix) — `coverage=0` testing
  confirmed clouds were still the dominant surface cost anyway, pointing at `cloudShadowFactor()`
  (full-res cloud-shadow-on-terrain/ocean, untouched all session) as the real bottleneck. Removed
  outright per user decision (cloud shadowing on terrain isn't in use) rather than optimized — its
  `CloudParams` UBO slot reverted to `pad0`. See `TERRAIN_PLAN.md` session 23 log for both fixes.
- **Terrain/ocean/atmosphere perf follow-up (session 24):** fixed a real regression — the terrain
  march's altitude-scaled step count was `mix(320.0, 320.0, ...)` (a no-op, always paid the
  LEO-tuned 320-step budget at ground level too), restored to `mix(196.0, 320.0, ...)` matching its
  own comment. Also made 5 previously-hardcoded quality constants UBO-tunable (new sliders, all
  defaulting to prior fixed behavior): `N_VIEW`/`N_LIGHT` (main atmosphere loop, `cloud.viewSamples`/
  `lightSamples`) and ocean's `seaMap`/`seaMapDetail` octave counts + reflection sample count
  (`cloud.oceanSeaOctaves`/`oceanDetailOctaves`/`oceanReflSamples`). `CloudParams` grew 208→224
  bytes. **Session 29 update:** real GPU-timestamp profiling superseded the guess that `N_VIEW`/
  `N_LIGHT` (and a transmittance LUT) were the lead cost suspects — `optDepth`'s isolated cost was
  consistently near-zero; terrain's step-count formula (see below) and aurora (see its own entry)
  were the real dominant costs. See `TERRAIN_PLAN.md` session 24 log for the original follow-up,
  session 29 log for the profiling toolkit and the corrected picture, and "Subsystem: GPU
  Performance Profiling" below for how to re-run this kind of investigation.
- `SatDrawPC`, `PointDrawPC` and `CloudMarchPC` are all exactly **128 bytes** — the
  `maxPushConstantsSize` floor. `debugDisableMask` and the other per-frame tail fields ride in the
  CloudParams UBO now (`cloud.dbgDisableMask` etc.); see the **SatDrawPC** / **PointDrawPC** entries
  under "Subsystem: GPU Orbital Pipeline → Push constants" and the "Push-constant relief" block in
  `GpuCloudParams`. Both point pipeline layouts (`drawPipeLayout`, `starPipeLayout`) use
  `sizeof(PointDrawPC)`; `skyBgPipeLayout` uses `sizeof(SatDrawPC)`.
- Sky descriptor set has 33 bindings (0-32; 32 = the dust list `kDustBuf`, whose tail holds the engine lights the sea mirrors, 2026-10-20; 30 = the clouds' flash buffer and 31 = the clouds v2 UBO, for the clouds' shadows on the air, 2026-10-10; 29 = the far cloud layer `cloudFarImg`, a storage image read with imageLoad, review 18; 28 = the major-roads storage buffer for the city lights; 27 = the terrain material array, terrain v2 P3; 26 = terrainFrameBuf, the observer's detailed ground height
  from scene_depth.comp, 2026-09-25). Before that it had 26 (0-25; 22/23 the mesh targets, 24 the env star grid, 25 the
  sharp-reflection G-buffer — the last two read only by the SKY_ENV / SKY_REFL variants). The original 22 (0-21): GlowBuf, noise, moon, earthDay, earthNight, earthElev, earthSpec (the R8G8 water map since terrain v2 P2), earthClouds (since 2026-09-29 the v2 weather CUBE), cloudNoiseTex (sampler3D), CloudParams UBO, half-res cloud march targets A/B, lightDomeBuf, milkyWayTex, cityDayDetail, cityNightDetail, auroraNoiseTex (sampler3D), reflectBeamsBuf, beamGlowDomeBuf, sceneDepthTex, oceanGlintBuf, groundBeamsBuf. Binding 18 was `cloudShadowTex` until that pass was deleted; 19/20 were compacted down into 18/19 rather than leaving a hole, since the C++ side fills its binding array contiguously. groundBeamsBuf (21, perf follow-up) is the CPU-compacted, observer-range-culled subset of reflectBeamsBuf that sat_sky.frag's ground-spot loop reads instead of the raw (up to 2048-entry) buffer — see GpuGroundBeams in SatelliteSim.h. **As of 2026-08-10 its entries are `GpuGroundBeam` (32 bytes), not raw `GpuReflectBeam`** — a pre-solved record, see "Beam ground-spot CPU hoist" below
- GPU-side observer ground height lookup added; CPU observer height also corrected (see elevation encoding below)
- `sat_sky.frag` ground path: terrain march step count is path-length-adaptive as of session 29
  (`kN` scales with this ray's own `tExit`, clamped to a user-tuned [64,164] range — the old
  altitude-only formula gave a grazing/horizon ray and a steep ray from the same observer altitude
  identical step budgets regardless of how much further the grazing ray actually travels, a real
  bug contributing to reported terrain jitter, not just under-tuned; see `TERRAIN_PLAN.md` session
  29 log) + 12-step binary search; terrain hits use gradient-computed normals; sea-level sphere
  fallback; satellites/stars depth-tested against terrain (gl_FragDepth — since Phase 4 the unified log-distance
  encoding, see "Unified scene depth"; originally close terrain → [0, 0.5),
  sky → 1.0)
- **The atmosphere loop's densities are INTEGRATED over each step** (2026-10-24, issue 20261023_230542 "Rings in the
  sky"): exp(-h / H) averaged exactly for a height linear from the step's start to its end (`atmHalfAvg3`, the start's
  exponentials carried from the last step's end: one vec3 exp a step, as before), split at the middle only for the step
  holding the ray's lowest point (a ray dipping toward the horizon from altitude), for the Rayleigh, the Mie and the "Haze
  near clouds" columns. With the default "View samples (min)" 6.5 (~15-km steps looking up) the midpoint sample of the Mie
  (1.2 km scale height) and the haze (2.5 km) was off by up to ~50x, and N_VIEW is a whole number that grows with the
  path's length: each time it stepped, every few degrees from the zenith, the haze's share jumped — concentric rings round
  the zenith, gone at 32 samples (the A/B that found it). Smaller N-dependent terms went with it: a step's in-scatter is
  attenuated to the step's MIDDLE (`odR_cam - 0.5 densR0`; by the whole step, the first ~15-km step looking up lost ~13%
  of its blue to its own Rayleigh column), and the god rays' shadow is weighted along the step by the Rayleigh (8 km) and
  the Mie + haze (1.5 km) profiles separately (`shK` / `shKMie`, geometric weights: two exps a step; it was one 3-km
  weighting, switched off past 30 km). Ring strength (high-pass of the zenith's radial profile, issue view): 0.158 ->
  0.069, the 32-sample reference 0.057 (0.047 without god rays); a faint step remains with god rays on (open). The zenith
  now carries its real Mie column (a little hazier overhead, mean +1-3 levels in the matrix stills; the horizon,
  already finely sampled, unchanged). SKY_LITE 226.8 of 230.4 KB. Its cost was NOT as before: interleaved in one app the
  sky pass was +0.5..+1.0 ms in every matrix cell (three divisions and an extra exp a step, four exps a step for the god
  rays' weights, a second inlined copy of the froxel lookups); perf pass 3 (2026-10-24, "Clouds v2" above) took it back to
  within +0.0..+0.4 ms of the midpoint version with the same image: `atmHalfAvg3(ea, eb, dh)` takes one reciprocal, the
  god-ray weights are ray constants, and the step's reads and the piece-in-front's share one loop.
- **The atmosphere loop starts where the ray ENTERS the atmosphere** (`tStart`, 2026-09-29), not at the eye:
  from orbit nearly every N_VIEW step fell in the vacuum above R_ATMOS (zero density, but each still paid the
  city-glow fetch, the trig and the airglow noise) while the few left in the air undersampled it. Knockout
  sweep at 2000 km: the loop was 13 of the sky pass's 16.6 ms. Sky pass 420 km 6.9 -> 3.3 ms, 2000 km
  16.5 -> 4.9, 10000 km 5.2 -> 2.3; images within ~1/255 (harness_runs/impostor). Every other atmosphere
  integral (cloud airlight, airglow red, godrays) already started at the entry. The same measurement showed
  a far-field cloud impostor is not worth building: the cloud march gets CHEAPER with altitude (10 ms at
  420 km, 8.5 at 2000, 4 at 10000, 2 at GEO), and at LEO a pixel is only ~1-2 km, finer than a globe-wide bake.
- Ocean wave material: specular map (binding 6) gates UBO-tunable-octave noise wave normals +
  Blinn-Phong sun glint (exp=300) + Schlick Fresnel on sea-level sphere hits
- **Volumetric clouds (C7+C8):** shell march with full C8 lighting:
  - `cloudDensity` takes two UVW args — `uvwPresence` (Z=posZ) for Perlin R threshold,
    `uvwDetail` (Z=hNorm×kVertTiles) for Worley erosion. Keeps cloud-existence horizontal only.
  - **Altitude-stratified stepping:** `stepLen = (shellThick/N) / max(abs(dir.z), 0.02)`.
    Equal altitude per step regardless of ray angle — no oblique-angle slab artifacts.
  - **Spectral sun color:** `sunColorCloud` from `optDepth` at shell entry → orange/red at sunset.
    Night-side gated by Earth shadow test. Replaces old gray `vec3(1.0)` lighting.
  - **Night darkening:** ambient transitions from blue day dome to near-zero at night using
    per-sample `dot(normalize(pECEF), sunDirECEF)` geographic terminator check.
  - **City upwelling:** `earthNightTex` at mip 3 contributes warm orange into cloud bases at night.
- **Aurora (C16):** visual design centered on the geomagnetic pole (`kGeomagPoleECEF`, antipodal
  dipole model covers both hemispheres with one constant), colatitude oval band (`auroraOvalMask`)
  with ripple-warped centerline, anisotropic curtain-fold noise (`auroraCurtainNoise` — high freq
  along azimuth for many separate folds, low freq along colatitude so each fold reads as a long
  streak, not a blob). Day-gated PER-SAMPLE on that sample's own geographic day/night (mirrors
  airglowRed's `rDayness`/`rNight`). `CloudParams` grew 288→304 bytes for `stormStrength` +
  `auroraGain` (mirrored in `cloud_march.comp`, which hand-duplicates this UBO layout, and in
  `GpuCloudParams`). `kAuroraScale = 0.000001`, same order as `kAirglowScale`. Visual
  design/tuning **DONE, closed 2026-07-16 (session 28 follow-up #22) after 22 follow-up rounds**
  — brightness/exposure, per-sample day/night gating, fold axis/unit calibration, cloud occlusion
  depth-ordering, terrain/ocean/cloud ambient lighting + ocean reflection glint, atmospheric
  extinction, light-pollution/moonlight suppression, a sigmoid-based airglow blend at both shell
  edges, per-column elevation variation, and organic domain-warped shimmer evolution. See
  `TERRAIN_PLAN.md` session 28 log (all 22 follow-ups) for the full design and bug history.
  **Architecture changed significantly in session 29 for performance** (a ~40fps-swing cost down
  to a minor one) — read this before touching aurora code:
  - The sky curtain march itself (whole-ray bounding pre-check → adaptive-step march →
    light-pollution/moonlight suppression → extinction) moved OUT of `sat_sky.frag` into
    `cloud_march.comp`'s `auroraMarchCS`, running at half resolution alongside clouds/cirrus.
    Its result folds additively into the same `B_total` channel clouds already write — no new
    sampling code needed in `sat_sky.frag`.
  - `sat_sky.frag` keeps its own copies of `auroraFrame`/`auroraCoverage`/`auroraOvalMask`/
    `auroraCurtainNoise`/`auroraSampleAt` — still used by `auroraGlowAt` (terrain/ocean ambient
    lighting) and the ocean sky-reflection's own aurora sample, both legitimately full-resolution.
    `cloud_march.comp` has near-verbatim duplicates of the same functions for its own march — keep
    both copies in sync, same standing rule as the cloud/cirrus code this file already duplicates.
  - Most of the curtain-fold and column-window noise is now baked into a texture
    (`aurora_noise.comp`, 1024×16×256 RGBA8, `createAuroraNoisePipeline` — same one-shot-bake-at-
    init pattern as `cloud_noise.comp`) instead of computed live via `warpPerlin3` every sample —
    the direct fix for "aurora is much more expensive than clouds despite looking simpler," since
    clouds' noise was already baked in a prior session and aurora's never was. New descriptor
    binding 16 (`auroraNoiseTex`, sampler3D) on `sat_sky.frag`'s set, binding 8 on
    `cloud_march.comp`'s own set (separate descriptor sets, same underlying image/sampler).
  - Real behavior change, not just perf: aurora now respects terrain occlusion the same way clouds
    do (folded into the same terrain-gated composite branch) — previously it ignored terrain
    entirely. Judged more physically correct and accepted without further tuning.
  - `kAuroraStepsMin`/`kAuroraStepsMax` are 4/64 (was 24/160) — user-tuned in-app; the min rarely
    binds (a straight-down path through the ~200km shell is already ~14 steps at the target
    resolution). See `TERRAIN_PLAN.md` session 29 log for the full four-round history (step cap →
    pre-filters → noise bake → resolution move) and the specific approximations each step accepted.
- **Aurora sheets (2026-09-30, `cloud_march.comp` auroraSheetIndex / auroraSheetLight, in
  auroraMarchSegment).** Thin emissive curtains over the diffuse volume: the level sets S = n of an index
  field (colatitude / spacing + a sum of travelling waves; where its gradient across the oval passes 1 the
  sheets fold into curls). Each ray crossing is integrated EXACTLY: emission x sigma sqrt(2 pi) / |dS/dt|
  (the path length through a Gaussian sheet, capped at two steps), so edge-on curtains are crisp ribbons at
  any resolution with no sampling noise; crossings are found between the volume march's own samples and
  refined by two regula-falsi steps on the exact index. **Review 11:** each sheet is integrated over each STEP
  with S linear in t (erf of both ends; candidates within 1.2 sigma of the step's S range, faded at the edge), and
  the march start is jittered per pixel and per frame (IGN + the TAA's Halton phase, `gAurJitter`). The first cut
  credited a whole Gaussian (capped at two steps) to the step holding a crossing and nothing otherwise: where a ray
  grazed a fold, detection flipped with the elevation, drawing stacked horizontal zigzags (user report). Without
  the jitter soft bands remain (piecewise-linear S). Cost +0.55 ms in a Coldfoot storm view (the first cut +0.21;
  3-sigma candidates +0.9). Crisp or fuzzy per sheet and per ~500 km of oval;
  a sharp lower border (pink N2 edge), rays (field-aligned striations: from the ground they converge on the
  magnetic zenith as a corona), red tops. The index is carried across EVERY sample (dropping it where the
  oval mask was 0 cut the sheets in whole-step chunks: a staircase). Folds finer than the 15-km steps alias
  (a crossing and its return inside one step are missed): the 71x wave does not scale with "Sheet folds",
  capped at 2. Cost +0.55 ms of cloud march in a storm. Settings (Atmosphere tab, aurora): "Curtain sheets"
  `clouds.aurora_sheets` 1.4, "Sheet spacing (deg)" 0.34, "Crisp sheets (share)" 0.77, "Sheet folds" 1.1 (2026-10-20; slots
  215-218, UBO `auroraSheets`). The aurora's light on the ground (terrain and sea) was REMOVED the same day
  (the user: green and bad); `auroraContribTerrain` stays as a zero term. Test from a DARK site (Coldfoot,
  67.25 N 150.18 W, December): Fairbanks' skyglow gates the aurora out. `scripts/aurora_sheets.satcmd`.
- **The auroral oval + space weather (2026-10-03, `include/aurora_oval.glsl`, `src/simulations/SpaceWeather.h/.cpp`).**
  The oval is a RING fixed relative to the SUN: magnetic local time from `cloud.auroraMidnight` (the anti-Sun
  direction projected on the dipole's equatorial plane), widest and furthest equatorward at ~23 MLT, thin at noon,
  the polar cap inside it dark; a sharp poleward edge (discrete arcs) and a soft equatorward one (diffuse aurora;
  the sheets carry a quarter of their light there, `auroraDiscreteShare`). Its edges are Feldstein-Starkov-like
  functions of an activity index Kp (`auroraOvalBounds`: midnight equatorward edge 66.5 - 2.1 Kp MLAT, Kp 9 ->
  ~47), brightness 0.4 + 0.2 Kp (1 at Kp 3). It replaced a band at 20 deg colatitude (+8 x storm) that faded
  out over TWICE its width: at the default storm it was lit from ~5 deg off the pole, a cap. **Kp is a pure
  function of sim time** (`spaceWeatherAt`, CPU double, each frame into the UBO's auroraMidnight / auroraOval /
  auroraSub / auroraOval2, 864 -> 928): the 11-year cycle (cycle 25 min Dec 2019), recurrent coronal-hole
  streams (27.27 d, a hole lives ~6 rotations), CME storms (a daily Poisson chance x cycle x the Russell-McPherron
  equinox bias; sudden commencement, a 3-9 h main phase, a 0.5-1.3 d recovery; peak Kp from knots fitted to
  NOAA's per-cycle counts — measured 936 / 501 / 174 / 103 / 9 three-hour intervals at G1-G5 against NOAA's
  1700 / 600 / 200 / 100 / 4), and substorms (2.5-h bins, ~2.5 a day, more with activity: onset near 23 MLT,
  a bulge pushed 2-6.5 deg poleward that widens and drifts west, brighter and filled in, ~2 h). Time reversal,
  warp and bookmarks see the same storm. `cloud.stormStrength` (fold chaos, coverage fill) is now DERIVED:
  (Kp - 1) / 9, max 0.85 (at 1 the coverage gate filled the oval). Atmosphere tab: "AURORA NOW" readout,
  "Space weather (0 manual / 1 auto)" (slot 258), "Aurora Kp (manual)" (slot 25, was "Storm strength"),
  "Storm frequency (x)" (slot 259); keys `clouds.aurora_activity_auto / aurora_kp_manual / aurora_storm_rate`
  (`storm_strength` is no longer read). The ambience's `aurora` driver uses the CPU mirror
  (`auroraOvalWeightCpu`). Harness `aurora` (docs/HARNESS.md). Open: a storm from orbit is a filled ring of
  concentric bands (the coverage noise varies with colatitude by design); the noise azimuth is still Earth-fixed
  while the oval is Sun-fixed; no dawn-side omega bands / pulsating aurora; the dipole axis is fixed (no IGRF).
- **Aurora storms + a physical sky gate (2026-10-14, issue 20261014_193231; `cloud_march.comp`, `include/darksky.glsl`,
  UBO `auroraStorm` / `auroraSky`, 928 -> 960; Atmosphere tab slots 298-301, keys `clouds.aurora_storm_features` 1.2 /
  `aurora_blue_rays` 0.004 / `aurora_arc_systems` 1 (0-1; was 2 past its slider until 2026-10-21, the coverage threshold remapped so 1 is that look) / `aurora_storm_red` 1.1; defaults since 2026-10-20).**
  - **The gate (the bug):** a Kp 9 storm was invisible from an Iowa farm and from Arizona and was cut off along a hard
    azimuth toward a town. The aurora read the Milky Way's dark-sky dome: a LOG ramp from 1e-6 (a raw glow of 0.01 is
    already 83% "city"; a sector of 0 beside one of 0.83 = the hard edge; populated farmland = 1.0 in every sector,
    harness `state` -> `light_dome`) toward an artistic 13 mag/arcsec^2. It now takes `darkSkySkyMagLinear`: skyglow is
    ADDED light, sky = pristine x (1 + d / 0.0241) with d the dome's compression undone (d / (d + 0.08) ->
    d), so a downtown core reads ~18 (Bortle 8-9), rural Arizona / Iowa ~21.3, Fairbanks ~20; and the aurora's
    brightness scale is the physical 2.5 mag per decade (was 0.85: every part of a display went at once). Still 1 under
    a pristine sky. **Since 2026-10-28 the sky is the skyglow model** ("Light Pollution Dome"), shared with the Milky Way
    and zodiacal light (`darkSkySkyMagLinear` = `darkSkySkyMag`): Iowa farm 21.7, Arizona 21.0, BC 21.7 mag/arcsec^2.
  - **Arc systems** (`auroraArcWeight`): sheets in groups of 5, ~70% of groups an arc system bright over 1-2 stretches
    of the oval (thousands of km), mean ~1, bright parts ~6x; the diffuse volume follows at 0.6. The storm coverage
    no longer fills the oval (-0.35 instead of -0.6 at "Arc systems" 1; both copies of `auroraCoverage`). A first cut
    (55% of groups, shorter stretches, coverage -0.2) left a Kp 9 sky over Coldfoot almost empty.
  - **Storm look** (`auroraStormHi` = smoothstep(Kp 5, 8.5)): a share of sheets rising with the storm are TALL (decay
    190 km, red from ~35-105 km above the border, stronger rays: red pillars), bright sheets get a magenta lower border
    9 km lower and magenta between green and red; sunlit tops add N2+ blue. **Storm red** (`auroraStormRed`): 630-nm
    glow 290-380 km up, up to 7 deg equatorward of the green edge (CPU `auroraGpuParams` widens `auroraOval2.y`), patchy,
    faint from orbit, a red northern sky from 41 deg MLAT at Kp 9. **Blue rays** (rare, like sprites): a ~100-km stretch of a
    sheet (one per ~220-km cell at the chance x smoothstep(Kp 4, 7)) whose own rays stand to 350-650 km, soft-ended,
    tapering, blue where sunlit, the sheet's dim red otherwise. A first cut (one ~10-km flat ray, magenta unlit) drew
    isolated straight lines from orbit (shots 7, 9). The march's top rises 410 -> 700 km with the storm while any of
    these is on, and every sheet fades out over the top 35% of the march's reach above 100 km (`gAurTopM`): from
    590 km the red pillars ended on a hard ceiling where lines of sight left the 700-km shell (shot 8).
  - Cost: within noise (Kp 9 at Coldfoot 13.79 vs 13.85 ms with all four off). Harness scenes: `harness_runs/aur14`.
    Open: from orbit the oval still shows the volume's concentric fold rings; blue rays are untested in sunlight
    (no dusk view framed one yet); how it looks is the user's to judge.
- **Next:** C14 (Anvil) remains not started — deferred repeatedly in favor of C15/C16 per the
  2026-07-12 session — and can be picked up whenever; it has no dependency on C15/C16. Otherwise
  Phase E is complete (C13, C15, C16 done); C9/C11/C12 and noise-repetition cleanup are next in
  line per the 2026-07-12 planning session's priority order. Resolution scaling shipped in session
  29 (background-only, satellites/stars/UI always native res) — see "Subsystem: Resolution
  Scaling" below.

### Procedural terrain detail (2026-09-25) — `shaders/include/terrain_detail.glsl`

Built overnight with the automation harness (docs/HARNESS.md) as its first real client; the
`erosion` branch's approach (a one-Newton-step displacement at the smooth hit + a normal perturbation)
was studied and not merged — its own notes say a real 3D surface was the goal and could not be
reached by patching the bisection. This is that surface. Read the header comment of
terrain_detail.glsl first; invariants and the reasons behind them:

- **The surface is `H = DEM + D`, and every pass that needs "where is the ground" uses the same
  function and the same march** (`terrainMarchDetailed`): sat_sky.frag, scene_depth.comp,
  terrain_probe.comp. D is eight octaves of 3D value noise (2048 m .. 16 m cells) on the sea-level
  sphere, slope-damped (IQ-style erosion look), amplitude = `terrainDetailAmpM` x roughness (the DEM's
  SIGNED relief against mip 3 — ridges rugged, valley floors smooth; |relief| dug pits into every
  valley floor) x coast fade. The geometry LOD (`tdGeomLodM` = max(2 px, 1.2% of t)) depends on t
  alone at any normal FOV, which is what makes the half-res and full-res passes see the SAME surface.
- **Never form an absolute ECEF coordinate in float.** The lattice is anchored to the observer's
  sea-level point as an integer 2048-m cell + offset (CPU double -> `terrainAnchorCell/Rel`); every
  height uses the observer-relative `q` (`tdAltitude`, `tdSphereOffset`). The erosion branch fought
  exactly this precision loss (stretched/swimming noise).
- **A seed pyramid: quarter-res -> half-res -> full-res.** scene_depth.comp runs twice: a
  quarter-res pre-pass (`quarterPass`, its own descriptor set `sceneDepthQDescSet`, writes
  `sceneDepthQImg` and `terrainFrameBuf`) marches the true surface from the eye; the half-res pass
  starts from the nearest of its 2x2 texels (x0.995) or skips rays where all four were sky; sat_sky.frag
  does the same from the half-res result and usually resolves in 1-5 steps + a 3-step regula falsi.
  Conservative at every level because each coarser pass's pixel-footprint test is wider, so it stops
  EARLY and counts near misses of a crest as hits. Each set's binding 7 samples the OTHER image (so
  neither is read in the layout it is being written in). The first cuts (full-res march from the eye;
  an envelope depth) cost 10-25 ms at ground level; measured with `perf` + `debugview steps`.
- **A step past the exit lands ON it** (`atExit`, 2026-09-29): the step floor (1.5-5% of t) is ~1 km at
  70 km, and over low land a steep ray's last step jumped from above the ground to past the sea sphere:
  a MISS, drawn as flat sea-level land in rings about the nadir (user snapshots 2, 8 — found with
  `probe`: "from eye -1 m (34 steps)" alternating with hits). The loop now clamps once to tExit, so the
  crossing is bracketed. Shared with the depth passes.
- **Past the march's range, land sits at its DEM height** (sat_sky.frag, two re-intersections of the
  sphere R + h): on the sea sphere its air column was the whole atmosphere to 0 m — from orbit the
  Tibetan plateau was clear inside the range and white-veiled past it (user snapshot 1's disc).
- **Out of budget is not a miss**: the march finishes on a coarse-octave tail. Returning -1 made the
  depth say "sky" and the sky pass skip the pixel — holes through distant hills (found with `probe`).
- **The observer's ground includes the detail** (`observerEffHeightDetailed`), computed once by
  scene_depth.comp into `terrainFrameBuf` (sky binding 26) and per texel by cloud_march.comp.
  beam_self_march.comp still takes the CPU's obsEffH (pre-existing), and the mesh shaders use the
  DEM-only `observerEffHeight` — both off by at most the detail height.
- **Shading**: detail normals at a 3 px LOD (finer aliases into paper-like grain), plus shading-only
  micro relief (`tdMicroBump`, 8 m .. 0.25 m, gradient noise, SOLID 3D so steep faces are not
  vertical smears) and a solid albedo mottle (`tdMicro`). Materials key off the day map (its white =
  snow, rock on steep faces, darker where the map is snow); a latitude snowline painted Tibet white.
  All fade out by a ~400 m pixel footprint, so orbit views are the day map untouched. Soft terrain
  sun shadows (16 steps, 3 octaves, normal-offset start — starting on the surface gave texel-sized
  acne, and lifted onto the coarse surface the ray is tested against) with a 15% bounce floor. Not marched where the
  clouds' shadow already takes the direct sun (cloudShadowT < 1e-3; every use is x cloudShadowT): ~2 ms of a clear
  ground view's sky pass, nothing under an overcast (2026-10-24).
- **Lighting of faces turned from the Sun** (found flying the harness into a glacier, where half
  the metre-scale facets of a snowfield went pure black under a 20-degree Sun):
  - `dayFrac` is the GEOGRAPHIC horizon gate only. It was also gated by the shading normal, which
    sent every face turned > ~8 degrees past edge-on to the Sun to the NIGHT branch: no skylight,
    and the city-lights map, in daylight. DEM normals seldom got there; detail normals constantly do.
  - The direct term's 0.05 floor is reached smoothly (`sunLit`); its hard clamp drew contour rims.
  - A ground bounce proportional to the day map's luminance and the Sun's local height
    (`bounceK`, scaled by the material strength): snow lifts its shaded faces, forest barely moves.
  - `tdMicroBump` is 0.3x on day-map snow, and its floor on smooth ground is 0.25 (was 0.35).
- **Erosion octaves (`tdErosion`, 2026-09-25)** — the `erosion` branch's algorithm (clayjohn 2018 /
  Fewes 2023 "eroded terrain noise": per-cell cosine stripes whose phase runs across the slope, so
  gullies run downhill, each octave following the slope plus the octaves before it = branching),
  made part of the marched surface: two octaves (512, 256 m) between the coarse and fine value
  octaves, amplitude `terrainErosion.x` x octave-0 amplitude x a slope factor, bounded, so the march
  stays conservative (`tdErosionBound`, `tdRemainingBound`). Sliders "Erosion strength" (0.6) /
  "Erosion branching" (1.0), keys `clouds.terrain_erosion_*`; `debugview erosion`.
  - **Domain**: the ECEF plane perpendicular to the dominant axis of up (drop one coordinate — the
    anchor's integer cell carries over exactly, so nothing swims), blended across cube-face edges.
    Stripes vary across the PROJECTED slope, so grooves run exactly downhill whatever the projection
    stretch. The Alps sit on the x/z face edge — views there pay for two faces in a narrow band.
  - **Kernel**: compact (1 - d^2/R^2)^3, R = 1.25 cells, points jittered within the middle half of
    their cell, so the 3x3 neighbourhood is exact (no cell-border seams); per-point frequency
    (+-25%) — one frequency read as regular ripples on a far slope; the erosion needs a cell of 4x the
    LOD for full weight (twice the value octaves' margin) for the same reason.
  - **Steering**: the DEM slope by central differences over +-1 texel (continuous across texel
    borders) plus the coarse octaves' gradient. Where that direction flips (crests, valley floors)
    the stripes crowd into fringes — visible in `debugview erosion`, faint or hidden by haze in the
    render; steering by the smoothest slope only removed them but also most of the effect (tried).
  - **Cost is what shaped the code.** GLSL inlines everything and the registers are sized for the
    largest path, so erosion code that never runs still slowed every terrain pixel: a first cut with
    the erosion inside the octave loop (copied into every unrolled iteration), two face-evaluation
    sites and three full-height sites in the march made the ground views ~50% slower with the
    erosion switched OFF. Now: one erosion call site outside the value-octave loop (`tdOctaves` =
    value octaves, erosion, value octaves; `allowEro` literal false on coarse-only paths, which use
    `terrainHeightCoarse`), one face site, `[[dont_unroll]]` loops (GL_EXT_control_flow_attributes in
    every shader including terrain_detail.glsl). The march's refinement and the shading normal take
    the erosion as the plane the last full evaluation left (`terrainHeightLinEro`,
    `terrainDetailLinEro`, `gTdHitEro`) — the gullies are flat to centimetres over a bracket. A
    per-cell cull (skip cells out of the kernel's reach) was SLOWER (divergence), a third 128-m
    octave cost ~1 ms for little. The terrain shadow's lift reuses the normal's coarse height (`hC3`).
  - **Measured** (RTX 3070 Ti, 1600x900, High, harness perf): +2.5-3 ms on the ground in mountains
    with clouds off (v1 13.0, v4 13.7 ms), ~0 from 10 km up; Grand Canyon rim with clouds at High
    15.8 ms (p90 16.4) vs 13.0 without, at Medium 12.8 vs 10.7.
- **"Erosion size" (review 6, `clouds.terrain_erosion_size`, slot 211):** the erosion octaves' cells
  x 0.5 / 1 / 2 / 4 (256-m .. 2048-m first cell), powers of two only so the lattice stays exact on the
  2048-m anchor cells (`tdErosionK`, `tdEroAnchor`: cells past 2048 m take the anchor's remainder into
  the lattice coordinate). Carried in `envMainObsDir.w` (log2 of the factor; the w was unused). On
  Fuji (a smooth cone in the DEM) the lumps are mostly the erosion, the rest the value octaves: erosion
  0 leaves soft bumps, "Detail height" 0 the clean cone.
- **Empty-space skipping was tried and removed**: a CPU-built max-mip chain of the DEM, tested in
  the depth pass where the ray cleared the local max + the detail bound. It made the depth pass
  SLOWER at every altitude (v4 3.3 -> 4.5 ms): the rays that cost are the ones just above the
  horizon, and those are within the bound of the local max for tens of km, so the extra fetches on
  every step bought almost no skipped steps. (Its upload also taught that Vulkan mip sizes round
  DOWN — the validation layer via VK_INSTANCE_LAYERS caught it; see docs/HARNESS.md.)
- **The DEM is sampled exactly near the observer** (`tdDemTexel`/`tdDemBilinear`, within 4 km):
  the float UV resolves ~2.4 m and hardware bilinear of R8 uses 8-bit sub-texel weights; together
  they built metre-high shelves on a steep wall seen from 10 m. The CPU passes the observer's texel
  (integer + fraction, `terrainObsTexel`); each point adds a cancellation-free lon/lat offset.
- **Cost (RTX 3070 Ti, 1600x900, clouds off, harness `perf`, final)**: +4-6 ms at High on the ground
  in mountains (worst total 9.7 ms vs ~4 ms without detail: depth passes 1.5-2.1 ms, sky 4-5 ms incl.
  1-2 ms shadows), ~+1 ms from aircraft, ~0 from orbit; +3 ms at Medium in the Anchorage worst
  case (measured before the quarter pass; less now). Presets: on for
  Medium/High/Ultra, off for Low/Planetarium/Potato (`applyGraphicsPreset`). Terrain tab sliders:
  Terrain detail / Detail height / roughness / erosion (the value octaves' slope damping) / Terrain
  shadows / Terrain materials / Erosion strength / Erosion branching (the gully octaves).
- **Tools**: `tools/harness/scripts/terrain_views.satcmd` (eight golden views), harness `debugview`
  (normals, detail, steps, albedo, shadow, rough, elevzebra, distzebra, erosion) and `probe x y`.
- **Known limits**: the ground within ~20 m is soft (it would need real texture maps); silhouettes
  are not antialiased; value noise shapes; the DEM itself is 2.67 km / 8-bit (the Everest region is a
  smooth plateau in it); SKY_LITE and SKY_ENV draw the plain DEM (`tdEnabled()`).
- **Night and ambient light (terrain v2 P1, 2026-09-29, `.plans/TERRAIN_V2_PLAN.md`):** the night map
  is a Black Marble composite with a BLUE base under every texel (land sRGB ~(12,13,25)); used as the
  terrain's emission it out-shone full-moon ground and made the night side a flat blue 5 km texture.
  The terrain now takes only the LIGHTS from it (`cityLights` = night map - linear (0.006, 0.006,
  0.0132), a plain subtraction — a knee on the filtered value drew every texel as a hard square from
  orbit), and the night surface is the day albedo lit by the Moon, the moonlit sky (0.15 of the direct
  Moon) and the moonless night sky ("Night sky light", `clouds.terrain_night_sky_light`, 0.25 of the
  full Moon overhead: the relief reads, dark). The sky ambient takes a sky-view factor (0.5 + 0.5 n.up)
  and a gain ("Terrain sky light", `clouds.terrain_sky_light`, 1). Both ride in `terrainErosion.zw`.
  Only the terrain emission changed: the dome, ambience and cloud city glow read the night map with
  their own response curves; Potato (`sat_sky_minimal.frag`) still draws the raw night map. Harness:
  `debugview nightsky` / `citylights`, `scripts/terrain_night.satcmd`. **`clouds.coverage` is the
  retired v1's — clear the volumetric clouds with `clouds_v2.coverage 0`** (a night LA view "without
  lights" was an overcast).
- **Water: lakes at their own level, shores from a distance field (terrain v2 P2, 2026-09-29).**
  Binding 6 ("earthSpecTex" everywhere) is now the WATER MAP, R8G8, baked by
  `tools/make_water_map.py` (scipy) into `assets/textures/earth_water_sdf.r8` + `earth_water_level.r8`
  and interleaved at load: R = a smoothed signed distance to the shore (0.5 + d / 2 x 19.5 km, d > 0
  in water — `r > 0.5` is still "water" for every old consumer), G = the level of the NEAREST water
  body in DEM units (each connected body's median DEM over its interior; the DEM is flat over a lake).
  The shared height function (terrain.glsl `waterAdjustHeight`, called from `tdDemAt`): a water texel
  is its body's flat level, marked `hMip3 = kTdWaterMark` (tdAmp0 0 there, and a hit tests it); land
  within 3 km is banked up to at least the level and within 1.5-3 km capped by a 0.25 m/m ramp from
  the shore (the DEM's 35-m land baseline was a step at every coast; the cap must let go, or it
  flattens mountains rising from lakes); `tdShoreOffset` moves the shoreline by one octave of
  anchored 1024-m value noise (+-700 m: coves, headlands, sea stacks at Big Sur). `sat_sky.frag`: a
  hit on a lake takes the ocean path at that level (`tSeaLvl` = the hit, `waterLevelM`), and a ray
  that fell through to the sea sphere re-tests water at that point with `tdDemAt` (`waterPx`) —
  most sea pixels come that way, and the raw map lacks the coves. Before this, water was drawn only
  where the DEM was < 160 m, at sea level: Superior, Victoria, Titicaca, Baikal were blue LAND.
  **Cost**: the map is fetched only where its mip 2 is non-zero (within ~15 km of water); in-app A/B
  (knockout bit 4194304 `water_map`, `scripts/terrain_water_perf.satcmd`): Geneva +0.5 ms, Big Sur
  from 1.5 km +0.7 ms, inland 0. The first cut fetched it on every height evaluation (+1.7 ms in the
  Alps) and a second noise octave cost +0.3-0.4 ms. The source mask's last row was all water (a
  3600-m "lake" round the South Pole) — the bake copies the row above. Re-run the bake after editing
  the mask or the DEM. Harness scene: `scripts/terrain_water.satcmd`.
- **The biplanar weights have a floor** (`tmSample`, +0.02): near the cube diagonal (|n| components all
  ~0.58, central Japan) both were ~0 and the clamped normalisation returned black material blobs and
  flat-lit relief (user snapshot 7).
- **Close-up material textures (terrain v2 P3, 2026-09-29).** Sky binding 27 (`terrainMatTex`,
  `sampler2DArray`, the stage's 16th and last sampled image): six CC0 ambientCG sets (grass, forest
  floor, rock, snow, sand, dirt) baked by `tools/make_terrain_materials.py` into
  `assets/textures/terrain_materials.rgba8` (raw, "SLTA"; mips box-filtered at load by
  `createTerrainMaterials`, a 1x1 neutral array without the file). Layer 2m = the albedo as a linear
  RATIO to the set's own mean x 0.4 + height; 2m+1 = normal XY + roughness + AO. In the hit block
  (after tdMicroBump): the day map picks the material (green = grass, dark green = forest floor,
  bright unvegetated = sand, else dirt; steep = rock; its white = snow), the two strongest are sampled
  biplanar in ECEF on the anchored lattice at 4-m tiles (2048 is a multiple, so world-fixed) with an
  explicit LOD from the footprint (no derivatives in the divergent branch), blended by height, and a
  16-m read of the main one multiplied in against tiling; the ratio multiplies the day colour (so the
  hue at distance is unchanged), the normal perturbs the shading normal, AO joins terrainAO. Fades in
  below 0.6-3 m per pixel. "Close-up textures" (`clouds.terrain_texture_strength`, the old
  `terrainPad0`, slot 57; on for Medium and up). Cost +0.27 ms on an Alpine meadow at 2 m (in-app
  A/B). Harness: `scripts/terrain_materials.satcmd`.
- **Procedural city lights (2026-09-29, `.plans/CITIES_PLAN.md` phase 1).** `cityLightPattern()` in
  sat_sky.frag MULTIPLIES the night map's lights (after the blue base is removed) on land at night.
  Layout: jittered-Voronoi districts of 4096 m (2 anchor cells, integer-anchored, on the erosion's face
  projection; an arterial runs along the face seam), but ONE street grid per region of 4x4 districts
  (angle, base spacing 90-120 m, phase and arterial interval measured from the region's origin), so
  streets run straight through district borders — districts differ only in which streets exist
  (suburbs every other street and more drop-outs); a fifth of districts lay out their own grid
  (T-junctions, organic warps). Light is LAMP POSTS, not glowing streets (user, 2026-09-29): posts
  every 35 m (30 on arterials, 3.5x brighter), each a pool on the road (sigma 6 m) + a small head
  (1.2 m, 30%), per-post brightness 0.5-1.5x and one in ten dark; they stay separate dots until the
  pool is wider than ~0.4 of the spacing, then merge into lines, then a uniform glow (normalised to the
  expected lamp density, mean 1). Macro: commercial strips along arterials, soft dark voids, a 2-km
  value noise; sodium / LED tint over ~8 km. The REAL major roads (`cityRoadLight`: Natural Earth 10m,
  647k segments baked by `tools/make_city_roads.py` into `assets/textures/city_roads.bin`, sky binding
  28 storage buffer, a 0.25-degree lat/lon cell list; distances in the anchor cell's frame, where a
  float ECEF endpoint minus the 2048-m-multiple cell origin is exact) are dotted the same way (posts
  every 40 m). The mean ramps from 0.12 of the map close up (the old tiled detail texture averaged ~0.1
  of it: a city core at the night exposure is otherwise a lit sheet) to 1 at a 350-m footprint, and
  the pattern stops at 400 m, so ORBIT IS BIT-IDENTICAL (checked: Europe from 400 km). Also on land at
  sea level (`waterPx == 0`, most of the LA basin). Knobs: "City street lights"
  (`clouds.city_lights_strength`, slot 58; 0 = the old detail texture) and "Major road lights"
  (`clouds.city_roads_strength`, slot 77), in the UBO where `oceanGlintPad0/1` were (renamed in place).
  Cost +0.8 ms over LA from 10 km (in-app A/B). Harness: `scripts/city_lights.satcmd`.
  **Not gated on terrain detail (2026-10-04):** Low and Planetarium (SKY_LITE) draw the full pattern too (it was
  `tdEnabled()`-gated, so those tiers showed the night map's blobs): +0.7 ms on Low, +2.4 ms on Planetarium over LA
  from 1.5 km (RTX 3070 Ti). Farmland and beaches still follow the terrain detail. **Planetarium no longer does
  (2026-10-06):** the pattern is compiled out of SKY_LITE with the rest of the v1.2 ground features (the tier is
  for GPUs where the compiled size is the cost); its cities are the night map + the tiled detail textures again.
  **Day side (phase 2, 2026-09-29):** the same layout (`cityLayout()`, a `CityLayout` struct computed
  ONCE per pixel before the material block and reused by the night) drives `cityDayAlbedo()`: asphalt
  streets (12 m, arterials 24 m, exact box-filter coverage `cityBoxCover`), each base block split into
  2-4 lots per side, roofs with their own margins and offsets from a palette (muted terracotta in
  "warm" regions), commercial blocks (1-2 big flat-roofed buildings) along the arterials — where the
  night's bright strips are — houses between, yards lawn / paved / bare, a tree canopy (16 m / 8 m value
  noise, ~70% of suburban yards, ~30% in cores), parks in the voids. It returns a RATIO to the
  pattern's own expected mean that multiplies the day map (like the terrain textures), converging to
  exactly 1 once unresolved, so the hand-off is seamless and orbit stays bit-identical. Weighted by the
  city presence (the night map's lights); the tiled city day texture and the natural close-up textures
  step aside inside cities. First cuts: equal lots / centred square roofs / grey yards / no trees read
  as a board game; a density-only chance of big buildings made half of LA warehouses. +0.55 ms at LA
  from 2 km. Harness: `scripts/city_day.satcmd`.
  **Follow-up (2026-09-29, user review):** trees are individual CROWNS (a jittered tree per 8-m cell,
  2.5-6.5 m, own shade, darker edge, clustered by a 64-m field) — two octaves of value noise were
  same-sized blobs the user saw repeating. Night lamps each have their own type (`cityLampColor`:
  sodium / warm LED / cool LED / halide; LED share 0.25 suburbs .. 0.85 cores, +0.25 on arterials, a
  regional bias), rare neon signs on arterials, traffic lights at arterial crossings cycling with
  `pc.waveTime`; the faintest-lit countryside drew farmsteads (512-m lattice; removed in review 2) not street grids.
  **Farmland** (`farmDayAlbedo`, same ratio-to-mean scheme, stops at a 350-m footprint so orbit is
  bit-identical): where the day map is cultivated (not dark forest, desert, snow, steep, a city, >3 km
  up). Style per ~130-km area: GRID in the Americas (1024-m sections split into 1-4 fields, 10-m gravel
  roads, centre pivots with probability ~dryness^2) or PATCHWORK elsewhere (Voronoi fields stretched
  2.2:1 along the area's direction, hedgerows where green). EVERY rotated lattice is measured from the
  AREA origin (`big * 32 - dAnc`, fixed), never the district origin — that would jump as the observer
  moves; unrotated lattices must divide 4096 m. `cityFrame()` is the shared world-fixed 2D frame.
  +0.35 ms in rural Iowa. Harness: `scripts/rural.satcmd` (rural points: pick them from the night map —
  the first "Iowa" and "Punjab" points were Ames and Ludhiana).
  **Continuity + transition (2026-09-29, user review 3):** nothing in the layout may jump at a district
  border any more. Density is the night map AT THE PIXEL (it was sampled per district), street existence
  is hashed per REGION with even/odd keep probabilities falling with density (no stride switch), lamp-type
  and terracotta biases come from a 4-km value noise (not per-region hashes). Where two DIFFERENT grids
  meet (another region, or an independent district: `CityLayout::differ`) the NIGHT evaluates both grids
  (`cityNightGrid`) and cross-fades them over 80 m (lights add); the DAY does not (an 80-m blend of two
  street grids read as a ghosted double exposure) — it draws a 14-m road along the border instead.
  Low-density blocks become crop fields (`cityFieldP`, up to 80%), and the farm pass cross-fades with the
  same city-presence curve, so cities thin into farmland. Farmland at night got a farmstead FLOOR (removed in review 2)
  (`farmsteadLights` x a map-equivalent 0.004, faded out by a 350-m footprint): the map is ~0 there.
  Farms keep ~0.5 km back from shores and off water-tinted map texels (a lavender field band at Big Sur).
  **Beaches** (`beachAt`): sand 40-140 m wide on low (< ~6 m above the water level), gentle shores, from
  the height function's own shoreline (hand-filtered water map near the observer + `tdShoreOffset`), wet
  near the water; the close-up sand texture there. Harness: `scripts/quickwins.satcmd`.
  **Night v2 — points, not lines (2026-09-29, the user's reference photos).** Averaging an unresolved
  light over its pixel is right in energy but wrong on screen: a real light saturates its own pixel and
  a distant city GLITTERS; the posts-to-lines-to-uniform filtering made far cities flat yellow and the
  street grids too distinct. Now `cityGlitter()`: a world-fixed lattice of 8 m x 2^k cells tracking the
  footprint (cell ~4 footprints, two levels cross-faded, 2x2 nearest cells), a point in 40% of cells
  carrying the cell's light (lognormal weight, mean 1) drawn at ~a pixel, coloured sodium / LED mostly
  and 14% signage (green, blue, red, violet), with a slow sim-time scintillation past ~15 m a pixel.
  Street posts only close up (< ~10 m a pixel: 55% posts, 45% glitter); beyond, arterials only (15%, the
  reference's streaks) + glitter 85%. The night's two-grid border cross-fade is GONE (it doubled the
  cost there; with the streets this faint the borders do not read). Steep ground is 4% lit (the map's
  blur lit the Sandias). Roads: the cell list is 0.125 degrees (43 MB; 0.25 put ~40 segments in an LA
  cell: 0.47 ms -> 0.2). Cost now +1.1 ms at LA 10 km by night (pattern 0.9, roads 0.2), +0.8 by day,
  +0.5 rural. Harness: `scripts/city_lights.satcmd` (incl. Mount Wilson / Sandia Crest low angles).
  **Night v3 (2026-09-29):** the glitter continues at EVERY distance (`cityLightFar`: frame + glitter
  only, past a 400-m footprint; lattice levels to 4-km cells) — the smooth 5-km map read as blobs, so
  orbit views of cities CHANGED on purpose (fields of points); the day side is still orbit-identical.
  Glitter density follows the map (`dens01` = lum / 0.25 sets how many cells hold a point: a suburb is
  fewer points at the same brightness, city edges stay crisp). Points are 0.3-m sources with a star-like
  point-spread (tight core + a 2x halo whose share grows with brightness — the user found uniform discs
  "flat"). Street posts split into HEADS (emission) and POOLS: the pools light the final ground albedo
  (city layout, textures, fields) x the shading normal's up-facing (relief, detail, micro bump) x AO,
  so the street and its texture show in each pool. Close up: posts 70%, glitter 30%.
  **City light sprites (2026-09-29, `city_sprites.comp`, "City light sprites" `clouds.city_sprite_gain`
  1, slot 200):** FAR city lights drawn as SATELLITE POINT SPRITES, so the horizon seen from the ground
  and aircraft is a packed field of crisp points where the surface can only draw the map's blobs (the
  user's intent). After sat_flare.comp, `kCitySpriteLevels` (7) dispatches of a lattice whose cell grows
  with distance (64 m x 2^k cells out to 8 km x 2^k, each cross-faded in over the last quarter of the
  level before: roughly one density ON SCREEN; the last level is 250 cells a side, to 1024 km) put a light in a cell with probability 0.6 x
  the night map's density, a few metres above the DEM (below the sea-level horizon: none), and APPEND a
  finished `GpuSatVisible` to the compact list: effectFlare = 8 x the cell's area / 64^2 at 1 km, 1/r^2,
  the air along the SLANT path (8 km scale height, 40 km sea-level visibility), scintillation, lamp
  colours only (a coloured sprite read as a beacon), warmer than the ground's mix; capped at 8 (no glare
  spikes); gone by 100-150 km of eye altitude (review 2: they were gone by 40 km, so from the
  stratosphere the far towns were the ground's blobs). The point, bloom and glare draws show them with no new
  pipeline. `kCitySpriteMax` (131072) extra slots in `satVisibleBuf` / `satVisibleIdxBuf`, near levels
  first. A city record: slot index 0xFFFFFFFF (picking skips it), `meshPx = -1` (the trail pass skips
  it: `sat_point.vert` skips it when `manualTerrainTest` is 2, the trail pass's value — 1 is the LIVE
  draw at renderScale < 1, and keying on > 0.5 hid every sprite at the Medium preset (0.85); that path's
  manual depth test takes a city light at 0.95 of its range, or the half-res ground hid most of them). The post-flare barrier covers
  `satListBuf` for INDIRECT_COMMAND_READ (the vertex count grows after the post-orbit barrier). Where the
  sprites carry the light the ground glitter keeps "Ground glitter under sprites" (`city_sprite_ground`
  0.6, slot 202, `cityParams.y`) — a point's display saturates, so sprites cannot carry a dense city's
  light per area (gain 12 recovered ~70% at 0.35). **FAR ONLY (review 2: up close they read as
  floating lanterns):** a light is drawn only where a pixel spans more than "City sprites from (m/px)"
  (`city_sprite_start_footprint_m` 25, slot 206, `cityParams.w`) of ground — the SAME stretched footprint
  the ground uses (pixAngle x t / max(|dir . up|, 0.2)), tested per light in the compute
  (`CitySpritePC::pixAng/startFootM`), and the ground's hand-off uses it too (`cFoot`); levels whose
  reach is inside 1.5x the nearest range that can qualify are not dispatched. **Not under Potato** (it skips the
  depth pass whose terrain frame they are placed in: they froze, locked to the observer), and with terrain knocked out
  (Planetarium, bit 1) they sit on the sea-level sphere the ground is drawn as (2026-10-04). **Fog hides
  them** (point cloud occlusion: an LA radiation fog of optical depth ~3 counts as opaque while the
  brighter ground glitter still shows through) — test them with `clouds_v2.fog_amount 0`. Harness
  `state` reports `satellites.visible_count` (the list with them: +36k over LA from 2 km). Cost ~0.
  **User review (2026-09-29, snapshots in harness_runs/fb10):** twinkle 10x slower and a slider ("City
  light twinkle rate" `city_twinkle_rate` 1, slot 201, `cityParams.x`: 0.5-1 rad/s x it); the coloured
  glitter share (signs, traffic lights) only below a 4-16 m footprint; lamp pools light the albedo's
  BRIGHTNESS, not its hue (green suburbs); the city's day base is the map's brightness, not its hue (a
  blue coastal Tokyo); past the march range land is tested by the water map (the city pattern stopped
  on an arc from orbit); the glitter's point-spread is windowed to 0.55 cells (a crosshatch on the 2x2
  choice's switch lines), the arterials' share fades past 40 m a pixel (a milky veil), farmsteads only
  below 30-80 m a pixel (sodium rings round towns from orbit). Farmland needs SETTLED land: the night
  map at mip 5 (~160 km, base removed) 0.0015-0.008 — 0.005-0.05 over farm belts, 0 over the outback,
  the Amazon, the Congo, which the colour test alone drew as fields.
  **Review 2 (2026-09-29, harness_runs/fb11):** NO farm lights at all — the farmstead pattern and the
  farmland floor are deleted ("everybody is not simultaneously blasting their floodlights"; none beats
  few). The procedural lights take the night map's BRIGHTNESS only (the lamps give the colour): the
  map's blue base is uneven, so its subtraction left pure blue/red residue at city fringes, and the
  sparse fringe glitter (x ~25 per point) turned it into vivid coloured dots seen from orbit. **The
  glitter point is ANISOTROPIC** (`gCityViewE`, `gCityFootX`, set before the pattern): sgA along the
  view's ground direction (the stretched footprint), sgX across it (pixAngle x t) — a round point of the
  stretched size drew as a wide horizontal smear at grazing angles, the "gigantic yellow blobs" of far
  towns; now they are dots on screen, sprites or not. The fog's city glow is linear in the lights (a
  full core at 0.2), not `cityBrightness` (saturated by a small town: a tan fog slab over Denver read as
  a dust storm).
  **Review 7 (day at distance):** the tiled city day texture (`cityDayDetailTex`) comes back on the
  day side where the procedural streets stop resolving (weight 1 - strength x (1 - smoothstep(30, 120 m
  footprint))); it was off at every distance with "City street lights" on, and the procedural pattern
  averages to the map by design, so from an aircraft or 40 km a city was the map's smooth 5-km texels.
  `cityDayFar` adds km-scale structure over 80 m .. 4 km footprints (parks on the near layout's void
  field, bright roof zones, a mottle following the footprint; a ratio of mean ~1).
  **Farms (review 8, `farmDayAlbedo`):** rectangular fields from a recursive split of blocks
  (`farmRects`), styles per ~130-km area by region: GRID (the Americas, Australia, some steppe: mile
  sections, quarters, gravel roads, centre pivots where dry, crop rows), STRIPS (Europe, Africa, the Middle
  East, dry Asia: 300-700 m blocks split into strips, orientation per 2-km cell with a track on its edge),
  PADDIES (monsoon Asia where green: small bunded plots, a regional share flooded; `farmWet` adds a
  Fresnel sky reflection and sun glint in the terrain lighting). Replaced a stretched Voronoi patchwork.
  "Ground pattern range (m/px)" (`clouds.ground_pattern_range_m` 800, slot 212, UBO `groundPatternFootM`
  = v1's unread shadowMaxDistM renamed): the farms fade over 0.55-0.9 of it (was a fixed 400).
- **City lights under cloud (2026-10-10, `gCityFlat`):** the procedural pattern (glitter, posts, road dots) gives way to
  its own local mean (`meanC` / `cityLampMean`) as the cloud + fog transmittance in front of the ground falls from 0.35
  to 0.02. A glitter point is hundreds of times the map's value, so points stayed crisp through thick cloud once the
  map blur was removed (user snapshot, Anchorage from 3 km). Not a blur of the map; same total light. A peak cap was
  tried first (dots at 1x the map on dark ground still read as points).
- **City light LOD settings (review 13, Night lights tab, UBO `cityLod` / `cityLod2`, slots 221-226):** the ground
  footprints (m per pixel) where each layer ends — "Lamp posts to" `clouds.city_posts_footprint_m` 10, "Street grid
  to" `city_grid_footprint_m` 150, "Major roads to" `city_roads_footprint_m` 350 (capped by the layout), "Street
  layout to" `city_layout_footprint_m` 400 (past it `cityLightFar`; the macro and mean ramps scale with it, so the
  pattern's mean still reaches the map's there) — each fading in from ~0.4x its value, and the streets' share of
  the light near / as they fade (`city_street_share_near` 0.7, `_far` 0.15). Defaults reproduce review 12.
- **City light through cloud (review 14):** the cloud blur (`cityLightBlurLod`, by the cloud's opacity in front)
  applies to the FINISHED light (`nightBlurK`, before `tNight`), not to the map the pattern reads: the blurred map
  spread a city's light over the desert beside it and the pattern drew street grids there whenever a cloud was in
  front (west of Phoenix, the grid "turned on" with the eye inside the cloud). The user's tuned LOD values are the
  defaults (posts 27, grid 380, roads 1300, layout 2000 m/px, street share 0 near / 0.13 far, sprite gain 4,
  ground glitter under sprites 1).
  **The blur under cloud was REMOVED (2026-10-06):** at mip 8.1 (~1000 km of map per texel) by the cloud's opacity,
  from orbit every city under thin or broken cloud became a grey wash with the cloud's outline (the release
  cinematic's pull-out: "blobby" cities). The lights are attenuated by the cloud composite only; the slider and
  `clouds.city_light_blur_lod` are gone (slot 62 free; `GpuCloudParams::cityLightBlurLod` unread, written 0). The
  **air's city glow** (sat_sky.frag atmosphere loop, `accumCity`) reads the night map at mip 4 below 800 km of eye
  altitude and blends to a mip following the sample's height (log2(3 h / 4.9 km)) by 3000 km: mip 4 drew every
  town as a ~150-km disc from high orbit. Applied from 30 km up it drew 10-20 km texels as squares round towns
  from 400 km (bilinear). `scripts/city_orbit_blobs.satcmd`.
- **Regional city styles (2026-09-30, sat_sky.frag `cityStyleWeights`, `kCs*` tables).** Nine styles by
  soft continental boxes with wobbled borders: 0 North America + Oceania, 1 Latin America, 2 Mediterranean
  Europe, 3 Northern Europe, 4 Middle East / N Africa / Central Asia, 5 Sub-Saharan Africa, 6 South + SE Asia,
  7 East Asia, 8 post-Soviet. GEOMETRY is one style per 16-km region, drawn from the weights at the region's
  CENTRE (`cityRegionUp`: the face frame's absolute coordinates are dAnc x 4096 + p2), so a grid never changes
  inside a region: street spacing and range, long blocks (the odd y lines dropped: `CityGrid::oddY`), grids on
  local north (US survey, Chinese plans: the angle of north at the region centre in the face frame), organic
  warp, patchwork share, lots per side, PERIMETER courtyard blocks and SLAB rows — by DISTRICT (`distH`;
  scattered single rings read as picture frames). COLOUR blends per pixel: roof shares (terracotta, light flat,
  blue-green metal, slate + the generic mix, `cityRoofS`), bare / paved / trees, lamp LED share and cool-white
  share (`gCityCool`), coloured-sign multiplier (`gCitySign`). **The day ratio is taken against the pattern
  mean's LUMINANCE where the base is greyed** (`cityDayAlbedo(..., grey)`, `cityDayFar(..., grey)`): per
  channel, the lawn-green mean turned neutral asphalt magenta on the grey base (purple streets at ground level).
  `scripts/city_styles.satcmd`.
- **Terrain-following city limits (2026-09-30, terrain.glsl `cityTerrainLimit`).** Lights and built-up land end
  at a CONTOUR 150-480 m (brighter cores higher, wobbled +-90 m) above the ~40 km mean ground (DEM mip 4.5), and
  none above ~4.5 km, besides the old slope cut; shared by sat_sky.frag (surface: `cityLights` and `cityLum`
  scaled before the layout) and city_sprites.comp (the far points). Rio's peaks and the ranges behind Hong Kong
  and Salt Lake City go dark; the night map's 5-km blur no longer climbs mountainsides.
- **Land at 0 m is terrain (review 9):** a march that ends on the sea sphere over LAND (the water map's
  test at that point) is a terrain hit there, and only water voids a hit (`hitWater`). Land can read 0 m
  (the Lena delta: DEM 0, the water map land), and whether the march landed on the sphere or passed it was
  step luck — terrain shading or the flat sea-level land path: rings about the nadir and z-fighting.
- **Ocean waves ride the world offset (review 9):** `seaMap` reads hitPt.xy + (cloud.pad1, pad2), the
  observer's cumulative east/north motion (CPU double, now from obsDir projected on the previous frame's
  east/north; it was the difference of float degrees, ~1 m steps). The old phase (float lat/lon x R,
  wrapped to the first octave's period only) let the other octaves slide against the terrain.
- Pre-existing bugs fixed on the way: the water mask forced INLAND lakes to sea level (pits under
  Lake Thun, Powell, Titicaca — now only where the DEM is < 160 m, `kWaterMaskMaxM`); terrain normals
  used 21600x10800 texel offsets on the 14999x7500 DEM; the per-pixel jittered march start was the
  salt-and-pepper silhouette speckle.

### The terrain height clipmap (2026-11-10) — `shaders/include/terrain_clip.glsl`, `terrain_clip_bake.comp`, `SatelliteSimTerrainClip.cpp`

The VALUE octaves of the procedural detail are baked into nested textures round the observer, and the three terrain
marches of a frame (scene_depth.comp's quarter and half passes, sat_sky.frag's full-resolution pass) read them instead
of evaluating the octaves at every step. `clouds.terrain_clipmap` 1 (0 = the procedural shaders; no UI). Issue
issue_20261109_184851; evidence `harness_runs/cloudsky/attr1`, `lc2` (t2_*), `tc`.

- **Why.** At ground level the procedural detail was the largest single cost of the frame, above the clouds:
  `clouds.terrain_detail_strength 0` = -7.7 ms at clear_2m (frame 21.9: sky pass 9.6 -> 5.1, scene depth 4.4 -> 1.0), -7.3 at
  term_2m, -9.9 at the Reflect-beam site (30.2); the march over the DEM alone is ~1 ms. The surface is static, and
  the marches re-derived it per step, per pass, per frame.
- **What is baked.** Level j (0..10) holds the value octaves at the geometry LOD lod_j = 1 m x 2^j: every octave whose
  cell is at least 2 lod_j, with the damping the erosion's slope adds to the fine ones (the bake runs tdValueOctaves,
  tdErosion, tdValueOctaves, as tdOctaves does, and stores the height WITHOUT the erosion's own). An octave's weight is
  smoothstep(lod, 2 lod, cell) and its damping depends only on coarser octaves, so for lod_j <= lod < 2 lod_j the height
  is exactly mix(level j + 1, level j, smoothstep(lod, 2 lod, 2 lod_j)): two reads. A (r32f, 11 layers of 1024^2, 46 MB):
  that height, read with one gather and exact bilinear weights. B (rgba16f, 8 layers from level 3, 67 MB): x = the
  first three octaves' height (the terrain shadow's surface, terrainSunShadow), yz = the coarse octaves' gradient
  along the plane's axes (what the erosion follows).
- **The erosion is NOT baked.** Its gullies crease at the scale of a pixel, far below a level's texel (lod / 3). A first
  cut that baked it (height + gradient) drew the gullies soft and their low-Sun shading flat (the Alps at an 8-degree
  Sun: mean |diff| 0.73 levels against a floor of 0.13, the `shadow` debug view 5.4 levels off an exact evaluation at the
  hit where the procedural march is 0.5 off). It is evaluated as before: once per march, at the sample where the ray
  comes within its bound of the value surface (tdClipErosion: tdErosion fed the baked coarse gradient), then taken as
  a plane through that sample for the refinement and the shading normal (gTdHitEro). The normal's value octaves stay
  procedural (terrainDetailLinEro: they run at the shading LOD, finer than the geometry's).
- **Levels 0-3 hold the same sum** (the finest octave's cell is 16 m): they exist for their texels. With level 3 alone
  (2.7-m texels, bilinear) the ground within ~80 m of a 2-m eye sat a few centimetres off and its features moved
  pixels at that grazing angle (the Alps from the ground 1.4 levels, with levels 0-2 0.5-0.7 against a floor of 0.35-0.5).
- **The plane.** Orthographic onto the tangent plane at an anchor point (re-anchored, and everything rebaked, 300 km
  away: 0.1% of foreshortening). Texel = lod_j / 3, so a level reaches 170 lod_j from the observer, and the LOD is at
  least 1.2% of the distance: the level a point needs always holds it (a guard takes a coarser level outside the
  window). Toroidal: a texel lives at its absolute plane cell mod 1024; the CPU (double) gives each level the
  observer's cell and the fraction inside it, so the shaders form no coordinate larger than a window.
- **The bake** (recordTerrainClip, before the scene-depth passes): the strips of each level's window that came in since
  last frame, one dispatch a rectangle. Standing still: nothing. Walking: ~2k texels a frame, 280 m/s at 2.6 km: ~9k,
  5.7 km/s at 4.5 km: ~50k. A level the observer crosses half of in one frame is dropped and the readers start one
  coarser (`tcAxU.w`). The finest level kept comes from the nearest ground on the CPU's DEM (under the eye and on five
  rings round it, + the detail's relief, x 0.7): from 5 km up the levels under 32 m are not baked. Rebaked whole when the
  terrain sliders or the launch pads' graded ground change (a hash) and on a jump (~11.5 M texels, one frame).
- **Where it runs.** The readers' set (UBO + A + B) is set 1 of the scene-depth and sky pipeline layouts; the variants
  are scene_depth_clip.comp.spv, sat_sky_clip.frag.spv and sat_sky_taa_clip.frag.spv (-DTD_CLIPMAP). Two samplers past
  sat_sky.frag's sixteen: on a device at the 16-sampler floor (build.md) there is no clipmap and the procedural
  shaders run (`tcSupported`). The observer's own ground (the frame pass, one thread) stays procedural. SKY_LITE /
  SKY_ENV have no detail and no clipmap. `state` -> `observer.terrain_clip` (active, finest_level, baked_texels).
- **Measured** (RTX 3070 Ti, 1920x1009, the user's settings, GPU frame p50, clipmap off -> on, interleaved in one app):
  the eight golden views: Alps ground 28.8 -> 21.4 ms, Alps 3.5 km low Sun 22.5 -> 19.8, Everest 24.8 -> 18.9, Grand
  Canyon 33.9 -> 27.3, Rockies 10.5 km 17.1 -> 17.3, Andes 30 km 18.0 -> 17.1, Himalaya 400 km equal, Big Sur 34.5 ->
  28.1. Matrix cells (x3): clear_2m 21.3 -> 16.3 (sky pass 10.3 -> 7.7, scene depth 4.25 -> 1.8), ovc_2m 27.1 -> 21.4,
  term_2m 27.0 -> 21.8, beams_site 29.2 -> 23.5, stA_2m / dense_6km / ovc_6km / ovc_850km within noise. Moving (GPU
  mean over a 3-s path): walking 29.0 -> 21.0, 280 m/s 19.4 -> 16.4, 5.7 km/s 18.8 -> 16.9, a 1.5 -> 14 km climb 21.9 ->
  19.4. Images, mean |diff| against the procedural shaders (a recapture of the same shaders in brackets): Alps ground
  0.66 (0.35), Alps low Sun 0.18 (0.17), Everest 0.92 (1.00), Grand Canyon 1.36 (1.55), the four from altitude and
  every matrix cell at the floor. Against a march with a 20x tighter hit test and no erosion plane (the Alps ground):
  procedural 1.75, clipmap 1.73 levels: what is left between the two is inside the march's own tolerance.
- **Costs.** 113 MB of VRAM. From altitude the sky pass is ~0.4-0.7 ms dearer where no fine level is read (two
  gathers a height against one or two hashed octaves), paid back in the depth passes (Rockies 10.5 km: +0.2 ms net).
- **Before it** (same day, e8220f3): scene_depth.comp ran the whole detail + erosion at the observer in every texel
  for the eye's ground height; a one-workgroup frame pass (quarterPass 2) writes it to terrainFrameBuf once and both
  depth passes read it (scene depth 4.49 -> 4.23 ms at clear_2m).

### Elevation texture encoding — READ THIS BEFORE TOUCHING TERRAIN CODE

**File:** `assets/textures/earth_elevation.png` (R8_UNORM, 14999×7500 — ~2.67 km/texel; older notes said 21600×10800 — land-only DEM)

**The app does not decode this PNG (2026-09-29).** It reads `earth_elevation_0.r8` / `_1.r8` (raw rows,
split under GitHub's 100 MB limit) and `8k_earth_specular_map.r8` through `readRawR8`; the city detail
maps are JPGs. stb_image decoding these PNGs froze the development machine within a few decodes with no
Vulkan involved (docs/FREEZES.md; `UploadStress --decode-only` reproduces it). The PNGs stay the source:
after editing one, run `python tools/make_raw_textures.py` (byte-exact, verified). The PNG path remains
only as a logged fallback when a `.r8` is missing.

**This is NOT ETOPO1 and has NO bathymetry / below-sea-level data.** Do not assume
pixel=0 means sea level — it does not. The actual encoding is:

| Pixel value | Meaning |
|-------------|---------|
| 0–14/255    | Compression noise in ocean regions — treat as sea level |
| **15/255**  | **Ocean / sea level baseline** |
| 16–255/255  | Land elevation, linearly scaled above sea level |
| 255/255     | ≈ 8848 m (Everest) |

**Correct formula used in `sat_sky.frag` and `SatelliteSim.cpp`:**
```
kElevOffset = 15.0/255.0 * kElevRange          // ≈ 529 m baseline
terrainH    = max(0, pixel * kElevRange − kElevOffset)
```
Failing to subtract `kElevOffset` makes every coastline on Earth appear as a ~530 m vertical
cliff because sea-level land reads as 529 m above the ocean sphere. This bug has been
introduced and re-introduced across multiple sessions. The ocean sphere sits at exactly
`R_EARTH`; the terrain height formula must produce 0 m for ocean-baseline pixels.

### The sea is not terrain — the march's `h0 <= 0` gate (2026-09-25) — `sat_sky.frag`

The terrain march can "hit" the sea-level sphere. Over water `tdDemAt` reports `h0 = 0` and nothing the
march adds on top is non-zero (the detail octaves scale with `tdAmp0`'s `smoothstep(0, 80, h0)`), so the
surface such a hit lies on *is* the sphere `tExit` bounds the march by — landing on it is step-size luck:
the ray has to land inside the sub-metre band the march accepts (`minStep` alone is 0.5 m, then the
regula-falsi branch) and beat `tExit` by a fraction of a millimetre. Which rays win flips with distance,
in **arcs concentric about the nadir**: the ocean "rings" (`harness_runs\ocean_rings_sea`, `debugview
normals` over open water — 15.26 % of the frame repainted at 60 m AGL, 0 % at 1200 m). Those pixels drew
flat, detail-free sea-level *land* shading where the ocean (wave) branch belongs.

**Fix:** after the march, `if (terrainH0 <= 0.0) { tHit = -1.0; }`, with every normal/shading consumer
gated on `tHit > 0`. `h0` alone is the test because it is the march's own height function, so the two
cannot disagree, and land cannot read 0 here: the DEM's land baseline (16/255) is 35 m above the sea
baseline (15/255, the table above), and the water mask only forces a sea height below `kWaterMaskMaxM`
(`tdDemAt`, `terrain.glsl`).

**Verified at pixel level** — probes are *not* evidence here (`probe` runs in its own compute shader and
never consumes the fragment-side `tHit` gate, so probe output is byte-identical pre/post): `debugview
normals` repaints **0.00 %** of open water at 60 m AGL afterwards, the beauty diff's `changed_frac`
(0.1526) is exactly the ring pixels, and the 1200 m captures are bit-identical. Land views: v1-v4
bit-identical to the pre-fix baselines, v6/v7 ≈0 %, v8 1.68 % confined to the seaward wedge below the
horizon (that wedge *is* ocean), and v5's 32.6 % was a moved *date* from splitting the script rather than
the fix — see `docs/HARNESS.md`, *Gotchas* → `time sun`.

### The sea (2026-09-30) — `sat_sky.frag` seaHeight / oceanSeaState / shoreSignedDist

- **The wave field is EXACTLY periodic** over (kSeaPeriodX, kSeaPeriodY) = (28571.4, 21428.6) m: the noise
  lattice hashed mod `kSeaCells` (1200), ridges every 6 cells, an INTEGER octave matrix [2 1; -1 2] and integer
  per-octave scales (`kSeaOctA`), arguments reduced mod kSeaCells per octave. The CPU wraps the observer's world
  offset (cityOffsetEast/NorthM, double) into one period (`cloud.oceanState.xy`): seamless, and small at any
  distance flown. Review 9 had fed the waves the unbounded float offset: after a long flight the octave
  arguments (~12x the offset) lost their fraction and the texture broke up (the user's report).
- **Sea state** (`oceanSeaState`, "Sea state from weather" `clouds.ocean_sea_state` 1, slot 213): the wave
  amplitude as a multiple of the old fixed sea from the weather cube (~40 km: storm = cover x convective type
  + rain), the westerly belts and a slow regional wind; choppiness follows. **Whitecaps** ("Whitecaps"
  `clouds.ocean_whitecaps` 1, slot 214): crest foam near (the crest values run ~0.35-0.8, top 5% ~0.7), the mean
  foam fraction (~Monahan) as albedo far, lit as a white diffuser (sun + 0.9 x the sky reflection).
- **Shore** (`shoreSignedDist`, the height function's own waterline): waves shoal to 0.4 over 150 m, breaker
  lines roll in, and the waterline is anti-aliased half from each side (the sea toward wet sand, the beach
  toward a dark water albedo) over a pixel footprint.
- **The wave trace steps to the FIRST crossing** (5 coarse steps, then 6 secant) — the secant alone converged on
  back faces behind crests. **Below-horizon reflections** (a steep facet mirrors the next wave, not the sky) take
  0.2 x the horizon's light (`reflWaterK`). **The sky-reflection march uses u^2 spacing**: 6 uniform samples over
  a ~1000-km grazing path made the extinction far too high and the reflected horizon tan (streaks on every
  steep face — the "tan patches" were pre-existing). The on-screen clouds are composited into the reflection,
  leaning back to the clear march by the cloud's distance (the target lacks the air in front of it).
- **The satellite ocean-glint loop is skipped when its result is zero** (day, or above ~8 km): 512 entries x two
  texture fetches per ocean pixel cost 6 ms of the sky pass from orbit (the `water_map` knockout "saved" it only
  because it turns the sea into cheaper terrain).
- Debug views 40-45 (harness `debugview oceanrefl|oceanfresnel|oceanstate|oceansurf|oceannormal|oceanshore`).
- **Wave normals are filtered to the pixel footprint ALONG the view** (review 14, `gSeaFootM`, seaHeight's `lodW`:
  an octave fades as its cell nears 2 footprints, octave 0 included). A rough sea under a low Sun aliased into
  glittering pixel noise that crawled with any camera motion (the 80th-power diffuse term and the below-horizon
  reflection test amplify any normal noise); the far band's frame-to-frame change at 860 m/s fell from ~11-26 to
  ~0-5 levels. The removed slope roughens the reflection instead (`seaRough`: the reflected ray tilts up, half of
  it the next wave's water, the reflected clouds blur) or the filtered far sea was a mirror. The reflected clouds'
  weight is taken per texel by opacity (the filtered alpha mixed in the no-cloud marker: hard blocks).
- **Water map from Natural Earth 10 m** (`tools/make_water_map.py`, 2026-09-30): land + lakes rasterised at 16K
  between 60 S and 72 N, the distance field averaged down to the 8K texels (sub-texel zero crossing; ~1 km vs
  the 5-km mask's ~2.5 km). Outside the band the 8K mask (ice shelves). Reads the .r8 copies, never the PNGs.
- **Thin sea channels survive the 16K->8K mean** (2026-11-03, issue_20261030_204632): a channel 1-2 px wide at 16K
  (Puyuhuapi, Sognefjord, the Zeeland estuaries) averaged to land, so the fjord was drawn as flat 0-m land in
  the day map's sea blue, and the field's shallow negative ridge along it (~-0.3 texel) read as a shore ~1 km
  away: the shore cap shaved the wall beside it (probe at the report's slab: DEM 835 m drawn at 280 m, texel-
  stepped). The bake now finds the water a 3x3 opening removes at 16K and, where the DEM footprint reaches the
  sea byte and the nearest body is the sea, raises the 8K field by a Gaussian bump (K_CHANNEL 0.12, sigma 0.8,
  gain 2; a hard-set texel value creased the shore ramp along texel edges). 25,740 texels raised, 21,420 flip
  land->water, none the other way; lakes (level > sea) untouched. Report view: ground samples on mapped water
  2.2% -> 7.7%, fjord drawn as 0-m land 18.5% -> 13.2% (the rest is DEM sea that Natural Earth calls land).
- **Sea-level land the day map paints as sea is dunked by the bake** (2026-11-03, same issue; the user: "dunk the
  blue terrain"): texels the coastline calls land where the DEM byte is the sea's, the nearest body is the sea
  (within 3 texels) and the 8K day map is sea blue ((b - (r+g)/2) / luminance > 0.22) get the same Gaussian bump
  (K_DUNK 0.25): 39,035 texels, 32,866 of them water after it. Green polders and deltas at DEM 0 are not blue and
  stay land (Zandvoort: 0 texels changed). The bake works on whole 8K texels; the rest is in `sat_sky.frag`:
  - `seaLevelDayDunk`: ground at the sea's level (h0 <= 0.5 m) that the day map paints sea blue (`daySeaBlue`) is
    drawn as the sea, but only amid relief (the DEM's mip 3, ~20 km mean, >= 150 m): the sub-texel fringe of a fjord.
    Without the relief test it flooded Zandvoort's dunes in day-map-texel stairs (its shore texels are blue too).
  - Land 1-15 m and more above the level whose day-map texel is sea blue takes the least-blue of eight texels 2 and
    4 texels away (the forest or rock beside the fjord), again only amid relief (mip 3 100-200 m).
  Report shot (masks from a variant that paints h0 <= 0.5 m red and higher land green; sea blue = (b - (r+g)/2) /
  luminance > 0.35 in the frame): sea-level ground drawn as land 18.1% of the terrain (a669a8c) / 16.1% (bake only)
  -> 0.0%; sea-blue land above the sea 82% of it -> 0.2%. Zandvoort with these shaders vs without, same app: 0.004%
  of pixels (floor 0.001%). Measuring it: `shaders reload depth=` rebuilds the sky pipelines from the SHIPPED SPVs,
  so reload depth first and sky / sky_taa after it, or a sky variant silently is not drawn (two rounds of this
  fix's A/Bs were void for that).
- **The shore cap keeps steep shores** (2026-11-03, same issue, terrain.glsl `shoreDemAbove`): the 0.25 m/m ramp
  exists for the coastal artifact (the DEM's value where the map draws the shore), but as a plain cap it flattened
  every steep shore the DEM gets right (a restored fjord's wall: 937 m drawn at 338 m, 1.35 km from the water). Now
  the land is never pushed lower than DEM - hS x (1 - smoothstep(0, 800 m, x)), hS = the DEM (mip 0) at this
  point's shore above the level, found down the distance field's forward-difference gradient; joined to the ramp
  by a smooth max (k 20 m), and past 800 m the DEM stands. Flat coasts never reach it (offline over 0.5-0.7 deg
  boxes: Zandvoort and Zeeland 0.00% of samples changed; Sognefjord 4.8% >5 m). Report shot, 416-probe grid:
  samples > 100 m below the DEM 43.5% (before any fix) / 57.5% (channel restored, old cap) -> 1.7%; every sample
  with DEM >= 700 m within 19 m. Cost: the fetches are free (a variant with hS = 0 cost the same; Alps / NL
  0.0 +- 0.1 ms); drawing the restored relief costs +2.8 ms at the shot on a contended GPU (+6%, A-A spread 2 ms).
  A mip-1 hS (first cut) averaged a fjord's walls into its floor and changed almost nothing. hS fades with the
  field's gradient size (smoothstep 0.25-0.6 m/m): on the ridge between two shores the gradient flips and the
  found shore would jump sides. With the walls standing, the DEM's own 2.7-km bilinear facets show on them
  (straight-edged planes on Sognefjord's walls): that is the DEM's resolution, not the cap.
  Final shader set vs the a669a8c shaders, same app, report shot, new map: GPU 17.6 vs 15.4 ms (+2.2 ms, A-A
  spread 0.6); Zandvoort / Alps 0.0 +- 0.1 ms.
- **Review 16:** waves at `kSeaSpeed` 0.75 (the ~107-m ridges ran at ~27 m/s; a real 107-m wave at ~13), and a base
  ROUGHNESS from the sea state that grows toward grazing views (the unmodelled wind ripples, Cox & Munk), whatever the
  footprint: within ~8 km a calm sea mirrored clouds and the Milky Way crisply up to the horizon. It drives the same
  terms as the footprint's lost slope (reflected ray tilted up, darker, clouds blurred) and the Milky Way reflection's
  mip (3 + 4 x roughness).
- `scripts/ocean_v2.satcmd` (long travel + beach + sea state + aurora ground).
- **Review 22:** **the Sun glint is a Beckmann microfacet lobe** in this pass's units (pi L / E): slope variance
  = Cox & Munk (0.003 + 0.00512 W, W = 7 m/s x the sea state) + 0.06 x the footprint filter's lost slope
  (`seaRoughFoot`) + the Sun's disc; Schlick F (F0 0.02), Smith (Walter's rational fit); x the Sun's REAL
  transmittance (`sunTransMax`: sunSpecTint is hue-normalised). The Phong lobe it replaced peaked at ~0.6 of a white
  surface and narrowed as the waves were filtered flat — no glitter path (snap 9). **Foam, surf and wet sand are
  diffusers**: the Sun on the facet + the terrain's sky irradiance (`skyAmbientTerrain` x 0.4 x "Terrain sky light")
  + the Moon; they took the MIRRORED sky (reflColor) — whitewater showed a reflected image ("mercury").
  **Under an overcast** the reflection's clear-march share (the air in front of reflected clouds, and the sky off
  screen) is x `ovcK` = mix(0.35, 1, the sea point's cloud shadow) where the reflection has cloud or is off screen;
  the on/off-screen fade spans 30% past the edge (8% drew dark wedges in the sea's lower corners under a sunset-lit
  deck). Snap 5's sea had been brighter than the overcast over it (debug view 50: full shadow, 0.48 clear march).
- **The sea's "white static" (2026-10-06):** `ovcK` read the RAW centre tap of the cloud shadow (`cloudBCenter.a`),
  which carries cloud_march.comp's 2x2 ordered march offsets — only the [1 2 1] tent (`cloudShadowT`) averages them
  out. Under broken cloud the sea's reflection took a fixed grid of period 2 half-res texels: white dots over the
  bright sea that shimmered as the waves moved under them. It reads `cloudShadowT` now (so does debug view 50).
  Measured at Big Sur, 20 m, Sun 4 deg: the period-4-px energy was 4.4x its neighbours' (5.0x with the TAA), now
  1.0x. **Anything that reads the cloud shadow must read `cloudShadowT`, never `cloudB.a` / `cloudBCenter.a`.**

### Satellite ocean-glint gain / floor (2026-09-26) — `sat_sky.frag`, `OceanGlintBuf`

A satellite's mirror flare reaches the water twice: the point sprite/bloom/glare the observer sees
directly, and the specular hit `sat_flare.comp` appends to `OceanGlintBuf` that `sat_sky.frag`
composites onto sea-level hits. Only the first had controls, so "a mild satellite lights up the water"
could only be answered by dimming the satellite itself (all of it shares
`brightnessScale`/`flareGlowGain`). `GpuCloudParams` grew 688 → 704 for two floats, and
`oceanGlintPad0/1` are the load-bearing std140 rounding (C++ packs the pair at 696). The pair first
shipped defaulted to the constants that were hardcoded at the use site (gain 1.0, floor 2.0) so an
unchanged `settings.json` rendered exactly as before; later the same day the defaults were moved to the
tuned values below.

| Field | Ocean tab slider | settings key | Default | Effect |
|-------|------------------|--------------|---------|--------|
| `oceanGlintGain` | "Ocean flare refl" (idx 110) | `ocean_glint_gain` | 0.020175438 | multiplier on the whole glint contribution (0 = none) |
| `oceanGlintMinFlux` | "Flare refl floor" (idx 111) | `ocean_glint_min_flux` | 37.894737 | drop an entry whose `effectFlare` (`.w`) is below this |

The floor is the surgical one: the entry is skipped before any per-pixel work, so faint satellites stop
appearing on the water while a spectacular mirror flare is untouched; the gain scales both alike. They
were written as `if (flux < 2.0) continue;` and a bare contribution term in the sea-level composite
until this change — `sat_flare.comp`'s own write threshold (`OCEAN_GLINT_THRESH`, 1.0) means the list
never carries a lower `effectFlare`, so a floor of 0 is simply "draw everything the list holds".

**Defaults are the author's tuned values** (second half of 2026-09-26): both literals in
`SatelliteSim.h` are float32-exact copies of the pair in `build-win-release/Release/settings.json`
(`0.02017543837428093` / `37.894737243652344`, i.e. the UI's `0.02` / `37.9`), so a first run or the
`SatelliteSimFresh` build reproduces the tuned look rather than the old hardcoded one. The trailing
digits are slider-drag noise, not intent — read them as "~2 % of the original glint term" and "~60 % of
`fIntens`' range". Nothing changes for the tuned file itself (it carries both keys), and nothing in
`applyGraphicsPreset`'s table owns either field, so no preset re-decides them.
`tools/settings_defaults_diff.py <settings.json>` lists every key in a settings file that differs from
the compiled-in defaults — `build-win-release/Release/settings.json` currently differs on ~38 others,
all older tuning or runtime state (camera, window geometry, active tab).

**Latent OOB found on the way:** `hovPhotoMinus/hovPhotoPlus/draggingPhoto` were `[33]` while the
Photometry table's idx range already reached 34 ("Glare near gain" 33 / "Glare near range (km)" 34 —
added after the `[33]` resize and never followed up). An idx-33/34 write therefore landed in the *next*
array's `[0]`: the near-glare rows' hover flags fought the first rows' (`hovPhotoMinus[33]` →
`hovPhotoPlus[0]`) and `draggingPhoto[33]` → `hovCloudMinus[0]`, so their drag lock could grab the wrong
slider. All three are `[35]` now — same class of silent-array bug as the `[33]`→`[46]`→`[112]` cloud
history, and the reason this pair of sliders was slotted at free idx 110/111 rather than beside
"Ocean MW refl" (renumbering a table means renumbering all four shared arrays in lockstep).
`tools/check_cloud_params.py`: 118 fields, identical order.
